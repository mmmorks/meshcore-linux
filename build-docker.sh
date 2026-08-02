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
#   FIRMWARE_VERSION=1.0 ./build-docker.sh
#   BASE_IMAGE=debian:trixie ./build-docker.sh linux_repeater  # libgpiod v2
#
# Output: <build-dir>/<env>/meshcored   (written into this repo via the mount)
# PlatformIO's packages are cached in a "mc_pio_cache*" Docker volume so repeat
# builds skip the re-download.
#
# BASE_IMAGE selects the container's libgpiod major version: bookworm ships
# libgpiod 1.x, trixie ships 2.x. Default is bookworm, so existing behaviour
# is unchanged unless BASE_IMAGE is set.
#
# PlatformIO's dependency scanner doesn't track system headers like
# /usr/include/gpiod.h, so reusing one build dir across libgpiod major
# versions can silently relink objects compiled against the other version's
# headers. To avoid that, the package cache and build dir are namespaced by
# BASE_IMAGE: the default (bookworm) keeps the original untagged paths
# (cache volume "mc_pio_cache", build dir ".pio/build") so existing caches
# and tooling keep working; any other BASE_IMAGE gets its own
# "mc_pio_cache_<tag>" volume and ".pio/build-<tag>" dir, e.g. trixie builds
# land in ".pio/build-trixie/linux_repeater/meshcored".
set -euo pipefail

ENV_NAME="${1:-linux_repeater}"
FIRMWARE_VERSION="${FIRMWARE_VERSION:-dev}"
IMAGE="${BASE_IMAGE:-debian:bookworm}"

if [ "${IMAGE}" = "debian:bookworm" ]; then
  CACHE_VOL="mc_pio_cache"
  BUILD_DIR=".pio/build"
else
  # Derive a filesystem/volume-name-safe tag from the image reference. Strip
  # to the part after the last ':' (the tag; also correct for a
  # registry:port/name:tag reference, since ## takes the longest match), then
  # drop any remaining path component (e.g. a tagless "myregistry.io/debian"
  # reference, where the whole string would otherwise land here), then
  # replace anything that isn't safe in a Docker volume name or directory
  # name with '_'.
  TAG="${IMAGE##*:}"
  TAG="${TAG##*/}"
  TAG="${TAG//[^A-Za-z0-9._-]/_}"
  CACHE_VOL="mc_pio_cache_${TAG}"
  BUILD_DIR=".pio/build-${TAG}"
fi

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v docker >/dev/null 2>&1; then
  echo "build-docker.sh: docker not found on PATH" >&2
  exit 1
fi

echo ">> building env '${ENV_NAME}' (version '${FIRMWARE_VERSION}') in ${IMAGE} [linux/arm64] -> ${BUILD_DIR}"

# No -it: a TTY is not needed for a batch build, and requesting one makes the
# script fail outright when stdin is not a terminal (CI, agents, `| tee`).
#
# The container itself still runs as root -- apt-get needs it, and there is no
# host-matching user account inside the image for `--user "$(id -u):$(id -g)"`
# to resolve against without extra setup. Root only ever writes into one thing
# host-visible through the bind mount: /src/.pio (BUILD_DIR plus the
# non-namespaced libdeps/ cache PlatformIO keeps alongside it) -- everything
# else root touches (apt/pip state, the venv, PlatformIO's own package cache
# in CACHE_VOL) lives in the container's own filesystem or a Docker-managed
# volume, neither host-visible. So chowning .pio back to the invoking user
# after the build is equivalent to running unprivileged for the one directory
# that would otherwise leave root-owned files on a Linux host; it is a no-op
# on macOS, where Docker Desktop's bind-mount layer does not carry container
# UIDs through to the host anyway.
docker run --rm \
  --platform linux/arm64 \
  -v "${REPO_DIR}":/src -w /src \
  -v "${CACHE_VOL}":/root/.platformio \
  -e "FIRMWARE_VERSION=${FIRMWARE_VERSION}" \
  -e "ENV_NAME=${ENV_NAME}" \
  -e "PLATFORMIO_BUILD_DIR=${BUILD_DIR}" \
  -e "HOST_UID=$(id -u)" \
  -e "HOST_GID=$(id -g)" \
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
    chown -R "${HOST_UID}:${HOST_GID}" .pio
  '

echo ">> done: ${BUILD_DIR}/${ENV_NAME}/meshcored"
