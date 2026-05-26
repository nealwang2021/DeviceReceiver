# Dock 面板布局持久化加固 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Dock 面板布局偶发丢失：统一布局落盘入口、原子写 `config.ini`、启动期防误写、恢复失败可观测。

**Architecture:** `MainWindow::persistLayout(reason)` 作为唯一落盘路径（先 `saveConfigFromUI` 再 `AppConfig::saveToFileAtomic`）；启动阶段用 `m_uiReady` / `m_layoutRestored` / `ConfigPersistSuppressor` 阻止误覆盖；Dock 使用稳定 `objectName` 并检查 `restoreState` 返回值。

**Tech Stack:** C++17, Qt 5.15, QSettings(IniFormat), QMainWindow::saveState/restoreState

**Spec:** `docs/superpowers/specs/2026-05-26-dock-layout-persistence-design.md`

---

## 文件职责

| 文件 | 变更 |
|------|------|
| `AppConfig.h` / `AppConfig.cpp` | 新增 `saveToFileAtomic`；`saveToFile` 委托原子实现；无 BOM 写 |
| `MainWindow.h` / `MainWindow.cpp` | `persistLayout`、启动标志、`ConfigPersistSuppressor`、Dock objectName、`restoreState` 检查、替换写盘调用点 |
| `main.cpp` | **不改**（`aboutToQuit` 在 `MainWindow` 内连接 `qApp`） |

---

### Task 1: AppConfig 原子保存

**Files:**
- Modify: `AppConfig.h`（`saveToFileAtomic` 声明）
- Modify: `AppConfig.cpp`（实现 + 重构 `saveToFile`）

- [ ] **Step 1: 在 `AppConfig.h` 增加 API**

在 `bool saveToFile(const QString& filename);` 之后添加：

```cpp
/// 原子写入 config.ini（先写 .tmp，备份 .bak，再 rename）。reason 仅用于日志。
bool saveToFileAtomic(const QString& filename, const QString& reason = QString());
```

- [ ] **Step 2: 抽取 `writeSettingsToPath` 私有逻辑**

在 `AppConfig.cpp` 的匿名命名空间或 `AppConfig` 内新增静态/私有函数，把现有 `saveToFile` 中 `QSettings settings(filename, ...)` 到 `settings.sync()` 的**全部 setValue** 移入：

```cpp
static bool writeSettingsToPath(const AppConfig* self, const QString& path, QString* errorOut)
{
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue("General/AppTitle", self->m_appTitle);
  // ... 复制现有 saveToFile 全部 setValue（Window/Cache/Serial/UI/Export/Log 等）
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (errorOut) *errorOut = QStringLiteral("QSettings sync failed");
        return false;
    }
    return true;
}
```

- [ ] **Step 3: 实现 `saveToFileAtomic`**

```cpp
bool AppConfig::saveToFileAtomic(const QString& filename, const QString& reason)
{
    const QString tmpPath = filename + QStringLiteral(".tmp");
    const QString bakPath = filename + QStringLiteral(".bak");

    QString writeError;
    if (!writeSettingsToPath(this, tmpPath, &writeError)) {
        qWarning().noquote() << QString("[AppConfig] persist failed reason=%1 stage=writeTmp err=%2")
                                    .arg(reason, writeError);
        QFile::remove(tmpPath);
        return false;
    }

    // 去掉 UTF-8 BOM：QSettings 默认可能带 BOM，用 QFile 读 tmp 若以 EF BB BF 开头则重写无前导 BOM 版本
    {
        QFile f(tmpPath);
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray content = f.readAll();
            f.close();
            if (content.startsWith("\xEF\xBB\xBF")) {
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(content.mid(3));
                    f.close();
                }
            }
        }
    }

    if (QFile::exists(filename)) {
        QFile::remove(bakPath);
        if (!QFile::copy(filename, bakPath)) {
            qWarning() << "[AppConfig] 无法创建备份" << bakPath;
        }
    }

    if (QFile::exists(filename) && !QFile::remove(filename)) {
        qWarning().noquote() << QString("[AppConfig] persist failed reason=%1 stage=removeTarget")
                                    .arg(reason);
        QFile::remove(tmpPath);
        return false;
    }
    if (!QFile::rename(tmpPath, filename)) {
        qWarning().noquote() << QString("[AppConfig] persist failed reason=%1 stage=rename err=%2")
                                    .arg(reason, QFile(tmpPath).errorString());
        QFile::remove(tmpPath);
        return false;
    }

    qInfo().noquote() << QString("[AppConfig] persist ok reason=%1 stateBytes=%2 geometryBytes=%3 path=%4")
                             .arg(reason)
                             .arg(m_mainWindowState.size())
                             .arg(m_mainWindowGeometry.size())
                             .arg(filename);
    return true;
}
```

- [ ] **Step 4: `saveToFile` 委托原子实现**

```cpp
bool AppConfig::saveToFile(const QString& filename)
{
    return saveToFileAtomic(filename, QStringLiteral("saveToFile"));
}
```

- [ ] **Step 5: 构建验证**

```powershell
cmd /c "cd /d d:\WS\qtpro\DeviceReceiver && build_cmake.bat"
```

Expected: `[OK] 构建成功`

- [ ] **Step 6: Commit**

```bash
git add AppConfig.h AppConfig.cpp
git commit -m "feat(配置): 新增 saveToFileAtomic 原子写入 config.ini 与备份"
```

---

### Task 2: MainWindow 持久化门禁与 `persistLayout`

**Files:**
- Modify: `MainWindow.h`
- Modify: `MainWindow.cpp`

- [ ] **Step 1: 在 `MainWindow.h` 增加成员与 API**

```cpp
public:
    /// 从 UI 采集布局并原子写入 config.ini（reason 用于日志）
    void persistLayout(const QString& reason);

private:
    class ConfigPersistSuppressor {
    public:
        explicit ConfigPersistSuppressor(MainWindow* w);
        ~ConfigPersistSuppressor();
    private:
        MainWindow* m_window = nullptr;
        bool m_prev = false;
    };

    bool m_uiReady = false;
    bool m_layoutRestored = false;
    bool m_suppressConfigPersist = false;
```

- [ ] **Step 2: 实现 `ConfigPersistSuppressor`**

```cpp
MainWindow::ConfigPersistSuppressor::ConfigPersistSuppressor(MainWindow* w)
    : m_window(w), m_prev(w->m_suppressConfigPersist)
{
    m_window->m_suppressConfigPersist = true;
}

MainWindow::ConfigPersistSuppressor::~ConfigPersistSuppressor()
{
    m_window->m_suppressConfigPersist = m_prev;
}
```

- [ ] **Step 3: 实现 `persistLayout`**

```cpp
void MainWindow::persistLayout(const QString& reason)
{
    if (!m_uiReady || !m_layoutRestored || m_suppressConfigPersist) {
        qDebug().noquote() << QString("[MainWindow] persistLayout skipped reason=%1 uiReady=%2 layoutRestored=%3 suppress=%4")
                                  .arg(reason).arg(m_uiReady).arg(m_layoutRestored).arg(m_suppressConfigPersist);
        return;
    }
    saveConfigFromUI();
    AppConfig* config = AppConfig::instance();
    if (!config) {
        return;
    }
    config->saveToFileAtomic(AppConfig::defaultConfigFilePath(), reason);
}
```

- [ ] **Step 4: `initUI` 末尾设 `m_uiReady = true`**

在 `initUI` 最后一个 `qDebug() << "[MainWindow::initUI] 完成";` **之前**：

```cpp
m_uiReady = true;
```

注意：`m_layoutRestored` 在 `initialize()` 里 `restoreSavedPlotWindowsFromConfig()` 之后置 true（Task 3）。

- [ ] **Step 5: Commit**

```bash
git add MainWindow.h MainWindow.cpp
git commit -m "feat(UI): MainWindow 增加 persistLayout 与启动期落盘门禁"
```

---

### Task 3: Dock objectName 与 `restoreState` 加固

**Files:**
- Modify: `MainWindow.cpp`（`initUI` 创建 Dock 处 + 恢复逻辑）

- [ ] **Step 1: 创建 Dock 后立即设置 objectName**

在 `m_devicePanel = new QDockWidget(...)` 之后一行：

```cpp
m_devicePanel->setObjectName(QStringLiteral("dock_device"));
```

同理：

| 变量 | objectName |
|------|------------|
| `m_commandPanel` | `dock_command` |
| `m_plotPanel` | `dock_plot` |
| `m_stagePanel` | `dock_stage` |
| `m_monitorPanel` | `dock_monitor` |
| `m_overviewPanel` | `dock_overview` |

（`m_stagePanel` 若已有 `stage_panelDock`，改为 `dock_stage` 以统一；旧 state 可能失效一次，符合 spec 风险说明。）

- [ ] **Step 2: 替换 `restoreState` 块**

将：

```cpp
if (!config->mainWindowState().isEmpty()) {
    restoreState(config->mainWindowState());
}
```

改为：

```cpp
bool dockStateOk = true;
if (!config->mainWindowState().isEmpty()) {
    dockStateOk = restoreState(config->mainWindowState());
    qInfo().noquote() << QString("[MainWindow] restoreState ok=%1 bytes=%2")
                             .arg(dockStateOk ? 1 : 0)
                             .arg(config->mainWindowState().size());
    if (!dockStateOk) {
        qWarning() << "[MainWindow] restoreState failed; fallback to Show*Panel flags only";
    }
}

if (dockStateOk) {
    // 原有 Show*Panel setVisible + raise 逻辑保持在此分支内
    showDeviceAction->setChecked(config->showDevicePanel());
    // ... 其余不变
}
else {
    // 回退：仅应用 Show* 与 geometry，不执行 raise/tabify 相关 raise 链
    m_devicePanel->setVisible(config->showDevicePanel());
    m_commandPanel->setVisible(config->showCommandPanel());
    m_plotPanel->setVisible(config->showPlotPanel());
    if (m_stagePanel) m_stagePanel->setVisible(config->showStagePanel());
    m_monitorPanel->setVisible(config->showMonitorPanel());
    if (m_overviewPanel) m_overviewPanel->setVisible(config->showOverviewPanel());
    // 同步 menu action checked 状态（与上面 setVisible 一致）
}
```

- [ ] **Step 3: `initialize()` 末尾设 `m_layoutRestored = true`**

在 `restoreSavedPlotWindowsFromConfig();` 与 `updateWindowList();` 之后：

```cpp
m_layoutRestored = true;
```

- [ ] **Step 4: `loadConfigToUI` 使用 `ConfigPersistSuppressor`**

在 `loadConfigToUI()` 函数体开头：

```cpp
ConfigPersistSuppressor suppress(this);
```

- [ ] **Step 5: `rebuildGrpcParamUI` 开头同样加 suppress**

```cpp
void MainWindow::rebuildGrpcParamUI(const QVector<BackendParamDescriptor>& params)
{
    ConfigPersistSuppressor suppress(this);
    // 现有实现...
```

- [ ] **Step 6: 构建 + Commit**

```bash
git add MainWindow.cpp
git commit -m "fix(UI): Dock 稳定 objectName 与 restoreState 失败回退"
```

---

### Task 4: 替换所有布局相关写盘调用点

**Files:**
- Modify: `MainWindow.cpp`

- [ ] **Step 1: `closeEvent`**

将：

```cpp
saveConfigFromUI();
if (config) {
    config->saveToFile(AppConfig::defaultConfigFilePath());
}
```

改为：

```cpp
persistLayout(QStringLiteral("closeEvent"));
```

（`saveConfigFromUI` 已在 `persistLayout` 内调用。）

- [ ] **Step 2: `setStyle` 末尾**

将：

```cpp
config->setCurrentStyle(style);
config->saveToFile(AppConfig::defaultConfigFilePath());
```

改为：

```cpp
config->setCurrentStyle(style);
persistLayout(QStringLiteral("styleChange"));
```

- [ ] **Step 3: `onConnectClicked`**

将：

```cpp
saveConfigFromUI();
if (config) {
    config->saveToFile(AppConfig::defaultConfigFilePath());
}
```

改为：

```cpp
persistLayout(QStringLiteral("userConnect"));
```

- [ ] **Step 4: `aboutToQuit` 连接（在 `initConnections` 末尾）**

```cpp
connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
    persistLayout(QStringLiteral("aboutToQuit"));
});
```

- [ ] **Step 5: 确认无遗漏裸 `saveToFile`**

```powershell
rg "saveToFile" MainWindow.cpp AppConfig.cpp main.cpp
```

Expected: `MainWindow.cpp` 中无直接 `saveToFile`（仅 `persistLayout` → `saveToFileAtomic`）；`setStyle` / `closeEvent` / `onConnectClicked` 已替换。

- [ ] **Step 6: 构建 + Commit**

```bash
git add MainWindow.cpp
git commit -m "fix(UI): 布局落盘统一走 persistLayout（含 close/style/aboutToQuit）"
```

---

### Task 5: 人工回归测试（必做）

**Files:** 无代码变更；记录结果到 PR/会话。

- [ ] **Step 1: 基线 — 调整 Dock 后正常关闭**

1. 启动 `build_cmake\build\release\realtime_data.exe`
2. 拖动「被测设备」到右侧、隐藏「数据监控」
3. 正常关闭窗口
4. 再启动

Expected: 布局一致；日志含 `[AppConfig] persist ok reason=closeEvent` 且 `stateBytes > 0`

- [ ] **Step 2: 验证 H1 — 切换主题后关闭**

1. 调整 Dock 布局
2. 菜单切换深色/浅色主题
3. 关闭并重启

Expected: Dock 布局仍一致；日志含 `reason=styleChange`

- [ ] **Step 3: `restoreState` 失败回退**

1. 关闭程序
2. 编辑 exe 同目录 `config.ini`，将 `UI/MainWindowState` 设为 `@Invalid()` 或清空
3. 启动

Expected: 不崩溃；日志 `[MainWindow] restoreState ok=0` 或 failed；面板按 `Show*` 显隐，默认停靠

- [ ] **Step 4: 原子写 — 检查备份**

1. 完成 Step 1 后查看 exe 目录

Expected: 存在 `config.ini.bak`；`config.ini` 非 0 字节

- [ ] **Step 5: 记录未覆盖项**

强杀（任务管理器结束）后布局可能丢失 — 与 spec 一致，在提交说明中注明。

---

## Spec 覆盖自检

| Spec 条目 | Task |
|-----------|------|
| 统一 `persistLayout` | Task 2, 4 |
| `saveToFileAtomic` + `.bak` | Task 1 |
| 启动期 `m_uiReady` / `m_layoutRestored` / suppress | Task 2, 3 |
| Dock `objectName` | Task 3 |
| `restoreState` 检查与回退 | Task 3 |
| 无 debounce 自动保存 | （无任务 — 故意不做） |
| `aboutToQuit` | Task 4 |
| 日志 `persist ok` / `restoreState ok` | Task 1, 3 |
| 无 BOM 写 | Task 1 Step 3 |

## 风险提醒（实施时注意）

- `aboutToQuit` 与 `closeEvent` 可能连续触发两次 `persistLayout`：幂等，可接受。
- 修改 Dock `objectName` 后，用户**第一次**升级可能丢失旧 `MainWindowState`（一次性），日志会有 `restoreState failed`。
- 勿在析构中调用 `persistLayout` / `saveConfigFromUI`（现有注释约束保持）。
