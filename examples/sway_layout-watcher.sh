#!/bin/bash
# layout-watcher.sh — Companion to swaykbdd.
# Listens for Sway input events and signals dvorak daemons
# whenever the keyboard layout changes.
#
# Usage:  sway_layout-watcher.sh [--daemon]
#
# Layout indexes (must match your sway input config):
#   0 = dvorak  → dvorak-signal.sh on
#   1/2/3       → dvorak-signal.sh off

# ── Single global flag controlling all output ────────────────────────
QUIET=false
for _arg in "$@"; do
    [[ "$_arg" == "--daemon" ]] && QUIET=true
done

log() { "$QUIET" && return 0; printf '%s\n' "$*" >&2; }
# ─────────────────────────────────────────────────────────────────────

# ── Kill the previous instance (if any) ─────────────────────────────
SCRIPT_NAME="$(basename "$0")"
PIDFILE="/tmp/${SCRIPT_NAME}.pid"
MY_PID=$$

get_start_ticks() {
    awk '{
        sub(/^.*\) /, "")
        print $20
    }' "/proc/$1/stat" 2>/dev/null
}

old_pid=""
if [[ -f "$PIDFILE" ]]; then
    old_pid=$(<"$PIDFILE")
fi

_tmpfile=$(mktemp "${PIDFILE}.XXXXXX")
echo "$MY_PID" > "$_tmpfile"
mv -f "$_tmpfile" "$PIDFILE"

if [[ -n "$old_pid" && "$old_pid" != "$MY_PID" ]] &&
   kill -0 "$old_pid" 2>/dev/null &&
   grep -Fqa "$SCRIPT_NAME" "/proc/$old_pid/cmdline" 2>/dev/null; then

    old_children=()
    old_child_ticks=()
    while IFS= read -r cpid; do
        [[ -z "$cpid" ]] && continue
        old_children+=("$cpid")
        old_child_ticks+=("$(get_start_ticks "$cpid")")
    done < <(pgrep -P "$old_pid" 2>/dev/null)

    kill "$old_pid" 2>/dev/null
    sleep 0.3
    kill -9 "$old_pid" 2>/dev/null

    for i in "${!old_children[@]}"; do
        cpid="${old_children[$i]}"
        expected="${old_child_ticks[$i]}"
        actual=$(get_start_ticks "$cpid")
        if [[ -n "$actual" && "$actual" == "$expected" ]]; then
            kill -9 "$cpid" 2>/dev/null
        fi
    done
fi

SWAYMSG_PID=""

cleanup() {
    if [[ -n "$SWAYMSG_PID" ]] && kill -0 "$SWAYMSG_PID" 2>/dev/null; then
        kill "$SWAYMSG_PID" 2>/dev/null
        kill -9 "$SWAYMSG_PID" 2>/dev/null
    fi
    pkill -P $$ 2>/dev/null
    sleep 0.1
    pkill -9 -P $$ 2>/dev/null
    [[ -f "$PIDFILE" && "$(<"$PIDFILE")" == "$$" ]] && rm -f "$PIDFILE"
    rm -f "$SUBSCRIBE_FIFO"
}
trap cleanup EXIT
# ─────────────────────────────────────────────────────────────────────

SIGNAL_SCRIPT="/usr/local/bin/dvorak-signal.sh"
LAST_INDEX=""
SUBSCRIBE_FIFO="/tmp/${SCRIPT_NAME}.$$.fifo"

LOG_FILE="/tmp/dvorak-layout.log"
MAX_LOG_BYTES=1048576  # 1 MiB

# CHANGE: Max consecutive read-timeouts before we tear down the subscription
# and reconnect.  At 5 s per timeout this is ~5 minutes.
MAX_CONSECUTIVE_TIMEOUTS=60

rotate_log() {
    local size
    size=$(stat -c%s "$LOG_FILE" 2>/dev/null) || return
    if [[ "$size" -gt "$MAX_LOG_BYTES" ]]; then
        mv -f "$LOG_FILE" "${LOG_FILE}.old"
    fi
}

get_layout_index() {
    local result
    result=$(swaymsg -t get_inputs 2>/dev/null | jq -r '
        [.[] | select(.type=="keyboard"
                      and (.name | test("Virtual Dvorak"; "i") | not))]
        | .[0].xkb_active_layout_index // 0
    ' 2>/dev/null)
    if [[ "$result" =~ ^[0-9]+$ ]]; then
        echo "$result"
    else
        echo ""
    fi
}

signal_for_index() {
    local index="$1"
    [[ -z "$index" ]] && return
    [[ "$index" == "$LAST_INDEX" ]] && return

    local cmd
    if [[ "$index" -eq 0 ]]; then cmd="on"; else cmd="off"; fi

    local log_target="$LOG_FILE"
    if "$QUIET"; then
        log_target="/dev/null"
    else
        rotate_log
    fi

    if "$SIGNAL_SCRIPT" "$cmd" >>"$log_target" 2>&1; then
        LAST_INDEX="$index"
    else
        local rc=$?
        if ! "$QUIET"; then
            echo "$(date '+%F %T') signal_for_index: dvorak-signal.sh $cmd failed (exit $rc), will retry" >>"$LOG_FILE" 2>&1
        fi
        LAST_INDEX=""
    fi
}

wait_for_sway() {
    while ! swaymsg -t get_version &>/dev/null; do
        sleep 1
    done
}

is_alive() {
    local state
    state=$(awk '/^State:/ {print $2; exit}' "/proc/$1/status" 2>/dev/null)
    [[ -n "$state" && "$state" != "Z" ]]
}

while true; do
    wait_for_sway

    LAST_INDEX=""
    signal_for_index "$(get_layout_index)"

    rm -f "$SUBSCRIBE_FIFO"
    mkfifo "$SUBSCRIBE_FIFO"

    exec 3<> "$SUBSCRIBE_FIFO"

    # CHANGE: stdbuf -oL forces line-buffered stdout so events aren't
    # trapped in glibc's full-size buffer when writing to a FIFO.
    stdbuf -oL swaymsg -t subscribe -m '["input"]' > "$SUBSCRIBE_FIFO" 2>/dev/null 3<&- &
    SWAYMSG_PID=$!

    sleep 0.2
    if ! is_alive "$SWAYMSG_PID"; then
        exec 3<&-
        wait "$SWAYMSG_PID" 2>/dev/null
        SWAYMSG_PID=""
        rm -f "$SUBSCRIBE_FIFO"
        sleep 1
        continue
    fi

    # CHANGE: Track consecutive timeouts for staleness detection.
    consecutive_timeouts=0

    while is_alive "$SWAYMSG_PID"; do
        if read -r -t 5 _event <&3 2>/dev/null; then
            # CHANGE: Reset staleness counter on any successful read.
            consecutive_timeouts=0
            index=$(get_layout_index)
            signal_for_index "$index"
        else
            # CHANGE [PRIMARY FIX]: The timeout path now polls the layout
            # as a fallback.  Previously this was a no-op — if the event
            # stream stalled (sway reload, suspend/resume, buffering),
            # layout changes were never noticed and the toggle "hung".
            index=$(get_layout_index)
            signal_for_index "$index"

            # CHANGE: If we haven't received a single event in
            # MAX_CONSECUTIVE_TIMEOUTS cycles, assume the IPC
            # subscription is stale and force a reconnect.
            consecutive_timeouts=$((consecutive_timeouts + 1))
            if [[ $consecutive_timeouts -ge $MAX_CONSECUTIVE_TIMEOUTS ]]; then
                log "Stale subscription detected (${consecutive_timeouts} consecutive timeouts), restarting..."
                kill "$SWAYMSG_PID" 2>/dev/null
                sleep 0.1
                kill -9 "$SWAYMSG_PID" 2>/dev/null
                break
            fi
        fi
    done

    exec 3<&-
    wait "$SWAYMSG_PID" 2>/dev/null
    SWAYMSG_PID=""
    rm -f "$SUBSCRIBE_FIFO"

    sleep 1
done
