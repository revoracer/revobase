// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#include <revobase/logger.h>

#include <memory>
#include <system_error>

#include <unistd.h>

#include <fmt/core.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace revobase::logger {
constexpr const char *kDefaultLogPattern =
    "[%m/%d %T.%6F] [%^%=5l%$] [p-%P] [%s:%#] %v";

void setup(const std::string &logger_name,
           const std::filesystem::path &log_file_path,
           spdlog::level::level_enum log_level, bool enable_console) {

  static bool logger_initialized = false;
  if (logger_initialized) {
    throw std::runtime_error("Logger already initialized");
  }

  const std::filesystem::path log_dir = log_file_path.has_parent_path()
                                            ? log_file_path.parent_path()
                                            : std::filesystem::path(".");
  std::error_code ec;
  if (!std::filesystem::is_directory(log_dir, ec) || ec) {
    throw std::runtime_error(
        fmt::format("log directory {} does not exist", log_dir.string()));
  }
  if (::access(log_dir.c_str(), W_OK) != 0) {
    throw std::runtime_error(
        fmt::format("log directory {} is not writable", log_dir.string()));
  }

  spdlog::init_thread_pool(32768, 1);
  auto console_sink =
      enable_console ? std::make_shared<spdlog::sinks::stdout_color_sink_mt>()
                     : nullptr;
  if (console_sink) {
    console_sink->set_level(log_level);
  }

  auto daily_sink =
      std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_file_path, 0, 0);
  daily_sink->set_level(log_level);
  auto logger = std::make_shared<spdlog::async_logger>(
      logger_name,
      console_sink ? spdlog::sinks_init_list{console_sink, daily_sink}
                   : spdlog::sinks_init_list{daily_sink},
      spdlog::thread_pool(),
      spdlog::async_overflow_policy::block
  );
  logger->set_pattern(kDefaultLogPattern, spdlog::pattern_time_type::utc);
  logger->set_level(log_level);
  spdlog::set_default_logger(logger);

  spdlog::flush_on(spdlog::level::critical);

  logger_initialized = true;
  SPDLOG_INFO("AsyncLogger [{}] initialized with level {}", logger_name,
              spdlog::level::to_string_view(log_level));
}
}
