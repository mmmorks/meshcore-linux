#!/usr/bin/env bash
# build-docker.sh - build a MeshCore Linux firmware in an arm64 Debian container.
#
# The linux_repeater target compiles natively against libgpiod/bluez/libuv and
# only builds on Linux, so this runs the build inside a linux/arm64 container
# (matching a Raspberry Pi). On Apple Silicon this is native and fast; on Intel
# it emulates aarch64 via QEMU (works, but slow).
#
# Usage:
#   ./build-docker.sh                 # builds env "linux_repeater"
#   ./build-docker.sh linux           # builds a different env
#   FIRMWARE_VERSION=1.0 ./build-docker.sh
#
# Output: .pio/build/<env>/meshcored   (written into this repo via the mount)
# PlatformIO's packages are cached in the "mc_pio_cache" Docker volume so repeat
# builds skip the re-download.
set -euo pipefail

ENV_NAME="${1:-linux_repeater}"
FIRMWARE_VERSION="${FIRMWARE_VERSION:-dev}"
IMAGE="debian:bookworm"
CACHE_VOL="mc_pio_cache"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v docker >/dev/null 2>&1; then
  echo "build-docker.sh: docker not found on PATH" >&2
  exit 1
fi

echo ">> building env '${ENV_NAME}' (version '${FIRMWARE_VERSION}') in ${IMAGE} [linux/arm64]"

docker run --rm -it \
  --platform linux/arm64 \
  -v "${REPO_DIR}":/src -w /src \
  -v "${CACHE_VOL}":/root/.platformio \
  -e "FIRMWARE_VERSION=${FIRMWARE_VERSION}" \
  -e "ENV_NAME=${ENV_NAME}" \
  "${IMAGE}" bash -c '
    set -e
    apt-get update
    apt-get install -y --no-install-recommends \
      build-essential git python3 python3-venv \
      pkg-config libgpiod-dev libi2c-dev libbluetooth-dev libuv1-dev
    python3 -m venv /pio
    . /pio/bin/activate
    pip install --quiet --upgrade platformio
    pio run -e "${ENV_NAME}"
  '

echo ">> done: .pio/build/${ENV_NAME}/meshcored"
