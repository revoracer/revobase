#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="Release"
BUILD_TESTS="ON"
RUN_TESTS="OFF"
CONFIGURE_ONLY="OFF"
JOBS=""
DEPENDENCY_SUBMODULES=(external/fmt external/spdlog external/nlohmann_json)

usage() {
  cat <<EOF
Usage: ./build.sh [command] [options]

Commands:
  release          Configure and build Release (default)
  debug            Configure and build Debug
  test             Build and run C++ tests
  clean            Remove the build directory
  doctor           Print local build tool versions

Options:
  --build-dir DIR      Use a custom build directory
  -j, --jobs N         Parallel build jobs
  --no-tests           Do not build C++ tests
  --configure-only     Configure without building
  -h, --help           Show this help

Examples:
  ./build.sh
  CXX=/usr/bin/clang++-20 ./build.sh test --jobs 16
EOF
}

command="release"
if [[ $# -gt 0 && "$1" != -* ]]; then
  command="$1"
  shift
fi

case "${command}" in
  release)
    BUILD_TYPE="Release"
    ;;
  debug)
    BUILD_TYPE="Debug"
    ;;
  test)
    BUILD_TYPE="Release"
    BUILD_TESTS="ON"
    RUN_TESTS="ON"
    ;;
  clean)
    rm -rf "${BUILD_DIR}"
    exit 0
    ;;
  doctor)
    echo "CXX=${CXX:-}"
    cmake --version
    git --version
    if command -v "${CXX:-c++}" >/dev/null 2>&1; then
      "${CXX:-c++}" --version | head -n 1
    else
      echo "C++ compiler not found. Set CXX=/path/to/clang++ or install a C++23 compiler."
    fi
    exit 0
    ;;
  -h|--help|help)
    usage
    exit 0
    ;;
  *)
    echo "Unknown command: ${command}" >&2
    usage >&2
    exit 2
    ;;
esac

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      if [[ $# -lt 2 ]]; then
        echo "--build-dir requires a value" >&2
        exit 2
      fi
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-dir=*)
      BUILD_DIR="${1#*=}"
      shift
      ;;
    -j|--jobs)
      if [[ $# -lt 2 ]]; then
        echo "$1 requires a value" >&2
        exit 2
      fi
      JOBS="$2"
      shift 2
      ;;
    --jobs=*)
      JOBS="${1#*=}"
      shift
      ;;
    --no-tests)
      BUILD_TESTS="OFF"
      shift
      ;;
    --configure-only)
      CONFIGURE_ONLY="ON"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

cd "${ROOT_DIR}"

if [[ "${BUILD_TESTS}" == "ON" ]]; then
  DEPENDENCY_SUBMODULES+=(external/Catch2)
fi

mkdir -p external

registered_submodules=()
missing_registrations=()
for submodule in "${DEPENDENCY_SUBMODULES[@]}"; do
  if git submodule status "${submodule}" >/dev/null 2>&1; then
    registered_submodules+=("${submodule}")
  elif [[ ! -f "${ROOT_DIR}/${submodule}/CMakeLists.txt" ]]; then
    missing_registrations+=("${submodule}")
  fi
done

if [[ ${#registered_submodules[@]} -gt 0 ]]; then
  git submodule sync --recursive "${registered_submodules[@]}"
  git submodule update --init --recursive "${registered_submodules[@]}"
fi

missing_sources=()
for submodule in "${DEPENDENCY_SUBMODULES[@]}"; do
  if [[ ! -f "${ROOT_DIR}/${submodule}/CMakeLists.txt" ]]; then
    missing_sources+=("${submodule}")
  fi
done

if [[ ${#missing_sources[@]} -gt 0 ]]; then
  {
    echo "Missing dependency source directories:"
    printf '  %s\n' "${missing_sources[@]}"
    echo
    if [[ ${#missing_registrations[@]} -gt 0 ]]; then
      echo "These paths are not registered as git submodules in this checkout:"
      printf '  %s\n' "${missing_registrations[@]}"
      echo
      echo "Register them in the repo, then rerun:"
    else
      echo "Initialize the dependency submodules, then rerun:"
    fi
    printf '  git submodule update --init --recursive'
    printf ' %q' "${DEPENDENCY_SUBMODULES[@]}"
    printf '\n'
  } >&2
  exit 1
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DREVOBASE_BUILD_TESTS="${BUILD_TESTS}"

if [[ "${CONFIGURE_ONLY}" == "ON" ]]; then
  exit 0
fi

build_args=("--build" "${BUILD_DIR}")
if [[ -n "${JOBS}" ]]; then
  build_args+=("--parallel" "${JOBS}")
fi
cmake "${build_args[@]}"

if [[ "${RUN_TESTS}" == "ON" ]]; then
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi
