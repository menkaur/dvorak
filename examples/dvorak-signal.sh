#!/usr/bin/env bash
#
# dvorak-signal.sh — Send on/off signals to all running dvorak daemons.
#
# Usage:
#   dvorak-signal.sh [--quiet] on    # SIGUSR1 — enable Dvorak mapping
#   dvorak-signal.sh [--quiet] off   # SIGUSR2 — passthrough mode
#
# --quiet suppresses all informational output.
#
# Automatically elevates to root via sudo if needed (dvorak daemons run as root).
# Requires a sudoers rule for non-interactive use — see examples/dvorak-signal-sudoers.
#
# Works with:
#   1. PID files (reads from /run/dvorak-*.pid by default)
#   2. Falls back to pkill if no PID files found/valid
#

set -uo pipefail
shopt -s nullglob

PIDFILE_DIR="/run"
PIDFILE_GLOB="dvorak-*.pid"
PROCESS_NAME="dvorak"

# ── Single global flag controlling all output ────────────────────────
QUIET=false

log() { "$QUIET" && return 0; echo "$@"; }
log_err() { "$QUIET" && return 0; echo "$@" >&2; }
# ─────────────────────────────────────────────────────────────────────

# usage() always prints — bad invocation needs visible feedback.
usage() {
    echo "Usage: $0 [--quiet] {on|off}" >&2
    echo "  on     — enable Dvorak mapping (SIGUSR1)" >&2
    echo "  off    — passthrough / no mapping (SIGUSR2)" >&2
    echo "  --quiet  suppress informational output" >&2
    exit 1
}

# ── Parse optional --quiet flag ──────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --quiet) QUIET=true; shift ;;
        on|off)  break ;;
        *)       usage ;;
    esac
done

[[ $# -eq 1 ]] || usage

case "$1" in
    on)  SIG="USR1" ;;
    off) SIG="USR2" ;;
    *)   usage ;;
esac

# Auto-elevate: dvorak daemons run as root, signaling them requires root.
if [[ $EUID -ne 0 ]]; then
    SELF="$(realpath "$0" 2>/dev/null || readlink -f "$0")"
    if "$QUIET"; then
        exec sudo -n -- "$SELF" --quiet "$1"
    else
        exec sudo -n -- "$SELF" "$1"
    fi
fi

sent=0
failed=0

# Try PID files first
for pidfile in "${PIDFILE_DIR}"/${PIDFILE_GLOB}; do
    pid=$(cat "$pidfile" 2>/dev/null) || continue
    [[ "$pid" =~ ^[0-9]+$ ]] || { log_err "Warning: corrupt PID file ${pidfile}"; failed=$((failed + 1)); continue; }

    if ! kill -0 "$pid" 2>/dev/null; then
        log_err "Warning: stale PID file ${pidfile} (pid=${pid})"
        failed=$((failed + 1))
        continue
    fi

    proc_name=$(cat "/proc/$pid/comm" 2>/dev/null) || true
    if [[ "$proc_name" != "$PROCESS_NAME" ]]; then
        log_err "Warning: PID ${pid} from ${pidfile} is not dvorak (is: ${proc_name:-unknown})"
        failed=$((failed + 1))
        continue
    fi

    if kill -s "$SIG" "$pid" 2>/dev/null; then
        log "Sent SIG${SIG} to PID ${pid} (from ${pidfile})"
        sent=$((sent + 1))
    else
        log_err "Warning: failed to signal PID ${pid} (from ${pidfile})"
        failed=$((failed + 1))
    fi
done

# Fall back to pkill if no valid PID files were found
if [[ $sent -eq 0 ]]; then
    if pkill -"$SIG" -x "$PROCESS_NAME" 2>/dev/null; then
        log "Sent SIG${SIG} to '${PROCESS_NAME}' process(es) via pkill"
    else
        log_err "Error: no running '${PROCESS_NAME}' processes found."
        exit 1
    fi
fi

# Partial success = non-zero exit so callers can detect
if [[ $failed -gt 0 ]]; then
    log_err "Warning: ${failed} pidfile(s) had issues (${sent} signaled successfully)"
    exit 2
fi

exit 0
