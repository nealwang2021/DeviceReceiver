#ifndef IRECEIVERBACKEND_H
#define IRECEIVERBACKEND_H

#include <QObject>
#include <QByteArray>
#include "FrameData.h"
#include <QJsonObject>
#include "BackendParamDescriptor.h"

class IReceiverBackend : public QObject
{
    Q_OBJECT
public:
    explicit IReceiverBackend(QObject* parent = nullptr) : QObject(parent) {}
    ~IReceiverBackend() override = default;

    /// 返回可配置参数描述符列表（由子类重写）。默认返回空列表。
    virtual QVector<BackendParamDescriptor> configParameters() const;

public slots:
    virtual bool connectBackend(const QString& endpoint) = 0;
    virtual void disconnectBackend() = 0;
    virtual bool isBackendConnected() const = 0;
    virtual void startAcquisition(int intervalMs = 100) = 0;
    virtual void stopAcquisition() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void sendCommand(const QByteArray& command) = 0;
    virtual void sendCommand(const QString& command, bool isHex = false) = 0;

signals:
    void frameReceived(const FrameData& frame);
    void commandSent(const QByteArray& command);
    void commandError(const QString& error);
    void dataReceived(const QByteArray& data, bool isHex = false);
    /// 后端连接状态变化（true = 已连接，false = 已断开）
    void connectionStateChanged(bool connected);
    /// 设备状态更新（JSON 对象，字段由各后端自由定义）
    void backendStatusChanged(const QJsonObject& status);
};

inline QVector<BackendParamDescriptor> IReceiverBackend::configParameters() const { return {}; }

#endif // IRECEIVERBACKEND_H
