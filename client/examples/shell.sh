#!/usr/bin/env bash
# Driving the display from a plain shell: no client needed.
# The port: /dev/tdisplay with the udev rule installed, otherwise usually /dev/ttyACM0.
set -euo pipefail
TTY=${TDISPLAY_PORT:-/dev/tdisplay}
[ -e "$TTY" ] || TTY=/dev/ttyACM0

say() { printf '%s\n' "$*" > "$TTY"; }

say "RESET"
say "TITLE $(hostname -s | tr '[:lower:]' '[:upper:]')"
say "STATUS ok|ONLINE"
say "BIG Users|$(who | wc -l)"
say "BAR Disk /|$(df --output=pcent / | tail -1 | tr -dc 0-9)|100|$(df -h --output=avail / | tail -1 | tr -d ' ') free"
say "KV Kernel|$(uname -r)"
say "LED sshd|$(systemctl is-active --quiet sshd && echo ok || echo crit)|$(systemctl is-active sshd || true)"
say "TICKER $(uptime | sed 's/^ *//')"
say "+ shell example done"

# A live log: every new journal line scrolls on the display (Ctrl-C to stop).
# journalctl -f -o cat | while IFS= read -r line; do say "$line"; done

# A timed alert:
# say "ALERT warn|Backup running, do not power off|30"
