// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

#include <cstring>
#include <fstream>
#include <pthread.h>
#include <sstream>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif

namespace revobase::system {

class CpuAffinity {
public:
  // The verify step is not belt-and-braces: setting an affinity mask expresses
  // a constraint, and a caller that treats a successful syscall as "I am on
  // core N" can silently mis-attribute a whole measurement run - or leave a hot
  // thread off its isolated core - with nothing in the logs. Linux migrates a
  // running task synchronously when its current CPU leaves the mask, so
  // sched_getcpu() is already authoritative on return; no sleep is needed.
  static bool pinToCore(int core_id) {
#ifdef __linux__
    if (!validCore(core_id))
      return false;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    const int rc = applyMask(pthread_self(), cpuset);
    if (rc != 0) {
      SPDLOG_ERROR("Failed to pin thread to core {}: {}", core_id,
                   std::strerror(rc));
      return false;
    }

    const int landed = sched_getcpu();
    if (landed != core_id) {
      SPDLOG_ERROR("Pinned to core {} but running on core {}", core_id, landed);
      return false;
    }
    SPDLOG_DEBUG("Thread pinned to core {}", core_id);
    return true;
#elif defined(__APPLE__)
    // Mach has no equivalent of sched_getcpu(), and THREAD_AFFINITY_POLICY is
    // an affinity *hint* the scheduler may ignore outright (it is unsupported
    // on Apple Silicon). So this cannot be verified - only reported.
    thread_affinity_policy_data_t policy = {core_id};
    const kern_return_t kr = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_AFFINITY_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
      SPDLOG_ERROR("Failed to set affinity hint for core {}: kern_return {}",
                   core_id, static_cast<int>(kr));
      return false;
    }
    SPDLOG_DEBUG("Affinity hint set for core {} (unverified)", core_id);
    return true;
#else
    (void)core_id;
    SPDLOG_WARN("CPU pinning not supported on this platform");
    return false;
#endif
  }

  // Pin current thread to multiple cores (allows migration within set). No
  // landing check here - any core in the set is a correct outcome.
  static bool pinToCores(const std::vector<int> &core_ids) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int core_id : core_ids) {
      if (!validCore(core_id))
        return false;
      CPU_SET(core_id, &cpuset);
    }
    const int rc = applyMask(pthread_self(), cpuset);
    if (rc != 0) {
      SPDLOG_ERROR("Failed to pin thread to cores: {}", std::strerror(rc));
      return false;
    }
    SPDLOG_DEBUG("Thread pinned to {} cores", core_ids.size());
    return true;
#else
    (void)core_ids;
    SPDLOG_WARN("CPU pinning not supported on this platform");
    return false;
#endif
  }

  static std::vector<int> getCurrentAffinity() {
    std::vector<int> cores;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    pthread_t thread = pthread_self();
    if (pthread_getaffinity_np(thread, sizeof(cpu_set_t), &cpuset) == 0) {
      for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &cpuset)) {
          cores.push_back(i);
        }
      }
    }
#endif

    return cores;
  }

  // First line of a sysfs cpu-list file such as
  // /sys/devices/system/cpu/isolated; empty if the file is missing or empty.
  static std::string readCpuList(const char *path) {
    std::ifstream f(path);
    std::string line;
    if (f)
      std::getline(f, line);
    return line;
  }

  // Whether `cpu` is in a kernel cpu-list string ("0-3,8,10-11"). Tokens that
  // do not start with a digit (blank entries, a trailing newline) are skipped
  // rather than handed to std::stoi, which would throw on them.
  static bool cpuListContains(const std::string &list, int cpu) {
    std::istringstream ss(list);
    std::string token;
    while (std::getline(ss, token, ',')) {
      if (token.empty() || token.front() < '0' || token.front() > '9')
        continue;
      const auto dash = token.find('-');
      const int lo = std::stoi(token.substr(0, dash));
      const int hi =
          dash == std::string::npos ? lo : std::stoi(token.substr(dash + 1));
      if (cpu >= lo && cpu <= hi)
        return true;
    }
    return false;
  }

#ifdef __linux__
  // Housekeeping CPUs: every configured CPU that is in neither `isolated` nor
  // `nohz_full`. If that leaves nothing (every CPU isolated), falls back to all
  // CPUs. Returns false only when the CPU count is unknown.
  static bool housekeepingCpuSet(cpu_set_t &cpuset) {
    CPU_ZERO(&cpuset);
    const std::string isolated =
        readCpuList("/sys/devices/system/cpu/isolated");
    const std::string nohz_full =
        readCpuList("/sys/devices/system/cpu/nohz_full");
    long cpu_count = sysconf(_SC_NPROCESSORS_CONF);
    if (cpu_count <= 0 || cpu_count > CPU_SETSIZE)
      cpu_count = CPU_SETSIZE;
    for (int cpu = 0; cpu < cpu_count; ++cpu) {
      if (!cpuListContains(isolated, cpu) && !cpuListContains(nohz_full, cpu))
        CPU_SET(cpu, &cpuset);
    }
    if (CPU_COUNT(&cpuset) > 0)
      return true;

    for (int cpu = 0; cpu < cpu_count; ++cpu)
      CPU_SET(cpu, &cpuset);
    return cpu_count > 0;
  }

  // Pin `thread` (not necessarily the caller) to `core_id`, or - when core_id
  // is negative or that pin fails - to the housekeeping CPUs.
  // pinned_to_requested (optional out): set true ONLY when the requested core
  // was actually pinned - false on the housekeeping fallback or when no core
  // was requested. Callers gate RT/busy-poll on it: a fallback core is shared,
  // so it must NOT run an RT spinner. The bool return still reports whether
  // the thread was pinned *somewhere* (fallback included).
  static bool pinThreadOrHousekeeping(pthread_t thread, int core_id,
                                      const char *thread_name,
                                      bool *pinned_to_requested = nullptr) {
    if (pinned_to_requested)
      *pinned_to_requested = false;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (core_id >= 0 && validCore(core_id)) {
      CPU_SET(core_id, &cpuset);
      const int rc = applyMask(thread, cpuset);
      if (rc == 0) {
        SPDLOG_DEBUG("{} pinned to CPU {}", thread_name, core_id);
        if (pinned_to_requested)
          *pinned_to_requested = true;
        return true;
      }
      SPDLOG_WARN("{} failed to pin to CPU {}: {}", thread_name, core_id,
                  std::strerror(rc));
    }

    if (!housekeepingCpuSet(cpuset))
      return false;
    const int rc = applyMask(thread, cpuset);
    if (rc != 0) {
      SPDLOG_WARN("{} failed to pin to housekeeping CPUs: {}", thread_name,
                  std::strerror(rc));
      return false;
    }
    SPDLOG_DEBUG("{} pinned to housekeeping CPUs", thread_name);
    return true;
  }
#endif

private:
#ifdef __linux__
  // CPU_SET() past CPU_SETSIZE, or with a negative index, is undefined
  // behaviour rather than an error - it writes outside the mask. Callers pass
  // core ids straight from config and command lines, where -1 is a common
  // "no dedicated core" sentinel, so the guard belongs here and not in each
  // of them. In-range-but-nonexistent cores are left to the syscall's EINVAL.
  static bool validCore(int core_id) {
    if (core_id < 0 || core_id >= CPU_SETSIZE) {
      SPDLOG_ERROR("Core id {} is out of range [0, {})", core_id, CPU_SETSIZE);
      return false;
    }
    return true;
  }

  // pthread_setaffinity_np's error code (0 on success); callers log it.
  static int applyMask(pthread_t thread, const cpu_set_t &cpuset) {
    return pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
  }
#endif
};

// RAII wrapper for CPU pinning
class ScopedCpuPin {
public:
  explicit ScopedCpuPin(int core_id) : core_id_(core_id), pinned_(false) {
    // Save original affinity
    original_affinity_ = CpuAffinity::getCurrentAffinity();

    // Pin to new core
    pinned_ = CpuAffinity::pinToCore(core_id);
  }

  // Non-copyable: a copy's destructor would restore the original affinity
  // while the scope that asked to be pinned is still running, silently
  // un-pinning a hot loop. Non-movable too - there is no use for transferring
  // one of these out of the scope that created it.
  ScopedCpuPin(const ScopedCpuPin &) = delete;
  ScopedCpuPin &operator=(const ScopedCpuPin &) = delete;

  ~ScopedCpuPin() {
    // Restore on isPinned() == false too: pinToCore() reports failure when the
    // thread did not land on the requested core, but the mask was already
    // replaced by then, so skipping the restore here would leak a narrowed
    // affinity out of the scope that asked for it.
    if (!original_affinity_.empty()) {
      CpuAffinity::pinToCores(original_affinity_);
    }
  }

  bool isPinned() const { return pinned_; }
  int getCoreId() const { return core_id_; }

private:
  int core_id_;
  bool pinned_;
  std::vector<int> original_affinity_;
};

} // namespace revobase::system
