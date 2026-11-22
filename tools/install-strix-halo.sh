#!/bin/bash
# install-strix-halo.sh: install everything a Strix Halo host pair needs, through DKMS.
#
#   sudo tools/install-strix-halo.sh [--kver KVER] [--no-ringfix] [--no-h2h]
#
#   1. thunderbolt_ibverbs from this repository (make dkms-add/-build/-install)
#   2. thunderbolt-ringfix: the in-tree thunderbolt.ko with two fixes that are not
#      upstream yet (tools/thunderbolt-ringfix; downloads the kernel's sources)
#   3. USB4 H2H (tools/usb4-h2h): only on the Sixunited AXB35-02 board (Bosgame M5);
#      installs the module, /usr/local/sbin/usb4-h2h and usb4-h2h.service (enabled),
#      after the SMU answers its test message
#
# It does not touch /etc/modprobe.d, the tbv0 netdev or udev rules (README.md,
# step 3) and does not load anything: reboot afterwards. Run it on both hosts.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
die() { echo "install-strix-halo: $*" >&2; exit 2; }
step() { echo; echo "== $*"; }

kver=$(uname -r) ringfix=1 h2h=1
while [ $# -gt 0 ]; do
  case $1 in
    --kver) kver=$2; shift 2 ;;
    --no-ringfix) ringfix=0; shift ;;
    --no-h2h) h2h=0; shift ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) die "unknown option $1" ;;
  esac
done
[ "$(id -u)" = 0 ] || die "run as root"
command -v dkms >/dev/null || die "dkms is not installed (README.md, step 1)"
[ -d "/lib/modules/$kver/build" ] || die "no kernel headers for $kver (README.md, step 1)"

step "thunderbolt_ibverbs"
cd "$repo"
name=$(awk -F '"' '/^PACKAGE_NAME=/ { print $2; exit }' dkms.conf)
ver=$(awk -F '"' '/^PACKAGE_VERSION=/ { print $2; exit }' dkms.conf)
dkms status "$name/$ver" 2>/dev/null | grep -q . || make dkms-add
make dkms-build KVER="$kver"
make dkms-install KVER="$kver"

if [ "$ringfix" = 1 ]; then
  step "thunderbolt-ringfix"
  "$repo/tools/thunderbolt-ringfix/build-dkms.sh" "$kver"
fi

if [ "$h2h" = 1 ]; then
  step "USB4 H2H"
  board=$(cat /sys/class/dmi/id/board_name 2>/dev/null || true)
  if [ "$board" != AXB35-02 ]; then
    echo "board '$board' is not AXB35-02: skipped (tools/usb4-h2h/README.md)"
  else
    t=$repo/tools/usb4-h2h
    hver=$(awk -F '"' '/^PACKAGE_VERSION=/ { print $2; exit }' "$t/dkms.conf")
    dkms status "usb4-h2h/$hver" 2>/dev/null | grep -q . && dkms remove "usb4-h2h/$hver" --all >/dev/null
    rm -rf "/usr/src/usb4-h2h-$hver"
    install -d -m 755 "/usr/src/usb4-h2h-$hver"
    install -m 644 "$t/bos_usb4h2h.c" "$t/Makefile" "$t/dkms.conf" "/usr/src/usb4-h2h-$hver/"
    dkms install "usb4-h2h/$hver" -k "$kver"
    if [ "$kver" = "$(uname -r)" ]; then
      modprobe bos_usb4h2h test_only=1
      line=$(dmesg | grep 'bos_usb4h2h: SMU test message' | tail -1)
      rmmod bos_usb4h2h
      echo "$line" | grep -q 'response 0x1, arg0 0x1234 -> 0x1235' ||
        die "the SMU did not answer the test message as expected; H2H not enabled: $line"
      echo "SMU test message: ok"
    fi
    install -m 755 "$t/usb4-h2h.sh" /usr/local/sbin/usb4-h2h
    install -m 644 "$t/usb4-h2h.service" /etc/systemd/system/usb4-h2h.service
    install -D -m 644 "$t/README.md" /usr/local/share/doc/usb4-h2h/README.md
    systemctl daemon-reload
    systemctl enable usb4-h2h.service
  fi
fi

step "done"
dkms status | grep -E "thunderbolt-ibverbs|thunderbolt-ringfix|usb4-h2h" || true
cat <<EOF

Next: configure modprobe.d, tbv0 and udev if not done yet (README.md, step 3),
then reboot both hosts. After the reboot check:
  usb4-h2h status; journalctl -b -u usb4-h2h     (links at 20.0 Gb/s x2, response 0x1)
  modinfo -n thunderbolt                          (.../extra/thunderbolt.ko.xz with the ringfix)
  ibv_devices                                     (four usb4_rdma* devices)
EOF
