#!/usr/bin/env bash
# serial_capture.sh — capture the carsh USB-C CDC-ACM console to a log file.
# Survives power-cycles: waits for the device, configures raw 115200, cats it,
# and loops when the gadget detaches on reboot. Each (re)connect is marked.
set -u
DEV="${1:-/dev/ttyACM0}"
LOG="${2:-/tmp/carsh-serial.log}"

echo "=== serial_capture started $(date '+%F %T') dev=$DEV log=$LOG ===" >>"$LOG"
while true; do
    if [ -e "$DEV" ]; then
        echo "" >>"$LOG"
        echo "=== [connect] $DEV up @ $(date '+%F %T') ===" >>"$LOG"
        # raw, 115200, no flow control / echo / translation
        stty -F "$DEV" 115200 raw -echo -echoe -echok -crtscts -ixon -ixoff 2>>"$LOG"
        # cat returns when the device detaches (power-cycle/unplug)
        cat "$DEV" >>"$LOG" 2>/dev/null
        echo "" >>"$LOG"
        echo "=== [detach] $DEV gone @ $(date '+%F %T') ===" >>"$LOG"
    fi
    sleep 0.3
done
