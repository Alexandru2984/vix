#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
GENERATOR="${CMAKE_GENERATOR:-}"

if [[ -z "${GENERATOR}" ]]; then
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
  else
    GENERATOR="Unix Makefiles"
  fi
fi

if [[ "${BUILD_DIR}" != /* ]]; then
  BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}"
fi

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  EXISTING_GENERATOR="$(grep -E '^CMAKE_GENERATOR:INTERNAL=' "${BUILD_DIR}/CMakeCache.txt" | cut -d= -f2- || true)"
  if [[ -n "${EXISTING_GENERATOR}" && "${EXISTING_GENERATOR}" != "${GENERATOR}" ]]; then
    echo "Reconfiguring ${BUILD_DIR}: cached generator is '${EXISTING_GENERATOR}', requested '${GENERATOR}'." >&2
    rm -rf "${BUILD_DIR}"
  fi
fi

CONFIGURE_ARGS=(
  -S "${ROOT_DIR}"
  -B "${BUILD_DIR}"
  -G "${GENERATOR}"
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)

if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
  CONFIGURE_ARGS+=("-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
elif [[ -d "${HOME:-}/.local" ]]; then
  CONFIGURE_ARGS+=("-DCMAKE_PREFIX_PATH=${HOME}/.local")
fi

cmake "${CONFIGURE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --parallel "${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"
