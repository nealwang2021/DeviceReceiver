# Dock 面板布局持久化偶发丢失 设计

日期: 2026-05-26

## 背景与症状

- **现象**：程序退出后再次启动，偶发上次 **Dock 面板**（被测设备、指令发送、绘图管理、数据监控、历史总览、三轴台等）的停靠位置、分栏、显隐与预期不符，表现为「布局消失」或回到近似默认状态。
- **范围**：仅 Dock 面板（用户确认）；不含 MDI 绘图子窗口、主窗最大化状态（本设计不扩展范围，但 `MainWindowGeometry` 仍随统一保存入口维护）。
- **退出方式**：用户无法稳定区分正常关闭与强杀；设计需同时覆盖「未保存」与「保存了但被覆盖/损坏/恢复失败」。

## 目标

1. **可定位**：日志能区分「未写入 / 写入失败 / 读空 / restoreState 失败 / 被旧数据覆盖」。
2. **可恢复**：正常关闭与 `aboutToQuit` 路径下，Dock 布局稳定持久化；写盘原子化，避免半截 `config.ini`。
3. **避免误写**：启动恢复完成前、UI 未就绪时，禁止把临时/默认布局写回磁盘。
4. **最小行为变更**：不引入 Dock 拖动后的 debounce 自动落盘（用户明确不要）。

## 非目标

- 不重构为单一配置源（去掉 `UI/Show*Panel`）——留作后续方案 3。
- 不保证任务管理器强杀后 100% 保留最后一次 Dock 调整（无 debounce 时接受该限制）。
- 不改动 MDI `SavedPlotWindowTypes` 恢复逻辑（除非实现时发现与 Dock 恢复冲突）。

## 根因假设（按优先级）

| ID | 假设 | 机制 |
|----|------|------|
| H1 | 中途写盘使用内存旧布局 | `setStyle()` 等仅 `saveToFile()`，未先 `saveConfigFromUI()`，用旧的 `m_mainWindowState` 覆盖磁盘 |
| H2 | 非正常退出 | 强杀/崩溃未执行 `closeEvent`，布局未写入 |
| H3 | 非原子写盘 | `QSettings` 直接写 `config.ini`，中断导致 INI 损坏或 `MainWindowState` 为空 |
| H4 | 恢复失败未感知 | `restoreState()` 返回 `false` 未记录，界面呈默认 Dock |
| H5 | 双源冲突 | `restoreState` 后仍按 `UI/Show*Panel` 强制 `setVisible`，与 state 内嵌布局冲突 |
| H6 | INI 编码 | UTF-8 BOM 导致读侧整段 UI 为空（读侧已有临时无 BOM 副本规避；写侧须避免写入 BOM） |

## 方案选择

采用 **方案 2：可观测性 + 持久化加固**（用户已确认）。

**明确不做**：Dock `dockLocationChanged` 的 debounce 自动保存（用户已确认）。

## 现有架构（摘要）

```
启动: main.cpp → AppConfig::loadFromFile(config.ini)
     → MainWindow::initUI() → restoreState + restoreGeometry + Show*Panel setVisible
     → MainWindow::initialize() → restoreSavedPlotWindowsFromConfig()

关闭: MainWindow::closeEvent() → saveConfigFromUI() → saveToFile()

持久化键（exe 同目录 config.ini）:
  UI/MainWindowState      ← QMainWindow::saveState()
  UI/MainWindowGeometry   ← saveGeometry()
  UI/ShowDevicePanel 等   ← 各 Dock isVisible()
```

已知代码约束：`MainWindow` 析构中 **禁止** `saveConfigFromUI()`（`isVisible()` 不可靠）。

## 设计

### 1. 统一持久化入口 `MainWindow::persistLayout(const char* reason)`

**职责**：唯一允许把 Dock/主窗几何写入 `AppConfig` 并落盘的路径（布局相关）。

**顺序**（固定）：

1. 若 `!m_uiReady || !m_layoutRestored || m_suppressConfigPersist` → 仅打 debug 日志并返回（不写盘）。
2. `saveConfigFromUI()` — 从当前 UI 读取 `saveState()` / `saveGeometry()` / `Show*Panel`。
3. `AppConfig::saveToFileAtomic(path, reason)` — 原子写入。

**调用点**（`reason` 字符串）：

| 触发点 | reason | 说明 |
|--------|--------|------|
| `closeEvent` | `closeEvent` | 主路径，替代直接 `saveToFile` |
| `QApplication::aboutToQuit` | `aboutToQuit` | 补一层；不替代 closeEvent |
| `setStyle()` | `styleChange` | 必须先 persist 再写样式键，禁止裸 `saveToFile` |
| 用户点「连接」等已有 `saveToFile` | 保持或改为 `persistLayout("userAction")` | 与布局一并落盘 |

**不新增**：Dock 位置变化 debounce 写盘。

### 2. `AppConfig::saveToFileAtomic(const QString& path, const QString& reason)`

**写入流程**：

1. 与现有 `saveToFile` 相同字段写入 `QSettings`，目标为 `path + ".tmp"`（或 `QTemporaryFile` + 固定命名）。
2. `settings.sync()`；检查 `QSettings::status() == NoError`。
3. 若存在 `path`，复制为 `path + ".bak"`（覆盖旧 bak，仅保留 1 份）。
4. `QFile::remove(path)` + `QFile::rename(tmp, path)`（Windows 兼容）；失败则 **保留原 config.ini**，打 `qWarning`。
5. 日志：`[AppConfig] persist ok reason=... stateBytes=... geometryBytes=... path=...`

**编码**：写入 UTF-8 **无 BOM**（避免 H6 复发）。

**读侧**：保持现有 BOM 临时副本逻辑不变。

### 3. 启动期保护标志

在 `MainWindow` 增加：

- `m_uiReady`：`initUI()` 成功结束后置 `true`。
- `m_layoutRestored`：`restoreState` / `Show*Panel` / `restoreSavedPlotWindowsFromConfig` 完成后置 `true`。
- `m_suppressConfigPersist`：在 `loadConfigToUI()`、`rebuildGrpcParamUI()` 等批量改 UI 期间为 `true`（RAII 守卫类 `ConfigPersistSuppressor`）。

`saveConfigFromUI()` 仍可更新内存中的 `AppConfig`；仅 **落盘** 受 `persistLayout` 门禁约束。

### 4. Dock 稳定标识与恢复加固

为每个 `QDockWidget` 设置稳定 `objectName`（与标题无关）：

| Dock | objectName |
|------|------------|
| 被测设备 | `dock_device` |
| 指令发送 | `dock_command` |
| 绘图管理 | `dock_plot` |
| 三轴台 | `dock_stage`（已有 `stage_panelDock` 可统一或保留兼容） |
| 数据监控 | `dock_monitor` |
| 历史总览 | `dock_overview` |

**恢复**（`initUI` 内）：

```cpp
const bool ok = restoreState(config->mainWindowState());
if (!ok) {
    qWarning() << "[MainWindow] restoreState failed, bytes=" << config->mainWindowState().size();
    // 回退：仅应用 Show*Panel + geometry，不强行 tabify/raise 破坏默认
} else {
    // 现有 Show*Panel + raise 逻辑
}
```

启动日志（已有加载字节数日志保留）：

- `[AppConfig] UI/MainWindowState 已加载字节数: N`
- `[MainWindow] restoreState ok=0/1`

### 5. 可观测性（定位手册）

**复现后检查**（exe 同目录）：

1. `config.ini` 中 `UI/MainWindowState` 是否为空或长度骤降。
2. 是否存在 `config.ini.bak` 且其 `MainWindowState` 更长 → 指向 H1/H3 覆盖或写坏。
3. 当天 `data/YYYYMMDD/realtime_data_*.log` 中最后一次 `persist ok` 的 `reason` 与 `stateBytes`。

**关联操作**：丢失前是否切换主题（`styleChange`）、切换后端类型。

### 6. 错误处理

| 情况 | 行为 |
|------|------|
| `saveToFileAtomic` 失败 | 保留旧 `config.ini`；`qWarning`；UI 不提示（避免打扰），下次启动仍用旧配置 |
| `restoreState` 失败 | 警告日志；回退 Show* + geometry；不崩溃 |
| `loadFromFile` 失败 | 保持现有默认配置 + 警告（已有） |

### 7. 测试计划

| 场景 | 期望 |
|------|------|
| 调整 Dock 停靠/显隐 → 正常关闭 → 再开 | 布局一致；日志 `persist ok reason=closeEvent` |
| 调整 Dock → 切换主题 → 关闭 → 再开 | 布局一致（验证 H1 修复） |
| 启动后未改布局即关闭 | `config.ini` 不被写坏；`stateBytes` 稳定 |
| 删除 `UI/MainWindowState` 后启动 | `restoreState failed` 日志；默认 Dock，不崩溃 |
| 模拟写盘中断（手工截断 ini） | 若有 `.bak`，可手动恢复；启动不崩溃 |

## 涉及文件

- `AppConfig.h` / `AppConfig.cpp` — `saveToFileAtomic`、日志、无 BOM 写
- `MainWindow.h` / `MainWindow.cpp` — `persistLayout`、标志位、Dock objectName、`restoreState` 检查、`closeEvent` / `setStyle` / `aboutToQuit` 接线
- `main.cpp` — `aboutToQuit` 连接至 `MainWindow::persistLayout`（或通过 `ApplicationController` 转发，以实现时耦合最小为准）

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| `aboutToQuit` 与 `closeEvent` 双写 | 幂等：同一布局写两次结果应一致；原子写避免损坏 |
| objectName 变更导致旧 state 失效 | 一次性降级为默认布局；日志明确 `restoreState failed` |
| 强杀仍丢布局 | 用户接受；文档说明仅正常退出与 aboutToQuit 保证 |

## 决策记录

- **2026-05-26**：采用方案 2；**不**做 Dock debounce 自动保存（用户确认）。
