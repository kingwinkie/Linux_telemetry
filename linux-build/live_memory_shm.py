#!/usr/bin/env python3
r"""
Live Memory SHM client — Linux-native replacement for the old live_memory_io.py
Uses /dev/shm/nr2003_mem (Z:\dev\shm\nr2003_mem inside Wine) instead of ctypes.windll
Fixes: address_offset -1 bug (runtime=0x400000+RVA), winedbg pid fragility, &H parsing
Pairs with telemetry_linux/tsample.cpp pollMemCommands() (WriteProcessMemory via Toolhelp)
and nr2003_mem.h 4096B MEM3 structure.

Usage:
  python3 live_memory_shm.py read chassis --fair-csv ../csv/fair_chassis.csv
  python3 live_memory_shm.py write ../csv/fair_chassis.csv
  python3 live_memory_shm.py set 0x2EE57D 3400.5 --label "Sing 3400.5 Net Weight"
  python3 live_memory_shm.py status

CSV compat: reads fair_*.csv with &H Address, Sing type, Value column (also handles RVA hex)
Set NR2003_CSV_DIR to override the default ../csv data folder.
"""
import struct, time, csv, sys, os
from pathlib import Path

SHM_LINUX = "/dev/shm/nr2003_mem"
SHM_WINE = "Z:\\dev\\shm\\nr2003_mem"
MAGIC = 0x334D454D  # MEM3
TAIL = 0xEBC4A698
SIZE = 131072  # 32 pages, was 4096
MAX_ENTRIES = 2978  # was 64, covers 2924 fair_ rows
ENTRY_FMT = "<I f I 32s"  # rva, value, type, label[32]
ENTRY_SIZE = 44
HDR_FMT = "<8I"  # magic,ver,size,seq,count,ackSeq,status,tail
HDR_SIZE = 32

def _pack_mem(seq, entries):
    hdr = struct.pack(HDR_FMT, MAGIC, 1, SIZE, seq, len(entries), 0, 0, TAIL)
    body = b"".join(struct.pack(ENTRY_FMT, rva, val, typ, lbl.encode()[:32].ljust(32,b'\x00')) for rva,val,typ,lbl in entries)
    body += b"\x00" * (MAX_ENTRIES*ENTRY_SIZE - len(body))
    reserved = b"\x00" * 8  # was 1248 for 4096
    return hdr + body + reserved

def _unpack_mem(data):
    magic,ver,size,seq,count,ack,status,tail = struct.unpack(HDR_FMT, data[:HDR_SIZE])
    entries=[]
    off=HDR_SIZE
    for i in range(count):
        rva,val,typ,lbl = struct.unpack(ENTRY_FMT, data[off:off+ENTRY_SIZE])
        entries.append((rva,val,typ,lbl.rstrip(b'\x00').decode(errors='ignore')))
        off+=ENTRY_SIZE
    return {"magic":magic,"seq":seq,"count":count,"ack":ack,"status":status,"entries":entries}

def _next_seq():
    try:
        with open(SHM_LINUX,"rb") as f:
            d=f.read(SIZE)
            if len(d)==SIZE:
                seq=struct.unpack(HDR_FMT, d[:HDR_SIZE])[3]
                return (seq+1) & 0xFFFFFFFF or 1
    except: pass
    return 1

def write_entries(entries):
    seq=_next_seq()
    data=_pack_mem(seq, entries)
    Path("/dev/shm").mkdir(parents=True, exist_ok=True)
    with open(SHM_LINUX,"wb") as f:
        f.write(data); f.flush()
    print(f"[shm] wrote seq {seq} count {len(entries)} -> {SHM_LINUX} (Wine {SHM_WINE})")
    # wait ack
    for _ in range(50):
        time.sleep(0.02)
        try:
            with open(SHM_LINUX,"rb") as f:
                d=f.read(SIZE)
                info=_unpack_mem(d)
                if info["ack"]==seq:
                    print(f"[shm] ack seq {seq} status {info['status']}/{len(entries)} ok")
                    return info["status"]
        except: pass
    print("[shm] no ack (is tsample.exe running inside Wine?)")
    return None

def parse_fair_csv(path, limit=2978):
    entries=[]
    with open(path) as f:
        r=csv.DictReader(f)
        for row in r:
            if len(entries)>=limit: break
            addr=row.get("Address","").strip()  # &H2EE57D already RVA+1 (<0x400000) vs VA — tester off-by-one
            if addr.startswith("&H"):
                va=int(addr[2:],16)
                raw=va-0x400000 if va>=0x400000 else va  # → RVA+1
                rva=(raw-1) if raw>0 else raw  # compensate &H+1: 0x2EE57D→0x2EE57C (3400.5 0x45548800), 0x2C1ED→0x2C1EC
            elif row.get("RVA","").strip().startswith("0x"):
                rva=int(row["RVA"],16)
            else: continue
            typ=0 if row.get("Type","Sing").lower().startswith("sing") else 0
            val=float(row.get("Value", row.get("CurrentValue", 0)) or 0)
            lbl=row.get("Label","")[:32]
            entries.append((rva,val,typ,lbl))
    return entries

def _csv_dir():
    """Locate telemetry_linux/csv, allowing an explicit override."""
    env = os.environ.get("NR2003_CSV_DIR")
    if env:
        p = Path(env).expanduser()
        if p.is_dir():
            return p
    here = Path(__file__).resolve().parent
    for start in (here, Path.cwd()):
        for d in (start, *start.parents):
            candidate = d / "csv"
            if candidate.is_dir():
                return candidate
    return here.parent / "csv"

CSV_DIR = _csv_dir()
MEM_CSV = {
    "CHASSIS": CSV_DIR/"Original - Chassis - Mem.csv",
    "ENGINE":   CSV_DIR/"Original - Engine - Mem.csv",
    "WHEEL":    CSV_DIR/"Original - Wheel - Mem.csv",
}

def load_mem_csv_values():
    """The authoritative value source.

    tester's *-Mem.csv are transposed (Field rows x car-series columns); the live
    game stores the same numbers as contiguous per-series float blocks.  Mapping
    (live-verified, see nr2003_blocks.h):
        block k   <-> CSV column k           (FILE order, not sorted)
        slot i    <-> CSV row Field(i + desc.fieldOffset)   offset 1 chassis/wheel, 3 engine
    Returns {rva: (value, name, module, slot, label)}.
    """
    values={}
    for mod, path in MEM_CSV.items():
        if not path.exists():
            print(f"[mem] MISSING {path}"); continue
        with open(path) as f:
            rows=list(csv.reader(f))
        def cell(r, i):
            return r[i] if i < len(r) else ""
        ncol=len(rows[0])
        fieldrows=rows[1:]                      # row 0 = header (Field00 + label + series)
        for baseRva,count,m,is_rdata,name,col,foff in _load_blocks():
            if m != mod: continue
            for slot in range(count):
                # fieldrows[0] is the Field01 row, so Field(n) lives at fieldrows[n-1]
                ri=(foff+slot)-1
                if ri < 0 or ri >= len(fieldrows): continue
                raw=cell(fieldrows[ri], 2+col)
                try: v=float(raw)
                except ValueError: continue
                values[baseRva+4*slot]=(v, name, mod, slot, cell(fieldrows[ri], 1))
    return values

def mem_values_for_block(baseRva, count, values):
    """{rva: (value,name,module,slot,label)} narrowed to one block."""
    return {rva: v for rva, v in values.items()
            if baseRva <= rva < baseRva + 4*count}

OP_BLOCKW  = 0x7FFFFFFD
OP_BLOCKR  = 0x7FFFFFFC
OP_BLOCKM  = 0x7FFFFFFB
VAL_KEEP   = 0xFFFFFFFF
# Chassis-shaped blocks (>= 0x72 floats) hold addresses the game dereferences in
# slots 0x3B..0x51 (23 slots -- live-verified against csv/*-Mem.csv).  Writing
# a float there (0.0f -> 0x00000000) is what nulled a pointer and crashed NR2003
# at 0x008be5e0, so the block path never sends them.
PTR_SLOT_LO, PTR_SLOT_HI = 0x3B, 0x51
def is_ptr_slot(slot, count):
    return count >= 0x72 and PTR_SLOT_LO <= slot <= PTR_SLOT_HI
RDATA_LO, RDATA_HI = 0x2E9000, 0x312000

def _pack_op(seq, count, status, entries):
    """pack a command with an explicit opcode (per-value or block)"""
    hdr = struct.pack(HDR_FMT, MAGIC, 1, SIZE, seq, count, 0, status, TAIL)
    body = b"".join(struct.pack(ENTRY_FMT, rva & 0xFFFFFFFF, val, typ, lbl.encode()[:32].ljust(32,b'\x00'))
                    for rva,val,typ,lbl in entries)
    body += b"\x00" * (MAX_ENTRIES*ENTRY_SIZE - len(body))
    return hdr + body + b"\x00" * 8

def _open_rw():
    """open the shm without truncating it - tsample keeps a Win32 handle mapped"""
    try:    return open(SHM_LINUX, "r+b")
    except FileNotFoundError: return open(SHM_LINUX, "w+b")

def _load_blocks():
    """parse nr2003_blocks.h -> [(baseRva, floatCount, module, isRdata, name, csvCol, fieldOffset)]"""
    import re as _re
    out=[]
    for line in (Path(__file__).parent.parent/"nr2003_blocks.h").read_text().splitlines():
        m=_re.match(r'\s*\{(0x[0-9A-Fa-f]+),(\d+),NR2003_BLOCK_MOD_(\w+),([^,]+),\s*'
                    r'"([^"]*)"\s*,\s*(\d+),(\d+)\s*\},',line)
        if m:
            out.append((int(m.group(1),16), int(m.group(2)), m.group(3),
                        ("RDATA" in m.group(4)), m.group(5),
                        int(m.group(6)), int(m.group(7))))
    return out

def write_blocks(entries, skip_rdata=True):
    """tester.exe style: one read-modify-write per per-car block, keep unknown slots"""
    want={rva:val for rva,val,_t,_l in entries}
    nok=nfail=nskip=nempty=npatch=nptr=0
    for baseRva,count,mod,is_rdata,name,_col,_foff in _load_blocks():
        if skip_rdata and is_rdata:
            nskip+=1; continue
        seq=_next_seq()
        ents=[(baseRva,0.0,0,name[:32])]
        patched=0
        nptr=0
        for s in range(count):
            rva=baseRva+4*s
            if is_ptr_slot(s, count):
                ents.append((VAL_KEEP, 0.0, 0, "")); nptr+=1; continue
            if rva in want: ents.append((rva, want[rva], 0, "")); patched+=1
            else:           ents.append((VAL_KEEP, 0.0, 0, ""))
        npatch+=patched
        if patched==0:
            nempty+=1; continue
        with _open_rw() as f:
            f.seek(0); f.write(_pack_op(seq,count,OP_BLOCKM,ents)); f.flush(); os.fsync(f.fileno())
        st=None
        for _ in range(75):
            time.sleep(0.02)
            try:
                with open(SHM_LINUX,"rb") as f: info=_unpack_mem(f.read(SIZE))
                if info["ack"]==seq: st=info["status"]; break
            except: pass
        if st is not None and st>0: nok+=1; print(f"[blk] {mod:8s} {name:<10s} rva=0x{baseRva:X} n={count} patched={patched} -> ok {st}")
        else:
            nfail+=1; print(f"[blk] {mod:8s} {name:<10s} rva=0x{baseRva:X} n={count} patched={patched} -> FAIL {st}")
    print(f"[blk] done ok={nok} fail={nfail} rdata-skipped={nskip} empty={nempty} slots-patched={npatch} pointer-slots-kept={nptr}")
    return nok

def read_blocks():
    nok=nfail=0; first=0.0
    for baseRva,count,mod,is_rdata,name,_col,_foff in _load_blocks():
        seq=_next_seq()
        ents=[(baseRva,0.0,0,name[:32])]+[(VAL_KEEP,0.0,0,"") for _ in range(count)]
        with _open_rw() as f:
            f.seek(0); f.write(_pack_op(seq,count,OP_BLOCKR,ents)); f.flush(); os.fsync(f.fileno())
        st=None; vals=None
        for _ in range(75):
            time.sleep(0.02)
            try:
                with open(SHM_LINUX,"rb") as f: d=f.read(SIZE)
                info=_unpack_mem(d)
                if info["ack"]==seq: st=info["status"]; vals=info["entries"][1:]; break
            except: pass
        if st is not None and st>0:
            nok+=1
            if vals: first+=sum(v for (_r,v,_t,_l) in vals[:8])
        else:
            nfail+=1; print(f"[blk] read {mod} {name} rva=0x{baseRva:X} FAIL {st}")
    print(f"[blk] read done ok={nok} fail={nfail} first-8-sum={first:.2f}")
    return nok

def read_one_block(baseRva, count, name):
    """live BLOCKR read of a single block -> list of floats (or None)."""
    seq=_next_seq()
    ents=[(baseRva,0.0,0,name[:32])]+[(VAL_KEEP,0.0,0,"") for _ in range(count)]
    with _open_rw() as f:
        f.seek(0); f.write(_pack_op(seq,count,OP_BLOCKR,ents)); f.flush(); os.fsync(f.fileno())
    for _ in range(75):
        time.sleep(0.02)
        try:
            with open(SHM_LINUX,"rb") as f: d=f.read(SIZE)
            info=_unpack_mem(d)
            if info["ack"]==seq and info["status"]>0:
                got=info["entries"][1:1+count]
                if len(got)<count: got=got+[float("nan")]*(count-len(got))
                return [v for (_r,v,_t,_l) in got]
        except Exception: pass
    return None

def verify_blocks(verbose=False):
    """Compare live block contents against tester *-Mem.csv values, slot by slot."""
    values=load_mem_csv_values()
    tot=match=0; per={}
    bad={}
    for baseRva,count,mod,is_rdata,name,_col,_foff in _load_blocks():
        live=read_one_block(baseRva,count,name)
        if live is None:
            print(f"[vfy] {mod:8s} {name:<10s} rva=0x{baseRva:06X} READ FAIL"); continue
        m=0; t=0; miss=[]
        for slot in range(count):
            rva=baseRva+4*slot
            if rva not in values: continue
            want=values[rva][0]; got=live[slot]
            t+=1
            if (want!=want and got!=got) or (want==want and got==got and
                    abs(got-want) <= 1e-3*max(1.0, abs(want))):
                m+=1                                          # NaN==NaN, else 1e-3 relative
            else: miss.append((slot, values[rva][4], want, got))
        tot+=t; match+=m
        k=mod; a,b=per.get(k,(0,0)); per[k]=(a+m,b+t)
        bad.setdefault(mod, []).extend(miss)
        if verbose and miss:
            for s,lbl,w,g in miss[:6]:
                print(f"[vfy]   {name} slot {s:3d} {lbl[:28]:<28s} csv={w!r:<14} live={g!r}")
    print(f"[vfy] total {match}/{tot} = {100.0*match/max(tot,1):.1f}%")
    for k,(m,t) in sorted(per.items()):
        print(f"[vfy]   {k:8s} {m}/{t} = {100.0*m/max(t,1):.1f}%")
    for k,miss in sorted(bad.items()):
        slots=sorted({s for (s,_,_,_) in miss})
        if slots: print(f"[vfy]   {k:8s} mismatching slots: {slots[:12]}{' ...' if len(slots)>12 else ''}")
    return match, tot

if __name__=="__main__":
    if len(sys.argv)<2:
        print(__doc__)
        sys.exit(1)
    cmd=sys.argv[1]
    if cmd=="set" and len(sys.argv)>=4:
        rva=int(sys.argv[2],16)
        if rva>=0x400000: rva-=0x400000
        val=float(sys.argv[3])
        lbl=sys.argv[4] if len(sys.argv)>=5 else ""
        write_entries([(rva,val,0,lbl)])
    elif cmd=="write" and len(sys.argv)>=3:
        ents=parse_fair_csv(sys.argv[2], limit=MAX_ENTRIES)
        print(f"parsed {len(ents)} from {sys.argv[2]}")
        write_entries(ents)
    elif cmd=="status":
        try:
            with open(SHM_LINUX,"rb") as f:
                print(_unpack_mem(f.read(SIZE)))
        except Exception as e: print(e)
    elif cmd=="read":
        # via snapshot shm for telemetry, not mem
        print("read via /dev/shm/nr2003_telem (snapshot) — use telemetry_reader.py; mem writes only")
    elif cmd=="blocks":
        write_blocks(parse_fair_csv(sys.argv[2]) if len(sys.argv)>=3 else [])
    elif cmd=="readblocks":
        read_blocks()
    elif cmd=="blocktable":
        src=Path(__file__).parent.parent/"nr2003_blocks.h"
        blocks=_load_blocks()
        for n,(baseRva,count,mod,is_rdata,name,col,foff) in enumerate(blocks):
            print(f"{n:2d} {mod:8s} rva=0x{baseRva:06X} floats={count:>3d} col={col:>2d} off={foff} "
                  f"{name:<10s} {'rdata' if is_rdata else ''}")
        print(f"{len(blocks)} blocks (from {src})")
    elif cmd=="verifyblocks":
        verify_blocks(verbose=("-v" in sys.argv))
    elif cmd=="memcsv":
        vals=load_mem_csv_values()
        print(f"[mem] {len(vals)} slot values from csv/*-Mem.csv")
        bymod={}
        for rva,(v,name,mod,slot,label) in vals.items(): bymod[mod]=bymod.get(mod,0)+1
        for k,v in sorted(bymod.items()): print(f"[mem]   {k:8s} {v}")
        for rva in sorted(vals)[:6]:
            v,name,mod,slot,label=vals[rva]
            print(f"[mem]   0x{rva:06X} slot {slot:3d} {name:<10s} {label[:30]:<30s} = {v}")
    else:
        print("unknown", cmd)
