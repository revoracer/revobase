// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

#include <filesystem>
#include <spdlog/spdlog.h>
#include <string>

namespace revobase::logger {

void setup(const std::string &logger_name,
           const std::filesystem::path &log_file_path,
           spdlog::level::level_enum log_level, bool enable_console);
}
