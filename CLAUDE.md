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
- [PlotWindowManager](PlotWindowManager.h) — 单例，统一管理所有绘图窗口。定时轮询 DataCacheManager，经 PlotDataHub 聚合后广播 `dataUpdated` / `plotSnapshotUpdated` 信号。

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

## 自愈协议

当 lint/build/test 失败时，最多 3 轮自动修复：
1. 第 1 轮：局部最小修复
2. 第 2 轮：按错误类别根因修复（语法/链接/测试回归/线程并发/持久化兼容/环境工具）
3. 第 3 轮：稳定化加固

3 轮内无法修复则输出阻塞报告。详见 [.cursor/skills/self-heal/SKILL.md](.cursor/skills/self-heal/SKILL.md)。
