// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <sys/stat.h>

#include <fmt/core.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace revobase {
class FileUtils {
public:

  static inline bool rmDir(const std::filesystem::path &dir) {
    namespace fs = std::filesystem;
    if (dir.empty())
      return false;
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec)
      return false;
    return fs::remove_all(dir, ec) != 0 && !ec;
  }

  static inline bool mkDir(const std::filesystem::path &directory) {
    return std::filesystem::create_directory(directory);
  }

  static inline bool exists(const std::filesystem::path &path) {
    return std::filesystem::exists(path);
  }

  static inline bool touch(const std::filesystem::path &path) {
    {
      std::ofstream f(path, std::ios::app);
      if (!f.is_open())
        return false;
    }
    return ::utimensat(AT_FDCWD, path.c_str(), nullptr, 0) == 0;
  }

  static inline nlohmann::json loadConfig(const std::filesystem::path &path) {
    if (!FileUtils::exists(path) || !std::filesystem::is_regular_file(path)) {
      throw std::runtime_error(fmt::format("error file {}", path.string()));
    }
    std::ifstream fin(path);
    return nlohmann::json::parse(fin, nullptr, true, true);
  }

  static inline std::vector<std::filesystem::path>
  listDirs(const std::filesystem::path &path) {
    std::vector<std::filesystem::path> dirs;
    for (const auto &entry : std::filesystem::directory_iterator(path)) {
      if (entry.is_directory()) {
        dirs.emplace_back(entry.path());
      }
    }
    return dirs;
  }

  [[nodiscard]] static inline bool hasEnv(const std::string &name) {
    return std::getenv(name.c_str()) != nullptr;
  }

  [[nodiscard]] static inline std::string getEnv(const std::string &name) {
    const char *v = std::getenv(name.c_str());
    return !v ? "" : v;
  }
};

}
