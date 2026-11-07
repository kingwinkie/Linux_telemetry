#include "mainwindow.h"
#include "../nr2003_snapshot.h"
#include "../nr2003_mem.h"
#include "../nr2003_blocks.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFile>
#include <QMessageBox>
#include <QFileInfo>
#include <QTextStream>
#include <QDir>
#include <cstring>
#include <cmath>
#include <QThread>
#include <QHash>
#include <QLineEdit>
#include <QAbstractItemView>
#include <QAbstractItemDelegate>
#include <QApplication>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <QProcess>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QDateTime>
#include <QVector>
#include <QPair>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QDesktopServices>
#include <QUrl>
#include <functional>
#include <utility>
static void logQt(const QString &msg){
    QFile f("/tmp/nr2003_qt.log");
    if(f.open(QIODevice::Append|QIODevice::Text)){
        QTextStream s(&f);
        s<<QDateTime::currentDateTime().toString(Qt::ISODateWithMs)<<" "<<msg<<"\n";
    }
    fprintf(stderr,"[qt] %s\n", qUtf8Printable(msg));
}

// Single CSV data directory: override with NR2003_CSV_DIR, otherwise look for
// a "csv" folder beside telemetry_linux/ or in the current working tree.
static QString csvDir() {
    const QString env = qEnvironmentVariable("NR2003_CSV_DIR");
    if (!env.isEmpty() && QDir(env).exists()) return QDir(env).absolutePath();

    const QStringList starts = {QCoreApplication::applicationDirPath(), QDir::currentPath()};
    for (const QString &start : starts) {
        QDir d(start);
        for (int i = 0; i < 8; ++i) {
            const QString candidate = d.filePath("csv");
            if (QDir(candidate).exists()) return QDir(candidate).absolutePath();
            if (!d.cdUp()) break;
        }
    }
    return QDir(QCoreApplication::applicationDirPath()).filePath("csv");
}

// Telemetry CSV logs live in a "logs" subfolder of the nr2003_qt binary
// directory.  NR2003_LOG_DIR overrides it (handy for tests).
static QString telemetryLogDir() {
    const QString env = qEnvironmentVariable("NR2003_LOG_DIR");
    const QString dir = env.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath("logs")
        : QDir(env).absolutePath();
    QDir().mkpath(dir);
    return dir;
}

static QString csvEscape(const QString &s) {
    if (!s.contains(',') && !s.contains('"') && !s.contains('\n') && !s.contains('\r'))
        return s;
    QString t = s;
    t.replace('"', "\"\"");
    return "\"" + t + "\"";
}

static QString logNum(double v) { return QString::number(v, 'g', 9); }

// One selectable telemetry column.  The getter reads directly from the 4K
// snapshot so adding a field is one entry in telemetryLogFields().
struct TelemetryLogField {
    QString group;
    QString header;
    QString label;
    bool defaultOn = true;
    std::function<QString(const Nr2003Snapshot&)> value;
};

static const QVector<TelemetryLogField>& telemetryLogFields() {
    static const QVector<TelemetryLogField> fields = [] {
        QVector<TelemetryLogField> f;
        auto add = [&](const QString &group, const QString &header, const QString &label,
                       bool on, std::function<QString(const Nr2003Snapshot&)> fn) {
            f.append({group, header, label, on, std::move(fn)});
        };

        add("Header", "seq", "Sequence", true,
            [](const Nr2003Snapshot &s){ return QString::number(s.sequence); });
        add("Header", "tick", "Tick", true,
            [](const Nr2003Snapshot &s){ return QString::number(s.tick); });
        add("Header", "step", "Physics step", true,
            [](const Nr2003Snapshot &s){ return QString::number(s.step); });
        add("Header", "sim_time", "Sim time (s)", true,
            [](const Nr2003Snapshot &s){ return QString::number(s.time, 'f', 6); });

        add("Chassis", "trk_pct", "Track position (%)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.trkPct); });
        add("Chassis", "pos_x", "Position X (m)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.r[0]); });
        add("Chassis", "pos_y", "Position Y (m)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.r[1]); });
        add("Chassis", "pos_z", "Position Z (m)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.r[2]); });
        add("Chassis", "vel_x", "Velocity X (m/s)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.v[0]); });
        add("Chassis", "vel_y", "Velocity Y (m/s)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.v[1]); });
        add("Chassis", "vel_z", "Velocity Z (m/s)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.v[2]); });
        add("Chassis", "speed_mph", "Speed (mph)", true,
            [](const Nr2003Snapshot &s){
                const double vx=s.chassis.v[0], vy=s.chassis.v[1], vz=s.chassis.v[2];
                return logNum(sqrt(vx*vx + vy*vy + vz*vz) * 2.23694);
            });
        add("Chassis", "yaw", "Yaw (rad)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.q[0]); });
        add("Chassis", "pitch", "Pitch (rad)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.q[1]); });
        add("Chassis", "roll", "Roll (rad)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.q[2]); });
        add("Chassis", "angvel_yaw", "Yaw rate (rad/s)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.w[0]); });
        add("Chassis", "angvel_pitch", "Pitch rate (rad/s)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.w[1]); });
        add("Chassis", "angvel_roll", "Roll rate (rad/s)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.w[2]); });
        add("Chassis", "normal_x", "Track normal X", false,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.N[0]); });
        add("Chassis", "normal_y", "Track normal Y", false,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.N[1]); });
        add("Chassis", "normal_z", "Track normal Z", false,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.N[2]); });
        add("Chassis", "heading_x", "Track heading X", false,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.H[0]); });
        add("Chassis", "heading_y", "Track heading Y", false,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.H[1]); });
        add("Chassis", "steer_torque", "Steering torque (N*m)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.chassis.steerT); });

        const char *wheelName[4] = {"LF", "RF", "LR", "RR"};
        for (int i = 0; i < 4; ++i) {
            const QString wn = QString::fromLatin1(wheelName[i]);
            const QString wl = wn.toLower();
            add("Wheels", QString("wheel_%1_v").arg(wl), QString("%1 wheel speed (rad/s)").arg(wn), true,
                [i](const Nr2003Snapshot &s){ return logNum(s.wheel.w[i].v); });
            add("Wheels", QString("wheel_%1_defl").arg(wl), QString("%1 shock deflection (m)").arg(wn), true,
                [i](const Nr2003Snapshot &s){ return logNum(s.wheel.w[i].defl); });
            add("Wheels", QString("wheel_%1_camber").arg(wl), QString("%1 camber (rad)").arg(wn), true,
                [i](const Nr2003Snapshot &s){ return logNum(s.wheel.w[i].camber); });
            add("Wheels", QString("wheel_%1_fz").arg(wl), QString("%1 normal force (N)").arg(wn), true,
                [i](const Nr2003Snapshot &s){ return logNum(s.wheel.w[i].Fz); });
        }

        for (int i = 0; i < 4; ++i) {
            const QString wn = QString::fromLatin1(wheelName[i]);
            const QString wl = wn.toLower();
            add("Tires", QString("tire_%1_inner").arg(wl), QString("%1 inner temp (C)").arg(wn), true,
                [i](const Nr2003Snapshot &s){ return logNum(s.tire.w[i].temp[0]); });
            add("Tires", QString("tire_%1_middle").arg(wl), QString("%1 middle temp (C)").arg(wn), true,
                [i](const Nr2003Snapshot &s){ return logNum(s.tire.w[i].temp[1]); });
            add("Tires", QString("tire_%1_outer").arg(wl), QString("%1 outer temp (C)").arg(wn), true,
                [i](const Nr2003Snapshot &s){ return logNum(s.tire.w[i].temp[2]); });
            add("Tires", QString("tire_%1_pressure").arg(wl), QString("%1 pressure (kPa)").arg(wn), true,
                [i](const Nr2003Snapshot &s){ return logNum(s.tire.w[i].pressure); });
        }

        add("Driveline", "eng_rpm", "Engine RPM", true,
            [](const Nr2003Snapshot &s){ return logNum(s.driveline.engRPM); });
        add("Driveline", "eng_power", "Engine power", true,
            [](const Nr2003Snapshot &s){ return logNum(s.driveline.engPower); });
        add("Driveline", "eng_torque", "Engine torque (N*m)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.driveline.engT); });

        add("Driver input", "throttle", "Throttle", true,
            [](const Nr2003Snapshot &s){ return logNum(s.input.throttle); });
        add("Driver input", "brake", "Brake", true,
            [](const Nr2003Snapshot &s){ return logNum(s.input.brake); });
        add("Driver input", "clutch", "Clutch", true,
            [](const Nr2003Snapshot &s){ return logNum(s.input.clutch); });
        add("Driver input", "steer", "Steering (rad)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.input.steer); });
        add("Driver input", "gear", "Gear", true,
            [](const Nr2003Snapshot &s){ return QString::number(s.input.gear); });

        add("Gauges", "gauge_rpm", "Gauge RPM", true,
            [](const Nr2003Snapshot &s){ return logNum(s.gauge.rpm); });
        add("Gauges", "water_temp_c", "Water temp (C)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.gauge.waterTemp); });
        add("Gauges", "fuel_press_bar", "Fuel pressure (bar)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.gauge.fuelPress); });
        add("Gauges", "oil_temp_c", "Oil temp (C)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.gauge.oilTemp); });
        add("Gauges", "oil_press_bar", "Oil pressure (bar)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.gauge.oilPress); });
        add("Gauges", "voltage", "Voltage (V)", true,
            [](const Nr2003Snapshot &s){ return logNum(s.gauge.voltage); });
        add("Gauges", "rpm_warning", "RPM warning", true,
            [](const Nr2003Snapshot &s){ return QString(s.gauge.rpmWarning ? "1" : "0"); });
        add("Gauges", "water_temp_warning", "Water temp warning", true,
            [](const Nr2003Snapshot &s){ return QString(s.gauge.waterTempWarning ? "1" : "0"); });
        add("Gauges", "fuel_pressure_warning", "Fuel pressure warning", true,
            [](const Nr2003Snapshot &s){ return QString(s.gauge.fuelPressureWarning ? "1" : "0"); });
        add("Gauges", "oil_pressure_warning", "Oil pressure warning", true,
            [](const Nr2003Snapshot &s){ return QString(s.gauge.oilPressureWarning ? "1" : "0"); });

        add("Opponents", "opponent_count", "Opponent count", true,
            [](const Nr2003Snapshot &s){ return QString::number(s.opponentCount); });
        add("Opponents", "opponents", "Opponent detail (JSON)", false,
            [](const Nr2003Snapshot &s){
                const uint32_t n = qMin<uint32_t>(s.opponentCount, NR2003_SNAPSHOT_MAX_OPPONENTS);
                QString out = "[";
                for (uint32_t i = 0; i < n; ++i) {
                    const auto &o = s.opponents[i];
                    if (i) out += ",";
                    QString item = QString("{\"i\":%1,\"t\":%2").arg(o.carIdx).arg(logNum(o.trkPct));
                    item += QString(",\"r\":[%1,%2,%3]").arg(logNum(o.r[0]), logNum(o.r[1]), logNum(o.r[2]));
                    item += QString(",\"v\":[%1,%2,%3]").arg(logNum(o.v[0]), logNum(o.v[1]), logNum(o.v[2]));
                    item += QString(",\"q\":[%1,%2,%3]}").arg(logNum(o.q[0]), logNum(o.q[1]), logNum(o.q[2]));
                    out += item;
                }
                out += "]";
                return out;
            });

        return f;
    }();
    return fields;
}

// Checkable tree used by "Select fields...".  Group items are tri-state and
// toggle all of their children; leaves map to one telemetryLogFields() entry.
class TelemetryFieldDialog : public QDialog {
public:
    TelemetryFieldDialog(const QVector<TelemetryLogField> &fields,
                         const QVector<bool> &selected, QWidget *parent)
        : QDialog(parent), m_fieldCount(fields.size()) {
        setWindowTitle("Telemetry fields to log");
        resize(680, 720);
        auto *lay = new QVBoxLayout(this);
        auto *hint = new QLabel("Choose the values written to each CSV row. "
                                "The wall-clock time column is always included.", this);
        hint->setWordWrap(true);
        lay->addWidget(hint);

        m_tree = new QTreeWidget(this);
        m_tree->setHeaderLabels({"Telemetry value", "CSV column"});
        m_tree->setColumnWidth(0, 400);
        m_tree->setRootIsDecorated(true);
        lay->addWidget(m_tree, 1);

        QHash<QString, QTreeWidgetItem*> groups;
        for (int i = 0; i < fields.size(); ++i) {
            const auto &f = fields[i];
            QTreeWidgetItem *g = groups.value(f.group);
            if (!g) {
                g = new QTreeWidgetItem(m_tree);
                g->setText(0, f.group);
                g->setFlags(g->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
                g->setCheckState(0, Qt::Unchecked);
                groups.insert(f.group, g);
            }
            auto *it = new QTreeWidgetItem(g);
            it->setText(0, f.label);
            it->setText(1, f.header);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(0, selected.value(i, f.defaultOn) ? Qt::Checked : Qt::Unchecked);
            it->setData(0, Qt::UserRole, i);
        }
        m_tree->expandAll();

        connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *it, int col) {
            if (m_updating || col != 0) return;
            m_updating = true;
            if (!it->parent()) {
                const Qt::CheckState cs = it->checkState(0);
                if (cs != Qt::PartiallyChecked)
                    for (int j = 0; j < it->childCount(); ++j)
                        it->child(j)->setCheckState(0, cs);
            }
            m_updating = false;
        });

        auto *selLay = new QHBoxLayout;
        auto *allBtn = new QPushButton("Select all", this);
        auto *noneBtn = new QPushButton("Clear all", this);
        selLay->addWidget(allBtn);
        selLay->addWidget(noneBtn);
        selLay->addStretch();
        lay->addLayout(selLay);

        auto setAll = [this](Qt::CheckState cs) {
            m_updating = true;
            for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
                QTreeWidgetItem *gi = m_tree->topLevelItem(g);
                gi->setCheckState(0, cs);
                for (int j = 0; j < gi->childCount(); ++j)
                    gi->child(j)->setCheckState(0, cs);
            }
            m_updating = false;
        };
        connect(allBtn, &QPushButton::clicked, this, [setAll]{ setAll(Qt::Checked); });
        connect(noneBtn, &QPushButton::clicked, this, [setAll]{ setAll(Qt::Unchecked); });

        auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
        lay->addWidget(box);
    }

    QVector<bool> selection() const {
        QVector<bool> sel(m_fieldCount, false);
        for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
            QTreeWidgetItem *gi = m_tree->topLevelItem(g);
            for (int j = 0; j < gi->childCount(); ++j) {
                QTreeWidgetItem *it = gi->child(j);
                const int idx = it->data(0, Qt::UserRole).toInt();
                if (idx >= 0 && idx < sel.size())
                    sel[idx] = (it->checkState(0) == Qt::Checked);
            }
        }
        return sel;
    }

private:
    QTreeWidget *m_tree = nullptr;
    int m_fieldCount = 0;
    bool m_updating = false;
};

// Type-aware formatting for the fair_*.csv values.  A float must round-trip
// through 9 significant digits; a double needs 17.  Long values are integers
// (gear counts etc.), not floats.
static bool csvTypeIsDouble(const QString &t) { return t.trimmed().startsWith("doub", Qt::CaseInsensitive); }
static bool csvTypeIsLong  (const QString &t) { return t.trimmed().startsWith("long", Qt::CaseInsensitive); }
static QString csvValueText(double val, const QString &type) {
    if (csvTypeIsDouble(type)) return QString::number(val, 'g', 17);
    if (csvTypeIsLong(type))   return QString::number(qint64(val));
    return QString::number(val, 'g', 9);
}

// Load a fair_*.csv into a table.
//
// Live Mem:  column 0 is the runtime RVA, i.e. (&H-1) after stripping the
//            0x400000 image base.  (&H is stored as &H = RVA+1 because the
//            original tester treated it as a 1-based address.)
//
// EXE File:  column 0 is the raw static-file offset, i.e. &H-1.  Never map it
//            through the PE section table: &H already describes where the
//            value lives in the file, and section RVA != file offset for
//            some .data layouts.
static bool loadFairCsvFile(const QString &path, QTableWidget *tbl, QString *msg,
                            bool staticFileOffsets=false) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly|QIODevice::Text)) return false;
    QTextStream in(&f);
    QString header = in.readLine();
    tbl->setRowCount(0);
    int loaded=0;
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;
        QStringList parts; bool inQ=false; QString cur;
        for (QChar c: line) { if (c=='\"') inQ=!inQ; else if (c==',' && !inQ) { parts<<cur; cur.clear(); } else cur+=c; }
        parts<<cur;
        if (parts.size()<2) continue;
        QString addrStr, typeStr="Sing", labelStr, valStr;
        for (auto &p: parts) { QString t=p.trimmed(); if (t.startsWith("&H")) addrStr=t; }
        if (addrStr.isEmpty()) {
            if (header.contains("RVA")) { addrStr = parts[0].trimmed(); if (!addrStr.startsWith("0x") && !addrStr.startsWith("&H")) continue; if (addrStr.startsWith("0x")) addrStr="&H"+addrStr.mid(2); } else continue;
        }
        if (parts.size()>=5) { typeStr = parts[1].trimmed(); valStr = parts[2].trimmed(); labelStr = parts[4].trimmed(); }
        bool ok=false; uint32_t va = addrStr.mid(2).toUInt(&ok,16); if (!ok) continue;
        uint32_t addr;
        if (staticFileOffsets) {
            // &H2EE57D is file offset 0x2EE57C (3400.5 at that byte).
            addr = (va > 0) ? va - 1 : va;
        } else {
            uint32_t raw = (va >= 0x400000) ? va - 0x400000 : va; // &H may be RVA (0x2EE57D fair_*.csv) or VA (0x6EE57D)
            addr = (raw > 0) ? raw - 1 : raw;                     // &H is 1-based: live RVA = &H-1
        }
        double val = valStr.toDouble(&ok); if (!ok) val=0;
        int row = tbl->rowCount(); tbl->insertRow(row);
        tbl->setItem(row,0,new QTableWidgetItem(QString("0x%1").arg(addr,8,16,QChar('0')).toUpper()));
        tbl->setItem(row,1,new QTableWidgetItem(addrStr));
        tbl->setItem(row,2,new QTableWidgetItem(labelStr));
        tbl->setItem(row,3,new QTableWidgetItem(typeStr));
        QTableWidgetItem *it = new QTableWidgetItem(csvValueText(val, typeStr));
        it->setFlags(it->flags()|Qt::ItemIsEditable); tbl->setItem(row,4,it);
        ++loaded;
    }
    if (msg) *msg = staticFileOffsets
        ? QString("Loaded %1 entries from %2 — &H-1 is the static file offset; edit Value then Write values to EXE")
              .arg(loaded).arg(QFileInfo(path).fileName())
        : QString("Loaded %1 entries from %2 — edit Value col then Read/Write (VA=0x400000+RVA)")
              .arg(loaded).arg(QFileInfo(path).fileName());
    return loaded>0;
}

QList<QTableWidget*> MainWindow::exTables() const { return {exChassisTable, exEngineTable, exWheelTable, exGarageTable}; }
QList<QTableWidget*> MainWindow::allMemTables() const {
    return {chassisTable, engineTable, wheelTable, garageTable, memlocTable};
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("NR2003 Telemetry + Live Mem (Qt6)");
    resize(1100, 650);
    tabs = new QTabWidget(this);
    setCentralWidget(tabs);
    setupTelemetryTab();
    setupMemTab();
    pollTimer = new QTimer(this);
    connect(pollTimer, &QTimer::timeout, this, &MainWindow::pollSnapshot);
    pollTimer->start(32); // ~31Hz, close to 36Hz telemetry
    ackTimer = new QTimer(this);
    connect(ackTimer, &QTimer::timeout, this, &MainWindow::pollMemAck);
    ackTimer->start(200);
    // auto-load &H→RVA map per module (fair_*.csv 1521/449/193/765 = 2928, separate tabs fix 65-vs-764)
    // All CSVs come from the self-contained telemetry_linux/csv/ folder.
    auto resolveCsv = [](const QString &name)->QString{
        QStringList cands = {
            csvDir() + "/" + name,
            QCoreApplication::applicationDirPath() + "/" + name,
            QDir::currentPath() + "/" + name,
        };
        for (auto &c: cands) if (QFile::exists(c)) return c;
        return cands.first();
    };
    const QList<QPair<QString,QTableWidget*>> autoLoads = {
        {resolveCsv("fair_chassis.csv"), chassisTable},
        {resolveCsv("fair_engine.csv"),  engineTable},
        {resolveCsv("fair_wheel.csv"),   wheelTable},
        {resolveCsv("fair_garage.csv"),  garageTable},
    };
    QString autoMsg; int totalAuto=0;
    for (auto &pr : autoLoads) {
        QString msg; if (loadFairCsvFile(pr.first, pr.second, &msg)) totalAuto += pr.second->rowCount();
        else qWarning("autoLoad missing %s", qUtf8Printable(pr.first));
    }
    if (totalAuto) memStatus->setText(QString("Auto-loaded %1 entries from fair_*.csv (C:%2 E:%3 W:%4 G:%5) — those &H are static-EXE offsets")
        .arg(totalAuto).arg(chassisTable->rowCount()).arg(engineTable->rowCount()).arg(wheelTable->rowCount()).arg(garageTable->rowCount()));
    // The fair_*.csv addresses are raw file offsets into the static EXE, so they
    // cannot address live memory.  Replace chassis/engine/wheel with the real
    // per-car .data block tables keyed on live RVAs.  CSV values only here — a
    // live read is 87 round trips and would stall startup; press
    // "Read from memory" to fill them from the running game.
    reloadBlockTables(false);

    // Connect AFTER the fair_*.csv auto-load and the block-table rebuild:
    // QTableWidget::setItem() emits itemChanged, so connecting earlier would
    // record every one of the ~3000 static-EXE-offset rows as a "user edit"
    // and m_userEdits would be pure noise.
    connect(chassisTable, &QTableWidget::itemChanged, this, &MainWindow::onMemItemChanged);
    connect(wheelTable,   &QTableWidget::itemChanged, this, &MainWindow::onMemItemChanged);
    logQt(QString("itemChanged connected; m_userEdits=%1").arg(m_userEdits.size()));
}

MainWindow::~MainWindow() {
    stopTelemetryLog();
}

void MainWindow::startTelemetryLog() {
    if (teleLogFile) return;

    const auto &fields = telemetryLogFields();
    int selectedCount = 0;
    for (bool on : teleLogSelected) if (on) ++selectedCount;
    if (selectedCount == 0) {
        QMessageBox::warning(this, "Telemetry log",
            "Select at least one telemetry field before starting the log.");
        return;
    }

    const QString dir = telemetryLogDir();
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString path = QDir(dir).filePath(QString("telemetry_%1.csv").arg(stamp));
    for (int n = 2; QFile::exists(path); ++n)
        path = QDir(dir).filePath(QString("telemetry_%1_%2.csv").arg(stamp).arg(n));

    auto *file = new QFile(path, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, "Telemetry log",
            QString("Cannot create %1\n\n%2").arg(path, file->errorString()));
        delete file;
        return;
    }
    teleLogFile = file;
    teleLogStream = new QTextStream(teleLogFile);

    QStringList header;
    header << "wall_time";
    for (int i = 0; i < fields.size(); ++i)
        if (teleLogSelected.value(i, false)) header << fields[i].header;
    *teleLogStream << header.join(",") << "\n";
    teleLogStream->flush();

    teleLogRows = 0;
    teleLogHaveLast = false;
    // The sim writes at 36 Hz; poll faster than that while logging so every
    // tick reaches the CSV (the default 32 ms UI poll would drop ~1 in 8).
    if (pollTimer) pollTimer->setInterval(16);
    teleLogBtn->setText("Stop logging");
    teleLogFieldsBtn->setEnabled(false);
    teleLogStatus->setText(QString("Logging %1 fields → %2 — 0 rows")
                               .arg(selectedCount).arg(path));
    logQt(QString("telemetry log started: %1 (%2 fields)").arg(path).arg(selectedCount));
}

void MainWindow::stopTelemetryLog() {
    if (!teleLogFile) return;

    const QString path = teleLogFile->fileName();
    if (teleLogStream) {
        teleLogStream->flush();
        delete teleLogStream;
        teleLogStream = nullptr;
    }
    teleLogFile->close();
    delete teleLogFile;
    teleLogFile = nullptr;

    if (pollTimer) pollTimer->setInterval(32); // restore the UI poll rate
    if (teleLogBtn) teleLogBtn->setText("Start logging");
    if (teleLogFieldsBtn) teleLogFieldsBtn->setEnabled(true);
    if (teleLogStatus)
        teleLogStatus->setText(QString("Stopped — %1 rows → %2").arg(teleLogRows).arg(path));
    logQt(QString("telemetry log stopped: %1 (%2 rows)").arg(path).arg(teleLogRows));
}

void MainWindow::toggleTelemetryLog() {
    if (teleLogFile) stopTelemetryLog();
    else startTelemetryLog();
}

void MainWindow::selectTelemetryLogFields() {
    if (teleLogFile) {
        QMessageBox::information(this, "Telemetry log",
            "Stop logging before changing the selected fields.");
        return;
    }
    TelemetryFieldDialog dlg(telemetryLogFields(), teleLogSelected, this);
    if (dlg.exec() != QDialog::Accepted) return;
    teleLogSelected = dlg.selection();

    int selectedCount = 0;
    for (bool on : teleLogSelected) if (on) ++selectedCount;
    teleLogStatus->setText(QString("Logging off — %1 of %2 fields selected — folder: %3")
                               .arg(selectedCount).arg(telemetryLogFields().size())
                               .arg(telemetryLogDir()));
    logQt(QString("telemetry log fields: %1 of %2 selected")
              .arg(selectedCount).arg(telemetryLogFields().size()));
}

void MainWindow::openTelemetryLogDir() {
    const QString dir = telemetryLogDir();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir)))
        QMessageBox::information(this, "Telemetry log",
            QString("The logs are in:\n%1").arg(dir));
}

void MainWindow::appendTelemetryLog(const Nr2003Snapshot &s) {
    if (!teleLogFile || !teleLogStream) return;
    // pollSnapshot runs at ~31Hz, the sim writes at 36Hz (or 288Hz).  Log one
    // row per new (sequence, tick) so the UI poll rate cannot duplicate rows.
    if (teleLogHaveLast && s.sequence == teleLogLastSeq && s.tick == teleLogLastTick)
        return;
    teleLogLastSeq = s.sequence;
    teleLogLastTick = s.tick;
    teleLogHaveLast = true;

    const auto &fields = telemetryLogFields();
    QStringList row;
    row << QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    for (int i = 0; i < fields.size(); ++i)
        if (teleLogSelected.value(i, false))
            row << csvEscape(fields[i].value(s));
    *teleLogStream << row.join(",") << "\n";
    ++teleLogRows;

    if ((teleLogRows % 32) == 0) {
        teleLogStream->flush();
        int selectedCount = 0;
        for (bool on : teleLogSelected) if (on) ++selectedCount;
        teleLogStatus->setText(QString("Logging %1 fields → %2 — %3 rows")
                                   .arg(selectedCount).arg(teleLogFile->fileName()).arg(teleLogRows));
    }
}

void MainWindow::setupTelemetryTab() {
    QWidget *w = new QWidget;
    QVBoxLayout *lay = new QVBoxLayout(w);
    teleStatus = new QLabel("Waiting for /dev/shm/nr2003_telem (NR23 4096) — start run_nr2003_allinone.sh", w);
    lay->addWidget(teleStatus);

    const auto &allFields = telemetryLogFields();
    teleLogSelected.resize(allFields.size());
    int selectedCount = 0;
    for (int i = 0; i < allFields.size(); ++i) {
        teleLogSelected[i] = allFields[i].defaultOn;
        if (teleLogSelected[i]) ++selectedCount;
    }

    auto *logBar = new QHBoxLayout;
    teleLogBtn = new QPushButton("Start logging", w);
    teleLogBtn->setToolTip("Write one CSV row per new telemetry tick");
    teleLogFieldsBtn = new QPushButton("Select fields...", w);
    teleLogFieldsBtn->setToolTip("Choose which telemetry values are written to the CSV log");
    teleLogOpenBtn = new QPushButton("Open log folder", w);
    teleLogOpenBtn->setToolTip("Open the directory that contains the telemetry CSV logs");
    logBar->addWidget(teleLogBtn);
    logBar->addWidget(teleLogFieldsBtn);
    logBar->addWidget(teleLogOpenBtn);
    logBar->addStretch();
    lay->addLayout(logBar);

    teleLogStatus = new QLabel(QString("Logging off — %1 of %2 fields selected — folder: %3")
                                   .arg(selectedCount).arg(allFields.size()).arg(telemetryLogDir()), w);
    teleLogStatus->setWordWrap(true);
    lay->addWidget(teleLogStatus);
    connect(teleLogBtn, &QPushButton::clicked, this, &MainWindow::toggleTelemetryLog);
    connect(teleLogFieldsBtn, &QPushButton::clicked, this, &MainWindow::selectTelemetryLogFields);
    connect(teleLogOpenBtn, &QPushButton::clicked, this, &MainWindow::openTelemetryLogDir);

    teleTable = new QTableWidget(0, 3, w);
    teleTable->setHorizontalHeaderLabels({"Field","Value","Raw"});
    teleTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    teleTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    teleTable->verticalHeader()->setVisible(false);
    teleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(teleTable);
    tabs->addTab(w, "Telemetry");
}

// Column 0 of the block-keyed tables is written as uppercase hex with a "0X"
// prefix, e.g. "0X32355C".  QString::toUInt(.., 16) does NOT accept the
// "0X" prefix, so every RVA parse in this file has to strip it first - that is
// why "Apply to selected row" reported "Cannot parse RVA".  One helper, one
// rule, all call sites.
static bool parseRva(const QString &s, uint32_t *out) {
    QString t = s.trimmed();
    if (t.startsWith("0x", Qt::CaseInsensitive)) t = t.mid(2);
    bool ok = false;
    uint32_t v = t.toUInt(&ok, 16);
    if (ok && v) { *out = v; return true; }
    return false;
}

static QTableWidget* makeMemTable(QWidget *parent) {
    auto *t = new QTableWidget(0,5,parent);
    t->setHorizontalHeaderLabels({"RVA","VA (&H)","Label","Type","Value"});
    t->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    t->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    return t;
}
void MainWindow::setupMemTab() {
    QWidget *w = new QWidget;
    QVBoxLayout *lay = new QVBoxLayout(w);
    QHBoxLayout *btnLay = new QHBoxLayout;
    readBtn = new QPushButton("Read from memory", w);
    readBtn->setToolTip("one BLOCKR round trip per block (87) and refresh every Value cell from the running game");
    saveCsvBtn = new QPushButton("Save live values (Mem CSV)", w);
    saveCsvBtn->setToolTip("Write the current table contents (live values + your edits) back out as\n"
                           "csv/Live - <Mod> - Mem.csv, in the same transposed layout\n"
                           "as the originals.  Lets you back up, diff runs, or hand-edit offline.");
    probeBtn = new QPushButton("Probe: does this stick?", w);
    probeBtn->setToolTip("Pick a row, press once to write a sentinel into it, change car / restart the race,\n"
                          "then press again.  STUCK means the per-car block is authoritative and the game\n"
                          "keeps your value; REVERTED means the game repopulates the block from the MemLoc\n"
                          "tables or the .car file, so edit the MemLoc tab instead.");
    exportMemlocBtn = new QPushButton("Export MemLoc (CSV)", w);
    exportMemlocBtn->setToolTip("Export all 23 MemLoc tables (memloc-00..22, 40 floats each)\n"
                                "to CSV files in the selected directory. One file per table.");
    writeBlockBtn = new QPushButton("Write my edits (BLOCKM)", w);
    writeBlockBtn->setToolTip("tester.exe style: one ReadProcessMemory + one WriteProcessMemory per per-car block\n"
                               "values come from csv/Original - * - Mem.csv, overridden by anything you type in the\n"
                               "Chassis / Wheel tables; game-owned slots (0x3B..0x51 Mem Loc pointers, uint32-bit indices) are kept\n"
                               "48 writable (.data) blocks; the 16 engine blocks are .rdata -> pre-race only");
    memStatus = new QLabel("Idle — 4 tabs (C/E/W/G) keep separated, Write chunked 64/batch MEM3 seq/ack via /dev/shm/nr2003_mem", w);
    btnLay->addWidget(readBtn);
    btnLay->addWidget(writeBlockBtn);
    btnLay->addWidget(saveCsvBtn);
    btnLay->addWidget(probeBtn);
    btnLay->addWidget(exportMemlocBtn);
    btnLay->addStretch();
    lay->addLayout(btnLay);
    lay->addWidget(memStatus);
    memTabs = new QTabWidget(w);
    chassisTable = makeMemTable(w); engineTable = makeMemTable(w);
    wheelTable = makeMemTable(w);   garageTable = makeMemTable(w);
    memlocTable = makeMemTable(w);
    memTabs->addTab(chassisTable, "Chassis"); memTabs->addTab(engineTable, "Engine");
    memTabs->addTab(wheelTable, "Wheel");      memTabs->addTab(garageTable, "Garage (EXE offsets)");
    memTabs->addTab(memlocTable, "MemLoc");
    lay->addWidget(memTabs);
    // compat: default memTable points to chassis, updated on tab change
    memTable = chassisTable;
    QObject::connect(memTabs, &QTabWidget::currentChanged, this, [this](int i){
        auto tbls = allMemTables(); if (i>=0 && i<tbls.size()) memTable = tbls[i];
    });
    // --- value editor row -------------------------------------------------
    // QTableWidget's inline editor never committed into the item model under
    // this setup, so typing in a cell could never be observed.  This row is a
    // plain QDoubleSpinBox: pick a row (single click selects it), type the
    // number, press Apply.  The write is programmatic (setText on the item),
    // so no delegate is involved and it cannot be lost.
    QHBoxLayout *valLay = new QHBoxLayout;
    valInfo = new QLabel("no row selected", w);
    valEdit = new QDoubleSpinBox(w);
    valEdit->setRange(-1e9, 1e9); valEdit->setDecimals(6);
    valEdit->setSingleStep(1.0); valEdit->setKeyboardTracking(false);
    QPushButton *applyBtn = new QPushButton("Apply to selected row", w);
    applyBtn->setToolTip("Writes the spin box value into the selected row's Value cell.\n"
                         "Required because the table's inline editor does not commit under Qt6 here.");
    valLay->addWidget(valInfo, 1);
    valLay->addWidget(new QLabel("new value:", w));
    valLay->addWidget(valEdit);
    valLay->addWidget(applyBtn);
    lay->addLayout(valLay);
    connect(applyBtn, &QPushButton::clicked, this, &MainWindow::applyValueToSelection);
    for (QTableWidget *t : {chassisTable, wheelTable, engineTable, garageTable, memlocTable}) {
        connect(t, &QTableWidget::itemSelectionChanged, this, &MainWindow::syncEditorFromSelection);
        connect(t, &QTableWidget::currentCellChanged, this, [this](int,int,int,int){ syncEditorFromSelection(); });
    }

    tabs->addTab(w, "Live Mem");
    setupExeTab();
    connect(readBtn, &QPushButton::clicked, this, &MainWindow::refreshLiveValues);
    connect(writeBlockBtn, &QPushButton::clicked, this, &MainWindow::writeBlocks);
    connect(saveCsvBtn, &QPushButton::clicked, this, &MainWindow::saveLiveMemCsv);
    connect(probeBtn, &QPushButton::clicked, this, &MainWindow::stickinessProbe);
    connect(exportMemlocBtn, &QPushButton::clicked, this, &MainWindow::exportMemlocCsv);
}

// gap 4: engine (448) + garage (192) rows are .rdata by design (16 of 87 blocks) —
// mark those tables non-editable and point the user at the offline EXE patcher.
void MainWindow::markReadOnlyTables() {
    struct RO { QTableWidget *tbl; const char *why; const char *fix; };
    const RO list[] = {
        { engineTable, "engine tables are .rdata (PAGE_EXECUTE_READ) — 16 static series blocks at RVA 0x2EECA8..0x2EF338",
                     "edit offline in the EXE File tab (Load fair_*.csv, then Write values to EXE)" },
        { garageTable, "garage values live in .rdata / setup code, not writable memory",
                     "edit offline in the EXE File tab (Load fair_*.csv, then Write values to EXE)" },
    };
    for (const auto &ro : list) {
        for (int r=0;r<ro.tbl->rowCount();++r) {
            QTableWidgetItem *v = ro.tbl->item(r,4);
            if (!v) continue;
            v->setFlags(v->flags() & ~Qt::ItemIsEditable);
            v->setToolTip(QString("read-only: %1\n%2").arg(ro.why, ro.fix));
            v->setBackground(QColor(0x2b,0x2b,0x2b));
        }
    }
    logQt(QString("markReadOnlyTables: engine %1 + garage %2 rows locked (.rdata)").arg(engineTable->rowCount()).arg(garageTable->rowCount()));
}

bool MainWindow::readSnapshotRaw(uint8_t *buf) {
    QFile f(NR2003_SNAPSHOT_PATH_LINUX);
    if (!f.open(QIODevice::ReadOnly)) return false;
    if (f.size() < (qint64)NR2003_SNAPSHOT_SIZE) return false;
    qint64 r = f.read((char*)buf, NR2003_SNAPSHOT_SIZE);
    return r == (qint64)NR2003_SNAPSHOT_SIZE;
}

void MainWindow::pollSnapshot() {
    uint8_t buf[NR2003_SNAPSHOT_SIZE];
    if (!readSnapshotRaw(buf)) {
        teleStatus->setText("No SHM — run ./run_nr2003_allinone.sh /path/to/NR2003.exe  (expects Z:\\dev\\shm → /dev/shm)");
        return;
    }
    auto *s = reinterpret_cast<Nr2003Snapshot*>(buf);
    if (s->magic != NR2003_SNAPSHOT_MAGIC || s->tailMagic != NR2003_SNAPSHOT_TAIL_MAGIC || s->size != NR2003_SNAPSHOT_SIZE) {
        teleStatus->setText(QString("Bad header magic=%1 tail=%2 size=%3").arg(s->magic,8,16).arg(s->tailMagic,8,16).arg(s->size));
        return;
    }
    if (s->sequence != s->sequence2) return; // torn
    if (s->sequence == lastSeq && s->tick == 0) { /* still waiting */ }
    lastSeq = s->sequence;
    appendTelemetryLog(*s);
    double speed = sqrt(s->chassis.v[0]*s->chassis.v[0] + s->chassis.v[1]*s->chassis.v[1] + s->chassis.v[2]*s->chassis.v[2]);
    // m/s to mph
    double mph = speed * 2.23694;
    teleStatus->setText(QString("seq %1 tick %2 step %3 time %4s trk %5% mph %6 rpm %7 gear %8 opp %9")
        .arg(s->sequence).arg(s->tick).arg(s->step).arg(s->time,0,'f',3).arg(s->chassis.trkPct,0,'f',4).arg(mph,0,'f',1).arg(s->driveline.engRPM,0,'f',0).arg(s->input.gear).arg(s->opponentCount));
    struct Row { const char *k; QString v; QString raw; };
    QVector<Row> rows = {
        {"seq/tick/step", QString("%1 / %2 / %3").arg(s->sequence).arg(s->tick).arg(s->step), ""},
        {"time", QString::number(s->time,'f',3), ""},
        {"trkPct", QString::number(s->chassis.trkPct,'f',5), ""},
        {"pos x,y,z", QString("%1, %2, %3").arg(s->chassis.r[0],0,'f',2).arg(s->chassis.r[1],0,'f',2).arg(s->chassis.r[2],0,'f',2), ""},
        {"vel Vx,Vy,Vz", QString("%1, %2, %3").arg(s->chassis.v[0],0,'f',2).arg(s->chassis.v[1],0,'f',2).arg(s->chassis.v[2],0,'f',2), ""},
        {"yaw/pitch/roll", QString("%1 / %2 / %3").arg(s->chassis.q[0],0,'f',3).arg(s->chassis.q[1],0,'f',3).arg(s->chassis.q[2],0,'f',3), ""},
        {"speed mph", QString::number(mph,'f',1), QString::number(speed,'f',2)+" m/s"},
        {"steerTorque", QString::number(s->chassis.steerT,'f',2), "N*m"},
        {"throttle/brake/clutch", QString("%1 / %2 / %3").arg(s->input.throttle,0,'f',3).arg(s->input.brake,0,'f',3).arg(s->input.clutch,0,'f',3), ""},
        {"steer/gear", QString("%1 rad / %2").arg(s->input.steer,0,'f',4).arg(s->input.gear), ""},
        {"RPM/power/torque", QString("%1 / %2 / %3").arg(s->driveline.engRPM,0,'f',0).arg(s->driveline.engPower,0,'f',0).arg(s->driveline.engT,0,'f',1), ""},
        {"wheel LF Fz/V", QString("%1 N / %2 rad/s").arg(s->wheel.w[0].Fz,0,'f',0).arg(s->wheel.w[0].v,0,'f',1), ""},
        {"tire LF I/M/O C", QString("%1 / %2 / %3").arg(s->tire.w[0].temp[0],0,'f',0).arg(s->tire.w[0].temp[1],0,'f',0).arg(s->tire.w[0].temp[2],0,'f',0), ""},
        {"gauge rpm/water", QString("%1 / %2 C").arg(s->gauge.rpm,0,'f',0).arg(s->gauge.waterTemp,0,'f',0), ""},
        {"opponentCount", QString::number(s->opponentCount), ""},
    };
    teleTable->setRowCount(rows.size());
    for (int i=0;i<rows.size();++i) {
        teleTable->setItem(i,0,new QTableWidgetItem(rows[i].k));
        teleTable->setItem(i,1,new QTableWidgetItem(rows[i].v));
        teleTable->setItem(i,2,new QTableWidgetItem(rows[i].raw));
    }
}



// one MEM3 round trip: pwrite 131072 (no truncate — truncating kills the Wine MapView),
// then poll the ack written back by tsample pollMemCommands()
bool MainWindow::sendMemCmd(Nr2003MemCmd &cmd, int *statusOut, int pollMs) {
    int fd = ::open(NR2003_MEM_PATH_LINUX, O_RDWR);
    if (fd < 0) { logQt(QString("sendMemCmd open fail %1").arg(strerror(errno))); return false; }
    ssize_t wn = pwrite(fd, &cmd, NR2003_MEM_SIZE, 0);
    int se = errno; fsync(fd); ::close(fd);
    if (wn != (ssize_t)NR2003_MEM_SIZE) { logQt(QString("sendMemCmd short write %1/%2 %3").arg(wn).arg(NR2003_MEM_SIZE).arg(strerror(se))); return false; }
    uint32_t seq2 = cmd.seq;
    static QVector<uint8_t> buf;
    buf.resize(NR2003_MEM_SIZE);
    for (int k=0; k*20 < pollMs; ++k) {
        QThread::msleep(20);
        int rfd = ::open(NR2003_MEM_PATH_LINUX, O_RDONLY);
        if (rfd < 0) continue;
        ssize_t rn = pread(rfd, buf.data(), NR2003_MEM_SIZE, 0);
        ::close(rfd);
        if (rn != (ssize_t)NR2003_MEM_SIZE) continue;
        auto *c = const_cast<Nr2003MemCmd*>(reinterpret_cast<const Nr2003MemCmd*>(buf.constData()));
        if (c->ackSeq == seq2) { if (statusOut) *statusOut = c->status; return true; }
    }
    if (statusOut) *statusOut = -1;
    return false;
}

// gap 3: one command per per-car block instead of 2924 per-value writes.

// The authoritative value source.  csv/Original - <mod> - Mem.csv
// is transposed: one row per Field (slot label), one column per car variant.
// Verified live against NR2003 (82.1% of 3892 slots exact; every remaining diff
// is a documented benign representation difference):
//   block k  <->> CSV column k, in FILE order (not sorted)
//   slot i   <->> CSV row  Field(i + desc.fieldOffset)   offset 1 chassis/wheel, 3 engine
//   CSV value lives at raw index csvCol+2  (0 = Field00, 1 = human label)
// and with fieldrows = rows[1:], Field(n) is at fieldrows[n-1], so the row index
// is (fieldOffset + slot) - 1.
// The fair_*.csv &H addresses are RAW FILE OFFSETS of the static EXE and can
// never match the live .data blocks, so they are not used here.
QHash<uint32_t,float> MainWindow::loadMemCsvValues(bool *ok, QString *msg) const {
    QHash<uint32_t,float> out;
    const QPair<const char*,uint8_t> files[] = {
        {"Original - Chassis - Mem.csv", NR2003_BLOCK_MOD_CHASSIS},
        {"Original - Engine - Mem.csv",  NR2003_BLOCK_MOD_ENGINE},
        {"Original - Wheel - Mem.csv",   NR2003_BLOCK_MOD_WHEEL},
    };
    QString appDir = QCoreApplication::applicationDirPath();
    const QString dataDir = csvDir();
    int nFiles = 0;
    auto cell = [](const QStringList &r, int i)->QString{
        return (i >= 0 && i < r.size()) ? r.at(i).trimmed() : QString();
    };
    for (auto &f : files) {
        QString name = QString::fromLatin1(f.first);
        QStringList cands = {
            dataDir + "/" + name,
            appDir + "/" + name,
            QDir::currentPath() + "/csv/" + name,
        };
        QString path;
        for (auto &c : cands) if (QFile::exists(c)) { path = c; break; }
        if (path.isEmpty()) continue;
        QFile fh(path);
        if (!fh.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        nFiles++;
        QList<QStringList> rows;
        {
            QTextStream ts(&fh);
            while (!ts.atEnd()) {
                QString line = ts.readLine();
                if (line.trimmed().isEmpty()) continue;
                rows.append(line.split(','));
            }
        }
        if (rows.isEmpty()) continue;
        // fieldrows[0] is the Field01 row.
        QList<QStringList> fieldrows = rows.mid(1);
        // one block per series column, in file order
        for (uint32_t bi=0; bi<NR2003_BLOCK_COUNT; ++bi) {
            const auto &b = NR2003_BLOCKS[bi];
            if (b.module != f.second) continue;
            int col = 2 + b.csvCol;
            if (b.fieldOffset == 0) continue;  // memloc: no Mem CSV
            for (uint16_t s=0; s<b.floatCount; ++s) {
                int ri = (b.fieldOffset + s) - 1;
                if (ri < 0 || ri >= fieldrows.size()) continue;
                QString v = cell(fieldrows.at(ri), col);
                if (v.isEmpty()) continue;
                bool okc=false;
                float fv = v.toFloat(&okc);
                if (!okc) continue;             // non-numeric cell ("cup", "Series", …)
                uint32_t rva = b.baseRva + 4u*s;
                // uint32-bit slots: 0-based int indices (memory = CSV - 1), the 23
                // Mem Loc pointers, and the NaN "not set" sentinel.  Never emit them.
                if (nr2003_block_is_ptr_slot(s) && b.floatCount >= 0x72) continue;
                static const int kIntSlots[] = {0,1,19,20,21,22,39,110};
                bool isIntSlot = false;
                for (int k : kIntSlots) if (b.floatCount >= 0x72 && k == s) isIntSlot = true;
                if (isIntSlot) continue;
                out.insert(rva, fv);
            }
        }
    }
    if (ok) *ok = (nFiles > 0);
    if (msg) *msg = QString("%1 file(s), %2 slot values").arg(nFiles).arg(out.size());
    return out;
}

void MainWindow::syncEditorFromSelection() {
    QTableWidget *tbl = memTable;
    if (!tbl) return;
    int r = tbl->currentRow();
    if (r < 0) { if (valInfo) valInfo->setText("no row selected"); return; }
    auto *rI = tbl->item(r,0), *lI = tbl->item(r,2), *vI = tbl->item(r,4), *tI = tbl->item(r,3);
    if (!rI || !vI) return;
    if (valInfo) valInfo->setText(QString("row %1  RVA %2  [%3]  %4  (%5)")
        .arg(r).arg(rI->text()).arg(tI?tI->text():QString())
        .arg(lI?lI->text():QString()).arg(vI->text()));
    bool ok=false; double cur = vI->text().trimmed().toDouble(&ok);
    if (ok) { valEdit->blockSignals(true); valEdit->setValue(cur); valEdit->blockSignals(false); }
}

void MainWindow::applyValueToSelection() {
    QTableWidget *tbl = memTable;
    if (!tbl) return;
    int r = tbl->currentRow();
    if (r < 0) { QMessageBox::information(this,"Apply","Select a row first (single click a row)."); return; }
    auto *rI = tbl->item(r,0), *vI = tbl->item(r,4);
    if (!rI || !vI) return;
    if (!(vI->flags() & Qt::ItemIsEditable)) {
        QMessageBox::warning(this,"Apply","This cell is game-owned (uint32 bits) and must not be written."); return;
    }
    uint32_t rva = 0;
    if (!parseRva(rI->text(), &rva)) {
        QMessageBox::warning(this,"Apply",
            QString("Cannot parse RVA from \"%1\"  (row %2, column 0)").arg(rI->text()).arg(r));
        logQt(QString("apply FAILED to parse rva \"%1\" at row %2").arg(rI->text()).arg(r));
        return;
    }
    float v = (float)valEdit->value();
    m_suppressEdits = true;
    vI->setText(QString::number(v,'g',9));
    m_suppressEdits = false;
    m_baseline.remove(rva);            // so the diff always reports it as edited
    m_userEdits[rva] = v;
    logQt(QString("apply rva 0x%1 = %2  (row %3, %4)").arg(rva,8,16).arg(v).arg(r)
          .arg(tbl->item(r,2)?tbl->item(r,2)->text():QString()));
    syncEditorFromSelection();
    QMessageBox::information(this,"Apply",
        QString("Staged rva 0x%1 = %2.\n\nNow press \"Write my edits (BLOCKM)\".").arg(rva,8,16).arg(v));
}

// ---------------------------------------------------------------------------
// MemLoc tables.  Each chassis-shaped block carries 23 pointers (slots 0x3B-0x51)
// into these 40-float tables; the game dereferences them for the per-car
// variant data, so they are the layer the block copy is built from.  There is
// no *-Mem.csv for them (fieldOffset == 0), so the only value source is a live
// BLOCKR read - readAllBlockValues() already returns rva -> value for them.
// ---------------------------------------------------------------------------
int MainWindow::loadMemlocTable(const QHash<uint32_t,float> &live, QString *msg) {
    m_suppressEdits = true;
    memlocTable->setRowCount(0);
    int rows = 0, fromLive = 0;
    for (uint32_t bi = 0; bi < NR2003_BLOCK_COUNT; ++bi) {
        const auto &b = NR2003_BLOCKS[bi];
        if (b.module != NR2003_BLOCK_MOD_MEMLOC) continue;
        for (uint16_t s = 0; s < b.floatCount; ++s) {
            const uint32_t rva = b.baseRva + 4u*s;
            const bool have = live.contains(rva);
            const float val = have ? live.value(rva) : 0.0f;
            if (have) fromLive++;
            int r = memlocTable->rowCount();
            memlocTable->insertRow(r);
            memlocTable->setItem(r,0,new QTableWidgetItem(QString("0x%1").arg(rva,8,16).toUpper()));
            memlocTable->setItem(r,1,new QTableWidgetItem(QString("&H%1").arg(0x400000u+rva,8,16).toUpper()));
            memlocTable->setItem(r,2,new QTableWidgetItem(QString("%1 [%2]").arg(QString::fromLatin1(b.name)).arg(s)));
            memlocTable->setItem(r,3,new QTableWidgetItem("Sing"));
            QTableWidgetItem *vi = new QTableWidgetItem(QString::number(val,'g',9));
            vi->setToolTip(have ? "live value read from NR2003 via BLOCKR"
                                : "no live value (game not running?) - reads as 0 until you press Read from memory");
            memlocTable->setItem(r,4,vi);
            m_baseline[rva] = val;
            rows++;
        }
    }
    memlocTable->resizeColumnToContents(0); memlocTable->resizeColumnToContents(1);
    m_suppressEdits = false;
    if (msg) *msg = QString("%1 rows (%2 from live)").arg(rows).arg(fromLive);
    return rows;
}

// ---------------------------------------------------------------------------
// EXE File tab - offline editing of Nr2003.eXe from csv/fair_*.csv.
// Those &H values are raw static-EXE file offsets, NOT live addresses, so this
// is the only place they make sense.  Writing goes through the PE section
// table to turn an RVA into a file offset (delta 0 for .text/.rdata, non-zero
// for .data because it is truncated on disk).
// ---------------------------------------------------------------------------
void MainWindow::setupExeTab() {
    QWidget *w = new QWidget;
    QVBoxLayout *lay = new QVBoxLayout(w);
    QHBoxLayout *row = new QHBoxLayout;
    // No hardcoded game path: pass NR2003_EXE in the environment or use Browse.
    exePathEdit = new QLineEdit(qEnvironmentVariable("NR2003_EXE"), w);
    QPushButton *browse = new QPushButton("Browse...", w);
    QPushButton *ld = new QPushButton("Load fair_*.csv", w);
    ld->setToolTip("Load the offline reference tables.  Their &H addresses are static-EXE\n"
                   "locations: file offset = &H-1.  This is why they live on this tab, not in Live Mem.");
    QPushButton *rd = new QPushButton("Read from EXE", w);
    rd->setToolTip("Read every listed value out of the EXE file (no game needed).\n"
                   "Sing=float32, Doub=float64 and Long=int32 are read according to the Type column.");
    QPushButton *wr = new QPushButton("Write values to EXE", w);
    wr->setToolTip("Patches the EXE file at file offset &H-1, writing each value with its\n"
                   "CSV Type (Sing=float32, Doub=float64, Long=int32).  A .bak copy is\n"
                   "created before the first write.");
    row->addWidget(new QLabel("EXE:", w));
    row->addWidget(exePathEdit, 1);
    row->addWidget(browse); row->addWidget(ld); row->addWidget(rd); row->addWidget(wr);
    lay->addLayout(row);
    exeStatus = new QLabel("No EXE loaded.", w);
    lay->addWidget(exeStatus);
    exChassisTable = makeMemTable(w); exEngineTable = makeMemTable(w);
    exWheelTable  = makeMemTable(w); exGarageTable  = makeMemTable(w);
    // The EXE tab is static-file editing, so it must not show a live RVA.
    // Column 0 is the file offset derived from &H; column 1 keeps the original
    // &H for reference.  Live Mem keeps its RVA/VA columns.
    for (QTableWidget *t : {exChassisTable, exEngineTable, exWheelTable, exGarageTable})
        t->setHorizontalHeaderLabels({"File Offset","&H Address","Label","Type","Value"});
    exeTabs = new QTabWidget(w);
    exeTabs->addTab(exChassisTable,"Chassis"); exeTabs->addTab(exEngineTable,"Engine");
    exeTabs->addTab(exWheelTable,"Wheel");    exeTabs->addTab(exGarageTable,"Garage");
    lay->addWidget(exeTabs);
    tabs->addTab(w, "EXE File");

    connect(browse, &QPushButton::clicked, this, [this]{
        QString f = QFileDialog::getOpenFileName(this, "NR2003 executable", exePathEdit->text());
        if (!f.isEmpty()) exePathEdit->setText(f);
    });
    connect(ld, &QPushButton::clicked, this, &MainWindow::loadFairCsvExe);
    connect(rd, &QPushButton::clicked, this, &MainWindow::readExeValues);
    connect(wr, &QPushButton::clicked, this, &MainWindow::writeExeValues);
}

// The EXE-file tables use static file offsets directly (column 0 = &H-1).
// No PE RVA translation is involved: &H already says where the value lives
// inside the file.  Only type-aware packing/unpacking is needed.
static int exeTypeSize(const QString &type) {
    if (csvTypeIsDouble(type)) return 8;
    return 4; // Sing float32, Long int32
}

static bool exeValueToBytes(const QString &type, const QString &text, QByteArray *out) {
    const QString t = text.trimmed();
    if (csvTypeIsDouble(type)) {
        bool ok = false; double v = t.toDouble(&ok); if (!ok) return false;
        out->resize(8); memcpy(out->data(), &v, 8); return true;
    }
    if (csvTypeIsLong(type)) {
        bool ok = false; qint64 v = t.toLongLong(&ok);
        if (!ok) { double d = t.toDouble(&ok); if (ok) v = qint64(d); }
        if (!ok) return false;
        qint32 iv = qint32(v);
        out->resize(4); memcpy(out->data(), &iv, 4); return true;
    }
    bool ok = false; float v = t.toFloat(&ok); if (!ok) return false;
    out->resize(4); memcpy(out->data(), &v, 4); return true;
}

static bool exeBytesToText(const QString &type, const QByteArray &b, QString *out) {
    if (csvTypeIsDouble(type)) {
        if (b.size() < 8) return false;
        double v; memcpy(&v, b.constData(), 8);
        *out = QString::number(v, 'g', 17); return true;
    }
    if (csvTypeIsLong(type)) {
        if (b.size() < 4) return false;
        qint32 v; memcpy(&v, b.constData(), 4);
        *out = QString::number(v); return true;
    }
    if (b.size() < 4) return false;
    float v; memcpy(&v, b.constData(), 4);
    *out = QString::number(v, 'g', 9); return true;
}

void MainWindow::loadFairCsvExe() {
    struct A { const char *f; QTableWidget *t; };
    const A a[] = {{"fair_chassis.csv", exChassisTable}, {"fair_engine.csv", exEngineTable},
                   {"fair_wheel.csv", exWheelTable},   {"fair_garage.csv", exGarageTable}};
    QString base = csvDir() + "/";
    if (!QDir(base).exists()) base = QCoreApplication::applicationDirPath() + "/";
    int total = 0;
    for (auto &x : a) {
        QString p = base + x.f;
        if (!QFile::exists(p)) p = QCoreApplication::applicationDirPath() + "/" + x.f;
        QString m; if (loadFairCsvFile(p, x.t, &m, /*staticFileOffsets=*/true)) total += x.t->rowCount();
        else exeStatus->setText("missing " + p);
    }
    exeStatus->setText(QString("Loaded %1 values from fair_*.csv (file offset = &H-1)").arg(total));
    logQt(QString("loadFairCsvExe: %1 values").arg(total));
}

void MainWindow::readExeValues() {
    if (!QFile::exists(exePathEdit->text())) { exeStatus->setText("EXE not found"); return; }
    int n = 0, bad = 0;
    QFile f(exePathEdit->text());
    if (!f.open(QIODevice::ReadOnly)) { exeStatus->setText("cannot open EXE for reading"); return; }
    const qint64 fileSize = f.size();
    for (QTableWidget *t : exTables()) {
        for (int r = 0; r < t->rowCount(); ++r) {
            auto *oI = t->item(r,0), *tI = t->item(r,3);
            if (!oI || !tI) continue;
            uint32_t off32 = 0; if (!parseRva(oI->text(), &off32)) continue;
            const qint64 off = off32;
            const QString type = tI->text();
            const int size = exeTypeSize(type);
            if (off + size > fileSize) { ++bad; continue; }
            f.seek(off); QByteArray b = f.read(size);
            QString text;
            if (b.size() != size || !exeBytesToText(type, b, &text)) { ++bad; continue; }
            auto *vI = t->item(r,4);
            if (vI) { m_suppressEdits = true; vI->setText(text); m_suppressEdits = false; }
            ++n;
        }
    }
    exeStatus->setText(QString("Read %1 values from %2 (%3 out of range)")
        .arg(n).arg(QFileInfo(exePathEdit->text()).fileName()).arg(bad));
    logQt(QString("readExeValues: %1 read, %2 bad").arg(n).arg(bad));
}

void MainWindow::writeExeValues() {
    const QString path = exePathEdit->text();
    if (!QFile::exists(path)) { exeStatus->setText("EXE not found"); return; }

    // Never patch without a fallback copy.  Keep the first backup; later runs
    // do not overwrite it, so the original remains recoverable.
    const QString bak = path + ".bak";
    bool madeBackup = false;
    if (!QFileInfo::exists(bak)) madeBackup = QFile::copy(path, bak);

    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) { exeStatus->setText("cannot open EXE for writing"); return; }
    const qint64 fileSize = f.size();
    int n = 0, bad = 0, verifyBad = 0; QStringList fails;
    QVector<QPair<qint64, QByteArray>> writes;

    for (QTableWidget *t : exTables()) {
        for (int r = 0; r < t->rowCount(); ++r) {
            auto *oI = t->item(r,0), *tI = t->item(r,3), *vI = t->item(r,4);
            if (!oI || !tI || !vI) continue;
            uint32_t off32 = 0; if (!parseRva(oI->text(), &off32)) continue;
            const qint64 off = off32;
            const QString type = tI->text();
            QByteArray bytes;
            if (!exeValueToBytes(type, vI->text(), &bytes)) {
                ++bad;
                if (fails.size()<6) fails.append(QString("%1@0x%2 (%3)").arg(vI->text()).arg(off32,8,16).arg(type));
                continue;
            }
            if (off + bytes.size() > fileSize) {
                ++bad;
                if (fails.size()<6) fails.append(QString("%1@0x%2 out of range").arg(vI->text()).arg(off32,8,16));
                continue;
            }
            f.seek(off);
            if (f.write(bytes) != bytes.size()) { ++bad; continue; }
            writes.append(qMakePair(off, bytes));
            ++n;
        }
    }
    f.flush();

    // Read the bytes back and compare exactly; report any mismatch instead of
    // silently claiming success.
    for (const auto &w : writes) {
        f.seek(w.first);
        if (f.read(w.second.size()) != w.second) ++verifyBad;
    }
    f.close();

    exeStatus->setText(QString("Wrote %1 values into %2 (%3 rejected, %4 read-back mismatches)%5")
        .arg(n).arg(QFileInfo(path).fileName()).arg(bad).arg(verifyBad)
        .arg(madeBackup ? QString(" — backup: %1").arg(QFileInfo(bak).fileName()) : QString())
        + (fails.isEmpty() ? QString() : QString(" | ") + fails.join(", ")));
    logQt(QString("writeExeValues: %1 written, %2 rejected, %3 mismatches, backup=%4")
        .arg(n).arg(bad).arg(verifyBad).arg(madeBackup ? bak : QString("existing/none")));
}

// Save the current table contents back out as csv/Live - <Mod>
// - Mem.csv, using the same transposed layout as the originals (row 0 is the
// Field00 header, one column per car variant in block order).
void MainWindow::saveLiveMemCsv() {
    struct Spec { uint8_t mod; const char *name; QTableWidget *tbl; };
    const Spec sp[] = {
        { NR2003_BLOCK_MOD_CHASSIS, "Chassis", chassisTable },
        { NR2003_BLOCK_MOD_ENGINE,  "Engine",  engineTable  },
        { NR2003_BLOCK_MOD_WHEEL,   "Wheel",   wheelTable   },
    };
    QString dir = QFileDialog::getExistingDirectory(this,
        "Save Mem CSVs to folder", csvDir());
    if (dir.isEmpty()) { logQt("saveLiveMemCsv: cancelled"); return; }

    // map rva -> displayed value for each module
    for (auto &x : sp) {
        QHash<uint32_t,float> vals;
        for (int r = 0; r < x.tbl->rowCount(); ++r) {
            auto *rI = x.tbl->item(r,0), *vI = x.tbl->item(r,4);
            if (!rI || !vI) continue;
            uint32_t rva = 0; if (!parseRva(rI->text(), &rva)) continue;
            bool ok=false; float v = vI->text().trimmed().toFloat(&ok); if (!ok) continue;
            vals[rva] = v;
        }
        // collect this module's blocks in table order
        QList<const Nr2003BlockDesc*> bl;
        int maxSlots = 0;
        for (uint32_t bi=0; bi<NR2003_BLOCK_COUNT; ++bi)
            if (NR2003_BLOCKS[bi].module == x.mod) { bl.append(&NR2003_BLOCKS[bi]); maxSlots = qMax<int>(maxSlots, NR2003_BLOCKS[bi].floatCount); }
        if (bl.isEmpty()) continue;
        QString out = QString("Field00,%1").arg(x.name);
        for (auto b : bl) out += "," + QString::fromLatin1(b->name);
        out += "\n";
        for (int s = 0; s < maxSlots; ++s) {
            QString row = QString("Field%1,").arg(s+1, 2, 10, QChar('0'));
            QString lbl = QString::fromLatin1(nr2003_block_field_name(x.mod, s));
            if (lbl.isEmpty()) lbl = QString("[%1]").arg(s);
            row += lbl;
            for (auto b : bl) {
                float v = 0.0f;
                if (s < b->floatCount) {
                    const uint32_t rva = b->baseRva + 4u*s;
                    if (vals.contains(rva)) v = vals.value(rva);
                }
                row += "," + QString::number(v, 'g', 9);
            }
            out += row + "\n";
        }
        const QString path = dir + QString("/Live - %1 - Mem.csv").arg(x.name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { exeStatus->setText("cannot write " + path); continue; }
        f.write(out.toUtf8()); f.close();
        logQt(QString("saveLiveMemCsv: wrote %1 (%2 blocks x %3 slots)")
            .arg(path).arg(bl.size()).arg(maxSlots));
        exeStatus->setText("Saved " + path);
    }
}

void MainWindow::exportMemlocCsv() {
    QString dir = QFileDialog::getExistingDirectory(this,
        "Export MemLoc tables to folder", csvDir());
    if (dir.isEmpty()) { logQt("exportMemlocCsv: cancelled"); return; }

    // Collect values from memlocTable
    QHash<uint32_t, float> vals;
    for (int r = 0; r < memlocTable->rowCount(); ++r) {
        auto *rI = memlocTable->item(r, 0), *vI = memlocTable->item(r, 4);
        if (!rI || !vI) continue;
        uint32_t rva = 0; if (!parseRva(rI->text(), &rva)) continue;
        bool ok = false; float v = vI->text().trimmed().toFloat(&ok); if (!ok) continue;
        vals[rva] = v;
    }

    if (vals.isEmpty()) {
        QMessageBox::warning(this, "Export MemLoc", "MemLoc table is empty. Run 'Read from memory' first.");
        return;
    }

    // Get the 23 MemLoc block descriptors in order
    QList<const Nr2003BlockDesc*> memlocBlocks;
    for (uint32_t bi = 0; bi < NR2003_BLOCK_COUNT; ++bi) {
        if (NR2003_BLOCKS[bi].module == NR2003_BLOCK_MOD_MEMLOC) {
            memlocBlocks.append(&NR2003_BLOCKS[bi]);
        }
    }

    if (memlocBlocks.isEmpty()) {
        QMessageBox::warning(this, "Export MemLoc", "No MemLoc blocks found in nr2003_blocks.h");
        return;
    }

    int exported = 0;
    for (const Nr2003BlockDesc* b : memlocBlocks) {
        // Build CSV for this block
        QString out = "Slot,RVA,Value\n";
        for (uint32_t s = 0; s < b->floatCount; ++s) {
            uint32_t rva = b->baseRva + 4u * s;
            float v = vals.value(rva, 0.0f);
            out += QString("%1,0x%2,%3\n")
                .arg(s).arg(rva, 8, 16, QChar('0')).arg(v, 0, 'g', 9);
        }
        QString path = dir + QString("/MemLoc_%1.csv").arg(b->name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logQt(QString("exportMemlocCsv: cannot write %1").arg(path));
            continue;
        }
        f.write(out.toUtf8());
        f.close();
        logQt(QString("exportMemlocCsv: wrote %1 (%2 floats)").arg(path).arg(b->floatCount));
        exported++;
    }

    exeStatus->setText(QString("Exported %1 MemLoc tables to %2").arg(exported).arg(dir));
    logQt(QString("exportMemlocCsv: exported %1 tables to %2").arg(exported).arg(dir));
}

void MainWindow::onMemItemChanged(QTableWidgetItem *it) {
    if (m_suppressEdits || !it || it->column() != 4) return;
    if (!(it->flags() & Qt::ItemIsEditable)) return;
    QTableWidget *tbl = nullptr;
    if (it->row() >= 0)
        for (QTableWidget *t : {chassisTable, wheelTable})
            if (t->item(it->row(), 4) == it) { tbl = t; break; }
    if (!tbl) return;
    auto *rI = tbl->item(it->row(), 0);
    if (!rI) return;
    uint32_t rva = 0;
    if (!parseRva(rI->text(), &rva)) return;
    bool ok = false;
    float v = it->text().trimmed().toFloat(&ok);
    if (!ok) return;
    m_userEdits[rva] = v;
    logQt(QString("user edit rva 0x%1 = %2  (%3)")
        .arg(rva, 8, 16).arg(v).arg(it->text()));
}

// ---------------------------------------------------------------------------
// Stickiness probe.
//
// NR2003 keeps per-car setup in 22 chassis blocks (.data) AND in 23 "Mem Loc"
// 40-float tables (memloc-00..22) that the blocks point at through slots
// 0x3B..0x51.  Editing the block is only useful if the game KEEPS that value;
// if it repopulates the block from MemLoc (or from the .car file) at car-load
// time, the edit silently disappears.  This button answers that question
// empirically instead of guessing:
//
//   press 1  -> read the selected slot, remember it, write a sentinel,
//               read back, report "confirmed", stay armed
//   (user changes car / restarts the race / loads a setup)
//   press 2  -> read the same slot again and report STUCK or REVERTED
// ---------------------------------------------------------------------------
void MainWindow::stickinessProbe() {
    QTableWidget *tbl = memTable;
    int row = tbl ? tbl->currentRow() : -1;
    if (row < 0) {
        QMessageBox::information(this, "Probe", "Select a row in one of the block tabs first.");
        return;
    }
    auto *rI = tbl->item(row, 0);
    uint32_t rva = 0;
    if (!rI || !parseRva(rI->text(), &rva)) {
        QMessageBox::warning(this, "Probe",
            QString("Cannot parse RVA from \"%1\" (row %2, column 0)").arg(rI ? rI->text() : "").arg(row));
        return;
    }
    // find the owning block so we can test slotIsGameOwned and know the label
    const Nr2003BlockDesc *bd = nullptr; uint16_t slot = 0;
    for (uint32_t bi = 0; bi < NR2003_BLOCK_COUNT; ++bi) {
        const auto &b = NR2003_BLOCKS[bi];
        if (rva < b.baseRva) continue;
        uint32_t d = rva - b.baseRva;
        if (d % 4u) continue;
        uint16_t sl = uint16_t(d / 4u);
        if (sl < b.floatCount) { bd = &b; slot = sl; break; }
    }
    if (!bd) {
        QMessageBox::warning(this, "Probe",
            QString("RVA 0x%1 is not inside any known block.").arg(rva, 8, 16));
        return;
    }
    if (slotIsGameOwned(*bd, slot)) {
        QMessageBox::warning(this, "Probe",
            QString("rva 0x%1 (%2 slot %3) is game-owned (uint32 bits / Mem Loc pointer).\n"
                    "Pick a normal float slot - writing here nulls a pointer and kills NR2003.")
                .arg(rva, 8, 16).arg(QString::fromLatin1(bd->name)).arg(slot));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString vmsg;
    QHash<uint32_t,float> live = readAllBlockValues(1500, &vmsg);
    QApplication::restoreOverrideCursor();
    float now = live.value(rva, 0.0f);
    QString label = QString("%1 %2 [%3]")
        .arg(QString::fromLatin1(bd->name))
        .arg(QString::fromLatin1(nr2003_block_field_name(bd->module, slot)))
        .arg(slot);

    if (!m_probeArmed || m_probeRva != rva) {
        // ---- press 1: arm, remember, write a sentinel, confirm ----
        m_probeOrig[rva] = now;
        m_probeRva  = rva;
        m_probeValue = 9999.0f;
        m_probeArmed = true;

        Nr2003MemCmd cmd{};
        cmd.magic = NR2003_MEM_MAGIC; cmd.version = NR2003_MEM_VERSION; cmd.size = NR2003_MEM_SIZE;
        cmd.seq = memSeq++; if (!memSeq) memSeq = 1;
        cmd.count = bd->floatCount; cmd.ackSeq = 0; cmd.status = NR2003_MEM_OP_BLOCKM;
        cmd.tailMagic = NR2003_MEM_TAIL_MAGIC;
        cmd.entries[0].rva = bd->baseRva; cmd.entries[0].type = 0; cmd.entries[0].value = 0.0f;
        memset(cmd.entries[0].label, 0, 32); strncpy(cmd.entries[0].label, bd->name, 31);
        for (uint16_t s = 0; s < bd->floatCount; ++s) {
            uint32_t er = (s == slot) ? rva : uint32_t(NR2003_MEM_VAL_KEEP);
            cmd.entries[1 + s].rva = er;
            cmd.entries[1 + s].value = (s == slot) ? m_probeValue : 0.0f;
            cmd.entries[1 + s].type = 0;
            memset(cmd.entries[1 + s].label, 0, 32);
        }
        int status = -2;
        bool ack = sendMemCmd(cmd, &status, 1500);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QHash<uint32_t,float> after = readAllBlockValues(1500, &vmsg);
        QApplication::restoreOverrideCursor();
        float got = after.value(rva, 0.0f);
        bool landed = (got == m_probeValue);
        logQt(QString("probe ARMED  rva 0x%1  %2  was %3 -> wrote %4  ack %5 status %6  readback %7  %8")
            .arg(rva, 8, 16).arg(label).arg(now).arg(m_probeValue)
            .arg(ack).arg(status).arg(got)
            .arg(landed ? "CONFIRMED" : "NOT CONFIRMED"));
        memStatus->setText(QString("Probe armed on %1 (rva 0x%2): wrote %3, read back %4. "
                                  "Now change car / restart the race, then press Probe again.")
            .arg(label).arg(rva, 8, 16).arg(m_probeValue).arg(got));
        if (!landed)
            logQt("probe WARNING: sentinel did not read back - the write was rejected or the slot is not writable");
        showValueInTable(rva, got);
        return;
    }

    // ---- press 2: same rva, read again ----
    float got = live.value(rva, 0.0f);
    bool stuck = (got == m_probeValue);
    logQt(QString("probe RESULT rva 0x%1  %2  sentinel %3  now %4  -> %5 (original was %6)")
        .arg(rva, 8, 16).arg(label).arg(m_probeValue).arg(got)
        .arg(stuck ? "STUCK" : "REVERTED").arg(m_probeOrig.value(rva)));
    showValueInTable(rva, got);
    memStatus->setText(stuck
        ? QString("Probe: STUCK - %1 kept %2. The per-car block is authoritative; edit the block tabs.").arg(label).arg(got)
        : QString("Probe: REVERTED - %1 is now %2 (was %3 before the probe). The game repopulates the "
                  "block, so edit the MemLoc tab (or the .car file) instead.").arg(label).arg(got).arg(m_probeOrig.value(rva)));
    QMessageBox::information(this, "Probe", memStatus->text()
        + "\n\n(Pressing Probe again on the same row re-arms it with a new sentinel.)");
    m_probeArmed = false;
}

// A probed slot may not be the selected row any more (the user clicks around
// between the two presses), and the block tables can be showing 0 simply
// because Read-from-memory has not been pressed since startup.  Put the value
// the probe just measured into whichever table owns that rva, and re-baseline
// it so the edit diff does not treat the probe as a pending user change.
void MainWindow::showValueInTable(uint32_t rva, float v) {
    for (QTableWidget *tbl : {chassisTable, engineTable, wheelTable, memlocTable}) {
        for (int r = 0; r < tbl->rowCount(); ++r) {
            auto *rI = tbl->item(r, 0);
            if (!rI) continue;
            uint32_t got = 0;
            if (!parseRva(rI->text(), &got) || got != rva) continue;
            auto *vI = tbl->item(r, 4);
            if (!vI) continue;
            m_suppressEdits = true;
            vI->setText(QString::number(v, 'g', 9));
            m_suppressEdits = false;
            vI->setToolTip("value read from NR2003 by the stickiness probe");
            m_baseline[rva] = v;
            m_userEdits.remove(rva);
            logQt(QString("showValueInTable rva 0x%1 = %2 in %3")
                .arg(rva, 8, 16).arg(v).arg(tbl->objectName().isEmpty()
                    ? QString("table") : tbl->objectName()));
            return;
        }
    }
}

void MainWindow::writeBlocks() {
    logQt(QString("writeBlocks click blocks=%1").arg(NR2003_BLOCK_COUNT));
    // Only the cells the user CHANGED are written.  Seeding this map with the
    // whole Mem CSV would blindly overwrite all 2762 live slots on every click
    // and the user's edit would simply be clobbered by the CSV value - the
    // button has to mean "apply my changes", nothing else.
    QHash<uint32_t,float> want;
    QHash<uint32_t,float> csvAll;
    {
        bool okLoad=false; QString loadMsg;
        csvAll = loadMemCsvValues(&okLoad, &loadMsg);
        logQt(QString("writeBlocks mem-csv available: %1").arg(loadMsg));
    }
    // Let the user override / hand-tune: any value typed into the editable
    // Value column of the chassis or wheel table wins over the CSV value.
    //
    // A QTableWidget cell that is still being edited keeps the typed text in
    // the open QLineEdit delegate - item()->text() still returns the OLD
    // value.  Commit any open editor first, otherwise a value the user just
    // typed is silently dropped and the write looks like a no-op.
    // Detect edits by DIFFING the displayed value against the value the table
    // was built with.  This is independent of QTableWidget::itemChanged and of
    // whether the inline editor has committed, which is the whole class of bug
    // we are avoiding here.
    QHash<uint32_t,float> userEdits;
    for (auto *tbl : {chassisTable, wheelTable, memlocTable}) {
        for (int i=0;i<tbl->rowCount();++i) {
            auto *rvaI = tbl->item(i,0); auto *valI = tbl->item(i,4);
            if (!rvaI || !valI) continue;
            if (!valI->flags().testFlag(Qt::ItemIsEditable)) continue;
            uint32_t rva = 0; if (!parseRva(rvaI->text(), &rva)) continue;
    bool ok=true;   // no longer the parse flag; reused by later toFloat/toUInt
            if (!ok || rva==0) continue;
            QString t = valI->text().trimmed();
            if (t.isEmpty()) continue;
            float v = t.toFloat(&ok); if (!ok) continue;
            auto base = m_baseline.constFind(rva);
            if (base != m_baseline.constEnd()) {
                float b0 = base.value();
                if (v==b0) continue;
                if (v==v && b0==b0 && qAbs(v-b0) <= 1e-6*qMax(1.0f,qAbs(b0))) continue;
            }
            userEdits[rva] = v;
        }
    }
    int nCommitted = userEdits.size();
    logQt(QString("writeBlocks edited cells: %1 (itemChanged saw %2)")
              .arg(nCommitted).arg(m_userEdits.size()));
    for (auto it = userEdits.constBegin(); it != userEdits.constEnd(); ++it)
        logQt(QString("  edit rva 0x%1 : %2 -> %3")
            .arg(it.key(),8,16).arg(m_baseline.value(it.key(),0.0f)).arg(it.value()));
    for (auto it = m_userEdits.constBegin(); it != m_userEdits.constEnd(); ++it)
        if (!userEdits.contains(it.key())) userEdits[it.key()] = it.value();

    // An inline editor that is still open holds the typed text in a QLineEdit
    // and never reaches item()->text().  Qt6 gives no public slot to commit it
    // (closeEditor/commitData are protected), so read it directly: only one
    // editor can be open at a time and it always belongs to the current cell.
    for (auto *tbl : {chassisTable, wheelTable, memlocTable}) {
        QAbstractItemView *v = tbl;   // QTableWidget IS-A QAbstractItemView
        QModelIndex ci = v->currentIndex();
        if (ci.row() < 0) continue;
        auto *le = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        if (!le) continue;
        bool ok=false; float fv = le->text().trimmed().toFloat(&ok);
        if (!ok) continue;
        auto *rI = tbl->item(ci.row(), 0), *vI = tbl->item(ci.row(), 4);
        if (!rI || !vI) continue;
        uint32_t rva = 0; if (!parseRva(rI->text(), &rva)) continue;
        if (rva==0) continue;
        if (!vI->flags().testFlag(Qt::ItemIsEditable)) continue;
        auto base = m_baseline.constFind(rva);
        if (base != m_baseline.constEnd()) {
            float b0 = base.value();
            if (fv==b0) continue;
            if (fv==fv && b0==b0 && qAbs(fv-b0) <= 1e-6*qMax(1.0f,qAbs(b0))) continue;
        }
        userEdits[rva] = fv;
        logQt(QString("  edit rva 0x%1 : %2 -> %3 (from open editor)")
            .arg(rva,8,16).arg(m_baseline.value(rva,0.0f)).arg(fv));
    }
    for (auto it = userEdits.constBegin(); it != userEdits.constEnd(); ++it)
        want[it.key()] = it.value();
    logQt(QString("writeBlocks cells to write %1 (csv pool %2)")
              .arg(want.size()).arg(csvAll.size()));
    if (want.isEmpty()) {
        QMessageBox::information(this, "Write Blocks",
            "No cells were changed, so nothing will be written.\n\n"
            "Edit a Value cell in the Chassis or Wheel tab first, then press this again.\n"
            "(The Mem CSVs hold 3210 reference values; this button only applies YOUR edits.)");
        logQt("writeBlocks nothing to write - no edited cells");
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    int nOk=0, nFail=0, nRdataSkipped=0, nPatched=0, nEmpty=0, nPtrKept=0;
    QStringList failures;
    for (uint32_t bi=0; bi<NR2003_BLOCK_COUNT; ++bi) {
        const auto &b = NR2003_BLOCKS[bi];
        if (b.flags & NR2003_BLOCK_FLAG_RDATA) {
            if (b.module == NR2003_BLOCK_MOD_ENGINE) { nRdataSkipped++; continue; } // offline patcher
        }
        Nr2003MemCmd cmd{};
        cmd.magic = NR2003_MEM_MAGIC; cmd.version = NR2003_MEM_VERSION; cmd.size = NR2003_MEM_SIZE;
        cmd.seq = memSeq++; if (!memSeq) memSeq = 1;
        cmd.count = b.floatCount; cmd.ackSeq = 0; cmd.status = NR2003_MEM_OP_BLOCKM;
        cmd.tailMagic = NR2003_MEM_TAIL_MAGIC;
        cmd.entries[0].rva = b.baseRva; cmd.entries[0].type = 0; cmd.entries[0].value = 0.0f;
        memset(cmd.entries[0].label,0,32); strncpy(cmd.entries[0].label, b.name, 31);
        int patched = 0, ptrKept = 0;
        for (uint16_t s=0; s<b.floatCount; ++s) {
            uint32_t rva = b.baseRva + 4u*s;
            // Slots 0x3B..0x51 of a chassis-shaped block hold the Mem Loc
            // addresses the game dereferences.  Writing a float there nulls the
            // pointer and kills NR2003 (0x008be5e0), so never send them - the
            // pointed-to table has to be edited instead.
            if (b.floatCount >= 0x72 && nr2003_block_is_ptr_slot(s)) {
                cmd.entries[1+s].rva = NR2003_MEM_VAL_KEEP; cmd.entries[1+s].value = 0.0f;
                ptrKept++;
                cmd.entries[1+s].type = 0;
                memset(cmd.entries[1+s].label,0,32);
                continue;
            }
            auto it = want.constFind(rva);
            if (it == want.constEnd()) { cmd.entries[1+s].rva = NR2003_MEM_VAL_KEEP; cmd.entries[1+s].value = 0.0f; }
            else { cmd.entries[1+s].rva = rva; cmd.entries[1+s].value = it.value(); patched++; }
            cmd.entries[1+s].type = 0;
            memset(cmd.entries[1+s].label,0,32);
        }
        nPtrKept += ptrKept;
        nPatched += patched;
        if (patched == 0) { nEmpty++; continue; }
        int status = -2;
        bool ack = sendMemCmd(cmd, &status, 1500);
        logQt(QString("writeBlocks %1 rva %2 n %3 patched %4 ack %5 status %6")
                 .arg(QString::fromLatin1(b.name)).arg(b.baseRva,8,16).arg(b.floatCount).arg(patched).arg(ack).arg(status));
        if (ack && status > 0) nOk++; else { nFail++; if (failures.size()<8) failures.append(QString("%1 0x%2 (%3)")
            .arg(QString::fromLatin1(b.name)).arg(b.baseRva,8,16).arg(status)); }
        QCoreApplication::processEvents();
    }
    QApplication::restoreOverrideCursor();
    const QString summary = QString("Blocks: %1 ok, %2 failed, %3 engine skipped (.rdata → offline patcher), %4 no CSV rows — %5 float slots patched, %6 pointer slots (0x3B-0x51) left untouched")
        .arg(nOk).arg(nFail).arg(nRdataSkipped).arg(nEmpty).arg(nPatched).arg(nPtrKept);
    memStatus->setText(summary + (failures.isEmpty()?"":(" | "+failures.join(", "))));
    logQt(QString("writeBlocks done ok=%1 fail=%2 rdataSkipped=%3 empty=%4 patched=%5 ptrKept=%6").arg(nOk).arg(nFail).arg(nRdataSkipped).arg(nEmpty).arg(nPatched).arg(nPtrKept));

    // ---- read-back verification -------------------------------------------
    // "ok" above only means the server accepted the command; it does not prove
    // the float is in the game's memory.  Re-read the blocks and compare every
    // slot the user actually typed, so a silent no-op is visible.
    if (!userEdits.isEmpty()) {
        QString vmsg; QHash<uint32_t,float> live = readAllBlockValues(1500, &vmsg);
        int nChecked=0, nLand=0, nBad=0; QStringList bad;
        for (auto it = userEdits.constBegin(); it != userEdits.constEnd(); ++it) {
            auto l = live.constFind(it.key());
            if (l == live.constEnd()) continue;
            nChecked++;
            float got = l.value(), wantv = it.value();
            bool ok = (got==got && wantv==wantv) ||
                      (got==got && wantv==wantv &&
                       qAbs(got-wantv) <= 1e-3*qMax(1.0f, qAbs(wantv)));
            if (ok) nLand++; else { nBad++; if (bad.size()<12)
                bad.append(QString("rva 0x%1 wrote %2 read %3")
                    .arg(it.key(),8,16).arg(wantv).arg(got)); }
        }
        logQt(QString("writeBlocks VERIFY userEdits=%1 checked=%2 landed=%3 notLanded=%4%5")
            .arg(userEdits.size()).arg(nChecked).arg(nLand).arg(nBad)
            .arg(bad.isEmpty()?QString():(" | "+bad.join(", "))));
        memStatus->setText(QString("Verify: %1/%2 of your edits are in game memory%3")
            .arg(nLand).arg(nChecked).arg(bad.isEmpty()?"":(" | "+bad.join(", "))));
    } else {
        logQt("writeBlocks VERIFY skipped: no user edits this run");
    }
}

// ---------------------------------------------------------------------------
// Live block tables.
//
// The Live Mem tab used to be keyed on csv/fair_*.csv, whose &H
// values are RAW FILE OFFSETS into the static Nr2003.eXe.  They only coincide
// with live addresses inside .rdata (delta 0); everything else is const
// .text/.rdata and was never writable.  Editing a weight there therefore did
// nothing at all.
//
// The live data is the per-car contiguous float BLOCKS that tester.exe reads
// (see nr2003_blocks.h, recovered from tester/out/decomp_CALLER_0041ab40.c).
// The tables are now keyed on  rva = b.baseRva + 4*slot  so a value typed in
// the GUI is the value that gets written back.
// ---------------------------------------------------------------------------

// Slots the game owns: the 23 Mem Loc pointers (chassis slots 0x3B..0x51) and
// the uint32-bit index / NaN-sentinel slots.  They are never float physics
// data, so they must not be edited (writing 0.0f there crashed the game with
// "flds (%edx)" and EDX=0 at nr2003+0x32e47).
bool MainWindow::slotIsGameOwned(const Nr2003BlockDesc &b, uint16_t s) {
    if (b.floatCount >= 0x72) {                       // chassis-sized block
        if (nr2003_block_is_ptr_slot(s)) return true; // 0x3B..0x51
        static const int kIntSlots[] = {0,1,19,20,21,22,39,110};
        for (int k : kIntSlots) if (k == (int)s) return true;
    }
    return false;
}

// One BLOCKR round trip per block; returns rva -> value for every slot.
QHash<uint32_t,float> MainWindow::readAllBlockValues(int perBlockMs, QString *msg) {
    QHash<uint32_t,float> out;
    int nOk=0, nFail=0;
    static QVector<uint8_t> rb; rb.resize(NR2003_MEM_SIZE);
    for (uint32_t bi=0; bi<NR2003_BLOCK_COUNT; ++bi) {
        const auto &b = NR2003_BLOCKS[bi];
        Nr2003MemCmd cmd{};
        cmd.magic = NR2003_MEM_MAGIC; cmd.version = NR2003_MEM_VERSION; cmd.size = NR2003_MEM_SIZE;
        cmd.seq = memSeq++; if (!memSeq) memSeq = 1;
        cmd.count = b.floatCount; cmd.ackSeq = 0; cmd.status = NR2003_MEM_OP_BLOCKR;
        cmd.tailMagic = NR2003_MEM_TAIL_MAGIC;
        cmd.entries[0].rva = b.baseRva; cmd.entries[0].type = 0;
        memset(cmd.entries[0].label,0,32); strncpy(cmd.entries[0].label, b.name, 31);
        for (uint16_t s=0; s<b.floatCount; ++s) { cmd.entries[1+s].rva = NR2003_MEM_VAL_KEEP; cmd.entries[1+s].value = 0.0f; cmd.entries[1+s].type = 0; memset(cmd.entries[1+s].label,0,32); }
        int status = -2;
        if (sendMemCmd(cmd, &status, perBlockMs) && status > 0) {
            nOk++;
            int rfd = ::open(NR2003_MEM_PATH_LINUX, O_RDONLY);
            if (rfd >= 0) {
                if (::pread(rfd, rb.data(), NR2003_MEM_SIZE, 0) == (ssize_t)NR2003_MEM_SIZE) {
                    auto *c = const_cast<Nr2003MemCmd*>(reinterpret_cast<const Nr2003MemCmd*>(rb.constData()));
                    if (c->ackSeq == cmd.seq)
                        for (uint16_t s=0;s<b.floatCount;++s) out.insert(b.baseRva + 4u*s, c->entries[1+s].value);
                }
                ::close(rfd);
            }
        } else { nFail++; }
        QCoreApplication::processEvents();
    }
    if (msg) *msg = QString("%1 ok, %2 failed").arg(nOk).arg(nFail);
    logQt(QString("readAllBlockValues blocks=%1 values=%2 (%3)").arg(nOk+nFail).arg(out.size())
              .arg(msg?*msg:QString()));
    return out;
}

// Rebuild one module table from the transposed csv/ Mem CSV,
// keyed on live block-slot RVAs.  Value column = live value when the game
// answered, otherwise the CSV value.  Game-owned slots are locked.
int MainWindow::loadMemCsvTable(uint8_t module, QTableWidget *tbl,
                                const QHash<uint32_t,float> &live,
                                const QHash<uint32_t,float> &csvValues, QString *msg) {
    // m_userEdits/m_baseline are cleared once per reloadBlockTables() call;
    // clearing here would wipe the baseline of every table loaded before this
    // one, making every earlier cell look like a user edit.
    m_suppressEdits = true;
    tbl->setRowCount(0);
    int rows = 0, locked = 0, fromLive = 0;
    for (uint32_t bi=0; bi<NR2003_BLOCK_COUNT; ++bi) {
        const auto &b = NR2003_BLOCKS[bi];
        if (b.module != module || b.fieldOffset == 0) continue; // memloc has no Mem CSV
        for (uint16_t s=0; s<b.floatCount; ++s) {
            const uint32_t rva = b.baseRva + 4u*s;
            const bool owned = slotIsGameOwned(b, s);
            int r = tbl->rowCount();
            tbl->insertRow(r);
            const float val = live.contains(rva) ? live.value(rva) : csvValues.value(rva, 0.0f);
            if (live.contains(rva)) fromLive++;
            tbl->setItem(r,0,new QTableWidgetItem(QString("0x%1").arg(rva,8,16).toUpper()));
            tbl->setItem(r,1,new QTableWidgetItem(QString("&H%1").arg(0x400000u+rva,8,16).toUpper()));
            const QString fld = QString::fromLatin1(nr2003_block_field_name(module, s));
            tbl->setItem(r,2,new QTableWidgetItem(QString("%1 %2").arg(QString::fromLatin1(b.name), fld)));
            tbl->setItem(r,3,new QTableWidgetItem(owned ? "int/ptr" : "Sing"));
            QTableWidgetItem *vi = new QTableWidgetItem(QString::number(val,'g',9));
            if (owned) {
                vi->setFlags(vi->flags() & ~Qt::ItemIsEditable);
                vi->setToolTip("game-owned: uint32 bits (index / Mem Loc pointer / NaN sentinel) — never written");
                vi->setBackground(QColor(0x2b,0x2b,0x2b));
                locked++;
            } else if (!live.contains(rva)) {
                vi->setToolTip("CSV default (game not running, or block read failed)");
            } else {
                vi->setToolTip("live value read from NR2003 via BLOCKR");
            }
            tbl->setItem(r,4,vi);
            m_baseline[rva] = val;   // as displayed, for edit detection
            rows++;
        }
    }
    tbl->resizeColumnToContents(0); tbl->resizeColumnToContents(1);
    m_suppressEdits = false;
    if (msg) *msg = QString("%1 rows (%2 from live, %3 locked)").arg(rows).arg(fromLive).arg(locked);
    return rows;
}

void MainWindow::refreshMemTabTitles() {
    memTabs->setTabText(0, QString("Chassis (%1)").arg(chassisTable->rowCount()));
    memTabs->setTabText(1, QString("Engine (%1)").arg(engineTable->rowCount()));
    memTabs->setTabText(2, QString("Wheel (%1)").arg(wheelTable->rowCount()));
    memTabs->setTabText(3, QString("Garage (%1)").arg(garageTable->rowCount()));
    memTabs->setTabText(4, QString("MemLoc (%1)").arg(memlocTable->rowCount()));
}

void MainWindow::reloadBlockTables(bool tryLive) {
    bool ok=false; QString msg;
    QHash<uint32_t,float> csv = loadMemCsvValues(&ok, &msg);
    if (!ok) { logQt("reloadBlockTables: no csv/*-Mem.csv — " + msg); return; }
    logQt("reloadBlockTables: loaded Mem CSVs — " + msg);
    QHash<uint32_t,float> live;
    if (tryLive) { QString lmsg; live = readAllBlockValues(1500, &lmsg); }
    // One clear for the whole reload: the individual loaders below must
    // accumulate into the same baseline, otherwise only the last table would
    // be recognised and every other cell would diff as a user edit.
    m_userEdits.clear(); m_baseline.clear();
    QString cmsg, wmsg, emsg;
    int c = loadMemCsvTable(NR2003_BLOCK_MOD_CHASSIS, chassisTable, live, csv, &cmsg);
    int w = loadMemCsvTable(NR2003_BLOCK_MOD_WHEEL,   wheelTable,   live, csv, &wmsg);
    // engine is .rdata — still list it so the numbers are visible, but locked
    int e = loadMemCsvTable(NR2003_BLOCK_MOD_ENGINE,  engineTable,  live, csv, &emsg);
    { QString mlm;
      int n = loadMemlocTable(live, &mlm);
      logQt(QString("reloadBlockTables memloc %1").arg(mlm));
      Q_UNUSED(n); }
    refreshMemTabTitles();
    markReadOnlyTables();
    memStatus->setText(QString("Live block tables (per-car, live .data) — chassis %1, engine %2, wheel %3 | %4")
                       .arg(c).arg(e).arg(w).arg(msg));
    logQt(QString("reloadBlockTables chassis[%1] engine[%2] wheel[%3] | csv: %4 | live: %5")
              .arg(cmsg, emsg, wmsg, msg).arg(live.size()));
}

// "Read from memory" — pull every live block and refresh the Value column.
void MainWindow::refreshLiveValues() {
    if (chassisTable->rowCount() == 0) reloadBlockTables(false); // build on demand
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString lmsg;
    QHash<uint32_t,float> live = readAllBlockValues(1500, &lmsg);
    QApplication::restoreOverrideCursor();
    if (live.isEmpty()) {
        QMessageBox::warning(this, "NR2003", QString("No live block values read (%1).\nIs run_nr2003_allinone.sh running?").arg(lmsg));
        return;
    }
    int applied=0;
    auto applyTo = [&](QTableWidget *tbl)->bool{
        for (int r=0;r<tbl->rowCount();++r) {
            QTableWidgetItem *rI = tbl->item(r,0), *vI = tbl->item(r,4);
            if (!rI || !vI) continue;
            uint32_t rva = 0; if (!parseRva(rI->text(), &rva)) continue;
            if (!live.contains(rva)) continue;
            const float lv = live.value(rva);
            vI->setText(QString::number(lv,'g',9));
            // The refreshed value is the new baseline: without this, Write
            // would treat every value that came from BLOCKR as a user edit and
            // blindly write thousands of cells the user never touched.
            m_baseline[rva] = lv;
            applied++;
        }
        return true;
    };
    m_suppressEdits = true;
    applyTo(chassisTable); applyTo(engineTable); applyTo(wheelTable); applyTo(memlocTable);
    m_suppressEdits = false;
    m_userEdits.clear();   // a fresh read supersedes any edits staged before it
    refreshMemTabTitles();
    memStatus->setText(QString("Live read: %1 slot values from %2 blocks — %3 cells refreshed")
                       .arg(live.size()).arg(lmsg).arg(applied));
    logQt(QString("refreshLiveValues cells=%1").arg(applied));
}

void MainWindow::pollMemAck() {
    QFile f(NR2003_MEM_PATH_LINUX);
    if (!f.open(QIODevice::ReadOnly)) return;
    if (f.size() < (qint64)NR2003_MEM_SIZE) return;
    uint8_t buf[NR2003_MEM_SIZE];
    if (f.read((char*)buf, NR2003_MEM_SIZE)!= (qint64)NR2003_MEM_SIZE) return;
    auto *c = reinterpret_cast<Nr2003MemCmd*>(buf);
    if (c->magic!=NR2003_MEM_MAGIC || c->tailMagic!=NR2003_MEM_TAIL_MAGIC) return;
    if (c->ackSeq!=0) {
        QString s = QString("ackSeq %1 status %2 count %3").arg(c->ackSeq).arg(c->status).arg(c->count);
        if (c->status>0) s+=" — OK";
        else if (c->status<0) s+=" — ERR (OpenProcess/WriteProcessMemory)";
        memStatus->setText(s);
    }
}

int MainWindow::findNr2003Pid() {
    // Real game under Wine: exe -> wine, comm -> nr2003.exe, cmdline -> Nr2003.eXe, maps has 00400000 r-xp
    // Previous exe.contains("nr2003") matched stale helpers (17223) not real 14942; fix to comm/cmdline+maps
    static int cachedPid=-1;
    auto pidAlive = [](int pid)->bool{
        QFile cf(QString("/proc/%1/comm").arg(pid));
        if (cf.open(QIODevice::ReadOnly)) {
            QString c = QString::fromUtf8(cf.readAll()).trimmed().toLower();
            if (c.contains("nr2003")) return true;
        }
        QFile cl(QString("/proc/%1/cmdline").arg(pid));
        if (cl.open(QIODevice::ReadOnly)) {
            QByteArray b = cl.readAll().toLower();
            if (b.contains("nr2003")) return true;
        }
        return false;
    };
    logQt(QString("findNr2003Pid cached=%1").arg(cachedPid));
    if (cachedPid>0 && pidAlive(cachedPid)) {
        QFile maps(QString("/proc/%1/maps").arg(cachedPid));
        if (maps.open(QIODevice::ReadOnly)) {
            QByteArray m = maps.readAll();
            if (m.contains("00400000-") && m.contains(" r-xp ")) return cachedPid;
        } else return cachedPid;
    }
    QDir proc("/proc");
    QStringList pids = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    std::sort(pids.begin(), pids.end(), [](const QString&a,const QString&b){return a.toInt()<b.toInt();});
    struct Cand{int pid; QString exe; QString comm; QString cmdline; bool hasMap;};
    QVector<Cand> cands;
    for (auto e : pids) {
        bool ok=false; int pid=e.toInt(&ok); if (!ok) continue;
        QFile commF(QString("/proc/%1/comm").arg(pid));
        QString comm; if (commF.open(QIODevice::ReadOnly)) comm=QString::fromUtf8(commF.readAll()).trimmed().toLower();
        QFile clF(QString("/proc/%1/cmdline").arg(pid));
        QByteArray clRaw; if (clF.open(QIODevice::ReadOnly)) clRaw=clF.readAll().toLower();
        // /proc/<pid>/cmdline is a fixed-size NUL-padded block, not a C string.  Keep
        // the NULs and every log line grows a tail of \00 garbage, so turn them into
        // spaces and collapse the padding.
        QString cl = QString::fromUtf8(clRaw);
        cl.replace(QChar(0), QChar(' '));
        cl = cl.simplified();
        // strict: must be real PE (comm == nr2003.exe) and cmdline is ".../Nr2003.eXe" not the wrapper
        bool isWrapper = cl.contains("run_nr2003");
        bool isNr = !isWrapper && (comm=="nr2003.exe" || comm=="nr2003") && cl.contains("nr2003.exe");
        if (!isNr) continue;
        char exePath[64]; snprintf(exePath,sizeof(exePath),"/proc/%d/exe",pid);
        char link[512]={0}; ssize_t n=readlink(exePath,link,sizeof(link)-1);
        QString exe; if (n>0) exe=QString::fromUtf8(link,n).toLower();
        QFile maps(QString("/proc/%1/maps").arg(pid));
        bool hasMap=false;
        if (maps.open(QIODevice::ReadOnly)) {
            QByteArray m = maps.readAll();
            hasMap = m.contains("00400000-") && m.contains(" r-xp ");
            if (hasMap && !m.toLower().contains("nr2003")) hasMap=false; // need PE-mapped nr2003 segment
        }
        if (!hasMap) continue;
        cands.push_back({pid, exe, comm, cl, hasMap});
    }
    if (cands.isEmpty()) { logQt("findNr2003Pid no cands"); cachedPid=-1; return -1; }
    // exactly one real game at a time — smallest pid among strict matches (14942)
    for(auto &c:cands) logQt(QString("findNr2003Pid cand pid=%1 exe=%2 comm=%3 hasMap=%4 cmd=%5").arg(c.pid).arg(c.exe).arg(c.comm).arg(c.hasMap).arg(c.cmdline.left(80)));
    int best=cands.first().pid;
    for (auto &c: cands) if (c.pid<best) best=c.pid;
    logQt(QString("findNr2003Pid pick %1 from %2 cands").arg(best).arg(cands.size()));
    cachedPid=best; return best;
}

bool MainWindow::readProcFloat(int pid, uint32_t va, float &out) {
    QString memPath = QString("/proc/%1/mem").arg(pid);
    int fd = ::open(memPath.toUtf8().constData(), O_RDONLY);
    if (fd<0) return false;
    // need to allow ptrace: try to attach? Native Qt runs as same user, but /proc/pid/mem requires PTRACE_MODE_ATTACH.
    // Fallback: try pread, if EIO try via process_vm_readv
    ssize_t n = pread(fd, &out, sizeof(float), (off_t)va);
    ::close(fd);
    if (n==sizeof(float)) return true;
    // fallback: process_vm_readv
    struct iovec local{&out,sizeof(float)}; struct iovec remote{(void*)(uintptr_t)va,sizeof(float)};
    n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    return n==sizeof(float);
}

static int getWinePid() {
    QByteArray wpfx = qgetenv("WINEPREFIX");
    QString prog = "wine";
    QProcess p; p.setProgram(prog); p.setArguments({"tasklist"});
    if (!wpfx.isEmpty()) p.setEnvironment(QProcess::systemEnvironment() << QString("WINEPREFIX=%1").arg(QString::fromUtf8(wpfx)));
    p.start(); if (!p.waitForFinished(3000)) return -1;
    QString out = QString::fromUtf8(p.readAllStandardOutput());
    for (auto line: out.split('\n')) {
        if (line.contains("nr2003", Qt::CaseInsensitive)) {
            QRegularExpression re(R"(\s+(\d+)\s+)");
            auto m = re.globalMatch(line);
            while (m.hasNext()) { auto cap = m.next().captured(1); bool ok=false; int v=cap.toInt(&ok); if (ok && v>0 && v<65536) return v; }
            // fallback token scan
            for (auto tok: line.split(QRegularExpression(R"(\s+)"), Qt::SkipEmptyParts)) { bool ok=false; int v=tok.toInt(&ok); if(ok && v>4 && v<65536) return v; }
        }
    }
    return -1;
}
static QString pidStatus(int linuxPid) {
    int winePid = getWinePid();
    if (winePid>0) return QString("Linux %1 / Wine %2").arg(linuxPid).arg(winePid);
    return QString("Linux %1 (Wine tasklist n/a)").arg(linuxPid);
}

void MainWindow::readMem() {
    auto tbls = allMemTables();
    int totalRows=0; for (auto *t: tbls) totalRows+=t->rowCount();
    if (totalRows==0) { QMessageBox::information(this,"Read","Load fair_*.csv first (4 tabs)"); return; }
    int pid = findNr2003Pid();
    if (pid<0) { QMessageBox::warning(this,"Read","NR2003.exe not found in /proc (run via run_nr2003_allinone.sh)"); return; }
    int okCnt=0, failCnt=0;
    for (auto *tbl: tbls) for (int i=0;i<tbl->rowCount();++i) {
        auto *rvaItem = tbl->item(i,0);
        auto *typeItem = tbl->item(i,3);
        auto *valItem = tbl->item(i,4);
        if (!rvaItem || !valItem) continue;
        uint32_t rva = 0; if (!parseRva(rvaItem->text(), &rva)) continue;
    bool ok=true;   // no longer the parse flag; reused by later toFloat/toUInt
        if (!ok) continue;
        uint32_t va = 0x400000 + rva; // fix -1 bug (live_memory_io.py:54 addresses.csv 0x6EE57C → 0x6EE57D) also correct for static &H RVA→raw
        QString type = typeItem? typeItem->text() : "Sing";
        if (type.contains("Dubl", Qt::CaseInsensitive)) {
            double d=0; bool got=false;
            QString memPath = QString("/proc/%1/mem").arg(pid);
            int fd = ::open(memPath.toUtf8().constData(), O_RDONLY);
            if (fd>=0) { got = pread(fd,&d,sizeof(double),(off_t)va)==sizeof(double); ::close(fd); }
            if (!got) { struct iovec lo{&d,sizeof(double)}; struct iovec rm{(void*)(uintptr_t)va,sizeof(double)}; got = process_vm_readv(pid,&lo,1,&rm,1,0)==sizeof(double); }
            if (got) { valItem->setText(QString::number(d,'g',12)); okCnt++; } else failCnt++;
        } else {
            float f=0; if (readProcFloat(pid, va, f)) { valItem->setText(QString::number(f,'g',8)); okCnt++; } else failCnt++;
        }
    }
    QString pids = pidStatus(pid);
    // fallback via MEM3 read through tsample (works inside Wine, avoids PTRACE_MODE_ATTACH EPERM for wine-preloader 131423)
    if (failCnt>0 && okCnt < totalRows) {
        auto fallbackRead = [&]() -> int {
            int fbOk=0;
            struct Item2 { uint32_t rva; int row; QTableWidget* tbl; int type; };
            QVector<Item2> all2; for (auto *tbl: tbls) for (int i=0;i<tbl->rowCount();++i) { auto *r=tbl->item(i,0); if (!r) continue; uint32_t rva=0; if (!parseRva(r->text(), &rva)) continue; auto *t=tbl->item(i,3); int tp=(t&&t->text().contains("Dubl",Qt::CaseInsensitive))?1:0; all2.push_back({rva,i,tbl,tp}); }
    bool ok=true;   // no longer the parse flag; reused by later toFloat/toUInt
            int total2=all2.size(), batches=(total2+63)/64;
            for (int b=0;b<batches;++b) {
                int off=b*64, n=qMin(64,total2-off);
                Nr2003MemCmd cmd{}; cmd.magic=NR2003_MEM_MAGIC; cmd.version=NR2003_MEM_VERSION; cmd.size=NR2003_MEM_SIZE; cmd.seq=memSeq++; if(memSeq==0) memSeq=1; cmd.count=n; cmd.status=0x7FFFFFFE; cmd.ackSeq=0; cmd.tailMagic=NR2003_MEM_TAIL_MAGIC;
                for(int i=0;i<n;++i){ cmd.entries[i].rva=all2[off+i].rva; cmd.entries[i].type=all2[off+i].type; cmd.entries[i].value=0; }
                int fd=::open(NR2003_MEM_PATH_LINUX,O_RDWR); if(fd<0) return fbOk; ssize_t wn=pwrite(fd,&cmd,NR2003_MEM_SIZE,0); fsync(fd); ::close(fd); if(wn!=(ssize_t)NR2003_MEM_SIZE) return fbOk;
                bool ack=false; Nr2003MemCmd rc{};
                for(int k=0;k<50;++k){ QThread::msleep(20); int rfd=::open(NR2003_MEM_PATH_LINUX,O_RDONLY); if(rfd<0) continue; ssize_t rn=pread(rfd,&rc,NR2003_MEM_SIZE,0); ::close(rfd); if(rn!=(ssize_t)NR2003_MEM_SIZE) continue; if(rc.ackSeq==cmd.seq && rc.status>0){ ack=true; break; } }
                if(!ack) return fbOk;
                for(int i=0;i<n;++i){ auto &it=all2[off+i]; float v=rc.entries[i].value; if(it.type==1){ // Dubl already coerced to float in tsample
                    { QTableWidgetItem *vi = it.tbl->item(it.row,4);
                      if (vi) { m_suppressEdits = true;
                                vi->setText(QString::number((double)v,'g',12));
                                m_suppressEdits = false; } }
                } else it.tbl->item(it.row,4)->setText(QString::number(v,'g',8)); fbOk++; }
            }
            return fbOk;
        };
        int fb = fallbackRead();
        if (fb>0) {
            memStatus->setText(QString("Read %1 ok %2 fail %3 → MEM3 fallback %4 ok (ReadProcessMemory 0x400000+RVA via tsample) — 4 tabs").arg(okCnt).arg(failCnt).arg(pids).arg(fb));
            return;
        }
    }
    memStatus->setText(QString("Read %1 ok %2 fail from %3 (VA=0x400000+RVA) via /proc/%4/mem + process_vm_readv — 4 tabs").arg(okCnt).arg(failCnt).arg(pids).arg(pid));
}
