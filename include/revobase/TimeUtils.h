// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

#include <fmt/core.h>

namespace revobase {

class Timestamp {
public:
  static inline constexpr std::size_t LEN_YYYY_MM_DD{10};
  static inline constexpr std::size_t LEN_YYYY_MM_DD_HH_MM_SS{19};

  constexpr Timestamp() noexcept = default;
  explicit constexpr Timestamp(std::int64_t ts) noexcept : nsec_(ts) {}

  [[nodiscard]] constexpr std::int64_t nsec() const noexcept { return nsec_; }
  [[nodiscard]] constexpr std::int64_t microsec() const noexcept {
    return static_cast<int64_t>(nsec_ / 1'000);
  }
  [[nodiscard]] constexpr std::int64_t millisec() const noexcept {
    return static_cast<int64_t>(nsec_ / 1'000'000);
  }
  [[nodiscard]] constexpr std::int64_t sec() const noexcept {
    return static_cast<int64_t>(nsec_ / 1'000'000'000);
  }

  static Timestamp now() noexcept;
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] std::string toDateString() const;
  static Timestamp fromYYYYmmddHHMMSS(const std::string &str_date_time);
  static Timestamp fromYYYYmmdd(const std::string &str_date);

  static constexpr Timestamp min() noexcept { return Timestamp(0); }
  static constexpr Timestamp invalid() noexcept {
    static_assert(std::numeric_limits<std::int64_t>::min() < min().nsec_,
                  "invalid timestamp is valid!");
    return Timestamp(std::numeric_limits<std::int64_t>::min());
  }

  static constexpr Timestamp MAX_TIME() noexcept {
    return Timestamp(+4102444800000000000L); // 2100-01-01
  }

  [[nodiscard]] constexpr bool isValid() const noexcept { return nsec_ >= 0; }

  [[nodiscard]] constexpr bool operator==(const Timestamp &rhs) const noexcept {
    return nsec_ == rhs.nsec_;
  }

  [[nodiscard]] constexpr bool operator!=(const Timestamp &rhs) const noexcept {
    return nsec_ != rhs.nsec_;
  }

  [[nodiscard]] constexpr bool operator<(const Timestamp &rhs) const noexcept {
    return nsec_ < rhs.nsec_;
  }

  [[nodiscard]] constexpr bool operator<=(const Timestamp &rhs) const noexcept {
    return nsec_ <= rhs.nsec_;
  }

  [[nodiscard]] constexpr bool operator>(const Timestamp &rhs) const noexcept {
    return nsec_ > rhs.nsec_;
  }

  [[nodiscard]] constexpr bool operator>=(const Timestamp &rhs) const noexcept {
    return nsec_ >= rhs.nsec_;
  }

private:
  std::int64_t nsec_{};
};

static_assert(std::is_standard_layout_v<Timestamp>);
static_assert(std::is_trivially_copyable_v<Timestamp>);
static_assert(std::is_trivially_destructible_v<Timestamp>);
static_assert(sizeof(Timestamp) == 8, "sizeof(Timestamp) != 8");

// Timestamp arithmetic uses std::chrono::nanoseconds as its duration type.
// A duration here is a plain scalar count with none of Timestamp's wire-format
// or sentinel obligations, so a bespoke type had nothing to add over the
// standard one — and it cost something real: collapsing every unit to
// nanoseconds at construction discards the unit safety that keeps
// milliseconds and nanoseconds distinct types with explicit lossy conversions.
inline Timestamp operator+(const Timestamp &lhs,
                           std::chrono::nanoseconds rhs) noexcept {
  return Timestamp(lhs.nsec() + rhs.count());
}

inline Timestamp operator-(const Timestamp &lhs,
                           std::chrono::nanoseconds rhs) noexcept {
  return Timestamp(lhs.nsec() - rhs.count());
}

inline Timestamp &operator+=(Timestamp &self,
                             std::chrono::nanoseconds rhs) noexcept {
  self = self + rhs;
  return self;
}

inline Timestamp &operator-=(Timestamp &self,
                             std::chrono::nanoseconds rhs) noexcept {
  self = self - rhs;
  return self;
}

inline std::chrono::nanoseconds operator-(const Timestamp &lhs,
                                          const Timestamp &rhs) noexcept {
  return std::chrono::nanoseconds(lhs.nsec() - rhs.nsec());
}

inline constexpr Timestamp MAX_TIME = Timestamp::MAX_TIME();
} // namespace revobase

namespace std {
template <> struct hash<revobase::Timestamp> {
  size_t operator()(const revobase::Timestamp &t) const noexcept {
    return hash<std::int64_t>()(t.nsec());
  }
};
} // namespace std

template <> struct fmt::formatter<revobase::Timestamp> {
  template <typename ParseContext> constexpr auto parse(ParseContext &ctx) {
    return ctx.begin();
  }
  template <typename FormatContext>
  constexpr auto format(const revobase::Timestamp &ts, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "{}", ts.toString());
  }
};
