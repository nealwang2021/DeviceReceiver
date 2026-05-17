#ifndef STAGERECEIVERBACKEND_H
#define STAGERECEIVERBACKEND_H

#include "IReceiverBackend.h"

#include <QTimer>
#include <QString>
#include <QtGlobal>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#ifdef HAS_GRPC
#include <grpcpp/grpcpp.h>
#include "stage.grpc.pb.h"
#endif

/**
 * @brief StageService 三轴控制台接收后端
 *
 * endpoint 约定：
 *   - "grpcHost:grpcPort" 或 IPv6 "[grpcHost]:grpcPort"
 *   - "…|stageHost:stagePort"（可选，显式指定 Stage Connect 目标；IPv6 同样用方括号）
 *
 * 行为：
 *   - connectBackend：建立 gRPC Channel，并调用 StageService::Connect。
 *   - startAcquisition：启动 PositionStream（或 Mock 定时器）。
 *   - sendCommand：解析文本命令并映射到 Jog/Move/Scan 等 RPC。
 */
class StageReceiverBackend : public IReceiverBackend
{
    Q_OBJECT
public:
    explicit StageReceiverBackend(QObject* parent = nullptr);
    ~StageReceiverBackend() override;

public slots:
    // -------- IReceiverBackend 接口实现 --------
    bool connectBackend(const QString& endpoint) override;
    void disconnectBackend() override;
    Q_INVOKABLE bool isBackendConnected() const override;
    void startAcquisition(int intervalMs = 100) override;
    void stopAcquisition() override;
    void setPaused(bool paused) override;
    void sendCommand(const QByteArray& command) override;
    void sendCommand(const QString& command, bool isHex = false) override;

    // -------- Stage 专属控制 --------
    void setMockMode(bool enabled);
    void requestPositions();
    void startPositionStream(int intervalMs = 100);
    void stopPositionStream();
    void jog(int axis, bool plus, bool enable);
    void moveAbs(double xMm, double yMm, double zMm, int timeoutMs = 10000);
    void moveRel(int axis, double deltaMm, int timeoutMs = 10000);
    void setSpeed(quint32 speedPulsePerSec, quint32 accelMs);
    void startScan(int mode,
                   double xs,
                   double xe,
                   double ys,
                   double ye,
                   double yStep,
                   double zFix);
    void stopScan();
    void requestScanStatus();

signals:
    /// 三轴台位更新；unixMs 为 PositionsReply 时间戳。mm=毫米；pulse=驱动/编码器整数计数（见 stage.proto AxisValue）。
    /// 不再通过 frameReceived 投递伪通道 FrameData，避免与 DUT 多通道混淆。
    void stagePoseUpdated(double xMm, double yMm, double zMm, int xPulse, int yPulse, int zPulse, qint64 unixMs);

private slots:
    void onMockTick();
    void onReconnectCheck();

private:
    void startStreamThread(int intervalMs);
    void stopStreamThread();
    void streamLoop(int intervalMs);

    void setConnected(bool connected);
    void emitBackendStatus(const QString& status, const QString& detail);
    void emitCommandResult(bool ok, const QString& command, const QString& message);
    void emitPositionsPacket(const QString& source,
                             double xMm,
                             double yMm,
                             double zMm,
                             int xPulse,
                             int yPulse,
                             int zPulse,
                             qint64 unixMs,
                             bool forceEmit = false);
    bool shouldEmitRealtimePacket(qint64 timestampMs);
    bool parseEndpoint(const QString& endpoint,
                       QString* grpcEndpoint,
                       QString* stageIp,
                       int* stagePort) const;
    bool ensureStubReady();
    bool callConnectRpc(const QString& ip, int port);
    bool callDisconnectRpc();

private:
    QString m_endpoint;
    QString m_stageIp;
    int     m_stagePort = 5000;
    int     m_acquisitionIntervalMs = 100;

    std::atomic<bool> m_connected  {false};
    std::atomic<bool> m_paused     {false};
    std::atomic<bool> m_stopStream {false};
    std::atomic<bool> m_mockMode   {false};
    std::atomic<qint64> m_lastRealtimePacketMs {0};
    int m_realtimePacketIntervalMs = 200;

    QTimer* m_mockTimer      = nullptr;
    QTimer* m_reconnectTimer = nullptr;
    std::thread m_streamThread;
    std::mutex m_streamStateMutex;  // 保护 m_streamCtx（跨流线程 / stopStreamThread 访问）

#ifdef HAS_GRPC
    std::shared_ptr<grpc::Channel>                m_channel;
    std::unique_ptr<stage::StageService::Stub>    m_stub;
    std::unique_ptr<grpc::ClientContext>          m_streamCtx;  // GUARDED_BY(m_streamStateMutex)
#endif
};

#endif // STAGERECEIVERBACKEND_H
