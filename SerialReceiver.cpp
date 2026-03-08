#include "SerialReceiver.h"
#include <QDateTime>
#include <QDebug>
#include <QRandomGenerator>
#include <QTimer>
#include <QRegularExpression>
#include <QDataStream>
#include <cstring>
#include <cmath>

SerialReceiver::SerialReceiver(QObject *parent) : IReceiverBackend(parent)
{
#ifndef QT_COMPILE_FOR_WASM
    m_serialPort = new QSerialPort(this);
#endif
    m_mockTimer = new QTimer(this);

#ifndef QT_COMPILE_FOR_WASM
    // 连接串口信号
    connect(m_serialPort, &QSerialPort::readyRead, this, &SerialReceiver::onSerialReadyRead);
#endif
    // 连接模拟数据定时器
    connect(m_mockTimer, &QTimer::timeout, this, &SerialReceiver::onMockDataTimer);
}

SerialReceiver::~SerialReceiver()
{
    disconnectBackend();
    m_mockTimer->stop();
}

bool SerialReceiver::connectBackend(const QString& endpoint)
{
#ifdef QT_COMPILE_FOR_WASM
    Q_UNUSED(endpoint)
    return true;
#else
    QStringList endpointParts = endpoint.split("|", Qt::SkipEmptyParts);
    QString portName = endpointParts.isEmpty() ? endpoint : endpointParts.first();
    int baudRate = 115200;
    if (endpointParts.size() > 1) {
        bool ok = false;
        int parsed = endpointParts.at(1).toInt(&ok);
        if (ok && parsed > 0) {
            baudRate = parsed;
        }
    }
    return openSerial(portName, baudRate);
#endif
}

void SerialReceiver::disconnectBackend()
{
    closeSerial();
}

bool SerialReceiver::isBackendConnected() const
{
    return isSerialOpen();
}

void SerialReceiver::startAcquisition(int intervalMs)
{
    startMockData(intervalMs);
}

void SerialReceiver::stopAcquisition()
{
    m_mockTimer->stop();
}

bool SerialReceiver::openSerial(const QString& portName, int baudRate)
{
#ifndef QT_COMPILE_FOR_WASM
    m_mockTimer->stop();
    m_paused = false;
    m_serialPort->setPortName(portName);
    m_serialPort->setBaudRate(baudRate);
    m_serialPort->setDataBits(QSerialPort::Data8);
    m_serialPort->setParity(QSerialPort::NoParity);
    m_serialPort->setStopBits(QSerialPort::OneStop);
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (m_serialPort->open(QIODevice::ReadWrite)) {
        qInfo() << QString("串口[%1]打开成功，波特率：%2").arg(portName).arg(baudRate);
        m_serialBuffer.clear();
        return true;
    } else {
        QString err = QString("串口[%1]打开失败：%2").arg(portName).arg(m_serialPort->errorString());
        qCritical() << err;
        emit commandError(err);
        return false;
    }
#else
    qInfo() << QString("WebAssembly环境：模拟打开串口[%1]，波特率：%2").arg(portName).arg(baudRate);
    return true;
#endif
}

void SerialReceiver::closeSerial()
{
    stopAcquisition();
#ifndef QT_COMPILE_FOR_WASM
    if (m_serialPort->isOpen()) {
        m_serialPort->close();
        qInfo() << "串口已关闭";
    }
#else
    qInfo() << "WebAssembly环境：模拟关闭串口";
#endif
}

bool SerialReceiver::isSerialOpen() const
{
#ifndef QT_COMPILE_FOR_WASM
    return m_serialPort->isOpen();
#else
    // WebAssembly环境下总是返回false，因为没有真实的串口连接
    return false;
#endif
}

void SerialReceiver::startMockData(int intervalMs)
{
    m_paused = false;
    qInfo() << QString("开启模拟数据，间隔：%1ms").arg(intervalMs);
    m_mockTimer->setInterval(intervalMs);
    m_mockTimer->start();
}

void SerialReceiver::setPaused(bool paused)
{
    m_paused = paused;
    if (m_paused) {
        stopAcquisition();
        m_serialBuffer.clear();
    }
}

void SerialReceiver::onSerialReadyRead()
{
#ifndef QT_COMPILE_FOR_WASM
    QByteArray chunk = m_serialPort->readAll();
    emit dataReceived(chunk, true);
    if (m_paused) {
        return;
    }
    m_serialBuffer.append(chunk);
    processSerialBuffer();
#else
    // WebAssembly环境下无真实串口数据
    qDebug() << "WebAssembly环境：模拟串口数据接收";
#endif
}

void SerialReceiver::onMockDataTimer()
{
    if (m_paused) {
        return;
    }

    // 生成模拟帧数据（无硬件时测试用）
    FrameData frame;
    frame.timestamp = QDateTime::currentMSecsSinceEpoch();
    frame.frameId = QRandomGenerator::global()->bounded(10000);

    // 模拟单个多通道信号（用于开发），周期性切换实数/复数
    const int MOCK_CHANNEL_COUNT = 8;
    static bool makeComplex = false;
    frame.channelCount = static_cast<uint8_t>(MOCK_CHANNEL_COUNT);
    double t = frame.timestamp / 1000.0;
    if (!makeComplex) {
        frame.detectMode = FrameData::MultiChannelReal;
        frame.channels_comp0.resize(MOCK_CHANNEL_COUNT);
        frame.channels_comp1.clear();
        for (int i = 0; i < MOCK_CHANNEL_COUNT; ++i) {
            double phase = (i * 0.5);
            double value = 10.0 + 3.0 * i + 5.0 * std::sin(t * (0.5 + i * 0.04) + phase) + (QRandomGenerator::global()->bounded(100) / 100.0 - 0.5);
            frame.channels_comp0[i] = value;
        }
    } else {
        frame.detectMode = FrameData::MultiChannelComplex;
        frame.channels_comp0.resize(MOCK_CHANNEL_COUNT);
        frame.channels_comp1.resize(MOCK_CHANNEL_COUNT);
        for (int i = 0; i < MOCK_CHANNEL_COUNT; ++i) {
            double re = 5.0 * std::sin(t * (0.3 + i * 0.02)) + (QRandomGenerator::global()->bounded(100) / 100.0 - 0.5);
            double im = 5.0 * std::cos(t * (0.3 + i * 0.02)) + (QRandomGenerator::global()->bounded(100) / 100.0 - 0.5);
            frame.channels_comp0[i] = re;
            frame.channels_comp1[i] = im;
        }
    }
    //makeComplex = !makeComplex;

    emit frameReceived(frame);

    QByteArray raw;
    QDataStream ds(&raw, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << static_cast<quint64>(frame.timestamp);
    ds << static_cast<quint16>(frame.frameId);
    emit dataReceived(raw.toHex(' ').toUpper(), true);

    // 节流调试日志：最多每秒输出一次，避免刷屏
    static qint64 lastMockLogMs = 0;
    const qint64 nowMs = frame.timestamp;
    if (nowMs - lastMockLogMs >= 1000) {
        lastMockLogMs = nowMs;
        qDebug() << QString("模拟帧[%1]：模式=%2 通道数=%3")
                    .arg(frame.frameId)
                    .arg(frame.detectMode == FrameData::MultiChannelReal ? "幅值/相位" : "复数")
                    .arg(frame.channelCount);
    }
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

        if (headIndex > 0) {
            m_serialBuffer.remove(0, headIndex);
        }

        if (m_serialBuffer.size() < FRAME_LENGTH) {
            break; // 数据不足，等待下一次
        }

        int frameLength = FRAME_LENGTH;
        if (m_serialBuffer.size() >= 18) {
            const quint8 modeByte = static_cast<quint8>(m_serialBuffer.at(16));
            const quint8 channelCount = static_cast<quint8>(m_serialBuffer.at(17));
            if (modeByte <= static_cast<quint8>(FrameData::MultiChannelComplex)) {
                int payloadBytes = 0;
                if (modeByte == static_cast<quint8>(FrameData::MultiChannelReal)) {
                    payloadBytes = static_cast<int>(channelCount) * static_cast<int>(sizeof(float));
                } else if (modeByte == static_cast<quint8>(FrameData::MultiChannelComplex)) {
                    payloadBytes = static_cast<int>(channelCount) * static_cast<int>(sizeof(float)) * 2;
                }

                const int candidateLength = 18 + payloadBytes;
                if (m_serialBuffer.size() < candidateLength) {
                    break; // 扩展帧尚未接收完整
                }
                frameLength = candidateLength;
            }
        }

        // 提取完整帧
        QByteArray rawFrame = m_serialBuffer.left(frameLength);
        m_serialBuffer.remove(0, frameLength);

        // 解析并写入缓存
        FrameData frame = parseRawData(rawFrame);
        frame.timestamp = QDateTime::currentMSecsSinceEpoch();
        emit frameReceived(frame);

        qDebug() << QString("解析帧[%1]：模式=%2 通道数=%3")
                    .arg(frame.frameId)
                    .arg(frame.detectMode == FrameData::MultiChannelReal ? "实数" : (frame.detectMode == FrameData::MultiChannelComplex ? "复数" : "Legacy"))
                    .arg(frame.channelCount);
    }
}

FrameData SerialReceiver::parseRawData(const QByteArray& rawFrame)
{
    FrameData frame;
    // 使用安全解析，假定帧格式（小端）：
    // [0-1] head(2) | [2-3] frameId(u16) | [4-7] temperature(float) | [8-11] humidity(float) | [12-15] voltage(float)
    if (rawFrame.size() < FRAME_LENGTH) {
        qWarning() << "解析错误：帧长度不足，期待至少" << FRAME_LENGTH << "实际" << rawFrame.size();
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
    // 跳过旧的温度、湿度、电压字段（各4字节，共12字节）
    float dummy = 0.0f;
    ds >> frameId;
    ds >> dummy; // temperature
    ds >> dummy; // humidity
    ds >> dummy; // voltage

    frame.frameId = static_cast<uint32_t>(frameId);

    // 如果帧长度超出基础16字节，尝试读取扩展字段：mode/count及通道数据
    if (rawFrame.size() >= 18) { // 至少要有 mode 和 count
        // 将数据流定位到第16字节之后
        ds.device()->seek(16);
        quint8 modeByte = 0;
        quint8 chCount = 0;
        ds >> modeByte;
        ds >> chCount;
        if (modeByte <= static_cast<quint8>(FrameData::MultiChannelComplex)) {
            frame.detectMode = static_cast<FrameData::DetectionMode>(modeByte);
            frame.channelCount = chCount;
        }

        if (frame.channelCount > 0) {
            if (frame.detectMode == FrameData::MultiChannelReal) {
                frame.channels_comp0.resize(frame.channelCount);
                for (int i = 0; i < frame.channelCount && !ds.atEnd(); ++i) {
                    float v;
                    ds >> v;
                    frame.channels_comp0[i] = v;
                }
                frame.channels_comp1.clear();
            } else if (frame.detectMode == FrameData::MultiChannelComplex) {
                frame.channels_comp0.resize(frame.channelCount);
                frame.channels_comp1.resize(frame.channelCount);
                for (int i = 0; i < frame.channelCount && !ds.atEnd(); ++i) {
                    float realV, imagV;
                    ds >> realV;
                    ds >> imagV;
                    frame.channels_comp0[i] = realV;
                    frame.channels_comp1[i] = imagV;
                }
            }
        }
    }

    // 报警功能已移除

    return frame;
}

FrameData SerialReceiver::parseRawFrameForTest(const QByteArray& rawFrame)
{
    return parseRawData(rawFrame);
}

void SerialReceiver::sendCommand(const QByteArray& command)
{
#ifndef QT_COMPILE_FOR_WASM
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
#else
    // WebAssembly环境下模拟发送指令
    if (command.isEmpty()) {
        emit commandError("指令不能为空");
        qWarning() << "尝试发送空指令";
        return;
    }
    
    qInfo() << QString("WebAssembly环境：模拟发送指令，长度: %1字节").arg(command.size());
    emit commandSent(command);
#endif
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
