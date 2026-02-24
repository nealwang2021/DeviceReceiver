#ifndef APPLICATIONCONTROLLER_H
#define APPLICATIONCONTROLLER_H

#include <QObject>
#include <QScopedPointer>

// 前置声明，减少头文件依赖
class DataCacheManager;
class SerialReceiver;
class PlotWindow;
class DataProcessor;
class AppConfig;
class PlotWindowManager;
class QThread;

/**
 * @brief 应用控制器类，负责协调所有模块的初始化和生命周期管理
 * 
 * 重构目标：将main函数中的职责迁移到此控制器中，实现单一职责原则
 */
class ApplicationController : public QObject
{
    Q_OBJECT
public:
    // PlotWindowManager::PlotType的前向声明
    enum PlotType {
        CombinedPlot,
        TemperaturePlot,
        HumidityPlot,
        VoltagePlot,
        HistoryPlot
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
    
    /**
     * @brief 获取绘图窗口指针（用于向后兼容）
     * @return PlotWindow指针，如果未初始化则返回nullptr
     */
    PlotWindow* plotWindow() const;
    
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
    PlotWindow* createPlotWindow(PlotType type = CombinedPlot);

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
    bool initSerialReceiver();
    
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
     * @brief 清理所有资源
     */
    void cleanup();

private:
    bool m_isRunning = false;
    
    // 核心模块实例
    DataCacheManager* m_cacheManager = nullptr;
    QScopedPointer<QThread> m_serialThread;
    QScopedPointer<SerialReceiver> m_serialReceiver;
    QScopedPointer<PlotWindow> m_plotWindow;  // 向后兼容的默认窗口
    QScopedPointer<DataProcessor> m_dataProcessor;
    PlotWindowManager* m_plotWindowManager = nullptr;  // 绘图窗口管理器
    
    // 配置参数（后续可迁移到AppConfig类）
    struct {
        int maxCacheSize = 600;          // 最大缓存帧数
        qint64 expireTimeMs = 60000;     // 数据过期时间（毫秒）
        QString serialPort = "COM3";     // 串口端口
        int baudRate = 115200;           // 波特率
        bool useMockData = true;         // 是否使用模拟数据
        int mockDataIntervalMs = 100;    // 模拟数据间隔（毫秒）
        int initialWindowCount = 1;      // 初始窗口数量
        PlotType initialWindowType = CombinedPlot; // 初始窗口类型
    } m_config;
};

#endif // APPLICATIONCONTROLLER_H
