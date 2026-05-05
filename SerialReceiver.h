#ifndef SERIALRECEIVER_H
#define SERIALRECEIVER_H
// SerialReceiver.h 第一行添加
#ifdef QT_COMPILE_FOR_WASM
#define max_align_t __qt_wasm_max_align_t  // 重命名冲突类型
#endif
#include <QObject>
// 仅在非wasm环境下包含串口头文件
#ifndef QT_COMPILE_FOR_WASM
#include <QSerialPort>
#endif
#include <QByteArray>
#include <QTimer>
#include "FrameData.h"
#include "IReceiverBackend.h"

// 串口数据接收线程类（独立线程运行，避免阻塞UI）
class SerialReceiver : public IReceiverBackend
{
    Q_OBJECT
public:
    explicit SerialReceiver(QObject *parent = nullptr);
    ~SerialReceiver();

    // 测试用：对外暴露解析函数以便单元测试调用
    Q_INVOKABLE FrameData parseRawFrameForTest(const QByteArray& rawFrame);

public slots:
    // 传输无关后端接口实现
    bool connectBackend(const QString& endpoint) override;
    void disconnectBackend() override;
    bool isBackendConnected() const override;
    Q_INVOKABLE void startAcquisition(int intervalMs = 100) override;
    Q_INVOKABLE void stopAcquisition() override;
    Q_INVOKABLE void setPaused(bool paused) override;

public:
    // 串口专用兼容方法
    bool openSerial(const QString& portName, int baudRate = 115200);
    void closeSerial();
    bool isSerialOpen() const;
    Q_INVOKABLE void startMockData(int intervalMs = 100);

public slots:
    /**
     * @brief 发送原始字节指令到串口
     * @param command 要发送的字节数组
     */
    void sendCommand(const QByteArray& command) override;
    
    /**
     * @brief 发送字符串指令到串口
     * @param command 要发送的字符串
     * @param isHex 是否为十六进制格式（true: 十六进制，false: ASCII字符串）
     */
    void sendCommand(const QString& command, bool isHex = false) override;
    
    /**
     * @brief 发送十六进制指令（便捷方法）
     * @param hexCommand 十六进制字符串（如"AA55FF"）
     */
    void sendHexCommand(const QString& hexCommand);

private slots:
    void onSerialReadyRead();    // 串口数据接收槽函数
    void onMockDataTimer();      // 模拟数据定时器槽函数

private:
    void processSerialBuffer();  // 处理串口粘包/拆包
    FrameData parseRawData(const QByteArray& rawFrame); // 解析原始数据为结构体
    
    /**
     * @brief 将十六进制字符串转换为字节数组
     * @param hex 十六进制字符串
     * @return 转换后的字节数组
     */
    QByteArray hexStringToByteArray(const QString& hex);

private:
    #ifndef QT_COMPILE_FOR_WASM
        QSerialPort* m_serialPort;
    #endif
    QByteArray m_serialBuffer;   // 串口缓存（处理粘包）
    QTimer* m_mockTimer;         // 模拟数据定时器
    bool m_paused = false;       // 采集暂停标记

    // 帧协议配置（根据硬件修改）
    const int FRAME_LENGTH = 22;                   // 固定帧长度（含 AA55 + uint64 frameId + 3x float 传感器占位字段）
    quint64 m_mockFrameSeq {0};                    // 串口模拟数据：单调递增序号（与 FrameData::sequence/frameId 对齐）
    const QByteArray FRAME_HEAD = QByteArray::fromHex("AA55"); // 帧头
};

#endif // SERIALRECEIVER_H
