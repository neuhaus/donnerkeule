#!/usr/bin/env bash
# Scripted two-host hardware qualification for thunderbolt_ibverbs (T3).
#
# Runs from the workstation over SSH against HOST0 (rank 0 / server) and
# HOST1 (rank 1 / client). Verifies link speed, reloads the installed DKMS
# module in DMA-BUF and native modes, runs bandwidth, integrity, and
# kill-during-transfer checks, diffs debugfs counters, checks dmesg, and
# restores the default DMA-BUF module configuration on exit.
#
# Usage:
#   tools/lab/qualify.sh [--out DIR] [--dmabuf-opts "k=v ..."] [--native-opts "k=v ..."]
#                        [--skip-native] [--skip-dmabuf]
set -euo pipefail

HOST0=${TBV_HOST0:-fuzzy}
HOST1=${TBV_HOST1:-misty}
PEER_IP=${TBV_PEER_IP:-192.168.211.2}
DEV=${TBV_DEV:-usb4_rdma0}
GID=${TBV_GID:-1}
DUR=${TBV_DUR:-3}
PORT=${TBV_PORT:-19123}
VERIFY_BIN=${TBV_VERIFY_BIN:-/home/sn/tbv/rc_write_imm_verify}
OUT=${TBV_OUT:-/tmp/tbv-qualify-$(date +%Y%m%d-%H%M%S)}
DMABUF_OPTS=${TBV_DMABUF_OPTS:-}
NATIVE_OPTS=${TBV_NATIVE_OPTS:-}
SKIP_NATIVE=0
SKIP_DMABUF=0

while [[ $# -gt 0 ]]; do
	case "$1" in
	--out) OUT=$2; shift 2 ;;
	--dmabuf-opts) DMABUF_OPTS=$2; shift 2 ;;
	--native-opts) NATIVE_OPTS=$2; shift 2 ;;
	--skip-native) SKIP_NATIVE=1; shift ;;
	--skip-dmabuf) SKIP_DMABUF=1; shift ;;
	-h|--help)
		sed -n '2,14p' "$0"
		exit 0
		;;
	*)
		echo "unknown argument: $1" >&2
		exit 2
		;;
	esac
done

mkdir -p "$OUT"
SUMMARY="$OUT/summary.txt"
: > "$SUMMARY"
PASS=0
FAIL=0

log() {
	printf '%s\n' "$*" | tee -a "$SUMMARY"
}

pass() {
	PASS=$((PASS + 1))
	log "PASS: $*"
}

fail() {
	FAIL=$((FAIL + 1))
	log "FAIL: $*"
}

check_links() {
	local h ok
	for h in "$HOST0" "$HOST1"; do
		ok=$(ssh "$h" '
			n=0
			for d in /sys/bus/thunderbolt/devices/*; do
				[ -f "$d/rx_speed" ] || continue
				rx=$(cat "$d/rx_speed")
				tx=$(cat "$d/tx_speed")
				rl=$(cat "$d/rx_lanes" 2>/dev/null || echo 0)
				tl=$(cat "$d/tx_lanes" 2>/dev/null || echo 0)
				echo "$(basename "$d"): rx=$rx tx=$tx lanes=$rl/$tl" >&2
				if [ "$rx" = "20.0 Gb/s" ] && [ "$tx" = "20.0 Gb/s" ] &&
				   [ "$rl" = "2" ] && [ "$tl" = "2" ]; then
					n=$((n + 1))
				fi
			done
			echo "$n"
		' 2>"$OUT/$h-links.log")
		if [[ ${ok:-0} -lt 2 ]]; then
			cat "$OUT/$h-links.log" >&2
			fail "link_speed ($h): expected >=2 links at 2x20.0 Gb/s, got ${ok:-0}"
			return 1
		fi
	done
	pass "link_speed: both $HOST0 and $HOST1 have >=2 links at 2x20.0 Gb/s"
}

check_idle() {
	local h
	for h in "$HOST0" "$HOST1"; do
		if ssh "$h" 'sudo fuser /dev/infiniband/uverbs* >/dev/null 2>&1'; then
			fail "idle ($h): /dev/infiniband/uverbs* is busy (stop gufo first)"
			return 1
		fi
	done
	pass "idle: no active verbs consumers on $HOST0 or $HOST1"
}

load_module() {
	local mode=$1
	shift
	local extra="$*"
	local h

	for h in "$HOST0" "$HOST1"; do
		ssh "$h" "sudo rmmod thunderbolt_ibverbs 2>/dev/null || true"
	done
	for h in "$HOST0" "$HOST1"; do
		ssh "$h" "sudo dmesg -C; sudo modprobe thunderbolt_ibverbs $extra" &
	done
	wait

	local i n0 n1
	for i in $(seq 1 20); do
		n0=$(ssh "$HOST0" "ibv_devices 2>/dev/null | grep -c usb4_rdma || true")
		n1=$(ssh "$HOST1" "ibv_devices 2>/dev/null | grep -c usb4_rdma || true")
		if [[ $n0 -ge 4 && $n1 -ge 4 ]]; then
			break
		fi
		sleep 1
	done
	if [[ $n0 -lt 4 || $n1 -lt 4 ]]; then
		fail "load ($mode): expected 4 usb4_rdma devices, got $HOST0=$n0 $HOST1=$n1"
		return 1
	fi

	local v0 v1
	v0=$(ssh "$HOST0" "cat /sys/module/thunderbolt_ibverbs/version 2>/dev/null || echo unknown")
	v1=$(ssh "$HOST1" "cat /sys/module/thunderbolt_ibverbs/version 2>/dev/null || echo unknown")
	if [[ "$v0" != "$v1" ]]; then
		fail "load ($mode): version mismatch $HOST0=$v0 $HOST1=$v1"
		return 1
	fi

	if [[ "$mode" == "dmabuf" ]]; then
		local h0 h1
		h0=$(ssh "$HOST0" 'sudo grep -c "descriptor_ready=1 quarantine_reason=0" /sys/kernel/debug/thunderbolt_ibverbs/peers 2>/dev/null || true')
		h1=$(ssh "$HOST1" 'sudo grep -c "descriptor_ready=1 quarantine_reason=0" /sys/kernel/debug/thunderbolt_ibverbs/peers 2>/dev/null || true')
		if [[ $h0 -lt 4 || $h1 -lt 4 ]]; then
			fail "load ($mode): expected 4 healthy rails, got $HOST0=$h0 $HOST1=$h1"
			return 1
		fi
	fi

	pass "load ($mode): version=$v0 devices=4/4"
}

snap_counters() {
	local tag=$1
	local h
	for h in "$HOST0" "$HOST1"; do
		ssh "$h" 'sudo cat /sys/kernel/debug/thunderbolt_ibverbs/summary' > "$OUT/$h-$tag.summary"
	done
}

diff_counters() {
	local before=$1
	local after=$2
	local mode=$3
	local h
	for h in "$HOST0" "$HOST1"; do
		python3 - "$OUT/$h-$before.summary" "$OUT/$h-$after.summary" > "$OUT/$h-$mode.diff" <<'PY'
import sys
def parse(p):
    d = {}
    with open(p) as f:
        for line in f:
            if ':' not in line:
                continue
            k, v = line.split(':', 1)
            k, v = k.strip(), v.strip()
            try:
                d[k] = int(v)
            except ValueError:
                pass
    return d
a, b = parse(sys.argv[1]), parse(sys.argv[2])
for k in sorted(set(a) | set(b)):
    da = b.get(k, 0) - a.get(k, 0)
    if da != 0:
        print(f"{k}: {da:+d}")
PY
		local bad
		bad=$(grep -E '^(data_tx_errors|data_rx_crc_error|data_rx_bad_frame|data_rx_qp_error|data_wr_timeout|dmabuf_cancel_stopped|dmabuf_cancel_rx_stopped):' "$OUT/$h-$mode.diff" || true)
		if [[ -n "$bad" ]]; then
			fail "counters ($mode/$h): unexpected error deltas: $(echo "$bad" | tr '\n' ' ')"
		else
			pass "counters ($mode/$h): zero error deltas"
		fi
	done
}

check_dmesg() {
	local mode=$1
	local h
	for h in "$HOST0" "$HOST1"; do
		ssh "$h" 'sudo dmesg' > "$OUT/$h-$mode.dmesg"
		local bad
		bad=$(grep -ciE 'stopped after|unexpected|BUG:|WARNING:|Oops|release failed|general protection fault|KASAN|lockdep' "$OUT/$h-$mode.dmesg" || true)
		if [[ ${bad:-0} -gt 0 ]]; then
			fail "dmesg ($mode/$h): $bad suspicious kernel lines (see $OUT/$h-$mode.dmesg)"
		else
			pass "dmesg ($mode/$h): clean"
		fi
	done
}

run_perftest() {
	local label=$1
	local bin=$2
	local size=$3
	shift 3
	local extra="$*"
	local args="-d $DEV -i 1 -x $GID -p $PORT -s $size -D $DUR -F --report_gbits $extra"

	ssh "$HOST0" "timeout 45 $bin $args" > "$OUT/$HOST0-$label.log" 2>&1 &
	local p0=$!
	sleep 2
	local rc1=0
	ssh "$HOST1" "timeout 45 $bin $args $PEER_IP" > "$OUT/$HOST1-$label.log" 2>&1 || rc1=$?
	local rc0=0
	wait "$p0" || rc0=$?

	local bw
	bw=$(awk '/^[[:space:]]*[0-9]+[[:space:]]+[0-9]+/ { v=$(NF-1) } END { print v }' "$OUT/$HOST1-$label.log")
	if [[ $rc0 -eq 0 && $rc1 -eq 0 && -n "$bw" ]]; then
		pass "$label ($bin ${size}B): ${bw} Gb/s"
	else
		fail "$label ($bin ${size}B): rc0=$rc0 rc1=$rc1 (see $OUT/$HOST1-$label.log)"
	fi
}

run_kill_test() {
	local mode=$1
	local label="${mode}-kill"
	local args="-d $DEV -i 1 -x $GID -p $PORT -s 16777216 -D 10 -F --report_gbits"

	ssh "$HOST0" "timeout 30 ib_write_bw $args" > "$OUT/$HOST0-$label.log" 2>&1 &
	local p0=$!
	sleep 2
	ssh "$HOST1" "ib_write_bw $args $PEER_IP & pid=\$!; sleep 2; kill -9 \$pid 2>/dev/null || true; wait \$pid 2>/dev/null || true" > "$OUT/$HOST1-$label.log" 2>&1
	# Server may linger waiting on socket or exit with error; ensure it terminates cleanly.
	ssh "$HOST0" "pkill -9 -x ib_write_bw 2>/dev/null || true"
	wait "$p0" 2>/dev/null || true
	sleep 1

	# Follow-up transfer must succeed and rails must remain healthy.
	run_perftest "${mode}-post-kill" ib_write_bw 4194304
}

run_verify() {
	local mode=$1
	local label="${mode}-rc-write-imm-verify"
	if ! ssh "$HOST0" "test -x $VERIFY_BIN" || ! ssh "$HOST1" "test -x $VERIFY_BIN"; then
		log "SKIP: $label ($VERIFY_BIN not present on both hosts)"
		return 0
	fi
	ssh "$HOST0" "timeout 30 $VERIFY_BIN --role recv --dev $DEV --gid-index $GID --tcp-port $((PORT + 1)) --max-size 1048576 --seconds 5" > "$OUT/$HOST0-$label.log" 2>&1 &
	local p0=$!
	sleep 2
	local rc1=0
	ssh "$HOST1" "timeout 30 $VERIFY_BIN --role send --dev $DEV --gid-index $GID --connect $PEER_IP --tcp-port $((PORT + 1)) --max-size 1048576 --seconds 5" > "$OUT/$HOST1-$label.log" 2>&1 || rc1=$?
	local rc0=0
	wait "$p0" || rc0=$?
	if [[ $rc0 -eq 0 && $rc1 -eq 0 ]]; then
		pass "$label: data integrity verified"
	else
		fail "$label: rc0=$rc0 rc1=$rc1 (see $OUT/$HOST1-$label.log)"
	fi
}

log "=== thunderbolt-ibverbs qualification ($(date -Iseconds)) ==="
log "hosts: $HOST0 (server/$PEER_IP) <-> $HOST1 (client), dev=$DEV gid=$GID out=$OUT"

check_links
check_idle

if [[ $SKIP_DMABUF -eq 0 ]]; then
	log "--- Stage: DMA-BUF mode ---"
	if load_module dmabuf $DMABUF_OPTS; then
		snap_counters dmabuf-before
		run_perftest dmabuf-write-64k ib_write_bw 65536
		run_perftest dmabuf-write-4m ib_write_bw 4194304
		run_perftest dmabuf-write-16m ib_write_bw 16777216 -t 32
		run_perftest dmabuf-send-64k ib_send_bw 65536
		run_perftest dmabuf-send-4m ib_send_bw 4194304
		snap_counters dmabuf-after
		diff_counters dmabuf-before dmabuf-after dmabuf
		run_kill_test dmabuf
		check_dmesg dmabuf
	fi
fi

if [[ $SKIP_NATIVE -eq 0 ]]; then
	log "--- Stage: Native mode ---"
	if load_module native dmabuf_verbs=0 $NATIVE_OPTS; then
		snap_counters native-before
		run_perftest native-write-64k ib_write_bw 65536
		run_perftest native-write-1m ib_write_bw 1048576
		run_perftest native-send-64k ib_send_bw 65536
		run_perftest native-read-256k ib_read_bw 262144
		run_verify native
		snap_counters native-after
		diff_counters native-before native-after native
		run_kill_test native
		check_dmesg native
	fi
	# Restore default DMA-BUF module configuration.
	log "--- Restore: default DMA-BUF mode ---"
	load_module dmabuf $DMABUF_OPTS || true
fi

log "=== Summary: $PASS passed, $FAIL failed (logs: $OUT) ==="
[[ $FAIL -eq 0 ]]
