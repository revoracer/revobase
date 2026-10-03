#include <revobase/HashUtils.h>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace revobase;

// Test fixture for performance tests
class HashPerfTestFixture {
public:
  HashPerfTestFixture() {
    // Pre-generate test data
    symbols = {"AAPL", "GOOGL", "MSFT", "AMZN", "META", "TSLA", "NVDA", "JPM"};

    // Generate order keys
    for (int i = 0; i < 10000; ++i) {
      orderKeys.push_back("ORDER_" + std::to_string(i) + "_BUY_100");
    }

    // Generate random binary data
    std::mt19937 rng(12345);
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    binaryData.resize(1024 * 1024); // 1MB
    for (auto &byte : binaryData) {
      byte = dist(rng);
    }
  }

  std::vector<std::string> symbols;
  std::vector<std::string> orderKeys;
  std::vector<uint8_t> binaryData;
};

TEST_CASE("MurmurHash3 basic functionality", "[hash]") {
  SECTION("Same input produces same output") {
    std::string key = "AAPL";
    uint32_t hash1 = HashUtils::hashStr32(key);
    uint32_t hash2 = HashUtils::hashStr32(key);
    REQUIRE(hash1 == hash2);
  }

  SECTION("Different inputs produce different outputs") {
    uint32_t hash1 = HashUtils::hashStr32("AAPL");
    uint32_t hash2 = HashUtils::hashStr32("GOOGL");
    REQUIRE(hash1 != hash2);
  }

  SECTION("Empty string handling") {
    uint32_t hash1 = HashUtils::hashStr32("", 0);
    uint32_t hash2 = HashUtils::hashStr32("", 0);
    REQUIRE(hash1 == hash2); // Consistent hash for empty string

    // Empty string with different seed produces different hash
    uint32_t hash3 = HashUtils::hashStr32("", 42);
    REQUIRE(hash3 != hash1);
  }

  SECTION("Different seeds produce different hashes") {
    std::string key = "MSFT";
    uint32_t hash1 = HashUtils::hashStr32(key, 0);
    uint32_t hash2 = HashUtils::hashStr32(key, 42);
    REQUIRE(hash1 != hash2);
  }
}

TEST_CASE("MurmurHash3 string_view optimization", "[hash]") {
  SECTION("string_view produces same hash as string") {
    std::string str = "TRADING_KEY";
    std::string_view sv = str;

    uint32_t hash1 = HashUtils::hashStr32(str);
    uint32_t hash2 = HashUtils::hashStr32(sv);
    REQUIRE(hash1 == hash2);
  }

  SECTION("Substring hashing without copy") {
    std::string full = "PREFIX_AAPL_SUFFIX";
    std::string_view symbol(full.data() + 7, 4); // "AAPL"

    uint32_t hash1 = HashUtils::hashStr32(symbol);
    uint32_t hash2 = HashUtils::hashStr32("AAPL");
    REQUIRE(hash1 == hash2);
  }
}

TEST_CASE("MurmurHash3 struct hashing", "[hash]") {
  struct OrderID {
    uint64_t timestamp;
    uint32_t counter;
    uint16_t venue;
    uint8_t side;
    uint8_t padding;
  } __attribute__((packed));

  SECTION("Same struct produces same hash") {
    OrderID order1 = {1234567890ULL, 1, 100, 1, 0};
    OrderID order2 = {1234567890ULL, 1, 100, 1, 0};

    uint32_t hash1 = HashUtils::hashStruct32(order1);
    uint32_t hash2 = HashUtils::hashStruct32(order2);
    REQUIRE(hash1 == hash2);
  }

  SECTION("Different structs produce different hashes") {
    OrderID order1 = {1234567890ULL, 1, 100, 1, 0};
    OrderID order2 = {1234567890ULL, 2, 100, 1, 0}; // Different counter

    uint32_t hash1 = HashUtils::hashStruct32(order1);
    uint32_t hash2 = HashUtils::hashStruct32(order2);
    REQUIRE(hash1 != hash2);
  }

  SECTION("Price struct hashing") {
    struct Price {
      int64_t mantissa;
      int32_t exponent;
    } __attribute__((packed));

    Price price1 = {15050, -2}; // 150.50
    Price price2 = {15051, -2}; // 150.51

    uint32_t hash1 = HashUtils::hashStruct32(price1);
    uint32_t hash2 = HashUtils::hashStruct32(price2);
    REQUIRE(hash1 != hash2);
  }
}

TEST_CASE("MurmurHash3 collision resistance", "[hash]") {
  SECTION("Trading symbols have no collisions") {
    std::unordered_set<uint32_t> hashes;
    std::vector<std::string> symbols = {
        "AAPL", "GOOGL", "MSFT", "AMZN", "META", "TSLA", "NVDA", "JPM",
        "V",    "WMT",   "JNJ",  "PG",   "MA",   "HD",   "DIS",  "BAC",
        "GS",   "AXP",   "CAT",  "CVX",  "KO",   "DOW",  "IBM",  "INTC"};

    for (const auto &symbol : symbols) {
      uint32_t hash = HashUtils::hashStr32(symbol);
      hashes.insert(hash);
    }

    REQUIRE(hashes.size() == symbols.size());
  }

  SECTION("Sequential order IDs have low collision rate") {
    std::unordered_set<uint32_t> hashes;

    for (int i = 0; i < 10000; ++i) {
      std::string key = "ORDER_" + std::to_string(i);
      uint32_t hash = HashUtils::hashStr32(key);
      hashes.insert(hash);
    }

    double collision_rate = 1.0 - (double)hashes.size() / 10000.0;
    REQUIRE(collision_rate < 0.01); // Less than 1% collision rate
  }
}

TEST_CASE_METHOD(HashPerfTestFixture, "MurmurHash3 latency - symbol hashing",
                 "[hash][latency][!benchmark]") {

  BENCHMARK_ADVANCED("Single symbol hash (hot path)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t result;
    meter.measure([&] {
      result = HashUtils::hashStr32("AAPL");
      return result;
    });
  };

  BENCHMARK_ADVANCED("Symbol hash with string_view")(
      Catch::Benchmark::Chronometer meter) {
    std::string_view sv = "AAPL";
    volatile uint32_t result;
    meter.measure([&] {
      result = HashUtils::hashStr32(sv);
      return result;
    });
  };

  BENCHMARK_ADVANCED("Symbol hash batch (8 symbols)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t sum;
    meter.measure([&] {
      sum = 0;
      for (const auto &symbol : symbols) {
        sum += HashUtils::hashStr32(symbol);
      }
      return sum;
    });
  };
}

TEST_CASE_METHOD(HashPerfTestFixture, "MurmurHash3 latency - order key hashing",
                 "[hash][latency][!benchmark]") {

  BENCHMARK_ADVANCED("Short order key (20 bytes)")(
      Catch::Benchmark::Chronometer meter) {
    const char *key = "ORDER_12345_BUY_100";
    const size_t len = std::strlen(key);
    volatile uint32_t result;

    meter.measure([&] {
      result = HashUtils::hashCStr32(key, len);
      return result;
    });
  };

  BENCHMARK_ADVANCED("Medium order key (40 bytes)")(
      Catch::Benchmark::Chronometer meter) {
    const char *key = "ORDER_12345_AAPL_BUY_100_LIMIT_150.50";
    const size_t len = std::strlen(key);
    volatile uint32_t result;

    meter.measure([&] {
      result = HashUtils::hashCStr32(key, len);
      return result;
    });
  };

  BENCHMARK_ADVANCED("Sequential order key hashing (1000 keys)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t sum;
    meter.measure([&] {
      sum = 0;
      for (int i = 0; i < 1000; ++i) {
        sum += HashUtils::hashStr32(orderKeys[i]);
      }
      return sum;
    });
  };
}

TEST_CASE_METHOD(HashPerfTestFixture, "MurmurHash3 latency - struct hashing",
                 "[hash][latency][!benchmark]") {

  struct OrderID {
    uint64_t timestamp;
    uint32_t counter;
    uint16_t venue;
    uint8_t side;
    uint8_t padding;
  } __attribute__((packed));

  struct MarketData {
    uint32_t symbol_id;
    int64_t price;
    uint64_t quantity;
    uint64_t timestamp;
  } __attribute__((packed));

  OrderID order = {1234567890ULL, 1, 100, 1, 0};
  MarketData md = {42, 15050, 1000, 1234567890ULL};

  BENCHMARK_ADVANCED("Small struct hash (OrderID, 16 bytes)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t result;
    meter.measure([&] {
      result = HashUtils::hashStruct32(order);
      return result;
    });
  };

  BENCHMARK_ADVANCED("Medium struct hash (MarketData, 28 bytes)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t result;
    meter.measure([&] {
      result = HashUtils::hashStruct32(md);
      return result;
    });
  };

  BENCHMARK_ADVANCED("Struct batch hashing (1000 orders)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t sum;
    meter.measure([&] {
      sum = 0;
      for (uint32_t i = 0; i < 1000; ++i) {
        order.counter = i;
        sum += HashUtils::hashStruct32(order);
      }
      return sum;
    });
  };
}

TEST_CASE_METHOD(HashPerfTestFixture,
                 "MurmurHash3 latency - binary data hashing",
                 "[hash][latency][!benchmark]") {

  BENCHMARK_ADVANCED("Cache-line sized hash (64 bytes)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t result;
    meter.measure([&] {
      result = HashUtils::hash32(binaryData.data(), 64);
      return result;
    });
  };

  BENCHMARK_ADVANCED("Page-sized hash (4KB)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t result;
    meter.measure([&] {
      result = HashUtils::hash32(binaryData.data(), 4096);
      return result;
    });
  };

  BENCHMARK_ADVANCED("Large buffer hash (64KB)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t result;
    meter.measure([&] {
      result = HashUtils::hash32(binaryData.data(), 65536);
      return result;
    });
  };

  BENCHMARK_ADVANCED("Sequential 1KB block hashing (1000 blocks)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t sum;
    meter.measure([&] {
      sum = 0;
      for (size_t i = 0; i < 1000; ++i) {
        size_t offset = (i * 1024) % binaryData.size();
        sum += HashUtils::hash32(binaryData.data() + offset, 1024);
      }
      return sum;
    });
  };

  BENCHMARK_ADVANCED("Random access hashing (1000 blocks)")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t sum;
    std::size_t seed = 12345;

    meter.measure([&] {
      sum = 0;
      for (int i = 0; i < 1000; ++i) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        size_t offset = seed % (binaryData.size() - 1024);
        sum += HashUtils::hash32(binaryData.data() + offset, 1024);
      }
      return sum;
    });
  };
}

TEST_CASE_METHOD(HashPerfTestFixture, "MurmurHash3 cache effects",
                 "[hash][latency][cache][!benchmark]") {

  // Prepare data that spans multiple cache lines
  std::vector<std::string> keys;
  for (int i = 0; i < 1000; ++i) {
    keys.push_back("KEY_" + std::to_string(i) +
                   "_PADDING_FOR_CACHE_LINE_TESTING");
  }

  BENCHMARK_ADVANCED("Cache-hot sequential access")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t sum;
    // Warm up cache
    for (int i = 0; i < 100; ++i) {
      HashUtils::hashStr32(keys[i]);
    }

    meter.measure([&] {
      sum = 0;
      for (int i = 0; i < 100; ++i) {
        sum += HashUtils::hashStr32(keys[i]);
      }
      return sum;
    });
  };

  BENCHMARK_ADVANCED("Cache-cold random access")(
      Catch::Benchmark::Chronometer meter) {
    volatile uint32_t sum;
    std::size_t seed = 12345;

    meter.measure([&] {
      sum = 0;
      for (int i = 0; i < 100; ++i) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        sum += HashUtils::hashStr32(keys[seed % keys.size()]);
      }
      return sum;
    });
  };
}

TEST_CASE("MurmurHash3 distribution quality", "[hash]") {
  SECTION("Distribution across hash table buckets") {
    const size_t bucket_count = 1024;
    std::vector<size_t> buckets(bucket_count, 0);

    for (int i = 0; i < 10000; ++i) {
      std::string key = "ORDER_" + std::to_string(i);
      uint32_t hash = HashUtils::hashStr32(key);
      buckets[hash % bucket_count]++;
    }

    size_t empty_buckets = 0;
    for (size_t count : buckets) {
      if (count == 0)
        empty_buckets++;
    }

    REQUIRE(empty_buckets < bucket_count / 10);

    size_t max_bucket = *std::max_element(buckets.begin(), buckets.end());
    double avg = 10000.0 / bucket_count;
    REQUIRE(max_bucket < avg * 3);
  }
}
