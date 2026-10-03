// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

#include <filesystem>
#include <string>

#include <spdlog/common.h>

namespace revobase::logger {
// Installs the process-wide async logger. Call once from main before any
// other thread is started or logs: this swaps spdlog's default logger, which
// the SPDLOG_* macros read unlocked. Concurrent calls, or calls racing a
// logging thread, are undefined. Throws if called twice.
//
// log_file_path names a DAILY-ROTATED sink, so the file actually written is
// dated - "<stem>_YYYY-MM-DD<ext>" alongside the given path - and
// log_file_path itself is never opened. It therefore does not need to exist;
// only its parent directory does, and it must be writable. Both are checked
// up front and reported as std::runtime_error.
void setup(const std::string &logger_name,
           const std::filesystem::path &øe,
           spdlog::level::level_enum log_level, bool enable_console);
}
