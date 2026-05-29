# 漏磁检测设备 (MagArray) 设计规格

日期: 2026-05-25

## 目标

支持 `mag_array.proto` 漏磁检测设备：20 传感器 × 3 轴 = 60 通道，阵列波形图 + 热力图同窗口显示。

## 设备协议

- 服务: `MagArrayLeakageDetection`
- 帧: `MagArrayFrame` — 60 通道 `channels` + 20 传感器 `sensor_results`
- 通道公式: `channel = sensor_index * 3 + axis_index` (axis: 0=X, 1=Y, 2=Z)
- 数据字段:
  - `channels[].processed_values[]` — 扣除 baseline 后的数值
  - `sensor_results[].x_mean / y_mean / z_mean` — 每传感器均值
- 预处理参数: `PreprocessMode` (None/FixedMidpoint/AutoBaseline/SlowTrackingBaseline), `baseline_frames`, `fixed_midpoint`, `tracking_factor`
- 连接参数: `port_name`, `baudrate` (默认 1000000)

## 架构

```
mag_array.proto → GrpcMagArrayBackend → FrameData(MagArray)
                                              ↓
                         DataCacheManager / RealtimeSqlRecorder
                         PlotDataHub → PlotSnapshot
                                              ↓
                                    MagArrayWindow
                                    ├─ 波形图（3 轴 × 20 通道）
                                    └─ 热力图（3 轴 × 20 传感器）
```

### 1. FrameData 扩展

- `DetectionMode::MagArray = 4` — 漏磁实数数据
- `QVector<MagSensorResult> magSensorResults` — 20 个传感器 X/Y/Z 均值
- 60 通道放入 `channels_comp0`（每通道取 processed_values 均值或首值）

### 2. GrpcMagArrayBackend

- 实现 `IReceiverBackend`
- `connectBackend(port_name|baudrate)` — 通过 gRPC 连接串口
- `StreamFrames(include_processed_values=true)` → `MagArrayFrame` → `FrameData`
- Mock 模式：生成 20 传感器 × 3 轴合成正弦波数据
- 参数通过 `BackendParamDescriptor` 暴露（端口、波特率、预处理模式等）

### 3. MagArrayWindow（新建）

继承 `PlotWindowBase`。QSplitter 左右布局：

**左侧：阵列波形图（QScrollArea）**
- 3 个轴纵向堆叠（X → Y → Z），每个轴标题分隔
- 每轴 20 个通道子图（纵向排列），共享时间轴
- X 轴：时间 (ms)，Y 轴：信号值
- 复用 `ArrayPlotWindow` 的每通道轴矩形模式
- 横轴横向最小 200px

**右侧：热力图（QScrollArea）**
- 3 个轴纵向堆叠（X → Y → Z），每个轴一个 `QCPColorMap`
- 纵轴：传感器编号 0-19
- 横轴模式：
  - 台位模式（默认）：三轴台 X 位置 mm → `stagePose.stageXMm`
  - 时间模式：帧时间戳 ms
- 颜色：`sensor_results.{x,y,z}_mean`
- 颜色刻度：Jet 渐变色
- min/max QDoubleSpinBox 可调

**底部控件栏**
- [台位/时间] 切换按钮
- min: [spinbox] max: [spinbox]
- 三轴台连接状态标签

### 4. 数据存储

新建 `mag_array_frames` 表：
```sql
CREATE TABLE mag_array_frames (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp_unix_ms INTEGER NOT NULL,
    frame_index INTEGER NOT NULL,
    sensor_index INTEGER NOT NULL,   -- 0..19
    x_mean REAL, y_mean REAL, z_mean REAL,
    x_latest REAL, y_latest REAL, z_latest REAL,
    magnitude_mean REAL, magnitude_latest REAL
);
```

60 通道时序数据量太大，暂不存入 DB，仅存 sensor_results 用于热力图回顾。

### 5. 历史总览

`HistoryOverviewWindow` 新增 `MagArray` 分支：
- 查询 `mag_array_frames` 表
- 包络图取所有传感器 magnitude_mean 的 MIN/MAX 聚合

## 文件变更

| 操作 | 文件 | 说明 |
|---|---|---|
| 生成 | `proto/generated/mag_array.pb.*` | proto 存根 |
| 修改 | `FrameData.h` | MagArray=4, MagSensorResult 结构 |
| 新建 | `GrpcMagArrayBackend.h/.cpp` | 后端 |
| 新建 | `MagArrayWindow.h/.cpp` | 波形图+热力图窗口 |
| 修改 | `PlotWindowManager.h/.cpp` | MagArrayPlot 类型 |
| 修改 | `MainWindow.cpp` | 窗口类型下拉 |
| 修改 | `RealtimeSqlRecorder.cpp` | mag_array_frames 存储 |
| 修改 | `SqlHistoryQuery.cpp` | 查询路径 |
| 修改 | `HistoryOverviewWindow.cpp` | 包络路径 |
| 修改 | `PlotDataHub.h/.cpp` | PlotSnapshot 扩展 |
| 修改 | `CMakeLists.txt` | proto + 新文件 |
| 修改 | `AppConfig.h/.cpp` | 漏磁参数 |
