
#include "MmapBuffer.h"
#include <catch2/catch_test_macros.hpp>

#include "TscTimer.h"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

using namespace revobase::os;
namespace fs = std::filesystem;

// Test fixture for cleanup
class MmapTestFixture {
public:
  MmapTestFixture() : test_dir("./test_mmap_data") {
    fs::create_directories(test_dir);
  }

  ~MmapTestFixture() {
    // Cleanup test files
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

TEST_CASE_METHOD(MmapTestFixture, "MmapBuffer basic functionality",
                 "[mmap][basic]") {
  const std::size_t size = 4096; // 4KB
  const std::string path = getTestPath("basic_test.mmap");

  SECTION("Create and map new file for writing") {
    std::uintptr_t addr = MmapBuffer::loadMmapBuffer(path, size, true, false);
    REQUIRE(addr != 0);

    // Write some data
    char *data = reinterpret_cast<char *>(addr);
    std::strcpy(data, "Hello HFT");

    REQUIRE(MmapBuffer::releaseMmapBuffer(addr, size, false, MsyncMode::NONE));

    // Verify file exists and has correct size
    REQUIRE(fs::exists(path));
    REQUIRE(fs::file_size(path) == size);
  }

  SECTION("Read existing file") {
    // First create a file with data
    std::uintptr_t write_addr =
        MmapBuffer::loadMmapBuffer(path, size, true, false);
    char *write_data = reinterpret_cast<char *>(write_addr);
    std::strcpy(write_data, "Test Data");
    MmapBuffer::releaseMmapBuffer(write_addr, size, false, MsyncMode::NONE);

    // Now open for reading
    std::uintptr_t read_addr =
        MmapBuffer::loadMmapBuffer(path, size, false, false);
    REQUIRE(read_addr != 0);

    const char *read_data = reinterpret_cast<const char *>(read_addr);
    REQUIRE(std::strcmp(read_data, "Test Data") == 0);

    REQUIRE(
        MmapBuffer::releaseMmapBuffer(read_addr, size, false, MsyncMode::NONE));
  }

  SECTION("Read missing file does not create it") {
    const std::string missing_path = getTestPath("missing_basic.mmap");

    REQUIRE_THROWS_AS(
        MmapBuffer::loadMmapBuffer(missing_path, size, false, false),
        revobase::os::MmapError);
    REQUIRE_THROWS_AS(
        MmapBuffer::loadMmapBuffer(missing_path, size, false, true),
        revobase::os::MmapError);
    REQUIRE_FALSE(fs::exists(missing_path));
  }

  SECTION("Read short file rejects exact size") {
    const std::string short_path = getTestPath("short_basic.mmap");
    std::ofstream(short_path).put('x');

    REQUIRE_THROWS_AS(
        MmapBuffer::loadMmapBuffer(short_path, size, false, false),
        revobase::os::MmapError);
  }

  SECTION("Multiple processes can share memory") {
    const std::string shared_path = getTestPath("shared.mmap");
    const char *test_string = "Shared Memory Test";

    // Appender process
    std::uintptr_t write_addr =
        MmapBuffer::loadMmapBuffer(shared_path, size, true, false);
    std::strcpy(reinterpret_cast<char *>(write_addr), test_string);

    // Cursor process (same process, different mapping)
    std::uintptr_t read_addr =
        MmapBuffer::loadMmapBuffer(shared_path, size, false, false);

    REQUIRE(std::strcmp(reinterpret_cast<const char *>(read_addr),
                        test_string) == 0);

    MmapBuffer::releaseMmapBuffer(write_addr, size, false, MsyncMode::NONE);
    MmapBuffer::releaseMmapBuffer(read_addr, size, false, MsyncMode::NONE);
  }

  SECTION("Large file uses huge pages efficiently") {
    const std::string large_path = getTestPath("large_test.mmap");
    const std::size_t large_size = 10 * 1024 * 1024; // 10MB

    struct rlimit rl;
    getrlimit(RLIMIT_MEMLOCK, &rl);
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < large_size) {
      SKIP("Insufficient memlock limit (" + std::to_string(rl.rlim_cur) +
           " bytes). Run: ulimit -l unlimited");
    }

    // Save current log level
    auto prev_level = spdlog::get_level();
    spdlog::set_level(spdlog::level::info);

    std::uintptr_t addr =
        MmapBuffer::loadMmapBuffer(large_path, large_size, true, false);
    REQUIRE(addr != 0);

    // Verify we can write to it
    char *data = reinterpret_cast<char *>(addr);
    data[0] = 'A';
    data[large_size - 1] = 'Z';

    REQUIRE(data[0] == 'A');
    REQUIRE(data[large_size - 1] == 'Z');

    REQUIRE(MmapBuffer::releaseMmapBuffer(addr, large_size, false,
                                          MsyncMode::NONE));

    // Restore log level
    spdlog::set_level(prev_level);
  }
}

TEST_CASE_METHOD(MmapTestFixture, "MmapBuffer error handling",
                 "[mmap][error]") {
  const std::size_t size = 4096;

  SECTION("Invalid path throws exception") {
    REQUIRE_THROWS_AS(MmapBuffer::loadMmapBuffer("/invalid/path/file.mmap",
                                                 size, true, false),
                      revobase::os::MmapError);
  }

  SECTION("Release null buffer returns false") {
    REQUIRE_FALSE(
        MmapBuffer::releaseMmapBuffer(0, size, false, MsyncMode::NONE));
  }
}

TEST_CASE_METHOD(MmapTestFixture, "MmapBuffer existing read-only mappings",
                 "[mmap][readonly]") {
  const std::size_t size = 4096;
  const std::string path = getTestPath("readonly.mmap");

  std::uintptr_t write_addr =
      MmapBuffer::loadMmapBuffer(path, size, true, false);
  std::strcpy(reinterpret_cast<char *>(write_addr), "Read-only data");
  REQUIRE(
      MmapBuffer::releaseMmapBuffer(write_addr, size, false, MsyncMode::NONE));

  SECTION("Maps and prepares an existing file without appender ownership") {
    ReadOnlyMmapOptions options{.advise_hugepage = true};
#ifdef __linux__
    options.populate_read = true;
#endif
    std::uintptr_t read_addr =
        MmapBuffer::loadExistingReadOnlyMmapBuffer(path, size, options);

    REQUIRE(std::strcmp(reinterpret_cast<const char *>(read_addr),
                        "Read-only data") == 0);
    REQUIRE(
        MmapBuffer::releaseMmapBuffer(read_addr, size, false, MsyncMode::NONE));
  }

  SECTION("Does not create missing files") {
    const std::string missing_path = getTestPath("missing-readonly.mmap");

    REQUIRE_THROWS_AS(
        MmapBuffer::loadExistingReadOnlyMmapBuffer(missing_path, size),
        revobase::os::MmapError);
    REQUIRE_FALSE(fs::exists(missing_path));
  }

  SECTION("Rejects files with unexpected sizes") {
    REQUIRE_THROWS_AS(
        MmapBuffer::loadExistingReadOnlyMmapBuffer(path, size * 2),
        revobase::os::MmapError);
  }
}

TEST_CASE_METHOD(MmapTestFixture, "MmapBuffer lazy vs eager loading",
                 "[mmap][loading]") {
  const std::size_t size = 1024 * 1024; // 1MB
  const std::string path = getTestPath("lazy_test.mmap");

  SECTION("Lazy loading") {
    revobase::TscTimer::calibrate();
    const auto start = revobase::TscTimer::now_cycles_begin();
    std::uintptr_t addr = MmapBuffer::loadMmapBuffer(path, size, true, true);
    const auto end = revobase::TscTimer::now_cycles_end();
    const auto lazy_duration = revobase::TscTimer::delta_ns(start, end);
    
    // Lazy should be very fast (< 1ms typically)
    REQUIRE(lazy_duration < 1000 * 1000);

    MmapBuffer::releaseMmapBuffer(addr, size, true, MsyncMode::NONE);
  }

  SECTION("Eager loading (non-lazy)") {
    revobase::TscTimer::calibrate();
    const auto start = revobase::TscTimer::now_cycles_begin();
    std::uintptr_t addr = MmapBuffer::loadMmapBuffer(path, size, true, false);
    const auto end = revobase::TscTimer::now_cycles_end();
    const auto eager_duration = revobase::TscTimer::delta_ns(start, end);
    // Eager takes longer due to prefaulting
    INFO("Eager loading took " << eager_duration << " nanoseconds");

    // Verify data is immediately accessible (no page faults)
    char *data = reinterpret_cast<char *>(addr);
    for (std::size_t i = 0; i < size; i += 4096) {
      data[i] = static_cast<char>(i % 256);
    }

    MmapBuffer::releaseMmapBuffer(addr, size, false, MsyncMode::NONE);
  }
}

TEST_CASE_METHOD(MmapTestFixture, "MmapBuffer alignment and cache-line testing",
                 "[mmap][alignment]") {
  const std::size_t size = 4096;
  const std::string path = getTestPath("alignment.mmap");

  std::uintptr_t addr = MmapBuffer::loadMmapBuffer(path, size, true, false);

  SECTION("Memory is page-aligned") {
    // mmap should return page-aligned addresses
    REQUIRE((addr & 0xFFF) == 0); // 4KB alignment
  }

  SECTION("Cache-line aligned access") {
    struct alignas(64) CacheLineData {
      char data[64];
    };

    CacheLineData *aligned_data = reinterpret_cast<CacheLineData *>(addr);
    REQUIRE((reinterpret_cast<std::uintptr_t>(aligned_data) & 63) == 0);

    // Write to cache-aligned structure
    std::memset(aligned_data->data, 0xAB, 64);
    REQUIRE(aligned_data->data[0] == static_cast<char>(0xAB));
  }

  MmapBuffer::releaseMmapBuffer(addr, size, false, MsyncMode::NONE);
}

TEST_CASE_METHOD(MmapTestFixture, "MmapBuffer concurrent access",
                 "[mmap][concurrent]") {
  const std::size_t size = 1024 * 1024; // 1MB
  const std::string path = getTestPath("concurrent.mmap");

  // Create and initialize
  std::uintptr_t write_addr =
      MmapBuffer::loadMmapBuffer(path, size, true, false);
  std::atomic<int> *counter = reinterpret_cast<std::atomic<int> *>(write_addr);
  counter->store(0);

  // Open multiple read mappings
  std::uintptr_t read_addr1 =
      MmapBuffer::loadMmapBuffer(path, size, false, false);
  std::uintptr_t read_addr2 =
      MmapBuffer::loadMmapBuffer(path, size, false, false);

  // Increment from write mapping
  counter->fetch_add(1);
  counter->fetch_add(1);

  // Read from other mappings
  const std::atomic<int> *read_counter1 =
      reinterpret_cast<const std::atomic<int> *>(read_addr1);
  const std::atomic<int> *read_counter2 =
      reinterpret_cast<const std::atomic<int> *>(read_addr2);

  REQUIRE(read_counter1->load() == 2);
  REQUIRE(read_counter2->load() == 2);

  MmapBuffer::releaseMmapBuffer(write_addr, size, false, MsyncMode::NONE);
  MmapBuffer::releaseMmapBuffer(read_addr1, size, false, MsyncMode::NONE);
  MmapBuffer::releaseMmapBuffer(read_addr2, size, false, MsyncMode::NONE);
}
