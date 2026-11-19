#!/usr/bin/env bash
# Temporary lab module switch. Run only within an agreed maintenance window.
set -euo pipefail
lab=${1:?usage: lab-verbs-dmabuf-module.sh LAB snapshot|unload|load|restore [ARG]}
action=${2:?missing action}
module=thunderbolt_ibverbs

idle() {
    if fuser /dev/kfd /dev/infiniband/uverbs* >/dev/null 2>&1; then
        echo 'Module switch refused: GPU or verbs device is in use' >&2
        exit 1
    fi
}

restore() {
    if [[ -d /sys/module/$module ]]; then
        rmmod "$module"
    fi
    mapfile -t params < "$lab/baseline.parameters"
    insmod "$lab/baseline.ko" "${params[@]}"
    cmp "$lab/baseline.note" "/sys/module/$module/notes/.note.gnu.build-id"
}

case "$action" in
snapshot)
    baseline=${3:?provide the actual loaded module file}
    test ! -e "$lab/baseline.ko"
    test "$(modinfo -F name "$baseline")" = "$module"
    objcopy --dump-section .note.gnu.build-id="$lab/baseline.note" \
        "$baseline" "$lab/.note-extraction.ko"
    rm "$lab/.note-extraction.ko"
    cmp "$lab/baseline.note" "/sys/module/$module/notes/.note.gnu.build-id"
    cp "$baseline" "$lab/baseline.ko"
    : > "$lab/baseline.parameters"
    for file in /sys/module/$module/parameters/*; do
        [[ ${file##*/} = dmabuf_control_bench ]] && continue
        printf '%s=%s\n' "${file##*/}" "$(cat "$file")" >> "$lab/baseline.parameters"
    done
    cat /proc/sys/kernel/tainted > "$lab/baseline.tainted"
    ;;
unload)
    test -f "$lab/baseline.ko"
    idle
    if [[ -d /sys/module/$module ]]; then
        rmmod "$module"
    fi
    ;;
load)
    mode=${3:?normal or dmabuf}
    [[ $mode = normal || $mode = dmabuf ]]
    test -f "$lab/baseline.ko"
    test -f "$lab/test-signed.ko"
    idle
    mapfile -t params < "$lab/baseline.parameters"
    extra=(dmabuf_verbs=0)
    if [[ $mode = dmabuf ]]; then
        extra=(dmabuf_verbs=1 register_verbs=1)
    fi
    index=${TBV_LAB_INDEX:-1}
    eager=${TBV_LAB_RX_EAGER:-1}
    parallel=${TBV_LAB_PARALLEL_POST:-1}
    control=${TBV_LAB_CONTROL_RING:-0}
    push_done=${TBV_LAB_PUSH_DONE:-0}
    credits=${TBV_LAB_CREDITS:-0}
    direct=${TBV_LAB_DIRECT_POST:-}
    [[ $index = 0 || $index = 1 ]]
    [[ $eager = 0 || $eager = 1 ]]
    [[ $parallel = 0 || $parallel = 1 ]]
    [[ $control = 0 || $control = 1 ]]
    [[ $push_done = 0 || $push_done = 1 ]]
    [[ $credits = 0 || $credits = 1 ]]
    [[ -z $direct || $direct = 0 || $direct = 1 ]]
    extra+=(dmabuf_index="$index" dmabuf_rx_eager="$eager"
        dmabuf_parallel_post="$parallel" dmabuf_control_ring="$control"
        dmabuf_push_done="$push_done")
    # Older test modules lack the parameter; pass it only when requested.
    if [[ $credits = 1 ]]; then
        extra+=(dmabuf_credits=1)
    fi
    if [[ -n $direct ]]; then
        extra+=(dmabuf_direct_post="$direct")
    fi
    if [[ -n ${TBV_LAB_LANES:-} ]]; then
        [[ $TBV_LAB_LANES = 1 || $TBV_LAB_LANES = 2 ]]
        extra+=(lanes="$TBV_LAB_LANES")
    fi
    if [[ -n ${TBV_LAB_DYNDBG:-} ]]; then
        extra+=("dyndbg=$TBV_LAB_DYNDBG")
    fi
    if [[ -d /sys/module/$module ]]; then
        rmmod "$module"
    fi
    if ! insmod "$lab/test-signed.ko" "${params[@]}" "${extra[@]}"; then
        restore
        exit 1
    fi
    ;;
restore)
    idle
    restore
    ;;
*) exit 2 ;;
esac
