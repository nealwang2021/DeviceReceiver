#include "GrpcMultiFreqBackend.h"

#include "AppConfig.h"
#include "FrameData.h"
#include "GrpcEndpointUtils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QtMath>

#include <chrono>
#include <cmath>

// ============================================================================
// 构造 / 析构
// ============================================================================

GrpcMultiFreqBackend::GrpcMultiFreqBackend(QObject* parent)
    : IReceiverBackend(parent)
{
    m_mockTimer = new QTimer(this);
    m_mockTimer->setTimerType(Qt::PreciseTimer);
    connect(m_mockTimer, &QTimer::timeout, this, &GrpcMultiFreqBackend::onMockTick);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(3000);
    connect(m_reconnectTimer, &QTimer::timeout, this, &GrpcMultiFreqBackend::onReconnectCheck);
}

GrpcMultiFreqBackend::~GrpcMultiFreqBackend()
{
    disconnectBackend();
}

// ============================================================================
// 静态配置参数
// ============================================================================

QVector<BackendParamDescriptor> GrpcMultiFreqBackend::configParameters() const
{
    return {
        {"MultiFreq/BaseFrequencyHz",  QStringLiteral("基频(Hz)"), ParamEnum,
         QVariant(100), 0, 0, 1, {"1","2","5","10","20","50","100","200","500","1000"}},
        {"MultiFreq/AverageCycleCount", QStringLiteral("平均周期数"), ParamInt,
         QVariant(10), 1, 1000, 1, {}},
        {"MultiFreq/NormalizeScale", QStringLiteral("归一化系数"), ParamDouble,
         QVariant(1.0), 0.01, 100.0, 0.1, {}},
        {"MultiFreq/FrequencyFactors", QStringLiteral("倍频系数"), ParamIntList,
         QVariant(QString("1,2,4,8")), 0, 0, 0, {}},
    };
}

// ============================================================================
// IReceiverBackend 接口
// ============================================================================

bool GrpcMultiFreqBackend::connectBackend(const QString& endpoint)
{
    m_cancelConnect.store(false, std::memory_order_relaxed);

    if (m_mockMode.load(std::memory_order_relaxed)) {
        setConnected(true);
        emitBackendStatus(QStringLiteral("多频涡流 Mock 就绪"), QString());
        emitDeviceStatus();
        emit connectAttemptFinished(true, QStringLiteral("mock 模式无需连接"));
        return true;
    }

    m_endpoint = endpoint;
    emitBackendStatus(QStringLiteral("正在连接多频涡流设备"), endpoint);

#ifdef HAS_GRPC
    QString grpcTarget;
    bool useTls = false;
    QString parsedHost;
    int parsedPort = 0;
    if (!GrpcEndpointUtils::parseChannelEndpoint(endpoint, &grpcTarget, &useTls, &parsedHost, &parsedPort)) {
        emitBackendStatus(QStringLiteral("多频涡流连接失败"), QStringLiteral("无法解析端点: ") + endpoint);
        emit connectAttemptFinished(false, QStringLiteral("无法解析端点"));
        return false;
    }

    const QString target = grpcTarget;
    const int deadlineMs = m_connectTimeoutMs > 0 ? m_connectTimeoutMs : 6000;
    const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(deadlineMs);

    // 依次尝试 TLS 和明文
    for (int pass = 0; pass < 2; ++pass) {
        if (m_cancelConnect.load(std::memory_order_relaxed)) return false;

        grpc::ChannelArguments args;
        args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);

        auto creds = (pass == 0 && useTls)
                         ? grpc::SslCredentials(grpc::SslCredentialsOptions())
                         : grpc::InsecureChannelCredentials();

        auto chan = grpc::CreateCustomChannel(target.toStdString(), creds, args);
        if (!chan) continue;

        const bool ready = chan->WaitForConnected(deadline);
        if (!ready) continue;

        m_channel = chan;
        m_stub = multifreqeddy::MultiFreqEddyCurrent::NewStub(m_channel);
        if (!m_stub) continue;

        // ListDevices 验证服务端可达
        grpc::ClientContext ctx;
        ctx.set_deadline(deadline);
        google::protobuf::Empty emptyReq;
        multifreqeddy::ListDevicesResponse listResp;
        const auto status = m_stub->ListDevices(&ctx, emptyReq, &listResp);
        if (status.ok()) {
            setConnected(true);
            const int devCount = listResp.devices_size();
            emitBackendStatus(QStringLiteral("多频涡流已连接"),
                              QStringLiteral("%1，设备数 %2").arg(target).arg(devCount));
            emitDeviceStatus();
            emit connectAttemptFinished(true, QStringLiteral("连接成功，设备数 %1").arg(devCount));
            return true;
        }

        // 本 pass 失败，清理
        m_stub.reset();
        m_channel.reset();
        if (!useTls) break; // 仅尝试一次明文
    }

    emitBackendStatus(QStringLiteral("多频涡流连接失败"),
                      QStringLiteral("无法连接 %1").arg(target));
    emit connectAttemptFinished(false, QStringLiteral("连接超时或服务端不可达"));
    return false;
#else
    Q_UNUSED(endpoint)
    emitBackendStatus(QStringLiteral("gRPC 未编译"), QString());
    return false;
#endif
}

void GrpcMultiFreqBackend::disconnectBackend()
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

bool GrpcMultiFreqBackend::isBackendConnected() const
{
    return m_connected.load(std::memory_order_relaxed);
}

void GrpcMultiFreqBackend::startAcquisition(int intervalMs)
{
    m_acquisitionIntervalMs = qMax(10, intervalMs);

    if (m_mockMode.load(std::memory_order_relaxed)) {
        m_frameCounter = 0;
        const int tickMs = qMax(10, m_acquisitionIntervalMs);
        m_mockTimer->start(tickMs);
        emitBackendStatus(QStringLiteral("多频涡流 Mock 采集中"),
                          QStringLiteral("间隔 %1 ms").arg(tickMs));
        emitDeviceStatus();
        return;
    }

    startStreamThread(m_acquisitionIntervalMs);
    emitDeviceStatus();
}

void GrpcMultiFreqBackend::stopAcquisition()
{
    m_mockTimer->stop();
    stopStreamThread();
    emitDeviceStatus();
}

void GrpcMultiFreqBackend::setPaused(bool paused)
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

void GrpcMultiFreqBackend::sendCommand(const QByteArray& command)
{
    Q_UNUSED(command)
    emit commandError(QStringLiteral("多频涡流后端不支持通用指令发送"));
}

void GrpcMultiFreqBackend::sendCommand(const QString& command, bool isHex)
{
    Q_UNUSED(command)
    Q_UNUSED(isHex)
    emit commandError(QStringLiteral("多频涡流后端不支持通用指令发送"));
}

// ============================================================================
// 模式控制
// ============================================================================

void GrpcMultiFreqBackend::setMockMode(bool enabled)
{
    m_mockMode.store(enabled, std::memory_order_relaxed);
}

void GrpcMultiFreqBackend::setConnectTimeoutMs(int ms)
{
    m_connectTimeoutMs = ms;
}

// ============================================================================
// 定时器槽
// ============================================================================

void GrpcMultiFreqBackend::onMockTick()
{
    if (m_paused.load(std::memory_order_relaxed)) return;
    if (!m_mockMode.load(std::memory_order_relaxed)) return;

    const auto* cfg = AppConfig::instance();
    const QList<int> factors = cfg ? cfg->multiFreqFrequencyFactors() : QList<int>{1, 2, 4, 8};
    const int nPoints = factors.size();
    const double normScale = cfg ? cfg->multiFreqNormalizeScale() : 1.0;

    FrameData frame;
    frame.timestamp = QDateTime::currentMSecsSinceEpoch();
    frame.frameId = m_frameCounter;
    frame.sequence = m_frameCounter;
    frame.detectMode = FrameData::MultiFreqEddy;
    frame.channelCount = 0;

    auto* rng = QRandomGenerator::global();
    frame.mfFreqPoints.resize(nPoints);
    for (int i = 0; i < nPoints; ++i) {
        MultiFreqPointResult& pt = frame.mfFreqPoints[i];
        const int factor = factors[i];
        pt.frequencyFactor = factor;
        pt.frequencyHz = cfg ? static_cast<double>(cfg->multiFreqBaseFrequencyHz()) * factor : 100.0 * factor;

        // 生成合理的合成阻抗数据
        const double mag = 0.005 + rng->generateDouble() * 0.05;
        const double phaseRad = (rng->generateDouble() - 0.5) * M_PI; // ±90°
        pt.impedanceMagnitude = mag;
        pt.impedancePhaseDeg = phaseRad * 180.0 / M_PI;
        pt.impedanceReal_raw = mag * std::cos(phaseRad);
        pt.impedanceImag_raw = mag * std::sin(phaseRad);
        pt.impedanceReal_raw = mag * std::cos(phaseRad);
        pt.impedanceImag_raw = mag * std::sin(phaseRad);

        // 电压/电流：生成关联的幅值
        pt.voltageMagnitude = 0.5 + rng->generateDouble() * 2.0;
        pt.currentMagnitude = 0.001 + rng->generateDouble() * 0.01;
        pt.voltageReal = pt.voltageMagnitude * std::cos(phaseRad * 0.3);
        pt.voltageImag = pt.voltageMagnitude * std::sin(phaseRad * 0.3);
        pt.currentReal = pt.currentMagnitude * std::cos(phaseRad * 0.3);
        pt.currentImag = pt.currentMagnitude * std::sin(phaseRad * 0.3);

        pt.normalizedImpedanceReal = pt.impedanceReal_raw / (2.0 * M_PI * pt.frequencyHz) * 1e6;
        pt.normalizedImpedanceImag = pt.impedanceImag_raw / (2.0 * M_PI * pt.frequencyHz) * 1e6;
        pt.valid = true;
    }

    ++m_frameCounter;
    m_lastFrameReceivedMs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
    emit frameReceived(frame);
}

void GrpcMultiFreqBackend::onReconnectCheck()
{
    if (!m_connected.load(std::memory_order_relaxed) && !m_disconnectInProgress.load(std::memory_order_relaxed)) {
        qDebug() << "[GrpcMultiFreqBackend] 断线重连尝试...";
        connectBackend(m_endpoint);
    }
}

// ============================================================================
// 流线程
// ============================================================================

void GrpcMultiFreqBackend::startStreamThread(int intervalMs)
{
    stopStreamThread();

    m_stopStream.store(false, std::memory_order_relaxed);
    m_streamStartMs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
    m_streamThread = std::thread(&GrpcMultiFreqBackend::streamLoop, this, intervalMs);
}

void GrpcMultiFreqBackend::stopStreamThread()
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

void GrpcMultiFreqBackend::streamLoop(int intervalMs)
{
#ifdef HAS_GRPC
    if (!m_stub) {
        emitBackendStatus(QStringLiteral("多频涡流流错误"), QStringLiteral("Stub 未初始化"));
        return;
    }

    // 先调用 StartDetection，传入多频参数
    {
        const auto* cfg = AppConfig::instance();
        multifreqeddy::StartDetectionRequest detReq;
        detReq.set_device_serial_number("");
        detReq.set_device_index(0);
        auto* detCfg = detReq.mutable_config();
        if (cfg) {
            detCfg->set_base_frequency(static_cast<multifreqeddy::BaseFrequency>(cfg->multiFreqBaseFrequencyHz()));
            detCfg->set_average_cycle_count(cfg->multiFreqAverageCycleCount());
            detCfg->set_normalize_scale(cfg->multiFreqNormalizeScale());
            for (int f : cfg->multiFreqFrequencyFactors()) {
                detCfg->add_frequency_factors(f);
            }
        } else {
            detCfg->set_base_frequency(multifreqeddy::BASE_FREQUENCY_HZ_100);
            detCfg->set_average_cycle_count(10);
            detCfg->set_normalize_scale(1.0);
            for (int f : {1, 2, 4, 8}) detCfg->add_frequency_factors(f);
        }

        grpc::ClientContext detCtx;
        multifreqeddy::OperationReply detReply;
        const auto detStatus = m_stub->StartDetection(&detCtx, detReq, &detReply);
        if (!detStatus.ok() || !detReply.ok()) {
            emitBackendStatus(QStringLiteral("多频涡流启动检测失败"),
                              QString::fromStdString(detReply.message()));
            return;
        }
        emitBackendStatus(QStringLiteral("多频涡流检测已启动"),
                          QString::fromStdString(detReply.message()));
    }

    // 构建 StreamFramesRequest
    multifreqeddy::StreamFramesRequest req;
    req.set_include_curve(false);
    req.set_include_spectrum(false);
    req.set_max_frames_per_second(0); // 不限制

    auto ctx = std::make_unique<grpc::ClientContext>();
    {
        std::lock_guard<std::mutex> lock(m_streamStateMutex);
        m_streamCtx = std::move(ctx);
    }

    auto reader = m_stub->StreamFrames(m_streamCtx.get(), req);
    if (!reader) {
        emitBackendStatus(QStringLiteral("多频涡流流错误"), QStringLiteral("StreamFrames 返回空 reader"));
        return;
    }

    emitBackendStatus(QStringLiteral("多频涡流流已启动"),
                      QStringLiteral("间隔 %1 ms").arg(intervalMs));
    Q_UNUSED(intervalMs)

    multifreqeddy::DetectionFrame pbFrame;
    while (!m_stopStream.load(std::memory_order_relaxed)) {
        if (!reader->Read(&pbFrame)) {
            break; // 流结束或出错
        }

        if (m_paused.load(std::memory_order_relaxed)) {
            continue;
        }

        FrameData frame;
        frame.timestamp = static_cast<int64_t>(pbFrame.timestamp_unix_ms());
        frame.frameId = static_cast<uint64_t>(pbFrame.frame_index());
        frame.sequence = frame.frameId;
        frame.detectMode = FrameData::MultiFreqEddy;
        frame.channelCount = 0;

        const int nPoints = pbFrame.point_results_size();
        frame.mfFreqPoints.resize(nPoints);
        for (int i = 0; i < nPoints; ++i) {
            const auto& pt = pbFrame.point_results(i);
            MultiFreqPointResult& r = frame.mfFreqPoints[i];
            r.frequencyFactor = pt.frequency_factor();
            r.frequencyHz = pt.frequency_hz();
            r.voltageReal = pt.voltage().real();
            r.voltageImag = pt.voltage().imag();
            r.currentReal = pt.current().real();
            r.currentImag = pt.current().imag();
            r.impedanceReal_raw = pt.impedance_real();
            r.impedanceImag_raw = pt.impedance_imag();
            r.impedanceMagnitude = pt.impedance_magnitude();
            r.impedancePhaseDeg = pt.impedance_phase_deg();
            r.normalizedImpedanceReal = pt.normalized_impedance_real();
            r.normalizedImpedanceImag = pt.normalized_impedance_imag();
            r.voltageMagnitude = pt.voltage_magnitude();
            r.currentMagnitude = pt.current_magnitude();
            r.valid = pt.valid();
        }

        m_lastFrameReceivedMs.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
        emit frameReceived(frame);

        // 节流 JSON 数据包
        if (shouldEmitRealtimePacket(pbFrame.timestamp_unix_ms())) {
            QJsonObject pkt;
            pkt["type"] = QStringLiteral("multifreq");
            pkt["frame_index"] = static_cast<qint64>(pbFrame.frame_index());
            pkt["n_points"] = nPoints;
            QJsonArray pts;
            for (int i = 0; i < nPoints; ++i) {
                const auto& pt = pbFrame.point_results(i);
                QJsonObject o;
                o["factor"] = pt.frequency_factor();
                o["freq_hz"] = pt.frequency_hz();
                o["z_real"] = pt.impedance_real();
                o["z_imag"] = pt.impedance_imag();
                o["z_mag"] = pt.impedance_magnitude();
                o["z_phase"] = pt.impedance_phase_deg();
                o["valid"] = pt.valid();
                pts.append(o);
            }
            pkt["points"] = pts;
            emit dataReceived(QJsonDocument(pkt).toJson(QJsonDocument::Compact), false);
        }
    }

    // 流结束
    const grpc::Status grpcStatus = reader->Finish();
    if (!grpcStatus.ok() && !m_stopStream.load(std::memory_order_relaxed)) {
        qWarning() << "[GrpcMultiFreqBackend] 流异常结束:" << grpcStatus.error_message().c_str();
        emitBackendStatus(QStringLiteral("多频涡流流中断"),
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

void GrpcMultiFreqBackend::setConnected(bool connected)
{
    bool prev = m_connected.exchange(connected, std::memory_order_relaxed);
    if (prev != connected) {
        emit connectionStateChanged(connected);
    }
}

void GrpcMultiFreqBackend::emitBackendStatus(const QString& status, const QString& detail)
{
    Q_UNUSED(status)
    qInfo() << "[GrpcMultiFreqBackend]" << status << detail;
}

void GrpcMultiFreqBackend::emitDeviceStatus()
{
    QJsonObject s;
    s["protocol"] = QStringLiteral("multifreq-grpc");
    s["endpoint"] = m_endpoint;
    s["mock"] = m_mockMode.load();
    emit backendStatusChanged(s);
}

bool GrpcMultiFreqBackend::shouldEmitRealtimePacket(qint64 timestampMs)
{
    const qint64 last = m_lastRealtimePacketMs.load(std::memory_order_relaxed);
    if (last == 0 || (timestampMs - last) >= m_realtimePacketIntervalMs) {
        m_lastRealtimePacketMs.store(timestampMs, std::memory_order_relaxed);
        return true;
    }
    return false;
}
