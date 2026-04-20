#!/usr/bin/env bash
# lima-monitor.sh — LIMA gateway headless monitor
# Byobu/tmux session with ratatui TUI + 4 secondary panes
#
# Layout:
#   ┌──────────────────────────┬─────────────────────────┐
#   │                          │  [1] mosquitto log      │
#   │                          ├─────────────────────────┤
#   │   [0] ratatui TUI        │  [2] hci1 watchdog      │
#   │       (gateway)          ├─────────────────────────┤
#   │                          │  [3] rpi health         │
#   │                          ├─────────────────────────┤
#   │                          │  [4] sqlite tail        │
#   └──────────────────────────┴─────────────────────────┘
#
# Usage:
#   ./lima-monitor.sh            # launch / reattach
#   ./lima-monitor.sh kill       # destroy session
#
# Config — adjust these paths if layout changes
SESSION="lima"
GATEWAY_DIR="${HOME}/lima-node/gateway"
GATEWAY_BIN="${GATEWAY_DIR}/target/release/gateway"
LIMA_DB="${GATEWAY_DIR}/lima.db"
HCI_IFACE="hci1"
MQTT_LOG="/var/log/mosquitto/mosquitto.log"

# ── helpers ──────────────────────────────────────────────────────────────────

has_session() { tmux has-session -t "$SESSION" 2>/dev/null; }

die() { echo "ERROR: $*" >&2; exit 1; }

# ── kill mode ─────────────────────────────────────────────────────────────────

if [[ "$1" == "kill" ]]; then
    tmux kill-session -t "$SESSION" 2>/dev/null && echo "Session '$SESSION' killed." || echo "No session to kill."
    exit 0
fi

# ── reattach if already running ───────────────────────────────────────────────

if has_session; then
    echo "Session '$SESSION' already running — reattaching."
    exec byobu attach-session -t "$SESSION" 2>/dev/null || exec tmux attach-session -t "$SESSION"
fi

# ── pane command strings ──────────────────────────────────────────────────────

# [0] Gateway TUI — run directly; falls back to a wait loop if binary missing
CMD_GATEWAY="
cd ${GATEWAY_DIR}
if [ -x '${GATEWAY_BIN}' ]; then
    while true; do
        echo '[lima] starting gateway...'
        ${GATEWAY_BIN}
        echo '[lima] gateway exited — restart in 5s (Ctrl-C to abort)'
        sleep 5
    done
else
    echo '[lima] WARNING: gateway binary not found at ${GATEWAY_BIN}'
    echo '[lima] run: cargo build --release  to build it'
    bash
fi
"

# [1] Mosquitto log tail — tries journalctl first, falls back to file
CMD_MOSQUITTO="
echo '[lima] mosquitto log — $(date)'
echo '─────────────────────────────────────────'
if systemctl is-active --quiet mosquitto; then
    journalctl -u mosquitto -f --no-pager -n 50
else
    echo '[lima] mosquitto not running via systemd — tailing file'
    sudo tail -f ${MQTT_LOG} 2>/dev/null || echo '[lima] no log accessible'
    bash
fi
"

# [2] BLE / hci1 watchdog — polls every 10s, highlights timeout
CMD_HCI_WATCHDOG="
echo '[lima] hci watchdog — ${HCI_IFACE}'
echo '─────────────────────────────────────────'
WARN=0
while true; do
    TS=\$(date '+%H:%M:%S')
    OUT=\$(hciconfig ${HCI_IFACE} 2>&1)
    STATE=\$(echo \"\$OUT\" | grep -oE 'UP RUNNING|DOWN|ERROR')
    TIMEOUT=\$(echo \"\$OUT\" | grep -c 'timed out')
    RX=\$(echo \"\$OUT\" | grep -oP 'RX bytes:\K[0-9]+' || echo '?')
    TX=\$(echo \"\$OUT\" | grep -oP 'TX bytes:\K[0-9]+' || echo '?')
    if [ \"\$TIMEOUT\" -gt 0 ]; then
        printf '\033[31m[%s] TIMEOUT — adapter hung! RX:%s TX:%s\033[0m\n' \"\$TS\" \"\$RX\" \"\$TX\"
        WARN=1
    elif [ \"\$STATE\" = 'UP RUNNING' ]; then
        if [ \"\$WARN\" -eq 1 ]; then
            printf '\033[32m[%s] RECOVERED — UP RUNNING RX:%s TX:%s\033[0m\n' \"\$TS\" \"\$RX\" \"\$TX\"
            WARN=0
        else
            printf '\033[32m[%s] OK RX:%s TX:%s\033[0m\n' \"\$TS\" \"\$RX\" \"\$TX\"
        fi
    else
        printf '\033[33m[%s] STATE:%s RX:%s TX:%s\033[0m\n' \"\$TS\" \"\${STATE:-unknown}\" \"\$RX\" \"\$TX\"
    fi
    sleep 10
done
"

# [3] RPi system health — CPU temp, memory, disk, load
CMD_HEALTH="
echo '[lima] rpi system health'
echo '─────────────────────────────────────────'
while true; do
    clear
    TS=\$(date '+%Y-%m-%d %H:%M:%S')
    TEMP=\$(vcgencmd measure_temp 2>/dev/null | cut -d= -f2 || cat /sys/class/thermal/thermal_zone0/temp | awk '{printf \"%.1f°C\", \$1/1000}')
    FREQ=\$(vcgencmd measure_clock arm 2>/dev/null | cut -d= -f2 | awk '{printf \"%.0f MHz\", \$1/1000000}' || echo 'n/a')
    VOLT=\$(vcgencmd measure_volts core 2>/dev/null | cut -d= -f2 || echo 'n/a')
    LOAD=\$(uptime | grep -oP 'load average: \K.*')
    UPTIME=\$(uptime -p)
    printf '%-12s %s\n' 'Time:' \"\$TS\"
    printf '%-12s %s\n' 'Uptime:' \"\$UPTIME\"
    printf '%-12s %s\n' 'Temp:' \"\$TEMP\"
    printf '%-12s %s\n' 'CPU freq:' \"\$FREQ\"
    printf '%-12s %s\n' 'Core volt:' \"\$VOLT\"
    printf '%-12s %s\n' 'Load avg:' \"\$LOAD\"
    echo ''
    free -h | awk 'NR==1{printf \"%-12s %8s %8s %8s\n\",\"Memory:\",\$2,\$3,\$4} NR==2{printf \"%-12s %8s %8s %8s\n\",\"\",\$2,\$3,\$4}'
    echo ''
    df -h / /boot/firmware 2>/dev/null | awk 'NR==1{print} NR>1{print}'
    sleep 15
done
"

# [4] SQLite tail — auto-discovers table, shows last 15 rows
CMD_SQLITE="
echo '[lima] sqlite audit log — ${LIMA_DB}'
echo '─────────────────────────────────────────'
if [ ! -f '${LIMA_DB}' ]; then
    echo '[lima] db not found at ${LIMA_DB}'
    echo '[lima] waiting...'
    while [ ! -f '${LIMA_DB}' ]; do sleep 5; done
fi
# Discover the most likely audit/event table
TBL=\$(sqlite3 '${LIMA_DB}' '.tables' 2>/dev/null | tr ' ' '\n' | grep -iE 'event|audit|log|frame|packet' | head -1)
if [ -z \"\$TBL\" ]; then
    TBL=\$(sqlite3 '${LIMA_DB}' '.tables' 2>/dev/null | tr ' ' '\n' | head -1)
fi
if [ -z \"\$TBL\" ]; then
    echo '[lima] no tables found — db may be initializing'
    bash
fi
echo \"[lima] watching table: \$TBL\"
echo ''
while true; do
    sqlite3 -column -header '${LIMA_DB}' \
        \"SELECT * FROM \${TBL} ORDER BY rowid DESC LIMIT 15;\" 2>/dev/null \
        | head -40
    echo ''
    printf '  [updated %s — refresh in 10s]\n' \"\$(date '+%H:%M:%S')\"
    sleep 10
    clear
    echo '[lima] sqlite — '\"'\$TBL'\"
    echo '─────────────────────────────────────────'
done
"

# ── build session ─────────────────────────────────────────────────────────────

# Create detached session with pane 0 (gateway TUI)
tmux new-session -d -s "$SESSION" -x "$(tput cols)" -y "$(tput lines)"

# Send gateway command to pane 0
tmux send-keys -t "${SESSION}:0.0" "$CMD_GATEWAY" Enter

# Split right 40% → pane 1 (mosquitto)
tmux split-window -t "${SESSION}:0.0" -h -p 40
tmux send-keys -t "${SESSION}:0.1" "$CMD_MOSQUITTO" Enter

# Split pane 1 down → pane 2 (hci watchdog), leaving pane 1 as top-right
tmux split-window -t "${SESSION}:0.1" -v -p 75
tmux send-keys -t "${SESSION}:0.2" "$CMD_HCI_WATCHDOG" Enter

# Split pane 2 down → pane 3 (health), pane 2 becomes upper-mid
tmux split-window -t "${SESSION}:0.2" -v -p 67
tmux send-keys -t "${SESSION}:0.3" "$CMD_HEALTH" Enter

# Split pane 3 down → pane 4 (sqlite), pane 3 becomes lower-mid
tmux split-window -t "${SESSION}:0.3" -v -p 50
tmux send-keys -t "${SESSION}:0.4" "$CMD_SQLITE" Enter

# Focus back to gateway TUI pane
tmux select-pane -t "${SESSION}:0.0"

# ── set pane titles ───────────────────────────────────────────────────────────

tmux select-pane -t "${SESSION}:0.0" -T "gateway"
tmux select-pane -t "${SESSION}:0.1" -T "mosquitto"
tmux select-pane -t "${SESSION}:0.2" -T "hci1-watchdog"
tmux select-pane -t "${SESSION}:0.3" -T "rpi-health"
tmux select-pane -t "${SESSION}:0.4" -T "sqlite"

# ── attach ────────────────────────────────────────────────────────────────────

echo "Session '$SESSION' created — attaching."
exec byobu attach-session -t "$SESSION" 2>/dev/null || exec tmux attach-session -t "$SESSION"