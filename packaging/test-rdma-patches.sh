#!/usr/bin/env bash
# Smoke-test the rdma-core patches on a clean distro container.
#
# Uses the locked rdma-core source, applies our complete patch series, runs
# the distro-native rdma-core CMake build, verifies that
# `libusb4_rdma-rdmav*.so` and the `usb4_rdma.driver` config are produced.
# Does not install or load anything — just confirms patches apply and
# build succeeds in the distro's toolchain.
#
# Usage:
#   ./packaging/test-rdma-patches.sh                # debian:sid (default)
#   ./packaging/test-rdma-patches.sh ubuntu:24.04
#   ./packaging/test-rdma-patches.sh ubuntu:26.04
#   ./packaging/test-rdma-patches.sh archlinux:latest
#   ./packaging/test-rdma-patches.sh fedora:rawhide

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${1:-debian:sid}
DOCKER=${DOCKER:-docker}

# RDMA_CORE_TAG explicitly selects another upstream version for compatibility
# testing. The default shares the source used by Nix and patch regeneration.
RDMA_CORE_TAG=${RDMA_CORE_TAG:-}

STAGE=$(mktemp -d -t tb-ibv-rdma-patches-XXXXXX)
trap 'rm -rf "$STAGE"' EXIT
cp -r "$REPO_ROOT/packaging/rdma-core-patches" "$STAGE/patches"
cp -r "$REPO_ROOT/userspace/usb4_rdma" "$STAGE/provider"

if [[ -z "$RDMA_CORE_TAG" ]]; then
    RDMA_SRC=${RDMA_SRC:-$(nix build --no-link --print-out-paths "$REPO_ROOT#packages.x86_64-linux.rdma-core-source")}
    mkdir "$STAGE/upstream"
    if [[ -d "$RDMA_SRC" ]]; then
        cp -r "$RDMA_SRC"/. "$STAGE/upstream/"
    else
        tar -xf "$RDMA_SRC" -C "$STAGE/upstream" --strip-components=1
    fi
    [[ -f "$STAGE/upstream/CMakeLists.txt" && -f "$STAGE/upstream/libibverbs/verbs.h" ]]
fi

echo "=== Testing rdma-core ${RDMA_CORE_TAG:-locked source} + our patches on $IMAGE ==="
echo ""

$DOCKER run --rm \
    -v "$STAGE:/work:ro" \
    -e RDMA_CORE_TAG="$RDMA_CORE_TAG" \
    "$IMAGE" bash -euxc '
        if command -v apt-get >/dev/null; then
            apt-get update -qq
            DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
                git ca-certificates cmake build-essential pkg-config \
                libnl-3-dev libnl-route-3-dev libudev-dev libsystemd-dev \
                python3-pyelftools python3-docutils libssl-dev libcap-dev \
                ninja-build patch
        elif command -v pacman >/dev/null; then
            pacman -Sy --noconfirm --quiet \
                base-devel git cmake \
                libnl systemd python-pyelftools python-docutils \
                ninja >/dev/null
        elif command -v dnf >/dev/null; then
            dnf install -y -q \
                git cmake gcc gcc-c++ make pkgconf \
                libnl3-devel libudev-devel systemd-devel \
                python3-pyelftools python3-docutils openssl-devel libcap-devel \
                ninja-build patch
        else
            echo "Unknown distro" >&2
            exit 1
        fi

        cd /tmp
        if [[ -d /work/upstream ]]; then
            cp -r /work/upstream rdma-core
            chmod -R u+w rdma-core
        else
            git clone --depth=1 --branch "$RDMA_CORE_TAG" https://github.com/linux-rdma/rdma-core
        fi
        cd rdma-core
        while IFS= read -r name; do
            [ -n "$name" ] || continue
            patch -p1 < "/work/patches/$name"
        done < /work/patches/series
        git diff --no-index --exit-code /work/provider providers/usb4_rdma

        mkdir build && cd build
        cmake -GNinja -DNO_PYVERBS=1 -DENABLE_STATIC=1 ..
        ninja

        echo ""
        echo "=== Verify our provider was built ==="
        ls -l lib/libusb4_rdma-rdmav*.so
        ls -l etc/libibverbs.d/usb4_rdma.driver
        cat etc/libibverbs.d/usb4_rdma.driver

        echo "==> rdma-core+patches build OK on $(grep PRETTY_NAME /etc/os-release | cut -d= -f2)"
    '

echo ""
echo "rdma-core patches verified on $IMAGE."
