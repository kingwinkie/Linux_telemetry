#!/usr/bin/env python3
"""
nr2003_lab.py - headless experiment harness for NR2003 physics probing.

Two independent channels, both mapped into /dev/shm by tsample.exe:

  /dev/shm/nr2003_telem  NR23 4096 B  36 Hz PapyTelem snapshot (read-only)
  /dev/shm/nr2003_mem    MEM3 131072 B command channel (read AND write)

Workflow this is built for: you start the game and drive, I change one value
and read the telemetry response.  Nothing here restarts the game or touches
the EXE.

  ./nr2003_lab.py ls weight                      find the rva of a field
  ./nr2003_lab.py get chassis cup-5 3            read it
  ./nr2003_lab.py set chassis cup-5 3 3000       write it + verify
  ./nr2003_lab.py snap                           one decoded telemetry frame
  ./nr2003_lab.py watch 10                       stream for N seconds
  ./nr2003_lab.py log 20 run.csv                 record N seconds to CSV
  ./nr2003_lab.py diff a.csv b.csv               compare two runs

Field addressing is "module block slot", e.g. chassis cup-5 3, or a raw rva
(0x32355c).  Block/slot tables are parsed out of nr2003_blocks.h so names
always match the GUI.
"""

import csv
import importlib.util
import math
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BLOCKS_H = os.path.join(ROOT, "nr2003_blocks.h")

# ---------------------------------------------------------------- mem channel
_shm_spec = importlib.util.spec_from_file_location(
    "_lmshm", os.path.join(HERE, "live_memory_shm.py"))
lmshm = importlib.util.module_from_spec(_shm_spec)
# live_memory_shm.py has a main() guarded by __name__ == "__main__", but it also
# reads sys.argv at import time for its argparse-less CLI.  Point it at a
# harmless argv for the duration of the exec, then put ours back.
_ARGV = sys.argv
sys.argv = ["live_memory_shm.py"]
try:
    _shm_spec.loader.exec_module(lmshm)
finally:
    sys.argv = _ARGV

VAL_KEEP = 0xFFFFFFFF

# ------------------------------------------------------------- snapshot decode
TELEM = "/dev/shm/nr2003_telem"
TELEM_MAGIC = 0x3332524E
TELEM_TAIL = 0xEBC4A698
TELEM_SIZE = 4096
# packed offsets inside Nr2003Snapshot (see nr2003_snapshot.h)
O_MAGIC, O_VER, O_SIZE, O_SEQ, O_TICK, O_STEP, O_TIME = 0, 4, 8, 12, 16, 20, 24
O_CHASSIS, O_WHEEL, O_TIRE, O_DRIVE, O_INPUT, O_GAUGE = 32, 108, 172, 236, 248, 268
O_OPPCNT, O_OPP = 293, 301
OPP_SIZE = 44

WHEEL_NAMES = ("LF", "RF", "LR", "RR")


def f(b, o):
    return struct.unpack_from("<f", b, o)[0]


def i4(b, o):
    return struct.unpack_from("<i", b, o)[0]


def u4(b, o):
    return struct.unpack_from("<I", b, o)[0]


def read_telem():
    """Return the raw 4096 B snapshot, or None if it is missing/torn."""
    try:
        with open(TELEM, "rb") as fh:
            b = fh.read(TELEM_SIZE)
    except OSError:
        return None
    if len(b) != TELEM_SIZE:
        return None
    if u4(b, O_MAGIC) != TELEM_MAGIC or u4(b, 2417) != TELEM_TAIL:
        return None
    if u4(b, O_SEQ) != u4(b, 2413):          # sequence2 torn-read guard
        return None
    return b


def decode(b):
    """Decode one snapshot into a flat dict of readable signals."""
    d = {}
    d["sequence"] = u4(b, O_SEQ)
    d["tick"] = u4(b, O_TICK)
    d["step"] = i4(b, O_STEP)
    d["time"] = struct.unpack_from("<d", b, O_TIME)[0]

    c = O_CHASSIS
    d["trkPct"] = f(b, c + 0)
    d["x"], d["y"], d["z"] = f(b, c + 4), f(b, c + 8), f(b, c + 12)
    d["vx"], d["vy"], d["vz"] = f(b, c + 16), f(b, c + 20), f(b, c + 24)
    d["yaw"], d["pitch"], d["roll"] = f(b, c + 28), f(b, c + 32), f(b, c + 36)
    d["wyaw"], d["wpitch"], d["wroll"] = f(b, c + 40), f(b, c + 44), f(b, c + 48)
    d["nx"], d["ny"], d["nz"] = f(b, c + 52), f(b, c + 56), f(b, c + 60)
    d["hx"], d["hy"] = f(b, c + 64), f(b, c + 68)
    d["steerT"] = f(b, c + 72)
    # car frame is +x nose, +y left, +z up
    d["speed"] = math.sqrt(d["vx"] ** 2 + d["vy"] ** 2 + d["vz"] ** 2)
    d["speed_mph"] = d["speed"] * 2.236936
    d["speed_kph"] = d["speed"] * 3.6
    # long/lat accel by differencing the world velocity across ticks
    g = 9.80665
    dt = 1.0 / 36.0
    d["ax"] = (d["vx"] - getattr(decode, "_pvx", d["vx"])) / dt
    d["ay"] = (d["vy"] - getattr(decode, "_pvy", d["vy"])) / dt
    d["az"] = (d["vz"] - getattr(decode, "_pvz", d["vz"])) / dt
    decode._pvx, decode._pvy, decode._pvz = d["vx"], d["vy"], d["vz"]
    d["longG"] = d["ax"] / g
    d["latG"] = d["ay"] / g
    d["normG"] = d["az"] / g

    w = O_WHEEL
    for k in range(4):
        n = WHEEL_NAMES[k]
        d["w" + n + "_v"] = f(b, w + 16 * k + 0)
        d["w" + n + "_defl"] = f(b, w + 16 * k + 4)
        d["w" + n + "_camber"] = f(b, w + 16 * k + 8)
        d["w" + n + "_Fz"] = f(b, w + 16 * k + 12)

    t = O_TIRE
    for k in range(4):
        n = WHEEL_NAMES[k]
        d["t" + n + "_tinner"] = f(b, t + 16 * k + 0)
        d["t" + n + "_tmiddle"] = f(b, t + 16 * k + 4)
        d["t" + n + "_touter"] = f(b, t + 16 * k + 8)
        d["t" + n + "_kpa"] = f(b, t + 16 * k + 12)

    d["rpm"] = f(b, O_DRIVE + 0)
    d["engPower"] = f(b, O_DRIVE + 4)
    d["engT"] = f(b, O_DRIVE + 8)

    d["throttle"], d["brake"] = f(b, O_INPUT + 0), f(b, O_INPUT + 4)
    d["clutch"], d["steer"] = f(b, O_INPUT + 8), f(b, O_INPUT + 12)
    d["gear"] = i4(b, O_INPUT + 16)

    d["waterTemp"] = f(b, O_GAUGE + 4)
    d["fuelPress"] = f(b, O_GAUGE + 8)
    d["oilTemp"] = f(b, O_GAUGE + 12)
    d["oilPress"] = f(b, O_GAUGE + 16)
    d["voltage"] = f(b, O_GAUGE + 20)
    d["warnBits"] = b[O_GAUGE + 24]

    d["opponents"] = u4(b, O_OPPCNT)
    return d


# ---------------------------------------------------------------- field table
import re


def load_fields():
    """(rva, module, block, slot, label, owned) for every editable slot."""
    txt = open(BLOCKS_H).read()
    row = re.compile(
        r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*,\s*NR2003_BLOCK_MOD_(\w+)\s*,"
        r"\s*([^,]+?)\s*,\s*\"([^\"]*)\"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}")
    names = re.findall(r"NR2003_(\w+)_FIELDS\[\d+\]\s*=\s*\{(.*?)\};",
                       txt, re.S)

    def arr(n):
        for k, body in names:
            if k == n:
                return re.findall(r'"([^"]*)"', body)
        return []

    chassis = arr("CHASSIS")
    engine = arr("ENGINE")
    wheel = arr("WHEEL")

    def label_for(mod, slot, count):
        if mod == "CHASSIS" and slot < len(chassis):
            return chassis[slot]
        if mod == "ENGINE" and slot < len(engine):
            return engine[slot]
        if mod == "WHEEL" and slot < len(wheel):
            return wheel[slot]
        if mod == "MEMLOC":
            return "memloc[%d]" % slot
        return "slot%d" % slot

    out = []
    for base, count, mod, flagstr, name, _col, foff in row.findall(txt):
        base, count = int(base, 16), int(count)
        flags = flagstr.strip()
        for s in range(count):
            owned = False
            if mod == "CHASSIS" and count >= 0x72:
                owned = (0x3B <= s <= 0x51) or s in (0, 1, 19, 20, 21, 22, 39, 110)
            out.append((base + 4 * s, mod.lower(), name, s,
                        label_for(mod, s, count), owned, "RDATA" in flags))
    return out


FIELDS = load_fields()
BY_RVA = {f[0]: f for f in FIELDS}


def resolve(spec):
    """spec = rva | 'module block slot' | 'module block label-fragment'."""
    s = spec.strip()
    if s.lower().startswith("0x"):
        rva = int(s, 16)
        if rva not in BY_RVA:
            raise SystemExit("rva 0x%x is not a known block slot" % rva)
        return BY_RVA[rva]
    parts = s.split()
    if len(parts) == 3 and parts[0].lower() in ("chassis", "engine", "wheel", "memloc"):
        mod, blk, sl = parts[0].lower(), parts[1], parts[2]
        try:
            slot = int(sl, 0)
        except ValueError:
            raise SystemExit("slot must be a number")
        for rva, m, b, s2, lbl, owned, rd in FIELDS:
            if m == mod and b == blk and s2 == slot:
                return (rva, m, b, s2, lbl, owned, rd)
        raise SystemExit("no such field: %s %s %d" % (mod, blk, slot))
    if len(parts) == 2 and parts[0].lower() in ("chassis", "engine", "wheel", "memloc"):
        mod, blk = parts[0].lower(), parts[1]
        hits = [f for f in FIELDS if f[1] == mod and f[2] == blk]
        if not hits:
            raise SystemExit("no such block: %s %s" % (mod, blk))
        return hits
    raise SystemExit("bad spec %r" % spec)


def describe(fld):
    rva, mod, blk, slot, lbl, owned, rd = fld
    tag = []
    if owned:
        tag.append("GAME-OWNED")
    if rd:
        tag.append(".rdata not live-writable")
    return "0x%08x  %-8s %-10s [%3d]  %-34s %s" % (
        rva, mod, blk, slot, lbl, ("  <-- " + ", ".join(tag)) if tag else "")


# ------------------------------------------------------------------ mem rw
def block_base_for(rva, slot):
    return rva - 4 * slot


def read_slot(fld):
    """Live value of one slot (one BLOCKR)."""
    rva, mod, blk, slot, lbl, owned, rd = fld
    vals = lmshm.read_one_block(block_base_for(rva, slot), 0, blk)
    if vals is None:
        raise SystemExit("read failed (is run_nr2003_allinone.sh running?)")
    if not (0 <= slot < len(vals)):
        raise SystemExit("read returned %d slots, wanted %d" % (len(vals), slot + 1))
    return vals[slot]


def set_slots(pairs):
    """pairs = [(rva, value)] -> one BLOCKM per block via live_memory_shm."""
    return lmshm.write_blocks([(r, v, 0, "") for r, v in pairs])


def is_game_owned(fld):
    return fld[5]


# ------------------------------------------------------------------- commands
def cmd_ls(argv):
    pat = argv[0].lower() if argv else ""
    n = 0
    for fld in FIELDS:
        line = describe(fld)
        if pat and pat not in line.lower():
            continue
        print(line)
        n += 1
    print("-- %d of %d slots%s" % (n, len(FIELDS), " (filter %r)" % pat if pat else ""))


def cmd_get(argv):
    fld = resolve(" ".join(argv))
    rva = fld[0]
    base = rva - 4 * fld[3]
    vals = lmshm.read_one_block(base, 0, fld[2])
    if vals is None:
        raise SystemExit("read failed (is run_nr2003_allinone.sh running?)")
    print("rva 0x%08x  %.9g" % (rva, vals[fld[3]]))
    print(describe(fld))


def cmd_set(argv):
    if len(argv) < 2:
        raise SystemExit("usage: set <field> <value>   (field = rva | mod block slot)")
    fld = resolve(argv[0])
    val = float(argv[1])
    if is_game_owned(fld):
        raise SystemExit(
            "refusing %s: game-owned slot (uint32 bits / Mem Loc pointer / NaN "
            "sentinel). Writing 0 here nulls a pointer and crashes NR2003." %
            describe(fld))
    if fld[6]:
        raise SystemExit(
            "refusing %s: it lives in .rdata, which Wine will not let us write "
            "live (err 998). Use the EXE File tab / write_csv_to_exe.py." %
            describe(fld))
    before = read_slot(fld)
    set_slots([(fld[0], val)])
    after = read_slot(fld)
    ok = (abs(after - val) <= 1e-3 * max(1.0, abs(val)))
    print(describe(fld))
    print("  %.9g -> wrote %.9g -> read %.9g  %s"
          % (before, val, after, "OK" if ok else "MISMATCH"))
    if not ok:
        raise SystemExit(1)


def cmd_snap(argv):
    b = read_telem()
    if b is None:
        raise SystemExit("no valid telemetry (%s missing or torn)" % TELEM)
    d = decode(b)
    order = ["sequence", "tick", "time", "trkPct", "speed_mph", "gear", "rpm",
             "throttle", "brake", "clutch", "steer", "longG", "latG", "normG",
             "steerT", "wLF_Fz", "wRF_Fz", "wLR_Fz", "wRR_Fz",
             "tLF_tmiddle", "tRF_tmiddle", "tLR_tmiddle", "tRR_tmiddle",
             "tLF_kpa", "tRF_kpa", "tLR_kpa", "tRR_kpa",
             "x", "y", "z", "yaw", "pitch", "roll", "opponents"]
    for k in order:
        v = d.get(k)
        print("  %-12s %s" % (k, ("%.6g" % v) if isinstance(v, float) else v))


def cmd_watch(argv):
    secs = float(argv[0]) if argv else 5.0
    end = time.time() + secs
    print("%8s %8s %6s %5s %8s %6s %6s %6s" %
          ("time", "mph", "gear", "rpm", "thr", "longG", "latG", "normG"))
    while time.time() < end:
        b = read_telem()
        if b:
            d = decode(b)
            print("%8.2f %8.1f %6d %5.0f %8.2f %6.2f %6.2f %6.2f" %
                  (d["time"], d["speed_mph"], d["gear"], d["rpm"], d["throttle"],
                   d["longG"], d["latG"], d["normG"]))
        time.sleep(1.0 / 12.0)


def cmd_log(argv):
    secs = float(argv[0]) if argv else 10.0
    out = argv[1] if len(argv) > 1 else "run.csv"
    keys = ["time", "speed_mph", "gear", "rpm", "throttle", "brake", "clutch",
            "steer", "longG", "latG", "normG", "trkPct", "x", "y", "z", "yaw",
            "pitch", "roll", "wLF_Fz", "wRF_Fz", "wLR_Fz", "wRR_Fz",
            "tLF_tmiddle", "tRF_tmiddle", "tLR_tmiddle", "tRR_tmiddle",
            "tLF_kpa", "tRF_kpa", "tLR_kpa", "tRR_kpa", "oilTemp", "waterTemp"]
    end = time.time() + secs
    rows = []
    seen = set()
    while time.time() < end:
        b = read_telem()
        if b:
            d = decode(b)
            if d["sequence"] not in seen:
                seen.add(d["sequence"])
                rows.append([d[k] for k in keys])
        time.sleep(0.005)
    with open(out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(keys)
        w.writerows(rows)
    print("wrote %s (%d samples, %.1fs)" % (out, len(rows), secs))


def cmd_diff(argv):
    a, b_ = argv[0], argv[1]
    ra = list(csv.DictReader(open(a)))
    rb = list(csv.DictReader(open(b_)))
    keys = [k for k in (ra[0].keys() if ra else []) if k != "time"]
    print("%d samples in %s, %d in %s" % (len(ra), a, len(rb), b_))
    print("%-14s %12s %12s %12s %10s" % ("metric", "mean A", "mean B", "delta", "pct"))
    for k in keys:
        try:
            va = [float(r[k]) for r in ra]
            vb = [float(r[k]) for r in rb]
        except (ValueError, KeyError):
            continue
        if not va or not vb:
            continue
        ma, mb = sum(va) / len(va), sum(vb) / len(vb)
        pct = (mb - ma) / abs(ma) * 100.0 if ma else float("nan")
        if abs(pct) < 0.5:
            continue
        print("%-14s %12.5g %12.5g %+12.5g %+9.2f%%" % (k, ma, mb, mb - ma, pct))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    rest = sys.argv[2:]
    if cmd == "ls":
        cmd_ls(rest)
    elif cmd == "get":
        cmd_get(rest)
    elif cmd == "set":
        cmd_set(rest)
    elif cmd == "snap":
        cmd_snap(rest)
    elif cmd == "watch":
        cmd_watch(rest)
    elif cmd == "log":
        cmd_log(rest)
    elif cmd == "diff":
        cmd_diff(rest)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
