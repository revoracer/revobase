// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#include <revobase/Logger.h>

#include <memory>
#include <system_error>

#include <unistd.h> // access, W_OK

#include <fmt/core.h>
#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace revobase::logger {
constexpr const char *kDefaultLogPattern =
    "[%m/%d %T.%6F] [%^%=5l%$] [p-%P] [%s:%#] %v";

void setup(const std::string &logger_name,
           const std::filesystem::path &log_file_base,
           spdlog::level::level_enum log_level, bool enable_console) {
  // Plain bool, not atomic: setup() is single-threaded-startup only (see
  // logger.h). Set on success rather than on entry so a failed setup can be
  // retried with a corrected path.
  static bool logger_initialized = false;
  if (logger_initialized) {
    throw std::runtime_error("Logger already initialized");
  }
  // Validate the parent DIRECTORY, not the file. The daily sink dates the
  // filename, so it never opens log_file_path itself - requiring that path to
  // exist only forced callers to create a placeholder the logger then ignored.
  // A writable directory is the real precondition.
  //
  // spdlog's file_helper would fail on its own here, but only midway through
  // construction and after init_thread_pool() has spawned its worker. Failing
  // first turns a typo'd path into one clear message.
  const std::filesystem::path log_dir = log_file_base.has_parent_path()
                                            ? log_file_base.parent_path()
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

  spdlog::init_thread_pool(32768, 1); // 32K queue, 1 background thread
  auto console_sink =
      enable_console ? std::make_shared<spdlog::sinks::stdout_color_sink_mt>()
                     : nullptr;
  if (console_sink) {
    console_sink->set_level(log_level);
  }

  // Daily rotating file sink with large buffer
  auto daily_sink =
      std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_file_base, 0, 0);
  daily_sink->set_level(log_level);
  auto logger = std::make_shared<spdlog::async_logger>(
      logger_name,
      console_sink ? spdlog::sinks_init_list{console_sink, daily_sink}
                   : spdlog::sinks_init_list{daily_sink},
      spdlog::thread_pool(),
      spdlog::async_overflow_policy::block // or overrun_oldest; block
  );
  logger->set_pattern(kDefaultLogPattern, spdlog::pattern_time_type::utc);
  logger->set_level(log_level);
  spdlog::set_default_logger(logger);
  // DON'T flush on every log in production! Only critical errors
  spdlog::flush_on(spdlog::level::critical);
  // Periodic flush is better for latency (or disable entirely)
  // spdlog::flush_every(std::chrono::seconds(5));
  logger_initialized = true;
  SPDLOG_INFO("AsyncLogger [{}] initialized with level {}", logger_name,
              spdlog::level::to_string_view(log_level));
}
} // namespace revobase::logger
