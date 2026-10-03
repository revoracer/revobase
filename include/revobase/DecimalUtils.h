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
  // how many digits sit right of the decimal point, capped at 18
  std::uint8_t significant_decimal_places{0};
  // False when the string was malformed, or when the value does not fit in
  // `mantissa`. Sits in the struct's existing tail padding, so DecimalParts is
  // still 16 bytes.
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

// Magnitude budget: |INT64_MIN| == 2^63 is the widest value any sign can name.
inline constexpr std::uint64_t kMagLimit = std::uint64_t{1} << 63;
inline constexpr std::uint64_t kMagLimitDiv10 = kMagLimit / 10;
inline constexpr std::uint64_t kMagLimitMod10 = kMagLimit % 10;

// Point at which the exponent accumulator stops accumulating. Note this is NOT
// a maximum: the loop tests before the multiply, so sci_exp can leave it one
// digit larger (~10^7). All that matters is the pair of properties below.
//
// The digits of a scientific exponent still have to be scanned even once its
// value is hopeless, because "1e999999999999x" must be rejected for the
// trailing 'x' - the parser cannot stop early. Left unguarded, that scan
// overflows a signed int32 (UB). Saturating instead keeps it well-defined.
//
// So the constant only has to be:
//   - large enough that any exponent reaching it is unrepresentable at every
//     scale, so saturating loses no value we could have parsed; and
//   - small enough that one further `*10 + digit` stays far from INT32_MAX.
// 10'000 or 100'000 would serve equally well; the value is arbitrary within
// those bounds, the two properties are not.
inline constexpr std::int32_t kSciExpAccumulateLimit = 1'000'000;

// `Checked` is false for the short-string path, where the digit count alone
// guarantees the uint64 accumulator cannot wrap, so the hot loop carries no
// per-digit test at all. See parse_decimal_parts for why that is sound.
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
          // No magnitude budget left. A dropped fractional zero is
          // insignificant and can be ignored; anything else would change the
          // value, so the string is not representable.
          if (after_dot && c == '0')
            continue;
          return d;
        }
      }
      mag = mag * 10 + digit;
      if (after_dot)
        ++frac_digits;
    } else if (c == '.') {
      if (after_dot) // a second decimal point is not a number
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
          return d; // trailing junk inside the exponent
        if (sci_exp <= kSciExpAccumulateLimit)
          sci_exp = sci_exp * 10 + static_cast<std::int32_t>(ec - '0');
      }
      if (i == exp_start) // "1.5e" / "1.5e+"
        return d;
      if (exp_neg)
        sci_exp = -sci_exp;
      break;
    } else {
      // for junk before an exponent is entered
      // Examples:
      // "12x34"     // x
      // "1,234.5"   // comma
      // "12 34"     // space
      // "abc"       // a
      return d; // stray character
    }
  }

  if (!any_digit)
    return d;

  const std::uint64_t limit = neg ? kMagLimit : kMagLimit - 1;
  if (mag > limit)
    return d;

  // Two's-complement round trip; the conversion is modular since C++20, so
  // this also names INT64_MIN exactly.
  d.mantissa = neg ? static_cast<std::int64_t>(~mag + 1)
                   : static_cast<std::int64_t>(mag);

  // strip trailing zero - remove insignificant fractional zeros before
  // computing the exponent e.g. "1.200" -> mantissa 1200 -> 12,
  // frag_digits 3 -> 1
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

} // namespace detail

inline DecimalParts parse_decimal_parts(std::string_view s) noexcept {
  // A string of at most 19 characters cannot hold more than 19 digits (every
  // digit costs a character, and a sign or '.' only takes from that budget),
  // and the largest 19-digit number is 9.99e18 against a uint64 ceiling of
  // 1.84e19. So the accumulator provably cannot wrap and the digit loop needs
  // no overflow test whatsoever - one range check after the loop covers it.
  // note: fitting in uint64_t doesn't mean fitting in int64_t, so we have
  // a check of "limit" inside
  //
  // Note the magnitude may still land outside int64 range on this path (19
  // digits reach ~1e19, INT64_MAX is 9.22e18). That is the point: in a uint64
  // it is a well-defined large number the end check can reject, where in an
  // int64 it would already have been UB(undefined behavior) and unrecoverable.
  //
  // The guarded path is not just for malformed input. Binance publishes its
  // exchangeInfo limits as "92233720368.54775807" - 20 characters, 19 digits,
  // valid, and exactly INT64_MAX at scale 8. It is startup-only, which is why
  // paying a per-digit check there is fine.
  if (s.size() <= 19) [[likely]]
    return detail::parse_decimal_parts_impl<false>(s);
  return detail::parse_decimal_parts_impl<true>(s);
}

/* The input value is mantissa × 10^exponent. You want the output R such that:
  R × 10^(-scale) = mantissa × 10^exponent
  R = mantissa × 10^(scale + exponent)
  R = mantissa × 10^shift     where shift = scale + exponent
 */
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
    // One imul plus a jo that is never taken on conforming data.
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
  // Narrowing is allowed only when it is lossless. Truncating here would
  // silently move a price, and parse_decimal_parts has already dropped the
  // insignificant trailing zeros, so a non-zero remainder means real digits
  // would be discarded.
  if (mantissa % divisor != 0)
    return false;
  out = mantissa / divisor;
  return true;
}

// For callers that have already established representability for the whole
// message (see the SBE level pre-check) and just need the value inline. The
// explicit fallback is what keeps this from being the silent-garbage form the
// bare int64 signature used to be: if the guarantee is ever broken the caller
// gets the value it nominated, not a wrapped one.
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

// Startup-time helper: can `s` be held at `scale` without loss? Used to reject
// an instrument whose venue-advertised limits do not fit the configured scale,
// so that the per-message path never meets a value it cannot represent.
[[nodiscard]] inline bool decimal_string_fits(std::string_view s,
                                              std::uint8_t scale) noexcept {
  std::int64_t ignored = 0;
  return decimal_string_to_fixed(s, scale, ignored);
}

} // namespace revobase
