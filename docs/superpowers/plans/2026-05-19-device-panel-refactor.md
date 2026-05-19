# 被测设备面板重构 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重构被测设备面板：参数由 proto 驱动自动生成，删除模拟数据，设备状态通用化。

**Architecture:** 新增 BackendParamDescriptor 描述后端可配置参数，MainWindow 动态创建控件。IReceiverBackend 新增 backendStatusChanged 信号统一设备状态上报。

**Tech Stack:** C++17, Qt 5.15, QCustomPlot

---

### Task 1: 新建 BackendParamDescriptor.h

**Files:**
- Create: `BackendParamDescriptor.h`

- [ ] **Step 1: 创建描述符头文件**

```cpp
#ifndef BACKENDPARAMDESCRIPTOR_H
#define BACKENDPARAMDESCRIPTOR_H

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

enum ParamWidgetType {
    ParamInt,        // → QSpinBox
    ParamDouble,     // → QDoubleSpinBox
    ParamEnum,       // → QComboBox
    ParamIntList,    // → QLineEdit (comma-separated ints)
};

struct BackendParamDescriptor {
    QString key;              // AppConfig key, e.g. "MultiFreq/BaseFrequencyHz"
    QString label;            // UI label, e.g. "基频(Hz)"
    ParamWidgetType type;
    QVariant defaultValue;
    double minVal = 0;
    double maxVal = 100;
    double stepVal = 1;
    QStringList enumOptions;  // for ParamEnum
};

#endif // BACKENDPARAMDESCRIPTOR_H
```

- [ ] **Step 2: 添加 CMakeLists.txt 头文件列表**

In `CMakeLists.txt`, after `HEADER_FILES` block's `ApplicationController.h`, add:
```
BackendParamDescriptor.h
```

- [ ] **Step 3: 添加 include 到 IReceiverBackend.h**

Replace `#include "FrameData.h"` with:
```cpp
#include "FrameData.h"
#include <QJsonObject>
```

- [ ] **Step 4: 添加 backendStatusChanged 信号到 IReceiverBackend.h**

In `IReceiverBackend.h` signals block, after `connectionStateChanged`, add:
```cpp
/// 设备状态更新（JSON 对象，字段由各后端自由定义）
void backendStatusChanged(const QJsonObject& status);
```

- [ ] **Step 5: 添加虚方法 configParameters 到 IReceiverBackend.h**

In the `public:` section, add:
```cpp
/// 返回可配置参数描述符列表（由子类重写）。默认返回空列表。
virtual QVector<BackendParamDescriptor> configParameters() const;
```

Add inline default impl after the class:
```cpp
inline QVector<BackendParamDescriptor> IReceiverBackend::configParameters() const { return {}; }
```

Each backend ALSO provides a static version for querying without instantiation:
```cpp
// GrpcMultiFreqBackend.h
static QVector<BackendParamDescriptor> configParameters();
```

---

### Task 2: 各后端实现 configParameters + 状态上报

**Files:**
- Modify: `GrpcMultiFreqBackend.h`, `GrpcMultiFreqBackend.cpp`
- Modify: `GrpcReceiverBackend.h`, `GrpcReceiverBackend.cpp`
- Modify: `SerialReceiver.h`, `SerialReceiver.cpp`

- [ ] **Step 1: GrpcMultiFreqBackend::configParameters() (static)**

In `GrpcMultiFreqBackend.h` public section:
```cpp
static QVector<BackendParamDescriptor> configParameters();
QVector<BackendParamDescriptor> configParameters() const override { return configParameters(); }
```

In `GrpcMultiFreqBackend.cpp`:
```cpp
#include "BackendParamDescriptor.h"

QVector<BackendParamDescriptor> GrpcMultiFreqBackend::configParameters() const
{
    return {
        {"MultiFreq/BaseFrequencyHz",  QStringLiteral("基频(Hz)"), ParamEnum,
         QVariant(100), 0, 0, 1, {"1","2","5","10","20","50","100","200","500","1000"}},
        {"MultiFreq/AverageCycleCount", QStringLiteral("平均周期数"), ParamInt,
         QVariant(10), 1, 1000, 1, {}},
        {"MultiFreq/NormalizeScale", QStringLiteral("归一化系数"), ParamDouble,
         QVariant(1.0), 0.01, 100.0, 0.1, {}},
        {"MultiFreq/FrequencyFactors", QStringLiteral("倍频系数"), ParamIntList,
         QVariant(QString("1,2,4,8")), 0, 0, 0, {}},
    };
}
```

- [ ] **Step 2: GrpcMultiFreqBackend 发送 backendStatusChanged**

Add `#include <QJsonObject>` to `GrpcMultiFreqBackend.cpp`.

In `GrpcMultiFreqBackend` private section (header), add method:
```cpp
void emitDeviceStatus();
```

In `GrpcMultiFreqBackend.cpp`:
```cpp
void GrpcMultiFreqBackend::emitDeviceStatus()
{
    QJsonObject s;
    s["protocol"] = QStringLiteral("multifreq-grpc");
    s["endpoint"] = m_endpoint;
    s["mock"] = m_mockMode.load();
    emit backendStatusChanged(s);
}
```

Call `emitDeviceStatus()` at end of:
- `connectBackend()` after `setConnected(true)`
- `disconnectBackend()` after `setConnected(false)`
- `startAcquisition()` after mock timer start or stream thread start
- `stopAcquisition()` after stopping

- [ ] **Step 3: GrpcReceiverBackend::configParameters() (static)**

In `GrpcReceiverBackend.h` public section:
```cpp
static QVector<BackendParamDescriptor> configParameters() { return {}; }
QVector<BackendParamDescriptor> configParameters() const override { return configParameters(); }
```

No .cpp changes needed (inline in header).

- [ ] **Step 4: GrpcReceiverBackend 发送 backendStatusChanged**

In `GrpcReceiverBackend.h` private section, add `void emitDeviceStatus();`.

In `GrpcReceiverBackend.cpp`, implement similarly to MultiFreq but with `"protocol": "grpc"`:
```cpp
void GrpcReceiverBackend::emitDeviceStatus()
{
    QJsonObject s;
    s["protocol"] = QStringLiteral("grpc");
    s["endpoint"] = m_endpoint;
    s["mock"] = m_mockMode.load();
    emit backendStatusChanged(s);
}
```

Call at same connection/disconnection/acquisition points.

- [ ] **Step 5: SerialReceiver::configParameters() + status**

In `SerialReceiver.h`, add:
```cpp
static QVector<BackendParamDescriptor> configParameters() { return {}; }
QVector<BackendParamDescriptor> configParameters() const override { return configParameters(); }
void emitDeviceStatus();
```

In `SerialReceiver.cpp`:
```cpp
void SerialReceiver::emitDeviceStatus()
{
    QJsonObject s;
    s["protocol"] = QStringLiteral("serial");
    s["port"] = m_portName;
    emit backendStatusChanged(s);
}
```

Call at connect/disconnect points.

---

### Task 3: AppConfig 删除 Mock 相关

**Files:**
- Modify: `AppConfig.h`, `AppConfig.cpp`

- [ ] **Step 1: AppConfig.h 删除 mock getter/setter**

Remove these lines from public section:
```cpp
bool useMockData() const { return m_useMockData; }
void setUseMockData(bool use) { m_useMockData = use; }
int mockDataIntervalMs() const { return m_mockDataIntervalMs; }
void setMockDataIntervalMs(int ms) { m_mockDataIntervalMs = ms; }
```

Remove from private members:
```cpp
bool m_useMockData = false;
int m_mockDataIntervalMs = 100;
```

- [ ] **Step 2: AppConfig.cpp 删除 mock load/save/defaults**

In `loadFromFile()`, remove lines around 470-480 that read `Receiver/UseMockData` and `Receiver/MockDataIntervalMs`.

In `saveToFile()`, remove lines that write `Receiver/UseMockData` and `Receiver/MockDataIntervalMs`.

In `loadDefaults()`, remove:
```cpp
m_useMockData = false;
m_mockDataIntervalMs = 100;
```

---

### Task 4: ApplicationController 删除 Mock 逻辑

**Files:**
- Modify: `ApplicationController.cpp`, `ApplicationController.h`

- [ ] **Step 1: 删除 m_config.useMockData 和 m_config.mockDataIntervalMs**

In `ApplicationController.h`, remove from struct `m_config`:
```cpp
bool useMockData = false;
int mockDataIntervalMs = 100;
```

- [ ] **Step 2: reloadRuntimeConfig 删除 mock 读取**

In `ApplicationController.cpp::reloadRuntimeConfig()`, remove:
```cpp
m_config.useMockData = config->useMockData();
m_config.mockDataIntervalMs = config->mockDataIntervalMs();
```

- [ ] **Step 3: startReceiver 删除 mock 分支**

In `startReceiver()`, around line 285-300: remove the `setMockMode` invoke and the `if (m_config.useMockData)` branch. Simplify to always start real acquisition:
```cpp
QMetaObject::invokeMethod(m_serialReceiver.get(), "startAcquisition",
                          Qt::QueuedConnection,
                          Q_ARG(int, m_config.mockDataIntervalMs));
// → change to use a fixed interval:
QMetaObject::invokeMethod(m_serialReceiver.get(), "startAcquisition",
                          Qt::QueuedConnection,
                          Q_ARG(int, 100));
```

- [ ] **Step 4: 删除重连时的 mock fallback 对话框**

In the gRPC connect/reconnect flow (around lines 320-340 and 780-800), remove the mock fallback dialog (the "使用模拟数据" button in the QMessageBox). Just show the error and stop.

- [ ] **Step 5: 删除 setMockMode 调用**

Remove all `setMockMode` invocations throughout ApplicationController.cpp.

---

### Task 5: MainWindow.h 成员变量重组

**Files:**
- Modify: `MainWindow.h`

- [ ] **Step 1: 删除 Mock 相关成员**

Remove:
```cpp
QCheckBox* m_useMockDataCheck;
QSpinBox* m_mockIntervalSpin;
```

- [ ] **Step 2: 删除暂停/恢复按钮**

Remove:
```cpp
QPushButton* m_pauseButton;
QPushButton* m_resumeButton;
```

- [ ] **Step 3: 删除多频涡流硬编码参数控件**

Remove:
```cpp
QGroupBox* m_multifreqGroupBox = nullptr;
QComboBox* m_mfBaseFreqCombo = nullptr;
QSpinBox* m_mfAvgCycleSpin = nullptr;
QDoubleSpinBox* m_mfNormScaleSpin = nullptr;
QLineEdit* m_mfFreqFactorsEdit = nullptr;
```

- [ ] **Step 4: 删除 gRPC 自检面板所有成员**

Remove all 30+ gRPC self-test members (m_grpcTestGroup through m_grpcTestServerStopRequested).

- [ ] **Step 5: 删除 gRPC 测试服务器相关成员**

Remove all gRPC test server process-related members (m_grpcTestServerProcess through m_grpcTestServerStopRequested).

- [ ] **Step 6: 新增动态参数区域成员**

Add:
```cpp
// gRPC 参数区域（动态生成）
QGroupBox* m_grpcParamGroup = nullptr;
QFormLayout* m_grpcParamLayout = nullptr;
QVector<QWidget*> m_grpcParamWidgets;  // 当前参数控件
QVector<BackendParamDescriptor> m_currentBackendParams;  // 当前参数描述符

// 设备状态面板
QGroupBox* m_deviceStatusGroup = nullptr;
QVBoxLayout* m_deviceStatusLayout = nullptr;
QLabel* m_deviceStatusDeviceLabel = nullptr;
QLabel* m_deviceStatusProtocolLabel = nullptr;
QLabel* m_deviceStatusStateLabel = nullptr;
QLabel* m_deviceStatusEndpointLabel = nullptr;
QLabel* m_deviceStatusDetailsLabel = nullptr;
```

---

### Task 6: MainWindow.cpp 重写设备面板

**Files:**
- Modify: `MainWindow.cpp`

- [ ] **Step 1: 删除 Mock 组 UI 构造**

Remove lines creating `mockGroup` QGroupBox with `m_useMockDataCheck` and `m_mockIntervalSpin` (approx lines 555-569).

- [ ] **Step 2: 删除多频涡流参数硬编码 UI**

Remove lines creating `m_multifreqGroupBox` with all its contents (approx lines 571-598).

- [ ] **Step 3: 删除暂停/恢复按钮**

In controlGroup creation, remove `m_pauseButton` and `m_resumeButton` from controlRow1Layout and controlRow2Layout. Remove the enable/disable logic.

- [ ] **Step 4: 删除 gRPC 自检面板 UI**

Remove the entire `grpcTestGroup` QGroupBox creation and all its contents (approx lines 637-683+).

- [ ] **Step 5: 新增动态 gRPC 参数组**

After serialGroup and before controlGroup, add:
```cpp
// gRPC 参数组（根据后端类型动态生成）
m_grpcParamGroup = new QGroupBox(QStringLiteral("gRPC 参数"));
m_grpcParamLayout = new QFormLayout(m_grpcParamGroup);
m_grpcParamGroup->setVisible(false);
deviceLayout->addWidget(m_grpcParamGroup);
```

- [ ] **Step 6: 新增设备状态面板**

After controlGroup, add:
```cpp
// 设备状态面板
m_deviceStatusGroup = new QGroupBox(QStringLiteral("设备状态"));
m_deviceStatusLayout = new QVBoxLayout(m_deviceStatusGroup);
m_deviceStatusDeviceLabel = new QLabel(QStringLiteral("未连接"));
m_deviceStatusProtocolLabel = new QLabel();
m_deviceStatusStateLabel = new QLabel();
m_deviceStatusEndpointLabel = new QLabel();
m_deviceStatusDetailsLabel = new QLabel();
m_deviceStatusLayout->addWidget(m_deviceStatusDeviceLabel);
m_deviceStatusLayout->addWidget(m_deviceStatusProtocolLabel);
m_deviceStatusLayout->addWidget(m_deviceStatusStateLabel);
m_deviceStatusLayout->addWidget(m_deviceStatusEndpointLabel);
m_deviceStatusLayout->addWidget(m_deviceStatusDetailsLabel);
deviceLayout->addWidget(m_deviceStatusGroup);
```

- [ ] **Step 7: 实现 rebuildGrpcParamUI()**

Add method:
```cpp
void MainWindow::rebuildGrpcParamUI(const QVector<BackendParamDescriptor>& params)
{
    m_currentBackendParams = params;

    // 销毁旧控件
    for (QWidget* w : m_grpcParamWidgets) {
        m_grpcParamLayout->removeWidget(w);
        w->deleteLater();
    }
    m_grpcParamWidgets.clear();

    // 清除 form layout 中的旧行
    while (m_grpcParamLayout->rowCount() > 0) {
        m_grpcParamLayout->removeRow(0);
    }

    if (params.isEmpty()) {
        m_grpcParamGroup->setVisible(false);
        return;
    }

    for (const auto& p : params) {
        QWidget* w = nullptr;
        switch (p.type) {
        case ParamInt: {
            auto* spin = new QSpinBox(m_grpcParamGroup);
            spin->setRange((int)p.minVal, (int)p.maxVal);
            spin->setSingleStep((int)p.stepVal);
            spin->setValue(p.defaultValue.toInt());
            connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                    this, [this]() { saveConfigFromUI(); });
            w = spin;
            break;
        }
        case ParamDouble: {
            auto* spin = new QDoubleSpinBox(m_grpcParamGroup);
            spin->setRange(p.minVal, p.maxVal);
            spin->setSingleStep(p.stepVal);
            spin->setDecimals(4);
            spin->setValue(p.defaultValue.toDouble());
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                    this, [this]() { saveConfigFromUI(); });
            w = spin;
            break;
        }
        case ParamEnum: {
            auto* combo = new QComboBox(m_grpcParamGroup);
            combo->addItems(p.enumOptions);
            combo->setCurrentText(p.defaultValue.toString());
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this]() { saveConfigFromUI(); });
            w = combo;
            break;
        }
        case ParamIntList: {
            auto* edit = new QLineEdit(m_grpcParamGroup);
            edit->setText(p.defaultValue.toString());
            edit->setPlaceholderText(QStringLiteral("逗号分隔整数"));
            connect(edit, &QLineEdit::textChanged,
                    this, [this]() { saveConfigFromUI(); });
            w = edit;
            break;
        }
        }
        if (w) {
            m_grpcParamLayout->addRow(p.label + ":", w);
            m_grpcParamWidgets.append(w);
        }
    }
    m_grpcParamGroup->setVisible(true);
}
```

- [ ] **Step 8: 更新 onBackendTypeChanged**

Rewrite to:
```cpp
void MainWindow::onBackendTypeChanged(int index)
{
    Q_UNUSED(index)
    const QString backendType = m_backendTypeCombo->currentData().toString();
    const bool isGrpc = (backendType.compare("grpc", Qt::CaseInsensitive) == 0);
    const bool isMultiFreq = (backendType.compare("multifreq-grpc", Qt::CaseInsensitive) == 0);
    const bool isGrpcLike = isGrpc || isMultiFreq;

    m_grpcEndpointEdit->setEnabled(isGrpcLike);

    for (QWidget* field : m_serialOnlyFields) {
        if (field) { field->setVisible(!isGrpcLike); field->setEnabled(!isGrpcLike); }
    }
    for (QWidget* label : m_serialOnlyLabels) {
        if (label) label->setVisible(!isGrpcLike);
    }

    if (!isGrpcLike) updateSerialPortList();

    // 重建 gRPC 参数 UI
    QVector<BackendParamDescriptor> params;
    if (isGrpc) {
        params = GrpcReceiverBackend::configParameters();
    } else if (isMultiFreq) {
        params = GrpcMultiFreqBackend::configParameters();
    }
    rebuildGrpcParamUI(params);

    saveConfigFromUI();
    m_appController->applyReceiverBackendFromConfig();
}
```

- [ ] **Step 9: 更新 loadConfigToUI 中 gRPC 参数加载**

Replace the hardcoded multifreq config loading with dynamic version:
```cpp
// 加载 gRPC 参数（根据当前描述符列表）
for (int i = 0; i < m_currentBackendParams.size() && i < m_grpcParamWidgets.size(); ++i) {
    const auto& p = m_currentBackendParams[i];
    QWidget* w = m_grpcParamWidgets[i];
    switch (p.type) {
    case ParamInt: {
        auto* spin = qobject_cast<QSpinBox*>(w);
        if (spin) spin->setValue(configValue(p.key, p.defaultValue).toInt());
        break;
    }
    case ParamDouble: {
        auto* spin = qobject_cast<QDoubleSpinBox*>(w);
        if (spin) spin->setValue(configValue(p.key, p.defaultValue).toDouble());
        break;
    }
    case ParamEnum: {
        auto* combo = qobject_cast<QComboBox*>(w);
        if (combo) combo->setCurrentText(configValue(p.key, p.defaultValue).toString());
        break;
    }
    case ParamIntList: {
        auto* edit = qobject_cast<QLineEdit*>(w);
        if (edit) edit->setText(configValue(p.key, p.defaultValue).toString());
        break;
    }
    }
}
```

Helper method `configValue(key, fallback)` reads AppConfig by key:
```cpp
QVariant MainWindow::configValue(const QString& key, const QVariant& fallback) const
{
    AppConfig* cfg = AppConfig::instance();
    if (!cfg) return fallback;
    // Key format: "Section/Field", e.g. "MultiFreq/BaseFrequencyHz"
    const int slash = key.indexOf('/');
    if (slash < 0) return fallback;
    const QString section = key.left(slash);
    const QString field = key.mid(slash + 1);
    // Use QSettings directly to read by section/key
    QSettings settings(AppConfig::defaultConfigFilePath(), QSettings::IniFormat);
    return settings.value(section + "/" + field, fallback);
}
```

- [ ] **Step 10: 更新 saveConfigFromUI 中 gRPC 参数保存**

Replace hardcoded multifreq save with dynamic:
```cpp
for (int i = 0; i < m_currentBackendParams.size() && i < m_grpcParamWidgets.size(); ++i) {
    const auto& p = m_currentBackendParams[i];
    QWidget* w = m_grpcParamWidgets[i];
    switch (p.type) {
    case ParamInt:
        setConfigValue(p.key, qobject_cast<QSpinBox*>(w)->value());
        break;
    case ParamDouble:
        setConfigValue(p.key, qobject_cast<QDoubleSpinBox*>(w)->value());
        break;
    case ParamEnum:
        setConfigValue(p.key, qobject_cast<QComboBox*>(w)->currentText().toInt());
        break;
    case ParamIntList: {
        auto* edit = qobject_cast<QLineEdit*>(w);
        if (edit) setConfigValue(p.key, edit->text());
        break;
    }
    }
}
```

Helper `setConfigValue(key, value)`:
```cpp
void MainWindow::setConfigValue(const QString& key, const QVariant& value)
{
    AppConfig* cfg = AppConfig::instance();
    if (!cfg) return;
    // Map to specific setter based on key
    if (key == "MultiFreq/BaseFrequencyHz") cfg->setMultiFreqBaseFrequencyHz(value.toInt());
    else if (key == "MultiFreq/AverageCycleCount") cfg->setMultiFreqAverageCycleCount(value.toInt());
    else if (key == "MultiFreq/NormalizeScale") cfg->setMultiFreqNormalizeScale(value.toDouble());
    else if (key == "MultiFreq/FrequencyFactors") {
        QList<int> factors;
        for (const QString& p : value.toString().split(',', Qt::SkipEmptyParts)) {
            bool ok; int f = p.trimmed().toInt(&ok);
            if (ok && f > 0) factors.append(f);
        }
        if (!factors.isEmpty()) cfg->setMultiFreqFrequencyFactors(factors);
    }
}
```

- [ ] **Step 11: 实现设备状态更新槽**

Connect in connectSignals for gRPC-like backends:
```cpp
connect(m_appController->m_serialReceiver.get(), &IReceiverBackend::backendStatusChanged,
        this, &MainWindow::onBackendStatusChanged);
```

In MainWindow:
```cpp
void MainWindow::onBackendStatusChanged(const QJsonObject& s)
{
    m_deviceStatusDeviceLabel->setText(
        QStringLiteral("协议: %1").arg(s["protocol"].toString()));
    m_deviceStatusStateLabel->setText(
        QStringLiteral("状态: %1").arg(s.value("mock").toBool() ? QStringLiteral("模拟") : QStringLiteral("就绪")));
    m_deviceStatusEndpointLabel->setText(
        QStringLiteral("端点: %1").arg(s["endpoint"].toString()));
    m_deviceStatusDetailsLabel->setText(
        QStringLiteral(""));
}
```

---

### Task 7: 清理并编译

**Files:**
- Modify: `MainWindow.cpp` (remove unused includes and method bodies)

- [ ] **Step 1: 删除 MainWindow.cpp 中 gRPC 自检相关方法体**

Remove all method implementations for:
- `onStartGrpcTestServerClicked`, `onStopGrpcTestServerClicked`
- `onRunGrpcSelfTestClicked`, `onGrpcSelfTestTimeout`
- `resetGrpcSelfTestLabelStates`, `applyGrpcSelfTestLabelStates`
- `onGrpcModeComboChanged`, all gRPC test server launch helpers
- `startGrpcTestServerWithFallback`, `onGrpcTestServerProcessFinished`, etc.

- [ ] **Step 2: 删除 disconnectSignals 中自检相关 disconnect**

- [ ] **Step 3: 构建验证**

```powershell
Set-Location "d:\WS\qtpro\DeviceReceiver"; .\build_cmake.bat
```

Expected: 编译通过，无报错。

- [ ] **Step 4: 功能验证**

1. 启动主程序，选择 "gRPC（多频涡流）"→ gRPC 参数区显示四个控件
2. 选择 "gRPC（被测设备数据）"→ gRPC 参数区消失
3. 选择 "串口"→ 串口字段出现
4. 修改参数 → 关闭重启 → 参数保留
5. Mock 复选框/暂停恢复按钮/gRPC自检面板 不再存在
