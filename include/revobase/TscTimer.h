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

#include <revobase/portable_intrinsics.h>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach_time.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#include <x86intrin.h>
#endif
#endif

namespace revobase {
class TscTimer {
public:

  static void calibrate() {
    if (!is_suitable_for_cross_core_timing()) {
      throw std::runtime_error(
          "TSC not suitable for cross-core timing on this CPU. "
          "Missing invariant_tsc, constant_tsc, or nonstop_tsc support.");
    }

#if defined(__x86_64__) || defined(_M_X64)

    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = rdtscp();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t c1 = rdtscp();
    const auto t1 = std::chrono::steady_clock::now();

    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double ticks_per_ns = (c1 - c0) / ns;

    if (ticks_per_ns < 0.5 || ticks_per_ns > 10.0) {
      throw std::runtime_error(
          "TSC calibration failed: unreasonable ticks_per_ns=" +
          std::to_string(ticks_per_ns));
    }

    ticks_per_ns_.store(ticks_per_ns, std::memory_order_relaxed);

    detect_tsc_offset_issue();
#endif
  }

  static inline uint64_t now_cycles_begin() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned aux;
    PORTABLE_LFENCE();
    return __rdtscp(&aux);
#elif defined(__APPLE__) && defined(__aarch64__)
    __asm__ __volatile__("dsb sy" ::: "memory");
    return mach_absolute_time();
#else
    return now_ns();
#endif
  }

  static inline uint64_t now_cycles_end() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned aux;
    uint64_t tsc = __rdtscp(&aux);
    PORTABLE_LFENCE();
    return tsc;
#elif defined(__APPLE__) && defined(__aarch64__)
    uint64_t t = mach_absolute_time();
    __asm__ __volatile__("dsb sy" ::: "memory");
    return t;
#else
    return now_ns();
#endif
  }

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

  static inline double delta_us(uint64_t start, uint64_t end) noexcept {
    return delta_ns(start, end) / 1000.0;
  }

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
    bool has_constant_tsc = false;
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
    return true;
#endif

#elif defined(__APPLE__) && defined(__aarch64__)
    return true;
#else
    return true;
#endif
  }

  static bool has_cross_core_offset_issue() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return has_tsc_offset_issue_.load(std::memory_order_relaxed);
#else
    return false;
#endif
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

  static void detect_tsc_offset_issue() {
#if defined(__x86_64__) || defined(_M_X64)
#ifdef __linux__
    static constexpr const char *kClocksourcePath =
        "/sys/devices/system/clocksource/clocksource0/current_clocksource";
    std::ifstream f(kClocksourcePath);
    std::string clocksource;
    if (!f || !(f >> clocksource)) {

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

  static inline uint64_t now_ns() noexcept {
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

}
