# Lutris Integration

Use `linux-build/lutris_nr2003.sh` as a pair of hooks around the normal
NR2003 Lutris entry. Lutris still launches the game itself; the pre-launch
hook arms a watcher, the watcher starts `tsample.exe` in the game's own Wine
session once `NR2003.exe` appears, and the post-exit hook stops everything and
cleans up `/dev/shm/nr2003_*`.

## Quick setup

Open the NR2003 entry in Lutris and go to
**Configure → System options → Game execution**:

| Field | Value |
|---|---|
| Pre-launch script | `/path/to/telemetry_linux/linux-build/lutris_nr2003.sh start` |
| Wait for pre-launch script completion | **enabled** |
| Post-exit script | `/path/to/telemetry_linux/linux-build/lutris_nr2003.sh stop` |

Enter the paths **without quotes**; Lutris treats a quoted string as a single
filename and logs `Command ... not found`.

`start` arms the watcher and returns immediately, so keeping **Wait for
pre-launch script completion** enabled does not delay the game. The SHM files
appear a few seconds after `NR2003.exe` starts, not before it.

`tsample.exe` and `lutris_nr2003.sh` live in `linux-build/`. If you move the
project, update the two hook paths in Lutris.

## Full guide

See [`linux-build/LUTRIS.md`](linux-build/LUTRIS.md) for:

- why the watcher starts `tsample` after the game (Proton/`umu-run` deadlock)
- the YAML equivalent
- manual testing and status/stop commands
- environment overrides
- troubleshooting (timeouts, stale processes, Proton/`umu-run`, cleanup)
