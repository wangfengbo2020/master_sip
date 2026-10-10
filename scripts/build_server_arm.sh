#!/usr/bin/env bash
set -euo pipefail

# Build the SIP/PTT server directly with the target machine's native compiler.
# This is intended for the ARM64 Alibaba Cloud VM at /root/master and does not
# require CMake or any third-party library.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Wpedantic}"

mkdir -p "${BUILD_DIR}"

echo "Project : ${PROJECT_DIR}"
echo "Compiler: ${CC}"
echo "Target  : $(uname -m)"

"${CC}" ${CFLAGS} \
    -I"${PROJECT_DIR}/native/include" \
    "${PROJECT_DIR}/native/src/ptt_common.c" \
    "${PROJECT_DIR}/apps/server/ptt_server.c" \
    -o "${BUILD_DIR}/ptt_server"

echo "Built   : ${BUILD_DIR}/ptt_server"
file "${BUILD_DIR}/ptt_server" 2>/dev/null || true

