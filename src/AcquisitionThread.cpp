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

    if (!drv.configureSpeed(p.rangeAmps, p.nplc, p.keepDisplayOn, &err)) {
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
            // Size the burst so one fill takes ~refreshMs. Cap at 2500: the
            // 6485 buffer holds 2500 readings (6487: 3000) — use the smaller
            // so both models work.
            const int wanted =
                qBound(10, int(estRate * m_refreshMs.load() / 1000.0 + 0.5), 2500);
            // Reconfigure only on a meaningful change (>10%) to avoid
            // re-sending the trigger/buffer setup on every rate jitter.
            if (burstSize < 10 || std::abs(wanted - burstSize) * 10 > burstSize) {
                burstSize = wanted;
                if (!drv.configureBurst(burstSize, &err)) {
                    emit errorOccurred(QStringLiteral("Burst setup failed: ") + err);
                    failed = true;
                    break;
                }
            }

            if (!drv.armBurst(&err)) { failed = true; break; }
            const double tStart = clock.nsecsElapsed() * 1e-9;

            // Wait for the internal buffer to fill, sampling (time, count)
            // pairs so the true fill rate can be measured between polls —
            // excluding the fixed arm/transfer overhead, which previously fed
            // back into the burst sizing and collapsed it over high-latency
            // links (serial ended up at minimum burst, ~16 rdg/s).
            int actual = 0;
            double tFirst = 0, tLast = 0;
            int nFirst = 0, nLast = 0;
            while (!m_stop.load()) {
                msleep(5);
                if (!drv.burstPointCount(actual, &err)) { failed = true; break; }
                const double now = clock.nsecsElapsed() * 1e-9;
                if (nFirst == 0 && actual > 0) { tFirst = now; nFirst = actual; }
                if (actual > nLast)            { tLast = now;  nLast = actual;  }
                if (actual >= burstSize)
                    break;
            }
            if (failed || m_stop.load())
                break;
            const double tEnd = clock.nsecsElapsed() * 1e-9;

            QVector<double> currents;
            if (!drv.fetchBurst(currents, &err)) { failed = true; break; }
            if (nLast > nFirst && tLast > tFirst)
                estRate = (nLast - nFirst) / (tLast - tFirst); // pure fill rate
            // else: filled within one poll — keep the previous estimate

            // True fill window: n readings ending where the buffer was seen
            // full (tLast), spaced by the measured fill rate. Using the whole
            // arm..poll interval would inflate the clump width with command
            // latency that is not measurement time.
            const int n = currents.size();
            const double fillEnd = (tLast > 0) ? tLast : tEnd;
            const double dt = 1.0 / estRate;
            QVector<double> times(n);
            for (int i = 0; i < n; ++i)
                times[i] = fillEnd - (n - 1 - i) * dt;

            emit newReadings(times, currents);
        }
    } else { // Continuous: batched READ? — TRIG:COUN readings per round trip
        double estRate = qBound(1.0, 1000.0 * 0.01 / p.nplc, 1000.0);
        int batch = 0;
        while (!failed && !m_stop.load()) {
            const int wanted =
                qBound(1, int(estRate * m_refreshMs.load() / 1000.0 + 0.5), 2500);
            if (batch == 0 || std::abs(wanted - batch) * 10 > batch) {
                batch = wanted;
                if (!drv.configureContinuous(batch, &err)) {
                    emit errorOccurred(QStringLiteral("Continuous setup failed: ") + err);
                    failed = true;
                    break;
                }
            }

            const double t0 = clock.nsecsElapsed() * 1e-9;
            QVector<double> currents;
            int respBytes = 0;
            if (!drv.readBatch(currents, int(batch / estRate * 1000.0) + 1000,
                               &respBytes, &err)) {
                failed = true;
                break;
            }
            const double t1 = clock.nsecsElapsed() * 1e-9;
            // Includes one command round trip of overhead: mild underestimate
            // that converges (overhead shrinks relative to a growing batch).
            // Kept as wall-time rate on purpose — it sizes batches against the
            // refresh cadence.
            if (t1 > t0)
                estRate = currents.size() / (t1 - t0);

            // True sample times: the serial transfer of the reply happens
            // after the last reading is taken, so the measurement window ends
            // ~responseBytes*10/baud before t1 (10 bits/byte at 8N1).
            const double transfer =
                (p.bus == Bus::Serial) ? respBytes * 10.0 / p.baudRate : 0.0;
            const double measEnd = qMax(t0, t1 - transfer);
            const int n = currents.size();
            const double dt = (measEnd - t0) / n;
            QVector<double> times(n);
            for (int i = 0; i < n; ++i)
                times[i] = t0 + (i + 1) * dt;
            emit newReadings(times, currents);
        }
    }

    drv.shutdown();
    comm->close();

    if (failed)
        emit errorOccurred(QStringLiteral("Acquisition stopped: ") + err);
    else
        emit finishedNormally();
}
