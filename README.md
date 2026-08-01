# revobase

Low-level C++ utilities used by RevoRacer libraries.

`revobase` collects the small platform and data-path helpers shared by
`revoq`, `revobook` examples, and internal RevoRacer components: memory-mapped
file buffers, timestamp helpers with `std::chrono` duration interop,
string/hash utilities, fixed-scale decimal parsing, CPU affinity /
realtime-priority helpers, TSC timing, and logging setup.

## Status

This is an early public split of code that was previously shipped inside other
RevoRacer repositories. The API is not yet stable.

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

## Development

The default CMake build enables `-Wall -Wextra -Wpedantic` on GCC, Clang, and
Apple Clang for `revobase` targets. Use `-DREVOBASE_ENABLE_WARNINGS=OFF` to
disable those warnings locally.

## License

`revobase` is licensed under Apache-2.0. MurmurHash3 is public-domain code by
Austin Appleby; see `NOTICE`.
