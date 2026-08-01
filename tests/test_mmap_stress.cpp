#include "MmapBuffer.h"
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <thread>
#include <vector>

using namespace revobase::os;
namespace fs = std::filesystem;

class StressTestFixture {
public:
  StressTestFixture() : test_dir("./stress_test_data") {
    fs::create_directories(test_dir);
  }

  ~StressTestFixture() {
    if (fs::exists(test_dir)) {
      fs::remove_all(test_dir);
    }
  }

  std::string getTestPath(const std::string &name) {
    return test_dir + "/" + name;
  }

private:
  std::string test_dir;
};

TEST_CASE_METHOD(StressTestFixture, "MmapBuffer stress tests",
                 "[mmap][stress][.hide]") {

  SECTION("Multiple concurrent mappings") {
    const int num_mappings = 100;
    const std::size_t size = 4096;
    std::vector<std::uintptr_t> addresses;

    // Create many mappings
    for (int i = 0; i < num_mappings; i++) {
      std::string path = getTestPath("stress_" + std::to_string(i) + ".mmap");
      std::uintptr_t addr = MmapBuffer::loadMmapBuffer(path, size, true, false);
      addresses.push_back(addr);

      // Write unique pattern
      int *data = reinterpret_cast<int *>(addr);
      *data = i;
    }

    // Verify all mappings
    for (int i = 0; i < num_mappings; i++) {
      const int *data = reinterpret_cast<const int *>(addresses[i]);
      REQUIRE(*data == i);
    }

    // Release all
    for (int i = 0; i < num_mappings; i++) {
      REQUIRE(MmapBuffer::releaseMmapBuffer(addresses[i], size, false,
                                            MsyncMode::NONE));
    }
  }

  SECTION("Repeated map/unmap cycles") {
    const std::string path = getTestPath("cycle.mmap");
    const std::size_t size = 1024 * 1024; // 1MB

    for (int cycle = 0; cycle < 1000; cycle++) {
      std::uintptr_t addr = MmapBuffer::loadMmapBuffer(path, size, true, false);

      // Write and verify
      char *data = reinterpret_cast<char *>(addr);
      data[0] = static_cast<char>(cycle);
      REQUIRE(data[0] == static_cast<char>(cycle));

      REQUIRE(
          MmapBuffer::releaseMmapBuffer(addr, size, false, MsyncMode::NONE));
    }
  }

  SECTION("Multi-threaded access") {
    const std::string path = getTestPath("multithread.mmap");
    constexpr std::size_t size = 10 * 1024 * 1024; // 10MB

    std::uintptr_t addr = MmapBuffer::loadMmapBuffer(path, size, true, false);
    std::atomic<int> *counters = reinterpret_cast<std::atomic<int> *>(addr);

    constexpr int num_counters = size / sizeof(std::atomic<int>);
    constexpr int num_threads = 8;
    constexpr int increments_per_thread = 10000;

    // Initialize counters
    for (int i = 0; i < num_counters; i++) {
      counters[i].store(0);
    }

    // Spawn threads to increment counters
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
      threads.emplace_back([counters]() {
        for (int i = 0; i < increments_per_thread; i++) {
          int idx = i % num_counters;
          counters[idx].fetch_add(1, std::memory_order_relaxed);
        }
      });
    }

    // Wait for all threads
    for (auto &t : threads) {
      t.join();
    }

    // Verify total increments
    int total = 0;
    for (int i = 0; i < std::min(increments_per_thread, num_counters); i++) {
      total += counters[i].load();
    }

    REQUIRE(total == num_threads * increments_per_thread);

    MmapBuffer::releaseMmapBuffer(addr, size, false, MsyncMode::NONE);
  }
}

TEST_CASE_METHOD(StressTestFixture, "Page fault stress test",
                 "[mmap][pagefault][.hide]") {
  const std::size_t size = 100 * 1024 * 1024; // 100MB
  const std::string path = getTestPath("pagefault.mmap");

  SECTION("Touch every segment") {
    std::uintptr_t addr = MmapBuffer::loadMmapBuffer(path, size, true, false);
    char *data = reinterpret_cast<char *>(addr);

    // This should not cause page faults (already prefaulted)
    auto start = std::chrono::high_resolution_clock::now();

    for (std::size_t i = 0; i < size; i += 4096) {
      data[i] = static_cast<char>(i);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    INFO("Touched " << (size / 4096) << " segments in " << duration.count()
                    << "ms");

    // Should be fast (< 200ms for 100MB)
    REQUIRE(duration.count() < 200);

    MmapBuffer::releaseMmapBuffer(addr, size, false, MsyncMode::NONE);
  }
}
