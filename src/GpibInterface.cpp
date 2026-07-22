#include "GpibInterface.h"

#ifdef PICO_MOCK_GPIB

#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

// ===========================================================================
// Mock backend: pretends to be a Keithley 6487 producing noisy ~1 nA signal.
// Only the SCPI subset used by Pico6487Driver is emulated.
// ===========================================================================

GpibInterface::GpibInterface() : m_rng(std::random_device{}()) {}
GpibInterface::~GpibInterface() { close(); }

bool GpibInterface::open(int, int, QString *)
{
    m_open = true;
    m_responses.clear();
    m_burstArmed = false;
    return true;
}

void GpibInterface::close() { m_open = false; }
bool GpibInterface::isOpen() const { return m_open; }
bool GpibInterface::clear(QString *) { m_responses.clear(); return m_open; }

QByteArray GpibInterface::mockReading()
{
    std::normal_distribution<double> noise(0.0, 2e-11);
    constexpr double kTwoPi = 6.283185307179586;
    double t = m_burstTimer.isValid() ? m_burstTimer.elapsed() / 1000.0 : 0.0;
    double value = 1.0e-9 + 2.0e-10 * std::sin(kTwoPi * 0.2 * t) + noise(m_rng);
    return QByteArray::number(value, 'E', 6);
}

// Wraps count synthetic readings either as comma-separated ASCII or as a
// definite-length SREAL block, matching the configured FORM:DATA.
QByteArray GpibInterface::mockBatch(int count)
{
    if (!m_binaryFormat) {
        QByteArray out;
        for (int i = 0; i < count; ++i) {
            if (i) out += ',';
            out += mockReading();
        }
        return out;
    }
    QByteArray payload;
    payload.reserve(count * 4);
    for (int i = 0; i < count; ++i) {
        const float v = float(mockReading().toDouble());
        quint32 bits = 0;
        std::memcpy(&bits, &v, 4);
        bits = qToLittleEndian(bits);
        payload.append(reinterpret_cast<const char *>(&bits), 4);
    }
    const QByteArray len = QByteArray::number(payload.size());
    return "#" + QByteArray::number(len.size()) + len + payload;
}

void GpibInterface::mockHandle(const QByteArray &cmdRaw)
{
    const QByteArray cmd = cmdRaw.trimmed().toUpper();
    if (cmd == "*IDN?") {
        m_responses.enqueue("MOCK INSTRUMENTS,Model 6487 (simulated),0000000,A00");
    } else if (cmd == "FORM:DATA?") {
        m_responses.enqueue(m_binaryFormat ? "SRE" : "ASC");
    } else if (cmd.startsWith("FORM:DATA")) {
        m_binaryFormat = cmd.contains("SREAL");
    } else if (cmd == "READ?") {
        // Honors TRIG:COUN: returns m_burstSize readings taken back-to-back,
        // taking n/rate to "measure" plus a small command latency.
        if (!m_burstTimer.isValid())
            m_burstTimer.start();
        QThread::usleep(5000 + qint64(m_burstSize / m_mockRate * 1e6));
        m_responses.enqueue(mockBatch(m_burstSize));
    } else if (cmd.startsWith("TRIG:COUN")) {
        bool ok = false;
        int n = cmd.mid(cmd.lastIndexOf(' ') + 1).toInt(&ok);
        if (ok && n > 0)
            m_burstSize = n;
    } else if (cmd == "INIT") {
        m_burstArmed = true;
        m_burstTimer.start();
    } else if (cmd == "TRAC:POIN:ACT?") {
        int n = 0;
        if (m_burstArmed)
            n = std::min<int>(m_burstSize,
                              int(m_burstTimer.elapsed() / 1000.0 * m_mockRate));
        m_responses.enqueue(QByteArray::number(n));
    } else if (cmd == "TRAC:DATA?") {
        m_burstArmed = false;
        m_responses.enqueue(mockBatch(m_burstSize));
    } else if (cmd == "*OPC?") {
        m_responses.enqueue("1");
    }
    // all other configuration commands are accepted silently
}

bool GpibInterface::write(const QByteArray &data, QString *err)
{
    if (!m_open) {
        if (err) *err = QStringLiteral("mock GPIB: not open");
        return false;
    }
    // a message may contain several ";"-chained commands
    for (const QByteArray &part : data.split(';'))
        mockHandle(part);
    return true;
}

bool GpibInterface::read(QByteArray &out, int, QString *err, int)
{
    if (!m_open || m_responses.isEmpty()) {
        if (err) *err = QStringLiteral("mock GPIB: nothing to read (query timeout)");
        return false;
    }
    out = m_responses.dequeue();
    return true;
}

bool GpibInterface::readBlock(QByteArray &payload, QString *err, int timeoutMs)
{
    if (!read(payload, 0, err, timeoutMs))
        return false;
    return stripBlockHeader(payload, err);
}

#elif defined(PICO_NO_GPIB) // ====== built without a GPIB stack =============

GpibInterface::GpibInterface() = default;
GpibInterface::~GpibInterface() = default;

bool GpibInterface::open(int, int, QString *err)
{
    if (err)
        *err = QStringLiteral(
            "This build has no GPIB support (NI-488.2 / linux-gpib was not "
            "found at compile time). Use the RS-232 interface instead, or "
            "install the GPIB driver stack and rebuild.");
    return false;
}

void GpibInterface::close() {}
bool GpibInterface::isOpen() const { return false; }
bool GpibInterface::clear(QString *err) { return open(0, 0, err); }
bool GpibInterface::write(const QByteArray &, QString *err) { return open(0, 0, err); }
bool GpibInterface::read(QByteArray &, int, QString *err, int) { return open(0, 0, err); }
bool GpibInterface::readBlock(QByteArray &, QString *err, int) { return open(0, 0, err); }

#else // ===================== real GPIB backend =============================

#ifdef _WIN32
// ni4882.h is the current NI-488.2 header; fall back to the legacy name.
#if defined(__has_include) && !__has_include(<ni4882.h>)
#include <ni488.h>
#else
#include <ni4882.h>
#endif
#else
#include <gpib/ib.h>
#endif

namespace {
// ibdev timeout code: T1s=11, T3s=12, T10s=13 (same values in both stacks)
constexpr int kTimeoutCode = 13; // 10 s, generous for long burst transfers
}

GpibInterface::GpibInterface() = default;
GpibInterface::~GpibInterface() { close(); }

QString GpibInterface::ibErrorString(int iberrCode, int ibstaBits)
{
    static const char *names[] = {
        "EDVR (system error)",     "ECIC (not controller-in-charge)",
        "ENOL (no listener)",      "EADR (board not addressed)",
        "EARG (invalid argument)", "ESAC (not system controller)",
        "EABO (I/O aborted / timeout)", "ENEB (board not found)",
        "EDMA (DMA error)",        "",
        "EOIP (async I/O in progress)", "ECAP (no capability)",
        "EFSO (file system error)", "",
        "EBUS (bus error)",        "ESTB (status byte lost)",
        "ESRQ (SRQ stuck)",        "",
        "", "",
        "ETAB (table overflow)"
    };
    QString name = (iberrCode >= 0 && iberrCode < int(sizeof(names) / sizeof(*names)))
                       ? QString::fromLatin1(names[iberrCode])
                       : QStringLiteral("unknown");
    return QStringLiteral("GPIB error %1 [%2], ibsta=0x%3")
        .arg(iberrCode)
        .arg(name)
        .arg(ibstaBits, 0, 16);
}

bool GpibInterface::open(int boardIndex, int primaryAddress, QString *err)
{
    close();
    m_ud = ibdev(boardIndex, primaryAddress, 0 /*no secondary addr*/,
                 kTimeoutCode, 1 /*assert EOI on write*/, 0 /*no EOS char*/);
    if (m_ud < 0 || (ibsta & ERR)) {
        if (err)
            *err = QStringLiteral("ibdev(board=%1, addr=%2) failed: %3")
                       .arg(boardIndex)
                       .arg(primaryAddress)
                       .arg(ibErrorString(iberr, ibsta));
        m_ud = -1;
        return false;
    }
    return true;
}

void GpibInterface::close()
{
    if (m_ud >= 0) {
        ibonl(m_ud, 0); // take descriptor offline
        m_ud = -1;
    }
}

bool GpibInterface::isOpen() const { return m_ud >= 0; }

bool GpibInterface::clear(QString *err)
{
    if (m_ud < 0) {
        if (err) *err = QStringLiteral("GPIB: not open");
        return false;
    }
    ibclr(m_ud);
    if (ibsta & ERR) {
        if (err) *err = QStringLiteral("ibclr failed: ") + ibErrorString(iberr, ibsta);
        return false;
    }
    return true;
}

bool GpibInterface::write(const QByteArray &data, QString *err)
{
    if (m_ud < 0) {
        if (err) *err = QStringLiteral("GPIB: not open");
        return false;
    }
    QByteArray msg = data;
    if (!msg.endsWith('\n'))
        msg += '\n';
    ibwrt(m_ud, msg.constData(), msg.size());
    if (ibsta & ERR) {
        if (err)
            *err = QStringLiteral("ibwrt(\"%1\") failed: %2")
                       .arg(QString::fromLatin1(data.left(40)))
                       .arg(ibErrorString(iberr, ibsta));
        return false;
    }
    return true;
}

bool GpibInterface::read(QByteArray &out, int maxLen, QString *err, int /*timeoutMs*/)
{
    if (m_ud < 0) {
        if (err) *err = QStringLiteral("GPIB: not open");
        return false;
    }
    out.clear();
    QByteArray chunk(16384, Qt::Uninitialized);
    // Read until END (EOI) is seen or maxLen is reached.
    for (;;) {
        ibrd(m_ud, chunk.data(), chunk.size());
        if (ibsta & ERR) {
            if (err) *err = QStringLiteral("ibrd failed: ") + ibErrorString(iberr, ibsta);
            return false;
        }
        out.append(chunk.constData(), int(ibcntl));
        if ((ibsta & END) || out.size() >= maxLen)
            break;
    }
    while (out.endsWith('\n') || out.endsWith('\r'))
        out.chop(1);
    return true;
}

bool GpibInterface::readBlock(QByteArray &payload, QString *err, int /*timeoutMs*/)
{
    if (m_ud < 0) {
        if (err) *err = QStringLiteral("GPIB: not open");
        return false;
    }
    // Raw read until END — no trailing-newline trimming, which could eat
    // payload bytes that happen to equal 0x0A/0x0D.
    payload.clear();
    QByteArray chunk(16384, Qt::Uninitialized);
    for (;;) {
        ibrd(m_ud, chunk.data(), chunk.size());
        if (ibsta & ERR) {
            if (err) *err = QStringLiteral("ibrd failed: ") + ibErrorString(iberr, ibsta);
            return false;
        }
        payload.append(chunk.constData(), int(ibcntl));
        if (ibsta & END)
            break;
    }
    return stripBlockHeader(payload, err);
}

#endif // PICO_MOCK_GPIB
