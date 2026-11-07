#pragma once
// nr2003_snapshot.h — 4K binary snapshot for Wine<->Linux sharing via Z:\dev\shm
// Written by tsample.exe (MinGW) to Z:\dev\shm\nr2003_telem each tick (36/288Hz)
// Linux readers mmap /dev/shm/nr2003_telem and guard torn reads via sequence==sequence2
#include <cstdint>
#include "papytelemapp.h"

#pragma pack(push,1)

#define NR2003_SNAPSHOT_MAGIC      0x3332524Eu // "NR23" LE
#define NR2003_SNAPSHOT_VERSION    1u
#define NR2003_SNAPSHOT_TAIL_MAGIC 0xEBC4A698u // matches WindTunnel Companion tail
#define NR2003_SNAPSHOT_PATH_WINE  "Z:\\dev\\shm\\nr2003_telem"
#define NR2003_SNAPSHOT_PATH_LINUX "/dev/shm/nr2003_telem"
#define NR2003_SNAPSHOT_SIZE       4096u
#define NR2003_SNAPSHOT_MAX_OPPONENTS 48u

struct Nr2003Snapshot {
    uint32_t magic;       // NR2003_SNAPSHOT_MAGIC
    uint32_t version;     // NR2003_SNAPSHOT_VERSION
    uint32_t size;        // sizeof(Nr2003Snapshot) == 4096
    uint32_t sequence;    // SampleHeader.sequence (0 if unavailable, bumps on reset/shift-R)
    uint32_t tick;        // SampleHeader.tick (each 1/36s)
    int32_t  step;        // SampleHeader.step (0..7 physics, 7 at 36Hz)
    double   time;        // SampleHeader.time (sim seconds since reset, paused)
    PapyTelem::ChassisData   chassis;   // 76
    PapyTelem::WheelData     wheel;     // 64
    PapyTelem::TireData      tire;      // 64
    PapyTelem::DrivelineData driveline; // 12
    PapyTelem::DriverInput   input;     // 20
    PapyTelem::GaugeData     gauge;     // 25 (bool bitfields packed to 1B, pack(1))
    uint32_t opponentCount; // 0..48, actual valid entries in opponents[]
    uint32_t _pad0;         // align to 4
    PapyTelem::OpponentCarData opponents[48]; // 44*48=2112
    uint32_t sequence2;     // copy of sequence for torn-read guard
    uint32_t tailMagic;     // NR2003_SNAPSHOT_TAIL_MAGIC
    uint8_t  reserved[1675]; // pad to 4096
};

#pragma pack(pop)

static_assert(sizeof(Nr2003Snapshot)==NR2003_SNAPSHOT_SIZE, "snapshot must be 4096");
static_assert(sizeof(PapyTelem::ChassisData)==76, "ChassisData");
static_assert(sizeof(PapyTelem::WheelData)==64, "WheelData");
static_assert(sizeof(PapyTelem::TireData)==64, "TireData");
static_assert(sizeof(PapyTelem::DrivelineData)==12, "DrivelineData");
static_assert(sizeof(PapyTelem::DriverInput)==20, "DriverInput");
static_assert(sizeof(PapyTelem::GaugeData)==25, "GaugeData");
static_assert(sizeof(PapyTelem::OpponentCarData)==44, "OpponentCarData");
