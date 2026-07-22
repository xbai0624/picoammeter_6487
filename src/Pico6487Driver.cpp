#include <QList>
#include <QtEndian>
#include <cstring>
#include "Pico6487Driver.h"
#include "CommInterface.h"

Pico6487Driver::Pico6487Driver(CommInterface *comm) : m_gpib(comm) {}

bool Pico6487Driver::cmd(const char *scpi, QString *err)
{
    return cmd(QByteArray(scpi), err);
}

bool Pico6487Driver::cmd(const QByteArray &scpi, QString *err)
{
    return m_gpib->write(scpi, err);
}

bool Pico6487Driver::identify(QString &idn, QString *err)
{
    QByteArray resp;
    if (!m_gpib->query("*IDN?", resp, 256, err))
        return false;
    idn = QString::fromLatin1(resp).trimmed();
    return true;
}

bool Pico6487Driver::configureSpeed(double rangeAmps, double nplc, bool keepDisplayOn,
                                    QString *err)
{
    if (!m_gpib->clear(err))
        return false;
    if (!cmd("*RST", err))
        return false;
    // Zero check must be ON while changing measurement configuration.
    if (!cmd("SYST:ZCH ON", err))
        return false;
    if (rangeAmps > 0.0) {
        if (!cmd("CURR:RANG:AUTO OFF", err))
            return false;
        if (!cmd(QByteArray("CURR:RANG ") + QByteArray::number(rangeAmps, 'E', 3), err))
            return false;
    } else {
        if (!cmd("CURR:RANG:AUTO ON", err))
            return false;
    }
    if (!cmd(QByteArray("CURR:NPLC ") + QByteArray::number(nplc, 'f', 3), err))
        return false;
    if (!cmd("SYST:AZER OFF", err))      // autozero off: big speed gain
        return false;
    if (!cmd("FORM:ELEM READ", err))     // reading value only (no units/status)
        return false;
    if (!cmd("FORM:DATA SREAL", err))    // binary: 4 bytes/reading vs ~15 ASCII
        return false;
    if (!cmd("FORM:BORD SWAP", err))     // little-endian floats
        return false;
    if (!keepDisplayOn) {
        if (!cmd("DISP:ENAB OFF", err))  // front panel off: faster internal loop
            return false;
    }
    if (!cmd("SYST:ZCH OFF", err))       // enable measurement
        return false;
    // Wait for configuration to settle before triggering.
    QByteArray opc;
    return m_gpib->query("*OPC?", opc, 16, err);
}

bool Pico6487Driver::configureBurst(int burstSize, QString *err)
{
    m_burstSize = burstSize;
    if (!cmd("ARM:COUN 1", err))
        return false;
    if (!cmd(QByteArray("TRIG:COUN ") + QByteArray::number(burstSize), err))
        return false;
    if (!cmd("TRIG:DEL 0", err))
        return false;
    if (!cmd(QByteArray("TRAC:POIN ") + QByteArray::number(burstSize), err))
        return false;
    return cmd("TRAC:FEED SENS", err);
}

bool Pico6487Driver::armBurst(QString *err)
{
    if (!cmd("TRAC:CLE", err))
        return false;
    if (!cmd("TRAC:FEED:CONT NEXT", err)) // buffer fills once, then stops
        return false;
    return cmd("INIT", err);
}

bool Pico6487Driver::burstPointCount(int &actual, QString *err)
{
    QByteArray resp;
    if (!m_gpib->query("TRAC:POIN:ACT?", resp, 32, err))
        return false;
    bool ok = false;
    actual = resp.trimmed().toInt(&ok);
    if (!ok) {
        if (err)
            *err = QStringLiteral("unexpected TRAC:POIN:ACT? reply: \"%1\"")
                       .arg(QString::fromLatin1(resp));
        return false;
    }
    return true;
}

bool Pico6487Driver::parseSreal(const QByteArray &payload, QVector<double> &out,
                                QString *err)
{
    if (payload.isEmpty() || payload.size() % 4 != 0) {
        if (err)
            *err = QStringLiteral("SREAL payload has invalid length %1")
                       .arg(payload.size());
        return false;
    }
    const int n = payload.size() / 4;
    out.clear();
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        quint32 bits;
        std::memcpy(&bits, payload.constData() + i * 4, 4);
        bits = qFromLittleEndian(bits); // FORM:BORD SWAP
        float v;
        std::memcpy(&v, &bits, 4);
        out.append(double(v));
    }
    return true;
}

bool Pico6487Driver::fetchBurst(QVector<double> &readings, QString *err)
{
    QByteArray payload;
    if (!m_gpib->queryBlock("TRAC:DATA?", payload, err))
        return false;
    return parseSreal(payload, readings, err);
}

bool Pico6487Driver::configureContinuous(int triggerCount, QString *err)
{
    m_triggerCount = triggerCount;
    if (!cmd("ARM:COUN 1", err))
        return false;
    if (!cmd(QByteArray("TRIG:COUN ") + QByteArray::number(triggerCount), err))
        return false;
    return cmd("TRIG:DEL 0", err);
}

bool Pico6487Driver::readBatch(QVector<double> &readings, int expectedMs,
                               int *responseBytes, QString *err)
{
    QByteArray payload;
    // READ? blocks until all triggerCount readings are taken, then returns
    // them as an SREAL block — allow the measurement time plus margin.
    if (!m_gpib->queryBlock("READ?", payload, err, expectedMs + 3000))
        return false;
    if (responseBytes)
        *responseBytes = payload.size() + 6; // + block header/terminator bytes
    return parseSreal(payload, readings, err);
}

void Pico6487Driver::shutdown()
{
    // Best effort — ignore errors, the user may have unplugged the device.
    cmd("ABOR", nullptr);
    cmd("DISP:ENAB ON", nullptr);
    cmd("SYST:ZCH ON", nullptr);
}
