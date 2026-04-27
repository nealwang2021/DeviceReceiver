#ifndef HISTORYIMPORTSERVICE_H
#define HISTORYIMPORTSERVICE_H

#include <QAtomicInt>
#include <QObject>
#include <QString>

class HistoryImportService : public QObject
{
    Q_OBJECT
public:
    enum class Format {
        Csv = 0,
        Hdf5 = 1,
    };
    Q_ENUM(Format)

    struct Request {
        QString sourcePath;       // .csv / .h5 / .hdf5
        QString targetDbPath;     // 新建并导入的 sqlite 路径
        Format format = Format::Csv;
        int chunkSize = 2000;     // 导入批大小
    };

    explicit HistoryImportService(QObject* parent = nullptr);
    ~HistoryImportService() override;

    void setRequest(const Request& request) { m_request = request; }
    const Request& request() const { return m_request; }

public slots:
    void run();
    void cancel();

signals:
    void progress(qint64 writtenRows, qint64 totalRows);
    void finished(bool ok, QString message);

private:
    struct RowPayload {
        qint64 frameSequence = 0;
        qint64 timestampMs = 0;
        int cellCount = 0;
        int detectMode = 0;
        QString sourceTag;
    };

    Request m_request;
    QAtomicInt m_canceled;

    bool importCsv(QString* errorMessage);
    bool importHdf5(QString* errorMessage);

    bool initTargetDb(class QSqlDatabase& db, QString* errorMessage) const;
    bool insertRow(class QSqlQuery& insertQuery,
                   const RowPayload& payload,
                   const class QVector<class QVariant>& values,
                   QString* errorMessage) const;
};

#endif // HISTORYIMPORTSERVICE_H
