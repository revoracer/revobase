#include "RealtimePriority.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <utility>

using namespace revobase::system;


TEST_CASE("setRealtimePriority rejects out-of-range priorities",
          "[realtime]") {
  // Rejected before pthread_setschedparam is reached, so this holds for root
  // too. 0 matters most: it is the priority a SCHED_OTHER thread reports, so
  // it is what a naive save/restore round-trip would feed back in.
  REQUIRE_FALSE(RealtimePriority::setRealtimePriority(0));
  REQUIRE_FALSE(RealtimePriority::setRealtimePriority(-1));
  REQUIRE_FALSE(RealtimePriority::setRealtimePriority(100));
  REQUIRE_FALSE(RealtimePriority::setRealtimePriority(1000));
  REQUIRE_FALSE(
      RealtimePriority::setRealtimePriority(0, SchedulerPolicy::RR));
}

TEST_CASE("getPriorityRange matches the accepted 1-99 window", "[realtime]") {
  auto [fifo_min, fifo_max] = RealtimePriority::getPriorityRange(
      SchedulerPolicy::FIFO);
  REQUIRE(fifo_min >= 1);
  REQUIRE(fifo_max >= fifo_min);
  // The hardcoded 1-99 check in setRealtimePriority must not be narrower than
  // what the kernel actually accepts, or valid priorities become unreachable.
  REQUIRE(fifo_max <= 99);

  auto [rr_min, rr_max] = RealtimePriority::getPriorityRange(SchedulerPolicy::RR);
  REQUIRE(rr_min >= 1);
  REQUIRE(rr_max >= rr_min);

  // Non-RT policies have a single valid priority of 0.
  auto [other_min, other_max] =
      RealtimePriority::getPriorityRange(SchedulerPolicy::NORMAL);
  REQUIRE(other_min == 0);
  REQUIRE(other_max == 0);
}

TEST_CASE("getCurrentPriority and isRealtime agree", "[realtime]") {
  auto [policy, priority] = RealtimePriority::getCurrentPriority();
  const bool rt =
      policy == SchedulerPolicy::FIFO || policy == SchedulerPolicy::RR;
  REQUIRE(RealtimePriority::isRealtime() == rt);
  if (rt) {
    REQUIRE(priority >= 1);
  } else {
    REQUIRE(priority == 0);
  }
}

TEST_CASE("setNormalPriority leaves the thread non-realtime", "[realtime]") {
  REQUIRE(RealtimePriority::setNormalPriority());
  REQUIRE_FALSE(RealtimePriority::isRealtime());
  auto [policy, priority] = RealtimePriority::getCurrentPriority();
  REQUIRE(priority == 0);
  REQUIRE(policy != SchedulerPolicy::FIFO);
  REQUIRE(policy != SchedulerPolicy::RR);
}

namespace {

// A canary either side of the alloca descent: if prefaultStack ever walks past
// the guard page or scribbles outside its own buffer, the process dies or
// these change. Volatile so the writes and reads survive optimization.
void checkedPrefault(size_t bytes) {
  volatile unsigned long below = 0xC0FFEEUL;
  volatile unsigned long above = 0xDEADBEEFUL;
  RealtimePriority::prefaultStack(bytes);
  REQUIRE(below == 0xC0FFEEUL);
  REQUIRE(above == 0xDEADBEEFUL);
}

// Burn a known amount of stack so prefaultStack is called from a frame that is
// already deep - the case the headroom clamp exists for.
void recurseThenPrefault(int depth, size_t bytes) {
  volatile char frame[16 * 1024];
  frame[0] = static_cast<char>(depth);
  frame[sizeof(frame) - 1] = static_cast<char>(depth);
  if (depth > 0) {
    recurseThenPrefault(depth - 1, bytes);
    return;
  }
  checkedPrefault(bytes);
}

} // namespace

TEST_CASE("prefaultStack survives its edge cases", "[realtime]") {
  SECTION("zero bytes is a no-op") { checkedPrefault(0); }

  SECTION("default depth") { checkedPrefault(1024 * 1024); }

  SECTION("sub-page request") { checkedPrefault(64); }

  SECTION("absurd request is clamped, not fatal") {
    // 1 GB is far past any thread stack. The headroom clamp has to cut this
    // down; without it the alloca runs off the end of the stack and there is
    // no way to catch that after the fact.
    checkedPrefault(1024UL * 1024 * 1024);
  }

  SECTION("repeated calls") {
    for (int i = 0; i < 8; ++i)
      checkedPrefault(256 * 1024);
  }

  SECTION("from an already-deep frame") {
    // ~1 MB of frames burned before the call, so the clamp is computed against
    // meaningfully reduced headroom.
    recurseThenPrefault(64, 8 * 1024 * 1024);
  }
}

TEST_CASE("prefaultStack works on a non-main thread", "[realtime]") {
  // Worth its own case: the main thread's stack is grow-on-demand, while a
  // std::thread gets a fixed mapping, and pthread_getattr_np reports them
  // differently.
  bool ok = false;
  std::thread t([&ok] {
    checkedPrefault(1024 * 1024);
    checkedPrefault(512UL * 1024 * 1024); // larger than the thread's stack
    ok = true;
  });
  t.join();
  REQUIRE(ok);
}

TEST_CASE("boostKsoftirqd handles the no-op and not-found paths",
          "[realtime]") {
  // cpu < 0 means "not requested" and must succeed without scanning /proc.
  REQUIRE(RealtimePriority::boostKsoftirqd(-1, 50));
  REQUIRE(RealtimePriority::boostKsoftirqd(-1, 0));

  // No such CPU, so the /proc walk runs to completion and finds nothing. This
  // is the case that exercises the readdir/fopen loop against a live /proc,
  // including PIDs that vanish mid-scan.
  REQUIRE_FALSE(RealtimePriority::boostKsoftirqd(1 << 20, 50));
}

TEST_CASE("ScopedRealtimePriority restores the entry policy", "[realtime]") {
  RealtimePriority::setNormalPriority();
  const auto before = RealtimePriority::getCurrentPriority();

  {
    ScopedRealtimePriority scoped(50);
    if (scoped.isEnabled()) {
      // Only reachable with CAP_SYS_NICE.
      REQUIRE(RealtimePriority::isRealtime());
      auto [policy, priority] = RealtimePriority::getCurrentPriority();
      REQUIRE(policy == SchedulerPolicy::FIFO);
      REQUIRE(priority == 50);
    } else {
      // Failed to acquire: must leave the thread exactly as it found it.
      REQUIRE(RealtimePriority::getCurrentPriority() == before);
    }
  }

  const auto after = RealtimePriority::getCurrentPriority();
  REQUIRE(after == before);
  REQUIRE_FALSE(RealtimePriority::isRealtime());
}

TEST_CASE("setPolicy round-trips non-realtime policies", "[realtime]") {
  const auto entry = RealtimePriority::getCurrentPriority();

  // The pair every non-RT policy reports. setRealtimePriority() rejects this
  // outright, which is the whole reason setPolicy() exists.
  REQUIRE(RealtimePriority::setPolicy(SchedulerPolicy::NORMAL, 0));
  REQUIRE_FALSE(RealtimePriority::isRealtime());

  // Mismatched pairs are rejected before the syscall, both directions.
  REQUIRE_FALSE(RealtimePriority::setPolicy(SchedulerPolicy::NORMAL, 1));
  REQUIRE_FALSE(RealtimePriority::setPolicy(SchedulerPolicy::FIFO, 0));
  REQUIRE_FALSE(RealtimePriority::setPolicy(SchedulerPolicy::RR, 100));
  REQUIRE(RealtimePriority::getCurrentPriority() ==
          std::pair{SchedulerPolicy::NORMAL, 0});

  RealtimePriority::setPolicy(entry.first, entry.second);
}

#ifdef __linux__ // SCHED_BATCH, and therefore SchedulerPolicy::BATCH, is Linux-only
TEST_CASE("ScopedRealtimePriority restores a BATCH thread", "[realtime]") {
  // The regression case. It needs BOTH a non-NORMAL entry policy AND a
  // successful RT acquisition, so it self-skips when the host does not grant
  // them - unprivileged it proves nothing, as root it catches the thread
  // being stranded at SCHED_FIFO on scope exit.
  const auto entry = RealtimePriority::getCurrentPriority();

  if (!RealtimePriority::setPolicy(SchedulerPolicy::BATCH, 0)) {
    SUCCEED("SCHED_BATCH not permitted here");
    return;
  }
  REQUIRE(RealtimePriority::getCurrentPriority() ==
          std::pair{SchedulerPolicy::BATCH, 0});

  // The defect itself, without needing privileges: this is exactly the call
  // the old destructor made to restore a BATCH thread, and it cannot work.
  REQUIRE_FALSE(
      RealtimePriority::setRealtimePriority(0, SchedulerPolicy::BATCH));
  // What the destructor calls now.
  REQUIRE(RealtimePriority::setPolicy(SchedulerPolicy::BATCH, 0));

  bool acquired = false;
  {
    ScopedRealtimePriority scoped(50);
    acquired = scoped.isEnabled();
  }

  if (acquired) {
    // Previously left as {FIFO, 50}: the restore ran setRealtimePriority(0,
    // BATCH), which failed the 1-99 range check and returned without doing
    // anything.
    REQUIRE(RealtimePriority::getCurrentPriority() ==
            std::pair{SchedulerPolicy::BATCH, 0});
    REQUIRE_FALSE(RealtimePriority::isRealtime());
  }

  RealtimePriority::setPolicy(entry.first, entry.second);
  REQUIRE(RealtimePriority::getCurrentPriority() == entry);
}
#endif

TEST_CASE("ScopedRealtimePriority rejects an invalid priority", "[realtime]") {
  const auto before = RealtimePriority::getCurrentPriority();
  {
    ScopedRealtimePriority scoped(0); // out of the 1-99 window
    REQUIRE_FALSE(scoped.isEnabled());
  }
  REQUIRE(RealtimePriority::getCurrentPriority() == before);
}

TEST_CASE("unlockMemory is safe with nothing locked", "[realtime]") {
  // munlockall() succeeds regardless, so this is a plain no-crash guard on the
  // unlock half of the API.
  REQUIRE(RealtimePriority::unlockMemory());
}
