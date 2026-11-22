#!/bin/bash
# build-dkms.sh: build and install a patched thunderbolt.ko for one kernel through DKMS.
#
#   sudo tools/thunderbolt-ringfix/build-dkms.sh [--src DIR] [--out DIR] [KVER]
#
# Takes drivers/thunderbolt of that kernel version (the kernel.org tarball, or --src DIR
# for a distribution that patches it), applies patches/*.patch, and installs the result
# as DKMS package thunderbolt-ringfix/<version>.<REV>, built only for kernels of that
# version (not +debug). DKMS moves the stock thunderbolt.ko aside ("Original modules
# exist"); `dkms remove thunderbolt-ringfix/<version> --all` puts it back.
# --out DIR only writes the package source to DIR (no DKMS, no root needed).
# See README.md in this directory.
set -euo pipefail
REV=2  # fix revision: 1 = patches 0001-0002 only, 2 = with 0003 (clear + flush in ring_work())
here=$(cd "$(dirname "$0")" && pwd)
die() { echo "build-dkms: $*" >&2; exit 2; }

src="" out="" kver=""
while [ $# -gt 0 ]; do
  case $1 in
    --src) src=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    -*) die "unknown option $1" ;;
    *) kver=$1; shift ;;
  esac
done
kver=${kver:-$(uname -r)}
case $kver in *debug*) die "$kver is a debug kernel; not supported" ;; esac
base=$(echo "$kver" | grep -oE '^[0-9]+\.[0-9]+(\.[0-9]+)?') || die "cannot parse version from $kver"
case $base in *.*.*) ;; *) base=$base.0 ;; esac
tarver=${base%.0}          # kernel.org names 7.3.0 "linux-7.3"
pkgver=$base.$REV
[ -n "$out" ] || [ "$(id -u)" = 0 ] || die "run as root (or use --out DIR)"

work=$(mktemp -d "${TMPDIR:-/var/tmp}/thunderbolt-ringfix.XXXXXX")
trap 'rm -rf "$work"' EXIT
if [ -z "$src" ]; then
  url=https://cdn.kernel.org/pub/linux/kernel/v${base%%.*}.x/linux-$tarver.tar.xz
  echo "fetching $url"
  curl -fsSL --retry 3 -o "$work/linux.tar.xz" "$url" || die "download failed: $url"
  tar -C "$work" -xf "$work/linux.tar.xz" --wildcards "linux-$tarver/drivers/thunderbolt/*"
  src=$work/linux-$tarver/drivers/thunderbolt
fi
[ -f "$src/nhi.c" ] && [ -f "$src/Makefile" ] || die "$src is not a drivers/thunderbolt directory"

pkg=$work/thunderbolt-ringfix-$pkgver
mkdir "$pkg"
cp -a "$src"/. "$pkg"/
for p in "$here"/patches/*.patch; do
  if patch -d "$pkg" -p3 -s --dry-run -R -f < "$p" >/dev/null 2>&1; then
    echo "$(basename "$p"): already in this kernel, skipped"
    continue
  fi
  patch -d "$pkg" -p3 -s --dry-run -f < "$p" >/dev/null ||
    die "$(basename "$p") does not apply to $kver's drivers/thunderbolt"
  patch -d "$pkg" -p3 -s -f < "$p"
  echo "$(basename "$p"): applied"
done
find "$pkg" -name '*.orig' -delete

cat > "$pkg/dkms.conf" <<EOF
# drivers/thunderbolt of Linux $base plus the patches in tools/thunderbolt-ringfix
# (thunderbolt-ibverbs repository), fix revision $REV. Built only for $base kernels
# (not +debug); any other kernel keeps its stock module.
PACKAGE_NAME="thunderbolt-ringfix"
PACKAGE_VERSION="$pkgver"
BUILT_MODULE_NAME[0]="thunderbolt"
DEST_MODULE_LOCATION[0]="/updates"
MAKE[0]="make -C \${kernel_source_dir} M=\${dkms_tree}/\${PACKAGE_NAME}/\${PACKAGE_VERSION}/build CONFIG_USB4=m modules"
BUILD_EXCLUSIVE_KERNEL="^${base//./\\.}-[^+]*\$"
AUTOINSTALL="yes"
EOF

if [ -n "$out" ]; then
  mkdir -p "$out"
  rm -rf "${out:?}/thunderbolt-ringfix-$pkgver"
  cp -a "$pkg" "$out/"
  echo "wrote $out/thunderbolt-ringfix-$pkgver"
  exit 0
fi

if dkms status "thunderbolt-ringfix/$pkgver" -k "$kver" 2>/dev/null | grep -q installed; then
  echo "thunderbolt-ringfix/$pkgver is already installed for $kver"
  exit 0
fi
dkms status "thunderbolt-ringfix/$pkgver" 2>/dev/null | grep -q . &&
  dkms remove "thunderbolt-ringfix/$pkgver" --all >/dev/null
rm -rf "/usr/src/thunderbolt-ringfix-$pkgver"
cp -a "$pkg" /usr/src/
chown -R root:root "/usr/src/thunderbolt-ringfix-$pkgver"
dkms add "thunderbolt-ringfix/$pkgver"
dkms install "thunderbolt-ringfix/$pkgver" -k "$kver"
echo "installed: $(modinfo -k "$kver" -n thunderbolt)"
echo "Load it with a reboot (the initramfs holds a copy too), or unload thunderbolt_ibverbs,"
echo "thunderbolt_net and thunderbolt on both hosts and modprobe thunderbolt."
