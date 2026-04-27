#ifndef HISTORYEXPORTSERVICE_H
#define HISTORYEXPORTSERVICE_H

#include <QAtomicInt>
#include <QObject>
#include <QString>

/**
 * 历史数据导出 Worker（运行在独立 QThread 上）。
 *
 * - 从只读 SQLite（aligned_frames 表）按 (timestamp, frame_sequence) 游标分块读取原始行。
 * - 边读边写：CSV 以文本追加；HDF5 以 chunked unlimited dataset 逐批 H5Dset_extent + H5Dwrite。
 * - 通过 progress(written,total) 信号向 UI 汇报；total==0 时 UI 应显示 busy 状态。
 * - cancel() 线程安全：每个 chunk 结束后原子检查；取消即删除部分写出的文件。
 *
 * 导出 schema（v3）与 aligned_frames 表 1:1 对齐：amp/phase/x/y/source_channel × 40；
 * stage_pose/* 列与数据集保留但恒填空值，预留向前兼容位。
 */
class HistoryExportService : public QObject
{
    Q_OBJECT
public:
    enum class Format {
        Csv = 0,
        Hdf5 = 1,
    };
    Q_ENUM(Format)

    struct Request {
        QString dbPath;             // 源 SQLite 路径（只读打开）
        QString outputPath;         // 目标文件
        Format format = Format::Csv;
        qint64 startMs = 0;
        qint64 endMs = 0;
        QString sourceTag;          // 写入文件头（用于标识 source_mode 等）
        QString sourceMode;         // "SessionRealtime" / "OfflineExternal"
        qint64 estimatedTotal = 0;  // UI 预估值，用于进度 maximum；0 表示未知
        int chunkSize = 8192;       // 每批读取行数
    };

    explicit HistoryExportService(QObject* parent = nullptr);
    ~HistoryExportService() override;

    /// 设置请求参数（只在 start() 前调用）。
    void setRequest(const Request& request) { m_request = request; }
    const Request& request() const { return m_request; }

public slots:
    /// 在 Worker 线程执行（由 QueuedConnection 调用）。内部完整完成读写并 emit finished。
    void run();

    /// 从任意线程调用；下一个 chunk 结束时生效。
    void cancel();

signals:
    /// written: 已写出的行数；total: 估算或精确上限（=0 表示未知，UI 显示 busy）。
    void progress(qint64 written, qint64 total);

    /// ok=true 时 message 可为空；ok=false 时 message 为错误原因（UI 弹 warning）。
    void finished(bool ok, QString message);

private:
    Request m_request;
    QAtomicInt m_canceled;

    bool exportCsv(QString* errorMessage);
    bool exportHdf5(QString* errorMessage);
};

#endif // HISTORYEXPORTSERVICE_H
