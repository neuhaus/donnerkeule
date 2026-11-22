#!/bin/bash
# tbswap.sh stock|fixed: swap thunderbolt.ko on both hosts without a router reset
set -eu
K=7.3.4-100.fc43.x86_64
case $1 in stock) ko=/var/lib/dkms/thunderbolt-ringfix/original_module/$K/x86_64/thunderbolt.ko.xz ;; fixed) ko=/lib/modules/$K/extra/thunderbolt.ko.xz ;; esac
for h in fuzzy misty; do ssh $h "sudo rmmod thunderbolt_ibverbs 2>/dev/null; sudo rmmod thunderbolt; sudo insmod $ko && echo \"\$(hostname) loaded $1 taint=\$(cat /sys/module/thunderbolt/taint)\"" & done; wait
sleep 8
for h in fuzzy misty; do ssh $h 'for p in /sys/bus/thunderbolt/devices/[0-9]-2; do echo "  $(hostname) $(basename $p) $(cat $p/rx_speed) x$(cat $p/rx_lanes)"; done'; done
