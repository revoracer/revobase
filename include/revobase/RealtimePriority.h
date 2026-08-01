// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

#include <alloca.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <thread>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace revobase::system {

enum class SchedulerPolicy {
  NORMAL = SCHED_OTHER,
  FIFO = SCHED_FIFO,
  RR = SCHED_RR,
#ifdef __linux__
  BATCH = SCHED_BATCH,
  IDLE = SCHED_IDLE
#endif
};

class RealtimePriority {
public:

  static bool
  setRealtimePriority(int priority,
                      SchedulerPolicy policy = SchedulerPolicy::FIFO) {
#ifdef __linux__
    if (priority < 1 || priority > 99) {
      SPDLOG_ERROR("Priority must be 1-99, got {}", priority);
      return false;
    }

    struct sched_param param;
    param.sched_priority = priority;

    pthread_t thread = pthread_self();
    int result =
        pthread_setschedparam(thread, static_cast<int>(policy), &param);

    if (result != 0) {
      SPDLOG_ERROR("Failed to set real-time priority: {} (are you root?)",
                   strerror(result));
      return false;
    }

    SPDLOG_INFO("Set thread to RT priority {} with policy {}", priority,
                policy == SchedulerPolicy::FIFO ? "FIFO" : "RR");
    return true;
#else
    SPDLOG_WARN("Real-time scheduling not supported on this platform");
    return false;
#endif
  }

  static bool setPolicy(SchedulerPolicy policy, int priority) {
#ifdef __linux__
    const bool realtime =
        policy == SchedulerPolicy::FIFO || policy == SchedulerPolicy::RR;

    if (realtime ? (priority < 1 || priority > 99) : (priority != 0)) {
      SPDLOG_ERROR("Invalid priority {} for policy {}", priority,
                   static_cast<int>(policy));
      return false;
    }

    struct sched_param param{};
    param.sched_priority = priority;
    int result = pthread_setschedparam(pthread_self(),
                                       static_cast<int>(policy), &param);
    if (result != 0) {
      SPDLOG_ERROR("Failed to set policy {} priority {}: {} (are you root?)",
                   static_cast<int>(policy), priority, strerror(result));
      return false;
    }
    return true;
#else
    (void)policy;
    (void)priority;
    return true;
#endif
  }

  static bool setNormalPriority() {
#ifdef __linux__
    struct sched_param param;
    param.sched_priority = 0;

    pthread_t thread = pthread_self();
    int result = pthread_setschedparam(thread, SCHED_OTHER, &param);

    if (result != 0) {
      SPDLOG_ERROR("Failed to set normal priority: {}", strerror(result));
      return false;
    }

    return true;
#else
    return true;
#endif
  }

  static std::pair<SchedulerPolicy, int> getCurrentPriority() {
#ifdef __linux__
    struct sched_param param;
    int policy;

    pthread_t thread = pthread_self();
    if (pthread_getschedparam(thread, &policy, &param) == 0) {
      return {static_cast<SchedulerPolicy>(policy), param.sched_priority};
    }
#endif
    return {SchedulerPolicy::NORMAL, 0};
  }

  static bool lockMemory() {
#ifdef __linux__
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
      SPDLOG_ERROR("Failed to lock memory: {} (are you root?)",
                   strerror(errno));
      return false;
    }
    SPDLOG_INFO("Memory locked (MCL_CURRENT | MCL_FUTURE)");
    return true;
#else
    SPDLOG_WARN("Memory locking not supported on this platform");
    return false;
#endif
  }

  static bool lockCurrentMemory() {
#ifdef __linux__
    if (mlockall(MCL_CURRENT) != 0) {
      SPDLOG_ERROR("Failed to lock current memory: {} (are you root?)",
                   strerror(errno));
      return false;
    }
    SPDLOG_INFO("Memory locked (MCL_CURRENT only)");
    return true;
#else
    SPDLOG_WARN("Memory locking not supported on this platform");
    return false;
#endif
  }

  static bool unlockMemory() {
#ifdef __linux__
    if (munlockall() != 0) {
      SPDLOG_ERROR("Failed to unlock memory: {}", strerror(errno));
      return false;
    }
    return true;
#else
    return true;
#endif
  }

  static void prefaultStack(size_t bytes = 1024 * 1024) {
#ifdef __linux__

    size_t depth = bytes;
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
      void *stack_addr = nullptr;
      size_t stack_size = 0;
      if (pthread_attr_getstack(&attr, &stack_addr, &stack_size) == 0) {

        char probe_here = 0;
        const auto low = reinterpret_cast<std::uintptr_t>(stack_addr);
        const auto here = reinterpret_cast<std::uintptr_t>(&probe_here);
        const size_t headroom = (here > low && here - low <= stack_size)
                                    ? static_cast<size_t>(here - low)
                                    : 0;

        const size_t limit = headroom / 2;
        if (limit < depth)
          depth = limit;
      }
      pthread_attr_destroy(&attr);
    }
    if (depth == 0)
      return;

    volatile char *probe = static_cast<volatile char *>(alloca(depth));

    const long page = ::sysconf(_SC_PAGESIZE);
    const size_t stride = page > 0 ? static_cast<size_t>(page) : 4096;
    for (size_t off = 0; off < depth; off += stride) {
      probe[off] = 0;
    }
    probe[depth - 1] = 0;
    SPDLOG_DEBUG("Prefaulted {} KB of stack", depth / 1024);
#else
    (void)bytes;
#endif
  }

  static bool boostKsoftirqd(int cpu, int priority) {
#ifdef __linux__
    if (cpu < 0)
      return true;
    const std::string target = "ksoftirqd/" + std::to_string(cpu);

    DIR *proc = opendir("/proc");
    if (!proc) {
      SPDLOG_ERROR("boostKsoftirqd: cannot open /proc: {}", strerror(errno));
      return false;
    }

    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(proc)) != nullptr) {
      if (!isdigit(static_cast<unsigned char>(ent->d_name[0])))
        continue;

      std::string comm_path = std::string("/proc/") + ent->d_name + "/comm";
      FILE *f = fopen(comm_path.c_str(), "r");
      if (!f)
        continue;

      char comm[64]{};
      bool matched = false;
      if (fgets(comm, static_cast<int>(sizeof(comm)), f)) {
        size_t len = strlen(comm);
        if (len > 0 && comm[len - 1] == '\n')
          comm[len - 1] = '\0';
        matched = (target == comm);
      }
      fclose(f);
      if (!matched)
        continue;

      pid_t pid = static_cast<pid_t>(std::stoi(ent->d_name));
      struct sched_param param{};
      param.sched_priority = priority;
      if (sched_setscheduler(pid, SCHED_FIFO, &param) == 0) {
        SPDLOG_INFO("boosted ksoftirqd/{} (pid={}) → SCHED_FIFO {}", cpu, pid,
                    priority);
        found = true;
      } else {
        SPDLOG_ERROR("failed to boost ksoftirqd/{} (pid={}): {}", cpu, pid,
                     strerror(errno));
      }
      break;
    }
    closedir(proc);

    if (!found)
      SPDLOG_WARN("ksoftirqd/{} not found in /proc", cpu);
    return found;
#else
    (void)cpu;
    (void)priority;
    return true;
#endif
  }

  static bool isRealtime() {
    auto [policy, priority] = getCurrentPriority();
    return policy == SchedulerPolicy::FIFO || policy == SchedulerPolicy::RR;
  }

  static std::pair<int, int> getPriorityRange(SchedulerPolicy policy) {
#ifdef __linux__
    int min = sched_get_priority_min(static_cast<int>(policy));
    int max = sched_get_priority_max(static_cast<int>(policy));
    return {min, max};
#else
    return {0, 0};
#endif
  }
};

class ScopedRealtimePriority {
public:
  explicit ScopedRealtimePriority(
      int priority, SchedulerPolicy policy = SchedulerPolicy::FIFO)
      : enabled_(false) {

    auto [orig_policy, orig_priority] = RealtimePriority::getCurrentPriority();
    original_policy_ = orig_policy;
    original_priority_ = orig_priority;

    enabled_ = RealtimePriority::setRealtimePriority(priority, policy);
  }

  ScopedRealtimePriority(const ScopedRealtimePriority &) = delete;
  ScopedRealtimePriority &operator=(const ScopedRealtimePriority &) = delete;

  ~ScopedRealtimePriority() {
    if (enabled_) {

      RealtimePriority::setPolicy(original_policy_, original_priority_);
    }
  }

  bool isEnabled() const { return enabled_; }

private:
  bool enabled_;
  SchedulerPolicy original_policy_;
  int original_priority_;
};

}
