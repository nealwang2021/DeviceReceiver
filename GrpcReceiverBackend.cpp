#include "GrpcReceiverBackend.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QUuid>
#include <QtGlobal>

#ifdef HAS_GRPC
#include <chrono>
#include <grpcpp/grpcpp.h>
#include "device_data.pb.h"
#include "device_data.grpc.pb.h"
#endif

// ============================================================
// 构造 / 析构
// ============================================================

GrpcReceiverBackend::GrpcReceiverBackend(QObject* parent)
    : IReceiverBackend(parent)
    , m_mockTimer(new QTimer(this))
    , m_reconnectTimer(new QTimer(this))
{
    connect(m_mockTimer,      &QTimer::timeout, this, &GrpcReceiverBackend::onMockTick);
    connect(m_reconnectTimer, &QTimer::timeout, this, &GrpcReceiverBackend::onReconnectCheck);
}

GrpcReceiverBackend::~GrpcReceiverBackend()
{
    // 确保流线程安全退出后再析构
    disconnectBackend();
}

// ============================================================
// IReceiverBackend 接口实现
// ============================================================

bool GrpcReceiverBackend::connectBackend(const QString& endpoint)
{
    m_endpoint = endpoint.trimmed();
    if (m_endpoint.isEmpty()) {
        emit commandError("gRPC endpoint 不能为空");
        return false;
    }

    // ---- Mock 模式：直接标记已连接，无需网络 ----
    if (m_mockMode.load()) {
        setConnected(true);
        emitBackendStatus("connected", "gRPC Mock 模式已就绪");
        return true;
    }

#ifdef HAS_GRPC
    // ---- Real 模式：建立 Channel 并等待连接 ----
    m_channel = grpc::CreateChannel(
        m_endpoint.toStdString(),
        grpc::InsecureChannelCredentials()
    );
    if (!m_channel) {
        emit commandError("创建 gRPC Channel 失败");
        return false;
    }

    const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(2000);
    if (!m_channel->WaitForConnected(deadline)) {
        emit commandError(QString("连接 gRPC 服务端超时: %1").arg(m_endpoint));
        m_channel.reset();
        return false;
    }

    // 创建强类型 Stub（每次连接时重建）
    m_stub = device_data::DeviceDataService::NewStub(m_channel);

    setConnected(true);
    emitBackendStatus("connected", QString("已连接 gRPC 服务端: %1").arg(m_endpoint));
    return true;
#else
    emit commandError("当前构建未启用 gRPC 支持（请以 CONFIG+=grpc_client 重新编译）");
    return false;
#endif
}

void GrpcReceiverBackend::disconnectBackend()
{
    // 1. 停止采集（含流线程）
    stopAcquisition();

    // 2. 停止重连定时器
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }

#ifdef HAS_GRPC
    m_stub.reset();
    m_channel.reset();
#endif

    setConnected(false);
    emitBackendStatus("disconnected", "gRPC 后端已断开");
}

bool GrpcReceiverBackend::isBackendConnected() const
{
    return m_connected.load();
}

void GrpcReceiverBackend::startAcquisition(int intervalMs)
{
    if (!m_connected.load()) {
        return;
    }

    m_acquisitionIntervalMs = intervalMs;
    m_paused.store(false);

    if (m_mockMode.load()) {
        // Mock 模式：启动定时器
        m_mockTimer->setInterval(qMax(10, intervalMs));
        m_mockTimer->start();
    } else {
        // Real 模式：启动流线程 + 断线检查定时器
        startStreamThread(intervalMs);
        m_reconnectTimer->setInterval(5000);
        m_reconnectTimer->start();
    }
}

void GrpcReceiverBackend::stopAcquisition()
{
    if (m_mockTimer) {
        m_mockTimer->stop();
    }
    stopStreamThread();
}

void GrpcReceiverBackend::setPaused(bool paused)
{
    m_paused.store(paused);
    // Mock 模式下暂停/恢复定时器；Real 模式下流线程内检查 m_paused 标志
    if (m_mockMode.load()) {
        if (paused) {
            m_mockTimer->stop();
        } else if (m_connected.load()) {
            m_mockTimer->start();
        }
    }
}

void GrpcReceiverBackend::setMockMode(bool enabled)
{
    if (m_mockMode.load() == enabled) {
        return;
    }

    // 若当前正在运行，先断线再切换
    if (m_connected.load()) {
        disconnectBackend();
    }

    m_mockMode.store(enabled);
    emitBackendStatus("modeChanged",
        enabled ? "切换到 gRPC Mock 模式" : "切换到 gRPC 真实服务模式");
}

// ============================================================
// sendCommand
// ============================================================

void GrpcReceiverBackend::sendCommand(const QByteArray& command)
{
    if (!m_connected.load()) {
        emit commandError("gRPC 后端未连接");
        return;
    }
    if (command.isEmpty()) {
        emit commandError("指令不能为空");
        return;
    }

    emit commandSent(command);

    if (m_mockMode.load()) {
        // Mock 模式：模拟 ACK
        QJsonObject ack;
        ack.insert("type", "commandAck");
        ack.insert("mode", "mock");
        ack.insert("size", static_cast<int>(command.size()));
        emit dataReceived(QJsonDocument(ack).toJson(QJsonDocument::Compact), false);
        return;
    }

#ifdef HAS_GRPC
    if (!m_stub) {
        emit commandError("gRPC Stub 未初始化");
        return;
    }

    // 构造请求
    device_data::CommandRequest req;
    req.set_payload(command.constData(), static_cast<size_t>(command.size()));
    req.set_command_id(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());

    device_data::CommandResponse resp;
    grpc::ClientContext ctx;
    // 设置 5 秒超时
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    grpc::Status status = m_stub->SendCommand(&ctx, req, &resp);

    if (status.ok()) {
        QJsonObject ack;
        ack.insert("type",       "commandAck");
        ack.insert("mode",       "real");
        ack.insert("success",    resp.success());
        ack.insert("message",    QString::fromStdString(resp.message()));
        ack.insert("command_id", QString::fromStdString(resp.command_id()));
        emit dataReceived(QJsonDocument(ack).toJson(QJsonDocument::Compact), false);
    } else {
        emit commandError(QString("指令发送失败: [%1] %2")
            .arg(status.error_code())
            .arg(QString::fromStdString(status.error_message())));
    }
#else
    emit commandError("当前构建未启用 gRPC 支持");
#endif
}

void GrpcReceiverBackend::sendCommand(const QString& command, bool isHex)
{
    sendCommand(isHex ? QByteArray::fromHex(command.toLatin1()) : command.toUtf8());
}

// ============================================================
// Mock 模式：定时帧生成（onMockTick）
// ============================================================

void GrpcReceiverBackend::onMockTick()
{
    if (m_paused.load() || !m_connected.load()) {
        return;
    }

    FrameData frame;
    frame.timestamp   = QDateTime::currentMSecsSinceEpoch();
    frame.frameId     = static_cast<uint16_t>(++m_frameCounter);
    frame.detectMode  = FrameData::MultiChannelComplex;
    frame.channelCount = 8;
    frame.channels_comp0.resize(frame.channelCount);
    frame.channels_comp1.resize(frame.channelCount);

    for (int i = 0; i < frame.channelCount; ++i) {
        frame.channels_comp0[i] = QRandomGenerator::global()->bounded(1000) / 100.0;
        frame.channels_comp1[i] = QRandomGenerator::global()->bounded(1000) / 100.0;
    }

    emit frameReceived(frame);

    if (!shouldEmitRealtimePacket(frame.timestamp)) {
        return;
    }

    QJsonObject pkt;
    pkt.insert("type",         "streamFrame");
    pkt.insert("mode",         "mock");
    pkt.insert("frameId",      static_cast<int>(frame.frameId));
    pkt.insert("timestamp",    static_cast<qint64>(frame.timestamp));
    pkt.insert("channelCount", static_cast<int>(frame.channelCount));
    emit dataReceived(QJsonDocument(pkt).toJson(QJsonDocument::Compact), false);
}

// ============================================================
// Real 模式：断线重连检查（onReconnectCheck）
// ============================================================

void GrpcReceiverBackend::onReconnectCheck()
{
#ifdef HAS_GRPC
    if (!m_channel || m_mockMode.load()) {
        return;
    }

    const grpc_connectivity_state state = m_channel->GetState(false);
    if (state == GRPC_CHANNEL_TRANSIENT_FAILURE || state == GRPC_CHANNEL_SHUTDOWN) {
        emitBackendStatus("reconnecting",
            QString("Channel 状态异常 (%1)，尝试重连…").arg(state));

        // 中止当前流
        stopStreamThread();
        setConnected(false);

        // 尝试重新连接并重启流
        const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(2000);
        if (m_channel->WaitForConnected(deadline)) {
            m_stub = device_data::DeviceDataService::NewStub(m_channel);
            setConnected(true);
            startStreamThread(m_acquisitionIntervalMs);
            emitBackendStatus("reconnected", "重连成功");
        } else {
            emitBackendStatus("reconnectFailed", "重连失败，将在下次检查时重试");
        }
    }
#endif
}

// ============================================================
// Real 模式：流线程管理
// ============================================================

void GrpcReceiverBackend::startStreamThread(int intervalMs)
{
    stopStreamThread(); // 确保旧线程已退出

    m_stopStream.store(false);
    m_streamThread = std::thread([this, intervalMs]() {
        streamLoop(intervalMs);
    });
}

void GrpcReceiverBackend::stopStreamThread()
{
    m_stopStream.store(true);

#ifdef HAS_GRPC
    // TryCancel() 会终止阻塞中的 Read() 调用
    if (m_streamCtx) {
        m_streamCtx->TryCancel();
    }
#endif

    if (m_streamThread.joinable()) {
        m_streamThread.join();
    }

#ifdef HAS_GRPC
    m_streamCtx.reset();
#endif
}

// ============================================================
// Real 模式：流线程主循环（在 std::thread 内运行）
// ============================================================

void GrpcReceiverBackend::streamLoop(int intervalMs)
{
    Q_UNUSED(intervalMs)

#ifdef HAS_GRPC
    if (!m_stub) {
        QMetaObject::invokeMethod(this, [this]() {
            emit commandError("流线程启动失败：Stub 为空");
        }, Qt::QueuedConnection);
        return;
    }

    // 每次启动流时创建新 Context（Context 不可复用）
    auto ctx = std::make_unique<grpc::ClientContext>();
    // 保存指针供 stopStreamThread 调用 TryCancel
    {
        // 注：此赋值发生在 std::thread 内部，stopStreamThread 在 Qt 线程侧调用，
        // 但两者执行顺序已通过 startStreamThread 内的 stopStreamThread 先行调用保证。
        m_streamCtx = std::move(ctx);
    }

    // 构造订阅请求
    device_data::SubscribeRequest req;
    req.set_channel_count(0);        // 0 = 全通道
    req.set_interval_ms(static_cast<uint32_t>(qMax(10, intervalMs)));

    auto reader = m_stub->Subscribe(m_streamCtx.get(), req);

    // 读取帧循环
    device_data::DataFrame pbFrame;
    while (!m_stopStream.load() && reader->Read(&pbFrame)) {
        if (m_paused.load()) {
            continue; // 暂停时丢弃接收到的帧
        }

        // 将 protobuf DataFrame 转换为应用层 FrameData
        FrameData frame;
        frame.timestamp    = static_cast<int64_t>(pbFrame.timestamp());
        frame.frameId      = static_cast<uint16_t>(pbFrame.frame_id() & 0xFFFF);
        const uint32_t detectModeRaw = pbFrame.detect_mode();
        frame.detectMode   = (detectModeRaw <= static_cast<uint32_t>(FrameData::MultiChannelComplex))
            ? static_cast<FrameData::DetectionMode>(detectModeRaw)
            : FrameData::Legacy;
        frame.channelCount = static_cast<uint8_t>(pbFrame.channel_count());

        const int comp0Size = pbFrame.channels_comp0_size();
        const int comp1Size = pbFrame.channels_comp1_size();

        frame.channels_comp0.resize(comp0Size);
        for (int i = 0; i < comp0Size; ++i) {
            frame.channels_comp0[i] = pbFrame.channels_comp0(i);
        }

        frame.channels_comp1.resize(comp1Size);
        for (int i = 0; i < comp1Size; ++i) {
            frame.channels_comp1[i] = pbFrame.channels_comp1(i);
        }

        const int inferredChannels = qMax(comp0Size, comp1Size);
        if (frame.channelCount == 0 && inferredChannels > 0) {
            frame.channelCount = static_cast<uint8_t>(qBound(0, inferredChannels, 255));
        } else if (inferredChannels > static_cast<int>(frame.channelCount)) {
            frame.channelCount = static_cast<uint8_t>(qBound(0, inferredChannels, 255));
        }

        // 跨线程发射信号（Qt::QueuedConnection 自动处理）
        emit frameReceived(frame);

        if (!shouldEmitRealtimePacket(frame.timestamp)) {
            continue;
        }

        // 同步发送轻量状态包给 UI 显示
        QJsonObject pkt;
        pkt.insert("type",         "streamFrame");
        pkt.insert("mode",         "real");
        pkt.insert("frameId",      static_cast<int>(frame.frameId));
        pkt.insert("timestamp",    static_cast<qint64>(frame.timestamp));
        pkt.insert("channelCount", static_cast<int>(frame.channelCount));
        pkt.insert("detectMode",   static_cast<int>(frame.detectMode));
        emit dataReceived(QJsonDocument(pkt).toJson(QJsonDocument::Compact), false);
    }

    // 循环结束：区分正常停止 vs. RPC 错误
    grpc::Status status = reader->Finish();
    if (!m_stopStream.load()) {
        // 非主动停止 → 连接异常
        const QString errMsg = status.ok()
            ? "gRPC 流意外结束（服务端关闭）"
            : QString("gRPC 流错误: [%1] %2")
                .arg(status.error_code())
                .arg(QString::fromStdString(status.error_message()));

        QMetaObject::invokeMethod(this, [this, errMsg]() {
            setConnected(false);
            emit commandError(errMsg);
        }, Qt::QueuedConnection);
    }
#else
    QMetaObject::invokeMethod(this, [this]() {
        emit commandError("当前构建未启用 gRPC 支持（缺少 HAS_GRPC 宏）");
    }, Qt::QueuedConnection);
#endif
}

// ============================================================
// 辅助函数
// ============================================================

bool GrpcReceiverBackend::shouldEmitRealtimePacket(qint64 timestampMs)
{
    const qint64 previous = m_lastRealtimePacketMs.load(std::memory_order_relaxed);
    if (timestampMs - previous < m_realtimePacketIntervalMs) {
        return false;
    }

    m_lastRealtimePacketMs.store(timestampMs, std::memory_order_relaxed);
    return true;
}

void GrpcReceiverBackend::setConnected(bool connected)
{
    const bool previous = m_connected.exchange(connected);
    if (previous != connected) {
        emit connectionStateChanged(connected);
    }
}

void GrpcReceiverBackend::emitBackendStatus(const QString& status, const QString& detail)
{
    QJsonObject pkt;
    pkt.insert("type",     "grpcStatus");
    pkt.insert("status",   status);
    pkt.insert("mode",     m_mockMode.load() ? "mock" : "real");
    pkt.insert("endpoint", m_endpoint);
    pkt.insert("detail",   detail);
    emit dataReceived(QJsonDocument(pkt).toJson(QJsonDocument::Compact), false);
}

