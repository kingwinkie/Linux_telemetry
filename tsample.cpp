/*
 * Copyright (c) 2002, Papyrus Racing Games, Inc.
 * All rights reserved.
 *
 * Date			Who		What
 * 06-apr-02	RJC 	Created
 *
 * Description:
 *	A small test harness for the Papyrus telemetry system.
 *	It is the intent that you can place this file, along
 *	with papytelemapp.obj and papytelemapp.h in the same
 *	directory, and get a valid executable when you issue
 *		cl /W3 /MT tsample.cpp papytelemapp.obj
 *	(/W3 turns on DevStudio's second-highest warning level,
 *	and /MT says to link with the multi-threaded version
 *	of the C run-time library.  /W4 would work but for
 *	warning 4127 - conditional expression is constant).
 *	If you then run tsample.exe (first) and the sim
 *	(second), this program will read telemetry from the
 *	sim, and write some of it out to its console window.
 *
 *	What this program will output depends on the
 *	"formatForSpreadsheet" variable below.  Change it as
 *	desired.
 *
 */


/*
 * Include files
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "papytelemapp.h"
#include "nr2003_snapshot.h"
#include "nr2003_mem.h"
#include "nr2003_blocks.h"
#include <tlhelp32.h>


/*
 * Miscellaneous definitions local to this file
 */

// So that we don't have to qualify our use of the telem API
// components with "PapyTelem::".  
using namespace PapyTelem;



/*
 * Local/static functions defined in this file (prototype only; implement below)
 */


/*
 * Local/static variables defined in this file
 */

// Set this to true if you want the program to output a tab-delimited
// text file appropriate for importing into a spreadsheet.  You would
// most likely want to redirect this programs output to a text file
// (e.g.  C:\test> tsample > telemetry.txt).
// Set this to false if you want the program to output simple text
// messages that show what kind of telemetry the sim is generating.
// linux-build/Makefile passes -DFORMAT_CSV for tsample_csv.exe
#ifdef FORMAT_CSV
const bool formatForSpreadsheet = true;
#else
const bool formatForSpreadsheet = false;
#endif

// Field separator for spreadsheet format.  '\t' and ',' make sense.
const char	S = '\t';

// If you set this to be true, then the telemetry output will be in
// english units.  Otherwise they are output in SI units.
const bool englishUnits = true;

// When false, sampling is done at the game tick rate (36Hz, currently).
// When true, sampling is done at the physics rate (288Hz, currently).
const bool sampleAtPhysicsRate = false;


/*
 * Snapshot sharing via Wine Z: drive -> Linux /dev/shm (see nr2003_snapshot.h)
 * Use unbuffered Win32 HANDLE (CreateFileA/WriteFile/FlushFileBuffers) so
 * Linux pwrite via /dev/shm and Wine ReadFile see the same bytes without
 * CRT FILE* buffering (FILE* fseek+fflush can miss pwrite updates on Wine's
 * wineserver cache).
 */
static HANDLE g_snapFile = INVALID_HANDLE_VALUE;
static bool  g_snapEnabled = true;

/*
 * Live memory command channel via Wine Z: drive (see nr2003_mem.h)
 * Linux writes /dev/shm/nr2003_mem, Wine (this process) polls and does
 * WriteProcessMemory into NR2003.exe (base 0x400000+RVA). Fixes
 * tester/python/live_memory_io.py:54 address_offset=-1 and winedbg pid
 * fragility by using Toolhelp like blowover/windtunnel_exe/FUN_00403080.
 */
static HANDLE g_memFile = INVALID_HANDLE_VALUE;
static uint32_t g_memLastSeq = 0;

static bool openSnapshotFile()
{
    if (!g_snapEnabled) return false;
    g_snapFile = CreateFileA(NR2003_SNAPSHOT_PATH_WINE, GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_snapFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[snapshot] CreateFileA " NR2003_SNAPSHOT_PATH_WINE " failed %lu\n", (unsigned long)GetLastError());
        return false;
    }
    Nr2003Snapshot zero{};
    zero.magic = NR2003_SNAPSHOT_MAGIC;
    zero.version = NR2003_SNAPSHOT_VERSION;
    zero.size = NR2003_SNAPSHOT_SIZE;
    zero.tailMagic = NR2003_SNAPSHOT_TAIL_MAGIC;
    DWORD w=0; WriteFile(g_snapFile, &zero, sizeof(zero), &w, NULL); FlushFileBuffers(g_snapFile);
    fprintf(stderr, "[snapshot] opened " NR2003_SNAPSHOT_PATH_WINE " (%u bytes) HANDLE\n", NR2003_SNAPSHOT_SIZE);
    return true;
}
static void closeSnapshotFile()
{
    if (g_snapFile != INVALID_HANDLE_VALUE) { CloseHandle(g_snapFile); g_snapFile = INVALID_HANDLE_VALUE; }
}
static void writeSnapshot()
{
    if (g_snapFile == INVALID_HANDLE_VALUE) return;
    Nr2003Snapshot snap{};
    snap.magic = NR2003_SNAPSHOT_MAGIC;
    snap.version = NR2003_SNAPSHOT_VERSION;
    snap.size = NR2003_SNAPSHOT_SIZE;
    snap.tailMagic = NR2003_SNAPSHOT_TAIL_MAGIC;
    const SampleHeader* sh = (const SampleHeader*)AppGetSimData(kSampleHeader);
    const ChassisData* cd = (const ChassisData*)AppGetSimData(kChassisData);
    const WheelData* wd = (const WheelData*)AppGetSimData(kWheelData);
    const TireData* td = (const TireData*)AppGetSimData(kTireData);
    const DrivelineData* dl = (const DrivelineData*)AppGetSimData(kDrivelineData);
    const DriverInput* di = (const DriverInput*)AppGetSimData(kDriverInput);
    const GaugeData* gd = (const GaugeData*)AppGetSimData(kGaugeData);
    const OpponentCarData* ocd = (const OpponentCarData*)AppGetSimData(kOpponentCarData);
    if (sh) { snap.sequence = sh->sequence; snap.tick = sh->tick; snap.step = sh->step; snap.time = sh->time; }
    if (cd) snap.chassis = *cd; else memset(&snap.chassis, 0, sizeof(snap.chassis));
    if (wd) snap.wheel = *wd; else memset(&snap.wheel, 0, sizeof(snap.wheel));
    if (td) snap.tire = *td; else memset(&snap.tire, 0, sizeof(snap.tire));
    if (dl) snap.driveline = *dl; else memset(&snap.driveline, 0, sizeof(snap.driveline));
    if (di) snap.input = *di; else memset(&snap.input, 0, sizeof(snap.input));
    if (gd) snap.gauge = *gd; else memset(&snap.gauge, 0, sizeof(snap.gauge));
    if (ocd) {
        int n = AppGetSimDataSize(kOpponentCarData) / (int)sizeof(OpponentCarData);
        if (n < 0) n = 0; if (n > 48) n = 48;
        snap.opponentCount = (uint32_t)n;
        memcpy(snap.opponents, ocd, n * sizeof(OpponentCarData));
        if (n < 48) memset(snap.opponents + n, 0, (48 - n) * sizeof(OpponentCarData));
    } else {
        snap.opponentCount = 0;
        memset(snap.opponents, 0, sizeof(snap.opponents));
    }
    snap.sequence2 = snap.sequence;
    SetFilePointer(g_snapFile, 0, NULL, FILE_BEGIN);
    DWORD w=0; WriteFile(g_snapFile, &snap, sizeof(snap), &w, NULL); FlushFileBuffers(g_snapFile);
}

static bool openMemFile()
{
    g_memFile = CreateFileA(NR2003_MEM_PATH_WINE, GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_memFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[mem] CreateFileA " NR2003_MEM_PATH_WINE " failed %lu\n", (unsigned long)GetLastError());
        return false;
    }
    // check if we created or opened; if file was 0 bytes or bad magic, init
    DWORD sz = GetFileSize(g_memFile, NULL);
    if (sz < sizeof(Nr2003MemCmd)) {
        Nr2003MemCmd zero{};
        zero.magic = NR2003_MEM_MAGIC;
        zero.version = NR2003_MEM_VERSION;
        zero.size = NR2003_MEM_SIZE;
        zero.tailMagic = NR2003_MEM_TAIL_MAGIC;
        zero.seq = 0; zero.ackSeq = 0; zero.status = 0;
        SetFilePointer(g_memFile, 0, NULL, FILE_BEGIN);
        DWORD w=0; WriteFile(g_memFile, &zero, sizeof(zero), &w, NULL); FlushFileBuffers(g_memFile);
        fprintf(stderr, "[mem] created " NR2003_MEM_PATH_WINE " (%u bytes) HANDLE\n", NR2003_MEM_SIZE);
    } else {
        Nr2003MemCmd hdr{}; DWORD r=0;
        SetFilePointer(g_memFile, 0, NULL, FILE_BEGIN);
        if (ReadFile(g_memFile, &hdr, sizeof(hdr), &r, NULL) && r==sizeof(hdr) &&
            hdr.magic == NR2003_MEM_MAGIC && hdr.tailMagic == NR2003_MEM_TAIL_MAGIC) {
            g_memLastSeq = hdr.ackSeq;
            fprintf(stderr, "[mem] opened " NR2003_MEM_PATH_WINE " seq=%u ack=%u HANDLE\n", hdr.seq, hdr.ackSeq);
        } else {
            Nr2003MemCmd zero{};
            zero.magic = NR2003_MEM_MAGIC;
            zero.version = NR2003_MEM_VERSION;
            zero.size = NR2003_MEM_SIZE;
            zero.tailMagic = NR2003_MEM_TAIL_MAGIC;
            SetFilePointer(g_memFile, 0, NULL, FILE_BEGIN);
            DWORD w=0; WriteFile(g_memFile, &zero, sizeof(zero), &w, NULL); FlushFileBuffers(g_memFile);
        }
    }
    return true;
}
static void nr2003Detach();
static void closeMemFile()
{
    nr2003Detach();
    if (g_memFile != INVALID_HANDLE_VALUE) { CloseHandle(g_memFile); g_memFile = INVALID_HANDLE_VALUE; }
}
static DWORD findNr2003PidToolhelp()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"NR2003.exe")==0 || _wcsicmp(pe.szExeFile, L"nr2003.exe")==0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// tester.exe attaches by window, not by process enumeration: it calls
// FindWindowA("NR2003") + GetWindowThreadProcessId and opens that pid with
// PROCESS_ALL_ACCESS (tester/out/decomp_42AA20L_FUN_0042aa20.c, the READ4 /
// WRITE4 helpers).  That avoids the Wine Win32-vs-Linux PID namespace mismatch
// we hit with Toolhelp, so prefer it and keep Toolhelp only as a fallback.
static DWORD findNr2003Pid()
{
    HWND hwnd = FindWindowA("NR2003", NULL);
    if (hwnd) {
        DWORD pid = 0;
        if (GetWindowThreadProcessId(hwnd, &pid) && pid) return pid;
    }
    return findNr2003PidToolhelp();
}

// Cached process handle.  tester.exe re-opens per 4-byte access; we cache so a
// 2924-value batch (or a 114-float block) does not pay FindWindow+OpenProcess.
static HANDLE   g_hProc       = NULL;
static DWORD    g_hProcPid    = 0;
static uint64_t g_hProcTried  = 0;

static HANDLE nr2003Process()
{
    if (g_hProc) {
        if (GetExitCodeProcess(g_hProc, NULL) == 0) {
            // still alive
        } else {
            CloseHandle(g_hProc); g_hProc = NULL; g_hProcPid = 0;
        }
    }
    if (g_hProc) return g_hProc;
    DWORD pid = findNr2003Pid();
    if (!pid) return NULL;
    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) {
        // PROCESS_ALL_ACCESS can be denied; fall back to the minimum we need
        h = OpenProcess(PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) {
            g_hProcTried++;
            if (g_hProcTried % 20 == 1)
                fprintf(stderr, "[mem] OpenProcess(%lu) failed %lu\n", (unsigned long)pid, (unsigned long)GetLastError());
            return NULL;
        }
    }
    g_hProc = h; g_hProcPid = pid;
    g_hProcTried = 0;
    fprintf(stderr, "[mem] attached to NR2003 pid %lu via %s\n", (unsigned long)pid,
            g_hProcPid ? "FindWindowA(\"NR2003\")" : "toolhelp");
    return g_hProc;
}

static void nr2003Detach()
{
    if (g_hProc) { CloseHandle(g_hProc); g_hProc = NULL; }
    g_hProcPid = 0;
}

static void writeMemBack(Nr2003MemCmd& cmd, int status)
{
    cmd.status = status;
    cmd.ackSeq = cmd.seq;
    SetFilePointer(g_memFile, 0, NULL, FILE_BEGIN);
    DWORD w=0; WriteFile(g_memFile, &cmd, sizeof(cmd), &w, NULL); FlushFileBuffers(g_memFile);
    g_memLastSeq = cmd.seq;
}

// One WriteProcessMemory for a whole contiguous float run, with the
// VirtualProtectEx retry that is only safe before the sim goes active (a
// mid-physics VP + a 0.0f writing 0x00000000 over a pointer slot crashed
// NR2003 at 0x008be5e0 xorb %cl,(%edx) with EDX=0).
static int writeBlock(HANDLE hProc, uint32_t baseRva, const float* vals, int n, const char* tag)
{
    if (n <= 0) return 0;
    LPVOID addr = (LPVOID)(uintptr_t)(0x400000u + baseRva);
    SIZE_T bytes = (SIZE_T)n * sizeof(float);
    SIZE_T written = 0;
    if (WriteProcessMemory(hProc, addr, vals, bytes, &written) && written == bytes) {
        if (n > 1) fprintf(stderr, "[mem] block ok %s rva %08x va %p n %d (%lu B)\n", tag, baseRva, addr, n, (unsigned long)bytes);
        return 1;
    }
    DWORD err = GetLastError();
    bool isRData = nr2003_block_is_rdata(baseRva);
    bool preRace = !AppCheckIfSimActiveQ();
    if (!preRace && isRData) {
        fprintf(stderr, "[mem] block fail %s rva %08x n %d err %lu (.rdata pre-race only)\n", tag, baseRva, n, (unsigned long)err);
        return 0;
    }
    if (err == 998 || err == 5) {
        DWORD oldProt = 0;
        if (VirtualProtectEx(hProc, addr, bytes, PAGE_EXECUTE_READWRITE, &oldProt)) {
            written = 0;
            if (WriteProcessMemory(hProc, addr, vals, bytes, &written) && written == bytes) {
                fprintf(stderr, "[mem] block VP ok %s rva %08x n %d preRace=%d (was err %lu)\n", tag, baseRva, n, preRace, (unsigned long)err);
                return 1;
            }
            fprintf(stderr, "[mem] block VP retry fail %s rva %08x n %d err %lu/%lu\n", tag, baseRva, n, (unsigned long)err, (unsigned long)GetLastError());
        } else {
            fprintf(stderr, "[mem] block VP err %s rva %08x n %d err %lu/%lu\n", tag, baseRva, n, (unsigned long)err, (unsigned long)GetLastError());
        }
        return 0;
    }
    fprintf(stderr, "[mem] block fail %s rva %08x n %d err %lu (skipped)\n", tag, baseRva, n, (unsigned long)err);
    return 0;
}

static int readBlock(HANDLE hProc, uint32_t baseRva, float* vals, int n)
{
    if (n <= 0) return 0;
    LPVOID addr = (LPVOID)(uintptr_t)(0x400000u + baseRva);
    SIZE_T bytes = (SIZE_T)n * sizeof(float);
    SIZE_T got = 0;
    if (ReadProcessMemory(hProc, addr, vals, bytes, &got) && got == bytes) return 1;
    DWORD err = GetLastError();
    fprintf(stderr, "[mem] block read fail rva %08x va %p n %d err %lu\n", baseRva, addr, n, (unsigned long)err);
    return 0;
}

static void pollMemCommands()
{
    if (g_memFile == INVALID_HANDLE_VALUE) {
        // fprintf(stderr,"[mem] poll: no file\n");
        return;
    }
    Nr2003MemCmd cmd{}; DWORD r=0;
    SetFilePointer(g_memFile, 0, NULL, FILE_BEGIN);
    if (!ReadFile(g_memFile, &cmd, sizeof(cmd), &r, NULL) || r!=sizeof(cmd)) {
        fprintf(stderr, "[mem] poll: ReadFile failed %lu r=%lu\n", (unsigned long)GetLastError(), (unsigned long)r);
        return;
    }
    if (cmd.magic != NR2003_MEM_MAGIC || cmd.tailMagic != NR2003_MEM_TAIL_MAGIC) {
        fprintf(stderr, "[mem] poll: bad magic %08x tail %08x size %u seq %u ack %u\n", cmd.magic, cmd.tailMagic, cmd.size, cmd.seq, cmd.ackSeq);
        return;
    }
    fprintf(stderr, "[mem] poll: seq %u ack %u last %u count %u status %d size %u tail %08x\n", cmd.seq, cmd.ackSeq, g_memLastSeq, cmd.count, cmd.status, cmd.size, cmd.tailMagic);
    if (cmd.seq == 0 || cmd.seq == cmd.ackSeq || cmd.seq == g_memLastSeq) {
        if (cmd.seq == g_memLastSeq) fprintf(stderr, "[mem] poll: already acked seq %u\n", cmd.seq);
        return;
    }
    if (cmd.count == 0 || cmd.count > NR2003_MEM_MAX_ENTRIES) {
        fprintf(stderr, "[mem] poll: bad count %u max %u\n", cmd.count, NR2003_MEM_MAX_ENTRIES);
        return;
    }
    HANDLE hProc = nr2003Process();
    if (!hProc) {
        writeMemBack(cmd, -1);
        fprintf(stderr, "[mem] no NR2003 handle for seq %u\n", cmd.seq);
        return;
    }
    DWORD pid = g_hProcPid;

    int32_t op = cmd.status;
    if (op == NR2003_MEM_OP_BLOCKW || op == NR2003_MEM_OP_BLOCKR || op == NR2003_MEM_OP_BLOCKM) {
        // entries[0].rva = base, entries[0].type = 0 Sing|1 Dubl, count = float count
        uint32_t baseRva = cmd.entries[0].rva;
        int n = (int)cmd.count;
        if (baseRva > 0x00FFFFFF) {
            fprintf(stderr, "[mem] block skip rva %08x > 0xFFFFFF\n", baseRva);
            writeMemBack(cmd, 0);
            return;
        }
        static float buf[NR2003_MEM_MAX_ENTRIES];
        if (n > (int)NR2003_MEM_MAX_ENTRIES) n = (int)NR2003_MEM_MAX_ENTRIES;
        // Chassis-shaped blocks (>= 0x72 floats) keep addresses in slots
        // 0x3C..0x52.  Those are dereferenced by the game, so the block path must
        // never overwrite them with a float (0.0f -> 0x00000000 -> the null-deref
        // crash at 0x008be5e0).  Callers that mean to change them have to write
        // the pointed-to table instead.
        bool hasPtrSlots = (n >= 0x72);
        uint32_t ptrLo = baseRva + NR2003_BLOCK_PTR_SLOT_LO * 4u;
        uint32_t ptrHi = baseRva + (NR2003_BLOCK_PTR_SLOT_HI + 1u) * 4u;
        int ok = 0;
        if (op == NR2003_MEM_OP_BLOCKM) {
            // read-modify-write: keep every slot the sender marked KEEP so a sparse
            // CSV subset never lands as 0.0f over a pointer slot.
            if (readBlock(hProc, baseRva, buf, n)) {
                int patched = 0, ptrSkipped = 0, intSkipped = 0, denormKept = 0;
                static const int kIntSlots[] = {0,1,19,20,21,22,39,110};
                for (int i = 0; i < n; i++) {
                    if (cmd.entries[1 + i].rva == NR2003_MEM_VAL_KEEP) continue;
                    if (hasPtrSlots) {
                        uint32_t srva = baseRva + (uint32_t)i * 4u;
                        if (srva >= ptrLo && srva < ptrHi) {
                            ptrSkipped++;
                            continue;
                        }
                        // uint32-bit slots: 0-based int indices (memory = CSV - 1) and
                        // the NaN "not set" sentinel.  A CSV 0 here would become 0.0f ->
                        // 0x00000000, which the game dereferences as a null pointer.
                        bool isIntSlot = false;
                        for (int k = 0; k < 8; k++) if (kIntSlots[k] == i) isIntSlot = true;
                        if (isIntSlot) { intSkipped++; continue; }
                    }
                    // Universal safety net, index-independent.  A denormal float in a
                    // live table means the slot holds uint32 bits (an index or a
                    // pointer), not a physics value: no legitimate NR2003 parameter is
                    // denormal.  Overwriting one with 0.0f turns it into 0x00000000 and
                    // the game dereferences it as a null pointer (crash 0x00432e47,
                    // flds (%edx) with EDX=0, ECX/ESI = chassis block base 0x00723550).
                    // Keep whatever the game put there.
                    if (buf[i] > -1e-30f && buf[i] < 1e-30f) { denormKept++; continue; }
                    buf[i] = cmd.entries[1 + i].value;
                    patched++;
                }
                ok = writeBlock(hProc, baseRva, buf, n, "m");
                if (ptrSkipped)
                    fprintf(stderr, "[mem] block m rva %08x: kept %d pointer slots (0x3b..0x51)\n",
                            baseRva, ptrSkipped);
                if (intSkipped)
                    fprintf(stderr, "[mem] block m rva %08x: kept %d uint32-bit slots\n",
                            baseRva, intSkipped);
                if (denormKept)
                    fprintf(stderr, "[mem] block m rva %08x: kept %d denormal (uint32-bit) slots\n",
                            baseRva, denormKept);
                fprintf(stderr, "[mem] block m rva %08x n %d patched %d -> %d\n", baseRva, n, patched, ok);
            }
        } else if (op == NR2003_MEM_OP_BLOCKW) {
            int ptrSkipped = 0;
            for (int i = 0; i < n; i++) {
                if (hasPtrSlots) {
                    uint32_t srva = baseRva + (uint32_t)i * 4u;
                    if (srva >= ptrLo && srva < ptrHi) {
                        buf[i] = 0.0f; // placeholder, overwritten from memory below
                        ptrSkipped++;
                        continue;
                    }
                }
                buf[i] = cmd.entries[1 + i].value;
            }
            if (ptrSkipped) {
                // preserve the live pointer values
                if (!readBlock(hProc, baseRva, buf, n)) {
                    writeMemBack(cmd, -op);
                    return;
                }
            }
            ok = writeBlock(hProc, baseRva, buf, n, "w");
            if (ptrSkipped)
                fprintf(stderr, "[mem] block w rva %08x: kept %d pointer slots\n", baseRva, ptrSkipped);
        } else {
            for (int i = 0; i < n; i++) buf[i] = 0;
            ok = readBlock(hProc, baseRva, buf, n);
            for (int i = 0; i < n; i++) cmd.entries[1 + i].value = buf[i];
        }
        // ack with count = n+1 so the serialized window (entries[0..n]) carries the
        // base descriptor plus all n slot values; entries[n] would otherwise be
        // dropped because only cmd.count entries are written back.
        cmd.count = (uint32_t)(n + 1);
        writeMemBack(cmd, ok ? n : -op);
        fprintf(stderr, "[mem] seq %u: block %s %d floats rva %08x -> %d\n", cmd.seq,
                op == NR2003_MEM_OP_BLOCKW ? "write" : (op == NR2003_MEM_OP_BLOCKM ? "merge" : "read"), n, baseRva, ok);
        return;
    }

    bool isRead = (op == NR2003_MEM_OP_READ); // Qt read request marker
    int ok = 0;
    for (uint32_t i=0;i<cmd.count;i++) {
        uint32_t rva = cmd.entries[i].rva;
        if (rva > 0x00FFFFFF) { fprintf(stderr,"[mem] skip rva %08x > 0xFFFFFF\n", rva); continue; }
        LPVOID addr = (LPVOID)(uintptr_t)(0x400000u + rva); // fixes live_memory_io.py -1
        if (isRead) {
            float val=0; SIZE_T read=0;
            if (ReadProcessMemory(hProc, addr, &val, sizeof(val), &read) && read==sizeof(val)) { cmd.entries[i].value = val; ok++; }
            else if (cmd.entries[i].type==1) { // Dubl fallback
                double d=0; SIZE_T rd=0;
                if (ReadProcessMemory(hProc, addr, &d, sizeof(d), &rd) && rd==sizeof(d)) { cmd.entries[i].value = (float)d; ok++; }
                else fprintf(stderr,"[mem] Read fail rva %08x va %p err %lu\n", rva, addr, (unsigned long)GetLastError());
            } else fprintf(stderr,"[mem] Read fail rva %08x va %p err %lu\n", rva, addr, (unsigned long)GetLastError());
        } else {
            float val = cmd.entries[i].value;
            SIZE_T written=0;
            if (WriteProcessMemory(hProc, addr, &val, sizeof(val), &written) && written==sizeof(val)) ok++;
            else {
                DWORD err = GetLastError();
                bool isRData = nr2003_block_is_rdata(rva);
                bool preRace = !AppCheckIfSimActiveQ(); // menu/practice not racing — safe to VP
                if (!preRace && isRData) {
                    fprintf(stderr,"[mem] Write fail rva %08x va %p val %f err %lu (skipped, .rdata pre-race only)\n", rva, addr, val, (unsigned long)err);
                } else if (err==998 || err==5) {
                    // gated live: only VP when pre-race (avoids 0x008be5e0 EDX=0 crash mid-physics when null 0.0 float hits pointer slot)
                    DWORD oldProt=0;
                    if (VirtualProtectEx(hProc, addr, sizeof(val), PAGE_EXECUTE_READWRITE, &oldProt)) {
                        written=0;
                        if (WriteProcessMemory(hProc, addr, &val, sizeof(val), &written) && written==sizeof(val)) {
                            ok++;
                            fprintf(stderr,"[mem] VP ok rva %08x va %p preRace=%d (was err %lu)\n", rva, addr, preRace, (unsigned long)err);
                        } else {
                            DWORD err2=GetLastError();
                            fprintf(stderr,"[mem] Write fail rva %08x va %p val %f err %lu (VP retry err %lu, skipped)\n", rva, addr, val, (unsigned long)err, (unsigned long)err2);
                        }
                    } else {
                        fprintf(stderr,"[mem] Write fail rva %08x va %p val %f err %lu (VP err %lu, skipped)\n", rva, addr, val, (unsigned long)err, (unsigned long)GetLastError());
                    }
                } else {
                    fprintf(stderr,"[mem] Write fail rva %08x va %p val %f err %lu (skipped)\n", rva, addr, val, (unsigned long)err);
                }
            }
        }
    }
    writeMemBack(cmd, ok);
    fprintf(stderr, "[mem] seq %u: %d/%u %s to pid %lu\n", cmd.seq, ok, (unsigned)cmd.count, isRead?"reads":"writes", (unsigned long)pid);
}

/*
 * Global/public variables defined
 */




/*
 ***************************************************************************
 * Some utility functions
 ***************************************************************************
 */
static void matrix33FromEuler(
	float		m[3][3],
	float		yaw,
	float		pitch,
	float		roll)
{
	float cy = (float) cos(yaw);
	float cp = (float) cos(pitch);
	float cr = (float) cos(roll);
	float sy = (float) sin(yaw);
	float sp = (float) sin(pitch);
	float sr = (float) sin(roll);
	
	m[0][0] = cp*cy;
	m[0][1] = cy*sp*sr - cr*sy;
	m[0][2] = cr*cy*sp + sr*sy;
	m[1][0] = cp*sy;
	m[1][1] = cr*cy + sp*sr*sy;
	m[1][2] = cr*sp*sy - cy*sr;
	m[2][0] = -sp;
	m[2][1] = cp*sr;
	m[2][2] = cp*cr;
}

// Multiply vector by matrix transpose
static void matrix33MulTVector3(
	float		r[3],
	const float	m[3][3],
	const float	v[3])
{
	float		tmp[3];
	float		*o;

	o = (r == v) ? tmp : r;

	o[0] = (m[0][0]*v[0]) + (m[1][0]*v[1]) + (m[2][0]*v[2]);
	o[1] = (m[0][1]*v[0]) + (m[1][1]*v[1]) + (m[2][1]*v[2]);
	o[2] = (m[0][2]*v[0]) + (m[1][2]*v[1]) + (m[2][2]*v[2]);

	if (o != r) memcpy(r, o, sizeof(tmp));
}

static void vector3Add(
	float		r[3],
	const float	v0[3],
	const float	v1[3])
{
	r[0] = v0[0] + v1[0];
	r[1] = v0[1] + v1[1];
	r[2] = v0[2] + v1[2];
}

static void vector3Sub(
	float		r[3],
	const float	v0[3],
	const float	v1[3])
{
	r[0] = v0[0] - v1[0];
	r[1] = v0[1] - v1[1];
	r[2] = v0[2] - v1[2];
}

static void vector3Scale(
	float		r[3],
	float		s,
	const float	v[3])
{
	r[0] = v[0] * s;
	r[1] = v[1] * s;
	r[2] = v[2] * s;
}

/*
 ***************************************************************************
 * Routines to generate the program's output.
 ***************************************************************************
 */
static const char *sessionType(int t)
{
	switch(t)
	{
		case kSessionTypeInvalid :		return "invalid";
		case kSessionTypeTesting :		return "testing";
		case kSessionTypePractice :		return "practice";
		case kSessionTypeQualifyLone :	return "lone qualifying";
		case kSessionTypeQualifyOpen :	return "open qualifying";
		case kSessionTypeRace :			return "race";
	}
	return "?";
}

static const char *sessionState(int s)
{
	switch(s)
	{
		case kSessionStateInvalid :		return "invalid";
		case kSessionStateGetInCar :	return "get in car";
		case kSessionStateWarmup :		return "warmup";
		case kSessionStateParadeLaps :	return "parade laps";
		case kSessionStateRacing :		return "racing";
		case kSessionStateCheckered :	return "checkered";
		case kSessionStateCoolDown :	return "cool down";
	}
	return "?";
}

static const char *sessionFlag(int f)
{
	switch(f)
	{
		case kFlagGreen :		return "green";
		case kFlagYellow :		return "yellow";
		case kFlagRed :			return "red";
	}
	return "?";
}

static void printStateData(
	eSimDataType	newStateData)
{
	switch(newStateData)
	{
		case kCurrentWeekend :
		{
			const CurrentWeekend *wk = (const CurrentWeekend * )
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("CurrentWeekend: <%d, %s, %dft, <%s>, <%s>>\n",
									wk->atTrack ? 1 : 0,
									wk->track,
									int(wk->trackLength * 3.281f),
									wk->sessions,
									wk->options);
			}
			break;
		}
		case kDriverInCar :
		{
			const DriverInCar *in = (const DriverInCar * )
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("DriverInCar: <%d>\n", in->inCar ? 1 : 0);
			}
			break;
		}
		case kGameIsPaused :
		{
			const GameIsPaused *paused = (const GameIsPaused * )
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("GameIsPaused: <%d>\n", paused->paused ? 1 : 0);
			}
			break;
		}
		case kCarSetup :
		{
			const CarSetup *setup = (const CarSetup * )
					AppGetSimData(newStateData);
			(void) setup;
			if (!formatForSpreadsheet)
			{
				printf("CarSetup: ...\n");
			}

			// It's a good idea to clear out the results of
			// changes made during a prior pit stop since they
			// will no longer be meaningful when a completely
			// new setup is applied.
			AppClearSimData(kPitStop);
			break;
		}

		case kPitStop :
		{
			const PitStop *pitstop = (const PitStop * )
					AppGetSimData(newStateData);
			(void) pitstop;
			if (!formatForSpreadsheet)
			{
				printf("PitStop: ...\n");
			}
			break;
		}

		case kDriverEntry :
		{
			const DriverEntry *entry = (const DriverEntry *)
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("Driver Joins: <%2d, <%02x>, %s>\n",
						entry->carIdx, entry->flags, entry->name);
			}
			break;
		}

		case kDriverWithdrawl :
		{
			const DriverWithdrawl *withdrawl = (const DriverWithdrawl *)
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("Driver Withdraws: <%d>\n", withdrawl->carIdx);
			}
			break;
		}

		case kSessionInfo :
		{
			const SessionInfo *info = (const SessionInfo *)
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("Session: <%d,%d>, <%s,%s,%s>, time<%4d>, tr<%d>, lr<%d>\n",
						info->sessionNum,
						info->sessionCookie,
						sessionType(info->sessionType),
						sessionState(info->sessionState),
						sessionFlag(info->sessionFlag),
						int(info->currentTime),
						int(info->timeRemaining),
						info->lapsRemaining);
			}
			break;
		}

		case kLapCrossing :
		{
			const LapCrossing *crossing = (const LapCrossing *)
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("Lap Crossing: car<%2d>, lap<%3d>, at<%10.3f>, flags<%c%c%c%c>\n",
						crossing->carIdx, crossing->lapNum, crossing->crossedAt,
						crossing->flags & kCrossTimeInvalid	? 'I' : '-',
						crossing->flags & kCrossOffTrack		? 'O' : '-',
						crossing->flags & kCrossPitted		? 'P' : '-',
						crossing->flags & kCrossBlackFlag	? 'B' : '-');
			}
			break;
		}

		case kStandings :
		{
			const Standings *s = (const Standings *)
					AppGetSimData(newStateData);
			if (!formatForSpreadsheet)
			{
				printf("Standings<%d,%sofficial>...\n",
						s->sessionNum, s->flags & kStandingsOfficial ? "" : "un");
			}
			break;
		}
	}
}

static float TEMP(float c)
{
	if (englishUnits)
		return 9.0f*c/5.0f + 32.0f;		// degrees C to degrees F
	else
		return c;
}

static float PRESSURE(float p)
{
	if (englishUnits)
		return p * 0.1450377f;			// kPa to PSI
	else
		return p;
}

static float FORCE(float f)
{
	if (englishUnits)
		return f * 0.2248f;				// N to lb
	else
		return f;
}

static float DIST1(float d)
{
	if (englishUnits)
		return d * 3.281f;				// M to ft
	else
		return d;
}

static float DIST2(float d)
{
	if (englishUnits)
		return d * 39.372f;				// M to in
	else
		return d;
}

static float VEL(float v)
{
	if (englishUnits)
		return v * 2.237f;				// M/s to MPH
	else
		return v;
}

static float ANGLE(float a)
{
	if (englishUnits)
		return a * 180.0f / 3.14159f;	// rad to deg
	else
		return a;
}

static float TORQUE(float t)
{
	if (englishUnits)
		return FORCE(DIST1(t));			// N*m to Lb*ft
	else
		return t;
}

static float POWER(float t)
{
	if (englishUnits)
		return t/745.7f;				// Watts to hp
	else
		return t;
}






static void printSample()
{
	// The sim has finished writing a new sample.  We can
	// now check to see if each of the types of sample data
	// we have requested has been written.
	const SampleHeader		*sh;
	const ChassisData		*cd;
	const WheelData			*wd;
	const TireData			*td;
	const DrivelineData		*dl;
	const DriverInput		*di;
	const GaugeData			*gd;
	const OpponentCarData	*ocd;
	if ((sh = (const SampleHeader *) AppGetSimData(kSampleHeader)) != NULL &&
		!formatForSpreadsheet)
	{
		printf("%d, %d, %d, %f, ", sh->sequence, sh->tick, sh->step, sh->time);
	}
	if ((cd = (const ChassisData *) AppGetSimData(kChassisData)) != NULL &&
		!formatForSpreadsheet)
	{
		static const float mt_per_sec_to_MPH = 2.2374145f;
		float v = (float) sqrt(cd->v[0]*cd->v[0] +
								cd->v[1]*cd->v[1] +
								cd->v[2]*cd->v[2]) * mt_per_sec_to_MPH;
		printf("%f, %f,", cd->trkPct, VEL(v));
	}
	if ((wd = (const WheelData *) AppGetSimData(kWheelData)) != NULL &&
		!formatForSpreadsheet)
	{
	}
	if ((td = (const TireData *) AppGetSimData(kTireData)) != NULL &&
		!formatForSpreadsheet)
	{
	}
	if ((dl = (const DrivelineData *) AppGetSimData(kDrivelineData)) != NULL &&
		!formatForSpreadsheet)
	{
	}
	if ((di = (const DriverInput *) AppGetSimData(kDriverInput)) != NULL &&
		!formatForSpreadsheet)
	{
	}
	if ((gd = (const GaugeData *) AppGetSimData(kGaugeData)) != NULL &&
		!formatForSpreadsheet)
	{
	}
	if ((ocd = (const OpponentCarData *) AppGetSimData(kOpponentCarData)) != NULL &&
		!formatForSpreadsheet)
	{
		// to figure out number of opponent cars (including pace car) out on track,
		// divide the number of bytes read by the size of OpponentCarData struct
		int n_cars = AppGetSimDataSize(kOpponentCarData) / sizeof(OpponentCarData);
		printf("Opp %d ", n_cars);
		if (n_cars > 0)
		{
			for (int i = 0; i < n_cars; i++)
				printf("%d,", ocd[i].carIdx);
		}
	}

	if (formatForSpreadsheet)
	{
		if (sh)
		{
			printf("%d%c%d%c%d%c%f",
					sh->sequence, S, sh->tick, S, sh->step, S, sh->time);
		}else printf("%c%c%c", S,S,S);

		if (cd)
		{
			// Compute overall velocity.
			float v = (float) sqrt(cd->v[0]*cd->v[0] +
									cd->v[1]*cd->v[1] +
									cd->v[2]*cd->v[2]);
			printf("%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f",
					S, cd->trkPct, S, VEL(v),
					S, DIST1 (cd->r[0]), S, DIST1 (cd->r[1]), S, DIST1 (cd->r[2]),
					S, VEL  (cd->v[0]), S, VEL  (cd->v[1]), S, VEL  (cd->v[2]),
					S, ANGLE(cd->q[0]), S, ANGLE(cd->q[1]), S, ANGLE(cd->q[2]));
			printf("%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f",
					S, ANGLE(cd->w[0]), S, ANGLE(cd->w[1]), S, ANGLE(cd->w[2]),
					S, cd->N[0], S, cd->N[1], S, cd->N[2],
					S, cd->H[0], S, cd->H[1]);
			printf("%c%f",
					S, TORQUE(cd->steerT));

			// Compute lateral, longitudinal, and normal G loading
			// on the chassis.
			if (sh)
			{
				static unsigned int	sequence = -1;
				static SampleHeader	sh0;
				static ChassisData	cd0;
				float				ca[3], dt;

				// We do so by computing the change in velocity from one
				// sample to the next, scaling by 1/change in time, adding
				// in gravity, and transforming this into the car's
				// coordinate system.
				if (sh->sequence != sequence ||
					(dt = (float)(sh->time - sh0.time)) <= 0.0f ||
					dt > 0.125f)
				{
					// When the sample header sequence number changes, then
					// this is the first sample since the car was dropped into
					// the world, so we have no prior sample with which to
					// compare.
					// If a data overrun occurred, then we may have missed
					// some samples and so the current and most recent
					// samples could be far apart in time.  Computing
					// G loading the way we do would be terribly inaccurate
					// in such a case.
					ca[0]		= ca[1] = ca[2] = 0.0f;

					// Squirrel away current sample and header so we
					// can compute chassis acceleration at next sample.
					sh0			= *sh;
					cd0			= *cd;
					sequence	= sh->sequence;
				}
				else
				{
					static const float	one_g	= 9.80665f;
					static const float	wg[3]	= { 0.0f, 0.0f, one_g }; // m/sec^2
					float				wa[3];
					float				m[3][3];

					// Compute acceleration (change in velocity per time) of
					// chassis in world frame from change in velocity over
					// this and prior sample.
					vector3Sub(wa, cd->v, cd0.v);			// m/sec
					vector3Scale(wa, 1.0f / dt, wa);		// m/sec^2

					// Add in gravity.
					vector3Add(wa, wa, wg);					// m/sec^2

					// Form 3x3 rotation matrix from car relative
					// coordinates to world coordinates from the
					// yaw, pitch and roll of the chassis.
					matrix33FromEuler(m, cd->q[0], cd->q[1], cd->q[2]);

					// Rotate chassis acceleration vector from world frame
					// into car's frame (note that to do this we're
					// multiplying by the matrix transpose).
					matrix33MulTVector3(ca, m, wa);

					// Scale to convert from m/sec^2 to G's.
					vector3Scale(ca, 1.0f / one_g, ca);		// G's

					// Squirrel away current sample and header so we
					// can compute chassis acceleration at next sample.
					sh0 = *sh;
					cd0 = *cd;
				}

				// The car's reference frame has +x forwards, +y to the
				// left, and +z up.  So longitudinal is ca[0], lateral
				// is ca[1], and normal is ca[2].
				printf("%c%f%c%f%c%f",
						S, ca[0], S, ca[1], S, ca[2]);
			}
			else printf("%c%c%c", S,S,S);
		}
		else printf("%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c",
					  S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S);
		{
			const DriverInput *div = (const DriverInput *)
					AppGetSimData(kDriverInput);
			if (div && (uintptr_t)div > 0x10000)
			{
				printf("%c%f%c%f%c%f%c%f%c%d",
					S, div->throttle, S, div->brake, S, div->clutch,
					S, ANGLE(div->steer), S, div->gear);
			} else printf("%c%c%c%c%c", S,S,S,S,S);
		}
		{
			const DrivelineData *dlv = (const DrivelineData *)
					AppGetSimData(kDrivelineData);
			if (dlv && (uintptr_t)dlv > 0x10000)
			{
				printf("%c%f%c%f%c%f",
					S, dlv->engRPM, S, POWER(dlv->engPower), S, TORQUE(dlv->engT));
			}
			else printf("%c%c%c", S,S,S);
		}
		for(int w = 0; w < 4; ++w)
		{
			if (td && wd)
			{
				printf("%c%f%c%f%c%f%c%f%c%f%c%f%c%f%c%f",
					S, FORCE(wd->w[w].Fz),
					S, ANGLE(wd->w[w].v), S, DIST2(wd->w[w].defl),
					S, ANGLE(wd->w[w].camber),
					S, PRESSURE(td->w[w].pressure), S, TEMP(td->w[w].temp[0]),
					S, TEMP(td->w[w].temp[1]), S, TEMP(td->w[w].temp[2]));
			}
			else printf("%c%c%c%c%c%c%c%c", S,S,S,S,S,S,S,S);
		}
	}
	printf("\n");
}

/*
 ***************************************************************************
 ***************************************************************************
 */
int main(void)
{
	// By default the sim will run as a HIGH_PRIORITY_CLASS process.
	// If we don't do anything special, we'll be run as a
	// NORMAL_PRIORITY_CLASS process.  Since the sim will use
	// as much CPU time as it can, if it is running at a higher
	// priority class than we are, we will only be given the
	// CPU sporadically, and will more than likely not be able
	// to read telemetry quickly enough.  This would be particularly
	// bad if you need to use the telemetry in real-time (for
	// controlling a motion platform, for example).
	// To be serviced by the CPU adequately, we need to run
	// ourself at the same priority class as the sim.  This can
	// be done by either using this line of code (to run ourself
	// as a HIGH_PRIORITY_CLASS process), or by telling the sim
	// to run itself at NORMAL_PRIORITY_CLASS (by passing it
	// -kh0  (that's minus-k-h-zero) on the command line).
	// You probably should not run your telemetry gathering app
	// at a higher priority than the sim, particularly if you're
	// gathering telemetry at the 288Hz physics rate, since this
	// is likely to cause a lot of context switching.
	// So either use this line, or -kh0, but not both.
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

	// Assume that the [Telemetry] id= variable in core.ini is empty.
	// Otherwise, we'd have to pass a second parameter to AppBegin(),
	// indicating to which instance of the sim we're to connect.
	// The only scenario where this would be needed is where multiple
	// instances of a race server were being run on one machine at
	// the same time, and you wanted to gather telemetry from all
	// of them.  You would then need to also run multiple instances
	// of your telemetry gathering application, and would use
	// AppBegin()'s second parameter, along with the core.ini
	// [Telemetry] id= variable, to ensure that each instance of your
	// telemetry gathering app connected to the correct instance of
	// the race server.
	if (AppBegin("NR2003"))
	{
		// Define which types of telemetry we want the sim to
		// write to us.
		const eSimDataType desired[] =
		{
			kSampleHeader,
			kChassisData,
			kWheelData,
			kTireData,
			kDrivelineData,
			kDriverInput,

			kGaugeData,
			kOpponentCarData,

			kCurrentWeekend,		// Want state information, too
			kDriverInCar,
			kGameIsPaused,
			kCarSetup,
			kPitStop,
			kDriverEntry,
			kDriverWithdrawl,
			kSessionInfo,
			kLapCrossing,
			kStandings
		};

		// Note that, since these requests are written into a queue
		// that was initialized during AppBegin, we do not need to
		// wait for the sim to become active before issuing them.
		// When the sim does its correspondng SimBegin, it will
		// find this queue in memory and read our requests from
		// it.  The sim does check this queue frequently, so we
		// can issue these requests any time. 
		// Also note, however, that the queue into which these
		// requests are written is small, so you wouldn't want
		// to write these things repeatedly without checking the
		// return values to ensure the requests could be queued.
 		(void) AppRequestDataItems(sizeof(desired)/sizeof(desired[0]),
 								   desired);
		(void) AppRequestDataAtPhysicsRate(sampleAtPhysicsRate);
		(void) AppEnableSampling(true);
		openSnapshotFile();
		openMemFile();

		// All we do is print what telemetry we've read from the
		// sim, so there's nothing for us to do until the sim
		// becomes active.  Just hang around until it does.
		if (!formatForSpreadsheet)
		{
			printf("Waiting for the sim to become active...\n");
		}
		while(!AppCheckIfSimActiveQ())
		{
			pollMemCommands();
			Sleep(100);
		}

		// Ok, the sim is now active.  Once the sim exits, it will
		// indicate so.  Keep processing data from the sim until
		// it goes away.
		if (!formatForSpreadsheet)
		{
			printf("...Sim is now active...\n");
		}
		else
		{
			// We're outputting for import into a spreadsheet.  Print
			// out the column headers.  Note that columns with an
			// asterisk(*) are computed from the telemetry, they are
			// not generated by the simulator.
			printf("Sequence%cTick%cStep%cTime"								// 3 seps
					"%cTrkPct%cSpeed*"										// 2 seps
					"%cx%cy%cz%cVx%cVy%cVz"									// 6 seps
					"%cyaw%cpitch%croll%cVyaw%cVpitch%cVroll"				// 6 seps
					"%cNx%cNy%cNz%cHx%cHy"									// 5 seps
					"%cSteerTorque"											// 1 sep
					"%cLongG*%cLatG*%cNormG*"								// 3 seps
					"%cthrottle%cbrake%cclutch%csteer%cgear"				// 5 seps
					"%cRPM%cpower%cengF"									// 3 seps
					"%clfFz%clfV%clfBump%clfCamb%clfPress%clfI%clfM%clfO"	// 8 seps
					"%crfFz%crfV%crfBump%crfCamb%crfPress%crfI%crfM%crfO"	// 8 seps
					"%clrFz%clrV%clrBump%clrCamb%clrPress%clrI%clrM%clrO"	// 8 seps
					"%crrFz%crrV%crrBump%crrCamb%crrPress%crrI%crrM%crrO",	// 8 seps
					S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,	// 20
					S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,	// 40
					S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,S,	// 60
					S,S,S,S,S,S);								// 66
			printf("\n");
		}

		// Keep reading from the sim until it goes away, and there's
		// no telemetry left to be read.
		// See if the sim has written anything to us. The timeout
		// used here is fairly arbitrary since this program does
		// nothing but read telemetry from the sim and write it
		// to stdout.  If your program is in its own loop where
		// reading telemetry from the sim is only one of its tasks,
		// you'd probably want to use a timeout of 0.  This tells
		// AppWaitForNewSample to read any telemetry that is
		// queued, but not to wait if there is none available.
		// You should not, however, allow your program to gobble
		// up all available CPU time, since this will adversely
		// affect the sim's performance.  You could, for example
		// call Sleep() with an appropriate number of milliseconds
		// somewhere in your loop, or you could pass AppWaitForNewSample
		// a positive timeout.  Under the covers, it will use Sleep()
		// to ensure at least this amount of time passes before
		// returning.
		const int timeOutMs = 100;
		eSimDataType newStateData;
		bool newSample;
		while((newSample = AppWaitForNewSample(&newStateData, timeOutMs)) == true ||
			  newStateData != kNoStateInfo ||
			  AppCheckIfSimActiveQ())
		{
			pollMemCommands();
			// See if any new telemetry from the sim.
			if (newSample || newStateData != kNoStateInfo)
			{
				// Yes, it has either written a piece of state data,
				// or has finished writing a new sample.  Which one?
				// Either newStateData won't equal kNoStateInfo
				// (meaning a piece of state data was read), or
				// AppWaitForNewSample will return true (indicating
				// a complete sample is now available), but never
				// both at once.
				if (newStateData != kNoStateInfo)
				{
					// It was state data.  Process it.
					printStateData(newStateData);
				}
				else if (newSample)
				{
					printSample();
					writeSnapshot();
					pollMemCommands();

					// While it is not strictly necessary that you call
					// this function, it is a very good idea to do so
					// after processing each sample.
					// The various individual pieces of telemetry that
					// make up this sample are only guaranteed to be
					// self-consistent until the next time you call
					// AppWaitForNewSample.  Once called, it is entirely
					// possible that some parts of the sample will be
					// from the prior sampling, and some parts from the
					// current sampling.  If you need the sample data
					// across calls to AppWaitForNewSample, you should
					// copy it to memory that you control.
					AppClearSample();
				} else {
					// no sample this tick but still service mem writes (e.g. in menus)
					pollMemCommands();
				}

				// If, for some reason, our program went away for a while
				// and wasn't reading telemetry from the sim, the buffer
				// into which the sim writes the telemetry might have
				// filled up.  Whenever there is insufficient space
				// available for the sim to write telemetry into the buffer,
				// it simply discards the telemetry, and sets the flag
				// that is checked here.
				// You should carefully consider where you place this test.
				if (AppCheckIfSimDataOverrunQ())
				{
					// Note that this means that some telemetry was lost,
					// If it's important to your application that the
					// sample data be continuous (for example, if you're
					// controlling a motion platform), you would want to
					// deal with that possibility here.
					// If it's important to your application that it deal
					// with samples in real-time (back to the motion platform
					// idea), you should also note that since the telemetry
					// buffer is large enough to hold many samples, and since
					// it has overflowed, there are likely to be many samples
					// in the buffer that have been there for a while, and
					// should probably be considered 'stale'.
					printf("*OVERRUN*\n");

					// The flag will remain set until we (the app) explicitly
					// clear it.  That is, the sim won't reset the overrun
					// flag once it sees that there is space available in
					// the buffer.
					AppClearSimDataOverrun();
				}
			}
		}

		// Since we successfully called AppBegin, we are obliged to
		// call AppEnd.
		if (!formatForSpreadsheet)
		{
			printf("\n...Sim has deactivated.\n");
		}
		closeSnapshotFile();
		closeMemFile();
		AppEnd();
	}
	return 0;
}
