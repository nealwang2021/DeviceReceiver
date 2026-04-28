#ifndef DATAPROCESSOR_H
#define DATAPROCESSOR_H

#include <QObject>
#include <QTimer>
#include "FrameData.h"

// 数据处理模块（定时器批量统计）
class DataProcessor : public QObject
{
    Q_OBJECT
public:
    explicit DataProcessor(QObject *parent = nullptr);

private slots:
    void calcStats(); // 1Hz频率统计数据

private:
    QTimer* m_statTimer; // 统计定时器
    bool m_enabled = false;
};

#endif // DATAPROCESSOR_H