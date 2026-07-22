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

bool SerialInterface::read(QByteArray &out, int maxLen, QString *err, int timeoutMs)
{
    if (!isOpen()) {
        if (err) *err = QStringLiteral("serial: not open");
        return false;
    }
    out.clear();
    const int silenceMs = qMax(timeoutMs, kSilenceTimeoutMs);
    QElapsedTimer silence;
    silence.start();
    // Accumulate until the terminating newline (the 6487 ends every response
    // with CR and/or LF) or until the instrument goes quiet.
    for (;;) {
        if (m_port->bytesAvailable() == 0 &&
            !m_port->waitForReadyRead(int(silenceMs - silence.elapsed()))) {
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
        // Ignore stray leading terminators (e.g. the tail of a previous
        // binary block whose CR/LF arrived after the payload was consumed).
        while (out.startsWith('\n') || out.startsWith('\r'))
            out.remove(0, 1);
        if (out.contains('\n') || out.contains('\r') || out.size() >= maxLen)
            break;
        if (silence.elapsed() >= silenceMs) {
            if (err) *err = QStringLiteral("serial read timeout");
            return false;
        }
    }
    while (out.endsWith('\n') || out.endsWith('\r'))
        out.chop(1);
    return true;
}

bool SerialInterface::readBlock(QByteArray &payload, QString *err, int timeoutMs)
{
    if (!isOpen()) {
        if (err) *err = QStringLiteral("serial: not open");
        return false;
    }
    payload.clear();
    QByteArray buf;
    const int silenceMs = qMax(timeoutMs, kSilenceTimeoutMs);
    QElapsedTimer silence;
    silence.start();
    int need = -1; // total bytes of the full block once the header is known
    for (;;) {
        if (m_port->bytesAvailable() == 0 &&
            !m_port->waitForReadyRead(int(silenceMs - silence.elapsed()))) {
            if (err)
                *err = buf.isEmpty()
                           ? QStringLiteral("serial read timeout (no response)")
                           : QStringLiteral("serial read timeout (incomplete block)");
            return false;
        }
        if (m_port->bytesAvailable() > 0) {
            buf += m_port->readAll();
            silence.restart();
        }
        if (need < 0) {
            // Parse '#' + digit-count + length once enough bytes arrived.
            const int hash = buf.indexOf('#');
            if (hash >= 0 && buf.size() >= hash + 2) {
                const int nDigits = buf[hash + 1] - '0';
                if (nDigits < 1 || nDigits > 9) {
                    if (err)
                        *err = QStringLiteral("malformed binary block header");
                    return false;
                }
                if (buf.size() >= hash + 2 + nDigits) {
                    bool ok = false;
                    const int len = buf.mid(hash + 2, nDigits).toInt(&ok);
                    if (!ok) {
                        if (err)
                            *err = QStringLiteral("malformed binary block length");
                        return false;
                    }
                    need = hash + 2 + nDigits + len;
                }
            }
        }
        if (need >= 0 && buf.size() >= need) {
            QByteArray msg = buf.left(need);
            if (!stripBlockHeader(msg, err))
                return false;
            payload = msg;
            // Trailing CR/LF (arriving with or slightly after the payload) is
            // discarded by the next read's leading-'#' search.
            return true;
        }
        if (silence.elapsed() >= silenceMs) {
            if (err) *err = QStringLiteral("serial read timeout");
            return false;
        }
    }
}
