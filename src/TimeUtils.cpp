// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#include <revobase/TimeUtils.h>

#include <iomanip>
#include <sstream>

#ifdef __APPLE__
#include <time.h>
#endif

namespace revobase {

/*
 * How often does CLOCK_REALTIME jump backward?
1. Typical NTP adjustment: ±10ms per day (gradual slewing)
2. Large step adjustment: Very rare (months/years)
3. Magnitude: Usually <1 second

 Because our orders live for milliseconds, if NTP jumps during that window,
 we have bigger problems(exchange connection, network)
 In practice, negligible concern
 * */
Timestamp Timestamp::now() noexcept {
#ifdef __APPLE__
  return Timestamp(clock_gettime_nsec_np(CLOCK_REALTIME));
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return Timestamp(static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                   ts.tv_nsec);
#endif
}

std::string Timestamp::toString() const {
  static constexpr auto NANOSECONDS_PER_SECOND = 1'000'000'000LL;

  char buf[64]{0};
  auto seconds = static_cast<time_t>(nsec_ / NANOSECONDS_PER_SECOND);
  struct tm tm_time{};
  gmtime_r(&seconds, &tm_time);
  int nano = static_cast<int>(nsec_ % NANOSECONDS_PER_SECOND);

  snprintf(buf, sizeof(buf), "%4d-%02d-%02d %02d:%02d:%02d.%09d",
           tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
           tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, nano);
  return buf;
}

std::string Timestamp::toDateString() const {
  static constexpr auto NANOSECONDS_PER_SECOND = 1'000'000'000LL;

  char buf[64]{0};
  auto seconds = static_cast<time_t>(nsec_ / NANOSECONDS_PER_SECOND);
  struct tm tm_time{};
  gmtime_r(&seconds, &tm_time);

  snprintf(buf, sizeof(buf), "%4d-%02d-%02d", tm_time.tm_year + 1900,
           tm_time.tm_mon + 1, tm_time.tm_mday);
  return buf;
}

Timestamp Timestamp::fromYYYYmmddHHMMSS(const std::string &str_date_time) {
  if (str_date_time.size() != LEN_YYYY_MM_DD_HH_MM_SS) {
    throw std::runtime_error("invalid datetime format: " + str_date_time);
  }

  std::tm timeinfo{};
  std::istringstream ss(str_date_time);
  ss >> std::get_time(&timeinfo, "%Y-%m-%d %H:%M:%S");

  if (ss.fail()) {
    throw std::runtime_error("failed to parse datetime: " + str_date_time);
  }

  std::time_t epochTime = timegm(&timeinfo);
  return Timestamp(epochTime * 1'000'000'000LL);
}

Timestamp Timestamp::fromYYYYmmdd(const std::string &str_date) {
  if (str_date.size() != LEN_YYYY_MM_DD) {
    throw std::runtime_error("invalid date format: " + str_date);
  }
  return Timestamp::fromYYYYmmddHHMMSS(str_date + " 00:00:00");
}

} // namespace revobase
