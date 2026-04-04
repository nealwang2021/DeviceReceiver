/**
 * StageReceiverBackend 集成测试（与三轴台界面命令路径等价，不依赖 MainWindow）
 * 自动启动 stage_grpc_test_server.py 子进程。
 */
#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QTextStream>
#include <QDateTime>

#include "StageReceiverBackend.h"
#include "stage_test_paths.h"

namespace {

constexpr int kStagePort = 50053;
const QString kEndpoint = QStringLiteral("127.0.0.1:%1").arg(kStagePort);

QString stepLogPath()
{
    // 由于该测试由 ctest 在构建目录的 release 路径下运行，
    // 所以直接写入当前可执行文件所在目录即可。
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("stage_test_integration_steps_%1.log").arg(kStagePort));
}

void stepLog(const QString& line)
{
    QFile f(stepLogPath());
    if (!f.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << " " << line << "\n";
    f.close();
}

bool waitForSignal(QSignalSpy& spy, int minCount, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (spy.count() < minCount && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    return spy.count() >= minCount;
}

QJsonObject firstJsonPacketOfType(const QList<QList<QVariant>>& args, const QString& type)
{
    for (const QList<QVariant>& call : args) {
        if (call.isEmpty()) {
            continue;
        }
        const QByteArray data = call.first().toByteArray();
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        const QJsonObject o = doc.object();
        if (o.value(QStringLiteral("type")).toString() == type) {
            return o;
        }
    }
    return QJsonObject();
}

} // namespace

class StageIntegrationTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void test_connect_disconnect();
    void test_get_positions();
    void test_move_abs_and_rel();
    void test_jog_and_speed();
    void test_scan_flow();

private:
    QProcess* m_server = nullptr;
};

void StageIntegrationTest::initTestCase()
{
    stepLog(QStringLiteral("initTestCase: starting stage_grpc_test_server.py on port=%1").arg(kStagePort));
    m_server = new QProcess;
    m_server->setProcessChannelMode(QProcess::MergedChannels);

    const QString script = QString::fromUtf8(STAGE_TEST_SERVER_SCRIPT);
    QVERIFY2(!script.isEmpty(), "STAGE_TEST_SERVER_SCRIPT not defined at build time");

    QString python = QStringLiteral("python");
#ifdef Q_OS_WIN
    if (!QProcessEnvironment::systemEnvironment().value(QStringLiteral("PYTHON")).isEmpty()) {
        python = QProcessEnvironment::systemEnvironment().value(QStringLiteral("PYTHON"));
    }
#endif

    m_server->start(python, QStringList() << script << QStringLiteral("--port") << QString::number(kStagePort));
    QVERIFY2(m_server->waitForStarted(5000), "Failed to start stage_grpc_test_server.py");

    // Allow gRPC server to bind and start listening
    QThread::msleep(2500);
    if (m_server->state() != QProcess::Running) {
        qWarning() << "stage server output:" << m_server->readAllStandardOutput();
        QFAIL("stage_grpc_test_server.py exited prematurely");
    }
    stepLog(QStringLiteral("initTestCase: stage server running"));
}

void StageIntegrationTest::cleanupTestCase()
{
    stepLog(QStringLiteral("cleanupTestCase: stopping stage server"));
    if (!m_server) {
        return;
    }
    if (m_server->state() == QProcess::Running) {
        m_server->terminate();
        if (!m_server->waitForFinished(3000)) {
            m_server->kill();
            m_server->waitForFinished(1000);
        }
    }
    delete m_server;
    m_server = nullptr;
}

void StageIntegrationTest::test_connect_disconnect()
{
    stepLog(QStringLiteral("test_connect_disconnect: connectBackend(%1)").arg(kEndpoint));
    StageReceiverBackend backend;
    QSignalSpy spyConn(&backend, &StageReceiverBackend::connectionStateChanged);

    QVERIFY(backend.connectBackend(kEndpoint));
    QVERIFY(backend.isBackendConnected());
    QVERIFY(waitForSignal(spyConn, 1, 3000));
    stepLog(QStringLiteral("test_connect_disconnect: connected ok"));

    backend.disconnectBackend();
    QVERIFY(!backend.isBackendConnected());
    stepLog(QStringLiteral("test_connect_disconnect: disconnected ok"));
}

void StageIntegrationTest::test_get_positions()
{
    stepLog(QStringLiteral("test_get_positions: connectBackend(%1)").arg(kEndpoint));
    StageReceiverBackend backend;
    QSignalSpy spyData(&backend, &StageReceiverBackend::dataReceived);

    QVERIFY(backend.connectBackend(kEndpoint));

    stepLog(QStringLiteral("test_get_positions: requestPositions()"));
    backend.requestPositions();

    QVERIFY(waitForSignal(spyData, 1, 5000));
    const QJsonObject pkt = firstJsonPacketOfType(spyData, QStringLiteral("stagePositions"));
    QVERIFY(!pkt.isEmpty());
    QVERIFY(pkt.contains(QStringLiteral("xMm")));
    QVERIFY(pkt.contains(QStringLiteral("yMm")));
    QVERIFY(pkt.contains(QStringLiteral("zMm")));
    stepLog(QStringLiteral("test_get_positions: stagePositions received (xMm/yMm/zMm)"));

    backend.disconnectBackend();
}

void StageIntegrationTest::test_move_abs_and_rel()
{
    stepLog(QStringLiteral("test_move_abs_and_rel: connectBackend(%1)").arg(kEndpoint));
    StageReceiverBackend backend;
    QSignalSpy spyData(&backend, &StageReceiverBackend::dataReceived);

    QVERIFY(backend.connectBackend(kEndpoint));

    stepLog(QStringLiteral("test_move_abs_and_rel: moveAbs x=1.0 y=2.0 z=3.0 timeoutMs=5000"));
    backend.moveAbs(1.0, 2.0, 3.0, 5000);
    QVERIFY(waitForSignal(spyData, 1, 5000));
    QJsonObject cmd = firstJsonPacketOfType(spyData, QStringLiteral("stageCommandResult"));
    QVERIFY(!cmd.isEmpty());
    QVERIFY(cmd.value(QStringLiteral("ok")).toBool());
    stepLog(QStringLiteral("test_move_abs_and_rel: moveAbs ok"));

    spyData.clear();
    stepLog(QStringLiteral("test_move_abs_and_rel: moveRel axis=0 (X), delta=0.5 timeoutMs=5000"));
    backend.moveRel(0, 0.5, 5000);
    QVERIFY(waitForSignal(spyData, 1, 5000));
    cmd = firstJsonPacketOfType(spyData, QStringLiteral("stageCommandResult"));
    QVERIFY(cmd.value(QStringLiteral("ok")).toBool());
    stepLog(QStringLiteral("test_move_abs_and_rel: moveRel ok"));

    backend.disconnectBackend();
}

void StageIntegrationTest::test_jog_and_speed()
{
    stepLog(QStringLiteral("test_jog_and_speed: connectBackend(%1)").arg(kEndpoint));
    StageReceiverBackend backend;
    QSignalSpy spyData(&backend, &StageReceiverBackend::dataReceived);

    QVERIFY(backend.connectBackend(kEndpoint));

    stepLog(QStringLiteral("test_jog_and_speed: jog axis=X plus=on enable=on"));
    backend.jog(0, true, true);
    QVERIFY(waitForSignal(spyData, 1, 5000));
    QJsonObject cmd = firstJsonPacketOfType(spyData, QStringLiteral("stageCommandResult"));
    QVERIFY(cmd.value(QStringLiteral("ok")).toBool());
    stepLog(QStringLiteral("test_jog_and_speed: jog enable ok"));

    spyData.clear();
    stepLog(QStringLiteral("test_jog_and_speed: jog axis=X plus=on enable=off"));
    backend.jog(0, true, false);
    QVERIFY(waitForSignal(spyData, 1, 5000));

    spyData.clear();
    stepLog(QStringLiteral("test_jog_and_speed: setSpeed speedPulsePerSec=20000 accelMs=100"));
    backend.setSpeed(20000, 100);
    QVERIFY(waitForSignal(spyData, 1, 5000));
    cmd = firstJsonPacketOfType(spyData, QStringLiteral("stageCommandResult"));
    QVERIFY(cmd.value(QStringLiteral("ok")).toBool());
    stepLog(QStringLiteral("test_jog_and_speed: setSpeed ok"));

    backend.disconnectBackend();
}

void StageIntegrationTest::test_scan_flow()
{
    stepLog(QStringLiteral("test_scan_flow: connectBackend(%1)").arg(kEndpoint));
    StageReceiverBackend backend;
    QSignalSpy spyData(&backend, &StageReceiverBackend::dataReceived);

    QVERIFY(backend.connectBackend(kEndpoint));

    stepLog(QStringLiteral("test_scan_flow: startScan(mode=SNAKE xs=0 xe=10 ys=0 ye=10 yStep=1 zFix=0)"));
    backend.startScan(0, 0.0, 10.0, 0.0, 10.0, 1.0, 0.0);
    QVERIFY(waitForSignal(spyData, 1, 5000));
    QJsonObject cmd = firstJsonPacketOfType(spyData, QStringLiteral("stageCommandResult"));
    QVERIFY(cmd.value(QStringLiteral("ok")).toBool());
    stepLog(QStringLiteral("test_scan_flow: startScan ok"));

    spyData.clear();
    stepLog(QStringLiteral("test_scan_flow: requestScanStatus()"));
    backend.requestScanStatus();
    QVERIFY(waitForSignal(spyData, 1, 5000));
    QJsonObject st = firstJsonPacketOfType(spyData, QStringLiteral("stageScanStatus"));
    QVERIFY(!st.isEmpty());
    QVERIFY(st.value(QStringLiteral("running")).toBool());
    stepLog(QStringLiteral("test_scan_flow: scan status running ok"));

    spyData.clear();
    stepLog(QStringLiteral("test_scan_flow: stopScan()"));
    backend.stopScan();
    QVERIFY(waitForSignal(spyData, 1, 5000));
    stepLog(QStringLiteral("test_scan_flow: stopScan ok"));

    backend.disconnectBackend();
}

QTEST_MAIN(StageIntegrationTest)
#include "stage_integration_test.moc"
