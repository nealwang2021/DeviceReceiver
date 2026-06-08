#ifndef GRPCPULSEEDDYBACKEND_H
#define GRPCPULSEEDDYBACKEND_H

#include "IReceiverBackend.h"
#include "BackendParamDescriptor.h"

#include <QTimer>
#include <QString>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#ifdef HAS_GRPC
#include <grpcpp/grpcpp.h>
#include "pulse_eddy.grpc.pb.h"
#endif

/**
 * @brief 脉冲涡流 gRPC 接收后端（pulse_eddy.proto: PulseEddy）
 *
 * 支持两种工作模式：
 *   - Mock 模式（m_mockMode=true）：QTimer 驱动，本地生成合成脉冲曲线。
 *   - Real 模式（m_mockMode=false）：ListDevices → OpenDevice → StartAcquisition →
 *     StreamFrames 在独立 std::thread 中阻塞读取 PulseFrame，通过 Qt::QueuedConnection 回传。
 *
 * 参考线：
 *   sendCommand("start_reference") → StartReferenceCapture(frameCount)
 *   sendCommand("clear_reference")  → ClearReference()
 *   参考线数据随 PulseFrame.reference_values 下发，has_reference 标记有效性。
 */
class GrpcPulseEddyBackend : public IReceiverBackend
{
    Q_OBJECT
public:
    explicit GrpcPulseEddyBackend(QObject* parent = nullptr);
    ~GrpcPulseEddyBackend() override;

    QVector<BackendParamDescriptor> configParameters() const override;

public slots:
    bool connectBackend(const QString& endpoint) override;
    void disconnectBackend() override;
    bool isBackendConnected() const override;
    void startAcquisition(int intervalMs = 100) override;
    void stopAcquisition() override;
    void setPaused(bool paused) override;
    void sendCommand(const QByteArray& command) override;
    void sendCommand(const QString& command, bool isHex = false) override;

    void setMockMode(bool enabled);
    void setConnectTimeoutMs(int ms);

signals:
    void connectAttemptFinished(bool connected, const QString& detail);
    /// ListDevices 完成后发射，用于 UI 填充设备下拉列表
    void availableDevicesChanged(QStringList devices);

private slots:
    void onMockTick();
    void onReconnectCheck();

private:
    void startStreamThread(int intervalMs);
    void stopStreamThread();
    void streamLoop(int intervalMs);

    void setConnected(bool connected);
    void emitBackendStatus(const QString& status, const QString& detail);
    void emitDeviceStatus();
    bool shouldEmitRealtimePacket(qint64 timestampMs);

    QString m_endpoint;
    int     m_deviceIndex   = -1;
    int     m_baudrate      = 1000000;
    int     m_acquisitionIntervalMs = 100;
    int     m_connectTimeoutMs = 6000;

    QStringList m_availableDevices;
    QStringList m_availableDeviceKeys; // "0", "1", ...

    std::atomic<bool> m_connected  {false};
    std::atomic<bool> m_paused     {false};
    std::atomic<bool> m_stopStream {false};
    std::atomic<bool> m_mockMode   {false};
    std::atomic<qint64> m_lastRealtimePacketMs {0};
    std::atomic<qint64> m_streamStartMs {0};
    std::atomic<qint64> m_lastFrameReceivedMs {0};
    int m_realtimePacketIntervalMs = 100;

    QTimer*  m_mockTimer    = nullptr;
    quint64  m_frameCounter = 0;

    QTimer*  m_reconnectTimer = nullptr;

    std::thread m_streamThread;
    std::mutex m_streamStateMutex;
    std::atomic<bool> m_disconnectInProgress {false};
    std::atomic<bool> m_cancelConnect {false};

#ifdef HAS_GRPC
    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<pulseeddy::PulseEddy::Stub> m_stub;
    std::unique_ptr<grpc::ClientContext> m_streamCtx;
#endif
};

#endif // GRPCPULSEEDDYBACKEND_H
