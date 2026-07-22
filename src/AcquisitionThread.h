#ifndef ACQUISITION_THREAD_H
#define ACQUISITION_THREAD_H

#include <QString>
#include <QThread>
#include <QVector>
#include <atomic>

// Worker thread that owns the GPIB connection and the 6487 driver.
// Configure via setParameters() before start(); request exit with requestStop().
class AcquisitionThread : public QThread
{
    Q_OBJECT
public:
    enum class Mode { Burst, Continuous };
    enum class Bus { Gpib, Serial };

    struct Parameters {
        Bus bus = Bus::Gpib;
        int boardIndex = 0;
        int gpibAddress = 22;     // 6487 factory default
        QString serialPort;       // e.g. "COM3" / "/dev/ttyUSB0"
        int baudRate = 57600;     // must match the 6487's MENU -> RS-232 setting
        Mode mode = Mode::Burst;
        double rangeAmps = 2e-6;  // <= 0 means autorange
        double nplc = 0.01;
        double refreshSeconds = 0.05; // display update interval
        bool keepDisplayOn = false;   // leave front panel display enabled (slower)
    };

    explicit AcquisitionThread(QObject *parent = nullptr);

    void setParameters(const Parameters &p)
    {
        m_params = p;
        setRefreshSeconds(p.refreshSeconds);
    }
    void requestStop() { m_stop.store(true); }

    // Thread-safe; may be called while running to change the update rate live.
    // In burst mode the burst size is re-derived from the measured reading rate.
    void setRefreshSeconds(double s)
    {
        m_refreshMs.store(qBound(10, int(s * 1000.0 + 0.5), 3000));
    }

signals:
    // times are seconds elapsed since acquisition start (monotonic clock).
    void newReadings(QVector<double> times, QVector<double> currents);
    void instrumentInfo(QString idn);
    void errorOccurred(QString message);
    void finishedNormally();

protected:
    void run() override;

private:
    Parameters m_params;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_refreshMs{50};
};

#endif // ACQUISITION_THREAD_H
