#include <catch2/catch_test_macros.hpp>

#include <revobase/DecimalUtils.h>

#include <cstdint>
#include <limits>

using namespace revobase;

namespace {

// Terse helper: the conversion must succeed and produce `expected`.
void requires_fixed(std::string_view s, std::uint8_t scale,
                    std::int64_t expected) {
  std::int64_t out = -1;
  REQUIRE(decimal_string_to_fixed(s, scale, out));
  REQUIRE(out == expected);
}

// Terse helper: the conversion must be refused, and must not write `out`.
void requires_rejected(std::string_view s, std::uint8_t scale) {
  std::int64_t out = 0x5EED;
  REQUIRE_FALSE(decimal_string_to_fixed(s, scale, out));
  REQUIRE(out == 0x5EED);
}

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();

} // namespace

TEST_CASE("DecimalUtils parses plain decimal strings", "[decimal]") {
  SECTION("Integer") {
    const auto d = parse_decimal_parts("1000");
    REQUIRE(d.ok);
    REQUIRE(d.mantissa == 1000);
    REQUIRE(d.exponent == 0);
    REQUIRE(d.significant_decimal_places == 0);
  }

  SECTION("Decimal tick with insignificant trailing zeros") {
    const auto d = parse_decimal_parts("0.01000000");
    REQUIRE(d.ok);
    REQUIRE(d.mantissa == 1);
    REQUIRE(d.exponent == -2);
    REQUIRE(d.significant_decimal_places == 2);
  }

  SECTION("Quantity step") {
    const auto d = parse_decimal_parts("0.00001000");
    REQUIRE(d.ok);
    REQUIRE(d.mantissa == 1);
    REQUIRE(d.exponent == -5);
    REQUIRE(d.significant_decimal_places == 5);
  }

  SECTION("Signed decimal") {
    const auto d = parse_decimal_parts("-12.3400");
    REQUIRE(d.ok);
    REQUIRE(d.mantissa == -1234);
    REQUIRE(d.exponent == -2);
    REQUIRE(d.significant_decimal_places == 2);
  }

  SECTION("Zero is canonical") {
    const auto d = parse_decimal_parts("0.00000000");
    REQUIRE(d.ok);
    REQUIRE(d.mantissa == 0);
    REQUIRE(d.exponent == 0);
    REQUIRE(d.significant_decimal_places == 0);
  }
}

TEST_CASE("DecimalUtils parses scientific notation", "[decimal]") {
  SECTION("Negative exponent") {
    const auto d = parse_decimal_parts("1.23e-4");
    REQUIRE(d.ok);
    REQUIRE(d.mantissa == 123);
    REQUIRE(d.exponent == -6);
    REQUIRE(d.significant_decimal_places == 6);
  }

  SECTION("Positive exponent") {
    const auto d = parse_decimal_parts("1.23E2");
    REQUIRE(d.ok);
    REQUIRE(d.mantissa == 123);
    REQUIRE(d.exponent == 0);
    REQUIRE(d.significant_decimal_places == 0);
  }
}

TEST_CASE("DecimalUtils converts decimal strings to fixed scale", "[decimal]") {
  requires_fixed("0.01", 8, 1'000'000);
  requires_fixed("123.45", 8, 12'345'000'000);
  requires_fixed("0.00001000", 8, 1'000);
  requires_fixed("1000", 8, 100'000'000'000);
}

// ===========================================================================
// Boundary behaviour.
// ===========================================================================

TEST_CASE("DecimalUtils accepts the full int64 range", "[decimal][boundary]") {
  SECTION("18 digits is always safe for the accumulator") {
    const auto d = parse_decimal_parts("999999999999999999");
    REQUIRE(d.ok);
    REQUIRE(d.mantissa == 999'999'999'999'999'999);
  }

  SECTION("19 digits that fit exactly") {
    const auto d = parse_decimal_parts("9223372036854775807");
    REQUIRE(d.ok);
    REQUIRE(d.mantissa == kI64Max);
  }

  SECTION("Binance advertises its limits at exactly INT64_MAX") {
    // maxPrice / maxQty in exchangeInfo are INT64_MAX at scale 8. This must
    // round-trip exactly: it is the widest legal value on the wire.
    requires_fixed("92233720368.54775807", 8, kI64Max);
  }

  SECTION("Most negative value") {
    const auto d = parse_decimal_parts("-9223372036854775808");
    REQUIRE(d.ok);
    REQUIRE(d.mantissa == std::numeric_limits<std::int64_t>::min());
  }
}

TEST_CASE("DecimalUtils rejects mantissa overflow", "[decimal][boundary]") {
  SECTION("19 digits, one past the limit") {
    // Overflowed the multiply and wrapped to -8446744073709551617.
    REQUIRE_FALSE(parse_decimal_parts("9999999999999999999").ok);
    requires_rejected("9999999999999999999", 0);
  }

  SECTION("One ulp past Binance's own sentinel") {
    // Overflowed the ADD, not the multiply, and flipped sign to INT64_MIN.
    requires_rejected("92233720368.54775808", 8);
  }

  SECTION("20 digits") { requires_rejected("12345678901234567890", 0); }

  SECTION("The reported case: 12 integer digits, 9 decimals") {
    requires_rejected("999999999999.123456789", 8);
  }
}

TEST_CASE("DecimalUtils rejects unrepresentable scaling",
          "[decimal][boundary]") {
  SECTION("Ordinary price string, scale too large to hold it") {
    // scale 18 leaves room for values up to ~9.22 only. This produced a
    // NEGATIVE price from a positive nine-character input.
    requires_rejected("110328.80", 18);
    // ...and is fine at the scale the venue actually uses.
    requires_fixed("110328.80", 8, 11'032'880'000'000);
  }

  SECTION("Largest value representable at scale 18") {
    requires_fixed("9.223372036854775807", 18, kI64Max);
    requires_rejected("9.223372036854775808", 18);
  }

  SECTION("Requested shift beyond the power-of-ten table") {
    std::int64_t out = 0;
    REQUIRE_FALSE(normalize_decimal(2, 0, kMaxI64DecimalScale + 1, out));
  }
}

TEST_CASE("DecimalUtils only narrows when it is lossless",
          "[decimal][boundary]") {
  SECTION("Exact narrowing is accepted") {
    std::int64_t out = 0;
    REQUIRE(normalize_decimal(1'000, -9, 8, out)); // 1000 / 10, exact
    REQUIRE(out == 100);
  }

  SECTION("Lossy narrowing is refused rather than truncated") {
    std::int64_t out = 0;
    REQUIRE_FALSE(normalize_decimal(1, -9, 8, out));
    REQUIRE_FALSE(normalize_decimal(123, -10, 8, out));
  }

  SECTION("Truncation never rounds a price toward zero") {
    requires_rejected("0.123456789", 8);
    requires_rejected("-0.123456789", 8);
  }
}

TEST_CASE("DecimalUtils rejects malformed input", "[decimal][boundary]") {
  SECTION("Second decimal point") {
    // Was silently accepted as 1.23.
    REQUIRE_FALSE(parse_decimal_parts("1.2.3").ok);
    requires_rejected("1.2.3", 8);
  }

  SECTION("Embedded junk") {
    // Unknown characters were skipped: "12x34" parsed as 1234.
    requires_rejected("12x34", 8);
    requires_rejected("1,234.5", 8);
    requires_rejected("12 34", 8);
  }

  SECTION("Nothing to parse") {
    // is_zero_decimal("") used to report true.
    REQUIRE_FALSE(parse_decimal_parts("").ok);
    requires_rejected("", 8);
    requires_rejected("-", 8);
    requires_rejected("+", 8);
    requires_rejected(".", 8);
    requires_rejected("abc", 8);
  }

  SECTION("Malformed exponent") {
    requires_rejected("1.5e", 8);
    requires_rejected("1.5e+", 8);
    requires_rejected("1.5e2x", 8);
  }

  SECTION("Exponent that cannot be applied") {
    requires_rejected("1e300", 8);
    requires_rejected("1e-300", 8);
  }
}

TEST_CASE("DecimalUtils normalizes mantissa exponent pairs", "[decimal]") {
  REQUIRE(pow10_i64(0) == 1);
  REQUIRE(pow10_i64(8) == 100'000'000);
  REQUIRE(pow10_i64(18) == 1'000'000'000'000'000'000);

  std::int64_t out = 0;
  REQUIRE(normalize_decimal(12345, -2, 8, out));
  REQUIRE(out == 12'345'000'000);

  REQUIRE(normalize_decimal(1, -8, 8, out));
  REQUIRE(out == 1);
}

TEST_CASE("DecimalUtils fixed-point range helper", "[decimal][boundary]") {
  // Used at instrument-seed time to reject a scale that cannot hold the
  // venue's own advertised limits, so the hot path never has to care.
  REQUIRE(decimal_string_fits("92233720368.54775807", 8));
  REQUIRE_FALSE(decimal_string_fits("92233720368.54775808", 8));
  REQUIRE(decimal_string_fits("110328.80", 8));
  REQUIRE_FALSE(decimal_string_fits("110328.80", 18));
  REQUIRE_FALSE(decimal_string_fits("garbage", 8));
}
