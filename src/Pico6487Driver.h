#ifndef PICO6487_DRIVER_H
#define PICO6487_DRIVER_H

#include <QString>
#include <QVector>

class CommInterface;

// SCPI driver for the Keithley 6487 picoammeter.
// Works over any transport (GPIB or RS-232); all methods return false on
// failure and fill *err.
class Pico6487Driver
{
public:
    explicit Pico6487Driver(CommInterface *comm);

    bool identify(QString &idn, QString *err = nullptr);

    // One-time speed setup shared by both modes:
    // fixed range, short integration, autozero off, reading-only format.
    // rangeAmps <= 0 selects autorange (slower; not recommended for fast mode).
    // keepDisplayOn leaves the front panel display enabled (costs some speed).
    bool configureSpeed(double rangeAmps, double nplc, bool keepDisplayOn,
                        QString *err = nullptr);

    // --- Burst (buffered) mode: fastest readout ---
    bool configureBurst(int burstSize, QString *err = nullptr);
    bool armBurst(QString *err = nullptr);                   // re-arm buffer + INIT
    bool burstPointCount(int &actual, QString *err = nullptr); // TRAC:POIN:ACT?
    bool fetchBurst(QVector<double> &readings, QString *err = nullptr);

    // --- Continuous mode: one triggered reading per call ---
    bool configureContinuous(QString *err = nullptr);
    bool readSingle(double &amps, QString *err = nullptr);

    // Restore a friendly front-panel state (display on, zero check on).
    void shutdown();

    int burstSize() const { return m_burstSize; }

private:
    bool cmd(const char *scpi, QString *err);
    bool cmd(const QByteArray &scpi, QString *err);

    CommInterface *m_gpib;
    int m_burstSize = 1000;
};

#endif // PICO6487_DRIVER_H
