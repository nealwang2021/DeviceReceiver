#include "SerialReceiver.h"
#include "DataCacheManager.h"
#include <QDateTime>
#include <QDebug>
#include <QRandomGenerator>
#include <QTimer>
#include <QRegularExpression>
#include <QDataStream>
#include <cstring>

SerialReceiver::SerialReceiver(QObject *parent) : QObject(parent)
{
    m_serialPort = new QSerialPort(this);
    m_mockTimer = new QTimer(this);

    // 连接串口信号
    connect(m_serialPort, &QSerialPort::readyRead, this, &SerialReceiver::onSerialReadyRead);
    // 连接模拟数据定时器
    connect(m_mockTimer, &QTimer::timeout, this, &SerialReceiver::onMockDataTimer);
}

SerialReceiver::~SerialReceiver()
{
    closeSerial();
    m_mockTimer->stop();
}

bool SerialReceiver::openSerial(const QString& portName, int baudRate)
{
    m_serialPort->setPortName(portName);
    m_serialPort->setBaudRate(baudRate);
    m_serialPort->setDataBits(QSerialPort::Data8);
    m_serialPort->setParity(QSerialPort::NoParity);
    m_serialPort->setStopBits(QSerialPort::OneStop);
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (m_serialPort->open(QIODevice::ReadOnly)) {
        qInfo() << QString("串口[%1]打开成功，波特率：%2").arg(portName).arg(baudRate);
        m_serialBuffer.clear();
        return true;
    } else {
        QString err = QString("串口[%1]打开失败：%2").arg(portName).arg(m_serialPort->errorString());
        qCritical() << err;
        emit commandError(err);
        return false;
    }
}

void SerialReceiver::closeSerial()
{
    if (m_serialPort->isOpen()) {
        m_serialPort->close();
        qInfo() << "串口已关闭";
    }
}

bool SerialReceiver::isSerialOpen() const
{
    return m_serialPort->isOpen();
}

void SerialReceiver::startMockData(int intervalMs)
{
    qInfo() << QString("开启模拟数据，间隔：%1ms").arg(intervalMs);
    m_mockTimer->setInterval(intervalMs);
    m_mockTimer->start();
}

void SerialReceiver::onSerialReadyRead()
{
    m_serialBuffer.append(m_serialPort->readAll());
    processSerialBuffer();
}

void SerialReceiver::onMockDataTimer()
{
    // 生成模拟帧数据（无硬件时测试用）
    FrameData frame;
    frame.timestamp = QDateTime::currentMSecsSinceEpoch();
    frame.frameId = QRandomGenerator::global()->bounded(10000);
    frame.temperature = 20 + QRandomGenerator::global()->bounded(60.0); // 20-80℃
    frame.humidity = 30 + QRandomGenerator::global()->bounded(50.0);   // 30-80%
    frame.voltage = 3.0 + QRandomGenerator::global()->bounded(1.5);    // 3.0-4.5V
    frame.isAlarm = (frame.temperature > 75); // 模拟报警

    // 写入缓存
    DataCacheManager::instance()->addFrame(frame);

    qDebug() << QString("模拟帧[%1]：温度=%2℃ 湿度=%3% 电压=%4V 报警=%5")
                .arg(frame.frameId)
                .arg(frame.temperature, 0, 'f', 1)
                .arg(frame.humidity, 0, 'f', 1)
                .arg(frame.voltage, 0, 'f', 1)
                .arg(frame.isAlarm ? "是" : "否");
}

void SerialReceiver::processSerialBuffer()
{
    while (m_serialBuffer.size() >= FRAME_LENGTH) {
        int headIndex = m_serialBuffer.indexOf(FRAME_HEAD);
        if (headIndex == -1) {
            m_serialBuffer.clear();
            qWarning() << "无有效帧头，清空缓存";
            break;
        }

        if (m_serialBuffer.size() < headIndex + FRAME_LENGTH) {
            break; // 数据不足，等待下一次
        }

        // 提取完整帧
        QByteArray rawFrame = m_serialBuffer.mid(headIndex, FRAME_LENGTH);
        m_serialBuffer.remove(0, headIndex + FRAME_LENGTH);

        // 解析并写入缓存
        FrameData frame = parseRawData(rawFrame);
        frame.timestamp = QDateTime::currentMSecsSinceEpoch();
        DataCacheManager::instance()->addFrame(frame);

        qDebug() << QString("解析帧[%1]：温度=%2℃ 湿度=%3% 电压=%4V 报警=%5")
                    .arg(frame.frameId)
                    .arg(frame.temperature, 0, 'f', 1)
                    .arg(frame.humidity, 0, 'f', 1)
                    .arg(frame.voltage, 0, 'f', 1)
                    .arg(frame.isAlarm ? "是" : "否");
    }
}

FrameData SerialReceiver::parseRawData(const QByteArray& rawFrame)
{
    FrameData frame;
    // 使用安全解析，假定帧格式（小端）：
    // [0-1] head(2) | [2-3] frameId(u16) | [4-7] temperature(float) | [8-11] humidity(float) | [12-15] voltage(float)
    if (rawFrame.size() != FRAME_LENGTH) {
        qWarning() << "解析错误：帧长度不匹配，期待" << FRAME_LENGTH << "实际" << rawFrame.size();
        return frame;
    }

    if (!rawFrame.startsWith(FRAME_HEAD)) {
        qWarning() << "解析错误：帧头不匹配" << rawFrame.toHex();
        return frame;
    }

    QDataStream ds(rawFrame);
    ds.setByteOrder(QDataStream::LittleEndian);

    quint16 head = 0;
    ds >> head; // 读取并丢弃帧头

    quint16 frameId = 0;
    float temperature = 0.0f;
    float humidity = 0.0f;
    float voltage = 0.0f;

    ds >> frameId;
    ds >> temperature;
    ds >> humidity;
    ds >> voltage;

    frame.frameId = static_cast<uint32_t>(frameId);
    frame.temperature = temperature;
    frame.humidity = humidity;
    frame.voltage = voltage;

    // 报警：若协议在最后字节具有标志位则使用该位，否则依据温度阈值进行判断
    bool flagAlarm = false;
    if (rawFrame.size() >= 16) {
        quint8 lastByte = static_cast<quint8>(rawFrame.at(15));
        flagAlarm = (lastByte & 0x01) == 0x01;
    }

    // 温度阈值使用默认值（如需从配置读取，可在此处扩展）
    const float temperatureThreshold = 80.0f;
    frame.isAlarm = flagAlarm || (frame.temperature > temperatureThreshold);

    return frame;
}

FrameData SerialReceiver::parseRawFrameForTest(const QByteArray& rawFrame)
{
    return parseRawData(rawFrame);
}

void SerialReceiver::sendCommand(const QByteArray& command)
{
    if (!m_serialPort->isOpen()) {
        emit commandError("串口未打开，无法发送指令");
        qWarning() << "尝试发送指令时串口未打开";
        return;
    }
    
    if (command.isEmpty()) {
        emit commandError("指令不能为空");
        qWarning() << "尝试发送空指令";
        return;
    }
    
    qint64 bytesWritten = m_serialPort->write(command);
    if (bytesWritten == -1) {
        QString error = QString("指令发送失败: %1").arg(m_serialPort->errorString());
        emit commandError(error);
        qCritical() << error;
    } else {
        if (bytesWritten != command.size()) {
            qWarning() << QString("指令发送不完全: 期望%1字节，实际%2字节")
                          .arg(command.size()).arg(bytesWritten);
        }
        emit commandSent(command);
        qInfo() << QString("指令发送成功: %1字节").arg(bytesWritten);
    }
}

void SerialReceiver::sendCommand(const QString& command, bool isHex)
{
    if (command.isEmpty()) {
        emit commandError("指令不能为空");
        qWarning() << "尝试发送空指令";
        return;
    }
    
    QByteArray byteArray;
    if (isHex) {
        byteArray = hexStringToByteArray(command);
        if (byteArray.isEmpty()) {
            emit commandError("十六进制格式无效");
            qWarning() << "无效的十六进制格式:" << command;
            return;
        }
    } else {
        byteArray = command.toUtf8();
    }
    
    sendCommand(byteArray);
}

void SerialReceiver::sendHexCommand(const QString& hexCommand)
{
    sendCommand(hexCommand, true);
}

QByteArray SerialReceiver::hexStringToByteArray(const QString& hex)
{
    QByteArray byteArray;
    QString cleanedHex = hex.trimmed();
    
    // 移除可能的前缀和后缀
    cleanedHex.remove(QRegularExpression("^0x|^0X"));
    cleanedHex.remove(QRegularExpression("[^0-9A-Fa-f]"));
    
    if (cleanedHex.isEmpty() || cleanedHex.length() % 2 != 0) {
        qWarning() << "无效的十六进制字符串:" << hex;
        return QByteArray();
    }
    
    bool ok;
    for (int i = 0; i < cleanedHex.length(); i += 2) {
        QString byteString = cleanedHex.mid(i, 2);
        byteArray.append(static_cast<char>(byteString.toInt(&ok, 16)));
        if (!ok) {
            qWarning() << "无效的十六进制字节:" << byteString;
            return QByteArray();
        }
    }
    
    return byteArray;
}
