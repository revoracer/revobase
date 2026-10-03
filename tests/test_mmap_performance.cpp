#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <thread>

#include <revobase/MmapBuffer.h>

using namespace revobase::os;
namespace fs = std::filesystem;

class PerfTestFixture {
public:
  PerfTestFixture() : test_dir("./perf_test_data") {
    fs::create_directories(test_dir);
  }

  ~PerfTestFixture() {
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

TEST_CASE_METHOD(PerfTestFixture, "MmapBuffer performance benchmarks",
                 "[mmap][benchmark][!benchmark]") {
  const std::size_t small_size = 4096;              // 4KB
  const std::size_t medium_size = 1024 * 1024;      // 1MB
  const std::size_t large_size = 100 * 1024 * 1024; // 100MB

  BENCHMARK_ADVANCED("Map 4KB file (eager)")(
      Catch::Benchmark::Chronometer meter) {
    std::string path = getTestPath("bench_4k.mmap");
    meter.measure([&] {
      std::uintptr_t addr =
          MmapBuffer::loadMmapBuffer(path, small_size, true, false);
      MmapBuffer::releaseMmapBuffer(addr, small_size, false, MsyncMode::NONE);
      return addr;
    });
    fs::remove(path);
  };

  BENCHMARK_ADVANCED("Map 4KB file (lazy)")(
      Catch::Benchmark::Chronometer meter) {
    std::string path = getTestPath("bench_4k_lazy.mmap");
    meter.measure([&] {
      std::uintptr_t addr =
          MmapBuffer::loadMmapBuffer(path, small_size, true, true);
      MmapBuffer::releaseMmapBuffer(addr, small_size, true, MsyncMode::NONE);
      return addr;
    });
    fs::remove(path);
  };

  BENCHMARK_ADVANCED("Map 1MB file (eager)")(
      Catch::Benchmark::Chronometer meter) {
    std::string path = getTestPath("bench_1m.mmap");
    meter.measure([&] {
      std::uintptr_t addr =
          MmapBuffer::loadMmapBuffer(path, medium_size, true, false);
      MmapBuffer::releaseMmapBuffer(addr, medium_size, false, MsyncMode::NONE);
      return addr;
    });
    fs::remove(path);
  };

  BENCHMARK_ADVANCED("Map 100MB file (eager)")(
      Catch::Benchmark::Chronometer meter) {
    std::string path = getTestPath("bench_100m.mmap");
    meter.measure([&] {
      std::uintptr_t addr =
          MmapBuffer::loadMmapBuffer(path, large_size, true, false);
      MmapBuffer::releaseMmapBuffer(addr, large_size, false, MsyncMode::NONE);
      return addr;
    });
    fs::remove(path);
  };
}

TEST_CASE_METHOD(PerfTestFixture, "Memory access latency",
                 "[mmap][latency][!benchmark]") {
  const std::size_t size = 1024 * 1024; // 1MB
  const std::string path = getTestPath("latency.mmap");

  std::uintptr_t addr = MmapBuffer::loadMmapBuffer(path, size, true, false);
  volatile char *data = reinterpret_cast<volatile char *>(addr);

  BENCHMARK_ADVANCED("Sequential read (cache-hot)")(
      Catch::Benchmark::Chronometer meter) {
    meter.measure([&] {
      volatile char sum = 0;
      for (std::size_t i = 0; i < size; i += 64) { // Cache line stride
        sum += data[i];
      }
      return sum;
    });
  };

  BENCHMARK_ADVANCED("Random read (1000 accesses)")(
      Catch::Benchmark::Chronometer meter) {
    meter.measure([&] {
      volatile char sum = 0;
      std::size_t seed = 12345;
      for (int i = 0; i < 1000; i++) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        sum += data[seed % size];
      }
      return sum;
    });
  };

  BENCHMARK_ADVANCED("Sequential write")(Catch::Benchmark::Chronometer meter) {
    meter.measure([&] {
      for (std::size_t i = 0; i < size; i += 64) {
        data[i] = static_cast<char>(i);
      }
    });
  };

  MmapBuffer::releaseMmapBuffer(addr, size, false, MsyncMode::NONE);
}

TEST_CASE_METHOD(PerfTestFixture, "IPC throughput",
                 "[mmap][ipc][benchmark][!benchmark]") {
  const std::size_t buffer_size = 1024 * 1024; // 1MB ring buffer
  const std::string path = getTestPath("ipc.mmap");

  struct RingBuffer {
    std::atomic<uint64_t> write_pos{0};
    std::atomic<uint64_t> read_pos{0};
    char data[1024 * 1024 - 16]; // Leave room for atomics
  };

  std::uintptr_t addr =
      MmapBuffer::loadMmapBuffer(path, buffer_size, true, false);
  RingBuffer *ring = reinterpret_cast<RingBuffer *>(addr);

  BENCHMARK_ADVANCED("Single-producer single-consumer (1M messages)")(
      Catch::Benchmark::Chronometer meter) {
    meter.measure([&] {
      ring->write_pos.store(0);
      ring->read_pos.store(0);

      const int num_messages = 1000000;
      const std::size_t msg_size = 64;

      // Producer
      std::thread producer([ring]() {
        for (int i = 0; i < num_messages; i++) {
          std::size_t pos =
              ring->write_pos.fetch_add(msg_size, std::memory_order_release);
          pos %= (sizeof(ring->data) - msg_size);
          std::memcpy(ring->data + pos, &i, sizeof(i));
        }
      });

      // Consumer
      std::thread consumer([ring]() {
        int received = 0;
        while (received < num_messages) {
          uint64_t write = ring->write_pos.load(std::memory_order_acquire);
          uint64_t read = ring->read_pos.load(std::memory_order_relaxed);

          if (write - read >= msg_size) {
            std::size_t pos = read % (sizeof(ring->data) - msg_size);
            int value;
            std::memcpy(&value, ring->data + pos, sizeof(value));
            ring->read_pos.store(read + msg_size, std::memory_order_release);
            received++;
          }
        }
      });

      producer.join();
      consumer.join();
    });
  };

  MmapBuffer::releaseMmapBuffer(addr, buffer_size, false, MsyncMode::NONE);
}
