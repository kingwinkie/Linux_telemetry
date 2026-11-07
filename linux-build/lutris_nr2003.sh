#!/bin/bash
# lutris_nr2003.sh — start/stop tsample.exe around a Lutris-launched NR2003.
#
# Why this does not simply start tsample in the pre-launch hook:
#   With Lutris's Proton / umu-run runner (GE-Proton), every umu-run invocation
#   creates its own pressure-vessel and wineserver.  Two sessions on one
#   WINEPREFIX deadlock: if tsample's umu-run is still alive when Lutris starts
#   NR2003, the game's wineserver blocks forever (and vice versa).  The only
#   reliable order is: game's Wine session first, then tsample.
#
#   "start" therefore arms a small detached watcher and exits immediately, so
#   Lutris can launch the game.  Once NR2003.exe is visible the watcher starts
#   tsample.exe with Proton's own wine binary (not umu-run).  That client joins
#   the game's existing wineserver, so there is only one Wine session.
#
# Lutris -> Configure -> System options -> Game execution
#   Pre-launch script:  /path/to/telemetry_linux/linux-build/lutris_nr2003.sh start
#   [x] Wait for pre-launch script completion   (start returns at once)
#   Post-exit script:   /path/to/telemetry_linux/linux-build/lutris_nr2003.sh stop
#
# Lutris exports WINE, WINEPREFIX and PROTONPATH to both hooks.  For manual
# testing the script falls back to `wine` and ~/.wine.
#
# tsample.exe must be beside this script; set TSAMPLE_EXE to override.
#
# Useful overrides:
#   TSAMPLE_WINE             Wine binary used for tsample (auto-detected for Proton)
#   NR2003_PROC_NAME         process name to watch for (default nr2003.exe)
#   NR2003_WATCH_GAME_SEC    seconds to wait for the game (default 300)
#   NR2003_POST_GAME_DELAY   seconds to wait after detection (default 2)
#   NR2003_TSAMPLE_PID_FILE  default /tmp/nr2003_tsample.pid
#   NR2003_TSAMPLE_LOG       default /tmp/nr2003_tsample.log
#   NR2003_WATCH_PID_FILE    default /tmp/nr2003_watchdog.pid
#   NR2003_WATCH_LOG         default /tmp/nr2003_watchdog.log
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"

WINE="${WINE:-wine}"
WINEPREFIX="${WINEPREFIX:-${HOME}/.wine}"
TSAMPLE_EXE="${TSAMPLE_EXE:-${SCRIPT_DIR}/tsample.exe}"
SNAP_SHM="${SNAP_SHM:-/dev/shm/nr2003_telem}"
MEM_SHM="${MEM_SHM:-/dev/shm/nr2003_mem}"
WAIT_SHM_SEC="${WAIT_SHM_SEC:-15}"
GAME_PROC_NAME="${NR2003_PROC_NAME:-nr2003.exe}"
WATCH_GAME_SEC="${NR2003_WATCH_GAME_SEC:-300}"
POST_GAME_DELAY="${NR2003_POST_GAME_DELAY:-2}"
PID_FILE="${NR2003_TSAMPLE_PID_FILE:-/tmp/nr2003_tsample.pid}"
LOG_FILE="${NR2003_TSAMPLE_LOG:-/tmp/nr2003_tsample.log}"
WATCH_PID_FILE="${NR2003_WATCH_PID_FILE:-/tmp/nr2003_watchdog.pid}"
WATCH_LOG="${NR2003_WATCH_LOG:-/tmp/nr2003_watchdog.log}"

die() {
    echo "lutris_nr2003: $*" >&2
    exit 1
}

is_running() {
    [[ -n "${1:-}" ]] && kill -0 "$1" 2>/dev/null
}

# Print the Wine binary that should run tsample.  For Proton/umu-run this is
# the Proton build's own wine, not umu-run: a second umu-run deadlocks the
# game's wineserver, while Proton's wine joins the existing session.
resolve_tsample_wine() {
    if [[ -n "${TSAMPLE_WINE:-}" ]]; then
        printf '%s\n' "$TSAMPLE_WINE"
        return 0
    fi

    local base
    base="$(basename -- "$WINE")"
    case "$base" in
        umu-run|proton|proton-*) ;;
        *)
            printf '%s\n' "$WINE"
            return 0
            ;;
    esac

    local p candidates=()
    if [[ -n "${PROTONPATH:-}" ]]; then
        if [[ -d "$PROTONPATH" && -x "$PROTONPATH/files/bin/wine" ]]; then
            printf '%s\n' "$PROTONPATH/files/bin/wine"
            return 0
        fi
        # PROTONPATH is usually a name such as GE-Proton; the installed build
        # directory is named GE-Proton11-12-x86_64.
        for p in \
            "$HOME/.local/share/Steam/compatibilitytools.d/${PROTONPATH}"*/files/bin/wine \
            "$HOME/.steam/steam/compatibilitytools.d/${PROTONPATH}"*/files/bin/wine \
            "$HOME/.local/share/umu/${PROTONPATH}"*/files/bin/wine; do
            [[ -x "$p" ]] && candidates+=("$p")
        done
    fi

    if ((${#candidates[@]} == 0)); then
        for p in \
            "$HOME/.local/share/Steam/compatibilitytools.d"/*/files/bin/wine \
            "$HOME/.steam/steam/compatibilitytools.d"/*/files/bin/wine; do
            [[ -x "$p" ]] && candidates+=("$p")
        done
    fi

    ((${#candidates[@]})) || return 1
    printf '%s\n' "$(printf '%s\n' "${candidates[@]}" | sort -V | tail -n 1)"
}

check_wine() {
    local wine_bin="$1"
    if [[ "$wine_bin" == */* ]]; then
        [[ -x "$wine_bin" ]] || die "wine is not executable: $wine_bin"
    else
        command -v "$wine_bin" >/dev/null 2>&1 || die "wine not found: $wine_bin"
    fi
}

# Kill any process whose command line names this exact tsample.exe.  This
# catches leftovers from a crashed run and (with Proton) the wrapper too.
# PID/PPID are skipped so the hook cannot kill the shell that invoked it.
kill_strays() {
    command -v pgrep >/dev/null 2>&1 || return 0

    local p strays=()
    while read -r p; do
        [[ -n "$p" ]] || continue
        [[ "$p" == "$$" || "$p" == "$PPID" ]] && continue
        strays+=("$p")
    # Match the process name, not the command line: Wine may rewrite argv[0]
    # to just "tsample.exe", and a command-line match could also hit an
    # unrelated shell/editor that merely mentions the filename.
    done < <(pgrep -x -- 'tsample\.exe' 2>/dev/null || true)
    ((${#strays[@]})) || return 0

    for p in "${strays[@]}"; do
        echo "lutris_nr2003: stopping stray tsample pid $p"
        kill "$p" 2>/dev/null || true
    done
    for _ in $(seq 1 20); do
        local alive=0
        for p in "${strays[@]}"; do
            is_running "$p" && alive=1
        done
        ((alive == 0)) && break
        sleep 0.1
    done
    for p in "${strays[@]}"; do
        is_running "$p" && kill -9 "$p" 2>/dev/null || true
    done
}

# Find the real game process (same check nr2003_qt uses: comm is nr2003.exe
# and its PE image is mapped at 0x00400000 with r-xp).  Prints the PID.
game_pid() {
    local d c pid
    for d in /proc/[0-9]*; do
        c="$(cat "$d/comm" 2>/dev/null || true)"
        c="${c,,}"
        [[ "$c" == "$GAME_PROC_NAME" ]] || continue
        pid="${d##*/}"
        if [[ -r "/proc/$pid/maps" ]] &&
           grep -q '00400000-.* r-xp ' "/proc/$pid/maps" 2>/dev/null; then
            printf '%s\n' "$pid"
            return 0
        fi
    done
    return 1
}

start_tsample() {
    local wine_bin="$1"
    local pid i

    : > "$LOG_FILE"
    echo "lutris_nr2003: starting $TSAMPLE_EXE"
    echo "lutris_nr2003:   WINE=$wine_bin"
    echo "lutris_nr2003:   WINEPREFIX=$WINEPREFIX"

    # No setsid --wait here: this runs from the already detached watcher, and
    # tsample must outlive the watcher.
    WINEPREFIX="$WINEPREFIX" nohup "$wine_bin" "$TSAMPLE_EXE" >>"$LOG_FILE" 2>&1 &
    pid=$!
    echo "$pid" > "$PID_FILE"

    for ((i = 0; i < WAIT_SHM_SEC * 10; i++)); do
        if [[ -f "$SNAP_SHM" && -f "$MEM_SHM" ]]; then
            echo "lutris_nr2003: SHM ready ($(stat -c %s "$SNAP_SHM") / $(stat -c %s "$MEM_SHM") bytes), pid $pid"
            return 0
        fi
        if ! is_running "$pid"; then
            echo "lutris_nr2003: tsample exited before creating SHM" >&2
            tail -n 20 "$LOG_FILE" >&2 || true
            rm -f "$PID_FILE" "$SNAP_SHM" "$MEM_SHM"
            return 1
        fi
        sleep 0.1
    done

    echo "lutris_nr2003: timed out waiting for $SNAP_SHM / $MEM_SHM" >&2
    tail -n 20 "$LOG_FILE" >&2 || true
    kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
    rm -f "$PID_FILE" "$SNAP_SHM" "$MEM_SHM"
    return 1
}

stop_watcher() {
    local wp=""
    [[ -f "$WATCH_PID_FILE" ]] && wp="$(cat "$WATCH_PID_FILE" 2>/dev/null || true)"

    if is_running "$wp"; then
        echo "lutris_nr2003: stopping watcher pid $wp"
        kill -- "-$wp" 2>/dev/null || kill "$wp" 2>/dev/null || true
    fi

    # Fallback if the PID file was lost.
    if command -v pgrep >/dev/null 2>&1; then
        local p
        while read -r p; do
            [[ -n "$p" ]] || continue
            [[ "$p" == "$$" || "$p" == "$PPID" ]] && continue
            echo "lutris_nr2003: stopping stray watcher pid $p"
            kill "$p" 2>/dev/null || true
        done < <(pgrep -f -- "$0 watch" 2>/dev/null || true)
    fi
    rm -f "$WATCH_PID_FILE"
}

watch_game() {
    local wine_bin="$1"
    local pid="" i

    echo "$$" > "$WATCH_PID_FILE"
    echo "lutris_nr2003: watching for $GAME_PROC_NAME (up to ${WATCH_GAME_SEC}s)" >>"$WATCH_LOG"

    for ((i = 0; i < WATCH_GAME_SEC * 2; i++)); do
        if pid="$(game_pid)"; then
            break
        fi
        sleep 0.5
    done

    if [[ -z "$pid" ]]; then
        echo "lutris_nr2003: game not found after ${WATCH_GAME_SEC}s; not starting tsample" >>"$WATCH_LOG"
        rm -f "$WATCH_PID_FILE"
        return 1
    fi

    echo "lutris_nr2003: game pid $pid detected" >>"$WATCH_LOG"
    sleep "$POST_GAME_DELAY"

    if ! is_running "$pid"; then
        echo "lutris_nr2003: game pid $pid exited before tsample could start" >>"$WATCH_LOG"
        rm -f "$WATCH_PID_FILE"
        return 1
    fi

    start_tsample "$wine_bin" >>"$WATCH_LOG" 2>&1 || true
    rm -f "$WATCH_PID_FILE"
}

start() {
    [[ -f "$TSAMPLE_EXE" ]] || die "missing $TSAMPLE_EXE (run: make -C $SCRIPT_DIR)"
    [[ -d /dev/shm ]] || die "/dev/shm is missing"

    local wine_bin
    wine_bin="$(resolve_tsample_wine)" || die \
        "cannot find Proton's wine binary; set TSAMPLE_WINE=/path/to/proton/files/bin/wine"
    check_wine "$wine_bin"

    # A previous watcher must not start tsample behind our back.
    stop_watcher

    # A crashed previous run may have left tsample (and its SHM) behind.
    kill_strays
    rm -f "$PID_FILE"
    rm -f "$SNAP_SHM" "$MEM_SHM"
    : > "$WATCH_LOG"

    echo "lutris_nr2003: armed; tsample starts after $GAME_PROC_NAME appears"
    echo "lutris_nr2003:   tsample wine: $wine_bin"
    echo "lutris_nr2003:   WINEPREFIX=$WINEPREFIX"

    # Detach so Lutris can launch the game immediately.  The watcher inherits
    # the variables it needs to re-read this script.
    export WINE WINEPREFIX TSAMPLE_EXE SNAP_SHM MEM_SHM WAIT_SHM_SEC
    export GAME_PROC_NAME WATCH_GAME_SEC POST_GAME_DELAY
    export PID_FILE LOG_FILE WATCH_PID_FILE WATCH_LOG TSAMPLE_WINE="$wine_bin"
    setsid "$0" watch "$wine_bin" >>"$WATCH_LOG" 2>&1 &
    echo "$!" > "$WATCH_PID_FILE"
    return 0
}

stop() {
    stop_watcher

    local pid=""
    [[ -f "$PID_FILE" ]] && pid="$(cat "$PID_FILE" 2>/dev/null || true)"

    if is_running "$pid"; then
        echo "lutris_nr2003: stopping tsample pid $pid"
        kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
        local _
        for _ in $(seq 1 30); do
            is_running "$pid" || break
            sleep 0.1
        done
        if is_running "$pid"; then
            kill -9 -- "-$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null || true
        fi
    fi

    # Fallback for a missing/stale PID file.
    kill_strays

    rm -f "$PID_FILE" "$WATCH_PID_FILE" "$SNAP_SHM" "$MEM_SHM"
    echo "lutris_nr2003: cleanup done"
}

status() {
    local wp="" pid=""
    [[ -f "$WATCH_PID_FILE" ]] && wp="$(cat "$WATCH_PID_FILE" 2>/dev/null || true)"
    if is_running "$wp"; then
        echo "watcher: running (pid $wp)"
    else
        echo "watcher: not running"
    fi

    [[ -f "$PID_FILE" ]] && pid="$(cat "$PID_FILE" 2>/dev/null || true)"
    if is_running "$pid"; then
        echo "tsample: running (pid $pid)"
    else
        echo "tsample: not running"
    fi

    local f
    for f in "$SNAP_SHM" "$MEM_SHM"; do
        if [[ -f "$f" ]]; then
            echo "$f: $(stat -c %s "$f") bytes"
        else
            echo "$f: missing"
        fi
    done
}

case "${1:-}" in
    start)  start ;;
    stop)   stop ;;
    status) status ;;
    watch)
        [[ -n "${2:-}" ]] || die "internal watch command needs a wine path"
        watch_game "$2"
        ;;
    *)
        echo "usage: $0 {start|stop|status}" >&2
        echo "  start  - pre-launch hook: arm watcher, start tsample when NR2003 appears" >&2
        echo "  stop   - post-exit hook: stop watcher/tsample and remove /dev/shm files" >&2
        exit 2
        ;;
esac
