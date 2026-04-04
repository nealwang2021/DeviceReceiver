#ifndef GRPCRECEIVERBACKEND_H
#define GRPCRECEIVERBACKEND_H

#include "IReceiverBackend.h"

#include <QTimer>
#include <QString>
#include <atomic>
#include <memory>
#include <thread>

#ifdef HAS_GRPC
#include <grpcpp/grpcpp.h>
#include "device.grpc.pb.h"
#endif

/**
 * @brief gRPC 接收后端
 *
 * 支持两种工作模式：
 *   - Mock 模式（m_mockMode=true）：QTimer 驱动，本地生成伪随机帧，无需真实服务端。
 *   - Real 模式（m_mockMode=false）：订阅服务端流式 RPC（AcquisitionDevice::SubscribeProcessedFrames），
 *     在独立的 std::thread 中阻塞读取 ProcessedFrameReply，通过 Qt::QueuedConnection 回传主线程。
 *
 * 线程安全：
 *   m_connected / m_paused / m_stopStream / m_mockMode 均为 std::atomic<bool>，
 *   可在 Qt 工作线程与流线程之间安全读写。
 */
class GrpcReceiverBackend : public IReceiverBackend
{
    Q_OBJECT
public:
    explicit GrpcReceiverBackend(QObject* parent = nullptr);
    ~GrpcReceiverBackend() override;

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
    /// 切换 mock / real 模式（切换后需重新 connectBackend）
    void setMockMode(bool enabled);

private slots:
    void onMockTick();          ///< Mock 模式：定时生成帧
    void onReconnectCheck();    ///< Real 模式：检查 Channel 状态并按需重连

private:
    // -------- 真实模式流线程 --------
    void startStreamThread(int intervalMs);
    void stopStreamThread();
    /// 流线程主循环（在 m_streamThread 内执行，阻塞直到完成/取消/出错）
    void streamLoop(int intervalMs);

    // -------- 辅助 --------
    void setConnected(bool connected); ///< 统一更新 m_connected 并 emit connectionStateChanged
    void emitBackendStatus(const QString& status, const QString& detail);
    bool shouldEmitRealtimePacket(qint64 timestampMs);

    // -------- 配置 --------
    QString m_endpoint;
    int     m_acquisitionIntervalMs = 100;

    // -------- 原子状态（跨线程安全）--------
    std::atomic<bool> m_connected  {false};
    std::atomic<bool> m_paused     {false};
    std::atomic<bool> m_stopStream {false};
    std::atomic<bool> m_mockMode   {true};
    std::atomic<qint64> m_lastRealtimePacketMs {0};
    /// UI 侧 streamFrame JSON 节流（不影响 frameReceived / 缓存 / 热力图）
    int m_realtimePacketIntervalMs = 100;

    // -------- Mock 模式定时器 --------
    QTimer*  m_mockTimer    = nullptr;
    quint32  m_frameCounter = 0;

    // -------- Real 模式：断线重连定时器 --------
    QTimer*  m_reconnectTimer = nullptr;

    // -------- Real 模式：流线程 --------
    std::thread m_streamThread;

#ifdef HAS_GRPC
    std::shared_ptr<grpc::Channel>                         m_channel;
    std::unique_ptr<xiaoche::device::AcquisitionDevice::Stub>  m_stub;
    std::unique_ptr<grpc::ClientContext>                   m_streamCtx;
#endif
};

#endif // GRPCRECEIVERBACKEND_H
