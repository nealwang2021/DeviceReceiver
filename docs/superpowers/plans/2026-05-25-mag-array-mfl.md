# 漏磁检测设备 MagArray 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 mag_array.proto 漏磁检测设备：GrpcMagArrayBackend + MagArrayWindow（波形图+热力图）+ 数据存储 + 历史总览。

**Architecture:** 新建 GrpcMagArrayBackend 实现 IReceiverBackend，新建 MagArrayWindow 继承 PlotWindowBase，复用 ArrayPlotWindow 子图模式 + HeatMapPlotWindow ColorMap 模式，数据经 FrameData(MagArray) → PlotDataHub → PlotSnapshot 流水线。

**Tech Stack:** C++17, Qt 5.15, gRPC, SQLite, QCustomPlot

---

### Task 1: Proto 生成 + CMake + FrameData 扩展

**Files:**
- Create: `proto/generated/mag_array.pb.cc`, `mag_array.pb.h`, `mag_array.grpc.pb.cc`, `mag_array.grpc.pb.h`
- Modify: `FrameData.h`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 生成 proto C++ 存根**

```powershell
Set-Location "d:\WS\qtpro\DeviceReceiver\proto"
& "D:\vcpkg\installed\x64-windows\tools\protobuf\protoc.exe" `
  "--proto_path=." `
  "--proto_path=D:\vcpkg\installed\x64-windows\include" `
  "--cpp_out=generated" `
  "--grpc_out=generated" `
  "--plugin=protoc-gen-grpc=D:\vcpkg\installed\x64-windows\tools\grpc\grpc_cpp_plugin.exe" `
  "mag_array.proto"
```

- [ ] **Step 2: CMakeLists.txt 添加 proto 源文件**

在 `target_sources(realtime_data PRIVATE ...)` 的 proto 区段添加:
```
${CMAKE_CURRENT_SOURCE_DIR}/proto/generated/mag_array.pb.cc
${CMAKE_CURRENT_SOURCE_DIR}/proto/generated/mag_array.grpc.pb.cc
```

- [ ] **Step 3: FrameData.h 新增 MagArray 模式**

DetectionMode 枚举新增:
```cpp
MagArray = 4  // 漏磁检测（实数单频，60 通道）
```

新增结构体:
```cpp
struct MagSensorResult {
    int     sensorIndex = 0;
    double  xMean = 0.0;
    double  yMean = 0.0;
    double  zMean = 0.0;
    double  xLatest = 0.0;
    double  yLatest = 0.0;
    double  zLatest = 0.0;
    double  magnitudeMean = 0.0;
    double  magnitudeLatest = 0.0;
};
```

FrameData 新增:
```cpp
QVector<MagSensorResult> magSensorResults;
```

构造函数初始化列表添加 `magSensorResults()`。

- [ ] **Step 4: 构建提交**

```powershell
Set-Location "d:\WS\qtpro\DeviceReceiver"; .\build_cmake.bat
```

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add -A && git commit -m "feat(漏磁): proto生成 + FrameData MagArray 模式"
```

---

### Task 2: 创建 GrpcMagArrayBackend

**Files:**
- Create: `GrpcMagArrayBackend.h`, `GrpcMagArrayBackend.cpp`
- Modify: `ApplicationController.cpp`, `CMakeLists.txt`

- [ ] **Step 1: GrpcMagArrayBackend.h — 声明**

```cpp
#ifndef GRPCMAGARRAYBACKEND_H
#define GRPCMAGARRAYBACKEND_H

#include "IReceiverBackend.h"
#include "BackendParamDescriptor.h"
#include <QTimer>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#ifdef HAS_GRPC
#include <grpcpp/grpcpp.h>
#include "mag_array.grpc.pb.h"
#endif

class GrpcMagArrayBackend : public IReceiverBackend
{
    Q_OBJECT
public:
    explicit GrpcMagArrayBackend(QObject* parent = nullptr);
    ~GrpcMagArrayBackend() override;

    static QVector<BackendParamDescriptor> configParameters();
    QVector<BackendParamDescriptor> configParameters() const override { return configParameters(); }

public slots:
    bool connectBackend(const QString& endpoint) override;
    void disconnectBackend() override;
    bool isBackendConnected() const override;
    void startAcquisition(int intervalMs = 100) override;
    void stopAcquisition() override;
    void setPaused(bool paused) override;
    void sendCommand(const QByteArray& command) override;
    void sendCommand(const QString& command, bool isHex = false) override;

    void setMockMode(bool enabled);

signals:
    void connectAttemptFinished(bool connected, const QString& detail);

private slots:
    void onMockTick();
    void onReconnectCheck();

private:
    void startStreamThread(int intervalMs);
    void stopStreamThread();
    void streamLoop(int intervalMs);
    void setConnected(bool connected);
    void emitDeviceStatus();

    QString m_endpoint;
    int m_acquisitionIntervalMs = 100;
    int m_connectTimeoutMs = 6000;

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_stopStream{false};
    std::atomic<bool> m_mockMode{true};
    std::atomic<qint64> m_lastFrameReceivedMs{0};

    QTimer* m_mockTimer = nullptr;
    QTimer* m_reconnectTimer = nullptr;
    quint64 m_frameCounter = 0;

    std::thread m_streamThread;
    std::mutex m_streamStateMutex;
    std::atomic<bool> m_disconnectInProgress{false};

#ifdef HAS_GRPC
    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<magarray::MagArrayLeakageDetection::Stub> m_stub;
    std::unique_ptr<grpc::ClientContext> m_streamCtx;
#endif
};

#endif
```

- [ ] **Step 2: GrpcMagArrayBackend.cpp — configParameters**

```cpp
#include "GrpcMagArrayBackend.h"
#include "AppConfig.h"
#include "FrameData.h"
#include "GrpcEndpointUtils.h"
#include <QDateTime>
#include <QDebug>
#include <QJsonObject>
#include <QRandomGenerator>
#include <chrono>
#include <thread>

QVector<BackendParamDescriptor> GrpcMagArrayBackend::configParameters()
{
    return {
        {"MagArray/PortName", QStringLiteral("串口名"), ParamEnum,
         QVariant(QString()), 0, 0, 1,
         {}}, // 端口列表在 UI 侧动态填充
        {"MagArray/Baudrate", QStringLiteral("波特率"), ParamInt,
         QVariant(1000000), 9600, 4000000, 100, {}},
        {"MagArray/PreprocessMode", QStringLiteral("预处理模式"), ParamEnum,
         QVariant(0), 0, 0, 1,
         {QStringLiteral("None"), QStringLiteral("FixedMidpoint"),
          QStringLiteral("AutoBaseline"), QStringLiteral("SlowTrackingBaseline")}},
        {"MagArray/BaselineFrames", QStringLiteral("基线帧数"), ParamInt,
         QVariant(50), 1, 500, 1, {}},
        {"MagArray/FixedMidpoint", QStringLiteral("固定中点"), ParamDouble,
         QVariant(32768.0), 0, 65535, 100, {}},
        {"MagArray/TrackingFactor", QStringLiteral("跟踪因子"), ParamDouble,
         QVariant(0.01), 0.001, 1.0, 0.001, {}},
    };
}
```

- [ ] **Step 3: GrpcMagArrayBackend.cpp — connectBackend / streamLoop**

`connectBackend()`: 解析 `port_name|baudrate` 格式，创建 gRPC channel → ListSerialPorts → StartDetection(preprocess config)。

`streamLoop()`: `StreamFrames(include_processed_values=true, include_sensor_values=true)` → 每个 `MagArrayFrame` → `FrameData`:
```cpp
FrameData frame;
frame.detectMode = FrameData::MagArray;
// 60 通道 → channels_comp0（取 processed_values 均值）
const int nChan = pbFrame.channels_size();
frame.channelCount = static_cast<uint8_t>(qMin(nChan, 200));
frame.channels_comp0.resize(nChan);
for (int i = 0; i < nChan; ++i) {
    const auto& ch = pbFrame.channels(i);
    double sum = 0; int cnt = 0;
    for (double v : ch.processed_values()) { sum += v; cnt++; }
    frame.channels_comp0[i] = cnt > 0 ? sum / cnt : 0.0;
}
// sensor_results
frame.magSensorResults.resize(pbFrame.sensor_results_size());
for (int i = 0; i < pbFrame.sensor_results_size(); ++i) {
    const auto& sr = pbFrame.sensor_results(i);
    MagSensorResult& r = frame.magSensorResults[i];
    r.sensorIndex = sr.sensor_index();
    r.xMean = sr.x_mean(); r.yMean = sr.y_mean(); r.zMean = sr.z_mean();
    r.xLatest = sr.x_latest(); r.yLatest = sr.y_latest(); r.zLatest = sr.z_latest();
    r.magnitudeMean = sr.magnitude_mean(); r.magnitudeLatest = sr.magnitude_latest();
}
emit frameReceived(frame);
```

Mock 模式: 生成 20 传感器 × 3 轴合成正弦波。

- [ ] **Step 4: ApplicationController.cpp — 后端工厂**

`initReceiverBackend()` 新增 `"magarray"` 分支:
```cpp
else if (backendType.compare("magarray", Qt::CaseInsensitive) == 0) {
    m_serialReceiver.reset(new GrpcMagArrayBackend);
}
```

- [ ] **Step 5: 构建提交**

```powershell
Set-Location "d:\WS\qtpro\DeviceReceiver"; .\build_cmake.bat
```

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add GrpcMagArrayBackend.h GrpcMagArrayBackend.cpp ApplicationController.cpp CMakeLists.txt && git commit -m "feat(漏磁): GrpcMagArrayBackend 实现"
```

---

### Task 3: 创建 MagArrayWindow（波形图 + 热力图）

**Files:**
- Create: `MagArrayWindow.h`, `MagArrayWindow.cpp`
- Modify: `PlotWindowManager.h`, `PlotWindowManager.cpp`
- Modify: `MainWindow.cpp` (窗口类型下拉)

- [ ] **Step 1: MagArrayWindow.h — 窗口结构**

```cpp
class MagArrayWindow : public PlotWindowBase
{
    Q_OBJECT
public:
    explicit MagArrayWindow(QWidget* parent = nullptr);
    ~MagArrayWindow() override;

public slots:
    void onDataUpdated(const QVector<FrameData>& frames) override;
    void onPlotSnapshotUpdated(const QSharedPointer<const PlotSnapshot>& snapshot) override;
    void onCriticalFrame(const FrameData& frame) override;

private:
    void buildUi();
    void updateWaveformFromSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot);
    void updateHeatmapFromSnapshot(const QSharedPointer<const PlotSnapshot>& snapshot);
    void updateHeatmapFromFrame(const FrameData& frame);

    // 波形图（左侧）
    QScrollArea* m_waveformScroll = nullptr;
    QCustomPlot* m_waveformPlot = nullptr;
    QVector<QCPAxisRect*> m_waveformAxisRects; // 60 个子图（3轴 × 20通道）
    QLabel* m_waveformTitle[3]; // X / Y / Z 分隔标题

    // 热力图（右侧）
    QScrollArea* m_heatmapScroll = nullptr;
    QCustomPlot* m_heatmapPlot = nullptr;
    QCPColorMap* m_colorMap = nullptr;
    QCPColorScale* m_colorScale = nullptr;

    // 控件
    QRadioButton* m_posModeBtn = nullptr;
    QRadioButton* m_timeModeBtn = nullptr;
    QDoubleSpinBox* m_colorMinSpin = nullptr;
    QDoubleSpinBox* m_colorMaxSpin = nullptr;
    QLabel* m_stageStatusLabel = nullptr;

    // 状态
    int m_currentChannelCount = 60;
    QElapsedTimer m_replotThrottle;
    int m_replotMinMs = 33;
};
```

- [ ] **Step 2: buildUi() — 布局构造**

QSplitter 左右分。左侧 QScrollArea 内含 QCustomPlot（60 个子图轴矩形，纵向排列），右侧 QScrollArea 内含 QCustomPlot（QCPColorMap）。

波形图 60 个子图: 0-19 = X轴, 20-39 = Y轴, 40-59 = Z轴。轴标题分隔在每组前。

热力图: `m_colorMap->setSize(100, 20)` — 横轴 100 格（台位或时间），纵轴 20 格（传感器）。

底栏: `[台位] [时间] min:[spin] max:[spin] 三轴台:● 已连接`

- [ ] **Step 3: updateWaveformFromSnapshot — 数据刷新**

从 `snapshot->realAmp[i]`（i=0..59）读取各通道时间序列，设置到对应 `m_plot->graph(i)`。

- [ ] **Step 4: updateHeatmapFromSnapshot/Frame — 热力图刷新**

从 `snapshot->magSensorResults` 或 `frame.magSensorResults` 取 20 个传感器的当前轴均值，填入 ColorMap 的行。

台位模式: 横轴取 `stagePose.stageXMm`，纵轴取传感器索引。

时间模式: 横轴取 `frame.timestamp` 或序号。

- [ ] **Step 5: PlotWindowManager 注册新窗口类型**

```cpp
// PlotWindowManager.h
enum PlotType {
    CombinedPlot, HeatmapPlot, ArrayPlot, PulsedDecayPlot,
    ArrayHeatmapPlot = 9,
    MagArrayPlot = 10  // 漏磁窗口
};

// PlotWindowManager.cpp createWindow()
case MagArrayPlot:
    window = new MagArrayWindow(parent);
    title = QStringLiteral("漏磁检测");
    break;
```

- [ ] **Step 6: MainWindow 下拉框新增**

```cpp
m_windowTypeCombo->addItems({..., QStringLiteral("漏磁检测")});
```

`onCreateWindowClicked` 新增:
```cpp
case 5: type = PlotWindowManager::MagArrayPlot; break;
```

- [ ] **Step 7: 构建提交**

```powershell
Set-Location "d:\WS\qtpro\DeviceReceiver"; .\build_cmake.bat
```

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add MagArrayWindow.h MagArrayWindow.cpp PlotWindowManager.h PlotWindowManager.cpp MainWindow.cpp && git commit -m "feat(漏磁): MagArrayWindow 波形图+热力图窗口"
```

---

### Task 4: PlotDataHub 适配 + DB 存储 + 历史总览

**Files:**
- Modify: `PlotDataHub.h`, `PlotDataHub.cpp`
- Modify: `RealtimeSqlRecorder.h`, `RealtimeSqlRecorder.cpp`
- Modify: `SqlHistoryQuery.h`, `SqlHistoryQuery.cpp`
- Modify: `HistoryOverviewWindow.cpp`

- [ ] **Step 1: PlotDataHub — MagArray 聚合**

`PlotSnapshot` 新增:
```cpp
QVector<MagSensorResult> magSensorResults;
```

`appendFrames()` 新增 `MagArray` 分支（复用 `MultiChannelReal` 的 `realAmp` 管道）:
```cpp
else if (frame.detectMode == FrameData::MagArray) {
    // 同 MultiChannelReal，额外存储 magSensorResults
    next->magSensorResults = frame.magSensorResults;
}
```

- [ ] **Step 2: RealtimeSqlRecorder — mag_array_frames 表**

Sherma:
```sql
CREATE TABLE IF NOT EXISTS mag_array_frames (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp_unix_ms INTEGER NOT NULL,
    frame_index INTEGER NOT NULL,
    sensor_index INTEGER NOT NULL,
    x_mean REAL, y_mean REAL, z_mean REAL,
    x_latest REAL, y_latest REAL, z_latest REAL,
    magnitude_mean REAL, magnitude_latest REAL
);
CREATE INDEX IF NOT EXISTS idx_magarray_ts ON mag_array_frames(timestamp_unix_ms);
```

`insertBatch()` 新增 `MagArray` 分支:
```cpp
if (frame.detectMode == FrameData::MagArray) {
    for (const auto& sr : frame.magSensorResults) {
        bind mag_array_frames INSERT params;
        exec();
    }
}
```

- [ ] **Step 3: SqlHistoryQuery — mag_array_frames 查询**

新增 `queryMagArrayEnvelope()`:
```sql
SELECT (timestamp_unix_ms / :bucket) * :bucket AS bucket_start,
       MIN(magnitude_mean), MAX(magnitude_mean)
FROM mag_array_frames
WHERE timestamp_unix_ms BETWEEN :start AND :end
GROUP BY bucket_start ORDER BY bucket_start ASC
```

- [ ] **Step 4: HistoryOverviewWindow — MagArray 包络探测**

`rebuildEnvelope()` 新增 `mag_array_frames` 探测（与 `multifreq_frames` 探针并列）。

- [ ] **Step 5: 构建提交**

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add PlotDataHub.h PlotDataHub.cpp RealtimeSqlRecorder.h RealtimeSqlRecorder.cpp SqlHistoryQuery.h SqlHistoryQuery.cpp HistoryOverviewWindow.cpp && git commit -m "feat(漏磁): PlotDataHub + DB存储 + 历史总览适配"
```

---

### Task 5: 后端类型注册 + 测试服务器 + 端到端构建

**Files:**
- Modify: `MainWindow.cpp` (后端类型下拉)
- Modify: `AppConfig.h/.cpp` (MagArray 参数, 如有需要)

- [ ] **Step 1: MainWindow 后端类型下拉新增**

```cpp
m_backendTypeCombo->addItem(QStringLiteral("漏磁检测"), "magarray");
```

- [ ] **Step 2: 创建漏磁测试服务器脚本** `magarray_grpc_test_server.py`

- [ ] **Step 3: 端到端构建验证**

```powershell
Set-Location "d:\WS\qtpro\DeviceReceiver"; .\build_cmake.bat
```

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add -A && git commit -m "feat(漏磁): 后端类型注册 + 测试服务器 + 端到端构建"
```
