#!/bin/bash
# Reproduces the 2026-11-21 dk14 README sweep (DMA-BUF then native), tagged.
# usage: sweep.sh TAG "LABEL SUFFIX" OUTDIR
set -euo pipefail
tag=$1; suf=$2; out=$3; mkdir -p "$out"
cd "$(git rev-parse --show-toplevel)"
scp -q bench/sweep_perftest.py misty:/tmp/sweep_perftest.py
load() { for h in fuzzy misty; do ssh "$h" 'sudo modprobe -r thunderbolt_ibverbs 2>/dev/null || true'; done
  for h in fuzzy misty; do ssh "$h" "sudo modprobe --ignore-install thunderbolt_ibverbs roce_netdev=tbv0 $1" & done; wait; sleep 12
  for h in fuzzy misty; do n=$(ssh "$h" 'ls /sys/class/infiniband | wc -l'); echo "$h rails=$n"; [ "$n" = 4 ] || { echo "rails missing on $h"; exit 1; }; done; }
speeds() { for h in fuzzy misty; do ssh "$h" 'for p in /sys/bus/thunderbolt/devices/[0-9]-2; do echo "$(hostname) $(basename $p) $(cat $p/rx_speed) x$(cat $p/rx_lanes)"; done'; done; }
speeds | tee "$out/$tag-links.txt"
load ""
ssh misty "rm -f /tmp/$tag-dmabuf.csv && python3 /tmp/sweep_perftest.py --server fuzzy --dev usb4_rdma0 --gid 1 --seconds 4 \
  --label 'usb4_rdma DMA-BUF verbs, 2 cables$suf' --csv /tmp/$tag-dmabuf.csv --max-size ib_read_bw=0 --max-size ib_read_lat=0"
load "dmabuf_verbs=0"
ssh misty "rm -f /tmp/$tag-native.csv && python3 /tmp/sweep_perftest.py --server fuzzy --dev usb4_rdma0 --gid 1 --seconds 4 \
  --label 'usb4_rdma native, 2 cables (write striping)$suf' --csv /tmp/$tag-native.csv --max-size ib_send_bw=524288"
ssh misty "cat /tmp/$tag-dmabuf.csv" > "$out/$tag-dmabuf.csv"
ssh misty "cat /tmp/$tag-native.csv" > "$out/$tag-native.csv"
for h in fuzzy misty; do ssh "$h" 'sudo dmesg | grep -i -E "timeout|retransmit|error|fail" | grep -v -E "ib_srpt|MAD agent" | tail -5' > "$out/$tag-dmesg-$h.txt" || true; done
load ""
speeds | tee -a "$out/$tag-links.txt"
echo "sweep $tag done"
