#include "GrpcMagArrayBackend.h"

#include "AppConfig.h"
#include "FrameData.h"
#include "GrpcEndpointUtils.h"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
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

GrpcMagArrayBackend::GrpcMagArrayBackend(QObject* parent)
    : IReceiverBackend(parent)
{
    m_mockTimer = new QTimer(this);
    m_mockTimer->setTimerType(Qt::PreciseTimer);
    connect(m_mockTimer, &QTimer::timeout, this, &GrpcMagArrayBackend::onMockTick);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(3000);
    connect(m_reconnectTimer, &QTimer::timeout, this, &GrpcMagArrayBackend::onReconnectCheck);
}

GrpcMagArrayBackend::~GrpcMagArrayBackend()
{
    disconnectBackend();
}

// ============================================================================
// 静态配置参数
// ============================================================================

QVector<BackendParamDescriptor> GrpcMagArrayBackend::configParameters() const
{
    return {
        {"MagArray/PreprocessMode",  QStringLiteral("预处理模式"), ParamEnum,
         QVariant(1), 0, 0, 1, {"None","FixedMidpoint","AutoBaseline","SlowTrackingBaseline"}},
        {"MagArray/FixedMidpoint", QStringLiteral("固定中点值"), ParamDouble,
         QVariant(32768.0), 0.0, 65535.0, 1.0, {}},
        {"MagArray/BaselineFrames", QStringLiteral("基线帧数"), ParamInt,
         QVariant(100), 1, 10000, 1, {}},
        {"MagArray/TrackingFactor", QStringLiteral("慢跟踪因子"), ParamDouble,
         QVariant(0.001), 0.0, 1.0, 0.001, {}},
    };
}

// ============================================================================
// IReceiverBackend 接口
// ============================================================================

bool GrpcMagArrayBackend::connectBackend(const QString& endpoint)
{
    m_cancelConnect.store(false, std::memory_order_relaxed);

    if (m_mockMode.load(std::memory_order_relaxed)) {
        setConnected(true);
        emitBackendStatus(QStringLiteral("漏磁 Mock 就绪"), QString());
        emitDeviceStatus();
        emit connectAttemptFinished(true, QStringLiteral("mock 模式无需连接"));
        return true;
    }

    m_endpoint = endpoint;
    emitBackendStatus(QStringLiteral("正在连接漏磁设备"), endpoint);

#ifdef HAS_GRPC
    QString grpcTarget;
    bool useTls = false;
    QString parsedHost;
    int parsedPort = 0;
    if (!GrpcEndpointUtils::parseChannelEndpoint(endpoint, &grpcTarget, &useTls, &parsedHost, &parsedPort)) {
        emitBackendStatus(QStringLiteral("漏磁连接失败"), QStringLiteral("无法解析端点: ") + endpoint);
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
        qInfo() << "[GrpcMagArray] 尝试连接:" << a.label << connectedTarget;
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
                    qInfo() << "[GrpcMagArray] 尝试连接:" << label << ipTarget;
                    if (tryConnect(ipTarget, a.tls, a.tls ? parsedHost : QString(), &reason)) {
                        connected = true; useTls = a.tls; connectedTarget = ipTarget; break;
                    }
                    failures << QStringLiteral("%1 -> %2").arg(a.label, reason);
                }
                if (connected) break;
            }
        }
    }

    if (!connected) {
        emitBackendStatus(QStringLiteral("漏磁连接失败"),
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
    m_stub = magarray::MagArrayLeakageDetection::NewStub(m_channel);
    if (!m_stub) {
        emit connectAttemptFinished(false, QStringLiteral("CreateStub failed"));
        return false;
    }

    // 验证连接 - ListSerialPorts
    {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(3000));
        google::protobuf::Empty emptyReq;
        magarray::ListSerialPortsResponse listResp;
        const auto status = m_stub->ListSerialPorts(&ctx, emptyReq, &listResp);
        if (!status.ok()) {
            m_stub.reset(); m_channel.reset();
            emitBackendStatus(QStringLiteral("漏磁连接失败"),
                              QStringLiteral("ListSerialPorts: %1").arg(QString::fromStdString(status.error_message())));
            emit connectAttemptFinished(false, QStringLiteral("ListSerialPorts 失败"));
            return false;
        }
        const int portCount = listResp.ports_size();
        emitBackendStatus(QStringLiteral("漏磁已连接"),
                          QStringLiteral("%1，串口数 %2").arg(connectedTarget).arg(portCount));
        emitDeviceStatus();
        emit connectAttemptFinished(true, QStringLiteral("连接成功，串口数 %1").arg(portCount));
    }

    setConnected(true);
    return true;
#else
    Q_UNUSED(endpoint)
    emitBackendStatus(QStringLiteral("gRPC 未编译"), QString());
    return false;
#endif
}

void GrpcMagArrayBackend::disconnectBackend()
{
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

bool GrpcMagArrayBackend::isBackendConnected() const
{
    return m_connected.load(std::memory_order_relaxed);
}

void GrpcMagArrayBackend::startAcquisition(int intervalMs)
{
    if (!m_connected.load(std::memory_order_relaxed)) {
        return;
    }
    if (m_streamThread.joinable()) {
        return;
    }

    m_acquisitionIntervalMs = qMax(10, intervalMs);

    if (m_mockMode.load(std::memory_order_relaxed)) {
        m_frameCounter = 0;
        const int tickMs = qMax(10, m_acquisitionIntervalMs);
        m_mockTimer->start(tickMs);
        emitBackendStatus(QStringLiteral("漏磁 Mock 采集中"),
                          QStringLiteral("间隔 %1 ms").arg(tickMs));
        emitDeviceStatus();
        return;
    }

    startStreamThread(m_acquisitionIntervalMs);
    emitDeviceStatus();
}

void GrpcMagArrayBackend::stopAcquisition()
{
    m_mockTimer->stop();
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    stopStreamThread();

#ifdef HAS_GRPC
    if (m_stub && !m_mockMode.load(std::memory_order_relaxed)) {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(2000));
        google::protobuf::Empty req;
        magarray::OperationReply reply;
        const grpc::Status st = m_stub->StopDetection(&ctx, req, &reply);
        if (!st.ok() || !reply.ok()) {
            const QString detail = st.ok()
                ? QString::fromStdString(reply.message())
                : QString::fromStdString(st.error_message());
            qWarning() << "[GrpcMagArrayBackend] StopDetection failed:" << detail;
        }
    }
#endif

    emitDeviceStatus();
}

void GrpcMagArrayBackend::setPaused(bool paused)
{
    m_paused.store(paused, std::memory_order_relaxed);
    if (m_mockMode.load(std::memory_order_relaxed)) {
        if (paused) {
            m_mockTimer->stop();
        } else {
            m_mockTimer->start(qMax(10, m_acquisitionIntervalMs));
        }
    }
}

void GrpcMagArrayBackend::sendCommand(const QByteArray& command)
{
    Q_UNUSED(command)
    emit commandError(QStringLiteral("漏磁后端不支持通用指令发送"));
}

void GrpcMagArrayBackend::sendCommand(const QString& command, bool isHex)
{
    Q_UNUSED(command)
    Q_UNUSED(isHex)
    emit commandError(QStringLiteral("漏磁后端不支持通用指令发送"));
}

// ============================================================================
// 模式控制
// ============================================================================

void GrpcMagArrayBackend::setMockMode(bool enabled)
{
    m_mockMode.store(enabled, std::memory_order_relaxed);
}

void GrpcMagArrayBackend::setConnectTimeoutMs(int ms)
{
    m_connectTimeoutMs = ms;
}

// ============================================================================
// 定时器槽
// ============================================================================

void GrpcMagArrayBackend::onMockTick()
{
    if (m_paused.load(std::memory_order_relaxed)) return;
    if (!m_mockMode.load(std::memory_order_relaxed)) return;

    constexpr int kSensorCount = 20;
    constexpr int kAxisCount = 3;

    FrameData frame;
    frame.timestamp = QDateTime::currentMSecsSinceEpoch();
    frame.frameId = m_frameCounter;
    frame.sequence = m_frameCounter;
    frame.detectMode = FrameData::MagArray;
    frame.channelCount = 0;

    auto* rng = QRandomGenerator::global();
    frame.magSensorResults.resize(kSensorCount);
    for (int i = 0; i < kSensorCount; ++i) {
        MagSensorResult& sr = frame.magSensorResults[i];
        sr.sensorIndex = i;
        sr.xMean = (rng->generateDouble() - 0.5) * 200.0;
        sr.yMean = (rng->generateDouble() - 0.5) * 200.0;
        sr.zMean = 32768.0 + (rng->generateDouble() - 0.5) * 500.0;
        sr.xLatest = sr.xMean + (rng->generateDouble() - 0.5) * 10.0;
        sr.yLatest = sr.yMean + (rng->generateDouble() - 0.5) * 10.0;
        sr.zLatest = sr.zMean + (rng->generateDouble() - 0.5) * 10.0;
        sr.magnitudeMean = std::sqrt(sr.xMean * sr.xMean + sr.yMean * sr.yMean + sr.zMean * sr.zMean);
        sr.magnitudeLatest = std::sqrt(sr.xLatest * sr.xLatest + sr.yLatest * sr.yLatest + sr.zLatest * sr.zLatest);
    }

    ++m_frameCounter;
    m_lastFrameReceivedMs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
    emit frameReceived(frame);
}

void GrpcMagArrayBackend::onReconnectCheck()
{
    if (!m_connected.load(std::memory_order_relaxed) && !m_disconnectInProgress.load(std::memory_order_relaxed)) {
        qDebug() << "[GrpcMagArrayBackend] 断线重连尝试...";
        connectBackend(m_endpoint);
    }
}

// ============================================================================
// 流线程
// ============================================================================

void GrpcMagArrayBackend::startStreamThread(int intervalMs)
{
    stopStreamThread();

    m_stopStream.store(false, std::memory_order_relaxed);
    m_streamStartMs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
    m_streamThread = std::thread(&GrpcMagArrayBackend::streamLoop, this, intervalMs);
}

void GrpcMagArrayBackend::stopStreamThread()
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

void GrpcMagArrayBackend::streamLoop(int intervalMs)
{
#ifdef HAS_GRPC
    if (!m_stub) {
        emitBackendStatus(QStringLiteral("漏磁流错误"), QStringLiteral("Stub 未初始化"));
        return;
    }

    // 先调用 StartDetection
    {
        const auto* cfg = AppConfig::instance();
        magarray::StartDetectionRequest detReq;
        detReq.set_port_name("");
        detReq.set_baudrate(1000000);
        auto* preproc = detReq.mutable_preprocess();
        if (cfg) {
            preproc->set_mode(magarray::PREPROCESS_MODE_NONE);
            preproc->set_fixed_midpoint(32768.0);
            preproc->set_baseline_frames(100);
            preproc->set_tracking_factor(0.001);
        } else {
            preproc->set_mode(magarray::PREPROCESS_MODE_NONE);
            preproc->set_fixed_midpoint(32768.0);
            preproc->set_baseline_frames(100);
            preproc->set_tracking_factor(0.001);
        }

        grpc::ClientContext detCtx;
        magarray::OperationReply detReply;
        const auto detStatus = m_stub->StartDetection(&detCtx, detReq, &detReply);
        if (!detStatus.ok() || !detReply.ok()) {
            emitBackendStatus(QStringLiteral("漏磁启动检测失败"),
                              QString::fromStdString(detReply.message()));
            return;
        }
        emitBackendStatus(QStringLiteral("漏磁检测已启动"),
                          QString::fromStdString(detReply.message()));
    }

    // 构建 StreamFramesRequest
    magarray::StreamFramesRequest req;
    req.set_include_raw_values(false);
    req.set_include_processed_values(false);
    req.set_include_channel_values(true);
    req.set_include_sensor_values(true);
    req.set_max_frames_per_second(0);

    auto ctx = std::make_unique<grpc::ClientContext>();
    {
        std::lock_guard<std::mutex> lock(m_streamStateMutex);
        m_streamCtx = std::move(ctx);
    }

    auto reader = m_stub->StreamFrames(m_streamCtx.get(), req);
    if (!reader) {
        emitBackendStatus(QStringLiteral("漏磁流错误"), QStringLiteral("StreamFrames 返回空 reader"));
        return;
    }

    emitBackendStatus(QStringLiteral("漏磁流已启动"),
                      QStringLiteral("间隔 %1 ms").arg(intervalMs));
    Q_UNUSED(intervalMs)

    magarray::MagArrayFrame pbFrame;
    while (!m_stopStream.load(std::memory_order_relaxed)) {
        if (!reader->Read(&pbFrame)) {
            break;
        }

        if (m_paused.load(std::memory_order_relaxed)) {
            continue;
        }

        FrameData frame;
        frame.timestamp = static_cast<int64_t>(pbFrame.timestamp_unix_ms());
        frame.frameId = static_cast<uint64_t>(pbFrame.frame_index());
        frame.sequence = frame.frameId;
        frame.detectMode = FrameData::MagArray;
        frame.channelCount = 0;

        // 转换传感器结果
        const int nSensors = pbFrame.sensor_results_size();
        frame.magSensorResults.resize(nSensors);
        for (int i = 0; i < nSensors; ++i) {
            const auto& sr = pbFrame.sensor_results(i);
            MagSensorResult& r = frame.magSensorResults[i];
            r.sensorIndex = sr.sensor_index();
            r.xMean = sr.x_mean();
            r.yMean = sr.y_mean();
            r.zMean = sr.z_mean();
            r.xLatest = sr.x_latest();
            r.yLatest = sr.y_latest();
            r.zLatest = sr.z_latest();
            r.magnitudeMean = sr.magnitude_mean();
            r.magnitudeLatest = sr.magnitude_latest();
        }

        m_lastFrameReceivedMs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
        emit frameReceived(frame);

        // 节流 JSON 数据包
        if (shouldEmitRealtimePacket(pbFrame.timestamp_unix_ms())) {
            QJsonObject pkt;
            pkt["type"] = QStringLiteral("magarray");
            pkt["frame_index"] = static_cast<qint64>(pbFrame.frame_index());
            emit dataReceived(QJsonDocument(pkt).toJson(QJsonDocument::Compact), false);
        }
    }

    // 流结束
    const grpc::Status grpcStatus = reader->Finish();
    if (!grpcStatus.ok() && !m_stopStream.load(std::memory_order_relaxed)) {
        qWarning() << "[GrpcMagArrayBackend] 流异常结束:" << grpcStatus.error_message().c_str();
        emitBackendStatus(QStringLiteral("漏磁流中断"),
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

void GrpcMagArrayBackend::setConnected(bool connected)
{
    bool prev = m_connected.exchange(connected, std::memory_order_relaxed);
    if (prev != connected) {
        emit connectionStateChanged(connected);
    }
}

void GrpcMagArrayBackend::emitBackendStatus(const QString& status, const QString& detail)
{
    Q_UNUSED(status)
    qInfo() << "[GrpcMagArrayBackend]" << status << detail;
}

void GrpcMagArrayBackend::emitDeviceStatus()
{
    QJsonObject s;
    s["protocol"] = QStringLiteral("magarray-grpc");
    s["endpoint"] = m_endpoint;
    s["mock"] = m_mockMode.load();
    emit backendStatusChanged(s);
}

bool GrpcMagArrayBackend::shouldEmitRealtimePacket(qint64 timestampMs)
{
    const qint64 last = m_lastRealtimePacketMs.load(std::memory_order_relaxed);
    if (last == 0 || (timestampMs - last) >= m_realtimePacketIntervalMs) {
        m_lastRealtimePacketMs.store(timestampMs, std::memory_order_relaxed);
        return true;
    }
    return false;
}
