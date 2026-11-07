# Lutris Integration

`lutris_nr2003.sh` lets Lutris launch NR2003 normally while `tsample.exe`
gathers telemetry in the background. It is a **pre-launch / post-exit hook**,
not a replacement game launcher.

## Why the hook waits for the game

Lutris's Proton / `umu-run` runner (GE-Proton) gives every `umu-run` invocation
its own pressure-vessel and wineserver. Two sessions on the same `WINEPREFIX`
deadlock: if `tsample`'s `umu-run` is still alive when Lutris starts NR2003, the
game's wineserver blocks forever and the game never appears.

The hook therefore uses a two-stage start:

1. The pre-launch hook stops stale helpers, removes old SHM files, and arms a
   small detached watcher. It returns immediately so Lutris can start the game.
2. The watcher waits for the real `NR2003.exe` process, then starts
   `tsample.exe` with Proton's own `wine` binary (**not** a second `umu-run`).
   That client joins the game's existing wineserver, so there is only one Wine
   session.
3. `tsample` creates `/dev/shm/nr2003_telem` and `/dev/shm/nr2003_mem`.
4. On game exit, the post-exit hook stops the watcher/`tsample` and removes the
   SHM files.

The SHM files therefore appear a few seconds **after** the game starts, not
before. `nr2003_qt` and the Python tools poll for them, so they can be started
before or after the game.

## Why not use `run_nr2003_allinone.sh`?

`run_nr2003_allinone.sh` starts **both** `tsample.exe` and NR2003 itself.
Lutris must start NR2003 itself so its Wine prefix, DXVK, controller, and
display settings still apply. Using the all-in-one as a Lutris pre-launch
script would start a second copy of the game.

## Requirements

- `tsample.exe` must be in this directory, beside `lutris_nr2003.sh`.
  Build it with `make` if needed. Override with `TSAMPLE_EXE` if it lives
  elsewhere.
- `tsample` and NR2003 must run in the **same Wine prefix**. The hook gets the
  prefix from Lutris's `WINEPREFIX`, so this is automatic.
- `lutris_nr2003.sh` must be executable.

## Setup

Open the NR2003 entry in Lutris and go to:

**Configure → System options → Game execution**

Set:

| Field | Value |
|---|---|
| Pre-launch script | `/path/to/telemetry_linux/linux-build/lutris_nr2003.sh start` |
| Wait for pre-launch script completion | **enabled** |
| Post-exit script | `/path/to/telemetry_linux/linux-build/lutris_nr2003.sh stop` |

Enter the paths **without quotes**. If the field contains quotes around the
command, Lutris treats the whole quoted string as a filename and logs
`Command ... not found`.

The **Wait for pre-launch script completion** checkbox can stay enabled. The
`start` command arms the watcher and returns at once, so it does not delay the
game. (It no longer gates the game on SHM creation; see above.)

Save, then launch the game from Lutris as usual.

## YAML equivalent

Lutris stores the same settings under `system:` in the game's config file:

```yaml
system:
  prelaunch_command: /path/to/telemetry_linux/linux-build/lutris_nr2003.sh start
  prelaunch_wait: true
  postexit_command: /path/to/telemetry_linux/linux-build/lutris_nr2003.sh stop
```

The working entry in this workspace also pins the runner:

```yaml
wine:
  version: ge-proton
```

Prefer the Lutris GUI; editing YAML while Lutris is running can be
overwritten. If the file already has a `system:` section, add the three keys
to it rather than adding a second section.

## Manual use

The hook can be checked from a terminal. Lutris normally supplies `WINE`,
`WINEPREFIX` and `PROTONPATH`; set them yourself when testing:

```bash
cd /path/to/telemetry_linux/linux-build

WINE=/path/to/umu-run \
WINEPREFIX=/path/to/prefix \
PROTONPATH=GE-Proton \
./lutris_nr2003.sh start

# start the game yourself, then:
./lutris_nr2003.sh status
./lutris_nr2003.sh stop
```

`start` returns immediately; `status` first shows the watcher as running, then
shows `tsample` and the SHM sizes once the game appears. `stop` also works
without the original environment because it uses the saved PID files and
matches the `tsample.exe` process name.

Useful environment overrides:

| Variable | Default | Purpose |
|---|---|---|
| `TSAMPLE_WINE` | auto-detected | Wine binary used for `tsample`; set it if Proton detection fails |
| `TSAMPLE_EXE` | this directory's `tsample.exe` | Alternate telemetry build |
| `NR2003_PROC_NAME` | `nr2003.exe` | Process name the watcher waits for |
| `NR2003_WATCH_GAME_SEC` | `300` | Seconds to wait for the game |
| `NR2003_POST_GAME_DELAY` | `2` | Delay after detection before starting `tsample` |
| `WAIT_SHM_SEC` | `15` | Seconds to wait for both SHM files after starting `tsample` |
| `NR2003_TSAMPLE_PID_FILE` | `/tmp/nr2003_tsample.pid` | `tsample` PID file |
| `NR2003_TSAMPLE_LOG` | `/tmp/nr2003_tsample.log` | `tsample` output |
| `NR2003_WATCH_PID_FILE` | `/tmp/nr2003_watchdog.pid` | Watcher PID file |
| `NR2003_WATCH_LOG` | `/tmp/nr2003_watchdog.log` | Watcher output |
| `SNAP_SHM` | `/dev/shm/nr2003_telem` | Telemetry snapshot file |
| `MEM_SHM` | `/dev/shm/nr2003_mem` | Memory command channel |

## Troubleshooting

### Lutris log says `Command ... not found`

The hook path is quoted in Lutris. Remove the quotes from both fields and from
the YAML (`'''...'''` is wrong).

### Game does not start, or hangs at launch

A stale watcher/`tsample` is probably holding the prefix. Run:

```bash
./lutris_nr2003.sh stop
```

Then launch again. Do **not** start `tsample` through `umu-run` as a
pre-launch step; that is the deadlock this watcher avoids.

### `timed out waiting for /dev/shm/nr2003_telem`

- Check `/tmp/nr2003_watchdog.log` first: did it find `NR2003.exe`?
- Check `/tmp/nr2003_tsample.log` for the Wine/`tsample` error.
- Confirm the auto-detected Proton wine exists; if not, set
  `TSAMPLE_WINE=/path/to/GE-Proton.../files/bin/wine`.
- Confirm `tsample.exe` exists and was rebuilt with `make`.
- Check that `/dev/shm` exists and is writable.

### `tsample exited before creating SHM`

Run the hook manually with the same `WINE`, `WINEPREFIX`, `PROTONPATH` to see
the full log. A static build (`tsample_static.exe`) can help if the prefix is
missing MinGW runtime DLLs.

### A stale watcher or `tsample` is still running

`start` stops any previous watcher and stray `tsample.exe` before arming, but
you can clear them explicitly:

```bash
./lutris_nr2003.sh stop
```

Then launch again.

### Game runs but no telemetry

- Run `./lutris_nr2003.sh status` while the game is running and confirm both
  SHM files exist.
- `seq 0` at the main menu is normal; telemetry updates once you are in a race
  session.
- Make sure you set the hooks on the Lutris entry you are actually launching.
- If you changed the Lutris Wine version, restart the game so the hooks are
  re-armed.

### Cleanup after a crash or forced kill

```bash
./lutris_nr2003.sh stop
```

`stop` finds `tsample.exe` by process name even if the PID file was lost. As a
last resort, after confirming no game or telemetry process is running:

```bash
rm -f /dev/shm/nr2003_telem /dev/shm/nr2003_mem
```

### Plain Wine instead of Proton

Plain Wine runners work too. The watcher detects that `WINE` is a plain wine
binary and uses it directly, so the game and `tsample` still share one
wineserver.

## Files

| File | Purpose |
|---|---|
| `lutris_nr2003.sh` | `start` / `stop` hooks and the watcher |
| `tsample.exe` | Telemetry gatherer started by the watcher |
| `run_nr2003_allinone.sh` | Standalone launcher; not used by Lutris |

Logs from the hook go to `/tmp/nr2003_watchdog.log` and
`/tmp/nr2003_tsample.log`. The GUI and Python tools remain native Linux
programs and read `/dev/shm` directly, so they do not need to be launched
through Lutris.
