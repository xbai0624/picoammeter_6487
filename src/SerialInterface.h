#ifndef SERIAL_INTERFACE_H
#define SERIAL_INTERFACE_H

#include "CommInterface.h"

#include <memory>

class QSerialPort;

// RS-232 transport for the 6487's built-in serial port (8N1, no flow control).
// The instrument's MENU -> RS-232 settings must match the chosen baud rate.
class SerialInterface : public CommInterface
{
public:
    SerialInterface();
    ~SerialInterface() override;

    SerialInterface(const SerialInterface &) = delete;
    SerialInterface &operator=(const SerialInterface &) = delete;

    bool open(const QString &portName, int baudRate, QString *err = nullptr);
    void close() override;
    bool isOpen() const override;

    bool clear(QString *err = nullptr) override;
    bool write(const QByteArray &data, QString *err = nullptr) override;
    bool read(QByteArray &out, int maxLen = 65536, QString *err = nullptr,
              int timeoutMs = 3000) override;

private:
    std::unique_ptr<QSerialPort> m_port;
};

#endif // SERIAL_INTERFACE_H
