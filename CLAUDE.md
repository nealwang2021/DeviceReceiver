# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概览

- **项目名称：** DeviceReceiver
- **一句话描述：** 通用的测试台软件，收集设备数据实时可视化，离线可视化。
- **主要技术栈：** C++17、Qt 5.15、CMake，可选 Python gRPC 模拟服务。
- **主要环境：** Windows + MSVC 2019 + Qt 5.15.2 + vcpkg（gRPC、HDF5、protobuf）。
- **第三方库：** QCustomPlot v2.1 + OpenGL（freeglut）、spdlog（FetchContent v1.14.1）。
- **构建约定：** 统一使用 `build_cmake.bat`，不要使用 qmake 或其它构建脚本。

## 命令入口

### 快速构建（Windows，默认）

```powershell
cmd /c "cd /d d:\WS\qtpro\DeviceReceiver && build_cmake.bat"
```

默认 `-Fast` 模式：仅构建主程序（gRPC+HDF5 启用），跳过测试目标与 windeployqt。

常用参数：

| 参数 | 作用 |
|------|------|
| `-Debug` | 构建 Debug 版本 |
| `-Run` | 构建成功后启动程序 |
| `-Clean` | 清理构建目录 |
| `-NoGrpc` / `-NoHdf5` | 禁用 gRPC / HDF5 |
| `-All` / `-Full` | 全量模式：构建全部目标 + windeployqt + DLL 复制 |
| `-Rebuild` | 等价 `-Clean -All` |
| `-VcpkgRoot <path>` | 指定 vcpkg 根目录 |
| `-VSVersion 2022` | 指定 VS 版本 |

输出：`build_cmake/build/release/realtime_data.exe`（Debug 则为 `debug`）。

### Linux 构建（无 gRPC/HDF5）

```bash
mkdir -p build_linux && cd build_linux
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
  -DENABLE_GRPC=OFF -DENABLE_HDF5=OFF -DENABLE_WASM=OFF -DBUILD_TESTS=OFF
cmake --build . --config Release -j$(nproc)
```

必须传 `-DCMAKE_CXX_COMPILER=g++`（Clang 缺少 `-lstdc++` 无法链接）。

### CMake 关键选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `ENABLE_GRPC` | ON | gRPC 客户端支持（需 vcpkg protobuf v6.x） |
| `ENABLE_HDF5` | ON | HDF5 导出支持 |
| `ENABLE_WASM` | OFF | WebAssembly 构建 |
| `BUILD_TESTS` | ON | 三轴台 Qt Test（需 gRPC 启用） |

### 测试

仅限三轴台集成测试（`tests/`，需 `ENABLE_GRPC=ON`）：

```powershell
ctest --test-dir build_cmake --output-on-failure
```

两个测试目标：`tst_stage_integration`（StageReceiverBackend 独立测试）、`tst_stage_panel`（完整应用面板测试）。

### Lint

```powershell
clang-format --dry-run --Werror <changed_files...>
```

## 架构

### 数据流（核心管线）

```
设备/模拟器 → IReceiverBackend → DataCacheManager → RealtimeSqlRecorder (SQLite)
                    ↓                                      ↑
              FrameData 信号                        按需查询
                    ↓
            PlotWindowManager (定时轮询)
                    ↓
              PlotDataHub (聚合为 PlotSnapshot)
                    ↓
         PlotWindowBase 子类 (渲染到 QCustomPlot)
```

### 分层与核心类

**应用层：**
- [ApplicationController](ApplicationController.h) — 应用生命周期管理、模块初始化和协调。持有所有核心模块实例，负责后端类型切换、三轴台独立后端管理。
- [MainWindow](MainWindow.h) — 主界面，MDI + 浮动面板布局。设备控制面板、指令发送、窗口管理、数据监控、历史总览、gRPC 自检面板。
- [AppConfig](AppConfig.h) — 单例配置管理。从与 exe 同目录的 `config.ini` 加载/保存。管理串口、gRPC、绘图、UI 布局、导出等所有持久化设置。

**设备通信层（IReceiverBackend 多态）：**
- [IReceiverBackend](IReceiverBackend.h) — 统一后端接口：`connectBackend`、`disconnectBackend`、`startAcquisition`、`sendCommand`、`frameReceived` 信号。
- [SerialReceiver](SerialReceiver.h) — 串口后端。固定帧格式解析（AA55 帧头，22 字节帧），支持模拟数据定时器。
- [GrpcReceiverBackend](GrpcReceiverBackend.h) — gRPC 后端。Mock 模式用 QTimer 生成伪随机帧，Real 模式用独立 `std::thread` 阻塞读流（SubscribeProcessedFrames），通过 `Qt::QueuedConnection` 回传。状态变量为 `std::atomic<bool>` 保证跨线程安全。
- [StageReceiverBackend](StageReceiverBackend.h) — 三轴台测试装置独立后端，与被测设备数据通道分离。

**数据管线：**
- [FrameData](FrameData.h) — 核心数据结构。包含 timestamp、frameId、channelCount、多分量通道数据（comp0/comp1/amp/phase/x/y）、三轴台位姿、检测模式枚举（Legacy/MultiChannelReal/MultiChannelComplex）。已注册 Qt 元类型，可跨线程信号槽传递。
- [DataCacheManager](DataCacheManager.h) — 单例，线程安全环形缓存（QReadWriteLock）。最大 10000 帧，支持时间范围查询与过期清理。
- [PlotDataHub](PlotDataHub.h) — 单例，将 FrameData 批量聚合为 PlotSnapshot（按检测模式和通道数拆解时间序列），供各窗口零拷贝读取。
- [DataProcessor](DataProcessor.h) — 1Hz 定时统计（均值/最值/标准差/报警）。

**持久化层：**
- [RealtimeSqlRecorder](RealtimeSqlRecorder.h) — 实时 SQLite 写入。独立 worker 线程，批量写入（每 500 帧或 20ms flush 一次）。自动轮换：超过 24h 保留时长或 1GB 文件上限时切新文件。40 通道对齐宽表 `aligned_frames`。
- [HistoryDataProvider](HistoryDataProvider.h) — 只读连接提供 raw chunk 查询接口，异步加载 + BusyOverlay。
- [HistoryOverviewWindow](HistoryOverviewWindow.h) — 历史总览 dock，支持时间窗包络 + brush 选区，Live/Review 模式切换。
- [HistoryExportService](HistoryExportService.h) / [HistoryImportService](HistoryImportService.h) — HDF5/SQL 导入导出。

**绘图层（PlotWindowBase 继承体系）：**
- [PlotWindowBase](PlotWindowBase.h) — 抽象基类，接口：`onDataUpdated(QVector<FrameData>)`、`onCriticalFrame(FrameData)`、`onPlotSnapshotUpdated(PlotSnapshot)`。管理 OpenGL/主题/性能档。
- [PlotWindow](PlotWindow.h) — 基础时序折线图（组合图）。
- [HeatMapPlotWindow](HeatMapPlotWindow.h) — 热力图（ColorMap）。
- [ArrayPlotWindow](ArrayPlotWindow.h) — 阵列图（多通道并行子图）。
- [ArrayRgbHeatmapWindow](ArrayRgbHeatmapWindow.h) — 阵列 RGB 热力图。
- [PulsedDecayPlotWindow](PulsedDecayPlotWindow.h) — 脉冲衰减图。
- [InspectionPlotWindow](InspectionPlotWindow.h) — 检测分析窗口（分组视图）。
- [MagArrayWindow](MagArrayWindow.h) — 漏磁检测窗口（MDI 子窗口）。左右分割布局：左侧 3 行 QCPLayoutGrid 波形图（折线），右侧 3 行 QCPLayoutGrid 热力图（ColorMap + ColorScale）。顶部单行控制栏：最大帧数、XYZ 轴 CheckBox 显隐、热力图 X 轴时间/台位模式、色标范围 + 双色渐变颜色选择器。所有控件状态持久化到 `[MagArray]` 配置节。[[magarray-layout-lessons]] [[magarray-restore-bug]]
- [PlotWindowManager](PlotWindowManager.h) — 单例，统一管理所有绘图窗口。定时轮询 DataCacheManager，经 PlotDataHub 聚合后广播 `dataUpdated` / `plotSnapshotUpdated` 信号。`PlotType` 枚举包含 `MagArrayPlot = 10`，工厂方法 `createWindow()` 按类型创建窗口实例。**注意：** 新增 `PlotType` 值时必须同步更新 `MainWindow::restoreSavedPlotWindowsFromConfig()` 和 `ApplicationController::normalizeStoredPlotType()` 中的类型归一化函数。[[magarray-restore-bug]]

**跨线程模型：**
- 设备 I/O 线程（QThread）运行 SerialReceiver / GrpcReceiverBackend 流线程，通过 `Qt::QueuedConnection` 信号将 FrameData 投递到主线程。
- RealtimeSqlRecorder 使用独立 QThread + worker 对象，帧入队（QMutex 保护队列）后由 worker 批量写入。
- PlotWindowManager 定时器在主线程触发，从 DataCacheManager（QReadWriteLock）读取后分发。

## 不可违反约束

1. 设备通信层（`*Receiver*`、`*Backend*`）不得依赖 UI 层（`MainWindow`、`*Window*`）。
2. 实时采集线程与 UI 渲染线程必须隔离，跨线程通信只能用信号槽或线程安全队列。
3. 原始数据持久化格式必须向后兼容，读取历史文件不得崩溃。
4. 任何阻塞 I/O 不得在主 UI 线程执行。

## 配置系统

`config.ini` 位于可执行文件同目录（通过 `AppConfig::defaultConfigFilePath()` 定位）。各节组织：
- `[General]` — 应用标题、后端类型、gRPC 端点、日志级别
- `[SerialPort]` — 串口参数
- `[Plot]` — 绘图点数、刷新间隔、OpenGL 开关、阵列图行高
- `[MagArray]` — 漏磁检测窗口配置：最大帧数(`MaxFrames`)、轴显隐(`AxisX/Y/ZVisible`)、热力图X轴模式(`HeatmapXAxisMode`)、色标范围(`ColorDataMin/Max`)、渐变色(`GradientColorMin/Max`)
- `[UI]` — 面板显隐、窗口状态/几何、指令历史
- `[Export]` — 导出目录与格式

## Python gRPC 测试服务

```bash
# 设备数据服务 (port 50051)
python grpc_test_server.py --port 50051

# 三轴台服务 (port 50052)
python stage_grpc_test_server.py --port 50052
```

若缺 `stage_pb2` 存根：
```bash
python -m grpc_tools.protoc -Iproto --python_out=proto/generated_py --grpc_python_out=proto/generated_py proto/stage.proto
```

## 已知陷阱

- 默认 C++ 编译器若为 Clang 18 会因缺少 `-lstdc++` 链接失败，Linux 上始终传 `-DCMAKE_CXX_COMPILER=g++`。
- `tests/` 在 Linux 上不存在（`BUILD_TESTS=OFF` 必须）。
- QCustomPlot `HighQualityAntialiasing` 弃用警告来自 vendor 库，可忽略。
- gRPC C++ 的 proto 生成文件（`proto/generated/`）针对 protobuf v6.x（Windows vcpkg），与 Ubuntu 系统 protobuf v3.21.x 不兼容，Linux 上须 `-DENABLE_GRPC=OFF`。
- `config.ini` 不存在时应用使用内存默认值启动，正常退出时写出。
- QCPAxisRect 构造函数硬编码了 `setMinimumSize(50, 50)`（qcustomplot.cpp:17645）。隐藏轴矩形时必须同时调用 `setMinimumSize(0, 0)`，否则 `QCPLayoutGrid::getSectionSizes` 的 minimum violation check 会将已折叠的行推回 50px。恢复可见时需设置回 `setMinimumSize(50, 50)`。[[magarray-layout-lessons]]
- QCPLayoutGrid 的 `setRowStretchFactor(row, 0)` 是无效操作（要求 `factor > 0`），静默忽略。列宽 max 取跨行最小值（一个隐藏元素拖跨整列），行高 max 各行独立。[[magarray-layout-lessons]]

## 新增 gRPC 设备后端检查清单

每次新增 proto 文件并实现 `Grpc*Backend` 时，**必须**逐项确认（漏磁检测首次实现时全部踩坑）：

1. **connectBackend 后是否需要获取设备列表？** — 若服务端管理多个子设备/串口（如 `ListSerialPorts`、`ListDevices`），必须在 `connectBackend` 中调用该 RPC，并将结果通过信号传给 UI。参考 `GrpcMagArrayBackend::connectBackend` → `availableSerialPortsChanged`。
2. **connectBackend 后是否要自动 startAcquisition？** — 若需用户先选择子设备或配置参数，跳过自动采集。参考 `ApplicationController::handleGrpcConnectAttemptFinished` 中 `isMagArray` 判断。
3. **启动 RPC（StartDetection/StartSampling/...）是否需要用户配置参数？** — 若是，参数必须通过 `configParameters()` 返回 `BackendParamDescriptor` 列表；`streamLoop` 中通过 `QSettings` 读取，不得硬编码。
4. **参数是否需要持久化？** — 非标准参数（动态下拉框等）需在 `MainWindow::setConfigValue` 中添加处理或走通用 `QSettings` 回退，否则重启丢失。
5. **是否需要后端专属 UI 控件？** — 标准参数用 `BackendParamDescriptor` 自动生成；动态列表（如设备串口下拉框）需手动添加 `QComboBox`，在 `onBackendTypeChanged` 中控制显隐。参考 `m_magArrayPortGroup`。
6. **参数是否需要按条件显隐？** — 若参数之间存在依赖（如预处理模式决定哪些参数生效），需在 `rebuildGrpcParamUI` 后调用条件显隐更新，切换模式时同步刷新。参考 `updateMagArrayConditionalVisibility()`。

## 漏磁检测集成踩坑实录（2026-06）

### 信号连接时序
- **陷阱：** `initReceiverBackend()` 在 `initMainWindow()` 之前执行，此时 `m_mainWindow` 为 `null`。在此处连接 Backend → MainWindow 的信号会被 `if (m_mainWindow)` 跳过。
- **修复：** 将 UI 相关信号连接移至 `connectReceiverToMainWindow()`（在 `initMainWindow()` 末尾调用）。

### 配置持久化
- **陷阱 1：** `saveConfigFromUI` 在 `onBackendTypeChanged` 中于 `rebuildGrpcParamUI` 之后调用，此时控件是默认值，会覆盖已保存配置。
- **修复 1：** 改为在 `rebuildGrpcParamUI` 之后从 `QSettings` 恢复已保存值，不在此处保存。
- **陷阱 2：** `saveConfigFromUI` 不检查 `m_suppressConfigPersist` 标志，初始化期间填充默认值时触发保存。
- **修复 2：** 顶部加 `if (m_suppressConfigPersist) return;`。
- **陷阱 3：** `ParamEnum` 保存用 `currentText().toInt()`，"AutoBaseline"→0→永远读回0。加载用 `setCurrentText("0")` 匹配不到。
- **修复 3：** 改为 `currentIndex()` 保存 / `setCurrentIndex()` 加载，且枚举选项必须与 proto 定义一一对应。

### 数据数组索引映射
- **陷阱：** `channels_comp0` 按 proto 顺序排列 `ch = sensor×3 + axis`，但波形图的 graph 按轴分块排列 `graph[axis×20 + sensor]`。直接 `realAmp[i] → graph[i]` 导致全图错位。
- **修复：** `sensorIdx = i % 20; axisIdx = i / 20; dataIdx = sensorIdx * 3 + axisIdx`。

### QScrollArea 内容不可见
- **陷阱：** `QScrollArea` 默认 `widgetResizable = false`，内部容器不会随新添加的 widget 自动扩展。
- **修复：** 创建后立即调用 `setWidgetResizable(true)`，添加内容后调用容器 `adjustSize()`。

### Proto 枚举与 UI Combo 偏移
- **陷阱：** ComboBox 选项漏掉 `Unspecified(0)`，导致 combo index 与 proto enum value 整体偏移 1 位。
- **修复：** 补全 `Unspecified` 使 index == proto value。

## 脉冲涡流集成踩坑实录（2026-06）

### 数据帧体积与 PlotDataHub
- **陷阱：** PulseFrame 每帧 ~64k double（512KB），不能走 PlotSnapshot 聚合（OOM）。
- **修复：** `onDataUpdated` 直取最后一帧，PlotDataHub 仅记时间戳不存数据。

### 64k 数据存储
- **陷阱：** 512KB/帧不能拆成单值列存 SQLite（64000 列 × N 行不可行）。
- **修复：** `QByteArray(reinterpret_cast<const double*>(data), n*sizeof(double))` 序列化为 BLOB，读写零拷贝。

### 历史包络图聚合
- **陷阱：** BLOB 列无法 SQL 聚合（MIN/MAX）。
- **修复：** 入库时 `std::minmax_element` 预计算 `min_value` / `max_value` 存入独立列，包络查询直接聚合这两列。

### HDF5 导出 2D 数据集
- **陷阱：** 需在 `#ifdef HAS_HDF5` 内外各提供一份实现（真实 HDF5 + 空桩），否则未启用 HDF5 时链接失败。
- **修复：** 参照 `exportMultiFreqHdf5` 模式，`Impl` 方法放 `#ifdef` 内，外部包装 + 空桩放 `#else`。

### 多设备流程差异
- **陷阱：** PulseEddy 比 MagArray 多了一层 `OpenDevice` RPC，不能照搬 `StreamFrames` 直连。
- **修复：** `streamLoop` 内先 `OpenDevice` → `StartAcquisition` → `StreamFrames`，`stopAcquisition` 调 `StopAcquisition` + `CloseDevice`。

## 自愈协议

当 lint/build/test 失败时，最多 3 轮自动修复：
1. 第 1 轮：局部最小修复
2. 第 2 轮：按错误类别根因修复（语法/链接/测试回归/线程并发/持久化兼容/环境工具）
3. 第 3 轮：稳定化加固

3 轮内无法修复则输出阻塞报告。详见 [.cursor/skills/self-heal/SKILL.md](.cursor/skills/self-heal/SKILL.md)。
