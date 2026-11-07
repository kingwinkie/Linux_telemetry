#pragma once
// nr2003_mem.h — 128K bidirectional command channel for live physics memory edits
// Linux writer -> /dev/shm/nr2003_mem  <->  Wine reader Z:\dev\shm\nr2003_mem
// tsample.exe (inside Wine) polls this file each tick and does WriteProcessMemory
// into NR2003.exe (base 0x400000 + RVA). Mirrors tester/python fair_*.csv workflow
// but via shm instead of ctypes.windll.
//
// Fair CSVs use &H VA (e.g. &H2EE57D) -> RVA = VA - 0x400000, runtime = 0x400000+RVA
// (fixes live_memory_io.py:54 address_offset=-1 bug).

#include <cstdint>

#pragma pack(push,1)

#define NR2003_MEM_MAGIC      0x334D454Du // "MEM3" LE
#define NR2003_MEM_VERSION    1u
#define NR2003_MEM_TAIL_MAGIC 0xEBC4A698u // same tail as snapshot / Companion
#define NR2003_MEM_PATH_WINE  "Z:\\dev\\shm\\nr2003_mem"
#define NR2003_MEM_PATH_LINUX "/dev/shm/nr2003_mem"
#define NR2003_MEM_SIZE       131072u // 32 pages, fits 2924 fair_* rows (2924*44+32=128688)
#define NR2003_MEM_MAX_ENTRIES 2978u // (131072-32)/44, covers fair_chassis 1521+engine 449+garage 193+wheel 765=2928

// status field opcodes.  status <= 0 with the high bits set is a marker, so
// the normal "status > 0 == number of values written" convention is untouched.
#define NR2003_MEM_OP_WRITE  0            // per-value Read/WriteProcessMemory loop
#define NR2003_MEM_OP_READ   0x7FFFFFFEu  // per-value read request (Linux -> Wine)
#define NR2003_MEM_OP_BLOCKW 0x7FFFFFFDu  // block write: entries[0].rva = base RVA,
                                           // entries[0].type = 0 Sing|1 Dubl,
                                           // count = float count,
                                           // entries[1..count-1].value = payload
#define NR2003_MEM_OP_BLOCKR 0x7FFFFFFCu  // block read, same layout, results written
                                           // back into entries[1..count-1].value
#define NR2003_MEM_OP_BLOCKM 0x7FFFFFFBu  // block read-modify-write: tsample first reads
                                           // the run, then overwrites only the slots whose
                                           // entries[1..count-1].rva != NR2003_MEM_VAL_KEEP.
                                           // Lets us push a sparse subset of a 114-float
                                           // block in ONE round trip without writing 0.0f
                                           // over the slots the CSV does not describe.
#define NR2003_MEM_VAL_KEEP  0xFFFFFFFFu  // per-slot marker for OP_BLOCKM

struct Nr2003MemEntry {
    uint32_t rva;        // RVA (VA - 0x400000), e.g. 0x2EE57D for &H2EE57D
    float    value;      // Sing float value (Dubl truncated if needed)
    uint32_t type;       // 0=Sing float, 1=Dubl double (low 4B), extend
    char     label[32];  // optional, null-terminated, e.g. "Sing 3400.5 Net Weight"
};

struct Nr2003MemCmd {
    uint32_t magic;      // NR2003_MEM_MAGIC
    uint32_t version;    // NR2003_MEM_VERSION
    uint32_t size;       // sizeof(Nr2003MemCmd) == 131072
    uint32_t seq;        // Linux writer bumps on each batch (0 = idle)
    uint32_t count;      // 0..2978 entries valid
    uint32_t ackSeq;     // Wine writer copies seq here on completion
    int32_t  status;     // 0=pending/write, >0=written count, <0=error (-1 OpenProcess fail),
                         // or one of the NR2003_MEM_OP_* markers above
    uint32_t tailMagic;  // NR2003_MEM_TAIL_MAGIC
    Nr2003MemEntry entries[NR2003_MEM_MAX_ENTRIES]; // 2978*44=131032
    uint8_t  reserved[8]; // pad to 131072 (32+131032+8=131072)
};

#pragma pack(pop)

static_assert(sizeof(Nr2003MemEntry)==44, "MemEntry 44");
static_assert(sizeof(Nr2003MemCmd)==NR2003_MEM_SIZE, "MemCmd must be 131072");
