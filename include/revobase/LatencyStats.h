// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

// Samples are integer nanoseconds. int32 keeps the 4-byte sample footprint 
// that matters at multi-million samples; int64 would double a buffer sitting on 
// the core whose cache behaviour is the measurement's subject.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

#include <fmt/core.h>

namespace revobase::latency {

using Sample = std::int32_t;
inline constexpr Sample kSampleMax = 2147483647; // 2.147 s

// Count of samples that hit the ceiling. Nonzero means the run saw something
// that is not a latency; report it rather than letting it sit in `max`.
inline std::uint64_t &saturated_samples() {
  static std::uint64_t n = 0;
  return n;
}

// Converts a measured duration to a Sample, saturating instead of wrapping.
//
// This guard is not theoretical. TscTimer::delta_ns computes `end - start` in
// UNSIGNED 64-bit, so a TSC inversion (or an uncalibrated ticks_per_ns, where
// the divisor falls back to 1.0) does not yield a negative — it yields ~6e18,
// or ~1.8e19 in the uncalibrated case. Two things then go wrong without this:
//   - int32 conversion is modular in C++20, so 6e18 becomes some arbitrary
//     small value that looks like a PLAUSIBLE latency. A float sample would at
//     least have shown up as an absurd `max`; silent wrap is strictly worse.
//   - std::lround of 1.8e19 exceeds LONG_MAX, which is not well-defined.
// Clamping on the double, before lround, closes both. Rounding (not truncating)
// keeps the conversion unbiased. Cost is two compares, outside the timed window.
inline Sample sample_ns_saturating(double ns) {
  if (!(ns > 0.0)) // also catches NaN
    return 0;
  if (ns >= static_cast<double>(kSampleMax)) {
    ++saturated_samples();
    return kSampleMax;
  }
  return static_cast<Sample>(std::lround(ns));
}

// Superset of every field any consumer prints. p95/p99999 are only meaningful
// to some; the rest fill them anyway (one index lookup each, off the hot path).
struct Summary {
  std::uint64_t n = 0;
  double min = 0, max = 0, mean = 0, stddev = 0;
  double p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0, p9999 = 0, p99999 = 0;
};

namespace detail {
// Standard nearest-rank: floor(q * (n-1)), clamped.
template <class T>
double quantile(const std::vector<T> &sorted, double q) {
  const std::size_t i = std::min<std::size_t>(
      sorted.size() - 1, static_cast<std::size_t>(q * double(sorted.size() - 1)));
  return static_cast<double>(sorted[i]);
}
} // namespace detail

// SORTS `samples` IN PLACE — no defensive copy. copying to sort would 
// double a buffer that sits on the very core whose cache/TLB behaviour
// the benchmark is measuring. Callers holding const data must pass an
// explicit copy (the rvalue overload below).
//
// `with_stddev` costs a second full pass, so it is opt-in: consumers that do
// not print it must not pay an extra x MB sweep between series,
// which would perturb the next series' cold state.
template <class T>
Summary summarize(std::vector<T> &samples, bool with_stddev = false) {
  Summary s;
  s.n = samples.size();
  if (samples.empty())
    return s;
  std::sort(samples.begin(), samples.end());
  double sum = 0;
  for (const T v : samples)
    sum += static_cast<double>(v);
  s.min = static_cast<double>(samples.front());
  s.max = static_cast<double>(samples.back());
  s.mean = sum / static_cast<double>(samples.size());
  if (with_stddev) {
    double var = 0;
    for (const T v : samples) {
      const double d = static_cast<double>(v) - s.mean;
      var += d * d;
    }
    s.stddev = std::sqrt(var / static_cast<double>(samples.size()));
  }
  s.p50 = detail::quantile(samples, 0.50);
  s.p90 = detail::quantile(samples, 0.90);
  s.p95 = detail::quantile(samples, 0.95);
  s.p99 = detail::quantile(samples, 0.99);
  s.p999 = detail::quantile(samples, 0.999);
  s.p9999 = detail::quantile(samples, 0.9999);
  s.p99999 = detail::quantile(samples, 0.99999);
  return s;
}

template <class T>
Summary summarize(std::vector<T> &&samples, bool with_stddev = false) {
  return summarize(samples, with_stddev);
}

inline void print_row(const char *label, const Summary &s, int label_width) {
  fmt::print("  {:<{}} n={:<10} min={:6.1f} p50={:6.1f} p90={:6.1f} "
             "p99={:7.1f} p99.9={:8.1f} p99.99={:9.1f} max={:9.1f} "
             "mean={:6.1f}  (ns)\n",
             label, label_width, s.n, s.min, s.p50, s.p90, s.p99, s.p999,
             s.p9999, s.max, s.mean);
}

inline void print_block(std::ostream &os, const std::string &label,
                        const Summary &s) {
  os << "\n  " << label << "  (" << s.n << " samples)\n";
  os << std::fixed << std::setprecision(1);
  os << "    min    " << std::setw(9) << s.min << " ns\n";
  os << "    p50    " << std::setw(9) << s.p50 << " ns\n";
  os << "    p90    " << std::setw(9) << s.p90 << " ns\n";
  os << "    p99    " << std::setw(9) << s.p99 << " ns\n";
  os << "    p99.9  " << std::setw(9) << s.p999 << " ns\n";
  os << "    max    " << std::setw(9) << s.max << " ns\n";
  os << "    mean   " << std::setw(9) << s.mean << " ns\n";
}

// Boxed report incl. stddev and p95.
inline void print_report(std::ostream &os, const std::string &name,
                         const Summary &s) {
  os << "\n═══════════════════════════════════════════════════\n"
     << "  Latency Statistics: " << name << "\n"
     << "═══════════════════════════════════════════════════\n"
     << std::fixed << std::setprecision(2) << "  Samples:      " << s.n << "\n"
     << "  Std Dev:      " << s.stddev << " ns\n"
     << "  Min:          " << s.min << " ns\n"
     << "  Mean:         " << s.mean << " ns\n"
     << "  Median (p50): " << s.p50 << " ns\n"
     << "  p90:        " << s.p90 << " ns\n"
     << "  p95:        " << s.p95 << " ns\n"
     << "  p99:        " << s.p99 << " ns\n"
     << "  p99.9:      " << s.p999 << " ns\n"
     << "  p99.99:      " << s.p9999 << " ns\n"
     << "  Max:          " << s.max << " ns\n"
     << "═══════════════════════════════════════════════════\n\n";
}
} // namespace revobase::latency
