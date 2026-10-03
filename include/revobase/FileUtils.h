// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

#include <filesystem>
#include <fstream>

#include <fcntl.h>    // AT_FDCWD
#include <sys/stat.h> // utimensat

#include <fmt/core.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace revobase {
class FileUtils {
public:
  // Recursively removes a directory. Returns false - without deleting
  // anything - if the path is empty, missing, or not a directory.
  //
  // The precondition is checked at runtime, not with assert(): this recurses,
  // and NDEBUG builds would otherwise happily remove_all() a regular file, or
  // whatever an empty path resolves to.
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

  // both filename and filedir are acceptable.
  static inline bool exists(const std::filesystem::path &path) {
    return std::filesystem::exists(path);
  }

  // Creates the file if it is missing, and leaves an existing file's contents
  // alone. Append mode is deliberate: a default-constructed ofstream implies
  // ios::trunc, so the obvious spelling would silently discard whatever was
  // already there - the opposite of what the name promises, and of what every
  // caller wants. Callers therefore do NOT need to guard with exists().
  //
  // Then sets atime/mtime to now, like touch(1). Opening in append mode and
  // closing without writing does not itself move mtime, so this takes an
  // explicit utimensat(); a null timespec means "now" for both stamps.
  //
  // Returns true only when BOTH halves succeeded, i.e. the file exists and
  // its mtime is current. False means either the file could not be opened -
  // missing parent directory, no write permission, path is a directory - or
  // the timestamps could not be updated.
  static inline bool touch(const std::filesystem::path &path) {
    {
      std::ofstream f(path, std::ios::app);
      if (!f.is_open())
        return false;
    } // closed before restamping, so the write below is the last one
    return ::utimensat(AT_FDCWD, path.c_str(), nullptr, 0) == 0;
  }

  static inline nlohmann::json loadJsonFile(const std::filesystem::path &path) {
    if (!FileUtils::exists(path) || !std::filesystem::is_regular_file(path)) {
      throw std::runtime_error(fmt::format("error file {}", path.string()));
    }
    std::ifstream fin(path);
    return nlohmann::json::parse(fin, nullptr, // parse callback
                                 true,         // allow exceptions
                                 true,         // ignore comments
                                 false         // ignore_trailing_commas
    );
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
} // namespace revobase
