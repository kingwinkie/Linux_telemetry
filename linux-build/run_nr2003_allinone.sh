#!/bin/bash
# run_nr2003_allinone.sh — one-shot Linux launcher for NR2003 + tsample snapshot+mem SHM
# Order matters: papytelemapp.h:92-100 — AppBegin creates the shared memory/queue before SimBegin.
# So tsample (AppBegin) MUST start before NR2003.exe. This script enforces that.
# tsample already bridges to Linux via Z:\dev\shm -> /dev/shm (nr2003_snapshot.h:4096 NR23 + nr2003_mem.h:4096 MEM3).
# No separate wine2linux needed.
#
# For Lutris, do NOT use this script: Lutris must launch NR2003 itself.  Use
# lutris_nr2003.sh start/stop as the pre-launch/post-exit hooks instead.
set -euo pipefail

# Resolve paths relative to this script, so it works from any directory.
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"

# Config — override via env or args
WINEPREFIX="${WINEPREFIX:-${HOME}/.wine}"
WINE="${WINE:-wine}"
TSAMPLE_EXE="${TSAMPLE_EXE:-${SCRIPT_DIR}/tsample.exe}"  # or ${SCRIPT_DIR}/tsample_csv.exe
NR2003_EXE="${NR2003_EXE:-}"                          # e.g. /mnt/games/NR2003/NR2003.exe
NR2003_ARGS="${NR2003_ARGS:-}"                        # e.g. -kh0 for NORMAL_PRIORITY_CLASS (§ tsample.cpp:847 -kh0 comment)
SNAP_SHM="/dev/shm/nr2003_telem"                      # NR23 4096
MEM_SHM="/dev/shm/nr2003_mem"                         # MEM3 4096
WAIT_SHM_SEC="${WAIT_SHM_SEC:-5}"                     # how long to wait for tsample to create SHM

if [[ -z "${NR2003_EXE}" && $# -ge 1 ]]; then NR2003_EXE="$1"; shift; fi
if [[ -z "${NR2003_EXE}" ]]; then
  echo "Usage: WINEPREFIX=~/.wine NR2003_EXE=/path/to/NR2003.exe $0 [/path/to/NR2003.exe]"
  echo "  Env: WINEPREFIX TSAMPLE_EXE NR2003_EXE NR2003_ARGS WAIT_SHM_SEC"
  echo "  TSAMPLE_EXE default ${SCRIPT_DIR}/tsample.exe (or tsample_csv.exe with -DFORMAT_CSV)"
  echo "  Example: NR2003_EXE=\"\$HOME/Games/NR2003/NR2003.exe\" $0"
  exit 1
fi
if [[ ! -f "${TSAMPLE_EXE}" ]]; then echo "Missing ${TSAMPLE_EXE} (run: make -C ${SCRIPT_DIR} )"; exit 1; fi
if [[ ! -f "${NR2003_EXE}" ]]; then echo "Missing NR2003_EXE=${NR2003_EXE}"; exit 1; fi

cleanup() {
  echo "[allinone] shutting down..."
  # kill wine children if still running (same wineserver)
  if [[ -n "${TS_PID:-}" ]] && kill -0 "${TS_PID}" 2>/dev/null; then kill "${TS_PID}" || true; fi
  if [[ -n "${GAME_PID:-}" ]] && kill -0 "${GAME_PID}" 2>/dev/null; then kill "${GAME_PID}" || true; fi
  # wineserver -k is too aggressive (kills prefix); just wait
  wait "${TS_PID:-}" 2>/dev/null || true
  wait "${GAME_PID:-}" 2>/dev/null || true
  rm -f "${SNAP_SHM}" "${MEM_SHM}"
  echo "[allinone] done. SHM cleaned."
}
trap cleanup EXIT INT TERM

echo "[allinone] WINEPREFIX=${WINEPREFIX}"
echo "[allinone] TSAMPLE=${TSAMPLE_EXE} -> ${SNAP_SHM} + ${MEM_SHM} (Z:\\dev\\shm)"
echo "[allinone] GAME=${NR2003_EXE} ${NR2003_ARGS}"

# fresh SHM (tsample will recreate with correct headers; stale headers would confuse pollMemCommands)
rm -f "${SNAP_SHM}" "${MEM_SHM}"
# ensure /dev/shm exists and is writable
if [[ ! -d /dev/shm ]]; then echo "/dev/shm missing"; exit 1; fi

# 1) Start tsample first — AppBegin creates queue; must succeed before SimBegin
echo "[allinone] (1/2) starting tsample..."
rm -f /tmp/tsample.log /tmp/nr2003_qt.log
# keep wineserver alive: start tsample in same prefix; log to /tmp/tsample.log for poll debug (MEM3 131072 seq/ack)
WINEPREFIX="${WINEPREFIX}" "${WINE}" "${TSAMPLE_EXE}" > /tmp/tsample.log 2>&1 &
TS_PID=$!
echo "[allinone] tsample pid ${TS_PID} (wine pid, host pid ${TS_PID}) — log /tmp/tsample.log + /tmp/nr2003_qt.log"

# Wait for SHM headers ( tsample.cpp:openSnapshotFile + openMemFile -> fwrite NR23/MEM3 )
echo -n "[allinone] waiting for ${SNAP_SHM}..."
for i in $(seq 1 $((WAIT_SHM_SEC*10))); do
  if [[ -f "${SNAP_SHM}" && -f "${MEM_SHM}" ]]; then echo " ok"; break; fi
  sleep 0.1
  if ! kill -0 "${TS_PID}" 2>/dev/null; then echo " tsample exited early!"; wait "${TS_PID}" || true; exit 1; fi
  echo -n "."
done
if [[ ! -f "${SNAP_SHM}" ]]; then echo " timeout waiting for ${SNAP_SHM}"; kill "${TS_PID}" || true; exit 1; fi
if [[ ! -f "${MEM_SHM}" ]]; then echo " timeout waiting for ${MEM_SHM}"; kill "${TS_PID}" || true; exit 1; fi
echo "[allinone] SHM ready: $(stat -c %s "${SNAP_SHM}") / $(stat -c %s "${MEM_SHM}") bytes"
# Optional: confirm AppBegin printed? tail
sleep 0.5

# 2) Start game — SimBegin will attach to existing queue
echo "[allinone] (2/2) starting NR2003..."
# Use same WINEPREFIX so Toolhelp FindNr2003Pid (CreateToolhelp32Snapshot) sees it
WINEPREFIX="${WINEPREFIX}" "${WINE}" "${NR2003_EXE}" ${NR2003_ARGS} &
GAME_PID=$!
echo "[allinone] NR2003 pid ${GAME_PID}"

echo "[allinone] running. tsample polls mem (0x400000+RVA fix) and writes ${SNAP_SHM} (4096B)."
echo "[allinone] From Linux: cat ${SNAP_SHM} | python3 -c 'import struct; ...' or python3 ${SCRIPT_DIR}/live_memory_shm.py write ../csv/fair_chassis.csv"
echo "[allinone] Ctrl-C to stop both and clean SHM."
echo ""

# Monitor: if either exits, tear down the other (tsample loop exits when AppCheckIfSimActiveQ false)
wait "${GAME_PID}" || true
GAME_RC=$?
echo "[allinone] NR2003 exited rc=${GAME_RC}"
# Give tsample a moment to drain AppWaitForNewSample loop (tsample.cpp:979 100ms timeout + closeSnapshotFile)
sleep 1
if kill -0 "${TS_PID}" 2>/dev/null; then
  echo "[allinone] stopping tsample..."
  kill "${TS_PID}" || true
  wait "${TS_PID}" || true
fi
exit ${GAME_RC}
