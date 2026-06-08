#include "GrpcPulseEddyBackend.h"

#include "AppConfig.h"
#include "FrameData.h"
#include "GrpcEndpointUtils.h"
#include <QSettings>

#include <QCoreApplication>
#include <QAbstractSocket>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QtMath>

#include <chrono>
#include <cmath>
#include <thread>

// ============================================================================
// 构造 / 析构
// ============================================================================

GrpcPulseEddyBackend::GrpcPulseEddyBackend(QObject* parent)
    : IReceiverBackend(parent)
{
    m_mockTimer = new QTimer(this);
    m_mockTimer->setTimerType(Qt::PreciseTimer);
    connect(m_mockTimer, &QTimer::timeout, this, &GrpcPulseEddyBackend::onMockTick);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(3000);
    connect(m_reconnectTimer, &QTimer::timeout, this, &GrpcPulseEddyBackend::onReconnectCheck);
}

GrpcPulseEddyBackend::~GrpcPulseEddyBackend()
{
    disconnectBackend();
}

// ============================================================================
// 静态配置参数
// ============================================================================

QVector<BackendParamDescriptor> GrpcPulseEddyBackend::configParameters() const
{
    return {
        {"PulseEddy/Baudrate", QStringLiteral("波特率"), ParamInt,
         QVariant(1000000), 9600, 4000000, 100, {}},
        {"PulseEddy/ReferenceFrameCount", QStringLiteral("参考线帧数"), ParamInt,
         QVariant(10), 1, 1000, 1, {}},
    };
}

// ============================================================================
// IReceiverBackend 接口
// ============================================================================

bool GrpcPulseEddyBackend::connectBackend(const QString& endpoint)
{
    qInfo() << "[PulseEddy] connectBackend 开始, endpoint=" << endpoint;
    m_cancelConnect.store(false, std::memory_order_relaxed);

    if (m_mockMode.load(std::memory_order_relaxed)) {
        qInfo() << "[PulseEddy] Mock 模式, 跳过连接";
        setConnected(true);
        emitBackendStatus(QStringLiteral("脉冲涡流 Mock 就绪"), QString());
        emitDeviceStatus();
        emit connectAttemptFinished(true, QStringLiteral("mock 模式无需连接"));
        return true;
    }

    m_endpoint = endpoint;
    emitBackendStatus(QStringLiteral("正在连接脉冲涡流设备"), endpoint);

#ifdef HAS_GRPC
    QString grpcTarget;
    bool useTls = false;
    QString parsedHost;
    int parsedPort = 0;
    if (!GrpcEndpointUtils::parseChannelEndpoint(endpoint, &grpcTarget, &useTls,
                                                  &parsedHost, &parsedPort)) {
        emitBackendStatus(QStringLiteral("脉冲涡流连接失败"),
                          QStringLiteral("无法解析端点: ") + endpoint);
        emit connectAttemptFinished(false, QStringLiteral("无法解析端点"));
        return false;
    }

    const QString target = grpcTarget;
    const int connectTimeoutMs = 6000;

    auto tryConnect = [this, connectTimeoutMs](const QString& tgt, bool tls,
                                               const QString& tlsOverrideHost,
                                               QString* errorOut) -> bool {
        if (errorOut) errorOut->clear();
        std::shared_ptr<grpc::ChannelCredentials> creds;
        if (tls) creds = grpc::SslCredentials(grpc::SslCredentialsOptions());
        else     creds = grpc::InsecureChannelCredentials();
        grpc::ChannelArguments args;
        args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
        if (tls && !tlsOverrideHost.trimmed().isEmpty())
            args.SetSslTargetNameOverride(tlsOverrideHost.toStdString());
        m_channel = grpc::CreateCustomChannel(tgt.toStdString(), creds, args);
        if (!m_channel) { if (errorOut) *errorOut = QStringLiteral("CreateChannel failed"); return false; }
        constexpr int kPollMs = 100;
        int waited = 0;
        grpc_connectivity_state lastState = m_channel->GetState(true);
        while (waited < connectTimeoutMs) {
            if (m_cancelConnect.load(std::memory_order_relaxed)) { m_channel.reset(); return false; }
            lastState = m_channel->GetState(true);
            if (lastState == GRPC_CHANNEL_READY) return true;
            const int slice = qMin(kPollMs, connectTimeoutMs - waited);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            waited += slice;
        }
        if (errorOut) *errorOut = QStringLiteral("timeout, state=%1").arg(static_cast<int>(lastState));
        m_channel.reset();
        return false;
    };

    struct Attempt { bool tls; QString label; };
    QVector<Attempt> ordered;
    ordered.append({useTls, useTls ? QStringLiteral("TLS") : QStringLiteral("Insecure")});
    ordered.append({!useTls, !useTls ? QStringLiteral("TLS") : QStringLiteral("Insecure")});
    QVector<Attempt> deduped;
    for (const auto& a : ordered) {
        bool dup = false;
        for (const auto& b : deduped) if (a.tls == b.tls) { dup = true; break; }
        if (!dup) deduped.append(a);
    }

    bool connected = false;
    QStringList failures;
    QString connectedTarget = target;
    for (const auto& a : deduped) {
        QString reason;
        qInfo() << "[PulseEddy] 尝试连接:" << a.label << connectedTarget;
        if (tryConnect(connectedTarget, a.tls, QString(), &reason)) { connected = true; useTls = a.tls; break; }
        failures << QStringLiteral("%1 -> %2").arg(a.label, reason);
    }

    QHostAddress testAddr;
    if (!connected && !parsedHost.trimmed().isEmpty() && !testAddr.setAddress(parsedHost)) {
        const QHostInfo info = QHostInfo::fromName(parsedHost);
        if (info.error() == QHostInfo::NoError) {
            for (const QHostAddress& addr : info.addresses()) {
                if (addr.protocol() != QAbstractSocket::IPv4Protocol) continue;
                const QString ipTarget = QStringLiteral("%1:%2").arg(addr.toString()).arg(parsedPort);
                for (const auto& a : deduped) {
                    QString reason;
                    const QString label = QStringLiteral("%1/ip=%2").arg(a.label, addr.toString());
                    qInfo() << "[PulseEddy] 尝试连接:" << label << ipTarget;
                    if (tryConnect(ipTarget, a.tls, a.tls ? parsedHost : QString(), &reason)) {
                        connected = true; useTls = a.tls; connectedTarget = ipTarget; break;
                    }
                    failures << QStringLiteral("%1 -> %2").arg(label, reason);
                }
                if (connected) break;
            }
        }
    }

    if (!connected) {
        qWarning() << "[PulseEddy] connectBackend 失败, 无法连接" << target
                   << "failures=" << failures;
        emitBackendStatus(QStringLiteral("脉冲涡流连接失败"),
                          QStringLiteral("无法连接 %1 (%2)").arg(target, failures.join(QStringLiteral(" | "))));
        emit connectAttemptFinished(false, QStringLiteral("连接超时或服务端不可达"));
        return false;
    }

    // 重建 channel + stub
    {
        auto creds = useTls ? grpc::SslCredentials(grpc::SslCredentialsOptions())
                            : grpc::InsecureChannelCredentials();
        grpc::ChannelArguments args;
        args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
        if (useTls) args.SetSslTargetNameOverride(parsedHost.toStdString());
        m_channel = grpc::CreateCustomChannel(connectedTarget.toStdString(), creds, args);
    }
    m_stub = pulseeddy::PulseEddy::NewStub(m_channel);
    if (!m_stub) {
        emit connectAttemptFinished(false, QStringLiteral("CreateStub failed"));
        return false;
    }

    // 验证连接 - ListDevices
    {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(3000));
        google::protobuf::Empty emptyReq;
        pulseeddy::ListDevicesResponse listResp;
        const auto status = m_stub->ListDevices(&ctx, emptyReq, &listResp);
        if (!status.ok()) {
            m_stub.reset(); m_channel.reset();
            emitBackendStatus(QStringLiteral("脉冲涡流连接失败"),
                              QStringLiteral("ListDevices: %1").arg(QString::fromStdString(status.error_message())));
            emit connectAttemptFinished(false, QStringLiteral("ListDevices 失败"));
            return false;
        }
        const int devCount = listResp.devices_size();
        m_availableDevices.clear();
        m_availableDeviceKeys.clear();
        for (int i = 0; i < devCount; ++i) {
            const auto& d = listResp.devices(i);
            const QString label = QStringLiteral("Device %1: %2 (SN:%3)")
                .arg(d.index()).arg(QString::fromStdString(d.description()),
                                    QString::fromStdString(d.serial_number()));
            m_availableDevices.append(label);
            m_availableDeviceKeys.append(QString::number(d.index()));
        }
        qInfo() << "[PulseEddy] ListDevices 完成, 数量=" << devCount
                << "设备列表=" << m_availableDevices;
        emit availableDevicesChanged(m_availableDevices);
        emitBackendStatus(QStringLiteral("脉冲涡流已连接"),
                          QStringLiteral("%1，设备数 %2").arg(connectedTarget).arg(devCount));
        emitDeviceStatus();
        emit connectAttemptFinished(true, QStringLiteral("连接成功，设备数 %1").arg(devCount));
        qInfo() << "[PulseEddy] connectBackend 成功, 已连接";
    }

    setConnected(true);
    return true;
#else
    Q_UNUSED(endpoint)
    emitBackendStatus(QStringLiteral("gRPC 未编译"), QString());
    return false;
#endif
}

void GrpcPulseEddyBackend::disconnectBackend()
{
    qInfo() << "[PulseEddy] disconnectBackend 调用";
    m_cancelConnect.store(true, std::memory_order_relaxed);
    m_disconnectInProgress.store(true, std::memory_order_relaxed);

    stopAcquisition();

    if (m_reconnectTimer && m_reconnectTimer->isActive()) {
        m_reconnectTimer->stop();
    }

    m_stub.reset();
    m_channel.reset();

    setConnected(false);
    emitDeviceStatus();
    m_disconnectInProgress.store(false, std::memory_order_relaxed);
}

bool GrpcPulseEddyBackend::isBackendConnected() const
{
    return m_connected.load(std::memory_order_relaxed);
}

void GrpcPulseEddyBackend::startAcquisition(int intervalMs)
{
    qInfo() << "[PulseEddy] startAcquisition 调用, intervalMs=" << intervalMs;
    if (!m_connected.load(std::memory_order_relaxed)) {
        qWarning() << "[PulseEddy] startAcquisition 失败: 未连接";
        return;
    }
    if (m_streamThread.joinable()) {
        qWarning() << "[PulseEddy] startAcquisition 跳过: 流线程已在运行";
        return;
    }

    m_acquisitionIntervalMs = qMax(10, intervalMs);

    if (m_mockMode.load(std::memory_order_relaxed)) {
        m_frameCounter = 0;
        m_mockTimer->start(qMax(10, m_acquisitionIntervalMs));
        emitBackendStatus(QStringLiteral("脉冲涡流 Mock 采集中"), QString());
        emitDeviceStatus();
        return;
    }

    startStreamThread(m_acquisitionIntervalMs);
    emitDeviceStatus();
}

void GrpcPulseEddyBackend::stopAcquisition()
{
    qInfo() << "[PulseEddy] stopAcquisition 调用";
    m_mockTimer->stop();
    if (m_reconnectTimer) m_reconnectTimer->stop();
    stopStreamThread();

#ifdef HAS_GRPC
    if (m_stub && !m_mockMode.load(std::memory_order_relaxed)) {
        {
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(2000));
            google::protobuf::Empty req;
            pulseeddy::OperationReply reply;
            const grpc::Status st = m_stub->StopAcquisition(&ctx, req, &reply);
            if (!st.ok()) {
                qWarning() << "[PulseEddy] StopAcquisition failed:" << st.error_message().c_str();
            }
        }
        {
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(2000));
            google::protobuf::Empty req;
            pulseeddy::OperationReply reply;
            const grpc::Status st = m_stub->CloseDevice(&ctx, req, &reply);
            if (!st.ok()) {
                qWarning() << "[PulseEddy] CloseDevice failed:" << st.error_message().c_str();
            }
        }
    }
#endif

    emitDeviceStatus();
}

void GrpcPulseEddyBackend::setPaused(bool paused)
{
    m_paused.store(paused, std::memory_order_relaxed);
    if (m_mockMode.load(std::memory_order_relaxed)) {
        if (paused) m_mockTimer->stop();
        else        m_mockTimer->start(qMax(10, m_acquisitionIntervalMs));
    }
}

void GrpcPulseEddyBackend::sendCommand(const QByteArray& command)
{
    sendCommand(QString::fromUtf8(command), false);
}

void GrpcPulseEddyBackend::sendCommand(const QString& command, bool isHex)
{
    Q_UNUSED(isHex)
    qInfo() << "[PulseEddy] sendCommand:" << command;

#ifdef HAS_GRPC
    if (!m_stub) {
        emit commandError(QStringLiteral("脉冲涡流后端未连接"));
        return;
    }

    const QString cmd = command.trimmed().toLower();

    if (cmd == "start_reference") {
        int frameCount = 10;
        {
            QSettings settings(AppConfig::defaultConfigFilePath(), QSettings::IniFormat);
            frameCount = settings.value("PulseEddy/ReferenceFrameCount", QVariant(10)).toInt();
        }
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(3000));
        pulseeddy::StartReferenceCaptureRequest req;
        req.set_frame_count(frameCount);
        pulseeddy::OperationReply reply;
        const auto st = m_stub->StartReferenceCapture(&ctx, req, &reply);
        if (st.ok() && reply.success()) {
            qInfo() << "[PulseEddy] StartReferenceCapture 成功, frameCount=" << frameCount;
            emit commandSent(QString("start_reference OK, frameCount=%1").arg(frameCount).toUtf8());
        } else {
            const QString err = st.ok() ? QString::fromStdString(reply.message())
                                        : QString::fromStdString(st.error_message());
            qWarning() << "[PulseEddy] StartReferenceCapture 失败:" << err;
            emit commandError(err);
        }
    } else if (cmd == "clear_reference") {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(3000));
        google::protobuf::Empty req;
        pulseeddy::OperationReply reply;
        const auto st = m_stub->ClearReference(&ctx, req, &reply);
        if (st.ok() && reply.success()) {
            qInfo() << "[PulseEddy] ClearReference 成功";
            emit commandSent(QByteArray("clear_reference OK"));
        } else {
            const QString err = st.ok() ? QString::fromStdString(reply.message())
                                        : QString::fromStdString(st.error_message());
            qWarning() << "[PulseEddy] ClearReference 失败:" << err;
            emit commandError(err);
        }
    } else {
        emit commandError(QStringLiteral("脉冲涡流不支持指令: ") + command);
    }
#else
    emit commandError(QStringLiteral("gRPC 未编译"));
#endif
}

// ============================================================================
// 模式控制
// ============================================================================

void GrpcPulseEddyBackend::setMockMode(bool enabled)
{
    m_mockMode.store(enabled, std::memory_order_relaxed);
}

void GrpcPulseEddyBackend::setConnectTimeoutMs(int ms)
{
    m_connectTimeoutMs = ms;
}

// ============================================================================
// Mock 定时器
// ============================================================================

void GrpcPulseEddyBackend::onMockTick()
{
    if (m_paused.load(std::memory_order_relaxed)) return;
    if (!m_mockMode.load(std::memory_order_relaxed)) return;

    constexpr int kSampleCount = 1000; // Mock 用 1000 点，真实设备 64000

    FrameData frame;
    frame.timestamp = QDateTime::currentMSecsSinceEpoch();
    frame.frameId = m_frameCounter;
    frame.sequence = m_frameCounter;
    frame.detectMode = FrameData::PulseEddy;
    frame.channelCount = 0;
    frame.pulseSampleCount = kSampleCount;
    frame.pulseSampleRateHz = 1000000;

    frame.pulseRawValues.resize(kSampleCount);
    const double t = static_cast<double>(m_frameCounter) * 0.5;
    for (int i = 0; i < kSampleCount; ++i) {
        const double x = static_cast<double>(i) / kSampleCount * 10.0;
        frame.pulseRawValues[i] = std::sin(x * 6.28 + t) * std::exp(-x * 0.5) * 100.0
            + (QRandomGenerator::global()->generateDouble() - 0.5) * 2.0;
    }

    // 模拟参考线 (每 10 帧后出现)
    if (m_frameCounter > 0 && m_frameCounter % 10 == 0) {
        frame.hasPulseReference = true;
        frame.pulseRefValues = frame.pulseRawValues;
        for (double& v : frame.pulseRefValues)
            v += (QRandomGenerator::global()->generateDouble() - 0.5) * 0.5;
    }

    ++m_frameCounter;
    m_lastFrameReceivedMs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
    emit frameReceived(frame);
}

void GrpcPulseEddyBackend::onReconnectCheck()
{
    if (!m_connected.load(std::memory_order_relaxed) && !m_disconnectInProgress.load(std::memory_order_relaxed)) {
        qDebug() << "[PulseEddy] 断线重连尝试...";
        connectBackend(m_endpoint);
    }
}

// ============================================================================
// 流线程
// ============================================================================

void GrpcPulseEddyBackend::startStreamThread(int intervalMs)
{
    stopStreamThread();

    m_stopStream.store(false, std::memory_order_relaxed);
    m_streamStartMs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
    m_streamThread = std::thread(&GrpcPulseEddyBackend::streamLoop, this, intervalMs);
}

void GrpcPulseEddyBackend::stopStreamThread()
{
    m_stopStream.store(true, std::memory_order_relaxed);

#ifdef HAS_GRPC
    {
        std::lock_guard<std::mutex> lock(m_streamStateMutex);
        if (m_streamCtx) {
            m_streamCtx->TryCancel();
        }
    }
#endif

    if (m_streamThread.joinable()) {
        m_streamThread.join();
    }

#ifdef HAS_GRPC
    {
        std::lock_guard<std::mutex> lock(m_streamStateMutex);
        m_streamCtx.reset();
    }
#endif
}

void GrpcPulseEddyBackend::streamLoop(int intervalMs)
{
#ifdef HAS_GRPC
    if (!m_stub) {
        emitBackendStatus(QStringLiteral("脉冲涡流流错误"), QStringLiteral("Stub 未初始化"));
        return;
    }

    // 读取配置
    int deviceIndex = m_deviceIndex;
    int baudrate = m_baudrate;
    {
        QSettings settings(AppConfig::defaultConfigFilePath(), QSettings::IniFormat);
        const int cfgDevIdx = settings.value("PulseEddy/DeviceIndex", QVariant(-1)).toInt();
        if (cfgDevIdx >= 0) deviceIndex = cfgDevIdx;
        baudrate = settings.value("PulseEddy/Baudrate", QVariant(1000000)).toInt();
    }

    if (deviceIndex < 0 && !m_availableDeviceKeys.isEmpty()) {
        deviceIndex = m_availableDeviceKeys.first().toInt();
    }

    qInfo() << "[PulseEddy] OpenDevice: index=" << deviceIndex << "baudrate=" << baudrate;

    // OpenDevice
    {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(5000));
        pulseeddy::OpenDeviceRequest req;
        req.set_device_index(deviceIndex);
        req.set_baudrate(baudrate);
        pulseeddy::OperationReply reply;
        const auto st = m_stub->OpenDevice(&ctx, req, &reply);
        if (!st.ok() || !reply.success()) {
            const QString err = st.ok() ? QString::fromStdString(reply.message())
                                        : QString::fromStdString(st.error_message());
            qWarning() << "[PulseEddy] OpenDevice 失败:" << err;
            emitBackendStatus(QStringLiteral("脉冲涡流打开设备失败"), err);
            return;
        }
        qInfo() << "[PulseEddy] OpenDevice 成功:" << QString::fromStdString(reply.message());
    }

    // StartAcquisition
    {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(5000));
        pulseeddy::StartAcquisitionRequest req;
        req.set_device_index(deviceIndex);
        req.set_baudrate(baudrate);
        req.set_clear_reference(false);
        pulseeddy::OperationReply reply;
        const auto st = m_stub->StartAcquisition(&ctx, req, &reply);
        if (!st.ok() || !reply.success()) {
            const QString err = st.ok() ? QString::fromStdString(reply.message())
                                        : QString::fromStdString(st.error_message());
            qWarning() << "[PulseEddy] StartAcquisition 失败:" << err;
            emitBackendStatus(QStringLiteral("脉冲涡流启动采集失败"), err);
            return;
        }
        qInfo() << "[PulseEddy] StartAcquisition 成功";
    }

    emitBackendStatus(QStringLiteral("脉冲涡流采集中"), QStringLiteral("设备 %1").arg(deviceIndex));

    // StreamFrames
    pulseeddy::StreamFramesRequest req;
    req.set_max_fps(0);
    req.set_include_reference(true);

    auto ctx = std::make_unique<grpc::ClientContext>();
    {
        std::lock_guard<std::mutex> lock(m_streamStateMutex);
        m_streamCtx = std::move(ctx);
    }

    auto reader = m_stub->StreamFrames(m_streamCtx.get(), req);
    if (!reader) {
        qWarning() << "[PulseEddy] StreamFrames 失败: 返回空 reader";
        emitBackendStatus(QStringLiteral("脉冲涡流流错误"), QStringLiteral("StreamFrames 返回空 reader"));
        return;
    }

    qInfo() << "[PulseEddy] StreamFrames 已启动, 开始读取数据流";
    Q_UNUSED(intervalMs)

    pulseeddy::PulseFrame pbFrame;
    while (!m_stopStream.load(std::memory_order_relaxed)) {
        if (!reader->Read(&pbFrame)) {
            break;
        }

        if (m_paused.load(std::memory_order_relaxed)) {
            continue;
        }

        FrameData frame;
        frame.timestamp    = static_cast<int64_t>(pbFrame.timestamp_unix_ms());
        frame.frameId      = static_cast<uint64_t>(pbFrame.frame_index());
        frame.sequence     = frame.frameId;
        frame.detectMode   = FrameData::PulseEddy;
        frame.channelCount = 0;
        frame.pulseSampleCount  = pbFrame.sample_count();
        frame.pulseSampleRateHz = static_cast<qint64>(pbFrame.sample_rate_hz());

        // raw_values
        const int nRaw = pbFrame.raw_values_size();
        frame.pulseRawValues.resize(nRaw);
        for (int i = 0; i < nRaw; ++i)
            frame.pulseRawValues[i] = pbFrame.raw_values(i);

        // reference_values
        frame.hasPulseReference = pbFrame.has_reference();
        if (frame.hasPulseReference) {
            const int nRef = pbFrame.reference_values_size();
            frame.pulseRefValues.resize(nRef);
            for (int i = 0; i < nRef; ++i)
                frame.pulseRefValues[i] = pbFrame.reference_values(i);
        }

        m_lastFrameReceivedMs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
        emit frameReceived(frame);

        if (shouldEmitRealtimePacket(pbFrame.timestamp_unix_ms())) {
            QJsonObject pkt;
            pkt["type"] = QStringLiteral("pulse_eddy");
            pkt["frame_index"] = static_cast<qint64>(pbFrame.frame_index());
            pkt["sample_count"] = nRaw;
            pkt["has_reference"] = frame.hasPulseReference;
            emit dataReceived(QJsonDocument(pkt).toJson(QJsonDocument::Compact), false);
        }
    }

    // 流结束
    const grpc::Status grpcStatus = reader->Finish();
    if (m_stopStream.load(std::memory_order_relaxed)) {
        qInfo() << "[PulseEddy] 数据流正常停止 (用户主动停止)";
    } else {
        qWarning() << "[PulseEddy] 数据流异常结束:" << grpcStatus.error_message().c_str()
                   << "error_code=" << static_cast<int>(grpcStatus.error_code());
        emitBackendStatus(QStringLiteral("脉冲涡流流中断"),
                          QString::fromStdString(grpcStatus.error_message()));
        setConnected(false);
        if (m_reconnectTimer) {
            m_reconnectTimer->start();
        }
    }
#else
    Q_UNUSED(intervalMs)
#endif
}

// ============================================================================
// 辅助
// ============================================================================

void GrpcPulseEddyBackend::setConnected(bool connected)
{
    bool prev = m_connected.exchange(connected, std::memory_order_relaxed);
    if (prev != connected) {
        emit connectionStateChanged(connected);
    }
}

void GrpcPulseEddyBackend::emitBackendStatus(const QString& status, const QString& detail)
{
    qInfo() << "[GrpcPulseEddyBackend]" << status << detail;
}

void GrpcPulseEddyBackend::emitDeviceStatus()
{
    QJsonObject s;
    s["protocol"] = QStringLiteral("pulseeddy");
    s["endpoint"] = m_endpoint;
    s["mock"] = m_mockMode.load();
    emit backendStatusChanged(s);
}

bool GrpcPulseEddyBackend::shouldEmitRealtimePacket(qint64 timestampMs)
{
    const qint64 last = m_lastRealtimePacketMs.load(std::memory_order_relaxed);
    if (last == 0 || (timestampMs - last) >= m_realtimePacketIntervalMs) {
        m_lastRealtimePacketMs.store(timestampMs, std::memory_order_relaxed);
        return true;
    }
    return false;
}
