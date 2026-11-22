#!/bin/bash
# usb4-h2h.sh: AMD's "USB4 H2H Enhancement" (host-to-host) on Strix Halo, applied at
# runtime with bos_usb4h2h.ko. Adapted from StrixFold's tools/m5-usb4 for this driver.
# Run as root on BOTH hosts of a pair. See README.md in this directory.
#
#   usb4-h2h.sh status        each cable's partner, lane speed and lane count
#   usb4-h2h.sh apply [KO]    send the setting (enable=1) and restart both USB4 routers
#   usb4-h2h.sh disable [KO]  send the BIOS's "Disabled" value (enable=0) and restart them
#
# Without KO (or USB4_H2H_KO) the module is loaded with modprobe, i.e. the DKMS
# package built from this directory (dkms.conf); with KO, that .ko file is insmod'ed.
#
# The routers read the setting only while they start, so apply unloads
# thunderbolt_ibverbs / thunderbolt_net / thunderbolt, sends the SMU message,
# resets both NHIs through ACPI and loads back whatever was loaded before.
# The cables drop for about 10-20 s. A reboot clears the setting.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
KO=${2:-${USB4_H2H_KO:-}}
die() { echo "usb4-h2h: $*" >&2; exit 2; }

# The SMU router instances in bos_usb4h2h.c (0x1500003 / 0x1700003) were taken from
# the BIOS of the Sixunited AXB35-02 board (Bosgame M5). Other Strix Halo boards are
# unverified: set USB4_H2H_ANY_BOARD=1 to try anyway.
board=$(cat /sys/class/dmi/id/board_name 2>/dev/null || true)
[ "$board" = AXB35-02 ] || [ "${USB4_H2H_ANY_BOARD:-0}" = 1 ] ||
  die "board '$board' is not AXB35-02 (set USB4_H2H_ANY_BOARD=1 to override)"

# Strix Halo USB4 host routers: AMD 1022:158d (domain 0) and 1022:158e (domain 1).
nhis() {
  for d in /sys/bus/pci/devices/*; do
    [ "$(cat "$d/vendor")" = 0x1022 ] || continue
    case $(cat "$d/device") in 0x158d|0x158e) basename "$d" ;; esac
  done
}

speeds() {
  for p in /sys/bus/thunderbolt/devices/[0-9]-[1-9]; do
    [ -e "$p/rx_speed" ] || continue
    echo "$(basename "$p"): $(cat "$p/device_name" 2>/dev/null || echo '?') $(cat "$p/rx_speed") x$(cat "$p/rx_lanes")"
  done
}

awake() {
  for p in $(nhis); do echo on > "/sys/bus/pci/devices/$p/power/control"; done
  for r in /sys/bus/thunderbolt/devices/[0-9]-0; do
    [ -e "$r/power/control" ] && echo on > "$r/power/control"
  done
  return 0
}

loaded() { grep -q "^$1 " /proc/modules; }

reset_routers() {
  for p in $(nhis); do
    d=/sys/bus/pci/devices/$p
    was=$(cat "$d/reset_method")
    echo acpi > "$d/reset_method"
    echo 1 > "$d/reset"
    echo "$was" > "$d/reset_method"
    echo "$p reset"
  done
}

wait_partners() {  # $1 = seconds; true once both cables have a partner
  local n
  for _ in $(seq 1 "$1"); do
    n=$(compgen -G '/sys/bus/thunderbolt/devices/[0-9]-[1-9]' | wc -l || true)
    [ "$n" -ge 2 ] && return 0
    sleep 1
  done
  return 1
}

send() {  # $1 = enable value
  if [ -n "$KO" ]; then
    [ -r "$KO" ] || die "$KO not found (build and sign it first, README.md)"
  else
    modinfo bos_usb4h2h >/dev/null 2>&1 || die "bos_usb4h2h is not installed for $(uname -r) (dkms status, README.md)"
  fi
  local nh; nh=$(nhis | wc -l)
  [ "$nh" -eq 2 ] || die "expected 2 Strix Halo NHIs, found $nh"
  [ "$(id -u)" = 0 ] || die "run as root"

  local reload=()
  for m in thunderbolt_ibverbs thunderbolt_net; do loaded "$m" && reload+=("$m"); done
  # shellcheck disable=SC2064
  trap "modprobe thunderbolt || true; for m in ${reload[*]:-}; do modprobe --ignore-install \$m || true; done; awake || true" EXIT

  # rmmod, not modprobe -r: that would also try to remove ib_uverbs & co. and fail
  for m in thunderbolt_ibverbs thunderbolt_net thunderbolt_stream thunderbolt; do
    if loaded "$m"; then rmmod "$m"; fi
  done
  if [ -n "$KO" ]; then insmod "$KO" enable="$1"; else modprobe bos_usb4h2h enable="$1"; fi
  dmesg | grep bos_usb4h2h | tail -3 | cut -c16-200
  rmmod bos_usb4h2h
  [ "$(dmesg | grep bos_usb4h2h | tail -2 | grep -c 'USB config update.*response 0x1,')" = 2 ] ||
    die "the SMU did not accept the message (see dmesg); routers not reset"

  # If both hosts reset at nearly the same time (both booting), the links can stay
  # down; another reset of this side brings them back. The setting survives it.
  local try
  for try in 1 2 3; do
    [ "$try" = 1 ] || { echo "no partner on both cables; reset again ($try/3)"
      for m in thunderbolt_net thunderbolt_stream thunderbolt; do
        if loaded "$m"; then rmmod "$m"; fi
      done; }
    reset_routers
    modprobe thunderbolt
    awake
    wait_partners 30 && break
  done
  sleep 3
  for m in "${reload[@]}"; do modprobe --ignore-install "$m"; done
  trap - EXIT
  speeds
}

case "${1:-status}" in
  status)  speeds ;;
  apply)   send 1 ;;
  disable) send 0 ;;
  *) die "unknown command '${1}' (status, apply, disable)" ;;
esac
