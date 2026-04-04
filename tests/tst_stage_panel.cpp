/**
 * Qt Test：三轴台面关键路径（连接 + 读一次位置）
 * 依赖 tests/fixtures/config.ini（被测设备 gRPC Mock）与 stage_grpc_test_server.py（三轴台）。
 */
#include <QtTest/QtTest>
#include <QApplication>
#include <QFile>
#include <QElapsedTimer>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QThread>
#include <QWidget>
#include <QFile>
#include <QTextStream>

#include "ApplicationController.h"
#include "AppConfig.h"
#include "MainWindow.h"
#include "stage_test_paths.h"

namespace {

constexpr int kStagePort = 50053;

} // namespace

class StagePanelTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void test_connect_and_read_position();

private:
    QProcess* m_stageServer = nullptr;
};

namespace {

QString panelStepLogPath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("stage_test_panel_steps_%1.log").arg(kStagePort));
}

void panelStepLog(const QString& line)
{
    QFile f(panelStepLogPath());
    if (!f.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << " " << line << "\n";
    f.close();
}

} // namespace

void StagePanelTest::initTestCase()
{
    panelStepLog(QStringLiteral("initTestCase: starting stage_grpc_test_server.py on port=%1").arg(kStagePort));
    m_stageServer = new QProcess;
    m_stageServer->setProcessChannelMode(QProcess::MergedChannels);
    QString python = QStringLiteral("python");
    m_stageServer->start(python,
                           QStringList() << QString::fromUtf8(STAGE_TEST_SERVER_SCRIPT)
                                         << QStringLiteral("--port") << QString::number(kStagePort));
    QVERIFY2(m_stageServer->waitForStarted(8000), "Failed to start stage_grpc_test_server.py");
    QThread::msleep(2500);
    QVERIFY2(m_stageServer->state() == QProcess::Running, "stage server exited");
    panelStepLog(QStringLiteral("initTestCase: stage server running"));
}

void StagePanelTest::cleanupTestCase()
{
    panelStepLog(QStringLiteral("cleanupTestCase: stopping stage server"));
    if (!m_stageServer) {
        return;
    }
    if (m_stageServer->state() == QProcess::Running) {
        m_stageServer->terminate();
        if (!m_stageServer->waitForFinished(4000)) {
            m_stageServer->kill();
            m_stageServer->waitForFinished(1000);
        }
    }
    delete m_stageServer;
    m_stageServer = nullptr;
}

void StagePanelTest::test_connect_and_read_position()
{
    const QString fixtureIni = QString::fromUtf8(STAGE_TEST_FIXTURES_DIR) + QStringLiteral("/config.ini");
    QVERIFY2(QFile::exists(fixtureIni), qPrintable(QStringLiteral("Missing fixture: %1").arg(fixtureIni)));
    panelStepLog(QStringLiteral("test_connect_and_read_position: fixtureIni=%1").arg(fixtureIni));

    AppConfig::instance()->loadFromFile(fixtureIni);
    {
        const bool useMock = AppConfig::instance()->useMockData();
        const QString backendType = AppConfig::instance()->receiverBackendType();
        panelStepLog(QStringLiteral("test_connect_and_read_position: AppConfig loaded, backendType=%1, useMockData=%2")
                     .arg(backendType, useMock ? QStringLiteral("true") : QStringLiteral("false")));
    }

    ApplicationController controller;
    QVERIFY2(controller.initialize(), "ApplicationController::initialize failed");
    panelStepLog(QStringLiteral("test_connect_and_read_position: controller initialized"));
    // 注意：controller.start() 可能触发被测设备 gRPC 连接失败时弹出 QMessageBox 并等待人工。
    // 本 UI 用例聚焦三轴台面板的可操作性/联调，因此跳过 start，避免阻塞自动化。
    panelStepLog(QStringLiteral("test_connect_and_read_position: skip controller.start() to avoid QMessageBox blocking"));

    MainWindow* mw = nullptr;
    QElapsedTimer exposeWait;
    exposeWait.start();
    while (exposeWait.elapsed() < 15000 && !mw) {
        const QWidgetList tl = QApplication::topLevelWidgets();
        for (QWidget* w : tl) {
            if (w->objectName() == QLatin1String("mainWindow")) {
                mw = qobject_cast<MainWindow*>(w);
                break;
            }
        }
        if (mw) {
            break;
        }
        QTest::qWait(100);
    }
    QVERIFY2(mw != nullptr, "MainWindow not found");
    panelStepLog(QStringLiteral("test_connect_and_read_position: MainWindow found"));

    mw->show();
    QTest::qWait(300);
    QVERIFY(QTest::qWaitForWindowExposed(mw, 5000));
    panelStepLog(QStringLiteral("test_connect_and_read_position: MainWindow exposed"));

    auto* ep = mw->findChild<QLineEdit*>(QStringLiteral("stage_endpointEdit"));
    auto* connectBtn = mw->findChild<QPushButton*>(QStringLiteral("stage_connectButton"));
    auto* statusLbl = mw->findChild<QLabel*>(QStringLiteral("stage_backendStatusLabel"));
    auto* getPosBtn = mw->findChild<QPushButton*>(QStringLiteral("stage_getPositionButton"));
    auto* posLbl = mw->findChild<QLabel*>(QStringLiteral("stage_positionLabel"));
    QVERIFY(ep && connectBtn && statusLbl && getPosBtn && posLbl);
    panelStepLog(QStringLiteral("test_connect_and_read_position: stage controls found"));

    ep->setText(QStringLiteral("127.0.0.1:%1").arg(kStagePort));
    panelStepLog(QStringLiteral("test_connect_and_read_position: set endpoint text"));
    QTest::mouseClick(connectBtn, Qt::LeftButton);
    panelStepLog(QStringLiteral("test_connect_and_read_position: clicked stage_connectButton"));

    QElapsedTimer t;
    t.start();
    bool okStatus = false;
    while (t.elapsed() < 12000) {
        const QString s = statusLbl->text();
        // 只在关键节点记录最终文本，避免刷屏
        if (s.contains(QStringLiteral("已连接")) || s.contains(QLatin1String("connected"), Qt::CaseInsensitive)) {
            okStatus = true;
            break;
        }
        QTest::qWait(150);
    }
    QVERIFY2(okStatus, qPrintable(QStringLiteral("Stage did not connect: %1").arg(statusLbl->text())));
    panelStepLog(QStringLiteral("test_connect_and_read_position: connected ok, status=%1").arg(statusLbl->text()));

    QTest::mouseClick(getPosBtn, Qt::LeftButton);
    panelStepLog(QStringLiteral("test_connect_and_read_position: clicked stage_getPositionButton"));
    QTest::qWait(2000);

    const QString posText = posLbl->text();
    QVERIFY2(posText.contains(QLatin1String("X:")), qPrintable(posText));
    QVERIFY2(!posText.contains(QStringLiteral("X: --")), qPrintable(posText));
    panelStepLog(QStringLiteral("test_connect_and_read_position: got position, posLabel=%1").arg(posText));

    controller.disconnectStageBackend();
    panelStepLog(QStringLiteral("test_connect_and_read_position: stage backend disconnected"));
    controller.stop();
    panelStepLog(QStringLiteral("test_connect_and_read_position: controller stopped"));
}

QTEST_MAIN(StagePanelTest)
#include "tst_stage_panel.moc"
