// test_timeutils.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

#include <revobase/TimeUtils.h>

using namespace revobase;
using namespace std::chrono_literals;

// For floating point comparisons
constexpr double EPSILON = 1e-9;

TEST_CASE("Timestamp construction and basic operations", "[timestamp]") {
  SECTION("Default construction") {
    Timestamp ts;
    REQUIRE(ts.nsec() == 0);
    REQUIRE(ts == Timestamp::min());
    REQUIRE(ts.isValid());
  }

  SECTION("Explicit construction") {
    const std::int64_t nsec_value = 1234567890123456789L;
    Timestamp ts(nsec_value);
    REQUIRE(ts.nsec() == nsec_value);
    REQUIRE(ts.isValid());
  }

  SECTION("Invalid timestamp") {
    auto invalid_ts = Timestamp::invalid();
    REQUIRE_FALSE(invalid_ts.isValid());
    REQUIRE(invalid_ts.nsec() == std::numeric_limits<std::int64_t>::min());
  }

  SECTION("MAX_TIME constant") {
    auto max_ts = Timestamp::MAX_TIME();
    REQUIRE(max_ts.nsec() == 4102444800000000000L);
    REQUIRE(max_ts.isValid());
    REQUIRE(max_ts == MAX_TIME); // Global constant
  }

  SECTION("Timestamp::now() returns valid timestamps") {
    auto ts1 = Timestamp::now();
    REQUIRE(ts1.isValid());
    REQUIRE(ts1.nsec() > 0);

    auto ts2 = Timestamp::now();
    REQUIRE(ts2.isValid());
    REQUIRE(ts2 >= ts1); // Monotonic property
  }
}

TEST_CASE("Timestamp comparison operators", "[timestamp]") {
  Timestamp ts1(1000);
  Timestamp ts2(2000);
  Timestamp ts3(1000);

  SECTION("Equality") {
    REQUIRE(ts1 == ts1);
    REQUIRE(ts1 == ts3);
    REQUIRE_FALSE(ts1 == ts2);
  }

  SECTION("Inequality") {
    REQUIRE(ts1 != ts2);
    REQUIRE_FALSE(ts1 != ts3);
  }

  SECTION("Less than") {
    REQUIRE(ts1 < ts2);
    REQUIRE_FALSE(ts2 < ts1);
    REQUIRE_FALSE(ts1 < ts3);
  }

  SECTION("Less than or equal") {
    REQUIRE(ts1 <= ts2);
    REQUIRE(ts1 <= ts3);
    REQUIRE_FALSE(ts2 <= ts1);
  }

  SECTION("Greater than") {
    REQUIRE(ts2 > ts1);
    REQUIRE_FALSE(ts1 > ts2);
    REQUIRE_FALSE(ts1 > ts3);
  }

  SECTION("Greater than or equal") {
    REQUIRE(ts2 >= ts1);
    REQUIRE(ts1 >= ts3);
    REQUIRE_FALSE(ts1 >= ts2);
  }
}

TEST_CASE("Timestamp and chrono duration interactions", "[timestamp]") {
  Timestamp ts(1000000000); // 1 second in nanoseconds
  // milliseconds converts to nanoseconds implicitly: the conversion is exact.
  auto d = 500ms;

  SECTION("Timestamp + duration") {
    auto result = ts + d;
    REQUIRE(result.nsec() == 1500000000);
    REQUIRE(result > ts);
  }

  SECTION("Timestamp - duration") {
    auto result = ts - d;
    REQUIRE(result.nsec() == 500000000);
    REQUIRE(result < ts);
  }

  SECTION("Timestamp += duration") {
    Timestamp t = ts;
    t += d;
    REQUIRE(t.nsec() == 1500000000);
  }

  SECTION("Timestamp -= duration") {
    Timestamp t = ts;
    t -= d;
    REQUIRE(t.nsec() == 500000000);
  }

  SECTION("Timestamp - Timestamp = nanoseconds") {
    Timestamp ts1(2000000000);
    Timestamp ts2(1000000000);
    std::chrono::nanoseconds diff = ts1 - ts2;
    REQUIRE(diff.count() == 1000000000);
    REQUIRE_THAT(std::chrono::duration<double>(diff).count(),
                 Catch::Matchers::WithinAbs(1.0, EPSILON));
  }
}

TEST_CASE("Timestamp string conversions", "[timestamp]") {
  SECTION("fromYYYYmmddHHMMSS") {
    auto ts = Timestamp::fromYYYYmmddHHMMSS("2024-01-15 14:30:45");
    REQUIRE(ts.isValid());
    REQUIRE(ts.nsec() > 0);

    // Verify the string representation contains the expected date
    auto str = ts.toString();
    REQUIRE(str.find("2024-01-15") != std::string::npos);
    REQUIRE(str.find("14:30:45") != std::string::npos);
  }

  SECTION("fromYYYYmmdd") {
    auto ts = Timestamp::fromYYYYmmdd("2024-01-15");
    REQUIRE(ts.isValid());

    auto str = ts.toString();
    REQUIRE(str.find("2024-01-15") != std::string::npos);
    REQUIRE(str.find("00:00:00") != std::string::npos);
  }

  SECTION("toDateString") {
    auto ts = Timestamp::fromYYYYmmddHHMMSS("2024-01-15 14:30:45");
    auto date_str = ts.toDateString();
    REQUIRE(date_str == "2024-01-15");
  }

  SECTION("Invalid date format throws") {
    REQUIRE_THROWS(Timestamp::fromYYYYmmddHHMMSS("invalid"));
    REQUIRE_THROWS(Timestamp::fromYYYYmmddHHMMSS("2024-01-15")); // Missing time
    REQUIRE_THROWS(Timestamp::fromYYYYmmdd("2024-1-15"));        // Wrong format
    REQUIRE_THROWS(Timestamp::fromYYYYmmdd("20240115")); // No separators
  }
}

TEST_CASE("Timestamp::toString formatting", "[timestamp]") {
  auto ts = Timestamp::fromYYYYmmddHHMMSS("2024-01-15 14:30:45");
  auto str = ts.toString();

  SECTION("Format validation") {
    // Expected format: "YYYY-MM-DD HH:MM:SS.nnnnnnnnn"
    REQUIRE(str.length() == 29); // 19 for datetime + 1 dot + 9 for nanos
    REQUIRE(str[4] == '-');
    REQUIRE(str[7] == '-');
    REQUIRE(str[10] == ' ');
    REQUIRE(str[13] == ':');
    REQUIRE(str[16] == ':');
    REQUIRE(str[19] == '.');
  }
}

TEST_CASE("Timestamp monotonicity", "[timestamp]") {
  SECTION("Sequential calls are monotonic") {
    const int iterations = 1000;
    std::vector<Timestamp> timestamps;
    timestamps.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
      timestamps.push_back(Timestamp::now());
    }

    for (size_t i = 1; i < timestamps.size(); ++i) {
      REQUIRE(timestamps[i] >= timestamps[i - 1]);
    }
  }

  SECTION("Timestamps increase over time") {
    auto ts1 = Timestamp::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto ts2 = Timestamp::now();

    REQUIRE(ts2 > ts1);
    const auto diff_ms =
        std::chrono::duration<double, std::milli>(ts2 - ts1).count();
    REQUIRE(diff_ms >= 1.0);
    REQUIRE(diff_ms < 10.0); // Reasonable upper bound
  }
}

TEST_CASE("Hash functions", "[timestamp]") {
  SECTION("Timestamp hash") {
    Timestamp ts1(1234567890);
    Timestamp ts2(1234567890);
    Timestamp ts3(9876543210);

    std::hash<Timestamp> hasher;
    REQUIRE(hasher(ts1) == hasher(ts2));
    REQUIRE(hasher(ts1) != hasher(ts3));
  }


  SECTION("Can use in unordered_map") {
    std::unordered_map<Timestamp, int> ts_map;

    ts_map[Timestamp(1000)] = 1;
    ts_map[Timestamp(2000)] = 2;
    REQUIRE(ts_map.size() == 2);
    REQUIRE(ts_map[Timestamp(1000)] == 1);
  }
}

TEST_CASE("Edge cases and boundary conditions", "[timestamp]") {
  SECTION("Zero duration operations") {
    Timestamp ts(1000);
    std::chrono::nanoseconds zero{};

    REQUIRE((ts + zero) == ts);
    REQUIRE((ts - zero) == ts);
  }

  SECTION("Negative durations") {
    Timestamp ts(1000000000);
    auto neg_dur = -500ms;

    auto result = ts + neg_dur;
    REQUIRE(result.nsec() == 500000000);
    REQUIRE(result < ts);
  }

  SECTION("Large values") {
    // Close to max int64
    Timestamp large_ts(9000000000000000000L);
    REQUIRE(large_ts.isValid());
  }

  SECTION("Precision preservation") {
    // Test that nanosecond precision is maintained
    Timestamp ts1(123456789);
    Timestamp ts2(123456790);
    REQUIRE(ts2 > ts1);

    auto diff = ts2 - ts1;
    REQUIRE(diff.count() == 1);
  }
}
