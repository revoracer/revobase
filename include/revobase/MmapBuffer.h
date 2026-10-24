// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RevoRacer
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace revobase {
namespace os {
class MmapError : public std::runtime_error {
public:
  explicit MmapError(const std::string &message)
      : std::runtime_error(message) {}
};

enum class MsyncMode { NONE, ASYNC, SYNC };

struct ReadOnlyMmapOptions {
  bool populate_read{false};
  bool advise_hugepage{false};
  // mlock the mapping after any populate. Release it with locked=true.
  bool lock_pages{false};
};

// Populating and locking are separate decisions: populate removes later page
// faults, lock_pages keeps the pages resident. Both run synchronously on the
// calling thread and cost time proportional to the mapping size.
struct MmapLoadOptions {
  // Establish every page before returning (MAP_POPULATE plus an explicit
  // touch, with sequential/hugepage advice) so later accesses do not fault.
  bool populate{false};
  // mlock the mapping. Implies populate, since mlock faults every page in.
  // Release it with locked=true.
  bool lock_pages{false};
  // When a writer creates a brand-new file and the mapping is populated,
  // zero-fill it instead of only touching it. Callers that establish exclusive
  // ownership of the segment only AFTER this returns (e.g. AppenderWarmer,
  // which re-checks the active segment post-map and does its own zeroing) must
  // pass false: an early bulk memset here races a writer that maps the
  // just-created file and starts committing frames. With false the pages are
  // still faulted/locked, just not wiped.
  bool zero_new_file{true};
};

class MmapBuffer {
public:
  static std::uintptr_t loadMmapBuffer(const std::string &path,
                                       std::size_t size, bool is_writing,
                                       MmapLoadOptions options = {});
  static std::uintptr_t
  loadExistingReadOnlyMmapBuffer(const std::string &path, std::size_t size,
                                 ReadOnlyMmapOptions options = {});
  static void prepareReadOnlyMmapBuffer(const std::string &path,
                                        std::uintptr_t address,
                                        std::size_t size,
                                        ReadOnlyMmapOptions options);
  // msync_mode has no default on purpose: durability is the caller's decision,
  // and the obvious-looking default (SYNC) is the one no revoq path wants - it
  // blocks on write-back for the whole mapping, on whatever thread happens to
  // drop the last reference. Say NONE explicitly to rely on kernel writeback.
  // locked: the mapping was loaded with lock_pages, so munlock it first.
  static bool releaseMmapBuffer(std::uintptr_t address, std::size_t size,
                                bool locked, MsyncMode msync_mode);
};
} // namespace os
} // namespace revobase
