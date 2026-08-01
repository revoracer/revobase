// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

#include <cstring>
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/syscall.h>
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

  static bool pinToCore(int core_id) {
#ifdef __linux__
    if (!validCore(core_id))
      return false;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    const int result =
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
      SPDLOG_ERROR("Failed to pin thread to core {}: {}", core_id,
                   strerror(result));
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

  static bool pinToCores(const std::vector<int> &core_ids) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int core_id : core_ids) {
      if (!validCore(core_id))
        return false;
      CPU_SET(core_id, &cpuset);
    }

    pthread_t thread = pthread_self();
    int result = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

    if (result != 0) {
      SPDLOG_ERROR("Failed to pin thread to cores: {}", strerror(result));
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

  static int getNumCores() { return std::thread::hardware_concurrency(); }

  static int getCurrentCore() {
#ifdef __linux__
    return sched_getcpu();
#else
    return -1;
#endif
  }

  static void checkIsolated(int core_id) {
    if (core_id < 0)
      return;
#ifdef __linux__
    std::ifstream f("/sys/devices/system/cpu/isolated");
    if (!f) {
      SPDLOG_WARN("bench_cpu={}: cannot read /sys/devices/system/cpu/isolated",
                  core_id);
      return;
    }
    std::string line;
    std::getline(f, line);
    if (line.empty()) {
      SPDLOG_WARN("bench_cpu={}: no CPUs are isolated (isolcpus not set)",
                  core_id);
      return;
    }
    bool found = false;
    std::istringstream ss(line);
    std::string token;
    while (std::getline(ss, token, ',')) {
      auto dash = token.find('-');
      if (dash == std::string::npos) {
        if (std::stoi(token) == core_id) {
          found = true;
          break;
        }
      } else {
        int lo = std::stoi(token.substr(0, dash));
        int hi = std::stoi(token.substr(dash + 1));
        if (core_id >= lo && core_id <= hi) {
          found = true;
          break;
        }
      }
    }
    if (!found)
      SPDLOG_WARN("bench_cpu={} is NOT in isolated CPU set ({}); "
                  "scheduler noise may inflate latency",
                  core_id, line);
    else
      SPDLOG_INFO("bench_cpu={} is isolated ({})", core_id, line);
#else
    SPDLOG_WARN("CPU isolation check not supported on this platform");
#endif
  }

private:
#ifdef __linux__

  static bool validCore(int core_id) {
    if (core_id < 0 || core_id >= CPU_SETSIZE) {
      SPDLOG_ERROR("Core id {} is out of range [0, {})", core_id, CPU_SETSIZE);
      return false;
    }
    return true;
  }
#endif
};

class ScopedCpuPin {
public:
  explicit ScopedCpuPin(int core_id) : core_id_(core_id), pinned_(false) {

    original_affinity_ = CpuAffinity::getCurrentAffinity();

    pinned_ = CpuAffinity::pinToCore(core_id);
  }

  ScopedCpuPin(const ScopedCpuPin &) = delete;
  ScopedCpuPin &operator=(const ScopedCpuPin &) = delete;

  ~ScopedCpuPin() {

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

}
