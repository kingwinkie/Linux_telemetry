#pragma once
#include <QMainWindow>
#include <QTimer>
#include <QTableWidget>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTabWidget>
#include <QFileDialog>
#include <QLineEdit>
#include <QHash>
#include <QVector>
#include <cstdint>
#include "../nr2003_mem.h"
#include "../nr2003_blocks.h"

class QFile;
class QTextStream;
struct Nr2003Snapshot;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent=nullptr);
    ~MainWindow() override;
private slots:
    void pollSnapshot();
    void writeBlocks();
    void readMem();
    void refreshLiveValues();
    void applyValueToSelection();
    void syncEditorFromSelection();
    void pollMemAck();
    void saveLiveMemCsv();
    void stickinessProbe();
    void exportMemlocCsv();
    void showValueInTable(uint32_t rva, float v);
    void loadFairCsvExe();
    void readExeValues();
    void writeExeValues();
    void toggleTelemetryLog();
    void selectTelemetryLogFields();
    void openTelemetryLogDir();
private:
    void setupTelemetryTab();
    void startTelemetryLog();
    void stopTelemetryLog();
    void appendTelemetryLog(const Nr2003Snapshot &s);
    void setupMemTab();
    void setupExeTab();
    int  loadMemlocTable(const QHash<uint32_t,float> &live, QString *msg);
    bool readSnapshotRaw(uint8_t *buf);
    bool sendMemCmd(Nr2003MemCmd &cmd, int *statusOut, int pollMs=1000);
    void markReadOnlyTables();
    QHash<uint32_t,float> loadMemCsvValues(bool *ok, QString *msg) const;
    QHash<uint32_t,float> readAllBlockValues(int perBlockMs, QString *msg);
    int  loadMemCsvTable(uint8_t module, QTableWidget *tbl,
                         const QHash<uint32_t,float> &live,
                         const QHash<uint32_t,float> &csvValues, QString *msg);
    void reloadBlockTables(bool tryLive);
    void refreshMemTabTitles();
    void onMemItemChanged(QTableWidgetItem *it);
    QHash<uint32_t,float> m_probeOrig;
    uint32_t m_probeRva = 0;
    float m_probeValue = 0.f;
    bool m_probeArmed = false;
    QHash<uint32_t,float> m_userEdits;
    QDoubleSpinBox *valEdit = nullptr;
    QLabel *valInfo = nullptr;
    QHash<uint32_t,float> m_baseline;
    bool m_suppressEdits = false;
    static bool slotIsGameOwned(const Nr2003BlockDesc &b, uint16_t s);
    QTabWidget *tabs;
    // Telemetry tab
    QTableWidget *teleTable;
    QLabel *teleStatus;
    QTimer *pollTimer;
    uint32_t lastSeq=0;
    // Telemetry CSV logging
    QPushButton *teleLogBtn = nullptr;
    QPushButton *teleLogFieldsBtn = nullptr;
    QPushButton *teleLogOpenBtn = nullptr;
    QLabel *teleLogStatus = nullptr;
    QFile *teleLogFile = nullptr;
    QTextStream *teleLogStream = nullptr;
    QVector<bool> teleLogSelected;
    quint64 teleLogRows = 0;
    uint32_t teleLogLastSeq = 0;
    uint32_t teleLogLastTick = 0;
    bool teleLogHaveLast = false;
    // Mem tab — 4 separated modules (fix 65-vs-764: fair_*.csv 1521/449/193/765 total 2928, old code capped 64)
    QTabWidget *memTabs;
    QTableWidget *chassisTable;
    QTableWidget *engineTable;
    QTableWidget *wheelTable;
    QTableWidget *garageTable;
    QTableWidget *memlocTable;   // 23 x 40 floats, the per-car-variant tables the chassis blocks point at
    QPushButton *saveCsvBtn;
    QPushButton *probeBtn;
    QPushButton *exportMemlocBtn;
    QTableWidget *memTable; // alias to current visible table (for compat)
    QLabel *memStatus;
    // EXE File tab (offline Nr2003.eXe editing via csv/fair_*.csv)
    QTabWidget *exeTabs;
    QTableWidget *exChassisTable, *exEngineTable, *exWheelTable, *exGarageTable;
    QLabel *exeStatus;
    QLineEdit *exePathEdit;
    QList<QTableWidget*> exTables() const;
    QPushButton *loadBtn;
    QPushButton *writeBlockBtn;
    QPushButton *readBtn;
    QPushButton *readBlockBtn;
    uint32_t memSeq=1;
    QTimer *ackTimer;
    int findNr2003Pid();
    bool readProcFloat(int pid, uint32_t va, float &out);
    QList<QTableWidget*> allMemTables() const;
};
