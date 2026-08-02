# revobase

[![CI](https://github.com/revoracer/revobase/actions/workflows/ci.yml/badge.svg)](https://github.com/revoracer/revobase/actions/workflows/ci.yml)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](#quick-start)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey.svg)](#platform-support)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

Low-level C++23 utilities for latency-sensitive infrastructure.

`revobase` is the foundation library shared by RevoRacer's open-source data-path
projects. It keeps the platform code that tends to decide whether a fast design
stays fast in production: memory-mapped file handling, CPU affinity, realtime
scheduling helpers, timestamp conversion, TSC timing, hashing, fixed-scale
decimal parsing, string/file utilities, and logging setup.

## Why this repo exists

RevoRacer publishes `revobase` so teams can inspect the small operational pieces
under our low-latency systems instead of taking a benchmark headline on trust.
The interesting work in latency infrastructure is often not a single clever
algorithm; it is the accumulation of careful choices around memory, clocks,
scheduler behavior, file layout, and measurement.

In AI-agent systems, the infrastructure problem is expanding beyond chat
orchestration. Agents call tools, route events, replay decisions, cross process
boundaries, and eventually trigger real actions. The same engineering discipline
used in trading systems applies there too: deterministic event handling, clear
failure surfaces, auditable replay, and low tail latency where decisions pass
between local components. `revobase` is not an agent framework; it is the
low-level platform layer used by libraries that need those properties.

## What is inside

| Area | Public headers | Purpose |
| --- | --- | --- |
| Memory mapping | `MmapBuffer.h`, `FileUtils.h` | Mmap-backed buffers and file helpers for stream/journal storage. |
| Host tuning | `CpuAffinity.h`, `RealtimePriority.h` | CPU pinning, isolation checks, realtime policy helpers, memory locking, and stack prefaulting. |
| Time | `TimeUtils.h`, `TscTimer.h` | `std::chrono` interop, timestamp utilities, and cycle timing. |
| Data utilities | `DecimalUtils.h`, `HashUtils.h`, `MurMurHash3.h`, `StringUtils.hpp` | Fixed-scale decimal parsing, hashing, and small string helpers. |
| Runtime support | `logger.h`, `portable_intrinsics.h` | Logging setup and compiler/platform intrinsics used by RevoRacer libraries. |

## Relationship to revoq

[`revoq`](https://github.com/revoracer/revoq) is the higher-level transport and
journal library: a file-backed, memory-mapped IPC stream for low-latency
same-host messaging and replay. `revoq` uses `revobase` for mmap, timing,
hashing, affinity, realtime scheduling, and file utilities.

If you are evaluating RevoRacer as a low-latency infrastructure vendor,
`revobase` shows the platform primitives; `revoq` shows how those primitives are
assembled into a measurable transport with documented memory ordering, file
format, benchmarks, and failure semantics.

## Status

This is an early public split of code that was previously shipped inside other
RevoRacer repositories. The API is not yet stable.

Design limits:

- `revobase` is a platform utility library, not a transport, broker, storage
  engine, or agent runtime.
- Linux and macOS are supported. Windows/MSVC support is not implemented.
- Realtime scheduling, memory locking, CPU isolation, and hugepage-sensitive
  behavior depend on host configuration and privileges.
- Portable builds are useful for development and tests; publishable latency
  claims require a documented tuned host.

## Requirements

- CMake 3.23 or newer
- A C++23 Clang/GCC-compatible compiler
- Git submodules for bundled dependencies:
  `fmt`, `spdlog`, `nlohmann_json`, and `Catch2` for tests

## Platform Support

`revobase` currently supports Linux and macOS. Windows/MSVC support is not
implemented.

## Bundled Dependency Pins

- `fmt`: 11.2.0
- `spdlog`: v1.17.0
- `nlohmann_json`: v3.11.3
- `Catch2`: v3.15.3

## Quick Start

```bash
git submodule update --init --recursive
CXX=/usr/bin/clang++-20 ./build.sh
CXX=/usr/bin/clang++-20 ./build.sh test
```

`build.sh test` configures, builds, and runs the C++ test suite via `ctest`.

## Build Commands

```bash
./build.sh                 # Release build
./build.sh debug           # Debug build
./build.sh test            # Build and run tests
./build.sh clean           # Remove build/
./build.sh doctor          # Print tool versions
```

Use `--build-dir DIR` to select a different build directory and `-j N` or
`--jobs N` to control parallelism.

## CMake Usage

From a parent CMake project:

```cmake
add_subdirectory(external/revobase)
target_link_libraries(my_target PRIVATE revobase::revobase)
```

From an installed package:

```cmake
find_package(revobase CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE revobase::revobase)
```

When configured from source with bundled submodules, `cmake --install` installs
`revobase` plus its bundled runtime dependencies into the selected prefix, so
that prefix is sufficient for `find_package(revobase CONFIG REQUIRED)`. If
using system-provided dependencies instead, `fmt`, `spdlog`, `nlohmann_json`,
and Threads must be discoverable by CMake.

Public headers are under `include/revobase` and should be included with the
module prefix:

```cpp
#include <revobase/TimeUtils.h>
#include <revobase/MmapBuffer.h>
```

## Example

```cpp
#include <revobase/MmapBuffer.h>
#include <revobase/TscTimer.h>

#include <cstring>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

int main() {
  constexpr std::size_t size = 4096;
  const std::string path = "/tmp/revobase-example.mmap";

  revobase::TscTimer::calibrate();
  const std::uint64_t start = revobase::TscTimer::now_cycles_begin();

  const std::uintptr_t addr =
      revobase::os::MmapBuffer::loadMmapBuffer(path, size, true, false);
  std::strcpy(reinterpret_cast<char *>(addr), "ready");
  revobase::os::MmapBuffer::releaseMmapBuffer(
      addr, size, false, revobase::os::MsyncMode::NONE);

  const std::uint64_t end = revobase::TscTimer::now_cycles_end();
  std::cout << "mapped/write/released in "
            << revobase::TscTimer::delta_ns(start, end) << " ns\n";
}
```

## Development

The default CMake build enables `-Wall -Wextra -Wpedantic` on GCC, Clang, and
Apple Clang for `revobase` targets. Use `-DREVOBASE_ENABLE_WARNINGS=OFF` to
disable those warnings locally.

## Commercial

RevoRacer builds low-latency trading infrastructure for hedge funds and trading
teams. Open-source `revobase` is intended to make the lower layers visible:
platform behavior, dependency choices, test boundaries, and the utility code
used by higher-level RevoRacer components.

Contact us through [revoracer.com](https://revoracer.com).

## License

`revobase` is licensed under Apache-2.0. MurmurHash3 is public-domain code by
Austin Appleby; see `NOTICE`.
