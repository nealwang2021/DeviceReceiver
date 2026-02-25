#ifndef PLOTWINDOWMANAGER_H
#define PLOTWINDOWMANAGER_H

#include <QObject>
#include <QVector>
#include <QTimer>
#include <QList>
#include "FrameData.h"

// 前置声明
class PlotWindow;
class QMdiArea;

/**
 * @brief 绘图窗口管理器（单例模式）
 * 
 * 负责集中管理所有PlotWindow实例，统一从DataCacheManager获取数据并分发给各个窗口
 * 实现多个不同展示形式的窗口复用相同数据源
 */
class PlotWindowManager : public QObject
{
    Q_OBJECT
public:
    // 绘图窗口类型枚举
    enum PlotType {
        CombinedPlot,     // 组合图（温湿度）
        TemperaturePlot,  // 温度图
        HumidityPlot,     // 湿度图
        VoltagePlot,      // 电压图
        HistoryPlot,      // 历史数据图
        HeatmapPlot,      // 热力图
        ArrayPlot         // 阵列图（多通道分轴显示）
    };

    /**
     * @brief 获取全局唯一实例
     * @return PlotWindowManager指针
     */
    static PlotWindowManager* instance();

    /**
     * @brief 销毁单例实例
     */
    static void destroy();

    /**
     * @brief 创建指定类型的绘图窗口
     * @param type 窗口类型
     * @param parent 父窗口指针
     * @return 创建的PlotWindow指针
     */
    PlotWindow* createWindow(PlotType type = CombinedPlot, QWidget* parent = nullptr);
    
    /**
     * @brief 在MDI区域中创建窗口
     * @param mdiArea MDI区域
     * @param type 窗口类型
     * @return 创建的PlotWindow指针
     */
    PlotWindow* createWindowInMdiArea(QMdiArea* mdiArea, PlotType type = CombinedPlot);

    /**
     * @brief 注册绘图窗口（用于手动创建的窗口）
     * @param window 要注册的窗口指针
     */
    void registerWindow(PlotWindow* window);

    /**
     * @brief 注销绘图窗口
     * @param window 要注销的窗口指针
     */
    void unregisterWindow(PlotWindow* window);

    /**
     * @brief 获取所有已注册的窗口
     * @return 窗口列表
     */
    QList<PlotWindow*> windows() const { return m_windows; }

    /**
     * @brief 获取当前管理的窗口数量
     * @return 窗口数量
     */
    int windowCount() const { return m_windows.size(); }

    /**
     * @brief 更新所有窗口的数据
     */
    void updateAllWindows();

    /**
     * @brief 设置数据更新频率
     * @param intervalMs 更新间隔（毫秒）
     */
    void setUpdateInterval(int intervalMs);

    /**
     * @brief 获取当前更新间隔
     * @return 更新间隔（毫秒）
     */
    int updateInterval() const { return m_updateTimer->interval(); }

    /**
     * @brief 开始数据更新
     */
    void startUpdates();

    /**
     * @brief 停止数据更新
     */
    void stopUpdates();

    /**
     * @brief 获取最近的数据帧（用于分发）
     * @param count 要获取的帧数
     * @return 数据帧列表
     */
    QVector<FrameData> getRecentFrames(int count = 5);

signals:
    /**
     * @brief 数据更新信号（发送给所有注册的窗口）
     * @param frames 最新的数据帧
     */
    void dataUpdated(const QVector<FrameData>& frames);

    /**
     * @brief 窗口被添加信号
     * @param window 被添加的窗口
     */
    void windowAdded(PlotWindow* window);

    /**
     * @brief 窗口被移除信号
     * @param window 被移除的窗口
     */
    void windowRemoved(PlotWindow* window);

    /**
     * @brief 关键报警帧信号
     * @param frame 报警帧数据
     */
    void criticalFrameReceived(const FrameData& frame);

private:
    explicit PlotWindowManager(QObject *parent = nullptr);
    ~PlotWindowManager();

    // 禁用拷贝和赋值
    PlotWindowManager(const PlotWindowManager&) = delete;
    PlotWindowManager& operator=(const PlotWindowManager&) = delete;

    /**
     * @brief 初始化管理器
     */
    void initialize();

    /**
     * @brief 清理资源
     */
    void cleanup();

private slots:
    /**
     * @brief 定时器槽函数，定时获取数据并分发
     */
    void onUpdateTimer();

private:
    static PlotWindowManager* m_instance;  // 单例实例
    QList<PlotWindow*> m_windows;          // 管理的窗口列表
    QTimer* m_updateTimer;                 // 数据更新定时器
    bool m_isInitialized;                  // 初始化标志
};

#endif // PLOTWINDOWMANAGER_H