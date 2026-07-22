#include <QList>
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

bool Pico6487Driver::parseReadings(const QByteArray &resp, QVector<double> &out,
                                   QString *err)
{
    out.clear();
    for (const QByteArray &tok : resp.split(',')) {
        bool ok = false;
        double v = tok.trimmed().toDouble(&ok);
        if (ok)
            out.append(v);
    }
    if (out.isEmpty()) {
        if (err)
            *err = QStringLiteral("no parsable readings in reply: \"%1\"")
                       .arg(QString::fromLatin1(resp.left(64)));
        return false;
    }
    return true;
}

bool Pico6487Driver::fetchBurst(QVector<double> &readings, QString *err)
{
    QByteArray resp;
    // ~15 bytes per ASCII reading; allow generous headroom.
    if (!m_gpib->query("TRAC:DATA?", resp, m_burstSize * 32 + 256, err))
        return false;
    readings.reserve(m_burstSize);
    return parseReadings(resp, readings, err);
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

bool Pico6487Driver::readBatch(QVector<double> &readings, int expectedMs, QString *err)
{
    QByteArray resp;
    // READ? blocks until all triggerCount readings are taken, then returns
    // them comma-separated — allow the measurement time plus margin.
    if (!m_gpib->query("READ?", resp, m_triggerCount * 32 + 256, err,
                       expectedMs + 3000))
        return false;
    readings.reserve(m_triggerCount);
    return parseReadings(resp, readings, err);
}

void Pico6487Driver::shutdown()
{
    // Best effort — ignore errors, the user may have unplugged the device.
    cmd("ABOR", nullptr);
    cmd("DISP:ENAB ON", nullptr);
    cmd("SYST:ZCH ON", nullptr);
}
