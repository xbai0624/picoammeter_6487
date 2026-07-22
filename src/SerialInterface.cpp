#include "SerialInterface.h"

#include <QElapsedTimer>
#include <QSerialPort>

namespace {
// Give up if the instrument sends nothing for this long. Individual reads can
// take a while at low baud rates (a burst dump is thousands of bytes), so the
// timeout applies to silence, not to the whole message.
constexpr int kSilenceTimeoutMs = 3000;
}

SerialInterface::SerialInterface() = default;
SerialInterface::~SerialInterface() { close(); }

bool SerialInterface::open(const QString &portName, int baudRate, QString *err)
{
    close();
    m_port.reset(new QSerialPort(portName));
    m_port->setBaudRate(baudRate);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port->open(QIODevice::ReadWrite)) {
        if (err)
            *err = QStringLiteral("cannot open serial port %1: %2")
                       .arg(portName, m_port->errorString());
        m_port.reset();
        return false;
    }
    m_port->clear();
    return true;
}

void SerialInterface::close()
{
    if (m_port && m_port->isOpen())
        m_port->close();
    m_port.reset();
}

bool SerialInterface::isOpen() const { return m_port && m_port->isOpen(); }

bool SerialInterface::clear(QString *err)
{
    if (!isOpen()) {
        if (err) *err = QStringLiteral("serial: not open");
        return false;
    }
    m_port->clear(); // discard driver tx/rx buffers
    return true;
}

bool SerialInterface::write(const QByteArray &data, QString *err)
{
    if (!isOpen()) {
        if (err) *err = QStringLiteral("serial: not open");
        return false;
    }
    QByteArray msg = data;
    if (!msg.endsWith('\n') && !msg.endsWith('\r'))
        msg += "\r\n";
    if (m_port->write(msg) != msg.size() || !m_port->waitForBytesWritten(kSilenceTimeoutMs)) {
        if (err)
            *err = QStringLiteral("serial write failed: %1").arg(m_port->errorString());
        return false;
    }
    return true;
}

bool SerialInterface::read(QByteArray &out, int maxLen, QString *err)
{
    if (!isOpen()) {
        if (err) *err = QStringLiteral("serial: not open");
        return false;
    }
    out.clear();
    QElapsedTimer silence;
    silence.start();
    // Accumulate until the terminating newline (the 6487 ends every response
    // with CR and/or LF) or until the instrument goes quiet.
    for (;;) {
        if (m_port->bytesAvailable() == 0 &&
            !m_port->waitForReadyRead(int(kSilenceTimeoutMs - silence.elapsed()))) {
            if (err)
                *err = out.isEmpty()
                           ? QStringLiteral("serial read timeout (no response)")
                           : QStringLiteral("serial read timeout (incomplete response)");
            return false;
        }
        if (m_port->bytesAvailable() > 0) {
            out += m_port->readAll();
            silence.restart();
        }
        if (out.contains('\n') || out.contains('\r') || out.size() >= maxLen)
            break;
        if (silence.elapsed() >= kSilenceTimeoutMs) {
            if (err) *err = QStringLiteral("serial read timeout");
            return false;
        }
    }
    while (out.endsWith('\n') || out.endsWith('\r'))
        out.chop(1);
    return true;
}
