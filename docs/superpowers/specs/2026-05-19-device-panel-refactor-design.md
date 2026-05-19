# 被测设备面板重构设计

日期: 2026-05-19

## 目标

重构被测设备面板，使其紧凑、通用、可扩展，支持任意设备协议。

## 当前问题

- 模拟数据区域占空间，不常用
- 多频涡流参数硬编码在 MainWindow，添加新设备需改面板代码
- 暂停/恢复采集按钮多余（需求明确只需连接/断开）
- gRPC 自检面板与设备耦合，无法展示非 gRPC 设备状态

## 重构后面板布局

```
┌─ 被测设备连接 ──────────────────────┐
│ 数据来源: [串口|gRPC(阵列)|gRPC(多频)]│
│ 端点:     [127.0.0.1:50051        ]│
│ (串口字段仅串口模式下可见)             │
├─ gRPC 参数 ─────────────────────────┤(仅 gRPC 后端可见，proto 驱动生成)
│ 基频(Hz):  [100         ▼]         │
│ 平均周期数: [10        ▲▼]         │
│ 归一化系数: [1.0000    ▲▼]         │
│ 倍频系数:   [1,2,4,8            ]  │
├─ 被测设备采集 ───────────────────────┤
│ [连接] [断开]        ● 已连接       │
├─ 设备状态 ──────────────────────────┤
│ 设备: 多频涡流 Mock Device 001       │
│ 状态: 采集中   帧率: 10 fps         │
│ 帧数: 1256      频点数: 4          │
└────────────────────────────────────┘
```

## 架构

### 1. BackendParamDescriptor（参数描述符）

新增 `BackendParamDescriptor.h`（不依赖 UI）：

```cpp
enum ParamWidgetType { ParamInt, ParamDouble, ParamEnum, ParamIntList };

struct BackendParamDescriptor {
    QString key;              // AppConfig 键名 ("MultiFreq/BaseFrequencyHz")
    QString label;            // UI 标签 ("基频(Hz)")
    ParamWidgetType type;
    QVariant defaultValue;
    double minVal = 0, maxVal = 100, stepVal = 1;
    QStringList enumOptions;  // ParamEnum: 选项列表
};
```

每个 `IReceiverBackend` 子类提供静态方法：

```cpp
// GrpcMultiFreqBackend.h
static QVector<BackendParamDescriptor> configParameters();
```

MainWindow 通过后端类型字符串查表获取描述符（在 ApplicationController 中维护映射），动态创建控件组：

| ParamWidgetType | 控件 | AppConfig setter |
|---|---|---|
| ParamInt | QSpinBox | setValue → setXxx(int) |
| ParamDouble | QDoubleSpinBox | setXxx(double) |
| ParamEnum | QComboBox | currentText → setXxx(int/enum) |
| ParamIntList | QLineEdit | text split → setXxx(QList\<int\>) |

### 2. 设备状态面板

`IReceiverBackend` 新增信号：

```cpp
signals:
    void backendStatusChanged(const QJsonObject& status);
```

各后端在状态变化时 emit，字段自由定义：
- 阵列涡流: `{device, protocol, connected, sampling, fps, frameCount, channelCount, endpoint}`
- 多频涡流: `{device, protocol, connected, sampling, fps, frameCount, freqPointCount, endpoint}`
- 串口: `{device, protocol, connected, sampling, fps, frameCount, endpoint}`

MainWindow 接收此信号，动态更新状态面板的 QLabel 列表。

### 3. 删除项

| 删除项 | 替代 |
|---|---|
| 模拟数据 QGroupBox | Mock 模式完全删除 |
| 暂停/恢复按钮 | 仅保留连接/断开 + 状态标签 |
| 多频涡流参数硬编码 | 由 configParameters() 动态生成 |
| gRPC 自检面板 | 替换为通用设备状态面板 |

### 4. 后端类型切换流程

```
用户切换 backendTypeCombo
  → onBackendTypeChanged()
  → 更新串口字段可见性
  → 调用 m_activeBackend->configParameters()
  → 销毁旧参数控件，根据描述符重建
  → 从 AppConfig 加载参数值填入控件
  → saveConfigFromUI() 保存当前状态
```

## 文件变更

| 操作 | 文件 | 说明 |
|---|---|---|
| 新建 | `BackendParamDescriptor.h` | 参数描述符结构体 |
| 修改 | `IReceiverBackend.h` | 新增 `backendStatusChanged` 信号 |
| 修改 | `MainWindow.h` | 删除 mock/pause/resume/gRPC自检成员；新增参数控件容器、状态面板 |
| 修改 | `MainWindow.cpp` | 重写设备面板 UI；实现参数动态生成、状态面板更新 |
| 修改 | `GrpcMultiFreqBackend.h` | 新增 `configParameters()` 静态方法 |
| 修改 | `GrpcMultiFreqBackend.cpp` | emit `backendStatusChanged` |
| 修改 | `GrpcReceiverBackend.h` | 新增 `configParameters()`（无参数） |
| 修改 | `GrpcReceiverBackend.cpp` | emit `backendStatusChanged` |
| 修改 | `SerialReceiver.h` | `configParameters()` |
| 修改 | `SerialReceiver.cpp` | emit `backendStatusChanged` |
| 修改 | `AppConfig.cpp` | 移除 mockData/mockInterval 的 load/save/defaults |
| 修改 | `AppConfig.h` | 移除 mock 相关 getter/setter/members |
| 修改 | `ApplicationController.cpp` | 移除 mock 模式传递逻辑 |

## 验证

1. 编译通过，面板显示新布局
2. 切换到"gRPC（多频涡流）"→ 参数区出现基频/周期数/归一化系数/倍频系数
3. 切换到"gRPC（阵列涡流）"→ 参数区无额外参数
4. 切换到"串口"→ 串口字段出现，无参数区
5. 修改参数 → 关闭重启 → 参数保留
6. 连接设备 → 设备状态面板更新
7. 模拟数据（Mock）不再存在于 UI 中
