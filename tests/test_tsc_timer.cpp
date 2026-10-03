#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sched.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

#include <revobase/CpuAffinity.h>
#include <revobase/TscTimer.h>

using namespace revobase;

using revobase::system::CpuAffinity;

bool pin_to_core(int core_id) { return CpuAffinity::pinToCore(core_id); }

#ifdef __linux__
// The affinity mask as it was before any test ran.
//
// Captured at static-init time on purpose. Several tests below pin the MAIN
// thread (pin_to_core(0)) and never restore it, and Catch2 runs every test on
// that same thread - so by the time a later test asks sched_getaffinity() what
// it may use, the honest answer is "core 0", regardless of the machine.
// Reading the mask here, before main(), makes this independent of test order.
const cpu_set_t g_initial_affinity = [] {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
    // Unknown: fall back to "every core the hardware reports".
    for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i) {
      CPU_SET(i, &mask);
    }
  }
  return mask;
}();

// Undo a main-thread pin leaked by an earlier test.
void restore_initial_affinity() {
  sched_setaffinity(0, sizeof(g_initial_affinity), &g_initial_affinity);
}
#else
void restore_initial_affinity() {}
#endif

// Restores the calling thread's affinity on scope exit. Pinning a worker
// thread is self-cleaning because the thread dies, but Catch2 runs every test
// case on the same main thread - so a bare pin_to_core(0) at test scope leaves
// every later test in the binary confined to one core.
struct AffinityGuard {
  AffinityGuard() = default;
  AffinityGuard(const AffinityGuard &) = delete;
  AffinityGuard &operator=(const AffinityGuard &) = delete;
  ~AffinityGuard() { restore_initial_affinity(); }
};

// Cores this process is actually allowed to run on. taskset, a cgroup cpuset,
// or an offline CPU can all make hardware_concurrency() an overstatement, and
// pinning to a core outside the mask silently leaves the thread wherever it
// was - which would quietly turn a "one sample per core" measurement into
// several samples from the same core.
std::vector<int> usable_cores() {
  std::vector<int> cores;
  const int n = static_cast<int>(std::thread::hardware_concurrency());
#ifdef __linux__
  for (int i = 0; i < n; ++i) {
    if (CPU_ISSET(i, &g_initial_affinity)) {
      cores.push_back(i);
    }
  }
#else
  for (int i = 0; i < n; ++i) {
    cores.push_back(i);
  }
#endif
  return cores;
}

TEST_CASE("TscTimer::calibrate - validates system and initializes",
          "[tsc][calibrate]") {
  SECTION("calibrate() succeeds on suitable systems") {
    REQUIRE_NOTHROW(TscTimer::calibrate());
  }

  SECTION("is_suitable_for_cross_core_timing() returns true on valid systems") {
    // This must be true for calibrate() to succeed
    REQUIRE(TscTimer::is_suitable_for_cross_core_timing());
  }

  SECTION("calibration produces reasonable values") {
    TscTimer::calibrate();

    // Test that calibration actually worked
    uint64_t start = TscTimer::now_cycles_begin();
    std::this_thread::sleep_for(std::chrono::microseconds(1000)); // 1ms
    uint64_t end = TscTimer::now_cycles_end();

    double delta_us = TscTimer::delta_us(start, end);

    INFO("1ms sleep measured as: " << delta_us << " us");
    INFO("Cycle difference: " << (end - start));

    // Should measure roughly 1000us (allow wide tolerance for CI)
    REQUIRE(delta_us > 500);  // At least 0.5ms
    REQUIRE(delta_us < 5000); // Less than 5ms
    REQUIRE(std::isfinite(delta_us));
    REQUIRE(delta_us > 0);
  }
}

TEST_CASE(
    "TscTimer::now_cycles_begin/end - captures timestamps with serialization",
    "[tsc][now_cycles]") {
  TscTimer::calibrate();

  SECTION("now_cycles_end > now_cycles_begin after delay") {
    uint64_t start = TscTimer::now_cycles_begin();
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    uint64_t end = TscTimer::now_cycles_end();

    REQUIRE(end > start);
  }

  SECTION("repeated calls are monotonic") {
    std::vector<uint64_t> timestamps;
    for (int i = 0; i < 100; ++i) {
      timestamps.push_back(TscTimer::now_cycles_begin());
    }

    // All timestamps should be non-decreasing
    for (size_t i = 1; i < timestamps.size(); ++i) {
      REQUIRE(timestamps[i] >= timestamps[i - 1]);
    }
  }

  SECTION("begin/end pair captures non-zero interval") {
    uint64_t start = TscTimer::now_cycles_begin();
    volatile int work = 0;
    for (int i = 0; i < 100; ++i)
      work += i; // Some work
    uint64_t end = TscTimer::now_cycles_end();

    REQUIRE(end > start);
    double delta_ns = TscTimer::delta_ns(start, end);
    REQUIRE(delta_ns > 0);
  }
}

TEST_CASE("TscTimer::delta_ns - converts cycle difference to nanoseconds",
          "[tsc][delta_ns]") {
  TscTimer::calibrate();

  SECTION("measures ~100us sleep with reasonable accuracy") {
    uint64_t start = TscTimer::now_cycles_begin();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    uint64_t end = TscTimer::now_cycles_end();

    double elapsed_ns = TscTimer::delta_ns(start, end);
    double elapsed_us = elapsed_ns / 1000.0;

    INFO("Measured: " << elapsed_us << " us (expected ~100us)");
    REQUIRE(elapsed_us >= 80.0);  // At least 80us (OS scheduling variance)
    REQUIRE(elapsed_us <= 500.0); // Less than 500us (generous upper bound)
  }

  SECTION("delta is always non-negative for end >= start") {
    uint64_t start = TscTimer::now_cycles_begin();
    std::this_thread::sleep_for(std::chrono::microseconds(1));
    uint64_t end = TscTimer::now_cycles_end();

    double delta = TscTimer::delta_ns(start, end);
    REQUIRE(delta >= 0.0);
  }

  SECTION("zero delta when start == end") {
    uint64_t timestamp = TscTimer::now_cycles_begin();
    double delta = TscTimer::delta_ns(timestamp, timestamp);
    REQUIRE(delta == 0.0);
  }
}

TEST_CASE("TscTimer::delta_us - converts cycle difference to microseconds",
          "[tsc][delta_us]") {
  TscTimer::calibrate();

  SECTION("measures ~1ms sleep with reasonable accuracy") {
    uint64_t start = TscTimer::now_cycles_begin();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    uint64_t end = TscTimer::now_cycles_end();

    double elapsed_us = TscTimer::delta_us(start, end);

    INFO("Measured: " << elapsed_us << " us (expected ~1000us)");
    REQUIRE(elapsed_us >= 900.0);  // At least 0.9ms
    REQUIRE(elapsed_us <= 2000.0); // Less than 2ms
  }

  SECTION("delta_us = delta_ns / 1000") {
    uint64_t start = TscTimer::now_cycles_begin();
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    uint64_t end = TscTimer::now_cycles_end();

    double ns = TscTimer::delta_ns(start, end);
    double us = TscTimer::delta_us(start, end);

    REQUIRE_THAT(us * 1000.0, Catch::Matchers::WithinRel(ns, 0.001));
  }
}

TEST_CASE("TscTimer::ns_to_cycles - fractional inverse of delta_ns",
          "[tsc][ns_to_cycles]") {
  TscTimer::calibrate();

  SECTION("zero nanoseconds maps to zero cycles") {
    REQUIRE(TscTimer::ns_to_cycles(0.0) == 0.0);
  }

  SECTION("round-trips through delta_ns to within a cycle") {
    // ns -> cycles -> ns must recover the input; the only loss is rounding the
    // cycle count to an integer, i.e. sub-nanosecond on a GHz-class TSC.
    for (double ns : {100.0, 1'000.0, 1'000'000.0, 1'000'000'000.0}) {
      const double cycles = TscTimer::ns_to_cycles(ns);
      REQUIRE(cycles > 0.0);
      const double back =
          TscTimer::delta_ns(0, static_cast<uint64_t>(std::llround(cycles)));
      INFO("ns=" << ns << " cycles=" << cycles << " back=" << back);
      REQUIRE_THAT(back, Catch::Matchers::WithinAbs(ns, 2.0));
    }
  }

  SECTION("is linear in the input") {
    const double base = TscTimer::ns_to_cycles(1'000.0);
    REQUIRE(base > 0.0);
    REQUIRE_THAT(TscTimer::ns_to_cycles(2'000.0),
                 Catch::Matchers::WithinRel(2.0 * base, 1e-9));
    REQUIRE_THAT(TscTimer::ns_to_cycles(10'000.0),
                 Catch::Matchers::WithinRel(10.0 * base, 1e-9));
  }

  SECTION("per-interval accumulation stays aligned to a one-shot span") {
    // advancing by the per-interval cycle count (carrying the sub-cycle
    // fraction) each step must not drift from the exact elapsed-span
    // conversion, regardless of step count.
    const double interval_cycles = TscTimer::ns_to_cycles(1'000.0);
    const int64_t steps = 2'000'000;
    uint64_t acc = 0;
    double frac = 0.0;
    for (int64_t i = 0; i < steps; ++i) {
      frac += interval_cycles;
      const double whole = std::floor(frac);
      acc += static_cast<uint64_t>(whole);
      frac -= whole;
    }
    const double one_shot =
        TscTimer::ns_to_cycles(static_cast<double>(steps) * 1'000.0);
    INFO("accumulated=" << acc << " one_shot=" << one_shot);
    REQUIRE(std::fabs(static_cast<double>(acc) - one_shot) <= 2.0);
  }
}

TEST_CASE("TscTimer - cross-core timestamp consistency", "[tsc][cross-core]") {
  TscTimer::calibrate();

  // Earlier test cases pin the main thread to core 0 and leave it there. Undo
  // that, or this test spends its life gating N spinning workers from a
  // single core.
  restore_initial_affinity();

  // Only cores this process may actually run on. Deliberately NOT filtered to
  // exclude isolcpus/nohz_full cores: those are exactly the cores the hot
  // paths run on, so dropping them would skip coverage where it matters most.
  // Their scheduling stalls are handled by taking the best of several samples
  // below, which separates a transient stall from a real clock offset.
  const std::vector<int> cores = usable_cores();
  const int num_cores = static_cast<int>(cores.size());
  INFO("usable cores: " << num_cores << " of "
                        << std::thread::hardware_concurrency());
  if (num_cores < 2) {
    SKIP("fewer than 2 usable cores - cannot measure cross-core consistency");
  }

  // A "diagnose TSC synchronization" section used to sit here. It spawned one
  // thread per core, slept 1ms in each, then called a large raw-cycle spread an
  // offset problem. The threads were never synchronized, so the spread it
  // measured was thread-creation stagger and wakeup jitter, not TSC offset -
  // and the 10^10-cycle threshold it compared against is ~3.3s at 3GHz, so it
  // could never fire anyway. It asserted nothing. The section below does the
  // same measurement correctly, by releasing every thread from a spin on a
  // shared flag, and actually checks the result.

  SECTION("simultaneous timestamps across cores are close") {
    // Skip this test if TSC has known offset issues
    if (TscTimer::has_cross_core_offset_issue()) {
      SKIP("TSC has cross-core offset issues - skipping cross-core consistency "
           "test");
    }

    // One synchronized sample: every thread pins, announces arrival, spins on
    // a shared flag, and stamps the moment it flips.
    //
    // The arrival counter is load-bearing. Spinning on the flag alone
    // synchronizes when each thread READS the TSC but not that every thread
    // has REACHED the spin before the flag flips - a thread still being
    // created finds the flag already true and stamps whenever it eventually
    // runs, turning the "spread" into thread-startup latency.
    //
    // No yield() in the spin: once the flag is set, a yield is pure added
    // latency inside the window being measured.
    auto sample = [&]() -> std::vector<uint64_t> {
      std::atomic<bool> start_flag{false};
      std::atomic<int> ready{0};
      std::vector<uint64_t> timestamps(num_cores);
      std::vector<char> landed(num_cores, 0);
      std::vector<std::thread> threads;

      for (int i = 0; i < num_cores; ++i) {
        threads.emplace_back([&, slot = i, core_id = cores[i]]() {
          landed[slot] = pin_to_core(core_id) ? 1 : 0;
          ready.fetch_add(1, std::memory_order_release);
          while (!start_flag.load(std::memory_order_acquire)) {
          }
          timestamps[slot] = TscTimer::now_cycles_begin();
        });
      }

      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(10);
      bool timed_out = false;
      while (ready.load(std::memory_order_acquire) < num_cores) {
        if (std::chrono::steady_clock::now() > deadline) {
          timed_out = true;
          break;
        }
        std::this_thread::yield();
      }
      start_flag.store(true, std::memory_order_release);
      for (auto &t : threads) {
        t.join();
      }
      if (timed_out) {
        FAIL("timed out waiting for all "
             << num_cores << " threads to reach the start gate");
      }
      // A thread that did not land where it was told is not a sample from
      // that core, so this is not the per-core measurement it looks like.
      for (int i = 0; i < num_cores; ++i) {
        INFO("core " << cores[i] << " pin landed: " << (int)landed[i]);
        REQUIRE(landed[i] == 1);
      }
      return timestamps;
    };

    // Take the best of several samples. A genuine cross-core TSC offset is a
    // fixed property of the hardware and shows up in every sample; a thread
    // that gets stalled after the flag flips does not. Without this the test
    // measures the host's scheduling behaviour rather than the TSC: on this
    // box a thread pinned to an isolcpus+nohz_full core is intermittently
    // ~950ms LATE (not early), which is a tickless-core scheduling stall
    // wearing the costume of a clock offset.
    std::vector<uint64_t> timestamps;
    double spread_ns = 0.0;
    uint64_t min_ts = 0;
    uint64_t max_ts = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
      auto candidate = sample();
      const uint64_t lo = *std::min_element(candidate.begin(), candidate.end());
      const uint64_t hi = *std::max_element(candidate.begin(), candidate.end());
      const double s = TscTimer::delta_ns(lo, hi);
      if (attempt == 0 || s < spread_ns) {
        spread_ns = s;
        min_ts = lo;
        max_ts = hi;
        timestamps = std::move(candidate);
      }
      if (spread_ns < 1'000'000.0) { // 1ms - already clean, no need to retry
        break;
      }
    }

    INFO("Timestamp spread across " << num_cores << " cores: " << spread_ns
                                    << " ns");
    INFO("Min timestamp: " << min_ts);
    INFO("Max timestamp: " << max_ts);

    // Timestamps should be within 10ms of each other
    // (actual spread is usually <1us on properly synchronized systems)
    REQUIRE(spread_ns >= 0.0);
    REQUIRE(spread_ns < 10'000'000);

    // Verify no extreme outliers between any pair of cores
    // Check raw cycle differences first to detect overflow
    for (size_t i = 0; i < timestamps.size(); ++i) {
      for (size_t j = i + 1; j < timestamps.size(); ++j) {
        uint64_t ts_i = timestamps[i];
        uint64_t ts_j = timestamps[j];
        // Calculate absolute cycle difference
        uint64_t cycle_diff = (ts_i > ts_j) ? (ts_i - ts_j) : (ts_j - ts_i);
        INFO("Core " << i << " vs " << j << ": ts_i=" << ts_i
                     << ", ts_j=" << ts_j << ", cycle_diff=" << cycle_diff);
        // Convert to nanoseconds - use the smaller timestamp as start
        uint64_t start = std::min(ts_i, ts_j);
        uint64_t end = std::max(ts_i, ts_j);
        double delta = TscTimer::delta_ns(start, end);
        INFO("Delta between core " << i << " and " << j << ": " << delta
                                   << " ns (from " << cycle_diff << " cycles)");
        REQUIRE(delta >= 0.0);
        REQUIRE(delta < 10'000'000); // < 10ms
      }
    }
  }
}

TEST_CASE("TscTimer - cross-core message passing latency",
          "[tsc][cross-core][latency]") {
  TscTimer::calibrate();

  if (TscTimer::has_cross_core_offset_issue()) {
    SKIP("TSC has cross-core offset issues - skipping cross-core latency test");
  }

  const int core_sender = 0;
  const int core_receiver = std::thread::hardware_concurrency() > 1 ? 1 : 0;

  std::atomic<uint64_t> message_timestamp{0};
  std::atomic<bool> done{false};

  std::vector<double> latencies;
  const int iterations = 1000;

  SECTION("measure one-way cross-core latency") {
    std::thread receiver([&]() {
      pin_to_core(core_receiver);

      for (int i = 0; i < iterations; ++i) {
        // Wait for message
        uint64_t send_time = 0;
        while ((send_time = message_timestamp.exchange(
                    0, std::memory_order_acq_rel)) == 0) {
          if (done.load(std::memory_order_acquire))
            return;
          std::this_thread::yield();
        }

        uint64_t recv_time = TscTimer::now_cycles_end();
        double latency = TscTimer::delta_ns(send_time, recv_time);

        // Filter outliers (context switches, interrupts)
        if (latency > 0 && latency < 1'000'000) { // 0-1ms
          latencies.push_back(latency);
        }
      }
    });

    std::thread sender([&]() {
      pin_to_core(core_sender);
      std::this_thread::sleep_for(
          std::chrono::milliseconds(10)); // Let receiver start

      for (int i = 0; i < iterations; ++i) {
        // Send message with timestamp
        message_timestamp.store(TscTimer::now_cycles_begin(),
                                std::memory_order_release);

        // Wait for receiver to consume
        while (message_timestamp.load(std::memory_order_acquire) != 0) {
          std::this_thread::yield();
        }

        std::this_thread::sleep_for(std::chrono::microseconds(10)); // Pacing
      }

      done.store(true, std::memory_order_release);
    });

    sender.join();
    receiver.join();

    REQUIRE(latencies.size() > iterations / 2); // At least 50% valid samples

    std::sort(latencies.begin(), latencies.end());
    double min = latencies.front();
    double median = latencies[latencies.size() / 2];
    double p95 = latencies[static_cast<size_t>(latencies.size() * 0.95)];
    double p99 = latencies[static_cast<size_t>(latencies.size() * 0.99)];
    double max = latencies.back();

    INFO("Cross-core latencies (ns):");
    INFO("  min=" << min << ", median=" << median);
    INFO("  p95=" << p95 << ", p99=" << p99 << ", max=" << max);

    // Sanity checks
    REQUIRE(min > 0);
    REQUIRE(min < 100'000);      // Min < 100us
    REQUIRE(median < 1'000'000); // Median < 1ms
    REQUIRE(p99 < 5'000'000);    // p99 < 5ms
  }
}

TEST_CASE("TscTimer - strict monotonicity (no backwards time)",
          "[tsc][monotonic]") {
  TscTimer::calibrate();

  const int num_samples = 10000;
  const int num_cores = std::min(4, (int)std::thread::hardware_concurrency());

  SECTION("single core - strictly increasing timestamps") {
    AffinityGuard restore_affinity;
    pin_to_core(0);

    uint64_t prev = TscTimer::now_cycles_begin();
    int backwards_count = 0;

    for (int i = 0; i < num_samples; ++i) {
      uint64_t curr = TscTimer::now_cycles_begin();
      if (curr < prev) {
        backwards_count++;
      }
      prev = curr;
    }

    INFO("Backwards time events on single core: " << backwards_count);
    REQUIRE(backwards_count == 0);
  }

  SECTION("multi-core - no backwards time per thread") {
    std::atomic<int> total_backwards{0};
    std::vector<std::thread> threads;

    for (int core = 0; core < num_cores; ++core) {
      threads.emplace_back([&, core]() {
        pin_to_core(core);

        uint64_t prev = TscTimer::now_cycles_begin();
        int local_backwards = 0;

        for (int i = 0; i < num_samples / num_cores; ++i) {
          uint64_t curr = TscTimer::now_cycles_begin();
          if (curr < prev) {
            local_backwards++;
          }
          prev = curr;
        }

        total_backwards.fetch_add(local_backwards, std::memory_order_relaxed);
      });
    }

    for (auto &t : threads) {
      t.join();
    }

    INFO("Backwards time events across "
         << num_cores << " cores: " << total_backwards.load());
    REQUIRE(total_backwards.load() == 0);
  }
}

TEST_CASE("TscTimer - nanosecond-level precision", "[tsc][precision]") {
  TscTimer::calibrate();
  AffinityGuard restore_affinity;
  pin_to_core(0);

  SECTION("can measure sub-microsecond intervals") {
    std::vector<double> deltas;

    for (int i = 0; i < 1000; ++i) {
      uint64_t start = TscTimer::now_cycles_begin();

      // Minimal work
      volatile int x = 0;
      for (int j = 0; j < 10; ++j) {
        x += j;
      }

      uint64_t end = TscTimer::now_cycles_end();

      double delta = TscTimer::delta_ns(start, end);
      if (delta > 0 && delta < 10'000) { // 0-10us range
        deltas.push_back(delta);
      }
    }

    REQUIRE(!deltas.empty());
    std::sort(deltas.begin(), deltas.end());

    double min = deltas.front();
    double median = deltas[deltas.size() / 2];

    INFO("Measured intervals (ns): min=" << min << ", median=" << median);

    // Should be able to measure sub-microsecond intervals
    REQUIRE(min < 1000);    // < 1us
    REQUIRE(median < 5000); // < 5us
  }

  SECTION("measurement overhead is minimal") {
    std::vector<double> overheads;

    for (int i = 0; i < 1000; ++i) {
      uint64_t start = TscTimer::now_cycles_begin();
      uint64_t end = TscTimer::now_cycles_end();

      double overhead = TscTimer::delta_ns(start, end);
      if (overhead > 0 && overhead < 1000) { // 0-1us
        overheads.push_back(overhead);
      }
    }

    REQUIRE(!overheads.empty());
    std::sort(overheads.begin(), overheads.end());

    double min_overhead = overheads.front();
    double median_overhead = overheads[overheads.size() / 2];

    INFO("Measurement overhead (ns): min=" << min_overhead
                                           << ", median=" << median_overhead);

    // Overhead should be very small
    REQUIRE(min_overhead < 500);     // < 500ns typical
    REQUIRE(median_overhead < 1000); // < 1us median
  }
}
