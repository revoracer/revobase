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
#include <string>
#include <sys/mman.h>

#include <spdlog/spdlog.h>

#ifdef __linux__
#include <unistd.h>
#endif

namespace revobase::system {

enum class SchedulerPolicy {
  NORMAL = SCHED_OTHER, // Default Linux scheduler
  FIFO = SCHED_FIFO,    // Real-time FIFO (First-In-First-Out)
  RR = SCHED_RR,        // Real-time Round-Robin
#ifdef __linux__
  BATCH = SCHED_BATCH, // For batch processing (non-interactive)
  IDLE = SCHED_IDLE    // Very low priority
#endif
};

class RealtimePriority {
public:
  // Set real-time priority for current thread
  // priority: 1-99 (higher = more important, 99 = highest)
  // Note: Requires CAP_SYS_NICE capability or root
  static bool
  setRealtimePriority(int priority,
                      SchedulerPolicy policy = SchedulerPolicy::FIFO) {
#ifdef __linux__
    if (priority < 1 || priority > 99) {
      SPDLOG_ERROR("Priority must be 1-99, got {}", priority);
      return false;
    }
    if (!applySchedParam(policy, priority))
      return false;

    SPDLOG_INFO("Set thread to RT priority {} with policy {}", priority,
                policy == SchedulerPolicy::FIFO ? "FIFO" : "RR");
    return true;
#else
    (void)priority;
    (void)policy;
    SPDLOG_WARN("Real-time scheduling not supported on this platform");
    return false;
#endif
  }

  // Apply an arbitrary (policy, priority) pair - typically one captured
  // earlier from getCurrentPriority().
  //
  // setRealtimePriority() cannot be used to restore: it enforces priority
  // 1-99, and every non-RT policy (OTHER, BATCH, IDLE) reports priority 0.
  // Feeding that back in fails the range check, so a restore routed through
  // it would leave the thread at the elevated policy permanently.
  static bool setPolicy(SchedulerPolicy policy, int priority) {
#ifdef __linux__
    const bool realtime =
        policy == SchedulerPolicy::FIFO || policy == SchedulerPolicy::RR;
    // RT policies want 1-99; the others accept only 0. Checked here so a bad
    // pair is rejected before the syscall rather than as an opaque EINVAL.
    if (realtime ? (priority < 1 || priority > 99) : (priority != 0)) {
      SPDLOG_ERROR("Invalid priority {} for policy {}", priority,
                   static_cast<int>(policy));
      return false;
    }
    return applySchedParam(policy, priority);
#else
    (void)policy;
    (void)priority;
    return true;
#endif
  }

  // Set to normal (non-real-time) priority
  static bool setNormalPriority() {
#ifdef __linux__
    return applySchedParam(SchedulerPolicy::NORMAL, 0);
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

  // Lock all current and future memory to prevent page faults.
  // MCL_FUTURE causes every future mmap to be bulk-faulted on creation —
  // do NOT use this before lazy stream page mmaps.
  static bool lockMemory() {
    return applyMlockall(MCL_CURRENT | MCL_FUTURE, "MCL_CURRENT | MCL_FUTURE");
  }

  // Lock only currently mapped pages. Use this for lazy-mmap consumers
  // (e.g. stream readers) where MCL_FUTURE would bulk-fault future mmaps
  // and defeat lazy page loading.
  static bool lockCurrentMemory() {
    return applyMlockall(MCL_CURRENT, "MCL_CURRENT only");
  }

  // Unlock memory
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

  // Fault in the calling thread's stack, so that a later deep call - exception
  // unwinding, a large log-format frame, a reconnect path the steady state
  // never takes - does not pay minor page faults at the worst moment.
  //
  // It has to touch the STACK. The point is to walk the stack pointer down
  // through the unmapped pages below it; an equivalent heap buffer faults an
  // unrelated mapping, and (being well over MMAP_THRESHOLD) hands it straight
  // back to the kernel on free, leaving the stack exactly as it was.
  //
  // Call this BEFORE lockCurrentMemory(): mlockall(MCL_CURRENT) is a snapshot
  // of what is mapped at the time of the call and does not cover pages faulted
  // afterwards.
  //
  // Only affects the calling thread. Threads that run their own hot loops have
  // their own stacks and need their own call.
  static void prefaultStack(size_t bytes = 1024 * 1024) {
#ifdef __linux__
    // Cap the descent against the stack that is actually LEFT below us, not
    // against the thread's total stack size. Those differ by however deep the
    // current frame already is, and alloca() past the guard page is fatal with
    // no way to detect it beforehand. Sizing off the total would mean a call
    // from an already-deep frame overflows while looking bounded.
    size_t depth = bytes;
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
      void *stack_addr = nullptr;
      size_t stack_size = 0;
      if (pthread_attr_getstack(&attr, &stack_addr, &stack_size) == 0) {
        // Stacks grow down, so stack_addr is the LOW end of the region and a
        // local sits near the current stack pointer.
        char probe_here = 0;
        const auto low = reinterpret_cast<std::uintptr_t>(stack_addr);
        const auto here = reinterpret_cast<std::uintptr_t>(&probe_here);
        const size_t headroom = (here > low && here - low <= stack_size)
                                    ? static_cast<size_t>(here - low)
                                    : 0;
        // Half of what remains: leaves room for the guard page and for the
        // frames this call itself still needs.
        const size_t limit = headroom / 2;
        if (limit < depth)
          depth = limit;
      }
      pthread_attr_destroy(&attr);
    }
    if (depth == 0)
      return;

    // alloca moves the stack pointer, so every touch below is within the
    // stack region and above the new SP(Stack Pointer).
    volatile char *probe = static_cast<volatile char *>(alloca(depth));
    // Stride by the real page size; a stride larger than the page would skip
    // pages and silently under-prefault. volatile keeps the writes from being
    // elided now that nothing reads the buffer back.
    const long page = ::sysconf(_SC_PAGESIZE);
    const size_t stride = page > 0 ? static_cast<size_t>(page) : 4096;
    for (size_t off = 0; off < depth; off += stride) {
      probe[off] = 0;
    }
    probe[depth - 1] = 0;
    SPDLOG_DEBUG("Prefaulted {} KB of stack", depth / 1024);
#else
    (void)bytes; // macOS handles stack differently, no need to prefault
#endif
  }

  // Boost ksoftirqd/<cpu> to SCHED_FIFO at <priority>.
  // Call this before setRealtimePriority so ksoftirqd can preempt the app
  // and deliver packets when bench_cpu == irq_cpu.
  // No-op (returns true) if cpu < 0.
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

  // Get min/max priority for a policy
  static std::pair<int, int> getPriorityRange(SchedulerPolicy policy) {
#ifdef __linux__
    int min = sched_get_priority_min(static_cast<int>(policy));
    int max = sched_get_priority_max(static_cast<int>(policy));
    return {min, max};
#else
    return {0, 0};
#endif
  }

private:
  // Applies an already-validated (policy, priority) pair to the calling
  // thread.
  static bool applySchedParam(SchedulerPolicy policy, int priority) {
#ifdef __linux__
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

  // Shared mlockall() + error handling for lockMemory()/lockCurrentMemory(),
  // which differ only in the flags they lock with and their log label.
  static bool applyMlockall(int flags, const char *label) {
#ifdef __linux__
    if (mlockall(flags) != 0) {
      SPDLOG_ERROR("Failed to lock memory: {} (are you root?)",
                   strerror(errno));
      return false;
    }
    SPDLOG_INFO("Memory locked ({})", label);
    return true;
#else
    (void)flags;
    (void)label;
    SPDLOG_WARN("Memory locking not supported on this platform");
    return false;
#endif
  }
};

// RAII wrapper for real-time priority
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

  // Non-copyable: a copy's destructor would drop the thread back to the
  // original policy while the scope that asked for SCHED_FIFO is still
  // running, which shows up only as an unexplained tail-latency regression.
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

} // namespace revobase::system
