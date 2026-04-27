#ifndef APPLICATIONCONTROLLER_H
#define APPLICATIONCONTROLLER_H

#include <QObject>
#include <QScopedPointer>
#include "StagePoseLatch.h"

// 前置声明，减少头文件依赖
class DataCacheManager;
class IReceiverBackend;
class PlotWindowBase;
class DataProcessor;
class AppConfig;
class PlotWindowManager;
class MainWindow;
class QThread;
class RealtimeSqlRecorder;
/**
 * @brief 应用控制器类，负责协调所有模块的初始化和生命周期管理
 * 
 * 重构目标：将main函数中的职责迁移到此控制器中，实现单一职责原则
 */
class ApplicationController : public QObject
{
    Q_OBJECT
public:
    /// 与 PlotWindowManager::PlotType 枚举顺序一致（勿打乱，createPlotWindow 内 static_cast 依赖序值）
    enum PlotType {
        CombinedPlot,
        HeatmapPlot,
        ArrayPlot,
        PulsedDecayPlot,
        InspectionPlot,
        ArrayHeatmapPlot = 9
    };

    /**
     * @brief 构造函数
     * @param parent 父对象指针
     */
    explicit ApplicationController(QObject *parent = nullptr);
    
    /**
     * @brief 析构函数，确保资源正确释放
     */
    ~ApplicationController() override;

    /**
     * @brief 初始化所有应用模块
     * @return 初始化是否成功
     */
    bool initialize();

    /**
     * @brief 启动应用（显示窗口，开始数据接收）
     */
    void start();
    
    /**
     * @brief 停止应用（停止数据接收，准备退出）
     */
    void stop();
    void pauseAcquisition();
    void resumeAcquisition();
    bool isPaused() const { return m_isPaused; }
    
    /**
     * @brief 获取绘图窗口指针（用于向后兼容）
     * @return 默认绘图窗口指针，如果未初始化则返回 nullptr
     */
    PlotWindowBase* plotWindow() const;
    
    /**
     * @brief 获取PlotWindowManager实例
     * @return PlotWindowManager指针
     */
    PlotWindowManager* plotWindowManager() const;
    
    /**
     * @brief 检查应用是否正在运行
     * @return 运行状态
     */
    bool isRunning() const { return m_isRunning; }

    /**
     * @brief 创建新的绘图窗口
     * @param type 窗口类型
     * @return 创建的窗口指针
     */
    PlotWindowBase* createPlotWindow(PlotType type = CombinedPlot);
    
    /**
     * @brief 将指令转发到串口模块（线程安全，使用QueuedConnection）
     */
    void sendCommand(const QString& command, bool isHex = false);
    void reloadRuntimeConfig();
    /**
     * @brief 从 AppConfig 同步接收后端类型；若与当前实现不一致则停止采集并重建后端（须先 saveConfigFromUI）
     */
    void applyReceiverBackendFromConfig();

    /// 三轴台测试装置（StageService）独立后端，与主采集（被测设备）无关
    bool connectStageBackend(const QString& endpoint);
    void disconnectStageBackend();
    void sendStageCommand(const QString& command, bool isHex = false);
    bool isStageConnected();

signals:
    /**
     * @brief 应用启动信号
     * @param success 启动是否成功
     */
    void started(bool success);
    
    /**
     * @brief 应用停止信号
     */
    void stopped();
    void paused(bool paused);
    /// 主采集连接进行中状态（用于 UI 显示“连接中”并防重入）
    void connectionInProgressChanged(bool inProgress);
    /// 三轴台 gRPC 连接状态（与主窗口 m_isConnected 无关）
    void stageConnectionStateChanged(bool connected);

private:
    /**
     * @brief 初始化数据缓存管理器
     * @return 初始化是否成功
     */
    bool initCacheManager();
    
    /**
     * @brief 初始化串口接收模块
     * @return 初始化是否成功
     */
    bool initReceiverBackend();
    
    /**
     * @brief 初始化数据处理模块
     * @return 初始化是否成功
     */
    bool initDataProcessor();
    
    /**
     * @brief 初始化绘图窗口管理器
     * @return 初始化是否成功
     */
    bool initPlotWindowManager();
    
    /**
     * @brief 初始化默认绘图窗口（向后兼容）
     * @return 初始化是否成功
     */
    bool initDefaultPlotWindow();
    
    /**
     * @brief 初始化主界面窗口
     * @return 初始化是否成功
     */
    bool initMainWindow();
    void connectReceiverToMainWindow();
    void startGrpcBackendConnectAsync(const QString& endpoint);
    void handleGrpcConnectAttemptFinished(bool connected, const QString& detail);
    void connectStageReceiverToMainWindow();
    void disconnectStageReceiverFromMainWindow();
    
    /**
     * @brief 清理所有资源
     */
    void cleanup();

private:
    bool m_isRunning = false;
    bool m_isPaused = false;
    bool m_connectInProgress = false;
    
    // 核心模块实例
    DataCacheManager* m_cacheManager = nullptr;
    QScopedPointer<QThread> m_serialThread;
    QScopedPointer<IReceiverBackend> m_serialReceiver;
    QScopedPointer<QThread> m_stageThread;
    QScopedPointer<IReceiverBackend> m_stageReceiver;
    QScopedPointer<PlotWindowBase> m_plotWindow;  // 向后兼容的默认窗口
    QScopedPointer<DataProcessor> m_dataProcessor;
    PlotWindowManager* m_plotWindowManager = nullptr;  // 绘图窗口管理器
    QScopedPointer<MainWindow> m_mainWindow;  // 主界面窗口
    QString m_activeBackendType = "serial";
    StagePoseLatch m_stagePoseLatch;
    QScopedPointer<RealtimeSqlRecorder> m_realtimeRecorder;

    // 配置参数（后续可迁移到AppConfig类）
    struct {
        int maxCacheSize = 600;          // 最大缓存帧数
        qint64 expireTimeMs = 60000;     // 数据过期时间（毫秒）
        QString serialPort = "COM3";     // 串口端口
        int baudRate = 115200;           // 波特率
        QString backendType = "serial"; // serial/grpc（被测设备；不含 stage）
        QString grpcEndpoint = "127.0.0.1:50051";
        bool useMockData = true;         // 是否使用模拟数据
        int mockDataIntervalMs = 100;    // 模拟/gRPC Subscribe 帧间隔（毫秒，勿用 1000 除非刻意 1Hz）
        int initialWindowCount = 1;      // 初始窗口数量
        PlotType initialWindowType = CombinedPlot; // 初始窗口类型
        QString defaultExportDirectory = "exports";
        QString defaultExportFormat = "hdf5";
    } m_config;
};

#endif // APPLICATIONCONTROLLER_H
