#!/bin/bash
# xapply.sh MASK VALUE HOST...: apply via experimental module on each host in turn
set -u; m=$1; v=$2; shift 2
for h in "$@"; do
  ssh $h "sudo rmmod thunderbolt_ibverbs 2>/dev/null; sudo USB4_H2H_KO=/var/tmp/h2h-exp/bos_usb4h2h.ko USB4_H2H_ARGS='xmask=$m xvalue=$v' /var/tmp/h2h-exp/usb4-h2h.sh apply 2>&1 | grep -E 'router|usb4-h2h|Gb/s|no partner'; echo \"\$(hostname) rc=\${PIPESTATUS[0]}\""
done
