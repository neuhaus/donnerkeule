#!/bin/bash
# reboot.sh HOST...: reboot, wait for ssh, stop StrixFold, report thunderbolt module + link speed
set -u
for h in "$@"; do ssh "$h" 'sudo systemctl reboot' >/dev/null 2>&1 || true; done
sleep 45
for h in "$@"; do
  for _ in $(seq 1 60); do ssh -o ConnectTimeout=5 -o BatchMode=yes "$h" true 2>/dev/null && break; sleep 5; done
done
sleep 20
for h in "$@"; do ssh "$h" 'systemctl --user stop strixfold-sshd.service strixfold.service 2>/dev/null
  echo "$(hostname) up $(uptime -p) kernel=$(uname -r) tb=$(modinfo -n thunderbolt | sed "s|.*/7.3.4-100.fc43.x86_64/||") taint=$(cat /sys/module/thunderbolt/taint 2>/dev/null)"
  for p in /sys/bus/thunderbolt/devices/[0-9]-2; do echo "  $(basename $p) $(cat $p/device_name 2>/dev/null) $(cat $p/rx_speed 2>/dev/null) x$(cat $p/rx_lanes 2>/dev/null)"; done'; done
