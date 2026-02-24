#ifndef SERIALRECEIVER_H
#define SERIALRECEIVER_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>
#include "FrameData.h"

// 串口数据接收线程类（独立线程运行，避免阻塞UI）
class SerialReceiver : public QObject
{
    Q_OBJECT
public:
    explicit SerialReceiver(QObject *parent = nullptr);
    ~SerialReceiver();

    // 测试用：对外暴露解析函数以便单元测试调用
    Q_INVOKABLE FrameData parseRawFrameForTest(const QByteArray& rawFrame);

    // 串口操作接口
    bool openSerial(const QString& portName, int baudRate = 115200);
    void closeSerial();
    bool isSerialOpen() const;

    // 【测试用】开启模拟数据（无硬件时使用）
    Q_INVOKABLE void startMockData(int intervalMs = 100);

public slots:
    /**
     * @brief 发送原始字节指令到串口
     * @param command 要发送的字节数组
     */
    void sendCommand(const QByteArray& command);
    
    /**
     * @brief 发送字符串指令到串口
     * @param command 要发送的字符串
     * @param isHex 是否为十六进制格式（true: 十六进制，false: ASCII字符串）
     */
    void sendCommand(const QString& command, bool isHex = false);
    
    /**
     * @brief 发送十六进制指令（便捷方法）
     * @param hexCommand 十六进制字符串（如"AA55FF"）
     */
    void sendHexCommand(const QString& hexCommand);

signals:
    /**
     * @brief 指令发送成功信号
     * @param command 发送的指令内容
     */
    void commandSent(const QByteArray& command);
    
    /**
     * @brief 指令发送错误信号
     * @param error 错误信息
     */
    void commandError(const QString& error);
    
    /**
     * @brief 接收到数据信号（用于UI显示）
     * @param data 接收到的数据
     * @param isHex 是否以十六进制显示
     */
    void dataReceived(const QByteArray& data, bool isHex = false);

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
    QSerialPort* m_serialPort;
    QByteArray m_serialBuffer;   // 串口缓存（处理粘包）
    QTimer* m_mockTimer;         // 模拟数据定时器

    // 帧协议配置（根据硬件修改）
    const int FRAME_LENGTH = 16;                     // 固定帧长度
    const QByteArray FRAME_HEAD = QByteArray::fromHex("AA55"); // 帧头
};

#endif // SERIALRECEIVER_H
