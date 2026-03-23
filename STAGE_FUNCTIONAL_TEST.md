# 三轴台功能测试验证

本文档确认 `stage_grpc_test_server.py` 与三轴台界面（`MainWindow` 右侧「三轴台测试装置」面板）的 RPC 及功能对应关系，用于完整功能验证。

**验证状态**：`stage_grpc_test_server.py` 已与 proto（Python protobuf 6.x 字段命名 `unixMs`、`xMm` 等）对齐，全部 11 个 RPC 已通过自动化测试。

## 一、前置条件

1. **启动 Stage 测试服务器**（默认端口 50052）：
   ```bash
   cd d:\WS\qtpro\DeviceReceiver
   python stage_grpc_test_server.py --port 50052
   ```
   也可在资源管理器中双击 `run_stage_grpc_test_server.bat`（或在 cmd 里直接输入该文件名）。**不要**执行 `python run_stage_grpc_test_server.bat`，否则 Python 会把批处理当脚本解析并报 `SyntaxError`。
2. **启动 realtime_data 主程序**，在右侧显示「三轴台测试装置」面板。
3. **连接**：在地址栏填写 `127.0.0.1:50052`，点击「连接」。

### 无公网 IPv4（CGNAT）时从外网访问

若家庭宽带无公网 IPv4/IPv6，无法做路由器端口转发，需 **内网穿透**。本仓库提供 **ngrok** 一键脚本：

1. 安装 [ngrok](https://ngrok.com/download)，执行 `ngrok config add-authtoken <令牌>`。
2. 在仓库根目录运行 `scripts\start_stage_with_ngrok.bat`（或见 [`scripts/README_stage_tunnel.md`](scripts/README_stage_tunnel.md)）。
3. 脚本会打印 **外网 gRPC 地址** `host:port`，在远端电脑的 `realtime_data` 中将 **StageGrpcEndpoint** 填为该地址。

**安全**：当前为明文 gRPC，勿长期暴露不可信公网。

### 仅查看本机局域网 IPv4 提示

```bash
python stage_grpc_test_server.py --external
```

---

## 二、RPC 与 UI 功能对照

| proto RPC | 测试服务器实现 | UI 操作 / 命令 | 验证项 |
|-----------|----------------|----------------|--------|
| **Connect** | ✓ 接受 ip:port | 点击「连接」 | 连接成功、状态变绿 |
| **Disconnect** | ✓ 模拟断开 | 点击「断开」 | 状态变为未连接 |
| **GetPositions** | ✓ 返回 x,y,z (mm/pulse) | 点击「读一次」或 `get_positions` | 显示 X/Y/Z 位置 |
| **PositionStream** | ✓ 按 intervalMs 流式推送 | 点击「订阅位置流」或 `start_stream 100` | 位置持续刷新 |
| **Jog** | ✓ 模拟 X/Y/Z ± 速度 | 选轴+方向，点「开始点动」/「停止」 | 开流时位置随 Jog 变化 |
| **MoveAbs** | ✓ 设置内部位置 | 填 X/Y/Z，点「执行绝对运动」 | 位置跳转到目标值 |
| **MoveRel** | ✓ 相对移动 | 选轴+Δ，点「执行相对运动」 | 位置按步长变化 |
| **SetSpeed** | ✓ 更新速度/加速参数 | 填脉冲/s、加速 ms，点「应用」 | 控制台打印 set_speed ok |
| **StartScan** | ✓ 模拟扫描启动 | 填扫描参数，点「开始扫描」 | 控制台打印 scan started |
| **StopScan** | ✓ 模拟扫描停止 | 点「停止扫描」 | 控制台打印 scan stopped |
| **GetScanStatus** | ✓ 返回 running/status | 点「查询状态」或 `scan_status` | 显示扫描状态 |

---

## 三、逐项验证步骤

### 1. 连接 / 断开
- 地址填 `127.0.0.1:50052`，点「连接」
- 状态显示「已连接」且为绿色
- 点「断开」，状态变为「未连接」

### 2. 位置
- 连接后点「读一次」→ 显示 X/Y/Z（mm 与 pulse）
- 点「订阅位置流」，间隔 100ms → 位置持续更新
- 点「停止推流」→ 位置不再更新

### 3. Jog
- 保持「订阅位置流」开启
- 选 X、正向 +，点「开始点动」→ X 递增
- 点「停止」→ X 停止
- 可对 Y、Z 及负向做同样验证

### 4. MoveAbs
- 设 X=5、Y=10、Z=2，点「执行绝对运动」
- 读一次或观察流数据 → 显示约 (5, 10, 2)

### 5. MoveRel
- 选 X，Δ 1.0，点「执行相对运动」
- 位置 X 增加约 1 mm

### 6. SetSpeed
- 脉冲/s=20000、加速=100 ms，点「应用」
- 最近结果显示成功

### 7. 扫描
- 填 X/Y 范围、步长、Z 固定，点「开始扫描」
- 点「查询状态」→ 显示「运行中」及参数
- 点「停止扫描」
- 再点「查询状态」→ 显示「已停止」

### 8. 自定义命令
- 输入 `help`，点发送 → 显示支持指令列表
- 可输入 `get_positions`、`scan_status` 等验证

---

## 四、测试服务器输出示例

连接成功时，服务器控制台应看到类似：
```
[Connect] stage-grpc-test-server: accepted stage TCP target 127.0.0.1:50052
```

订阅位置流后：
```
[PositionStream] interval_ms=100
```

Jog/Move/Scan 时会有对应 `[Jog]`、`[MoveAbs]`、`[StartScan]` 等日志。

---

## 五、JSON 包类型

MainWindow 解析的 Stage 包类型：

| type | 含义 | 主要字段 |
|------|------|----------|
| `stageStatus` | 连接/断开/重连状态 | status, endpoint, detail |
| `stagePositions` | 位置数据 | xMm, yMm, zMm, xPulse, yPulse, zPulse, source, unixMs |
| `stageCommandResult` | 命令结果 | ok, command, message |
| `stageScanStatus` | 扫描状态 | running, status |

测试服务器通过 StageReceiverBackend 的 `emit*` 生成上述包，与 MainWindow 的 `handleStageBackendPacket` 完全对应。

---

## 六、自动化测试（CMake / Qt Test / 脚本）

1. **CMake 选项**：`BUILD_TESTS=ON`（默认），需 `ENABLE_GRPC=ON` 且非 WASM 构建。
2. **目标**：
   - `tst_stage_integration`：`StageReceiverBackend` 全流程（自动启动 `stage_grpc_test_server.py`），不依赖主窗口。
   - `tst_stage_panel`：完整 `MainWindow` + `ApplicationController`，使用 `tests/fixtures/config.ini`（被测设备 gRPC Mock），自动起 Stage 桩后点击「连接」「读一次」。
3. **三轴台控件 `objectName`**：便于 Qt Test / pywinauto 查找，前缀 `stage_`（如 `stage_connectButton`、`stage_endpointEdit`），主窗口为 `mainWindow`。
4. **运行**：
   ```bat
   build_cmake.bat
   run_stage_tests.bat
   ```
   或在构建根目录：`cd build_cmake && ctest -R Stage -V`（`include(CTest)` 后根目录即有 `CTestTestfile.cmake`）。HTML 汇总：`python scripts/run_stage_tests.py --build-dir build_cmake`（生成 `build_cmake/stage_test_report.html`）。
5. **Qt Test XML**（可选）：`tst_stage_integration -xml -o result.xml`（具体参数以 `tst_stage_integration -help` 为准）。
6. **Python E2E（可选）**：`pip install -r tests/e2e/requirements.txt`，`pytest tests/e2e --html=e2e_report.html`；若设置 `REALTIME_DATA_EXE` 指向 `realtime_data.exe` 可跑进程级烟测。

**若直接运行 `tst_stage_integration.exe` 报 “no Qt platform plugin could be initialized”**：可执行文件旁需要 `platforms\\qwindows.dll`。请重新执行 `build_cmake.bat`（已对测试 exe 调用 `windeployqt`），或在构建后让 CMake 的 POST_BUILD 复制该插件；勿单独把测试 exe 拷到无 Qt 插件的目录。

---

## 七、小结

- **stage_grpc_test_server.py** 实现了 `stage.proto` 中全部 11 个 RPC。
- **三轴台面板** 的按钮和文本命令与这些 RPC 一一对应。
- 在本地运行 `stage_grpc_test_server.py` 并连接 `127.0.0.1:50052`，即可完整验证三轴台当前支持的全部功能。
