#!/usr/bin/env bash
# Regenerate the rdma-core patches that ship our libibverbs provider.
#
# Pulls a clean rdma-core source tarball (the version nixpkgs is pinned
# to), checks in our provider's source files, and writes two numbered
# patches into ./packaging/rdma-core-patches/ that any rdma-core build
# can apply. Run this after touching userspace/usb4_rdma/ files.

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO_ROOT/packaging/rdma-core-patches"

# An explicit clean source directory/tarball permits regeneration without Nix.
if [ "$#" -gt 1 ]; then
    echo "Usage: $0 [clean-rdma-core-source-directory-or-tarball]" >&2
    exit 2
fi
if [ "$#" -eq 1 ]; then
    RDMA_SRC=$1
else
    RDMA_SRC=$(nix build --no-link --print-out-paths \
        "$REPO_ROOT#packages.x86_64-linux.rdma-core-usb4.src")
fi
if [ ! -e "$RDMA_SRC" ]; then
    echo "Missing rdma-core source: $RDMA_SRC" >&2
    exit 1
fi
echo "Using rdma-core source: $RDMA_SRC"

WORK=$(mktemp -d -t rdma-patches-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

if [ -d "$RDMA_SRC" ]; then
    cp -r "$RDMA_SRC"/. "$WORK/"
else
    tar -xf "$RDMA_SRC" -C "$WORK" --strip-components=1
fi
if [ ! -f "$WORK/libibverbs/verbs.h" ] || [ -d "$WORK/providers/usb4_rdma" ]; then
    echo "Expected a clean rdma-core source tree" >&2
    exit 1
fi
chmod -R u+w "$WORK"
cd "$WORK"

git init -q
git config user.email "ci@thunderbolt-ibverbs"
git config user.name "thunderbolt-ibverbs"
git config commit.gpgsign false
git add -A
git commit -qm "rdma-core baseline"

# Drop our provider source in.
mkdir -p providers/usb4_rdma
cp -r "$REPO_ROOT/userspace/usb4_rdma/." providers/usb4_rdma/
git add providers/usb4_rdma
git commit -qm "providers/usb4_rdma: add USB4 soft-RDMA provider

Out-of-tree provider for the usb4_rdma kernel module which exposes
Thunderbolt/USB4 host-to-host xdomain links as InfiniBand verbs
devices.

Source: https://github.com/hellas-ai/thunderbolt-ibverbs"

# Wire the provider into the build.
awk '{ print; if ($0 == "add_subdirectory(providers/siw)")
    print "add_subdirectory(providers/usb4_rdma)" }' CMakeLists.txt > CMakeLists.txt.new
mv CMakeLists.txt.new CMakeLists.txt
git add CMakeLists.txt
git commit -qm "CMakeLists.txt: build the usb4_rdma provider"

# Declare the provider in the public header so the static-link
# all_providers.c indirection sees it. rdma-core hand-maintains this
# list — every in-tree provider has an extern in libibverbs/verbs.h.
awk '{ print; if (index($0, "extern const struct verbs_device_ops verbs_provider_siw;"))
    print "extern const struct verbs_device_ops verbs_provider_usb4_rdma;" }' libibverbs/verbs.h > libibverbs/verbs.h.new
mv libibverbs/verbs.h.new libibverbs/verbs.h
git add libibverbs/verbs.h
git commit -qm "libibverbs/verbs.h: declare verbs_provider_usb4_rdma

Required for the static-archive build path (libibverbs/all_providers.c)
to compile when ENABLE_STATIC=1 is on, which is the default on Debian
and most distros."

mkdir -p "$OUT"
rm -f "$OUT"/*.patch
git format-patch -o "$OUT" HEAD~3

echo ""
echo "Regenerated patches in $OUT:"
ls -la "$OUT"/*.patch
