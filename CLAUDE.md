# DeviceReceiver 项目说明书

## 项目概览

- **项目名称：** DeviceReceiver
- **一句话描述：** 通用的测试台软件，收集设备数据实时可视化，离线可视化。
- **主要技术栈：** C++17、Qt 5.15、CMake，可选 Python gRPC 模拟服务。
- **主要环境：** Windows + MSVC 2019 + Qt 5.15.2。

## 命令入口

项目不强制使用顶层 `Makefile`。推荐优先使用现有脚本 `build_cmake.bat`，并保留原生 `cmake/ctest` 作为备用入口。

### 快速验证（等价于 `verify`）

推荐：

```powershell
cmd /c "cd /d d:\WS\qtpro\DeviceReceiver && build_cmake.bat"
```

备用：

```powershell
cmake -S . -B build_cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build build_cmake --config Release
ctest --test-dir build_cmake --output-on-failure -L critical
```

### 全量验证（等价于 `verify-full`）

```powershell
cmake -S . -B build_cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build build_cmake --config Release
ctest --test-dir build_cmake --output-on-failure
```

### Lint（等价于 `lint`）

优先对变更过的 C/C++ 文件执行：

```powershell
clang-format --dry-run --Werror <files...>
```

如果环境中可用，再执行：

```powershell
clang-tidy -p build_cmake <files...>
```

### 类型检查（等价于 `typecheck`）

C++ 项目的类型与语义检查通过配置 + 编译完成：

```powershell
cmake -S . -B build_cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build build_cmake --config Release
```

### 测试

- 关键测试（等价于 `test-critical`）：

```powershell
ctest --test-dir build_cmake --output-on-failure -L critical
```

- 全量测试（等价于 `test-full`）：

```powershell
ctest --test-dir build_cmake --output-on-failure
```

## 架构

- **UI 层（Qt Widgets）：** 负责界面交互、绘图与视图状态管理。
- **设备通信层：** 负责串口/网络/设备协议与数据采集。
- **数据管线：** 将原始数据转换为可渲染、可存储的标准数据帧。
- **持久化层：** 负责离线文件写入、读取与回放输入。
- **跨线程模型：** 采集线程必须独立于 UI 线程，跨线程通信仅使用 Qt 信号槽或线程安全队列。

## 不可违反约束

以下约束为强制要求：

1. 设备通信层不得依赖 UI 层。
2. 实时采集线程与 UI 渲染线程必须隔离，跨线程通信只能用信号槽或线程安全队列。
3. 原始数据持久化格式必须向后兼容，读取历史文件不得崩溃。
4. 任何阻塞 I/O 不得在主 UI 线程执行。
