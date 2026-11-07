NR2003 Telemetry App - Linux Cross-Compile Build
=================================================

This directory contains everything needed to build the NR2003
telemetry sample application for Windows using a MinGW cross-compiler
on Linux.

Prerequisites
-------------
Install the MinGW cross-compiler:

    # Debian/Ubuntu
    sudo apt install g++-mingw-w64-i686

    # Fedora
    sudo dnf install mingw32-gcc-c++

    # Arch
    sudo pacman -S mingw-w64-gcc

Building
--------
Simply run `make` to build both the standard and CSV versions:

    make

Output:
  tsample.exe          - Standard console output (human-readable)
  tsample_csv.exe      - CSV output (for spreadsheet import)

To build statically-linked versions (no DLL dependencies):

    make static

Output:
  tsample_static.exe
  tsample_csv_static.exe

To clean:

    make clean

Running
-------
These are Windows PE executables. The launcher finds `tsample.exe` next
to itself and takes the game executable as an argument:

    ./run_nr2003_allinone.sh "/path/to/Nr2003.eXe"

To run a telemetry executable directly under Wine:

    wine tsample.exe
    wine tsample_csv.exe

The app will wait for NR2003 to start. Once the sim is running, it
will begin printing telemetry data.

Lutris
------
Lutris must launch NR2003 itself, so `run_nr2003_allinone.sh` cannot be
used as its pre-launch script (that would start a second copy of the
game).  Use the hook script instead.  See [LUTRIS.md](LUTRIS.md) for
the full setup and troubleshooting guide.

In the Lutris entry for NR2003, open **Configure -> System options ->
Game execution** and set:

    Pre-launch script:  /path/to/telemetry_linux/linux-build/lutris_nr2003.sh start
    [x] Wait for pre-launch script completion
    Post-exit script:   /path/to/telemetry_linux/linux-build/lutris_nr2003.sh stop

The `start` hook arms a detached watcher and exits at once, so Lutris can
launch the game. When `NR2003.exe` appears the watcher starts `tsample.exe`
with the prefix's own `wine` binary, then waits for `/dev/shm/nr2003_telem`
and `/dev/shm/nr2003_mem`. This ordering matters with Proton/`umu-run`: two
sessions on one `WINEPREFIX` deadlock, so `tsample` joins the game's existing
wineserver instead of starting a second one. The `stop` hook stops the
watcher/`tsample` and removes the SHM files. SHM appears a few seconds after
the game starts, not before it.

You can test or clean up manually:

    ./lutris_nr2003.sh status
    ./lutris_nr2003.sh stop

`tsample.exe` must be in this directory (or set `TSAMPLE_EXE`).  Both
processes must use the same Wine prefix, which Lutris guarantees because
`WINE` and `WINEPREFIX` are exported to the hooks.

If you only have `papytelemapp.obj` (the original MSVC-compiled
object), you can regenerate `papytelemapp_renamed2.obj` from this
directory:

    make regenerate-obj

This requires `papytelemapp.obj` (not included here - copy from the
original telemetry SDK distribution) and `objcopy` (part of binutils).

Files
-----
tsample.cpp                 - Main telemetry sample application
papytelemapp.h              - Telemetry API header
papytelemapp_renamed2.obj   - MSVC .obj with GCC-renamed symbols
msvc_compat.cpp             - MSVC operator delete C++ stub
msvc_stubs.s                - MSVC operator delete assembly stub
redef.txt                   - Symbol remapping for objcopy
run_nr2003_allinone.sh      - Standalone launcher (takes the NR2003 executable path)
lutris_nr2003.sh            - Lutris pre-launch/post-exit hook (start/stop)
LUTRIS.md                   - Lutris setup, testing, and troubleshooting
nr2003_qt/                  - Qt6 GUI and its build directory
Makefile                    - Build rules
README.md                   - This file

CSV files used by the GUI and Python tools live in ../csv/.
Set NR2003_CSV_DIR to override that location.
