# 多频涡流历史总览 / Review / HDF5 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 `multifreq_frames` 表实现历史总览、暂停 Review、离线加载、HDF5 导出导入。

**Architecture:** 在 SqlHistoryQuery / HistoryDataProvider 中新增并行多频查询路径，HistoryOverviewWindow / PlotWindow / ExportService 按 detectMode 分发。

**Tech Stack:** C++17, Qt 5.15, SQLite, HDF5

---

### Task 1: SqlHistoryQuery 新增多频查询

**Files:**
- Modify: `SqlHistoryQuery.h`
- Modify: `SqlHistoryQuery.cpp`

- [ ] **Step 1: 在 SqlHistoryQuery.h 新增结构体和方法声明**

在 `AlignedFrameRow` 结构体之后添加：

```cpp
struct MultiFreqFrameRow
{
    qint64 rowId = 0;
    qint64 timestampMs = 0;
    qint64 frameIndex = 0;
    int     frequencyFactor = 0;
    double  frequencyHz = 0.0;
    double  impedanceReal = 0.0;
    double  impedanceImag = 0.0;
    double  impedanceMagnitude = 0.0;
    double  impedancePhaseDeg = 0.0;
    double  normImpedanceReal = 0.0;
    double  normImpedanceImag = 0.0;
    double  voltageMag = 0.0;
    double  currentMag = 0.0;
    bool    valid = false;
};

struct MultiFreqEnvelopeBucket
{
    qint64 bucketStartMs = 0;
    int    frequencyFactor = 0;
    double minImpedanceReal = 0.0;
    double maxImpedanceReal = 0.0;
    double minImpedanceImag = 0.0;
    double maxImpedanceImag = 0.0;
};
```

在类中新增方法声明（`queryOverviewEnvelope` 等方法附近）：

```cpp
QVector<MultiFreqFrameRow> fetchMultiFreqRawChunk(
    qint64 startMs, qint64 endMs,
    qint64 lastTimestampMs, qint64 lastRowId, int chunkSize);

QVector<MultiFreqEnvelopeBucket> queryMultiFreqOverviewEnvelope(
    qint64 startMs, qint64 endMs, qint64 bucketMs);

qint64 estimateMultiFreqRowCount(qint64 startMs, qint64 endMs);
```

- [ ] **Step 2: 实现 fetchMultiFreqRawChunk**

```cpp
QVector<MultiFreqFrameRow> SqlHistoryQuery::fetchMultiFreqRawChunk(
    qint64 startMs, qint64 endMs,
    qint64 lastTimestampMs, qint64 lastRowId, int chunkSize)
{
    QVector<MultiFreqFrameRow> rows;
    if (!m_db.isOpen()) return rows;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, timestamp_unix_ms, frame_index, frequency_factor, frequency_hz, "
        "impedance_real, impedance_imag, impedance_magnitude, impedance_phase_deg, "
        "normalized_impedance_real, normalized_impedance_imag, "
        "voltage_magnitude, current_magnitude, valid "
        "FROM multifreq_frames "
        "WHERE timestamp_unix_ms BETWEEN :start AND :end "
        "AND (timestamp_unix_ms > :lastTs "
        "OR (timestamp_unix_ms = :lastTsEq AND id > :lastRowId)) "
        "ORDER BY timestamp_unix_ms ASC, id ASC "
        "LIMIT :chunkSize"));
    q.bindValue(":start", startMs);
    q.bindValue(":end", endMs);
    q.bindValue(":lastTs", lastTimestampMs);
    q.bindValue(":lastTsEq", lastTimestampMs);
    q.bindValue(":lastRowId", lastRowId);
    q.bindValue(":chunkSize", chunkSize);

    if (!q.exec()) return rows;

    while (q.next()) {
        MultiFreqFrameRow r;
        r.rowId = q.value(0).toLongLong();
        r.timestampMs = q.value(1).toLongLong();
        r.frameIndex = q.value(2).toLongLong();
        r.frequencyFactor = q.value(3).toInt();
        r.frequencyHz = q.value(4).toDouble();
        r.impedanceReal = q.value(5).toDouble();
        r.impedanceImag = q.value(6).toDouble();
        r.impedanceMagnitude = q.value(7).toDouble();
        r.impedancePhaseDeg = q.value(8).toDouble();
        r.normImpedanceReal = q.value(9).toDouble();
        r.normImpedanceImag = q.value(10).toDouble();
        r.voltageMag = q.value(11).toDouble();
        r.currentMag = q.value(12).toDouble();
        r.valid = q.value(13).toBool();
        rows.append(r);
    }
    return rows;
}
```

- [ ] **Step 3: 实现 queryMultiFreqOverviewEnvelope**

```cpp
QVector<MultiFreqEnvelopeBucket> SqlHistoryQuery::queryMultiFreqOverviewEnvelope(
    qint64 startMs, qint64 endMs, qint64 bucketMs)
{
    QVector<MultiFreqEnvelopeBucket> result;
    if (!m_db.isOpen() || bucketMs <= 0) return result;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT (timestamp_unix_ms / :bucket) * :bucket AS bucket_start, "
        "frequency_factor, "
        "MIN(impedance_real), MAX(impedance_real), "
        "MIN(impedance_imag), MAX(impedance_imag) "
        "FROM multifreq_frames "
        "WHERE timestamp_unix_ms BETWEEN :start AND :end "
        "GROUP BY bucket_start, frequency_factor "
        "ORDER BY bucket_start ASC, frequency_factor ASC"));
    q.bindValue(":bucket", bucketMs);
    q.bindValue(":start", startMs);
    q.bindValue(":end", endMs);

    if (!q.exec()) return result;

    while (q.next()) {
        MultiFreqEnvelopeBucket b;
        b.bucketStartMs = q.value(0).toLongLong();
        b.frequencyFactor = q.value(1).toInt();
        b.minImpedanceReal = q.value(2).toDouble();
        b.maxImpedanceReal = q.value(3).toDouble();
        b.minImpedanceImag = q.value(4).toDouble();
        b.maxImpedanceImag = q.value(5).toDouble();
        result.append(b);
    }
    return result;
}
```

- [ ] **Step 4: 实现 estimateMultiFreqRowCount**

```cpp
qint64 SqlHistoryQuery::estimateMultiFreqRowCount(qint64 startMs, qint64 endMs)
{
    if (!m_db.isOpen()) return 0;
    const qint64 spanSec = (endMs - startMs) / 1000;
    if (spanSec <= 3600) {
        QSqlQuery q(m_db);
        q.prepare("SELECT COUNT(*) FROM multifreq_frames WHERE timestamp_unix_ms BETWEEN :start AND :end");
        q.bindValue(":start", startMs);
        q.bindValue(":end", endMs);
        if (q.exec() && q.next()) return q.value(0).toLongLong();
    }
    return spanSec * 40; // 约 10fps × 4 频点 = 40 行/秒
}
```

- [ ] **Step 5: 构建提交**

```powershell
Set-Location "d:\WS\qtpro\DeviceReceiver"; .\build_cmake.bat
```

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add SqlHistoryQuery.h SqlHistoryQuery.cpp && git commit -m "feat(历史): SqlHistoryQuery 新增 multifreq_frames 查询路径"
```

---

### Task 2: HistoryDataProvider 新增多频方法

**Files:**
- Modify: `HistoryDataProvider.h`
- Modify: `HistoryDataProvider.cpp`

- [ ] **Step 1: 在 HistoryDataProvider.h 新增方法声明和类型转发**

```cpp
struct MultiFreqFrameRow;
struct MultiFreqEnvelopeBucket;

// 在类中新增：
bool openDatabase(const QString& dbPath, FrameData::DetectionMode* outMode = nullptr);
QVector<MultiFreqFrameRow> fetchMultiFreqRawChunk(
    qint64 startMs, qint64 endMs, qint64 lastTimestampMs, qint64 lastRowId, int chunkSize);
QVector<MultiFreqEnvelopeBucket> queryMultiFreqOverviewEnvelope(
    qint64 startMs, qint64 endMs, qint64 bucketMs);
qint64 estimateMultiFreqRowCount(qint64 startMs, qint64 endMs);
```

- [ ] **Step 2: 实现 HistoryDataProvider 中的委托方法**

```cpp
QVector<MultiFreqFrameRow> HistoryDataProvider::fetchMultiFreqRawChunk(
    qint64 startMs, qint64 endMs, qint64 lastTimestampMs, qint64 lastRowId, int chunkSize)
{
    if (!m_query) return {};
    return m_query->fetchMultiFreqRawChunk(startMs, endMs, lastTimestampMs, lastRowId, chunkSize);
}

QVector<MultiFreqEnvelopeBucket> HistoryDataProvider::queryMultiFreqOverviewEnvelope(
    qint64 startMs, qint64 endMs, qint64 bucketMs)
{
    if (!m_query) return {};
    return m_query->queryMultiFreqOverviewEnvelope(startMs, endMs, bucketMs);
}

qint64 HistoryDataProvider::estimateMultiFreqRowCount(qint64 startMs, qint64 endMs)
{
    if (!m_query) return 0;
    return m_query->estimateMultiFreqRowCount(startMs, endMs);
}
```

- [ ] **Step 3: 构建提交**

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add HistoryDataProvider.h HistoryDataProvider.cpp && git commit -m "feat(历史): HistoryDataProvider 新增多频查询委托方法"
```

---

### Task 3: HistoryOverviewWindow 多频包络

**Files:**
- Modify: `HistoryOverviewWindow.h`
- Modify: `HistoryOverviewWindow.cpp`

- [ ] **Step 1: 新增多频包络绘制方法声明**

在 HistoryOverviewWindow.h 中新增：
```cpp
void rebuildMultiFreqEnvelope();
QVector<QCPGraph*> m_mfEnvelopeGraphs;
QVector<QCPGraph*> m_mfEnvelopeMaxGraphs;
QMap<int, QColor> m_mfFreqColors;
```

- [ ] **Step 2: 实现 rebuildMultiFreqEnvelope**

```cpp
void HistoryOverviewWindow::rebuildMultiFreqEnvelope()
{
    auto* hdp = HistoryDataProvider::instance();
    if (!hdp || !hdp->isOpen()) return;

    const qint64 start = m_dataMinMs > 0 ? m_dataMinMs : 0;
    const qint64 end = m_dataMaxMs > 0 ? m_dataMaxMs : QDateTime::currentMSecsSinceEpoch();
    const qint64 span = end - start;
    const qint64 bucket = qMax(1000LL, span / 500);

    const auto buckets = hdp->queryMultiFreqOverviewEnvelope(start, end, bucket);
    if (buckets.isEmpty()) return;

    m_mfEnvelopeGraphs.clear();
    m_mfEnvelopeMaxGraphs.clear();

    QSet<int> freqFactors;
    for (const auto& b : buckets) freqFactors.insert(b.frequencyFactor);

    int colorIdx = 0;
    for (int factor : freqFactors) {
        QColor c = QColor::fromHsv((colorIdx * 47) % 360, 200, 200);
        m_mfFreqColors[factor] = c;
        colorIdx++;

        QVector<double> times, mins, maxs;
        for (const auto& b : buckets) {
            if (b.frequencyFactor != factor) continue;
            const double t = static_cast<double>(b.bucketStartMs) / 1000.0;
            const double magMin = std::hypot(b.minImpedanceReal, b.minImpedanceImag);
            const double magMax = std::hypot(b.maxImpedanceReal, b.maxImpedanceImag);
            times.append(t);
            mins.append(magMin);
            maxs.append(magMax);
        }

        auto* gMin = m_plot->addGraph();
        gMin->setPen(QPen(c, 1));
        gMin->setData(times, mins, true);

        auto* gMax = m_plot->addGraph();
        gMax->setPen(QPen(c, 1));
        gMax->setData(times, maxs, true);

        m_mfEnvelopeGraphs.append(gMin);
        m_mfEnvelopeMaxGraphs.append(gMax);
    }

    m_plot->xAxis->setLabel(QStringLiteral("时间 (s)"));
    m_plot->yAxis->setLabel(QStringLiteral("阻抗幅值 (Ω)"));
    m_plot->rescaleAxes();
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}
```

- [ ] **Step 3: updateEnvelope 中按 detectMode 分发**

找到现有的 `updateEnvelope` / `rebuildEnvelope` 方法，在入口处添加：

```cpp
// 检测表类型：优先尝试 multifreq_frames
{
    QSqlQuery test(m_db);
    if (test.exec("SELECT COUNT(*) FROM multifreq_frames LIMIT 1") && test.next() && test.value(0).toInt() > 0) {
        rebuildMultiFreqEnvelope();
        return;
    }
}
// 否则走原有 aligned_frames 路径
```

- [ ] **Step 4: 构建提交**

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add HistoryOverviewWindow.h HistoryOverviewWindow.cpp && git commit -m "feat(历史): 历史总览支持多频涡流包络"
```

---

### Task 4: PlotWindow Review 多频数据加载

**Files:**
- Modify: `PlotWindow.cpp`

- [ ] **Step 1: 在 PlotWindow 中新增 loadMultiFreqReviewFromDb**

```cpp
void PlotWindow::loadMultiFreqReviewFromDb()
{
    auto* hdp = HistoryDataProvider::instance();
    if (!hdp || !hdp->isOpen()) return;

    m_reviewFrames.clear();
    const qint64 startMs = m_reviewStartMs;
    const qint64 endMs = m_reviewEndMs;

    auto* sel = SelectionState::instance();
    // 异步加载
    QtConcurrent::run([this, hdp, startMs, endMs]() {
        SqlHistoryQuery query;
        if (!query.open(hdp->currentDatabasePath())) return;

        const qint64 totalRows = query.estimateMultiFreqRowCount(startMs, endMs);
        const int maxPoints = 5000;
        const int stride = qMax(1, static_cast<int>(totalRows / maxPoints));

        QMap<qint64, QVector<MultiFreqFrameRow>> frameGroups; // frameIndex -> rows
        qint64 lastTs = startMs - 1;
        qint64 lastRowId = std::numeric_limits<qint64>::min();

        int rowIdx = 0;
        while (true) {
            const auto rows = query.fetchMultiFreqRawChunk(startMs, endMs, lastTs, lastRowId, 500);
            if (rows.isEmpty()) break;
            for (const auto& r : rows) {
                if (rowIdx++ % stride != 0) continue;
                frameGroups[r.frameIndex].append(r);
            }
            lastTs = rows.last().timestampMs;
            lastRowId = rows.last().rowId;
        }

        QVector<FrameRecord> reviewFrames;
        for (auto it = frameGroups.begin(); it != frameGroups.end(); ++it) {
            const auto& group = it.value();
            FrameRecord rec;
            rec.timestampMs = group.first().timestampMs;
            rec.sequence = group.first().frameIndex;
            rec.amp.clear();
            rec.phase.clear();
            rec.x.clear();
            rec.y.clear();
            // 将频点行转为 MultiFreqPointResult 列表，存入 FrameData（通过 FrameRecord 暂时不扩展）
            // 这里使用 PlotWindow 内部成员来存储 review frames
            // ...
        }
    });
}
```

考虑到 FrameRecord 目前只有 amp/phase/x/y 字段，需要扩展以支持 mfFreqPoints。改为：

在 PlotWindow.h 的 FrameRecord 中新增：
```cpp
QVector<MultiFreqPointResult> mfFreqPoints;
```

在 `loadMultiFreqReviewFromDb` 中填充：

```cpp
FrameRecord rec;
rec.timestampMs = group.first().timestampMs;
rec.sequence = group.first().frameIndex;
for (const auto& mfRow : group) {
    MultiFreqPointResult pt;
    pt.frequencyFactor = mfRow.frequencyFactor;
    pt.frequencyHz = mfRow.frequencyHz;
    pt.impedanceReal_raw = mfRow.impedanceReal;
    pt.impedanceImag_raw = mfRow.impedanceImag;
    pt.impedanceMagnitude = mfRow.impedanceMagnitude;
    pt.impedancePhaseDeg = mfRow.impedancePhaseDeg;
    pt.normalizedImpedanceReal = mfRow.normImpedanceReal;
    pt.normalizedImpedanceImag = mfRow.normImpedanceImag;
    pt.voltageMagnitude = mfRow.voltageMag;
    pt.currentMagnitude = mfRow.currentMag;
    pt.valid = mfRow.valid;
    rec.mfFreqPoints.append(pt);
}
reviewFrames.append(rec);
```

- [ ] **Step 2: 修改 sourceFramesForRender 支持 review 模式**

```cpp
const QVector<FrameRecord>& PlotWindow::sourceFramesForRender() const
{
    return m_reviewMode ? m_reviewFrames : m_frames;
}
```

当 `m_reviewMode && !m_reviewFrames.isEmpty()` 时，`updateMultiFreqPlots` 需要从 `m_reviewFrames` 构建临时的 `PlotSnapshot`。

- [ ] **Step 3: 构建提交**

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add PlotWindow.h PlotWindow.cpp && git commit -m "feat(Review): PlotWindow 支持多频涡流 Review 数据加载"
```

---

### Task 5: HistoryExportService HDF5 多频导出

**Files:**
- Modify: `HistoryExportService.h`
- Modify: `HistoryExportService.cpp`

- [ ] **Step 1: 声明导出方法**

```cpp
// 多频涡流 HDF5 导出
bool exportMultiFreqHdf5(const QString& filePath, qint64 startMs, qint64 endMs,
                         const QString& dbPath = QString(),
                         HistoryDataProvider::SourceMode mode = HistoryDataProvider::SessionRealtime);
```

- [ ] **Step 2: 实现**

```cpp
bool HistoryExportService::exportMultiFreqHdf5(const QString& filePath,
    qint64 startMs, qint64 endMs, const QString& dbPath,
    HistoryDataProvider::SourceMode mode)
{
#ifdef HAS_HDF5
    hid_t file = H5Fcreate(filePath.toUtf8().constData(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    // ... open db, query multifreq_frames ...
    // ... write /frames/* datasets ...
    // ... write /impedance/*, /voltage/*, /current/* datasets ...
    // ... write attributes ...
    H5Fclose(file);
    return true;
#else
    return false;
#endif
}
```

具体实现参照现有 `exportHdf5` 方法，将列改为 `multifreq_frames` 的 13 列。

- [ ] **Step 3: 构建提交**

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add HistoryExportService.h HistoryExportService.cpp && git commit -m "feat(导出): HistoryExportService 新增多频涡流 HDF5 导出"
```

---

### Task 6: HistoryImportService HDF5 多频导入

**Files:**
- Modify: `HistoryImportService.h`
- Modify: `HistoryImportService.cpp`

- [ ] **Step 1: 实现 importMultiFreqHdf5**

读取 Task 5 导出的 HDF5 结构，逆转为 `multifreq_frames` 表的 INSERT。

- [ ] **Step 2: 构建提交**

```bash
cd "d:\WS\qtpro\DeviceReceiver" && git add HistoryImportService.h HistoryImportService.cpp && git commit -m "feat(导入): HistoryImportService 新增多频涡流 HDF5 导入"
```

---

### Task 7: 端到端构建验证

**Files:** (验证所有修改编译通过)

- [ ] **Step 1: 构建**

```powershell
Set-Location "d:\WS\qtpro\DeviceReceiver"; .\build_cmake.bat
```

- [ ] **Step 2: 功能验证**

1. 启动多频涡流设备 → 采集数据 → 确认 `multifreq_frames` 表有数据
2. 打开历史总览 → 确认多频包络显示
3. Brush 选区 → 切换到 Review 模式 → 确认阻抗图显示历史数据
4. 导出 HDF5 → 确认文件包含 `/frames`, `/impedance`, `/voltage`, `/current`
5. 关闭程序 → 重新打开 → 离线加载 DB → 确认历史数据可查看
