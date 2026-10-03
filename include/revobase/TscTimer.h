// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include "PortableIntrinsics.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach_time.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#include <x86intrin.h> // __rdtscp
#endif
#endif

/*
*For low-latency cross-core measurements on AMD:
Pin critical threads to cores within the same CCX (typically cores 0-7 or 8-15)
Or use single-core designs where the measuring thread is pinned
The timer will automatically detect if cross-core is problematic: if you see
ridiculous latencies, it is probably wrong

Apple Silicon has properly synchronized counters
 *
 */
namespace revobase {
class TscTimer {
public:
  // ---- Core API for cross-core low-latency measurement ----

  // Call once at startup. Throws if system unsuitable for cross-core timing.
  static void calibrate() {
    if (!is_suitable_for_cross_core_timing()) {
      throw std::runtime_error(
          "TSC not suitable for cross-core timing on this CPU. "
          "Missing invariant_tsc, constant_tsc, or nonstop_tsc support.");
    }

#if defined(__x86_64__) || defined(_M_X64)
    // Initial rough calibration using steady_clock
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = rdtscp();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t c1 = rdtscp();
    const auto t1 = std::chrono::steady_clock::now();

    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double ticks_per_ns = (c1 - c0) / ns;

    // Sanity check: typical CPUs are 1-5 GHz, so expect 1-5 ticks per ns
    if (ticks_per_ns < 0.5 || ticks_per_ns > 10.0) {
      throw std::runtime_error(
          "TSC calibration failed: unreasonable ticks_per_ns=" +
          std::to_string(ticks_per_ns));
    }

    ticks_per_ns_.store(ticks_per_ns, std::memory_order_relaxed);

    // Detect cross-core TSC offset issues
    detect_tsc_offset_issue();
#endif
  }

  // Capture timestamp at START of measurement interval (with serialization)
  static inline uint64_t now_cycles_begin() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned aux;
    PORTABLE_LFENCE();
    return __rdtscp(&aux);
#elif defined(__APPLE__) && defined(__aarch64__)
    __asm__ __volatile__("dsb sy" ::: "memory"); // Data synchronization barrier
    return mach_absolute_time();
#else
    return fallback_now_ns();
#endif
  }

  // Capture timestamp at END of measurement interval (with serialization)
  static inline uint64_t now_cycles_end() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned aux;
    uint64_t tsc = __rdtscp(&aux); // rdtscp serializes after
    PORTABLE_LFENCE();
    return tsc;
#elif defined(__APPLE__) && defined(__aarch64__)
    uint64_t t = mach_absolute_time();
    __asm__ __volatile__("dsb sy" ::: "memory");
    return t;
#else
    return fallback_now_ns();
#endif
  }

  // Convert cycle difference to nanoseconds
  static inline double delta_ns(uint64_t start, uint64_t end) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    const double tpn = ticks_per_ns_.load(std::memory_order_relaxed);
    return (end - start) / (tpn > 0.0 ? tpn : 1.0);
#elif defined(__APPLE__) && defined(__aarch64__)
    return static_cast<double>(end - start) * mach_timebase();
#else
    return static_cast<double>(end - start);
#endif
  }

  // Convert cycle difference to microseconds
  static inline double delta_us(uint64_t start, uint64_t end) noexcept {
    return delta_ns(start, end) / 1000.0;
  }

  // Inverse of delta_ns: fractional TSC cycles spanning a nanosecond duration.
  // Lets callers build an intended schedule directly in the TSC domain so a
  // slot's reference time is known before it is actually reached. Returns a
  // double (unrounded) so callers can accumulate per-step without drift; feed
  // small per-interval values rather than one huge elapsed span to stay exact.
  static inline double ns_to_cycles(double ns) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    const double tpn = ticks_per_ns_.load(std::memory_order_relaxed);
    return ns * (tpn > 0.0 ? tpn : 1.0);
#elif defined(__APPLE__) && defined(__aarch64__)
    const double tb = mach_timebase();
    return ns / (tb > 0.0 ? tb : 1.0);
#else
    return ns;
#endif
  }

  static bool is_suitable_for_cross_core_timing() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    if (!has_invariant_tsc()) {
      return false;
    }

#ifdef __linux__
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo.is_open()) {
      return false;
    }

    std::string line;
    // the counter's tick rate doesn't change with P-states/C-states/thermal throttling
    bool has_constant_tsc = false;
    // the counter doesn't stop when the cores goes idle
    bool has_nonstop_tsc = false;
    while (std::getline(cpuinfo, line)) {
      if (line.find("flags") != std::string::npos) {
        has_constant_tsc = line.find("constant_tsc") != std::string::npos;
        has_nonstop_tsc = line.find("nonstop_tsc") != std::string::npos;
        if (has_constant_tsc && has_nonstop_tsc) {
          break;
        }
      }
    }

    return has_constant_tsc && has_nonstop_tsc;
#else
    return true; // Non-Linux x86, rely on invariant TSC check
#endif

#elif defined(__APPLE__) && defined(__aarch64__)
    return true; // Apple Silicon always suitable
#else
    return true; // Fallback uses steady_clock, always safe
#endif
  }

  // True when the kernel does not trust the TSC as a system-wide clocksource,
  // which is what makes cross-core timestamp comparison meaningless. Call
  // after calibrate(). See detect_tsc_offset_issue() for why this defers to
  // the kernel instead of measuring here.
  static bool has_cross_core_offset_issue() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return has_tsc_offset_issue_.load(std::memory_order_relaxed);
#else
    return false;
#endif
  }

  // Throws if cross-core timestamp comparison is not safe on this host.
  // Call this before comparing any two timestamps that might come from
  // different physical cores. That is not just an explicit cross-thread
  // comparison - an unpinned thread can be preempted and migrate cores at
  // any point, so even a single thread's begin/end pair is exposed unless
  // that thread is pinned to one core for the entire begin..end window.
  // Only a pinned, single-core begin/end pair is exempt.
  static void require_cross_core_safe() {
    if (has_cross_core_offset_issue()) {
      throw std::runtime_error(
          "TSC cross-core comparison requested, but the kernel does not trust "
          "the TSC as a cross-core clocksource on this host (clocksource != "
          "'tsc'). Timestamps taken on different cores are not comparable "
          "here - restrict to same-core begin/end pairs, or check "
          "has_cross_core_offset_issue() and handle it explicitly.");
    }
  }

private:
  static bool has_invariant_tsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid_max(0x80000000, nullptr) ||
        __get_cpuid_max(0x80000000, nullptr) < 0x80000007)
      return false;
    __cpuid(0x80000007, eax, ebx, ecx, edx);
    return (edx & (1u << 8)) != 0;
#else
    return false;
#endif
  }

  // Ask the kernel whether the TSC is usable as a system-wide clock.
  //
  // This used to spawn one thread per core, sleep 1ms in each, read rdtscp,
  // and call a large spread an offset problem. That cannot work: the threads
  // were never synchronized, so the spread was dominated by thread-creation
  // stagger and wakeup jitter (hundreds of microseconds) while a real
  // cross-core offset is nanoseconds to microseconds - the signal sat two to
  // three orders of magnitude under the noise. The threshold it compared
  // against was wrong too (10^10 cycles is ~3.3s at 3GHz, so it never fired),
  // but no threshold would have made that measurement sound.
  //
  // Linux already answers this properly: it runs a synchronized TSC sync test
  // across all CPUs at boot and demotes the clocksource if the TSC fails it.
  // If current_clocksource is "tsc", the kernel has verified it is coherent
  // system-wide; anything else means it did not.
  static void detect_tsc_offset_issue() {
#if defined(__x86_64__) || defined(_M_X64)
#ifdef __linux__
    static constexpr const char *kClocksourcePath =
        "/sys/devices/system/clocksource/clocksource0/current_clocksource";
    std::ifstream f(kClocksourcePath);
    std::string clocksource;
    if (!f || !(f >> clocksource)) {
      // Fail open: report no problem, because none was observed. Treating
      // "could not ask" as "broken" would silently disable cross-core timing
      // wherever /sys is not mounted (containers, chroots) with no signal.
      // The hard prerequisites are enforced separately - calibrate() throws if
      // is_suitable_for_cross_core_timing() fails. But say so out loud, so a
      // clean run is not mistaken for a verified one.
      SPDLOG_WARN("Cannot read {} - assuming the TSC is coherent across cores, "
                  "but this was NOT verified",
                  kClocksourcePath);
      return;
    }
    if (clocksource != "tsc") {
      SPDLOG_WARN("Kernel clocksource is '{}', not 'tsc' - cross-core "
                  "timestamp comparison is not meaningful on this host",
                  clocksource);
      has_tsc_offset_issue_.store(true, std::memory_order_relaxed);
    }
#endif
#endif
  }

  // Fallback: high-resolution "now" in nanoseconds
  static inline uint64_t fallback_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

#if defined(__x86_64__) || defined(_M_X64)
  static inline uint64_t rdtscp() noexcept {
    unsigned aux;
    return __rdtscp(&aux);
  }
  inline static std::atomic<double> ticks_per_ns_{1.0};
  inline static std::atomic<bool> has_tsc_offset_issue_{false};
#endif

#if defined(__APPLE__) && defined(__aarch64__)
  static inline double mach_timebase() noexcept {
    static std::atomic<double> scale{0.0};
    double s = scale.load(std::memory_order_acquire);
    if (s == 0.0) {
      mach_timebase_info_data_t info{};
      (void)mach_timebase_info(&info);
      s = (info.numer / static_cast<double>(info.denom));
      scale.store(s, std::memory_order_release);
    }
    return s;
  }
#endif
};

} // namespace revobase