#ifndef GRPCMULTIFREQBACKEND_H
#define GRPCMULTIFREQBACKEND_H

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
#include "multifreq_eddy.grpc.pb.h"
#endif

/**
 * @brief 多频涡流 gRPC 接收后端（multifreq_eddy.proto: MultiFreqEddyCurrent）
 *
 * 支持两种工作模式：
 *   - Mock 模式（m_mockMode=true）：QTimer 驱动，本地生成含合成阻抗频点的伪帧。
 *   - Real 模式（m_mockMode=false）：订阅服务端流式 RPC（StreamFrames），
 *     在独立的 std::thread 中阻塞读取 DetectionFrame，通过 Qt::QueuedConnection 回传主线程。
 *
 * 线程安全：
 *   m_connected / m_paused / m_stopStream / m_mockMode 均为 std::atomic<bool>，
 *   可在 Qt 工作线程与流线程之间安全读写。
 */
class GrpcMultiFreqBackend : public IReceiverBackend
{
    Q_OBJECT
public:
    explicit GrpcMultiFreqBackend(QObject* parent = nullptr);
    ~GrpcMultiFreqBackend() override;

    QVector<BackendParamDescriptor> configParameters() const override;

public slots:
    // -------- IReceiverBackend 接口实现 --------
    bool connectBackend(const QString& endpoint) override;
    void disconnectBackend() override;
    bool isBackendConnected() const override;
    void startAcquisition(int intervalMs = 100) override;
    void stopAcquisition() override;
    void setPaused(bool paused) override;
    void sendCommand(const QByteArray& command) override;
    void sendCommand(const QString& command, bool isHex = false) override;

    // -------- gRPC 专属控制 --------
    void setMockMode(bool enabled);
    void setConnectTimeoutMs(int ms);

signals:
    void connectAttemptFinished(bool connected, const QString& detail);

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
    int     m_acquisitionIntervalMs = 100;
    int     m_connectTimeoutMs = 6000;

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
    std::unique_ptr<multifreqeddy::MultiFreqEddyCurrent::Stub> m_stub;
    std::unique_ptr<grpc::ClientContext> m_streamCtx;
#endif
};

#endif // GRPCMULTIFREQBACKEND_H
