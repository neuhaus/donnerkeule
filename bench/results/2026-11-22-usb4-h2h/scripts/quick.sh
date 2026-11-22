#!/bin/bash
# quick.sh TAG OUTDIR: short A/B set, both modes
set -euo pipefail
tag=$1; out=$2; mkdir -p "$out"
cd "$(git rev-parse --show-toplevel)"
scp -q bench/sweep_perftest.py misty:/tmp/sweep_perftest.py
load() { for h in fuzzy misty; do ssh "$h" 'sudo modprobe -r thunderbolt_ibverbs 2>/dev/null || true'; done
  for h in fuzzy misty; do ssh "$h" "sudo modprobe --ignore-install thunderbolt_ibverbs roce_netdev=tbv0 $1" & done; wait; sleep 12
  for h in fuzzy misty; do n=$(ssh "$h" 'ls /sys/class/infiniband | wc -l'); [ "$n" = 4 ] || { echo "rails missing on $h"; exit 1; }; done; }
sw() { local c=$1 l=$2; shift 2
  timeout 240 ssh misty "python3 /tmp/sweep_perftest.py --server fuzzy --dev usb4_rdma0 --gid 1 --seconds 4 --label '$l' --csv /tmp/$c $*"; }
ssh misty "rm -f /tmp/q-$tag.csv"
load ""
sw q-$tag.csv "DMA-BUF $tag" --only ib_write_bw --sizes 4194304
load "dmabuf_verbs=0"
sw q-$tag.csv "native $tag" --only ib_read_bw --sizes 1048576
[ -n "${QUICK_NATIVE_WRITE:-}" ] && sw q-$tag.csv "native $tag" --only ib_write_bw --sizes 16384,4194304
ssh misty "cat /tmp/q-$tag.csv" > "$out/q-$tag.csv"
for h in fuzzy misty; do ssh "$h" 'sudo dmesg | grep -i -E "retransmit|timeout|error|fail" | grep -v -E "ib_srpt|MAD agent" | tail -5' > "$out/q-$tag-dmesg-$h.txt" || true; done
echo "quick $tag done"
