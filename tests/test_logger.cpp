#include "logger.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("logger::setup contract", "[logger]") {
  const fs::path dir = fs::temp_directory_path() /
                       ("racer_logger_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path log_file = dir / "test.log";

  // A missing DIRECTORY is the real error. The message names the directory,
  // not the file, because the file is not what failed.
  REQUIRE_THROWS_WITH(revobase::logger::setup(
                          "pre_init", dir / "no_such_subdir" / "app.log",
                          spdlog::level::info, false),
                      ContainsSubstring("does not exist"));

  // An unwritable directory is caught up front too, rather than surfacing
  // midway through spdlog sink construction.
  const fs::path ro_dir = dir / "readonly";
  fs::create_directories(ro_dir);
  fs::permissions(ro_dir, fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace);
  if (::access(ro_dir.c_str(), W_OK) != 0) { // skipped when running as root
    REQUIRE_THROWS_WITH(revobase::logger::setup("pre_init", ro_dir / "app.log",
                                             spdlog::level::info, false),
                        ContainsSubstring("not writable"));
  }
  fs::permissions(ro_dir, fs::perms::owner_all, fs::perm_options::replace);

  // The file itself does NOT need to exist - the daily sink creates its own
  // dated file. This is the whole point of checking the directory instead.
  REQUIRE_FALSE(fs::exists(log_file));
  REQUIRE_NOTHROW(
      revobase::logger::setup("racer_test", log_file, spdlog::level::info, false));

  // ...and the path handed in is still never created, only the dated sibling.
  REQUIRE_FALSE(fs::exists(log_file));

  // Second call must be rejected, not silently ignored - a silent no-op would
  // let a caller believe it had installed its own sinks.
  REQUIRE_THROWS_WITH(revobase::logger::setup("racer_test_again", log_file,
                                           spdlog::level::debug, false),
                      ContainsSubstring("already initialized"));

  // The already-initialized check runs BEFORE the path checks, so a repeat
  // call with a bad path reports the useful error rather than blaming the
  // path.
  REQUIRE_THROWS_WITH(revobase::logger::setup(
                          "racer_test_again", dir / "no_such_subdir" / "a.log",
                          spdlog::level::debug, false),
                      ContainsSubstring("already initialized"));

  // A failed setup must not have swapped the default logger out from under
  // whoever is already logging.
  REQUIRE(spdlog::default_logger() != nullptr);
  REQUIRE(spdlog::default_logger()->name() == "racer_test");

  SPDLOG_INFO("logger test wrote this line");
  spdlog::default_logger()->flush();

  // The daily sink dates the filename, so the configured path itself stays
  // empty; find the file it actually opened next to it.
  //
  // Polled, not checked once: setup() builds an async_logger, and both the
  // log call and flush() only enqueue onto the thread pool. Asserting
  // immediately races the background thread.
  auto anyFileHasContent = [&dir] {
    for (const auto &entry : fs::directory_iterator(dir)) {
      if (entry.is_regular_file() && entry.file_size() > 0)
        return true;
    }
    return false;
  };
  bool wrote_something = false;
  for (int i = 0; i < 300 && !wrote_something; ++i) {
    wrote_something = anyFileHasContent();
    if (!wrote_something)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(wrote_something);

  // Leave the process's logger pointed at a live sink: the file is unlinked
  // here but the fd stays open, so later logging in this process is harmless.
  std::error_code ec;
  fs::remove_all(dir, ec);
}
