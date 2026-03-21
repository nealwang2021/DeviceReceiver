# 实时数据监控系统

一个基于Qt 5.15.2开发的专业级实时数据监控系统，支持串口通信、数据处理、图形显示和远程控制功能。同时支持编译为 **WebAssembly (WASM)**，可直接在浏览器中运行并通过 **GitHub Pages** 托管部署。

## 📋 项目概述

**实时数据监控系统** 是一个功能完整的工业级数据监控解决方案，主要面向设备数据采集、实时监控和数据分析场景。系统采用模块化设计，支持多窗口绘图、设备配置管理、指令发送和数据记录等功能。

### ✨ 核心功能

#### 🖥️ **主界面与窗口管理**
- **多窗口MDI界面**: 支持平铺、层叠和选项卡式窗口管理
- **浮动面板**: 设备配置、指令发送、绘图管理和数据监控面板可自由停靠
- **实时状态显示**: 连接状态、帧率、数据量和报警信息实时更新
- **窗口列表管理**: 所有绘图窗口的统一管理和快速切换

#### 📊 **数据采集与处理**
- **双模式数据源**: 支持真实串口设备和模拟数据模式
- **协议解析**: 自动解析固定帧格式数据（16字节帧，AA55帧头）
- **DataFrame语义约定**: `detect_mode=1` 时 `comp0=幅值, comp1=相位`；`detect_mode=2` 时 `comp0=实部, comp1=虚部`
- **数据缓存管理**: LRU缓存策略，支持过期清理和最大帧数限制
- **实时数据流**: 毫秒级数据更新，支持高频率数据采集

#### 📈 **绘图与可视化**
- **多类型绘图窗口**: 组合图、温度图、湿度图、电压图、历史图
- **实时曲线绘制**: 基于QCustomPlot的高性能实时曲线
- **数据统计**: 均值、最大值、最小值、标准差等统计信息
- **报警阈值**: 可配置的温度报警阈值，实时报警提示

#### 🔧 **设备控制与配置**
- **串口配置**: 端口、波特率、数据位、停止位、校验位、流控制
- **后端联动显示**: 选择 gRPC 后端时自动隐藏串口专属配置项，避免误配置
- **设备控制**: 连接/断开控制，连接状态实时反馈
- **指令发送**: ASCII/十六进制指令发送，支持自动换行
- **指令历史**: 指令历史记录和快速重发
- **gRPC 测试验证**: 内置测试面板可验证固化协议命令 `selftest_ping`、`selftest_set_mode` 与周期流数据接收

#### 📝 **数据监控与记录**
- **实时数据监控**: 发送/接收数据显示，带时间戳和格式标记
- **日志记录**: 完整的运行日志记录，支持不同级别日志
- **交互追踪日志**: 客户端日志记录 gRPC 连接状态、命令回包、模式切换与周期数据包
- **配置文件**: INI格式配置文件，保存所有用户设置
- **历史记录**: 指令历史、窗口状态、面板布局的持久化存储

#### 🌐 **WebAssembly 支持**
- **浏览器运行**: 编译为 WASM，无需安装客户端即可在浏览器中使用
- **中文字体嵌入**: 自动嵌入 SimHei 字体，浏览器中中文正常显示
- **GitHub Pages 部署**: 一键生成静态资源到 `docs/` 目录，推送即部署
- **条件编译**: 通过 `QT_COMPILE_FOR_WASM` 宏，桌面端和 WASM 端共用同一套代码

### 🏗️ 系统架构

```
实时数据监控系统架构
├── 应用层 (Application Layer)
│   ├── ApplicationController (应用控制器)
│   ├── MainWindow (主界面窗口)
│   └── AppConfig (配置管理)
├── 业务层 (Business Layer)
│   ├── DataCacheManager (数据缓存管理)
│   ├── DataProcessor (数据处理)
│   └── PlotWindowManager (绘图窗口管理)
├── 数据层 (Data Layer)
│   ├── SerialReceiver (串口接收器 / WASM模拟数据)
│   ├── FrameData (数据帧结构)
│   └── PlotWindow / ArrayPlotWindow / HeatMapPlotWindow (绘图窗口)
├── 工具层 (Utility Layer)
│   ├── 构建脚本系统 (桌面端 + WASM)
│   ├── 日志系统
│   └── 单元测试框架
└── 部署层 (Deployment Layer)
    ├── wasm_build.bat (WASM 一体化构建脚本)
    ├── docs/ (GitHub Pages 静态资源)
    └── 条件编译 (QT_COMPILE_FOR_WASM)
```

#### 核心组件说明

- **ApplicationController**: 系统核心控制器，负责所有模块的初始化和生命周期管理
- **MainWindow**: 主界面，提供MDI窗口管理和浮动面板控制
- **PlotWindowManager**: 绘图窗口工厂和管理器，支持多种绘图类型
- **DataCacheManager**: 数据缓存单例，采用LRU策略管理实时数据帧
- **SerialReceiver**: 串口接收器，运行在独立线程，支持真实设备和模拟数据
- **AppConfig**: 配置管理单例，集中管理所有应用设置

## 🚀 快速开始

### 开发环境要求

#### 桌面端 (Windows)
- **Qt版本**: 5.15.2 msvc2019_64
- **Visual Studio**: 2019/2022 Community或Professional
- **编译器**: MSVC 2019 (v14.2+) 64位
- **操作系统**: Windows 10/11
- **内存**: 最低4GB，推荐8GB以上
- **磁盘空间**: 最少1GB可用空间

#### WebAssembly 端 (WASM)
- **Qt版本**: 5.15.2 wasm_32
- **Emscripten SDK**: 与 Qt 5.15.2 匹配的 emsdk 版本
- **make工具**: GNU make（如 `C:\arc_gnu\bin\make.exe`）
- **Python 3**: 用于本地 HTTP 预览服务器

> 💡 默认路径配置（可在 `wasm_build.bat` 中修改）：
> - Qt WASM: `C:\Qt\Qt5.15.2\5.15.2\wasm_32`
> - emsdk: `C:\Qt\emsdk\emsdk`

## 🛠️ 构建和运行

### 主要构建脚本

本项目提供了多种构建脚本，推荐使用增强版构建脚本。

#### 🚀 **`build_and_run_fixed_cn.bat` - 增强构建脚本（推荐）**
功能完整的构建脚本，支持多种参数选项，修复了中文编码问题。

**基本用法:**
```cmd
build_and_run_fixed_cn.bat [参数]
```

**可用参数:**
| 参数 | 说明 | 示例 |
|------|------|------|
| `-Help` | 显示帮助信息 | `build_and_run_fixed_cn.bat -Help` |
| `-Debug` | 构建调试版本 | `build_and_run_fixed_cn.bat -Debug` |
| `-Clean` | 清理构建文件 | `build_and_run_fixed_cn.bat -Clean` |
| `-Run` | 构建后运行程序 | `build_and_run_fixed_cn.bat -Run` |
| 无参数 | 构建Release版本 | `build_and_run_fixed_cn.bat` |

**组合使用示例:**
```cmd
# 清理后构建Release版本
build_and_run_fixed_cn.bat -Clean

# 构建Debug版本
build_and_run_fixed_cn.bat -Debug

# 清理、构建并运行
build_and_run_fixed_cn.bat -Clean -Run

# 构建Debug版本并运行
build_and_run_fixed_cn.bat -Debug -Run
```

#### 📦 **其他构建脚本**

| 脚本文件 | 说明 |
|----------|------|
| `build_and_run.bat` | 原始增强构建脚本 |
| `build_and_run_en.bat` | 英文版本增强构建脚本 |
| `full_build.bat` | 基础构建脚本，无参数支持 |
| `build.ps1` | PowerShell构建脚本 |
| `build_ps.ps1` | 原始PowerShell构建脚本 |

#### 🧩 **`package_grpc_test_server.bat` - gRPC 测试服务打包脚本**

将 `grpc_test_server.py` 打包为单文件 `grpc_test_server.exe`，供主程序“启动测试服务”按钮直接调用（生产环境推荐）。

**基本用法:**
```cmd
package_grpc_test_server.bat
```

**输出位置:**
- `build/release/grpc_test_server.exe`
- 如果存在 `build/debug/`，脚本会同步复制一份到 `build/debug/grpc_test_server.exe`

> 主程序优先启动 `grpc_test_server.exe`；仅在找不到 EXE 时，才回退到 Python 脚本模式（用于开发调试）。

#### 🎯 **`stage_grpc_test_server.py` / `run_stage_grpc_test_server.bat` — 三轴台 StageService 测试服务**

与 **`grpc_test_server.py`（被测设备 DeviceDataService）名称区分**，本组脚本实现 **`proto/stage.proto`** 的 **StageService**，供「三轴台测试装置」面板联调。

| 文件 | 说明 |
|------|------|
| `stage_grpc_test_server.py` | Python 模拟服务端（默认监听 **50052**，避免与 `grpc_test_server.py` 默认 **50051** 冲突） |
| `run_stage_grpc_test_server.bat` | 快速启动上述脚本 |
| `package_stage_grpc_test_server.bat` | 打包为 `build/release/stage_grpc_test_server.exe` |

控制端 gRPC 地址需与监听端口一致，例如 `127.0.0.1:50052`。

**重新生成 Python 桩**（修改 `stage.proto` 后）:
```cmd
python -m grpc_tools.protoc -Iproto --python_out=proto/generated_py --grpc_python_out=proto/generated_py proto/stage.proto
```

#### 🌐 **`wasm_build.bat` - WebAssembly 构建脚本**

将项目编译为 WebAssembly，可在浏览器中直接运行。

**基本用法:**
```cmd
wasm_build.bat [命令]
```

**可用命令:**
| 命令 | 说明 | 示例 |
|------|------|------|
| 无参数 | 增量构建 | `wasm_build.bat` |
| `rebuild` | 清理后完整重新构建 | `wasm_build.bat rebuild` |
| `clean` | 清理 build-wasm 目录 | `wasm_build.bat clean` |
| `serve` | 启动本地HTTP预览服务器 | `wasm_build.bat serve` |
| `deploy` | 复制产物到 docs/ 用于 GitHub Pages | `wasm_build.bat deploy` |

**典型工作流:**
```cmd
REM 首次构建
wasm_build.bat rebuild

REM 本地预览（浏览器访问 http://localhost:8000/realtime_data.html）
wasm_build.bat serve

REM 部署到 GitHub Pages
wasm_build.bat deploy
git add docs/
git commit -m "deploy wasm to GitHub Pages"
git push
```

### 构建目录结构

```
build/
├── release/                # Release版本输出目录
│   ├── realtime_data.exe   # 主程序
│   ├── grpc_test_server.exe      # 被测设备数据 gRPC 测试服务（DeviceDataService）
│   ├── stage_grpc_test_server.exe# 三轴台 StageService 测试服务（可选）
│   ├── realtime_data.pdb   # 程序数据库文件
│   └── realtime_data.log   # 运行日志文件
└── debug/                  # Debug版本输出目录
    ├── realtime_data.exe
    ├── grpc_test_server.exe
    ├── stage_grpc_test_server.exe
    ├── realtime_data.pdb
    └── realtime_data.log

build-wasm/                 # WASM构建输出目录
├── realtime_data.html      # 入口HTML页面 (~3KB)
├── realtime_data.js        # Emscripten 胶水代码 (~573KB)
├── realtime_data.wasm      # WebAssembly 二进制 (~23MB)
├── qtloader.js             # Qt WASM 加载器 (~21KB)
├── qtlogo.svg              # Qt Logo
├── obj/                    # 编译中间文件
├── moc/                    # MOC 生成文件
└── Makefile                # 生成的 Makefile

docs/                       # GitHub Pages 部署目录
├── index.html              # 入口页面 (从 realtime_data.html 复制)
├── realtime_data.js
├── realtime_data.wasm
├── qtloader.js
├── qtlogo.svg
└── .nojekyll               # 禁止 Jekyll 处理
```

### 🔧 **使用Qt Creator开发**

项目也可以通过Qt Creator正常打开和构建：

1. **打开项目**: 打开 `realtime_data.pro` 文件
2. **选择构建套件**: Desktop Qt 5.15.2 MSVC2019 64-bit
3. **构建/运行**: 点击构建或运行按钮
4. **调试**: 使用内置调试器进行代码调试

## 📖 使用指南

### 🎯 **首次启动**

1. **运行程序**: 双击 `build/release/realtime_data.exe` 或使用脚本运行
2. **初始界面**: 系统启动后显示主界面，包含四个浮动面板
3. **默认配置**: 使用模拟数据模式，无需硬件设备即可运行

### 🔌 **连接设备**

#### **模拟数据模式** (默认)
- **启用**: 勾选"启用模拟数据"复选框
- **间隔设置**: 调整模拟数据生成间隔（默认100ms）
- **点击连接**: 点击设备面板的"连接"按钮

#### **真实串口模式**
1. **选择串口**: 从端口列表选择正确的串口
2. **配置参数**: 设置波特率、数据位、停止位等参数
3. **禁用模拟数据**: 取消勾选"启用模拟数据"
4. **连接设备**: 点击"连接"按钮

#### **连接失败处理**
- **自动重试**: 连接失败时提示重试选项
- **切换到模拟数据**: 可一键切换到模拟数据模式
- **错误提示**: 详细错误信息显示在监控面板

### 📊 **数据监控**

#### **实时数据流**
- **监控面板**: 实时显示发送和接收的数据
- **时间戳**: 每条数据带精确到毫秒的时间戳
- **格式标记**: 区分HEX和TXT格式数据
- **自动滚动**: 新数据自动滚动到底部

#### **统计信息**
- **帧率**: 实时数据帧率显示
- **数据量**: 当前缓存中的数据帧数量
- **报警次数**: 触发报警的次数统计

### 📈 **绘图功能**

#### **创建绘图窗口**
1. **选择类型**: 在绘图管理面板选择窗口类型
2. **创建窗口**: 点击"新建窗口"按钮
3. **窗口位置**: 窗口在主界面MDI区域内显示

#### **绘图窗口类型**
| 窗口类型 | 说明 |
|----------|------|
| **组合图** | 温度、湿度、电压在同一坐标系中显示 |
| **温度图** | 专注于温度数据的实时变化曲线 |
| **湿度图** | 专注于湿度数据的实时变化曲线 |
| **电压图** | 专注于电压数据的实时变化曲线 |
| **历史图** | 显示历史数据趋势和统计分析 |

#### **窗口管理**
- **平铺窗口**: 所有窗口等分显示主界面区域
- **层叠窗口**: 窗口层叠排列，便于切换
- **窗口列表**: 所有窗口的集中管理和快速激活
- **关闭窗口**: 可单独关闭或关闭选中的窗口

### 💾 **数据管理**

#### **配置保存**
- **自动保存**: 关闭程序时自动保存所有配置
- **配置文件**: `config.ini` 保存串口参数、窗口状态等
- **手动保存**: 修改配置后立即生效

#### **日志记录**
- **日志文件**: `realtime_data.log` 保存运行日志
- **日志级别**: 支持DEBUG/INFO/WARNING/ERROR不同级别
- **日志内容**: 包含时间戳、日志级别和详细信息

#### **指令历史**
- **历史记录**: 自动保存发送的指令
- **快速重发**: 双击历史记录快速重发指令
- **历史限制**: 可配置最大历史记录数量

### ⚙️ **配置说明**

#### **配置文件格式**
```ini
[Serial]
Port=COM3
BaudRate=115200

[Receiver]
BackendType=grpc
GrpcEndpoint=127.0.0.1:50051
UseMockData=false
MockDataIntervalMs=100

[UI]
showDevicePanel=true
showCommandPanel=true
showPlotPanel=true
showMonitorPanel=true

[Commands]
sendAsHex=false
autoSendNewline=true
maxHistory=20
```

#### **配置项说明**
| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| **Serial/Port** | COM3 | 串口端口名称 |
| **Serial/BaudRate** | 115200 | 波特率 |
| **Receiver/BackendType** | grpc | 接收后端（serial/grpc） |
| **Receiver/GrpcEndpoint** | 127.0.0.1:50051 | gRPC 服务端地址 |
| **Receiver/UseMockData** | false | 是否启用本地模拟（true=mock, false=真实服务） |
| **Receiver/MockDataIntervalMs** | 100 | 模拟数据间隔(ms) |
| **UI/showDevicePanel** | true | 显示设备面板 |
| **UI/showCommandPanel** | true | 显示指令面板 |
| **Commands/maxHistory** | 20 | 最大指令历史数量 |

## 🐛 故障排除

### 常见问题

#### 1. **中文显示乱码**
**症状**: PowerShell中运行脚本出现中文乱码
**解决方法**:
```powershell
# 方法一：设置编码
[Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(936)
.\build_and_run_fixed_cn.bat

# 方法二：使用cmd运行
cmd /c "build_and_run_fixed_cn.bat -Help"
```

#### 2. **环境变量设置失败**
**症状**: "找不到vcvarsall.bat"或"找不到qmake.exe"错误
**检查项目**:
- Visual Studio是否正确安装
- Qt 5.15.2 (msvc2019_64)是否安装到默认路径

**默认安装路径**:
- Qt: `C:\Qt\Qt5.15.2\5.15.2\msvc2019_64\bin\qmake.exe`
- Visual Studio: `C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat`

#### 3. **构建失败**
**症状**: 编译过程中出现错误
**解决步骤**:
1. **清理项目**: `build_and_run_fixed_cn.bat -Clean`
2. **检查依赖**: 确认Qt和VS环境正确配置
3. **检查源文件**: 确认所有源文件完整无损坏

#### 4. **串口连接失败**
**症状**: 无法打开串口或连接失败
**检查步骤**:
1. **端口存在**: 确认设备已连接且端口正确
2. **权限问题**: 以管理员权限运行程序
3. **端口占用**: 检查端口是否被其他程序占用
4. **硬件状态**: 确认设备电源和连接正常

#### 5. **数据不显示**
**症状**: 连接成功但无数据显示
**检查项目**:
1. **数据源**: 确认模拟数据已启用或硬件正常
2. **协议匹配**: 确认数据格式与解析协议匹配
3. **缓存状态**: 检查DataCacheManager是否正常初始化
4. **信号连接**: 确认各模块间信号槽连接正常

## 🌐 WebAssembly 与 GitHub Pages 部署

### WASM 构建说明

项目通过条件编译宏 `QT_COMPILE_FOR_WASM` 区分桌面端和 WASM 端代码。WASM 构建时：
- 串口模块 (`QSerialPort`) 被排除，仅保留模拟数据模式
- 自动嵌入 SimHei 中文字体（`files/fonts/simhei.ttf`，约 9.3MB）
- OpenGL/freeglut 依赖被排除
- 初始内存设为 32MB (`TOTAL_MEMORY=33554432`)，并启用 `ALLOW_MEMORY_GROWTH`

### GitHub Pages 部署步骤

1. **构建 WASM 版本**
   ```cmd
   wasm_build.bat rebuild
   ```

2. **生成部署文件**
   ```cmd
   wasm_build.bat deploy
   ```
   这会将构建产物复制到 `docs/` 目录，并将 `realtime_data.html` 重命名为 `index.html`。

3. **提交并推送**
   ```cmd
   git add docs/ .gitignore .gitattributes
   git commit -m "deploy wasm to GitHub Pages"
   git push
   ```

4. **配置 GitHub Pages**
   - 进入 GitHub 仓库 **Settings → Pages**
   - Source 选择 **Deploy from a branch**
   - Branch 选择 `main`，Folder 选择 `/docs`
   - 保存后等待几分钟，即可通过 `https://<用户名>.github.io/<仓库名>/` 访问

### 注意事项

| 项目 | 说明 |
|------|------|
| **文件大小** | `.wasm` 约 23MB（含嵌入字体），在 GitHub 100MB 单文件限制内 |
| **首次加载** | 浏览器首次加载较慢，后续会使用缓存 |
| **COOP/COEP** | Qt 5.15.2 wasm_32 为单线程模式，不需要特殊 HTTP 头 |
| **浏览器兼容** | 支持 Chrome、Firefox、Edge 等现代浏览器 |
| **串口功能** | WASM 模式下仅支持模拟数据，不支持真实串口 |

## 🔧 高级配置

### 自定义数据协议
如需修改数据解析协议，请修改以下文件：
- **`SerialReceiver.cpp`**: `parseRawData()` 函数中的协议解析逻辑
- **`FrameData.h`**: 数据帧结构定义
- **协议参数**: 帧头、帧长度、字节顺序等配置常量

### 扩展绘图功能
要添加新的绘图类型：
1. **扩展枚举**: 在 `PlotWindowManager.h` 中添加新的绘图类型
2. **创建窗口类**: 继承 `PlotWindow` 类实现新类型
3. **工厂方法**: 在 `PlotWindowManager` 中添加创建逻辑
4. **界面集成**: 在主界面添加对应的创建选项

### 性能调优
针对高性能场景的优化建议：
- **缓存大小**: 根据内存调整 `maxCacheSize` (默认600帧)
- **绘图点数**: 调整 `maxPlotPoints` 平衡性能和显示效果
- **刷新间隔**: 根据数据频率调整 `plotRefreshIntervalMs`
- **线程优先级**: 可调整串口线程优先级以获得更及时的数据处理

## 📚 API参考

### 核心类说明

#### **ApplicationController** - 应用控制器
**职责**: 协调所有模块的初始化和生命周期管理
**关键方法**:
- `initialize()`: 初始化所有应用模块
- `start()`: 启动应用（显示窗口，开始数据接收）
- `stop()`: 停止应用（停止数据接收）
- `sendCommand()`: 线程安全的指令发送

#### **DataCacheManager** - 数据缓存管理器
**模式**: 单例模式
**功能**: LRU缓存管理，支持过期清理
**关键方法**:
- `addFrame()`: 添加数据帧到缓存
- `getFrame()`: 获取指定索引的数据帧
- `getCacheSize()`: 获取当前缓存大小
- `clearExpired()`: 清理过期数据

#### **PlotWindowManager** - 绘图窗口管理器
**模式**: 单例模式
**功能**: 绘图窗口的工厂和管理器
**关键方法**:
- `createWindow()`: 创建指定类型的绘图窗口
- `createWindowInMdiArea()`: 在MDI区域中创建窗口
- `windows()`: 获取所有窗口列表
- `startUpdates()`: 启动所有窗口的数据更新

#### **SerialReceiver** - 串口接收器
**线程**: 运行在独立线程
**功能**: 串口通信和数据解析
**关键方法**:
- `openSerial()`: 打开串口连接
- `closeSerial()`: 关闭串口
- `sendCommand()`: 发送指令到串口
- `startMockData()`: 启动模拟数据模式

### 信号槽系统
系统采用松耦合设计，通过信号槽实现模块间通信：

```
串口数据流:
SerialReceiver::dataReceived → MainWindow::onDataReceived (显示)
SerialReceiver::dataReceived → DataCacheManager::addFrame (缓存)
DataCacheManager::frameAdded → PlotWindow::updatePlot (绘图)

控制流:
MainWindow::connectClicked → ApplicationController::start
ApplicationController::started → MainWindow::updateConnectionStatus
```

## 🔄 版本历史

### v1.0 - 基础版本
- 基础串口通信功能
- 简单的数据绘图显示
- 基础构建脚本

### v1.1 - 增强版本
- 模块化重构，引入ApplicationController
- MDI多窗口界面
- 浮动面板设计
- 配置管理系统
- 增强的构建脚本系统

### v1.2 - 稳定版本
- 修复串口连接状态同步问题
- 优化重新连接逻辑
- 完善错误处理机制
- 更新文档和README

### v1.3 - WebAssembly 版本
- 新增 WebAssembly (WASM) 编译支持，可在浏览器中运行
- 条件编译区分桌面端和 WASM 端代码 (`QT_COMPILE_FOR_WASM`)
- 嵌入 SimHei 中文字体解决浏览器中文显示问题
- 排除 WASM 不支持的 OpenGL/freeglut/QSerialPort 依赖
- 新增 `wasm_build.bat` 一体化构建脚本（build/clean/rebuild/serve/deploy）
- 支持 GitHub Pages 一键部署（`wasm_build.bat deploy`）
- 新增 `.gitignore` 和 `.gitattributes` 配置

### 当前版本
- **版本号**: v1.3.0
- **状态**: 稳定运行
- **最后更新**: 2026年2月
- **主要特性**: 完整的实时数据监控解决方案 + WebAssembly 浏览器端部署

## 📞 支持与贡献

### 报告问题
如遇到问题，请提供以下信息：
1. **环境信息**: 操作系统、Qt版本、Visual Studio版本
2. **复现步骤**: 详细描述问题复现步骤
3. **错误日志**: 提供 `realtime_data.log` 文件内容
4. **截图**: 如有界面问题，请提供截图

### 贡献代码
欢迎提交Pull Request，请确保：
1. **代码规范**: 遵循现有代码风格
2. **功能完整**: 新功能需包含完整测试
3. **文档更新**: 更新相关文档和注释
4. **兼容性**: 不影响现有功能

## 📄 许可证

本项目采用MIT许可证，详情请见LICENSE文件。

---

**感谢使用实时数据监控系统！如有任何问题或建议，请随时联系我们。**
