#include "FileUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace revobase;
namespace fs = std::filesystem;

namespace {

// Every case works inside its own directory under temp_directory_path() so the
// destructive rmDir tests can never touch the source tree, and so running the
// binary directly (all cases in one process) does not cross-contaminate.
class TempTree {
public:
  TempTree() {
    root_ = fs::temp_directory_path() /
            fs::path("racer_fileutils_test_" + std::to_string(::getpid()) +
                     "_" + std::to_string(counter_++));
    fs::create_directories(root_);
  }
  ~TempTree() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }
  TempTree(const TempTree &) = delete;
  TempTree &operator=(const TempTree &) = delete;

  const fs::path &root() const { return root_; }
  fs::path operator/(const char *leaf) const { return root_ / leaf; }

  fs::path writeFile(const char *leaf, std::string_view contents) const {
    fs::path p = root_ / leaf;
    std::ofstream f(p);
    f << contents;
    return p;
  }

private:
  fs::path root_;
  static inline int counter_ = 0;
};

} // namespace

TEST_CASE("FileUtils::rmDir refuses anything that is not a directory",
          "[fileutils]") {
  TempTree tmp;

  SECTION("empty path") {
    // Guarded explicitly: an empty path resolves to the current directory for
    // some operations, and remove_all() on it would be catastrophic.
    REQUIRE_FALSE(FileUtils::rmDir(fs::path{}));
  }

  SECTION("missing path") {
    REQUIRE_FALSE(FileUtils::rmDir(tmp / "no_such_dir"));
  }

  SECTION("regular file is left in place") {
    fs::path file = tmp.writeFile("regular.txt", "payload");
    REQUIRE_FALSE(FileUtils::rmDir(file));
    REQUIRE(fs::exists(file)); // must not have been deleted
  }
}

TEST_CASE("FileUtils::rmDir removes a populated tree recursively",
          "[fileutils]") {
  TempTree tmp;
  fs::path nested = tmp / "outer";
  fs::create_directories(nested / "inner");
  std::ofstream(nested / "a.txt") << "a";
  std::ofstream(nested / "inner" / "b.txt") << "b";

  REQUIRE(FileUtils::rmDir(nested));
  REQUIRE_FALSE(fs::exists(nested));
  REQUIRE(fs::exists(tmp.root())); // only the requested subtree went away
}

TEST_CASE("FileUtils::rmDir on a symlink removes the link, not the target",
          "[fileutils]") {
  TempTree tmp;
  fs::path target = tmp / "target_dir";
  fs::create_directories(target);
  std::ofstream(target / "keepme.txt") << "keep";

  fs::path link = tmp / "link_to_dir";
  std::error_code ec;
  fs::create_directory_symlink(target, link, ec);
  if (ec) {
    SUCCEED("symlink creation unsupported here");
    return;
  }

  // is_directory() follows the link so the guard passes, but remove_all() does
  // not follow it. Pinning this down because the alternative - following into
  // the target - would make rmDir() delete data outside the path it was given.
  REQUIRE(FileUtils::rmDir(link));
  REQUIRE_FALSE(fs::exists(fs::symlink_status(link)));
  REQUIRE(fs::exists(target / "keepme.txt"));
}

TEST_CASE("FileUtils::mkDir / exists / touch", "[fileutils]") {
  TempTree tmp;

  fs::path dir = tmp / "made";
  REQUIRE(FileUtils::mkDir(dir));
  REQUIRE(FileUtils::exists(dir));
  // create_directory() reports false when the directory is already there.
  REQUIRE_FALSE(FileUtils::mkDir(dir));

  fs::path file = tmp / "touched.txt";
  REQUIRE_FALSE(FileUtils::exists(file));
  REQUIRE(FileUtils::touch(file));
  REQUIRE(FileUtils::exists(file));
  REQUIRE(fs::is_regular_file(file));

  std::ofstream(file) << "some content";
  const auto size_before = fs::file_size(file);
  REQUIRE(size_before > 0);
  REQUIRE(FileUtils::touch(file));
  REQUIRE(fs::file_size(file) == size_before);

  std::ifstream check(file);
  std::string contents;
  std::getline(check, contents);
  REQUIRE(contents == "some content");

  const auto backdated =
      fs::last_write_time(file) - std::chrono::hours(24);
  fs::last_write_time(file, backdated);
  REQUIRE(fs::last_write_time(file) == backdated);

  REQUIRE(FileUtils::touch(file));
  REQUIRE(fs::last_write_time(file) > backdated);
  REQUIRE(fs::file_size(file) == size_before); // restamping is not a write

  // Failure path: no such parent directory.
  REQUIRE_FALSE(FileUtils::touch(tmp / "missing_parent" / "f.txt"));
}

TEST_CASE("FileUtils::loadConfig rejects non-files", "[fileutils]") {
  TempTree tmp;

  REQUIRE_THROWS_AS(FileUtils::loadConfig(tmp / "absent.json"),
                    std::runtime_error);
  // A directory exists but is not a regular file - must throw, not try to
  // parse it.
  REQUIRE_THROWS_AS(FileUtils::loadConfig(tmp.root()), std::runtime_error);
}

TEST_CASE("FileUtils::loadConfig parsing rules", "[fileutils]") {
  TempTree tmp;

  SECTION("plain object") {
    fs::path p = tmp.writeFile("ok.json", R"({"a": 1, "b": [2, 3]})");
    auto j = FileUtils::loadConfig(p);
    REQUIRE(j["a"].get<int>() == 1);
    REQUIRE(j["b"].size() == 2);
  }

  SECTION("comments are accepted") {
    // loadConfig passes ignore_comments=true, which is why the checked-in
    // configs/*.json are allowed to carry // annotations.
    fs::path p = tmp.writeFile("comments.json", R"({
      // leading comment
      "a": 1, /* inline */ "b": 2
    })");
    auto j = FileUtils::loadConfig(p);
    REQUIRE(j["a"].get<int>() == 1);
    REQUIRE(j["b"].get<int>() == 2);
  }

  SECTION("trailing commas are rejected") {
    // ignore_trailing_commas=false: a stray comma is a config error, not
    // something to silently accept.
    fs::path p = tmp.writeFile("trailing.json", R"({"a": 1, "b": 2,})");
    REQUIRE_THROWS_AS(FileUtils::loadConfig(p), nlohmann::json::parse_error);
  }

  SECTION("malformed json throws") {
    fs::path p = tmp.writeFile("bad.json", R"({"a": )");
    REQUIRE_THROWS_AS(FileUtils::loadConfig(p), nlohmann::json::parse_error);
  }

  SECTION("empty file throws") {
    fs::path p = tmp.writeFile("empty.json", "");
    REQUIRE_THROWS_AS(FileUtils::loadConfig(p), nlohmann::json::parse_error);
  }
}

TEST_CASE("FileUtils::listDirs returns directories only", "[fileutils]") {
  TempTree tmp;
  fs::create_directories(tmp / "d1");
  fs::create_directories(tmp / "d2");
  std::ofstream(tmp / "f1.txt") << "x";

  auto dirs = FileUtils::listDirs(tmp.root());
  REQUIRE(dirs.size() == 2);

  std::vector<std::string> names;
  names.reserve(dirs.size());
  for (const auto &d : dirs)
    names.push_back(d.filename().string());
  std::sort(names.begin(), names.end());
  REQUIRE(names[0] == "d1");
  REQUIRE(names[1] == "d2");

  // Not recursive: a directory nested one level down is not reported.
  fs::create_directories(tmp / "d1" / "deep");
  REQUIRE(FileUtils::listDirs(tmp.root()).size() == 2);

  SECTION("empty directory yields nothing") {
    fs::path empty = tmp / "empty";
    fs::create_directories(empty);
    REQUIRE(FileUtils::listDirs(empty).empty());
  }
}

TEST_CASE("FileUtils::listDirs throws on a missing path", "[fileutils]") {
  TempTree tmp;
  REQUIRE_THROWS_AS(FileUtils::listDirs(tmp / "nope"),
                    std::filesystem::filesystem_error);
}

TEST_CASE("FileUtils env helpers", "[fileutils]") {
  const char *kName = "RACER_FILEUTILS_TEST_VAR";
  ::unsetenv(kName);

  REQUIRE_FALSE(FileUtils::hasEnv(kName));
  // Unset reads back as empty rather than dereferencing the null getenv().
  REQUIRE(FileUtils::getEnv(kName).empty());

  ::setenv(kName, "value-42", 1);
  REQUIRE(FileUtils::hasEnv(kName));
  REQUIRE(FileUtils::getEnv(kName) == "value-42");

  // Set-but-empty is indistinguishable from unset through getEnv() alone;
  // hasEnv() is the only way to tell them apart.
  ::setenv(kName, "", 1);
  REQUIRE(FileUtils::hasEnv(kName));
  REQUIRE(FileUtils::getEnv(kName).empty());

  ::unsetenv(kName);
  REQUIRE_FALSE(FileUtils::hasEnv(kName));
}
