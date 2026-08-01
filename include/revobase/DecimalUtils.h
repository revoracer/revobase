// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace revobase {

inline constexpr std::uint8_t kMaxI64DecimalScale = 18;

struct DecimalParts {
  std::int64_t mantissa{0};
  std::int32_t exponent{0};

  std::uint8_t significant_decimal_places{0};

  bool ok{false};
};

inline constexpr std::int64_t pow10_i64(int shift) noexcept {
  constexpr std::array<std::int64_t, 19> powers{
      1LL,
      10LL,
      100LL,
      1'000LL,
      10'000LL,
      100'000LL,
      1'000'000LL,
      10'000'000LL,
      100'000'000LL,
      1'000'000'000LL,
      10'000'000'000LL,
      100'000'000'000LL,
      1'000'000'000'000LL,
      10'000'000'000'000LL,
      100'000'000'000'000LL,
      1'000'000'000'000'000LL,
      10'000'000'000'000'000LL,
      100'000'000'000'000'000LL,
      1'000'000'000'000'000'000LL,
  };

  if (shift <= 0)
    return 1;
  if (shift >= static_cast<int>(powers.size()))
    return powers.back();
  return powers[static_cast<std::size_t>(shift)];
}

namespace detail {

inline constexpr std::uint64_t kMagLimit = std::uint64_t{1} << 63;
inline constexpr std::uint64_t kMagLimitDiv10 = kMagLimit / 10;
inline constexpr std::uint64_t kMagLimitMod10 = kMagLimit % 10;

inline constexpr std::int32_t kSciExpAccumulateLimit = 1'000'000;

template <bool Checked>
inline DecimalParts parse_decimal_parts_impl(std::string_view s) noexcept {
  DecimalParts d;

  std::uint64_t mag = 0;
  bool neg = false;
  bool after_dot = false;
  bool any_digit = false;
  std::int32_t frac_digits = 0;
  std::int32_t sci_exp = 0;
  std::size_t i = 0;

  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    neg = (s[i] == '-');
    ++i;
  }

  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c >= '0' && c <= '9') {
      const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
      any_digit = true;
      if constexpr (Checked) {
        if (mag > kMagLimitDiv10 ||
            (mag == kMagLimitDiv10 && digit > kMagLimitMod10)) {

          if (after_dot && c == '0')
            continue;
          return d;
        }
      }
      mag = mag * 10 + digit;
      if (after_dot)
        ++frac_digits;
    } else if (c == '.') {
      if (after_dot)
        return d;
      after_dot = true;
    } else if (c == 'e' || c == 'E') {
      ++i;
      bool exp_neg = false;
      if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        exp_neg = (s[i] == '-');
        ++i;
      }
      const std::size_t exp_start = i;
      for (; i < s.size(); ++i) {
        const char ec = s[i];
        if (ec < '0' || ec > '9')
          return d;
        if (sci_exp <= kSciExpAccumulateLimit)
          sci_exp = sci_exp * 10 + static_cast<std::int32_t>(ec - '0');
      }
      if (i == exp_start)
        return d;
      if (exp_neg)
        sci_exp = -sci_exp;
      break;
    } else {

      return d;
    }
  }

  if (!any_digit)
    return d;

  const std::uint64_t limit = neg ? kMagLimit : kMagLimit - 1;
  if (mag > limit)
    return d;

  d.mantissa = neg ? static_cast<std::int64_t>(~mag + 1)
                   : static_cast<std::int64_t>(mag);

  while (frac_digits > 0 && d.mantissa != 0 && d.mantissa % 10 == 0) {
    d.mantissa /= 10;
    --frac_digits;
  }

  if (d.mantissa == 0) {
    d.exponent = 0;
    d.significant_decimal_places = 0;
    d.ok = true;
    return d;
  }

  d.exponent = sci_exp - frac_digits;
  d.significant_decimal_places =
      d.exponent < 0 ? static_cast<std::uint8_t>(std::min<std::int32_t>(
                           kMaxI64DecimalScale, -d.exponent))
                     : 0;
  d.ok = true;
  return d;
}

}

inline DecimalParts parse_decimal_parts(std::string_view s) noexcept {

  if (s.size() <= 19) [[likely]]
    return detail::parse_decimal_parts_impl<false>(s);
  return detail::parse_decimal_parts_impl<true>(s);
}

[[nodiscard]] inline bool normalize_decimal(std::int64_t mantissa,
                                            std::int32_t exponent,
                                            std::uint8_t scale,
                                            std::int64_t &out) noexcept {
  if (scale > kMaxI64DecimalScale)
    return false;

  const std::int64_t shift = static_cast<std::int64_t>(scale) + exponent;
  if (shift == 0) {
    out = mantissa;
    return true;
  }

  if (shift > 0) {
    if (shift > kMaxI64DecimalScale)
      return false;

    std::int64_t scaled = 0;
    if (__builtin_mul_overflow(mantissa, pow10_i64(static_cast<int>(shift)),
                               &scaled))
      return false;
    out = scaled;
    return true;
  }

  if (-shift > kMaxI64DecimalScale)
    return false;
  const std::int64_t divisor = pow10_i64(static_cast<int>(-shift));

  if (mantissa % divisor != 0)
    return false;
  out = mantissa / divisor;
  return true;
}

[[nodiscard]] inline std::int64_t
normalize_decimal_or(std::int64_t mantissa, std::int32_t exponent,
                     std::uint8_t scale, std::int64_t fallback) noexcept {
  std::int64_t out = 0;
  return normalize_decimal(mantissa, exponent, scale, out) ? out : fallback;
}

[[nodiscard]] inline bool decimal_to_fixed(const DecimalParts &d,
                                           std::uint8_t scale,
                                           std::int64_t &out) noexcept {
  return d.ok && normalize_decimal(d.mantissa, d.exponent, scale, out);
}

[[nodiscard]] inline bool decimal_string_to_fixed(std::string_view s,
                                                  std::uint8_t scale,
                                                  std::int64_t &out) noexcept {
  return decimal_to_fixed(parse_decimal_parts(s), scale, out);
}

[[nodiscard]] inline bool decimal_string_fits(std::string_view s,
                                              std::uint8_t scale) noexcept {
  std::int64_t ignored = 0;
  return decimal_string_to_fixed(s, scale, ignored);
}

}
