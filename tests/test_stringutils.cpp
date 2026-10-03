#include <array>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <revobase/StringUtils.hpp>

using namespace revobase;

namespace {

// StringUtils no longer ships number parsers. The trading-scenario cases below
// exercise the splitting/trimming around the numbers, so they pull the values
// out with std::from_chars.
template <typename T> bool parseNumber(std::string_view s, T &out) noexcept {
  const char *first = s.data();
  const char *last = first + s.size();
  if (first != last && *first == '+')
    ++first; // from_chars rejects a leading '+'
  const auto res = std::from_chars(first, last, out);
  return res.ec == std::errc{} && res.ptr == last;
}

} // namespace

// Test fixture for performance tests
class StringUtilsPerfFixture {
public:
  StringUtilsPerfFixture() {
    // Trading symbols
    symbols = {"AAPL", "GOOGL", "MSFT", "AMZN", "META", "TSLA", "NVDA", "JPM"};

    // FIX-like messages
    fixMessages = {
        "8=FIX.4.2|9=178|35=D|49=SENDER|56=TARGET|34=1|52=20250110-12:30:00|",
        "8=FIX.4.2|9=154|35=8|49=TARGET|56=SENDER|34=2|52=20250110-12:30:01|",
        "8=FIX.4.2|9=201|35=D|49=SENDER|56=TARGET|34=3|52=20250110-12:30:02|"};

    // CSV data
    csvLine = "AAPL,150.50,1000,BUY,LIMIT,2025-01-10 12:30:00.123456";

    // Price strings
    prices = {"150.50", "99.99", "1234.5678", "0.001", "10000.00"};
  }

  std::vector<std::string> symbols;
  std::vector<std::string> fixMessages;
  std::string csvLine;
  std::vector<std::string> prices;
};

// ============================================================================
// BASIC FUNCTIONALITY TESTS
// ============================================================================

TEST_CASE("StringUtils replaceAll", "[string]") {
  SECTION("Replace substring") {
    std::string s = "hello world hello";
    StringUtils::replaceAll(s, "hello", "hi");
    REQUIRE(s == "hi world hi");
  }

  SECTION("Replace with longer string") {
    std::string s = "a b a b";
    StringUtils::replaceAll(s, "a", "abc");
    REQUIRE(s == "abc b abc b");
  }

  SECTION("No match") {
    std::string s = "hello world";
    StringUtils::replaceAll(s, "foo", "bar");
    REQUIRE(s == "hello world");
  }

  SECTION("Empty from string") {
    std::string s = "test";
    StringUtils::replaceAll(s, "", "x");
    REQUIRE(s == "test");
  }
}

TEST_CASE("StringUtils replaceChar", "[string]") {
  SECTION("Replace single character") {
    std::string s = "hello world";
    StringUtils::replaceChar(s, ' ', '_');
    REQUIRE(s == "hello_world");
  }

  SECTION("Replace multiple occurrences") {
    std::string s = "a,b,c,d";
    StringUtils::replaceChar(s, ',', '|');
    REQUIRE(s == "a|b|c|d");
  }
}

TEST_CASE("StringUtils case operations", "[string]") {
  SECTION("isUpper for trading symbols") {
    REQUIRE(StringUtils::isUpper("AAPL"));
    REQUIRE(StringUtils::isUpper("GOOGL123"));
    REQUIRE(StringUtils::isUpper("MSFT_US"));
    REQUIRE_FALSE(StringUtils::isUpper("Apple"));
    REQUIRE(StringUtils::isUpper(""));
  }

  SECTION("toLower") {
    REQUIRE(StringUtils::toLower("HELLO") == "hello");
    REQUIRE(StringUtils::toLower("HeLLo123") == "hello123");
    REQUIRE(StringUtils::toLower("already_lower") == "already_lower");
  }

  SECTION("toLowerCopy") {
    std::string_view s = "HeLLo123";
    REQUIRE(StringUtils::toLowerCopy(s) == "hello123");
  }

  SECTION("toLowerInPlace") {
    std::string s = "HELLO";
    StringUtils::toLowerInPlace(s);
    REQUIRE(s == "hello");
  }

  SECTION("toUpper") {
    REQUIRE(StringUtils::toUpper("hello") == "HELLO");
    REQUIRE(StringUtils::toUpper("HeLLo123") == "HELLO123");
    REQUIRE(StringUtils::toUpper("ALREADY_UPPER") == "ALREADY_UPPER");
  }

  SECTION("toUpperCopy") {
    std::string_view s = "btcusdt";
    REQUIRE(StringUtils::toUpperCopy(s) == "BTCUSDT");
  }

  SECTION("toUpperInPlace") {
    std::string s = "hello";
    StringUtils::toUpperInPlace(s);
    REQUIRE(s == "HELLO");
  }
}

TEST_CASE("StringUtils split operations", "[string]") {
  SECTION("Basic split") {
    auto result = StringUtils::split("a,b,c,d", ',');
    REQUIRE(result.size() == 4);
    REQUIRE(result[0] == "a");
    REQUIRE(result[3] == "d");
  }

  SECTION("Split with empty tokens") {
    auto result = StringUtils::split("a,,c", ',');
    REQUIRE(result.size() == 3);
    REQUIRE(result[1] == "");
  }

  SECTION("splitView (zero-copy)") {
    std::string s = "AAPL,150.50,1000";
    auto result = StringUtils::splitView(s, ',');
    REQUIRE(result.size() == 3);
    REQUIRE(result[0] == "AAPL");
    REQUIRE(result[1] == "150.50");
    REQUIRE(result[2] == "1000");
  }

  SECTION("splitFixed (fixed-size array)") {
    std::array<std::string_view, 6> fields;
    std::string s = "AAPL,150.50,1000,BUY,LIMIT,2025-01-10";
    size_t count = StringUtils::splitFixed(s, ',', fields);

    REQUIRE(count == 6);
    REQUIRE(fields[0] == "AAPL");
    REQUIRE(fields[3] == "BUY");
  }

  SECTION("splitAny (multiple delimiters)") {
    auto result = StringUtils::splitAny("a b\tc,d", " \t,");
    REQUIRE(result.size() == 4);
    REQUIRE(result[0] == "a");
    REQUIRE(result[3] == "d");
  }
}

TEST_CASE("StringUtils join operations", "[string]") {
  SECTION("Basic join") {
    std::vector<std::string> parts = {"a", "b", "c"};
    REQUIRE(StringUtils::joinContainer(parts, ",") == "a,b,c");
  }

  SECTION("Join with empty vector") {
    std::vector<std::string> parts;
    REQUIRE(StringUtils::joinContainer(parts, ",") == "");
  }

  SECTION("Join single element") {
    std::vector<std::string> parts = {"only"};
    REQUIRE(StringUtils::joinContainer(parts, ",") == "only");
  }

  SECTION("Join with multi-char separator") {
    std::vector<std::string> parts = {"a", "b", "c"};
    REQUIRE(StringUtils::joinContainer(parts, " | ") == "a | b | c");
  }
}

TEST_CASE("StringUtils trim operations", "[string]") {
  SECTION("Trim both sides") {
    REQUIRE(StringUtils::trim("  hello  ") == "hello");
    REQUIRE(StringUtils::trim("\t\nhello\r\n") == "hello");
    REQUIRE(StringUtils::trim("no_trim") == "no_trim");
  }

  SECTION("Trim left") {
    REQUIRE(StringUtils::trimLeft("  hello") == "hello");
    REQUIRE(StringUtils::trimLeft("hello  ") == "hello  ");
  }

  SECTION("Trim right") {
    REQUIRE(StringUtils::trimRight("hello  ") == "hello");
    REQUIRE(StringUtils::trimRight("  hello") == "  hello");
  }

  SECTION("Trim all whitespace") { REQUIRE(StringUtils::trim("   ") == ""); }
}

TEST_CASE("StringUtils comparison operations", "[string]") {
  SECTION("equalsIgnoreCase") {
    REQUIRE(StringUtils::equalsIgnoreCase("AAPL", "aapl"));
    REQUIRE(StringUtils::equalsIgnoreCase("Hello", "HELLO"));
    REQUIRE_FALSE(StringUtils::equalsIgnoreCase("AAPL", "GOOGL"));
    REQUIRE_FALSE(StringUtils::equalsIgnoreCase("test", "test2"));
  }

  SECTION("startsWith") {
    REQUIRE(StringUtils::startsWith("ORDER_12345", "ORDER_"));
    REQUIRE(StringUtils::startsWith("test", "test"));
    REQUIRE_FALSE(StringUtils::startsWith("test", "testing"));
  }

  SECTION("endsWith") {
    REQUIRE(StringUtils::endsWith("file.txt", ".txt"));
    REQUIRE(StringUtils::endsWith("test", "test"));
    REQUIRE_FALSE(StringUtils::endsWith("test", "testing"));
  }
}

TEST_CASE("StringUtils utility operations", "[string]") {
  SECTION("contains") {
    REQUIRE(StringUtils::contains("hello world", "world"));
    REQUIRE(StringUtils::contains("test", "test"));
    REQUIRE_FALSE(StringUtils::contains("hello", "world"));
  }

  SECTION("countChar") {
    REQUIRE(StringUtils::countChar("hello", 'l') == 2);
    REQUIRE(StringUtils::countChar("test", 'x') == 0);
    REQUIRE(StringUtils::countChar("a,b,c,d", ',') == 3);
  }

  SECTION("padRight") {
    REQUIRE(StringUtils::padRight("test", 8) == "test    ");
    REQUIRE(StringUtils::padRight("test", 8, '0') == "test0000");
    REQUIRE(StringUtils::padRight("toolong", 4) == "toolong");
  }

  SECTION("padLeft") {
    REQUIRE(StringUtils::padLeft("test", 8) == "    test");
    REQUIRE(StringUtils::padLeft("42", 8, '0') == "00000042");
    REQUIRE(StringUtils::padLeft("toolong", 4) == "toolong");
  }
}

// ============================================================================
// PERFORMANCE TESTS
// ============================================================================

TEST_CASE_METHOD(StringUtilsPerfFixture,
                 "StringUtils latency - case conversion",
                 "[string][latency][!benchmark]") {

  BENCHMARK_ADVANCED("toLower (copy)")(Catch::Benchmark::Chronometer meter) {
    std::string result;
    meter.measure([&] {
      result = StringUtils::toLower("AAPL");
      return result.length(); // Return something to prevent optimization
    });
  };

  BENCHMARK_ADVANCED("toLowerInPlace (no allocation)")(
      Catch::Benchmark::Chronometer meter) {
    std::string s = "AAPL";
    meter.measure([&] {
      StringUtils::toLowerInPlace(s);
      return s.length();
    });
  };

  BENCHMARK_ADVANCED("toUpper (copy)")(Catch::Benchmark::Chronometer meter) {
    std::string result;
    meter.measure([&] {
      result = StringUtils::toUpper("aapl");
      return result.length();
    });
  };

  BENCHMARK_ADVANCED("isUpper check")(Catch::Benchmark::Chronometer meter) {
    bool result;
    meter.measure([&] {
      result = StringUtils::isUpper("AAPL");
      return result;
    });
  };
}

TEST_CASE_METHOD(StringUtilsPerfFixture,
                 "StringUtils latency - split operations",
                 "[string][latency][!benchmark]") {

  BENCHMARK_ADVANCED("split (allocating strings)")(
      Catch::Benchmark::Chronometer meter) {
    size_t result;
    meter.measure([&] {
      auto parts = StringUtils::split(csvLine, ',');
      result = parts.size();
      return result;
    });
  };

  BENCHMARK_ADVANCED("splitView (zero-copy)")(
      Catch::Benchmark::Chronometer meter) {
    size_t result;
    meter.measure([&] {
      auto parts = StringUtils::splitView(csvLine, ',');
      result = parts.size();
      return result;
    });
  };

  BENCHMARK_ADVANCED("splitFixed (stack array)")(
      Catch::Benchmark::Chronometer meter) {
    std::array<std::string_view, 8> fields;
    size_t result;
    meter.measure([&] {
      result = StringUtils::splitFixed(csvLine, ',', fields);
      return result;
    });
  };
}

TEST_CASE_METHOD(StringUtilsPerfFixture, "StringUtils latency - comparison",
                 "[string][latency][!benchmark]") {

  BENCHMARK_ADVANCED("equalsIgnoreCase")(Catch::Benchmark::Chronometer meter) {
    bool result;
    meter.measure([&] {
      result = StringUtils::equalsIgnoreCase("AAPL", "aapl");
      return result;
    });
  };

  BENCHMARK_ADVANCED("startsWith")(Catch::Benchmark::Chronometer meter) {
    bool result;
    meter.measure([&] {
      result = StringUtils::startsWith("ORDER_12345_BUY", "ORDER_");
      return result;
    });
  };

  BENCHMARK_ADVANCED("endsWith")(Catch::Benchmark::Chronometer meter) {
    bool result;
    meter.measure([&] {
      result = StringUtils::endsWith("file.csv", ".csv");
      return result;
    });
  };
}

TEST_CASE_METHOD(StringUtilsPerfFixture,
                 "StringUtils latency - FIX message parsing",
                 "[string][latency][fix][!benchmark]") {

  BENCHMARK_ADVANCED("Parse FIX message with split")(
      Catch::Benchmark::Chronometer meter) {
    size_t result;
    meter.measure([&] {
      auto parts = StringUtils::split(fixMessages[0], '|');
      result = parts.size();
      return result;
    });
  };

  BENCHMARK_ADVANCED("Parse FIX message with splitView (zero-copy)")(
      Catch::Benchmark::Chronometer meter) {
    size_t result;
    meter.measure([&] {
      auto parts = StringUtils::splitView(fixMessages[0], '|');
      result = parts.size();
      return result;
    });
  };

  BENCHMARK_ADVANCED("Parse FIX message batch (1000 messages)")(
      Catch::Benchmark::Chronometer meter) {
    size_t sum;
    meter.measure([&] {
      sum = 0;
      for (int i = 0; i < 1000; ++i) {
        auto parts =
            StringUtils::splitView(fixMessages[i % fixMessages.size()], '|');
        sum += parts.size();
      }
      return sum;
    });
  };

  BENCHMARK_ADVANCED("Extract specific FIX field")(
      Catch::Benchmark::Chronometer meter) {
    std::string_view result;
    meter.measure([&] {
      auto parts = StringUtils::splitView(fixMessages[0], '|');
      // Find field 35 (message type)
      for (const auto &part : parts) {
        if (StringUtils::startsWith(part, "35=")) {
          result = part.substr(3);
          break;
        }
      }
      return result.length();
    });
  };
}

TEST_CASE_METHOD(StringUtilsPerfFixture,
                 "StringUtils latency - trim operations",
                 "[string][latency][!benchmark]") {

  std::string padded = "   AAPL   ";

  BENCHMARK_ADVANCED("trim (both sides)")(Catch::Benchmark::Chronometer meter) {
    std::string_view result;
    meter.measure([&] {
      result = StringUtils::trim(padded);
      return result.length();
    });
  };

  BENCHMARK_ADVANCED("trimLeft")(Catch::Benchmark::Chronometer meter) {
    std::string_view result;
    meter.measure([&] {
      result = StringUtils::trimLeft(padded);
      return result.length();
    });
  };

  BENCHMARK_ADVANCED("trimRight")(Catch::Benchmark::Chronometer meter) {
    std::string_view result;
    meter.measure([&] {
      result = StringUtils::trimRight(padded);
      return result.length();
    });
  };
}

TEST_CASE_METHOD(StringUtilsPerfFixture,
                 "StringUtils latency - replace operations",
                 "[string][latency][!benchmark]") {

  BENCHMARK_ADVANCED("replaceChar (single char)")(
      Catch::Benchmark::Chronometer meter) {
    std::string s = "hello world hello world";
    meter.measure([&] {
      StringUtils::replaceChar(s, ' ', '_');
      return s;
    });
  };

  BENCHMARK_ADVANCED("replaceAll (substring)")(
      Catch::Benchmark::Chronometer meter) {
    std::string s = "hello world hello world";
    meter.measure([&] {
      StringUtils::replaceAll(s, "hello", "hi");
      return s;
    });
  };
}

TEST_CASE_METHOD(StringUtilsPerfFixture,
                 "StringUtils latency - join operations",
                 "[string][latency][!benchmark]") {

  std::vector<std::string> fields = {"AAPL", "150.50", "1000", "BUY", "LIMIT"};

  BENCHMARK_ADVANCED("join 5 fields")(Catch::Benchmark::Chronometer meter) {
    std::string result;
    meter.measure([&] {
      result = StringUtils::joinContainer(fields, ",");
      return result.length();
    });
  };

  BENCHMARK_ADVANCED("join 8 symbols")(Catch::Benchmark::Chronometer meter) {
    std::string result;
    meter.measure([&] {
      result = StringUtils::joinContainer(symbols, "|");
      return result.length();
    });
  };
}

// ============================================================================
// REAL-WORLD TRADING SCENARIOS
// ============================================================================

TEST_CASE("StringUtils - Trading message parsing scenario",
          "[string][trading]") {
  SECTION("Parse CSV order message") {
    std::string msg = "AAPL,BUY,100,150.50,LIMIT,IOC";
    auto fields = StringUtils::splitView(msg, ',');

    REQUIRE(fields.size() == 6);
    REQUIRE(fields[0] == "AAPL");
    REQUIRE(fields[1] == "BUY");

    int64_t quantity;
    REQUIRE(parseNumber(fields[2], quantity));
    REQUIRE(quantity == 100);

    double price;
    REQUIRE(parseNumber(fields[3], price));
    REQUIRE(price == 150.50);
  }

  SECTION("Parse FIX-like message") {
    std::string fix = "8=FIX.4.2|35=D|55=AAPL|54=1|38=100|40=2|44=150.50|";
    auto fields = StringUtils::splitView(fix, '|');

    std::string_view symbol;
    int64_t side = 0, quantity = 0;
    double price = 0.0;

    for (const auto &field : fields) {
      if (StringUtils::startsWith(field, "55=")) {
        symbol = field.substr(3);
      } else if (StringUtils::startsWith(field, "54=")) {
        parseNumber(field.substr(3), side);
      } else if (StringUtils::startsWith(field, "38=")) {
        parseNumber(field.substr(3), quantity);
      } else if (StringUtils::startsWith(field, "44=")) {
        parseNumber(field.substr(3), price);
      }
    }

    REQUIRE(symbol == "AAPL");
    REQUIRE(side == 1);
    REQUIRE(quantity == 100);
    REQUIRE(price == 150.50);
  }

  SECTION("Normalize symbol (case insensitive lookup)") {
    std::string symbol = "aapl";
    StringUtils::toUpperInPlace(symbol);
    REQUIRE(symbol == "AAPL");
    REQUIRE(StringUtils::isUpper(symbol));
  }

  SECTION("Build order key for hash map") {
    std::vector<std::string> parts = {"ORDER", "12345", "AAPL", "BUY"};
    std::string key = StringUtils::joinContainer(parts, "_");
    REQUIRE(key == "ORDER_12345_AAPL_BUY");
  }
}

TEST_CASE("StringUtils - Fixed-width protocol parsing", "[string][trading]") {
  SECTION("Parse fixed-width message") {
    // Simulate a fixed-width trading message
    std::string msg = "AAPL  BUY 00100150.50LIMIT";

    // Extract fields (positions are known)
    std::string_view symbol = std::string_view(msg).substr(0, 6);
    std::string_view side = std::string_view(msg).substr(6, 4);
    std::string_view qty_str = std::string_view(msg).substr(10, 5);
    std::string_view price_str = std::string_view(msg).substr(15, 6);
    std::string_view type = std::string_view(msg).substr(21, 5);

    // Trim and validate
    symbol = StringUtils::trim(symbol);
    side = StringUtils::trim(side);
    type = StringUtils::trim(type);

    REQUIRE(symbol == "AAPL");
    REQUIRE(side == "BUY");
    REQUIRE(type == "LIMIT");

    int64_t qty;
    REQUIRE(parseNumber(qty_str, qty));
    REQUIRE(qty == 100);

    double price;
    REQUIRE(parseNumber(price_str, price));
    REQUIRE(price == 150.50);
  }

  SECTION("Build fixed-width message") {
    std::string symbol = StringUtils::padRight("AAPL", 6);
    std::string side = StringUtils::padRight("BUY", 4);
    std::string qty = StringUtils::padLeft("100", 5, '0');
    std::string price = "150.50";
    std::string type = StringUtils::padRight("LIMIT", 5);

    std::string msg = symbol + side + qty + price + type;
    REQUIRE(msg.length() == 26);
    REQUIRE(StringUtils::trim(std::string_view(msg).substr(0, 6)) == "AAPL");
  }
}

TEST_CASE("StringUtils - Market data parsing scenario", "[string][trading]") {
  SECTION("Parse multi-level quote") {
    // Format: SYMBOL|BID1,SIZE1|BID2,SIZE2|ASK1,SIZE1|ASK2,SIZE2
    std::string quote = "AAPL|150.48,100|150.47,200|150.52,150|150.53,250";
    auto parts = StringUtils::splitView(quote, '|');

    REQUIRE(parts.size() == 5);
    REQUIRE(parts[0] == "AAPL");

    // Parse bid level 1
    auto bid1 = StringUtils::splitView(parts[1], ',');
    REQUIRE(bid1.size() == 2);

    double bid_price;
    int64_t bid_size;
    REQUIRE(parseNumber(bid1[0], bid_price));
    REQUIRE(parseNumber(bid1[1], bid_size));
    REQUIRE(bid_price == 150.48);

    REQUIRE(bid_size == 100);
  }

  SECTION("Parse OHLCV data") {
    std::string ohlcv = "AAPL,150.50,151.00,149.50,150.75,1000000";
    std::array<std::string_view, 6> fields;
    size_t count = StringUtils::splitFixed(ohlcv, ',', fields);

    REQUIRE(count == 6);
    REQUIRE(fields[0] == "AAPL");

    double open, high, low, close;
    int64_t volume;

    REQUIRE(parseNumber(fields[1], open));
    REQUIRE(parseNumber(fields[2], high));
    REQUIRE(parseNumber(fields[3], low));
    REQUIRE(parseNumber(fields[4], close));
    REQUIRE(parseNumber(fields[5], volume));

    REQUIRE(open == 150.50);

    REQUIRE(high == 151.00);

    REQUIRE(low == 149.50);

    REQUIRE(close == 150.75);

    REQUIRE(volume == 1000000);
  }
}

TEST_CASE("StringUtils - Error handling", "[string][trading]") {
  SECTION("Invalid numeric parsing returns false") {
    int64_t int_val;
    double dbl_val;

    REQUIRE_FALSE(parseNumber("INVALID", int_val));
    REQUIRE_FALSE(parseNumber("NOT_A_NUMBER", dbl_val));
    REQUIRE_FALSE(parseNumber("", int_val));
    REQUIRE_FALSE(parseNumber("", dbl_val));
  }

  SECTION("Handle malformed messages gracefully") {
    std::string msg = "INCOMPLETE|FIELD";
    std::array<std::string_view, 5> fields;
    size_t count = StringUtils::splitFixed(msg, '|', fields);

    REQUIRE(count == 2); // Only 2 fields, not 5
  }

  SECTION("Trim handles all-whitespace strings") {
    REQUIRE(StringUtils::trim("   ").empty());
    REQUIRE(StringUtils::trim("\t\n\r").empty());
  }
}
