#ifndef GPIB_INTERFACE_H
#define GPIB_INTERFACE_H

#include "CommInterface.h"

#ifdef PICO_MOCK_GPIB
#include <QElapsedTimer>
#include <QQueue>
#include <random>
#endif

// Thin wrapper over the NI-488.2 (Windows) / linux-gpib (Linux) "ib*" C API.
// With PICO_MOCK_GPIB it instead emulates a 6487 producing synthetic data.
class GpibInterface : public CommInterface
{
public:
    GpibInterface();
    ~GpibInterface() override;

    GpibInterface(const GpibInterface &) = delete;
    GpibInterface &operator=(const GpibInterface &) = delete;

    bool open(int boardIndex, int primaryAddress, QString *err = nullptr);
    void close() override;
    bool isOpen() const override;

    // Device clear (ibclr) — resets the instrument's GPIB message buffers.
    bool clear(QString *err = nullptr) override;

    // Write one command/message (a trailing '\n' is appended if missing).
    bool write(const QByteArray &data, QString *err = nullptr) override;

    // Read one response message (up to maxLen bytes), trailing whitespace trimmed.
    // timeoutMs is ignored: the GPIB descriptor uses its own fixed 10 s timeout.
    bool read(QByteArray &out, int maxLen = 65536, QString *err = nullptr,
              int timeoutMs = 3000) override;

private:
#ifdef PICO_MOCK_GPIB
    // --- Mock backend: emulates a Keithley 6487 with synthetic data ---
    bool m_open = false;
    QQueue<QByteArray> m_responses;
    int m_burstSize = 1000;
    bool m_burstArmed = false;
    QElapsedTimer m_burstTimer;
    double m_mockRate = 900.0; // synthetic readings per second
    std::mt19937 m_rng;
    void mockHandle(const QByteArray &cmd);
    QByteArray mockReading();
#else
    int m_ud = -1; // ibdev unit descriptor
    static QString ibErrorString(int iberrCode, int ibstaBits);
#endif
};

#endif // GPIB_INTERFACE_H
