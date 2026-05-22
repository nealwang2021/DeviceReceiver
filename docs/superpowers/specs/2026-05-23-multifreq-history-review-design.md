# 多频涡流历史总览 / Review / HDF5 导出导入 设计

日期: 2026-05-23

## 目标

为 `multifreq_frames` 表适配历史系统的 4 项功能：历史总览、暂停 Review、离线加载 DB、HDF5 导出/导入。

## 架构：并行路径

在现有阵列涡流路径之外，新增并行多频涡流查询路径。两者互不影响。

```
HistoryOverviewWindow → HistoryDataProvider → SqlHistoryQuery
                            ├─ aligned_frames (阵列涡流，不改)
                            └─ multifreq_frames (多频涡流，新增)

PlotWindow (Review) → HistoryDataProvider
                            ├─ fetchRawChunk (阵列涡流)
                            └─ fetchMultiFreqRawChunk (多频涡流)

HistoryExportService → SqlHistoryQuery
                            ├─ exportHdf5 (阵列涡流)
                            └─ exportMultiFreqHdf5 (多频涡流)
```

## 1. SqlHistoryQuery 扩展

### 新增结构

```cpp
struct MultiFreqFrameRow {
    qint64 rowId, timestampMs, frameIndex;
    int frequencyFactor;
    double frequencyHz;
    double impedanceReal, impedanceImag, impedanceMagnitude, impedancePhaseDeg;
    double normImpedanceReal, normImpedanceImag;
    double voltageMag, currentMag;
    bool valid;
};
```

### 新增方法

```cpp
// 游标分页查询 multifreq_frames
QVector<MultiFreqFrameRow> fetchMultiFreqRawChunk(
    qint64 startMs, qint64 endMs,
    qint64 lastTimestampMs, qint64 lastRowId, int chunkSize);

// 多频总览包络
struct MultiFreqEnvelopeBucket {
    qint64 bucketStartMs;
    int frequencyFactor;
    double minImpedanceReal, maxImpedanceReal;
    double minImpedanceImag, maxImpedanceImag;
};
QVector<MultiFreqEnvelopeBucket> queryMultiFreqEnvelope(
    qint64 startMs, qint64 endMs, qint64 bucketMs);

// 行数估算
qint64 estimateMultiFreqRowCount(qint64 startMs, qint64 endMs);
```

### 性能考虑

- 每 N 行相当于 N×M 个频点记录（M 为倍频系数个数）
- 游标分页同时按 `(timestamp_unix_ms, id)` 排序，避免 OFFSET 性能退化
- 包络查询按 `(bucketStart, frequency_factor)` GROUP BY，每个分桶仅 2×N 行（MIN/MAX）
- 索引 `idx_multifreq_timestamp` 保证大时间跨度查询性能

## 2. HistoryDataProvider + HistoryOverviewWindow

### HistoryDataProvider 新增方法

```cpp
QVector<MultiFreqEnvelopeBucket> queryMultiFreqEnvelope(
    qint64 startMs, qint64 endMs, qint64 bucketMs);

QVector<MultiFreqFrameRow> fetchMultiFreqRawChunk(
    qint64 startMs, qint64 endMs,
    qint64 lastTimestampMs, qint64 lastRowId, int chunkSize);
```

### HistoryOverviewWindow

- 检测当前活跃的 `SelectionState::mode` 对应哪种设备数据
- `MultiFreqEddy` 时调用 `queryMultiFreqEnvelope`
- 包络绘制：每个 `frequencyFactor` 一条彩色包络带，横轴时间，纵轴阻抗幅值
- Brush 选区 → `commitRangeToSelectionState` → `SelectionState` 发信号（已有逻辑不变）

## 3. PlotWindow Review

`loadReviewFromDb` 新增 MultiFreqEddy 分支：

1. 获取 DB 路径 → `SqlHistoryQuery` → `fetchMultiFreqRawChunk`（游标循环）
2. 按 `frame_index` 分组累积 `MultiFreqPointResult` 列表
3. 组装为 `FrameRecord`（填充 `mfFreqPoints`，不填充 amp/phase/x/y）
4. 存入 `m_reviewFrames`
5. `sourceFramesForRender()` 和 `updateMultiFreqPlots` 无需改动——数据统一走 `FrameData` → `PlotSnapshot` 路径

## 4. HDF5 导出/导入

### HDF5 结构

```
/frames/
  timestamp_ms_utc          I64LE [N]
  frame_index               I64LE [N]
  frequency_factor          I32LE [N]
  frequency_hz              F64LE [N]

/impedance/
  real                      F64LE [N]
  imag                      F64LE [N]
  magnitude                 F64LE [N]
  phase_deg                F64LE [N]
  norm_real                 F64LE [N]
  norm_imag                 F64LE [N]

/voltage/
  magnitude                 F64LE [N]

/current/
  magnitude                 F64LE [N]
```

属性：
- `frequency_factors`: 逗号分隔的倍频系数列表
- `base_frequency_hz`: 基频

### HistoryExportService 新增

```cpp
bool exportMultiFreqHdf5(const QString& filePath, qint64 startMs, qint64 endMs);
```

### HistoryImportService 新增

```cpp
bool importMultiFreqHdf5(const QString& filePath);
```

## 文件变更

| 文件 | 操作 | 说明 |
|---|---|---|
| `SqlHistoryQuery.h/.cpp` | 修改 | 新增 MultiFreqFrameRow、fetchMultiFreqRawChunk、queryMultiFreqEnvelope |
| `HistoryDataProvider.h/.cpp` | 修改 | 新增多频查询方法 |
| `HistoryOverviewWindow.cpp` | 修改 | detectMode 分发到多频包络路径 |
| `PlotWindow.cpp` | 修改 | loadReviewFromDb 新增 MultiFreqEddy 分支 |
| `HistoryExportService.h/.cpp` | 修改 | 新增 exportMultiFreqHdf5 |
| `HistoryImportService.h/.cpp` | 修改 | 新增 importMultiFreqHdf5 |
