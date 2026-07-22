#include "AcquisitionThread.h"
#include "GpibInterface.h"
#include "Pico6487Driver.h"
#include "SerialInterface.h"

#include <QElapsedTimer>
#include <cstdlib>
#include <memory>

AcquisitionThread::AcquisitionThread(QObject *parent) : QThread(parent) {}

void AcquisitionThread::run()
{
    m_stop.store(false);
    const Parameters p = m_params;

    QString err;
    std::unique_ptr<CommInterface> comm;
#ifdef PICO_MOCK_GPIB
    // Mock builds simulate the instrument regardless of the selected bus.
    {
        auto gpib = std::make_unique<GpibInterface>();
        if (!gpib->open(p.boardIndex, p.gpibAddress, &err)) {
            emit errorOccurred(err);
            return;
        }
        comm = std::move(gpib);
    }
#else
    if (p.bus == Bus::Gpib) {
        auto gpib = std::make_unique<GpibInterface>();
        if (!gpib->open(p.boardIndex, p.gpibAddress, &err)) {
            emit errorOccurred(err);
            return;
        }
        comm = std::move(gpib);
    } else {
        auto serial = std::make_unique<SerialInterface>();
        if (!serial->open(p.serialPort, p.baudRate, &err)) {
            emit errorOccurred(err);
            return;
        }
        comm = std::move(serial);
    }
#endif

    Pico6487Driver drv(comm.get());
    QString idn;
    if (!drv.identify(idn, &err)) {
        emit errorOccurred(QStringLiteral("No response from instrument: ") + err);
        return;
    }
    emit instrumentInfo(idn);

    if (!drv.configureSpeed(p.rangeAmps, p.nplc, &err)) {
        emit errorOccurred(QStringLiteral("Configuration failed: ") + err);
        return;
    }

    QElapsedTimer clock;
    clock.start();
    bool failed = false;

    if (p.mode == Mode::Burst) {
        // Reading rate estimate, refined after every burst from measured fill
        // time; the initial guess assumes the instrument tops out near
        // 1000 rdg/s at NPLC 0.01 and scales with integration time.
        double estRate = qBound(10.0, 1000.0 * 0.01 / p.nplc, 1000.0);
        int burstSize = 0;
        while (!failed && !m_stop.load()) {
            // Size the burst so one fill takes ~refreshMs (6487 buffer: 3000).
            const int wanted =
                qBound(2, int(estRate * m_refreshMs.load() / 1000.0 + 0.5), 3000);
            // Reconfigure only on a meaningful change (>10%) to avoid
            // re-sending the trigger/buffer setup on every rate jitter.
            if (burstSize == 0 || std::abs(wanted - burstSize) * 10 > burstSize) {
                burstSize = wanted;
                if (!drv.configureBurst(burstSize, &err)) {
                    emit errorOccurred(QStringLiteral("Burst setup failed: ") + err);
                    failed = true;
                    break;
                }
            }

            if (!drv.armBurst(&err)) { failed = true; break; }
            const double tStart = clock.nsecsElapsed() * 1e-9;

            // Wait for the internal buffer to fill.
            int actual = 0;
            while (!m_stop.load()) {
                msleep(5);
                if (!drv.burstPointCount(actual, &err)) { failed = true; break; }
                if (actual >= burstSize)
                    break;
            }
            if (failed || m_stop.load())
                break;
            const double tEnd = clock.nsecsElapsed() * 1e-9;

            QVector<double> currents;
            if (!drv.fetchBurst(currents, &err)) { failed = true; break; }
            if (tEnd > tStart)
                estRate = currents.size() / (tEnd - tStart);

            // Readings are evenly spaced across the measured fill interval.
            const int n = currents.size();
            const double dt = (tEnd - tStart) / n;
            QVector<double> times(n);
            for (int i = 0; i < n; ++i)
                times[i] = tStart + (i + 1) * dt;

            emit newReadings(times, currents);
        }
    } else { // Continuous
        if (!drv.configureContinuous(&err)) {
            emit errorOccurred(QStringLiteral("Continuous setup failed: ") + err);
            failed = true;
        }
        QVector<double> times, currents;
        QElapsedTimer batchTimer;
        batchTimer.start();
        while (!failed && !m_stop.load()) {
            double amps = 0.0;
            if (!drv.readSingle(amps, &err)) { failed = true; break; }
            times.append(clock.nsecsElapsed() * 1e-9);
            currents.append(amps);
            if (batchTimer.elapsed() >= m_refreshMs.load()) {
                emit newReadings(times, currents);
                times.clear();
                currents.clear();
                batchTimer.restart();
            }
        }
        if (!currents.isEmpty())
            emit newReadings(times, currents);
    }

    drv.shutdown();
    comm->close();

    if (failed)
        emit errorOccurred(QStringLiteral("Acquisition stopped: ") + err);
    else
        emit finishedNormally();
}
