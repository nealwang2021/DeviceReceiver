#include "GrpcReceiverBackend.h"
#include "GrpcEndpointUtils.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QtGlobal>

#ifdef HAS_GRPC
#include <chrono>
#include <grpcpp/grpcpp.h>
#include "device.pb.h"
#include "device.grpc.pb.h"
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
    QString grpcTarget;
    if (!GrpcEndpointUtils::parseHostPort(endpoint, &grpcTarget, nullptr, nullptr)) {
        emit commandError(QStringLiteral(
            "gRPC 地址格式无效。示例: 127.0.0.1:50051、device.local:50051、或 IPv6: [::1]:50051"));
        return false;
    }
    m_endpoint = grpcTarget;

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
    m_stub = xiaoche::device::AcquisitionDevice::NewStub(m_channel);

    // 新协议通常需要先打开设备：自动选择第一个设备进行 Open
    {
        google::protobuf::Empty emptyReq;
        xiaoche::device::ListDevicesReply listReply;
        grpc::ClientContext listCtx;
        listCtx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
        const grpc::Status listStatus = m_stub->ListDevices(&listCtx, emptyReq, &listReply);

        if (listStatus.ok() && listReply.devices_size() > 0) {
            xiaoche::device::OpenDeviceRequest openReq;
            openReq.set_device_id(listReply.devices(0).device_id());
            xiaoche::device::CommandReply openReply;
            grpc::ClientContext openCtx;
            openCtx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
            const grpc::Status openStatus = m_stub->OpenDevice(&openCtx, openReq, &openReply);

            if (!openStatus.ok() || !openReply.ok()) {
                emitBackendStatus("openDeviceWarning",
                    QString("设备打开失败（将继续尝试订阅）: %1")
                        .arg(openStatus.ok() ? QString::fromStdString(openReply.message())
                                             : QString::fromStdString(openStatus.error_message())));
            }
        } else if (!listStatus.ok()) {
            emitBackendStatus("listDevicesWarning",
                QString("设备列表获取失败（将继续尝试订阅）: [%1] %2")
                    .arg(listStatus.error_code())
                    .arg(QString::fromStdString(listStatus.error_message())));
        }
    }

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
    if (m_stub) {
        google::protobuf::Empty req;
        xiaoche::device::CommandReply stopReply;
        xiaoche::device::CommandReply closeReply;

        grpc::ClientContext stopCtx;
        stopCtx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        (void)m_stub->StopSampling(&stopCtx, req, &stopReply);

        grpc::ClientContext closeCtx;
        closeCtx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        (void)m_stub->CloseDevice(&closeCtx, req, &closeReply);
    }

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
    Q_UNUSED(command)
    emit commandError("当前 device.proto 协议未提供通用 SendCommand，已禁用该功能");
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
            m_stub = xiaoche::device::AcquisitionDevice::NewStub(m_channel);
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

    google::protobuf::Empty req;

    // 新协议在订阅前尝试启动采样
    {
        xiaoche::device::CommandReply startReply;
        grpc::ClientContext startCtx;
        startCtx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
        const grpc::Status startStatus = m_stub->StartSampling(&startCtx, req, &startReply);
        if (!startStatus.ok() || !startReply.ok()) {
            const QString detail = startStatus.ok()
                ? QString::fromStdString(startReply.message())
                : QString("[%1] %2").arg(startStatus.error_code()).arg(QString::fromStdString(startStatus.error_message()));
            QMetaObject::invokeMethod(this, [this, detail]() {
                emitBackendStatus("startSamplingWarning", QString("StartSampling 失败，继续尝试订阅: %1").arg(detail));
            }, Qt::QueuedConnection);
        }
    }

    auto reader = m_stub->SubscribeProcessedFrames(m_streamCtx.get(), req);

    // 读取帧循环
    xiaoche::device::ProcessedFrameReply pbFrame;
    while (!m_stopStream.load() && reader->Read(&pbFrame)) {
        if (m_paused.load()) {
            continue; // 暂停时丢弃接收到的帧
        }

        // 将 protobuf ProcessedFrameReply 转换为应用层 FrameData
        FrameData frame;
        frame.timestamp    = static_cast<int64_t>(pbFrame.timestamp_unix_ms());
        frame.sequence     = static_cast<uint64_t>(pbFrame.sequence());
        frame.frameId      = static_cast<uint16_t>(pbFrame.sequence() & 0xFFFF);
        frame.detectMode   = FrameData::MultiChannelComplex;

        const int sampleCount = pbFrame.samples_size();
        const int declaredCount = static_cast<int>(pbFrame.cell_count());
        const int channelCount = qMax(sampleCount, declaredCount);
        frame.channelCount = static_cast<uint8_t>(qBound(0, channelCount, 255));

        frame.channels_amp.resize(sampleCount);
        frame.channels_phase.resize(sampleCount);
        frame.channels_x.resize(sampleCount);
        frame.channels_y.resize(sampleCount);
        frame.channels_display_index.resize(sampleCount);
        frame.channels_source_channel.resize(sampleCount);
        frame.channels_comp0.resize(sampleCount);
        frame.channels_comp1.resize(sampleCount);

        for (int i = 0; i < sampleCount; ++i) {
            const auto& sample = pbFrame.samples(i);
            frame.channels_amp[i] = static_cast<double>(sample.amp());
            frame.channels_phase[i] = static_cast<double>(sample.phase());
            frame.channels_x[i] = static_cast<double>(sample.x());
            frame.channels_y[i] = static_cast<double>(sample.y());
            frame.channels_display_index[i] = static_cast<int>(sample.display_index());
            frame.channels_source_channel[i] = static_cast<int>(sample.source_channel());

            // 兼容既有绘图链路：当前复数主通道沿用 x/y
            frame.channels_comp0[i] = frame.channels_x[i];
            frame.channels_comp1[i] = frame.channels_y[i];
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
        pkt.insert("sequence",     QString::number(static_cast<qulonglong>(frame.sequence)));
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

