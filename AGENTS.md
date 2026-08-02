# AGENTS.md

Instructions for coding agents working in `revobase`.

## Project role

`revobase` is a low-level C++23 platform utility library used by RevoRacer
data-path projects. Keep it focused on reusable primitives: mmap/file helpers,
time and TSC utilities, CPU affinity, realtime scheduling, hashing, fixed-scale
decimal parsing, strings, and logging support.

Do not add transport, broker, stream, journal, or AI-agent orchestration
semantics here. Those belong in higher-level repositories such as `revoq`.

## Build and test

Initialize dependencies before building:

```bash
git submodule update --init --recursive
```

Common commands:

```bash
./build.sh doctor
CXX=/usr/bin/clang++-20 ./build.sh
CXX=/usr/bin/clang++-20 ./build.sh test
```

`./build.sh test` configures, builds, and runs the C++ test suite through
`ctest`. CI also runs GCC/Clang on Linux and Clang on macOS.

## Platform assumptions

- Supported platforms: Linux and macOS.
- Unsupported: Windows/MSVC.
- Language level: C++23.
- Some host-tuning helpers require Linux privileges or host configuration:
  realtime priority, memory locking, CPU isolation, hugepage-sensitive mmap
  behavior, and stack prefaulting.

Portable tests prove functionality. Do not convert portable test results into
latency claims.

## Public API boundaries

Public headers live under `include/revobase/` and are included as:

```cpp
#include <revobase/MmapBuffer.h>
#include <revobase/TscTimer.h>
```

Preserve the `revobase` namespace and the `revobase::system` /
`revobase::os` subnamespace split unless an API change is deliberate and
documented.

## Editing guidance

- Prefer small platform helpers over broad abstractions.
- Keep Linux and macOS behavior explicit; avoid silent platform fallbacks that
  make tuning failures look successful.
- Add or update tests when behavior changes.
- Do not change mmap, affinity, realtime, or timing behavior without reading the
  corresponding tests first.
- Keep README claims tied to code or tests in this repo. Latency benchmark
  claims belong in repos that actually run benchmarks with captured host data.

## Files worth reading

- `README.md` - project overview, build instructions, CMake usage.
- `include/revobase/MmapBuffer.h` and `src/MmapBuffer.cpp` - mmap contract.
- `include/revobase/TscTimer.h` - TSC calibration and timestamp helpers.
- `include/revobase/CpuAffinity.h` - CPU pinning and isolation checks.
- `include/revobase/RealtimePriority.h` - realtime policy and memory-locking
  helpers.
- `tests/` - expected behavior and platform edge cases.
