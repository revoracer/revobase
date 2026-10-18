#include <catch2/catch_test_macros.hpp>

#include <revobase/CpuAffinity.h>

using namespace revobase::system;

TEST_CASE("cpuListContains parses single CPUs and ranges", "[affinity]") {
  const std::string list = "0-3,8,10-11";
  REQUIRE(CpuAffinity::cpuListContains(list, 0));
  REQUIRE(CpuAffinity::cpuListContains(list, 3));
  REQUIRE(CpuAffinity::cpuListContains(list, 8));
  REQUIRE(CpuAffinity::cpuListContains(list, 11));
  REQUIRE_FALSE(CpuAffinity::cpuListContains(list, 4));
  REQUIRE_FALSE(CpuAffinity::cpuListContains(list, 9));
  REQUIRE_FALSE(CpuAffinity::cpuListContains(list, 12));
}

TEST_CASE("cpuListContains skips blank and non-numeric tokens", "[affinity]") {
  // A raw sysfs read can carry a trailing newline or be empty; none of these
  // may throw out of std::stoi.
  REQUIRE_FALSE(CpuAffinity::cpuListContains("", 0));
  REQUIRE_FALSE(CpuAffinity::cpuListContains("\n", 0));
  REQUIRE(CpuAffinity::cpuListContains("2,,5", 5));
  REQUIRE(CpuAffinity::cpuListContains("1-2,\n", 2));
}

TEST_CASE("readCpuList returns empty for a missing file", "[affinity]") {
  REQUIRE(CpuAffinity::readCpuList("/nonexistent/revobase/cpulist").empty());
}

#ifdef __linux__
TEST_CASE("housekeepingCpuSet selects at least one CPU", "[affinity]") {
  cpu_set_t cpuset;
  REQUIRE(CpuAffinity::housekeepingCpuSet(cpuset));
  REQUIRE(CPU_COUNT(&cpuset) > 0);
}

TEST_CASE("pinThreadOrHousekeeping without a requested core falls back",
          "[affinity]") {
  const auto original = CpuAffinity::getCurrentAffinity();
  bool pinned_to_requested = true;
  const bool pinned = CpuAffinity::pinThreadOrHousekeeping(
      pthread_self(), -1, "test", &pinned_to_requested);
  CpuAffinity::pinToCores(original);
  REQUIRE(pinned);
  REQUIRE_FALSE(pinned_to_requested);
}
#endif
