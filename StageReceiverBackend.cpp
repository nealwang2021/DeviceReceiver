#include "StageReceiverBackend.h"
#include "GrpcEndpointUtils.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QtGlobal>

#include <cmath>

#ifdef HAS_GRPC
#include <chrono>
#include <thread>
#endif

namespace {

bool parseAxisToken(const QString& token, int* axis)
{
    if (!axis) {
        return false;
    }

    const QString normalized = token.trimmed().toLower();
    if (normalized == "x" || normalized == "0") {
        *axis = 0;
        return true;
    }
    if (normalized == "y" || normalized == "1") {
        *axis = 1;
        return true;
    }
    if (normalized == "z" || normalized == "2") {
        *axis = 2;
        return true;
    }
    return false;
}

bool parsePlusToken(const QString& token, bool* plus)
{
    if (!plus) {
        return false;
    }

    const QString normalized = token.trimmed().toLower();
    if (normalized == "+" || normalized == "plus" || normalized == "1" || normalized == "true") {
        *plus = true;
        return true;
    }
    if (normalized == "-" || normalized == "minus" || normalized == "0" || normalized == "false") {
        *plus = false;
        return true;
    }
    return false;
}

bool parseEnableToken(const QString& token, bool* enabled)
{
    if (!enabled) {
        return false;
    }

    const QString normalized = token.trimmed().toLower();
    if (normalized == "on" || normalized == "enable" || normalized == "enabled" ||
        normalized == "1" || normalized == "true") {
        *enabled = true;
        return true;
    }
    if (normalized == "off" || normalized == "disable" || normalized == "disabled" ||
        normalized == "0" || normalized == "false") {
        *enabled = false;
        return true;
    }
    return false;
}

#ifdef HAS_GRPC
stage::Axis toStageAxis(int axis)
{
    switch (axis) {
    case 1:
        return stage::Y;
    case 2:
        return stage::Z;
    case 0:
    default:
        return stage::X;
    }
}
#endif

QString axisToText(int axis)
{
    switch (axis) {
    case 1:
        return QStringLiteral("Y");
    case 2:
        return QStringLiteral("Z");
    case 0:
    default:
        return QStringLiteral("X");
    }
}

} // namespace

StageReceiverBackend::StageReceiverBackend(QObject* parent)
    : IReceiverBackend(parent)
    , m_mockTimer(new QTimer(this))
    , m_reconnectTimer(new QTimer(this))
{
    connect(m_mockTimer, &QTimer::timeout, this, &StageReceiverBackend::onMockTick);
    connect(m_reconnectTimer, &QTimer::timeout, this, &StageReceiverBackend::onReconnectCheck);
}

StageReceiverBackend::~StageReceiverBackend()
{
    disconnectBackend();
}

bool StageReceiverBackend::connectBackend(const QString& endpoint)
{
    QString grpcEndpoint;
    QString stageIp;
    int stagePort = 0;
    if (!parseEndpoint(endpoint, &grpcEndpoint, &stageIp, &stagePort)) {
        emit commandError(QStringLiteral(
            "Stage endpoint 格式无效。示例: host:port、[IPv6]:port，或 grpc 段|台下位机段，如 "
            "[::1]:50052|192.168.1.10:9000"));
        return false;
    }

    m_endpoint = grpcEndpoint;
    m_stageIp = stageIp;
    m_stagePort = stagePort;

    if (m_mockMode.load()) {
        setConnected(true);
        emitBackendStatus(QStringLiteral("connected"),
                          QStringLiteral("Stage Mock 模式已就绪"));
        return true;
    }

#ifdef HAS_GRPC
    // ---- Real 模式：与 GrpcReceiverBackend 完全一致的连接策略 ----
    const int connectTimeoutMs = 6000;

    // extract host/port from parsed endpoint for DNS fallback
    QString parsedHost;
    int parsedPort = 0;
    {
        QString unused;
        GrpcEndpointUtils::parseChannelEndpoint(endpoint, &unused, nullptr, &parsedHost, &parsedPort);
    }

    auto tryConnect = [this, connectTimeoutMs](const QString& target, bool tls,
                                               const QString& tlsOverrideHost,
                                               QString* errorOut) -> bool {
        if (errorOut) errorOut->clear();

        std::shared_ptr<grpc::ChannelCredentials> creds;
        if (tls) {
            creds = grpc::SslCredentials(grpc::SslCredentialsOptions());
        } else {
            creds = grpc::InsecureChannelCredentials();
        }

        grpc::ChannelArguments args;
        args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
        if (tls && !tlsOverrideHost.trimmed().isEmpty()) {
            args.SetSslTargetNameOverride(tlsOverrideHost.toStdString());
        }

        m_channel = grpc::CreateCustomChannel(target.toStdString(), creds, args);
        if (!m_channel) {
            if (errorOut) *errorOut = QStringLiteral("CreateChannel failed");
            return false;
        }

        constexpr int kPollMs = 100;
        int waited = 0;
        grpc_connectivity_state lastState = m_channel->GetState(true);
        while (waited < connectTimeoutMs) {
            lastState = m_channel->GetState(true);
            if (lastState == GRPC_CHANNEL_READY) return true;
            const int slice = qMin(kPollMs, connectTimeoutMs - waited);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            waited += slice;
        }
        if (errorOut) {
            *errorOut = QStringLiteral("WaitForConnected timeout, state=%1").arg(static_cast<int>(lastState));
        }
        m_channel.reset();
        return false;
    };

    // Attempt order: TLS first, then insecure
    struct Attempt { bool tls; QString label; };
    QVector<Attempt> ordered;
    ordered.append({m_useTls, m_useTls ? QStringLiteral("TLS") : QStringLiteral("Insecure")});
    ordered.append({!m_useTls, !m_useTls ? QStringLiteral("TLS") : QStringLiteral("Insecure")});
    // dedup
    QVector<Attempt> deduped;
    for (const auto& a : ordered) {
        bool dup = false;
        for (const auto& b : deduped) { if (a.tls == b.tls) { dup = true; break; } }
        if (!dup) deduped.append(a);
    }

    bool connected = false;
    QStringList failures;
    for (const auto& attempt : deduped) {
        QString reason;
        qInfo() << "[Stage] 尝试连接:" << attempt.label << m_endpoint;
        if (tryConnect(m_endpoint, attempt.tls, QString(), &reason)) {
            connected = true;
            m_useTls = attempt.tls;
            break;
        }
        failures << QStringLiteral("%1 -> %2").arg(attempt.label, reason);
    }

    // DNS 解析 → 逐个 IPv4 地址重试（ngrok 等域名需要此步骤）
    QHostAddress testAddr;
    const bool isLiteralIp = testAddr.setAddress(parsedHost);
    if (!connected && !parsedHost.trimmed().isEmpty() && !isLiteralIp) {
        const QHostInfo hostInfo = QHostInfo::fromName(parsedHost);
        if (hostInfo.error() == QHostInfo::NoError) {
            for (const QHostAddress& addr : hostInfo.addresses()) {
                if (addr.protocol() != QAbstractSocket::IPv4Protocol) continue;
                const QString ipTarget = QStringLiteral("%1:%2").arg(addr.toString()).arg(parsedPort);
                for (const auto& attempt : deduped) {
                    QString reason;
                    const QString label = QStringLiteral("%1/ip=%2").arg(attempt.label, addr.toString());
                    qInfo() << "[Stage] 尝试连接:" << label << ipTarget;
                    if (tryConnect(ipTarget, attempt.tls,
                                   attempt.tls ? parsedHost : QString(), &reason)) {
                        connected = true;
                        m_useTls = attempt.tls;
                        m_endpoint = ipTarget;
                        break;
                    }
                    failures << QStringLiteral("%1 -> %2").arg(label, reason);
                }
                if (connected) break;
            }
        }
    }

    if (!connected) {
        emit commandError(QStringLiteral("Stage gRPC 连接失败: %1 (尝试: %2)")
                              .arg(m_endpoint, failures.join(QStringLiteral(" | "))));
        return false;
    }

    m_stub = stage::StageService::NewStub(m_channel);

    // 仅当 "grpc地址|工控机IP:端口" 格式时才调用 Connect RPC
    // ngrok 直连时 m_stageIp 为域名，服务器自行管理硬件，无需 Connect
    {
        QHostAddress stageIpAddr;
        const bool hasDownstream = stageIpAddr.setAddress(m_stageIp);
        if (hasDownstream && !callConnectRpc(m_stageIp, m_stagePort)) {
            m_stub.reset();
            m_channel.reset();
            return false;
        }
    }

    setConnected(true);
    qInfo() << "[Stage] 已连接:" << m_endpoint;
    emitBackendStatus(QStringLiteral("connected"),
                      QStringLiteral("已连接 StageService（grpc=%1）").arg(m_endpoint));
    return true;
#else
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持（请开启 HAS_GRPC）"));
    return false;
#endif
}

void StageReceiverBackend::disconnectBackend()
{
    stopAcquisition();

    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }

    if (!m_mockMode.load()) {
        callDisconnectRpc();
    }

#ifdef HAS_GRPC
    m_stub.reset();
    m_channel.reset();
#endif

    setConnected(false);
    emitBackendStatus(QStringLiteral("disconnected"), QStringLiteral("Stage 后端已断开"));
}

bool StageReceiverBackend::isBackendConnected() const
{
    return m_connected.load();
}

void StageReceiverBackend::startAcquisition(int intervalMs)
{
    if (!m_connected.load()) {
        return;
    }

    m_acquisitionIntervalMs = qMax(10, intervalMs);
    m_paused.store(false);

    if (m_mockMode.load()) {
        m_mockTimer->setInterval(m_acquisitionIntervalMs);
        m_mockTimer->start();
        return;
    }

    startPositionStream(m_acquisitionIntervalMs);
    m_reconnectTimer->setInterval(5000);
    m_reconnectTimer->start();
}

void StageReceiverBackend::stopAcquisition()
{
    if (m_mockTimer) {
        m_mockTimer->stop();
    }
    stopPositionStream();
}

void StageReceiverBackend::setPaused(bool paused)
{
    m_paused.store(paused);
    if (!m_mockMode.load()) {
        return;
    }

    if (paused) {
        m_mockTimer->stop();
    } else if (m_connected.load()) {
        m_mockTimer->setInterval(qMax(10, m_acquisitionIntervalMs));
        m_mockTimer->start();
    }
}

void StageReceiverBackend::setMockMode(bool enabled)
{
    if (m_mockMode.load() == enabled) {
        return;
    }

    if (m_connected.load()) {
        disconnectBackend();
    }

    m_mockMode.store(enabled);
    emitBackendStatus(
        QStringLiteral("modeChanged"),
        enabled ? QStringLiteral("切换到 Stage Mock 模式")
                : QStringLiteral("切换到 Stage 真实服务模式"));
}

void StageReceiverBackend::requestPositions()
{
    if (!m_connected.load()) {
        emit commandError(QStringLiteral("Stage 后端未连接"));
        return;
    }

    if (m_mockMode.load()) {
        onMockTick();
        emitCommandResult(true, QStringLiteral("get_positions"), QStringLiteral("Mock 位置已返回"));
        return;
    }

#ifdef HAS_GRPC
    if (!ensureStubReady()) {
        return;
    }

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(2000));

    stage::Empty req;
    stage::PositionsReply reply;
    grpc::Status status = m_stub->GetPositions(&ctx, req, &reply);
    if (!status.ok()) {
        const QString error = QStringLiteral("GetPositions RPC 失败: [%1] %2 (端点: %3, 超时: 2000ms)")
                                  .arg(status.error_code())
                                  .arg(QString::fromStdString(status.error_message()))
                                  .arg(m_endpoint);
        emit commandError(error);
        emitCommandResult(false, QStringLiteral("get_positions"), error);
        return;
    }

    const int xPulse = reply.has_x() ? reply.x().pulse() : 0;
    const int yPulse = reply.has_y() ? reply.y().pulse() : 0;
    const int zPulse = reply.has_z() ? reply.z().pulse() : 0;
    const double xMm = reply.has_x() ? reply.x().mm() : 0.0;
    const double yMm = reply.has_y() ? reply.y().mm() : 0.0;
    const double zMm = reply.has_z() ? reply.z().mm() : 0.0;
    const qint64 ts = (reply.unixms() > 0) ? reply.unixms() : QDateTime::currentMSecsSinceEpoch();

    emitPositionsPacket(QStringLiteral("poll"), xMm, yMm, zMm, xPulse, yPulse, zPulse, ts, true);
    emitCommandResult(true, QStringLiteral("get_positions"), QStringLiteral("位置读取成功"));
#else
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
#endif
}

void StageReceiverBackend::startPositionStream(int intervalMs)
{
    if (!m_connected.load()) {
        return;
    }

    m_acquisitionIntervalMs = qMax(10, intervalMs);
    if (m_mockMode.load()) {
        m_mockTimer->setInterval(m_acquisitionIntervalMs);
        m_mockTimer->start();
        return;
    }

    startStreamThread(m_acquisitionIntervalMs);
}

void StageReceiverBackend::stopPositionStream()
{
    if (m_mockTimer) {
        m_mockTimer->stop();
    }
    stopStreamThread();
}

void StageReceiverBackend::jog(int axis, bool plus, bool enable)
{
    if (!m_connected.load()) {
        emit commandError(QStringLiteral("Stage 后端未连接"));
        return;
    }

    if (m_mockMode.load()) {
        emitCommandResult(
            true,
            QStringLiteral("jog"),
            QStringLiteral("Mock Jog: axis=%1 plus=%2 enable=%3")
                .arg(axisToText(axis), plus ? QStringLiteral("true") : QStringLiteral("false"),
                     enable ? QStringLiteral("true") : QStringLiteral("false")));
        return;
    }

#ifdef HAS_GRPC
    if (!ensureStubReady()) {
        return;
    }

    stage::JogRequest req;
    req.set_axis(toStageAxis(axis));
    req.set_plus(plus);
    req.set_enable(enable);

    stage::Result reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(3000));

    const grpc::Status status = m_stub->Jog(&ctx, req, &reply);
    if (!status.ok()) {
        const QString error = QStringLiteral("Jog RPC 失败: [%1] %2 (端点: %3, 超时: 3000ms)")
                                  .arg(status.error_code())
                                  .arg(QString::fromStdString(status.error_message()))
                                  .arg(m_endpoint);
        emit commandError(error);
        emitCommandResult(false, QStringLiteral("jog"), error);
        return;
    }

    const QString message = QString::fromStdString(reply.message());
    if (!reply.ok()) {
        emit commandError(message.isEmpty() ? QStringLiteral("Jog 被服务端拒绝") : message);
        emitCommandResult(false, QStringLiteral("jog"), message);
        return;
    }

    emitCommandResult(true, QStringLiteral("jog"),
                      message.isEmpty() ? QStringLiteral("Jog 执行成功") : message);
#else
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
#endif
}

void StageReceiverBackend::moveAbs(double xMm, double yMm, double zMm, int timeoutMs)
{
    if (!m_connected.load()) {
        emit commandError(QStringLiteral("Stage 后端未连接"));
        return;
    }

    if (m_mockMode.load()) {
        emitCommandResult(
            true,
            QStringLiteral("move_abs"),
            QStringLiteral("Mock MoveAbs: x=%1 y=%2 z=%3")
                .arg(xMm, 0, 'f', 3)
                .arg(yMm, 0, 'f', 3)
                .arg(zMm, 0, 'f', 3));
        return;
    }

#ifdef HAS_GRPC
    if (!ensureStubReady()) {
        return;
    }

    stage::MoveAbsRequest req;
    req.set_xmm(xMm);
    req.set_ymm(yMm);
    req.set_zmm(zMm);
    req.set_timeoutms(qMax(100, timeoutMs));

    stage::Result reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(qMax(500, timeoutMs)));

    const grpc::Status status = m_stub->MoveAbs(&ctx, req, &reply);
    if (!status.ok()) {
        const QString error = QStringLiteral("MoveAbs RPC 失败: [%1] %2 (端点: %3, 超时: %4ms)")
                                  .arg(status.error_code())
                                  .arg(QString::fromStdString(status.error_message()))
                                  .arg(m_endpoint)
                                  .arg(qMax(500, timeoutMs));
        emit commandError(error);
        emitCommandResult(false, QStringLiteral("move_abs"), error);
        return;
    }

    const QString message = QString::fromStdString(reply.message());
    if (!reply.ok()) {
        emit commandError(message.isEmpty() ? QStringLiteral("MoveAbs 被服务端拒绝") : message);
        emitCommandResult(false, QStringLiteral("move_abs"), message);
        return;
    }

    emitCommandResult(true, QStringLiteral("move_abs"),
                      message.isEmpty() ? QStringLiteral("MoveAbs 执行成功") : message);
#else
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
#endif
}

void StageReceiverBackend::moveRel(int axis, double deltaMm, int timeoutMs)
{
    if (!m_connected.load()) {
        emit commandError(QStringLiteral("Stage 后端未连接"));
        return;
    }

    if (m_mockMode.load()) {
        emitCommandResult(
            true,
            QStringLiteral("move_rel"),
            QStringLiteral("Mock MoveRel: axis=%1 delta=%2")
                .arg(axisToText(axis))
                .arg(deltaMm, 0, 'f', 3));
        return;
    }

#ifdef HAS_GRPC
    if (!ensureStubReady()) {
        return;
    }

    stage::MoveRelRequest req;
    req.set_axis(toStageAxis(axis));
    req.set_deltamm(deltaMm);
    req.set_timeoutms(qMax(100, timeoutMs));

    stage::Result reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(qMax(500, timeoutMs)));

    const grpc::Status status = m_stub->MoveRel(&ctx, req, &reply);
    if (!status.ok()) {
        const QString error = QStringLiteral("MoveRel RPC 失败: [%1] %2 (端点: %3, 超时: %4ms)")
                                  .arg(status.error_code())
                                  .arg(QString::fromStdString(status.error_message()))
                                  .arg(m_endpoint)
                                  .arg(qMax(500, timeoutMs));
        emit commandError(error);
        emitCommandResult(false, QStringLiteral("move_rel"), error);
        return;
    }

    const QString message = QString::fromStdString(reply.message());
    if (!reply.ok()) {
        emit commandError(message.isEmpty() ? QStringLiteral("MoveRel 被服务端拒绝") : message);
        emitCommandResult(false, QStringLiteral("move_rel"), message);
        return;
    }

    emitCommandResult(true, QStringLiteral("move_rel"),
                      message.isEmpty() ? QStringLiteral("MoveRel 执行成功") : message);
#else
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
#endif
}

void StageReceiverBackend::setSpeed(quint32 speedPulsePerSec, quint32 accelMs)
{
    if (!m_connected.load()) {
        emit commandError(QStringLiteral("Stage 后端未连接"));
        return;
    }

    if (m_mockMode.load()) {
        emitCommandResult(
            true,
            QStringLiteral("set_speed"),
            QStringLiteral("Mock SetSpeed: speed=%1 accel=%2")
                .arg(speedPulsePerSec)
                .arg(accelMs));
        return;
    }

#ifdef HAS_GRPC
    if (!ensureStubReady()) {
        return;
    }

    stage::SetSpeedRequest req;
    req.set_speedpulsepersec(speedPulsePerSec);
    req.set_accelms(accelMs);

    stage::Result reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(3000));

    const grpc::Status status = m_stub->SetSpeed(&ctx, req, &reply);
    if (!status.ok()) {
        const QString error = QStringLiteral("SetSpeed RPC 失败: [%1] %2 (端点: %3, 超时: 3000ms)")
                                  .arg(status.error_code())
                                  .arg(QString::fromStdString(status.error_message()))
                                  .arg(m_endpoint);
        emit commandError(error);
        emitCommandResult(false, QStringLiteral("set_speed"), error);
        return;
    }

    const QString message = QString::fromStdString(reply.message());
    if (!reply.ok()) {
        emit commandError(message.isEmpty() ? QStringLiteral("SetSpeed 被服务端拒绝") : message);
        emitCommandResult(false, QStringLiteral("set_speed"), message);
        return;
    }

    emitCommandResult(true, QStringLiteral("set_speed"),
                      message.isEmpty() ? QStringLiteral("速度参数更新成功") : message);
#else
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
#endif
}

void StageReceiverBackend::startScan(int mode,
                                     double xs,
                                     double xe,
                                     double ys,
                                     double ye,
                                     double yStep,
                                     double zFix)
{
    if (!m_connected.load()) {
        emit commandError(QStringLiteral("Stage 后端未连接"));
        return;
    }

    if (m_mockMode.load()) {
        emitCommandResult(true, QStringLiteral("start_scan"), QStringLiteral("Mock 扫描已启动"));
        return;
    }

#ifdef HAS_GRPC
    if (!ensureStubReady()) {
        return;
    }

    stage::ScanRequest req;
    req.set_mode(mode == 1 ? stage::ALTERNATE_RETURN : stage::SNAKE);
    req.set_xs(xs);
    req.set_xe(xe);
    req.set_ys(ys);
    req.set_ye(ye);
    req.set_ystep(yStep);
    req.set_zfix(zFix);

    stage::Result reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(5000));

    const grpc::Status status = m_stub->StartScan(&ctx, req, &reply);
    if (!status.ok()) {
        const QString error = QStringLiteral("StartScan RPC 失败: [%1] %2 (端点: %3, 超时: 5000ms)")
                                  .arg(status.error_code())
                                  .arg(QString::fromStdString(status.error_message()))
                                  .arg(m_endpoint);
        emit commandError(error);
        emitCommandResult(false, QStringLiteral("start_scan"), error);
        return;
    }

    const QString message = QString::fromStdString(reply.message());
    if (!reply.ok()) {
        emit commandError(message.isEmpty() ? QStringLiteral("StartScan 被服务端拒绝") : message);
        emitCommandResult(false, QStringLiteral("start_scan"), message);
        return;
    }

    emitCommandResult(true, QStringLiteral("start_scan"),
                      message.isEmpty() ? QStringLiteral("扫描已启动") : message);
#else
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
#endif
}

void StageReceiverBackend::stopScan()
{
    if (!m_connected.load()) {
        emit commandError(QStringLiteral("Stage 后端未连接"));
        return;
    }

    if (m_mockMode.load()) {
        emitCommandResult(true, QStringLiteral("stop_scan"), QStringLiteral("Mock 扫描已停止"));
        return;
    }

#ifdef HAS_GRPC
    if (!ensureStubReady()) {
        return;
    }

    stage::Empty req;
    stage::Result reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(3000));

    const grpc::Status status = m_stub->StopScan(&ctx, req, &reply);
    if (!status.ok()) {
        const QString error = QStringLiteral("StopScan RPC 失败: [%1] %2 (端点: %3, 超时: 3000ms)")
                                  .arg(status.error_code())
                                  .arg(QString::fromStdString(status.error_message()))
                                  .arg(m_endpoint);
        emit commandError(error);
        emitCommandResult(false, QStringLiteral("stop_scan"), error);
        return;
    }

    const QString message = QString::fromStdString(reply.message());
    if (!reply.ok()) {
        emit commandError(message.isEmpty() ? QStringLiteral("StopScan 被服务端拒绝") : message);
        emitCommandResult(false, QStringLiteral("stop_scan"), message);
        return;
    }

    emitCommandResult(true, QStringLiteral("stop_scan"),
                      message.isEmpty() ? QStringLiteral("扫描已停止") : message);
#else
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
#endif
}

void StageReceiverBackend::requestScanStatus()
{
    if (!m_connected.load()) {
        emit commandError(QStringLiteral("Stage 后端未连接"));
        return;
    }

    if (m_mockMode.load()) {
        QJsonObject mockStatus;
        mockStatus.insert(QStringLiteral("type"), QStringLiteral("stageScanStatus"));
        mockStatus.insert(QStringLiteral("running"), false);
        mockStatus.insert(QStringLiteral("status"), QStringLiteral("mock-idle"));
        emit dataReceived(QJsonDocument(mockStatus).toJson(QJsonDocument::Compact), false);
        emitCommandResult(true, QStringLiteral("scan_status"), QStringLiteral("Mock 状态: idle"));
        return;
    }

#ifdef HAS_GRPC
    if (!ensureStubReady()) {
        return;
    }

    stage::Empty req;
    stage::ScanStatusReply reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(2000));

    const grpc::Status status = m_stub->GetScanStatus(&ctx, req, &reply);
    if (!status.ok()) {
        const QString error = QStringLiteral("GetScanStatus RPC 失败: [%1] %2 (端点: %3, 超时: 2000ms)")
                                  .arg(status.error_code())
                                  .arg(QString::fromStdString(status.error_message()))
                                  .arg(m_endpoint);
        emit commandError(error);
        emitCommandResult(false, QStringLiteral("scan_status"), error);
        return;
    }

    QJsonObject pkt;
    pkt.insert(QStringLiteral("type"), QStringLiteral("stageScanStatus"));
    pkt.insert(QStringLiteral("running"), reply.running());
    pkt.insert(QStringLiteral("status"), QString::fromStdString(reply.status()));
    pkt.insert(QStringLiteral("timestamp"), QDateTime::currentMSecsSinceEpoch());
    emit dataReceived(QJsonDocument(pkt).toJson(QJsonDocument::Compact), false);

    emitCommandResult(
        true,
        QStringLiteral("scan_status"),
        QStringLiteral("running=%1 status=%2")
            .arg(reply.running() ? QStringLiteral("true") : QStringLiteral("false"),
                 QString::fromStdString(reply.status())));
#else
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
#endif
}

void StageReceiverBackend::sendCommand(const QByteArray& command)
{
    if (!m_connected.load()) {
        emit commandError(QStringLiteral("Stage 后端未连接"));
        return;
    }
    if (command.isEmpty()) {
        emit commandError(QStringLiteral("指令不能为空"));
        return;
    }

    emit commandSent(command);

    const QString text = QString::fromUtf8(command).trimmed();
    if (text.isEmpty()) {
        emit commandError(QStringLiteral("Stage 后端仅支持文本指令"));
        return;
    }

    const QStringList args = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (args.isEmpty()) {
        emit commandError(QStringLiteral("无效指令"));
        return;
    }

    const QString cmd = args.first().toLower();

    if (cmd == QStringLiteral("help")) {
        emitCommandResult(
            true,
            QStringLiteral("help"),
            QStringLiteral("支持指令: get_positions, start_stream [ms], stop_stream, "
                           "jog <x|y|z> <+|-> <on|off>, move_abs <x> <y> <z> [timeoutMs], "
                           "move_rel <x|y|z> <delta> [timeoutMs], set_speed <speed> <accelMs>, "
                           "start_scan <snake|alternate_return> <xs> <xe> <ys> <ye> <yStep> <zFix>, "
                           "stop_scan, scan_status"));
        return;
    }

    if (cmd == QStringLiteral("get_positions") || cmd == QStringLiteral("positions")) {
        requestPositions();
        return;
    }

    if (cmd == QStringLiteral("start_stream") || cmd == QStringLiteral("position_stream")) {
        int intervalMs = m_acquisitionIntervalMs;
        if (args.size() >= 2) {
            bool ok = false;
            const int parsed = args.at(1).toInt(&ok);
            if (!ok || parsed < 10) {
                emit commandError(QStringLiteral("start_stream 参数无效，需 >= 10"));
                return;
            }
            intervalMs = parsed;
        }
        startPositionStream(intervalMs);
        emitCommandResult(true, QStringLiteral("start_stream"),
                          QStringLiteral("位置流已启动，interval=%1ms").arg(intervalMs));
        return;
    }

    if (cmd == QStringLiteral("stop_stream")) {
        stopPositionStream();
        emitCommandResult(true, QStringLiteral("stop_stream"), QStringLiteral("位置流已停止"));
        return;
    }

    if (cmd == QStringLiteral("jog")) {
        if (args.size() < 4) {
            emit commandError(QStringLiteral("用法: jog <x|y|z> <+|-> <on|off>"));
            return;
        }

        int axis = 0;
        bool plus = true;
        bool enable = true;
        if (!parseAxisToken(args.at(1), &axis) ||
            !parsePlusToken(args.at(2), &plus) ||
            !parseEnableToken(args.at(3), &enable)) {
            emit commandError(QStringLiteral("jog 参数无效"));
            return;
        }

        jog(axis, plus, enable);
        return;
    }

    if (cmd == QStringLiteral("move_abs")) {
        if (args.size() < 4) {
            emit commandError(QStringLiteral("用法: move_abs <x> <y> <z> [timeoutMs]"));
            return;
        }

        bool okX = false;
        bool okY = false;
        bool okZ = false;
        const double x = args.at(1).toDouble(&okX);
        const double y = args.at(2).toDouble(&okY);
        const double z = args.at(3).toDouble(&okZ);
        if (!okX || !okY || !okZ) {
            emit commandError(QStringLiteral("move_abs 坐标参数无效"));
            return;
        }

        int timeoutMs = 10000;
        if (args.size() >= 5) {
            bool okTimeout = false;
            const int parsed = args.at(4).toInt(&okTimeout);
            if (!okTimeout || parsed <= 0) {
                emit commandError(QStringLiteral("move_abs timeoutMs 参数无效"));
                return;
            }
            timeoutMs = parsed;
        }

        moveAbs(x, y, z, timeoutMs);
        return;
    }

    if (cmd == QStringLiteral("move_rel")) {
        if (args.size() < 3) {
            emit commandError(QStringLiteral("用法: move_rel <x|y|z> <delta> [timeoutMs]"));
            return;
        }

        int axis = 0;
        if (!parseAxisToken(args.at(1), &axis)) {
            emit commandError(QStringLiteral("move_rel axis 参数无效"));
            return;
        }

        bool okDelta = false;
        const double delta = args.at(2).toDouble(&okDelta);
        if (!okDelta) {
            emit commandError(QStringLiteral("move_rel delta 参数无效"));
            return;
        }

        int timeoutMs = 10000;
        if (args.size() >= 4) {
            bool okTimeout = false;
            const int parsed = args.at(3).toInt(&okTimeout);
            if (!okTimeout || parsed <= 0) {
                emit commandError(QStringLiteral("move_rel timeoutMs 参数无效"));
                return;
            }
            timeoutMs = parsed;
        }

        moveRel(axis, delta, timeoutMs);
        return;
    }

    if (cmd == QStringLiteral("set_speed")) {
        if (args.size() < 3) {
            emit commandError(QStringLiteral("用法: set_speed <speedPulsePerSec> <accelMs>"));
            return;
        }

        bool okSpeed = false;
        bool okAccel = false;
        const uint speed = args.at(1).toUInt(&okSpeed);
        const uint accel = args.at(2).toUInt(&okAccel);
        if (!okSpeed || !okAccel) {
            emit commandError(QStringLiteral("set_speed 参数无效"));
            return;
        }

        setSpeed(speed, accel);
        return;
    }

    if (cmd == QStringLiteral("start_scan")) {
        if (args.size() < 8) {
            emit commandError(QStringLiteral("用法: start_scan <snake|alternate_return> <xs> <xe> <ys> <ye> <yStep> <zFix>"));
            return;
        }

        int mode = 0;
        const QString modeText = args.at(1).trimmed().toLower();
        if (modeText == QStringLiteral("1") || modeText == QStringLiteral("alternate_return") ||
            modeText == QStringLiteral("alternate")) {
            mode = 1;
        } else if (modeText != QStringLiteral("0") && modeText != QStringLiteral("snake")) {
            emit commandError(QStringLiteral("start_scan mode 参数无效（仅支持 snake/alternate_return）"));
            return;
        }

        bool okXs = false;
        bool okXe = false;
        bool okYs = false;
        bool okYe = false;
        bool okStep = false;
        bool okZ = false;
        const double xs = args.at(2).toDouble(&okXs);
        const double xe = args.at(3).toDouble(&okXe);
        const double ys = args.at(4).toDouble(&okYs);
        const double ye = args.at(5).toDouble(&okYe);
        const double yStep = args.at(6).toDouble(&okStep);
        const double zFix = args.at(7).toDouble(&okZ);
        if (!okXs || !okXe || !okYs || !okYe || !okStep || !okZ) {
            emit commandError(QStringLiteral("start_scan 数值参数无效"));
            return;
        }

        startScan(mode, xs, xe, ys, ye, yStep, zFix);
        return;
    }

    if (cmd == QStringLiteral("stop_scan")) {
        stopScan();
        return;
    }

    if (cmd == QStringLiteral("scan_status") || cmd == QStringLiteral("get_scan_status")) {
        requestScanStatus();
        return;
    }

    emit commandError(QStringLiteral("未知 Stage 指令: %1").arg(cmd));
}

void StageReceiverBackend::sendCommand(const QString& command, bool isHex)
{
    if (isHex) {
        emit commandError(QStringLiteral("Stage 后端不支持 HEX 指令模式，请使用文本命令"));
        return;
    }
    sendCommand(command.toUtf8());
}

void StageReceiverBackend::onMockTick()
{
    if (m_paused.load() || !m_connected.load()) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double t = static_cast<double>(nowMs % 60000) / 1000.0;

    const double xMm = 20.0 * std::sin(t * 0.8);
    const double yMm = 15.0 * std::cos(t * 0.6);
    const double zMm = 5.0 + 3.0 * std::sin(t * 0.4);

    const int xPulse = static_cast<int>(xMm * 1000.0);
    const int yPulse = static_cast<int>(yMm * 1000.0);
    const int zPulse = static_cast<int>(zMm * 1000.0);

    emitPositionsPacket(QStringLiteral("mock"), xMm, yMm, zMm, xPulse, yPulse, zPulse, nowMs, false);
}

void StageReceiverBackend::onReconnectCheck()
{
#ifdef HAS_GRPC
    if (m_mockMode.load() || !m_channel) {
        return;
    }

    const grpc_connectivity_state state = m_channel->GetState(false);
    if (state != GRPC_CHANNEL_TRANSIENT_FAILURE && state != GRPC_CHANNEL_SHUTDOWN) {
        return;
    }

    emitBackendStatus(QStringLiteral("reconnecting"),
                      QStringLiteral("Stage channel 异常(%1)，尝试重连...").arg(static_cast<int>(state)));

    stopStreamThread();
    setConnected(false);

    const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(2500);
    if (!m_channel->WaitForConnected(deadline)) {
        emitBackendStatus(QStringLiteral("reconnectFailed"), QStringLiteral("Stage channel 重连失败"));
        return;
    }

    m_stub = stage::StageService::NewStub(m_channel);
    {
        QHostAddress stageIpAddr;
        const bool hasDownstream = stageIpAddr.setAddress(m_stageIp);
        if (hasDownstream && !callConnectRpc(m_stageIp, m_stagePort)) {
            emitBackendStatus(QStringLiteral("reconnectFailed"), QStringLiteral("Stage Connect RPC 重试失败"));
            return;
        }
    }

    setConnected(true);
    startStreamThread(m_acquisitionIntervalMs);
    emitBackendStatus(QStringLiteral("reconnected"), QStringLiteral("Stage 重连成功并已恢复位置流"));
#endif
}

void StageReceiverBackend::startStreamThread(int intervalMs)
{
    stopStreamThread();
    m_stopStream.store(false);
    m_streamThread = std::thread([this, intervalMs]() {
        streamLoop(intervalMs);
    });
}

void StageReceiverBackend::stopStreamThread()
{
    m_stopStream.store(true);

#ifdef HAS_GRPC
    // TryCancel() 终止阻塞中的 Read() 调用；与 streamLoop 中写入 m_streamCtx 互斥。
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
    // streamLoop 已退出，reset 仍放在锁内以保持一致性。
    {
        std::lock_guard<std::mutex> lock(m_streamStateMutex);
        m_streamCtx.reset();
    }
#endif
}

void StageReceiverBackend::streamLoop(int intervalMs)
{
#ifdef HAS_GRPC
    if (!m_stub) {
        QMetaObject::invokeMethod(this, [this]() {
            emit commandError(QStringLiteral("Stage 流线程启动失败：Stub 为空"));
        }, Qt::QueuedConnection);
        return;
    }

    auto ctx = std::make_unique<grpc::ClientContext>();
    {
        std::lock_guard<std::mutex> lock(m_streamStateMutex);
        m_streamCtx = std::move(ctx);
    }

    stage::PositionStreamRequest req;
    req.set_intervalms(qMax(10, intervalMs));

    auto reader = m_stub->PositionStream(m_streamCtx.get(), req);
    stage::PositionsReply reply;

    while (!m_stopStream.load() && reader->Read(&reply)) {
        if (m_paused.load()) {
            continue;
        }

        const int xPulse = reply.has_x() ? reply.x().pulse() : 0;
        const int yPulse = reply.has_y() ? reply.y().pulse() : 0;
        const int zPulse = reply.has_z() ? reply.z().pulse() : 0;
        const double xMm = reply.has_x() ? reply.x().mm() : 0.0;
        const double yMm = reply.has_y() ? reply.y().mm() : 0.0;
        const double zMm = reply.has_z() ? reply.z().mm() : 0.0;
        const qint64 ts = (reply.unixms() > 0) ? reply.unixms() : QDateTime::currentMSecsSinceEpoch();

        emitPositionsPacket(QStringLiteral("stream"), xMm, yMm, zMm, xPulse, yPulse, zPulse, ts, false);
    }

    const grpc::Status status = reader->Finish();
    if (!m_stopStream.load()) {
        const QString error = status.ok()
            ? QStringLiteral("Stage 位置流意外结束")
            : QStringLiteral("Stage 位置流错误: [%1] %2")
                .arg(status.error_code())
                .arg(QString::fromStdString(status.error_message()));

        QMetaObject::invokeMethod(this, [this, error]() {
            // 位置流失败不切断连接，保留手动 RPC（Jog/MoveAbs 等）可用
            emitBackendStatus(QStringLiteral("streamClosed"), error);
            emit commandError(error);
        }, Qt::QueuedConnection);
    }
#else
    Q_UNUSED(intervalMs)
    QMetaObject::invokeMethod(this, [this]() {
        emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
    }, Qt::QueuedConnection);
#endif
}

void StageReceiverBackend::setConnected(bool connected)
{
    const bool previous = m_connected.exchange(connected);
    if (previous != connected) {
        emit connectionStateChanged(connected);
    }
}

void StageReceiverBackend::emitBackendStatus(const QString& status, const QString& detail)
{
    QJsonObject pkt;
    pkt.insert(QStringLiteral("type"), QStringLiteral("stageStatus"));
    pkt.insert(QStringLiteral("status"), status);
    pkt.insert(QStringLiteral("mode"), m_mockMode.load() ? QStringLiteral("mock") : QStringLiteral("real"));
    pkt.insert(QStringLiteral("endpoint"), m_endpoint);
    pkt.insert(QStringLiteral("stageIp"), m_stageIp);
    pkt.insert(QStringLiteral("stagePort"), m_stagePort);
    pkt.insert(QStringLiteral("detail"), detail);
    emit dataReceived(QJsonDocument(pkt).toJson(QJsonDocument::Compact), false);
}

void StageReceiverBackend::emitCommandResult(bool ok, const QString& command, const QString& message)
{
    QJsonObject pkt;
    pkt.insert(QStringLiteral("type"), QStringLiteral("stageCommandResult"));
    pkt.insert(QStringLiteral("ok"), ok);
    pkt.insert(QStringLiteral("command"), command);
    pkt.insert(QStringLiteral("message"), message);
    pkt.insert(QStringLiteral("timestamp"), QDateTime::currentMSecsSinceEpoch());
    emit dataReceived(QJsonDocument(pkt).toJson(QJsonDocument::Compact), false);
}

// 将 GetPositions / PositionStream 的各轴 mm 与 pulse 下发：mm 为工程单位毫米，pulse 为下位机原始位置计数。
void StageReceiverBackend::emitPositionsPacket(const QString& source,
                                               double xMm,
                                               double yMm,
                                               double zMm,
                                               int xPulse,
                                               int yPulse,
                                               int zPulse,
                                               qint64 unixMs,
                                               bool forceEmit)
{
    const qint64 timestampMs = (unixMs > 0) ? unixMs : QDateTime::currentMSecsSinceEpoch();

    emit stagePoseUpdated(xMm, yMm, zMm, xPulse, yPulse, zPulse, timestampMs);

    if (!forceEmit && !shouldEmitRealtimePacket(timestampMs)) {
        return;
    }

    QJsonObject pkt;
    pkt.insert(QStringLiteral("type"), QStringLiteral("stagePositions"));
    pkt.insert(QStringLiteral("source"), source);
    pkt.insert(QStringLiteral("xMm"), xMm);
    pkt.insert(QStringLiteral("yMm"), yMm);
    pkt.insert(QStringLiteral("zMm"), zMm);
    pkt.insert(QStringLiteral("xPulse"), xPulse);
    pkt.insert(QStringLiteral("yPulse"), yPulse);
    pkt.insert(QStringLiteral("zPulse"), zPulse);
    pkt.insert(QStringLiteral("unixMs"), timestampMs);
    emit dataReceived(QJsonDocument(pkt).toJson(QJsonDocument::Compact), false);
}

bool StageReceiverBackend::shouldEmitRealtimePacket(qint64 timestampMs)
{
    const qint64 previous = m_lastRealtimePacketMs.load(std::memory_order_relaxed);
    if (timestampMs - previous < m_realtimePacketIntervalMs) {
        return false;
    }

    m_lastRealtimePacketMs.store(timestampMs, std::memory_order_relaxed);
    return true;
}

bool StageReceiverBackend::parseEndpoint(const QString& endpoint,
                                         QString* grpcEndpoint,
                                         QString* stageIp,
                                         int* stagePort)
{
    if (!grpcEndpoint || !stageIp || !stagePort) {
        return false;
    }

    const QString raw = endpoint.trimmed();
    if (raw.isEmpty()) {
        return false;
    }

    // 支持 "|" 分隔的复合格式: grpc段|台下位机段
    const QStringList segments = raw.split('|', Qt::KeepEmptyParts);
    const QString grpcPart = segments.value(0).trimmed();
    const QString stagePart = (segments.size() > 1) ? segments.at(1).trimmed() : QString();

    // 使用 GrpcEndpointUtils::parseChannelEndpoint，支持 https://ngrok 等 TLS 地址
    QString grpcTarget;
    bool useTls = false;
    QString grpcHost;
    int grpcPort = 0;
    if (!GrpcEndpointUtils::parseChannelEndpoint(grpcPart, &grpcTarget, &useTls, &grpcHost, &grpcPort)) {
        return false;
    }

    // 将 useTls 信息编码到 grpcTarget 中（如 "andres-xxx.ngrok-free.dev:443"）
    // connectBackend 中通过检查目标是否包含 ngrok-free.dev 或 useTls 标志来选择 TLS
    *grpcEndpoint = grpcTarget;

    if (stagePart.isEmpty()) {
        *stageIp = grpcHost;
        *stagePort = grpcPort;
        m_useTls = useTls;
        return true;
    }

    // 台下位机段仅支持 host:port 格式（内网地址，不支持 TLS URL）
    QString parsedStageIp;
    int parsedStagePort = 0;
    if (!GrpcEndpointUtils::parseHostPort(stagePart, nullptr, &parsedStageIp, &parsedStagePort)) {
        return false;
    }

    *stageIp = parsedStageIp;
    *stagePort = parsedStagePort;
    m_useTls = useTls;
    return true;
}

bool StageReceiverBackend::ensureStubReady()
{
#ifdef HAS_GRPC
    if (m_stub) {
        return true;
    }

    emit commandError(QStringLiteral("StageService Stub 未初始化"));
    return false;
#else
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
    return false;
#endif
}

bool StageReceiverBackend::callConnectRpc(const QString& ip, int port)
{
    if (m_mockMode.load()) {
        return true;
    }

#ifdef HAS_GRPC
    if (!ensureStubReady()) {
        return false;
    }

    stage::ConnectRequest req;
    req.set_ip(ip.toStdString());
    req.set_port(port);

    stage::Result reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(3000));

    const grpc::Status status = m_stub->Connect(&ctx, req, &reply);
    if (!status.ok()) {
        const QString error = QStringLiteral("Connect RPC 失败: [%1] %2 (端点: %3, 超时: 3000ms)")
                                  .arg(status.error_code())
                                  .arg(QString::fromStdString(status.error_message()))
                                  .arg(m_endpoint);
        emit commandError(error);
        emitBackendStatus(QStringLiteral("connectRpcFailed"), error);
        return false;
    }

    const QString message = QString::fromStdString(reply.message());
    if (!reply.ok()) {
        const QString error = message.isEmpty() ? QStringLiteral("Stage Connect 被服务端拒绝") : message;
        emit commandError(error);
        emitBackendStatus(QStringLiteral("connectRpcRejected"), error);
        return false;
    }

    emitCommandResult(true, QStringLiteral("connect"),
                      message.isEmpty() ? QStringLiteral("Stage Connect 成功") : message);
    return true;
#else
    Q_UNUSED(ip)
    Q_UNUSED(port)
    emit commandError(QStringLiteral("当前构建未启用 gRPC 支持"));
    return false;
#endif
}

bool StageReceiverBackend::callDisconnectRpc()
{
    if (m_mockMode.load()) {
        return true;
    }

#ifdef HAS_GRPC
    if (!m_stub) {
        return false;
    }

    stage::Empty req;
    stage::Result reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(2000));

    const grpc::Status status = m_stub->Disconnect(&ctx, req, &reply);
    if (!status.ok()) {
        emitBackendStatus(
            QStringLiteral("disconnectRpcFailed"),
            QStringLiteral("Disconnect RPC 失败: [%1] %2 (端点: %3)")
                .arg(status.error_code())
                .arg(QString::fromStdString(status.error_message()))
                .arg(m_endpoint)
                .arg(m_endpoint)
                .arg(status.error_code())
                .arg(QString::fromStdString(status.error_message())));
        return false;
    }

    emitCommandResult(
        true,
        QStringLiteral("disconnect"),
        QString::fromStdString(reply.message()).isEmpty()
            ? QStringLiteral("Stage Disconnect 成功")
            : QString::fromStdString(reply.message()));
    return true;
#else
    return false;
#endif
}
