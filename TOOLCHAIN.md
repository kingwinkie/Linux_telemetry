# NR2003 Telemetry & Live-Memory Toolchain

Linux/Wine toolchain for NASCAR Racing 2003 Season (`Nr2003.eXe`, 32-bit).
Three cooperating pieces bridge Papyrus telemetry **and** live game-memory
read/write between Wine and native Linux.

```
  NR2003.exe (Win32, 32-bit)            tsample.exe (Win32, in Wine)      nr2003_qt (native Linux)
  ┌───────────────────────┐              ┌──────────────────────┐          ┌──────────────────┐
  │ PapyTelem sim data    │──shm──►      │ pollMemCommands()    │◄──shm─── │ Telemetry tab    │
  │ .car tables in .data  │              │ OpenProcess + RPM/   │  MEM3    │ Live Mem tab     │
  │                       │              │ WPM via cached       │  BLOCK*  │ EXE File tab      │
  └───────────────────────┘              │ FindWindowA handle   │          └──────────────────┘
                                         └──────────┬───────────┘
                                                    │ Z:\dev\shm  ⇄  /dev/shm
                                            nr2003_telem (NR23, 4096 B)
                                            nr2003_mem   (MEM3, 131072 B)
```

Everything the native side needs crosses the Wine boundary as a **plain file
under `/dev/shm`**, so no Win32 API is required outside Wine.

---

## 1. Quick start

```bash
cd telemetry_linux/linux-build

# 1. build (tsample is a Win32 exe, cross-compiled with MinGW)
make -B
cmake -S nr2003_qt -B nr2003_qt/build --fresh && cmake --build nr2003_qt/build
cp nr2003_qt/build/nr2003_qt nr2003_qt/nr2003_qt

# 2. run telemetry gatherer + game (order matters, see §2)
#    Lutris users: use the lutris_nr2003.sh hooks in §2 instead.
./run_nr2003_allinone.sh "/path/to/Nr2003.eXe"

# 3. GUI (NR2003_EXE only sets the EXE tab's default path)
NR2003_EXE="/path/to/Nr2003.eXe" ./nr2003_qt/nr2003_qt
```

All CSV data used by the app lives in `telemetry_linux/csv/`. Override the
location with `NR2003_CSV_DIR` if needed.

### Changing a car-setup value live

1. **Live Mem → Chassis** tab → **Read from memory** (87 `BLOCKR` round trips)
2. Single-click the row you want. The status strip shows
   `row N  RVA 0x…  [Sing]  <label>  (<current value>)`
3. Type the new number into the **new value** spin box
4. **Apply to selected row** → `logQt("apply rva 0x32355c = 3000  (row N, …)")`
5. **Write my edits (BLOCKM)** → `logQt("writeBlocks VERIFY userEdits=1 checked=1 landed=1")`

`landed=1` means the float was read back out of NR2003's address space and
matched. `notLanded=1` names the offending RVA.

> **Do not type into the table cell.** Qt6's `QTableWidget` inline editor never
> commits into the item model here (`closeEditor(QWidget*,…)` and
> `commitData(QWidget*)` are `protected Q_SLOTS`), so the edit would be
> invisible to both the diff and `itemChanged`. The **Apply to selected row**
> button bypasses the delegate entirely and is the supported path.

### RVA parsing gotcha

Column 0 of every live-keyed table is rendered as `0X32355C` (uppercase, via
`QString("0x%1").arg(rva,8,16).toUpper()`). **`QString::toUInt(&ok,16)` does
not accept a `0X` prefix**, so every RVA parse in `mainwindow.cpp` must go
through the single `parseRva()` helper. This was the cause of the
`Cannot parse RVA from +rI->text()+` dialog (which was itself a
string-literal-concatenation bug) and of `edited cells: 0` in an earlier build.

### Saving what you changed

**Save live values (Mem CSV)** writes `Live - <Mod> - Mem.csv` for Chassis /
Engine / Wheel into a folder you pick (default `csv/`), using the
**same transposed layout as the originals** — so it round-trips with §6. Values
come from the current table cells, i.e. live values *plus* your applied edits.
MemLoc has no original CSV and is not written out.

---

## 2. Startup order is mandatory

`papytelemapp.h:92` — `AppBegin()` *creates* the shared-memory area and request
queue; the sim's `SimBegin()` only attaches if that memory already exists. So
`tsample.exe` must be running **before a race session starts** (before the sim
calls `SimBegin()`), not necessarily before the game process launches.
Starting it while the game sits at the main menu is fine.
`run_nr2003_allinone.sh` uses the stricter order:

1. `rm -f /dev/shm/nr2003_*`
2. start `wine ./tsample.exe &` (it opens both SHM files and then waits for the sim)
3. poll up to 5 s for both SHM headers to appear
4. start `wine "$NR2003_EXE"` in the **same `WINEPREFIX`**
5. `trap EXIT` tears down both processes and removes the SHM files

Because the game and the gatherer share a Wine prefix, `tsample` can find it
with `FindWindowA("NR2003")` + `GetWindowThreadProcessId` — the same trick
`tester.exe` uses — and cache one `PROCESS_ALL_ACCESS` handle instead of
re-enumerating processes.

### Lutris

Lutris must launch NR2003 itself, so `run_nr2003_allinone.sh` cannot be its
pre-launch script — that would start a second game. Use the hook script
instead. In the Lutris entry: **Configure → System options → Game execution**:

| Field | Value |
|---|---|
| Pre-launch script | `/path/to/telemetry_linux/linux-build/lutris_nr2003.sh start` |
| Wait for pre-launch script completion | **enabled** |
| Post-exit script | `/path/to/telemetry_linux/linux-build/lutris_nr2003.sh stop` |

Lutris exports `WINE` and `WINEPREFIX` to both hooks. The `start` hook arms a
detached watcher and returns immediately; once `NR2003.exe` appears the watcher
starts `tsample.exe` with the prefix's own `wine` binary and waits for both SHM
files. This avoids the Proton/`umu-run` deadlock: a second `umu-run` on the same
prefix blocks the game, while the prefix's direct `wine` joins the game's
existing wineserver. `stop` kills the watcher/`tsample` and removes
`/dev/shm/nr2003_*`. Because both processes use the same prefix, window
discovery works exactly as above. SHM appears a few seconds after the game
starts, not before it; `nr2003_qt` and the Python tools poll for it.

`tsample.exe` must be beside `lutris_nr2003.sh` (override with `TSAMPLE_EXE`).
The script also accepts manual use: `./lutris_nr2003.sh status` and `stop`.
See [LUTRIS.md](LUTRIS.md) for the quick setup and
[linux-build/LUTRIS.md](linux-build/LUTRIS.md) for the full guide.

---

## 3. Files

### Shared headers (kept in sync between `telemetry_linux/` and `linux-build/`)

| File | Size | Purpose |
|---|---|---|
| `nr2003_snapshot.h` | 4096 B, `#pragma pack(1)` | `NR23` telemetry record: header + chassis/wheel/tire/driveline/driver-input/gauge + 48 opponents. Written every sample at 36 Hz. |
| `nr2003_mem.h` | 131072 B | `MEM3` command record: 8×`uint32` header + 2978 × 44 B entries. The whole read/write protocol. |
| `nr2003_blocks.h` | ~18 KB | **Generated** (`ghidra/gen_nr2003_blocks.py`). 87 per-car block descriptors + field-label arrays. Do not hand-edit. |

### `csv/` — self-contained data

| File | Purpose |
|---|---|
| `fair_{chassis,engine,wheel,garage}.csv` | Static-EXE `&H` reference tables for the **EXE File** tab and initial table population. |
| `Original - {Chassis,Engine,Wheel} - Mem.csv` | Transposed live-memory defaults used by the **Live Mem** tab. |

The GUI and `live_memory_shm.py` search this folder automatically. Set
`NR2003_CSV_DIR` to use a different folder. `Live - *.csv` and `MemLoc_*.csv`
files written by the GUI are also saved here by default. Telemetry CSV logs use
a separate `logs/` folder beside the GUI binary (override with
`NR2003_LOG_DIR`); see the **Telemetry** tab below.

### `tsample.cpp` → `tsample.exe`

PapyTelem gatherer *and* MEM3 server. Adds `openSnapshotFile/writeSnapshot`,
`openMemFile/closeMemFile`, `findNr2003Pid` (`FindWindowA` first, Toolhelp
fallback), `nr2003Process`/`nr2003Detach` (cached handle), `readBlock`,
`writeBlock`, `writeMemBack` and `pollMemCommands`. `pollMemCommands()` is
called from the pre-race wait loop (100 ms), the active sample loop, and idle
polls, so commands are served whether or not the sim is running.

### `nr2003_qt/` — Qt6 GUI

`CMakeLists.txt`, `mainwindow.h`, `mainwindow.cpp`. Three top-level tabs:

* **Telemetry** — polls `nr2003_telem` every 32 ms (16 ms while logging so no
  36 Hz sample is missed), guards on the sequence number, shows speed in mph.
  The log bar can write the stream to CSV:
  **Start logging** / **Stop logging** toggles recording, **Select fields...**
  opens a checkable tree (group rows toggle their children) for choosing the
  columns, and **Open log folder** opens the destination. `wall_time` is always
  the first column; each row is one new `(sequence, tick)` sample, so the UI
  poll rate cannot duplicate data. Fields are locked while logging — stop
  first, then change the selection. Logs default to `logs/` beside the
  `nr2003_qt` binary and are named `telemetry_YYYYmmdd_HHMMSS.csv`; set
  `NR2003_LOG_DIR` to override the directory. Defaults include the header,
  chassis, wheel, tire, driveline, driver-input and gauge groups plus the
  opponent count, while track normal/heading vectors and the per-opponent JSON
  detail column are off.
* **Live Mem** — five inner tabs (Chassis / Engine / Wheel / Garage / MemLoc)
  keyed on **live block RVAs**; `Read from memory`, `Write my edits (BLOCKM)`,
  `Save live values (Mem CSV)`, a `QDoubleSpinBox` + **Apply to selected row**,
  and a `QLockFile /tmp/nr2003_qt.lock` singleton.
* **EXE File** — offline file editing of the static `Nr2003.eXe`. This is where
  the `fair_*.csv` tables live, because their `&H` values are *static EXE file
  addresses*, not live addresses (§6). `&H` is 1-based, so the byte offset is
  `&H - 1`; no PE RVA → file mapping is involved. `Load fair_*.csv` /
  `Read from EXE` / `Write values to EXE` are type-aware (`Sing` = float32,
  `Long` = int32, `Doub` = float64), make a one-time `.bak`, and verify every
  write by reading the bytes back. See §11.

### `live_memory_shm.py` — headless CLI

`memcsv`, `verifyblocks [-v]`, `blocktable`, `blocks [csv]`, `readblocks`,
`set`, `write`, `status`. Same SHM protocol, no GUI. Useful for scripted
verification and for the bulk "write all 3210 Mem-CSV values" job that the GUI
deliberately does *not* do.

### `run_nr2003_allinone.sh`

Launcher described in §2. Tees `tsample` output to `/tmp/tsample.log`.
For Lutris, use the hooks below instead.

### `lutris_nr2003.sh`

`start` / `stop` hooks for a Lutris-launched NR2003 (see §2). `start` arms a
watcher that starts `tsample.exe` once `NR2003.exe` appears, using the prefix's
own `wine` binary and `WINEPREFIX`; `stop` kills the watcher/`tsample` and
removes `/dev/shm/nr2003_*`. Logs go to `/tmp/nr2003_watchdog.log` and
`/tmp/nr2003_tsample.log`; PIDs to `/tmp/nr2003_watchdog.pid` and
`/tmp/nr2003_tsample.pid`. Full guide: `linux-build/LUTRIS.md`.

---

## 4. SHM protocol

### `nr2003_telem` — `NR23` (4096 B)

`magic 0x3332524E` · `version 1` · `size` · `tailMagic 0xEBC4A698`, then the
sample. Written with `CreateFileA("Z:\\dev\\shm\\nr2003_telem")` +
`SetFilePointer`/`WriteFile`/`FlushFileBuffers` (deliberately **not** `FILE*`
stdio — buffered `FILE*` and the client's `pwrite` fight over the same file and
produce stale reads).

### `nr2003_mem` — `MEM3` (131072 B)

Header: `magic 0x334D454D, version 1, size, seq, count, ackSeq, status, tail`.
Entries: `{ uint32 rva; float value; uint32 type; char label[32] }` = 44 B.

| `status` | Name | Meaning |
|---|---|---|
| `0x00000000` | `WRITE` | per-entry `WriteProcessMemory` at `0x400000 + rva` |
| `0x7FFFFFFE` | `READ` | per-entry `ReadProcessMemory`, results into `entries[].value` |
| `0x7FFFFFFD` | `BLOCKW` | `entries[0].rva` = base, `entries[1..]` = floats, one `WriteProcessMemory` |
| `0x7FFFFFFC` | `BLOCKR` | one `ReadProcessMemory` of the whole block |
| `0x7FFFFFFB` | `BLOCKM` | read-modify-write: read the block, patch only listed slots, write once |

`NR2003_MEM_VAL_KEEP 0xFFFFFFFF` marks a slot the client did not supply.

**Result-window fix:** the ack re-serializes only `count` entries, so the
server sets `cmd.count = n + 1` before writing back. Without this the last
float of every block was silently dropped.

---

## 5. How the 87 blocks were found

Reverse-engineered from `tester/tester.exe` (a VB6 "Physics Tester"). Its
entire Win32 surface is six late-bound thunks: `FindWindowA`,
`GetWindowThreadProcessId`, `OpenProcess`, `ReadProcessMemory`,
`WriteProcessMemory`, `CloseHandle` — **no `VirtualProtectEx`**, which is why
it never needed one on real Windows.

Its read loops are plain `READ4` helpers, and the callers reveal that the data
is **per-car contiguous blocks**, not a flat address list:

| Module | Blocks | Floats/block | Base RVA | Stride | Section |
|---|---|---|---|---|---|
| Chassis | 22 | 114 | `0x323550` | `0x1C8` | `.data` (writable) |
| Engine | 16 | 28 | `0x2EECA8` | `0x70` | **`.rdata` (read-only)** |
| Wheel | 26 | 36 | `0x4B0C40` | `0x90` | `.data` |
| MemLoc | 23 | 40 | `0x31F908` | `0xA0` | `.data` |

Two bases break their stride and are flagged `NR2003_BLOCK_FLAG_OUTLIER`:
chassis `0x4945B8` (`cup-0`) and wheel `0x4B1930` (`whl-25`). They are **extra
entries**, not stride slots — letting `cup-0` consume a `0x1C8` slot drops the
chassis match rate from 73 % to 63 %.

Full analysis: `tester/TESTER_MEM_MODEL.md`.

---

## 6. CSV ↔ live-memory mapping (verified)

`csv/Original - <Mod> - Mem.csv` is **transposed**: one row per
field, one column per car variant. The contract, all confirmed against live
memory:

* `block k` ↔ **CSV column k in FILE order** (not sorted)
* `slot i`  ↔ CSV row `Field(i + desc.fieldOffset)`, offset **1** chassis/wheel,
  **3** engine
* the value sits at raw index **`csvCol + 2`** (index 0 = `Field00`,
  index 1 = the human label)
* with `fieldrows = rows.mid(1)`, `Field(n)` is at `fieldrows[n-1]`, so the row
  index is `(fieldOffset + slot) - 1`

`live_memory_shm.py verifyblocks` (1e-3 relative tolerance, NaN==NaN):

```
[vfy] total 3195/3892 = 82.1%
[vfy]   CHASSIS  1833/2508 = 73.1%
[vfy]   ENGINE   426/448  = 95.1%
[vfy]   WHEEL    936/936  = 100.0%
```

Every remaining difference is a documented representation quirk, not a mapping
error:

* chassis slots `0,1,19,20,21,22,39` — 0-based integer indices stored as
  uint32 bits, so `memory = CSV − 1`
* chassis slots `59..81` (23) — Mem Loc pointers stored as uint32 bits
  reinterpreted as float
* chassis slot `110` — `NaN` in memory vs `-1` ("not set") in the CSV
* engine slots `17, 27` — the two int-as-float slots

> ### `fair_*.csv` is NOT a live address source
> `csv/fair_chassis.csv` (1520 rows) and friends carry `&H` values in
> the range `0x2C1DE..0x2EE5D9` / `0x554E5..0x2F039C`. These are **1-based
> static-file addresses into `Nr2003.eXe`**, not runtime RVAs: the byte offset
> is `&H - 1`. They only *look* like RVAs inside `.rdata` (which happens to
> have `raw == RVA`, delta 0). They can never match a live `.data` block, which
> is why the Live Mem tables were re-keyed onto block RVAs and the `fair_*`
> files are now reference data for the EXE File tab only. The same `&H - 1`
> rule applies when deriving a live RVA (after stripping the `0x400000` image
> base); it is only the file write that must not use a PE section walk.
> A related trap: a bare `toUInt(&ok, 16)` on those strings *fails*, which is
> what silently produced "no cells were changed".

---

## 7. Crash classes and the guards against them

NR2003 died twice during development. Both are the same underlying hazard and
both are now defended at two layers.

### 7.1 `0x00432e47  flds (%edx)` with `EDX = 0`

```
EIP:00432e47  EAX:40400000  ECX:00723550  EDX:00000000  ESI:00723550
0x00432e47 nr2003+0x32e47: flds (%edx)
```

`ECX/ESI` = `0x00723550`, the chassis `cup-5` block base — the game
dereferenced a slot in that block that had been zeroed. Writing `0.0f`
produces the bit pattern `0x00000000`, and if the slot held an *index* or a
*pointer* that is a null dereference.

| Layer | Guard |
|---|---|
| Client (`loadMemCsvValues`, `loadMemCsvTable`, `writeBlocks`) | never sends ptr slots (`0x3B..0x51`) or the known uint32-bit slots `{0,1,19,20,21,22,39,110}` for chassis-shaped blocks; the 682 affected cells are shown non-editable with a dark background |
| Server (`pollMemCommands` BLOCKM) | `kIntSlots[]` check **plus** an index-independent denormal net: `if (buf[i] > -1e-30f && buf[i] < 1e-30f) { denormKept++; continue; }` |

The denormal net is the general cure: **no legitimate NR2003 parameter is
denormal**, so a denormal float in a live table unambiguously means the slot
holds uint32 bits. In the final run it caught **11 additional hidden
uint32-bit slots per chassis block** that the client did not know about
(`patched 83` sent, `72` applied).

### 7.2 `err 998 ERROR_NOACCESS` on `.rdata`

Wine's `NtWriteVirtualMemory` *does* enforce page protection, unlike Windows.
`.rdata` is `PAGE_EXECUTE_READ`, so the 16 engine blocks (448 values) and the
whole `garage` table can never be written live. `VirtualProtectEx` was tried
and crashed the game again, so it is gated to `!AppCheckIfSimActiveQ()`
(pre-race only) and those cells are locked in the UI with a tooltip pointing at
the offline patcher. Use the GUI's **EXE File** tab (`Load fair_*.csv` →
`Read from EXE` / `Write values to EXE`); it performs the same static-EXE patch.

### 7.3 Two Wine/POSIX traps

* **Never truncate `/dev/shm/nr2003_mem`.** `QFile` with `WriteOnly` truncates
  and invalidates the Wine `MapView`, killing the session. Use `O_RDWR` +
  `pwrite` + `fsync`.
* **Ack polls must re-read the whole file each iteration.** Reusing one handle
  and seeking back can return stale bytes and time out even though `tsample`
  acked in ~100 ms.

---

## 8. Block opcodes — why 64 calls, not 2924

`tester.exe` wrote 4 bytes at a time, one `OpenProcess`/`CloseHandle` pair per
value. That is catastrophic over Wine and racy against the physics loop. The
block opcodes do **one** `ReadProcessMemory` + **one** `WriteProcessMemory` per
per-car block:

```
writeBlocks done ok=48 fail=0 rdataSkipped=16 empty=23 patched=2762 ptrKept=506
```

`rdataSkipped=16` = the engine blocks (offline patcher). `empty=23` = memloc
(no Mem CSV exists for them). `ptrKept=506` = `22 × 23` pointer slots left
untouched. Measured wall time for all 48 blocks: ~5 s.

`patched` counts the slots we *sent*; the server may write fewer because of
the denormal net. The GUI reports both.

---

## 9. Environment gotchas

| Gotcha | Detail |
|---|---|
| `sed -i` | Fails in this tree (`Operation not permitted`). Rewrite files with a python `open(p,"w")`. |
| Background jobs | `nohup … &` inside a Bash tool call dies at the 120 s timeout. Use `setsid … &` then `sleep 3; echo started`. |
| Replacing a running binary | `cp` → `Text file busy`. Use `cp build/x ./x.new && mv -f ./x.new ./x`. |
| `QLockFile` | A second `nr2003_qt` instance silently refuses to start and writes **no log**. Close the first one. |
| MinGW link flags | Without `-static-libgcc -static-libstdc++` Wine fails with `libgcc_s_dw2-1.dll … c0000135`. |
| Ghidra headless | >120 s, so background it; byte-safe writes only (`UnicodeEncodeError` under Jython); always dump a listing fallback for large functions; one run per project (`LockException`). |

### 9.1 Direct3D cars render black under stock Wine

Symptom: the same game folder renders correctly when Lutris/GE-Proton launches
it, but under stock Wine (`~/.wine`, `wine-11.0`) every visible car texture is
black while the track and infield are normal.

**Check the adapter first, not the wrapper.** In `new_org`, the app-local
`d3d8.dll` / `d3d9.dll` are the same D8VK 3.1.1 + DXVK 3.1.1 pair the Lutris
prefix uses (hashes match), and `Direct3DCreate8` returns a valid device under
stock Wine. So the wrapper is not the differentiator for this folder.

The two prefixes expose different GPUs:

| Prefix | D3D8 adapters | `rend_dxg.ini` after a run |
|---|---|---|
| Lutris prefix | 3 × `NVIDIA GeForce RTX 4060` (`nvd3dum.dll`) | `Vendor=NVIDIA GeForce RTX 4060` |
| Stock `~/.wine` | 3 × `AMD Radeon RX 6600 (RADV NAVI23)` (`aticfx32.dll`) | AMD/RADV |

Lutris selects the NVIDIA card; a plain "Open With Wine" on the EXE does not
set any GPU filter, so stock Wine picks the AMD card first. The remaining
black-car cause is therefore in the AMD/RADV path (D8VK/DXVK + RADV), not in
`d3d8to9`.

**Fix (NVIDIA available): force the Vulkan ICD.** Start the game through
`new_org/Run-Nr2003-NVIDIA.sh`, which sets
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json` (and
`VK_DRIVER_FILES` for newer loaders) before running stock Wine:

```bash
/mnt/3t/Papyrus/new_org/Run-Nr2003-NVIDIA.sh
```

A desktop entry is installed as *NASCAR Racing 2003 (new_org, NVIDIA)*.
Right-clicking the EXE and choosing Wine bypasses the script and will still
select AMD. To verify the filter from a shell:

```bash
cd /mnt/3t/Papyrus/new_org
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json \
  WINEPREFIX="$HOME/.wine" wine ./Nr2003a.eXe
```

With that ICD, a 32-bit D3D8 probe reports 3 × NVIDIA
(`vendor=10de device=2882 driver='nvd3dum.dll'`). `DXVK_FILTER_DEVICE_NAME`
does **not** change D8VK adapter enumeration; use `VK_ICD_FILENAMES` instead.

**If NVIDIA is not available, test on AMD in this order:**

1. `app.ini` → `[Graphics] boardIndex=0` (all AMD adapters enumerate
   identically, but try the primary one anyway).
2. `players/<name>/player.ini` → `enable_CarMegaShader=0` (isolates the
   advanced car shader path).
3. Try a newer DXVK/D8VK pair; a RADV-specific DXVK bug would explain why the
   same DLLs work on NVIDIA.

The older `d3d8to9` black-car bug
([crosire/d3d8to9#109](https://github.com/crosire/d3d8to9/issues/109)) is real
but only applies while `dxwrapper` / `d3d8to9` is active: with "Shadows > On
Cars" enabled, cars go black. `new_org` no longer uses that path; its backups
are `d3d8.dll.d3d8to9.bak`, `d3d9.dll.dxvk-old.bak`,
`dxwrapper.dll.disabled`, and `dxwrapper.ini.disabled`. If you restore
`d3d8to9`, also disable on-car shadows (`on_car_shadow_method=0` in
`player.ini`) or switch to the D8VK pair.

---

## 10. Log reference

`/tmp/tsample.log` (server) and `/tmp/nr2003_qt.log` (client).

| Line | Meaning |
|---|---|
| `apply rva 0x… = … (row N, …)` | client staged an edit |
| `edit rva 0x… : old -> new` | diff found a changed cell |
| `writeBlocks cells to write N (csv pool 3210)` | only your edits are sent |
| `writeBlocks <block> rva … patched N ack 1 status N` | per-block BLOCKM ack |
| `writeBlocks done ok=… fail=… patched=… ptrKept=…` | run summary |
| `writeBlocks VERIFY userEdits=… landed=…` | **read-back proof** |
| `[mem] block m rva …: kept N denormal (uint32-bit) slots` | server protected N hidden non-float slots |
| `[mem] poll: seq N ack N last N count N status N` | SHM command state |
| `block ok` / `block VP ok` / `block fail` | per-block write result |

---

## 11. GUI reference

### Live Mem tab

| Widget | Purpose |
|---|---|
| **Read from memory** | 87 `BLOCKR` round trips; refreshes every Value cell and populates the MemLoc tab. The values read become the baseline, so a later write only sends cells the user actually changed. |
| **Write my edits (BLOCKM)** | Writes **only** the cells you applied. Refuses with a dialog when nothing changed, so it can never clobber the game with the Mem-CSV defaults by accident. |
| **Save live values (Mem CSV)** | Writes `Live - {Chassis,Engine,Wheel} - Mem.csv` in the original transposed layout. |
| **Probe: does this stick?** | Two-press experiment that settles whether the game keeps an edited value or repopulates it. See §11.1. |
| **new value** spin box + **Apply to selected row** | Stages an edit without touching the Qt delegate. |
| selection status strip | `row N  RVA 0x…  [Sing]  <label>  (<current value>)` |

Row counts at startup: Chassis 2508 (682 locked) · Engine 448 (read-only) ·
Wheel 936 · Garage 192 (read-only) · **MemLoc 920** (fully editable).

### 11.1 `Probe: does this stick?` (**MemLoc tab**)

Editing a slot is only useful if the game *keeps* it. If the car-load path
repopulates the table from another source, the edit silently vanishes — and
"the write succeeded" is not evidence that it stuck. The probe answers this
empirically instead of guessing. It uses the row you have selected.

1. Select the row (e.g. Live Mem → Chassis → `cup-5` / `weight-1 (kg)`,
   rva `0x32355C`, or MemLoc → `memloc-00` `[0]`, rva `0x31F908`).
2. Press **Probe: does this stick?**. The original value is remembered, a
   sentinel `9999` is written with one `BLOCKM` (every other slot sent as
   `VAL_KEEP`), and the block is immediately re-read. Log:
   ```
   probe ARMED  rva 0x31f908  memloc-00  [0]  was -0.018 -> wrote 9999  ack 1 status 40  readback 9999  CONFIRMED
   ```
   `status 40` is the block's `floatCount`; `ack 1` means the server took it.
   **`CONFIRMED` here only proves the write landed — not that it persists.**
3. **Change the car / restart the race / reload the setup.**
4. Press it again on the same row:
   ```
   probe RESULT rva 0x31f908  memloc-00  [0]  sentinel 9999  now 9999  -> STUCK (original was -0.018)
   ```

`STUCK` means the game left the value alone across a car load, so the block (or
table) you edited is authoritative. `REVERTED` means the game repopulated it —
edit the layer it is derived from instead. The probe refuses game-owned slots
(§7.1) so it cannot trigger the `flds (%edx)` crash, and `showValueInTable()`
writes whatever it measured back into the row so the GUI never shows a stale
`0` for the probed slot.

**Measured result:** probing `memloc-00` slot 0 (`0x31F908`) across a car change
came back **STUCK**, i.e. the 23 MemLoc 40-float tables are **authoritative
storage**, not a derived copy. The MemLoc tab is therefore a legitimate editing
surface, and the block slots `0x3B..0x51` that point into them must stay locked.
The corresponding per-car block slots were never probed, so whether the 114-float
chassis blocks are themselves authoritative is still open.

### MemLoc tab

The 23 `memloc-00..22` blocks (23 × 40 floats at `0x31F908..0x321628`) were
previously invisible — they showed up as `empty=23` in the write summary. They
are live, and the game *dereferences* them: each chassis block's slots
`0x3B..0x51` hold the pointers, and the probe confirmed the tables themselves
survive a car load (§11.1). No game-owned slots, so everything is editable and it
flows through the normal `BLOCKM` path. Values come from `Read from memory`;
they read as `0` until then, and the probe also fills the single row it touches.
Their field labels are still unknown — the original Mem CSV calls them
`"Mem Loc - Data Table?"` with a question mark, so their author did not know
either; the sampled contents are physics-shaped (alternating ±coefficients, and
`-π` at `0x720348`).

### EXE File tab

Offline editing of the static `Nr2003.eXe`, and the correct home for the
`fair_*.csv` tables — their `&H` values are 1-based static-file addresses, not
live addresses (§6). `&H2EE57D` therefore means byte offset `0x2EE57C`; the
EXE tab seeks directly to `&H - 1` and never walks the PE section table. This
matters because `.data` is `0x895000` virtual but only `0x46000` on disk, so an
RVA-based translation would seek to the wrong bytes.

Each row is encoded according to the CSV `Type` (`Sing` = float32, `Long` =
int32, `Doub` = float64). `Write values to EXE` creates `<exe>.bak` once, then
reads every written range back and reports any mismatch instead of claiming
success. `Read from EXE` applies the same type-aware decode.

This is the in-GUI replacement for the old `write_csv_to_exe.py` route, and the
only way to change the 16 `.rdata` engine blocks, which Wine will not let us
write live (§7.2).
