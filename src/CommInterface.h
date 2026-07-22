#ifndef COMM_INTERFACE_H
#define COMM_INTERFACE_H

#include <QByteArray>
#include <QString>

// Abstract byte transport to the instrument (GPIB or RS-232).
// All methods return false on failure and fill *err with a description.
// Not thread-safe: use one instance from one thread only.
class CommInterface
{
public:
    virtual ~CommInterface() = default;

    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Reset the link / discard pending data (device clear on GPIB,
    // buffer flush on serial).
    virtual bool clear(QString *err = nullptr) = 0;

    // Write one command/message (line termination appended as needed).
    virtual bool write(const QByteArray &data, QString *err = nullptr) = 0;

    // Read one response message, trailing line terminators trimmed.
    // timeoutMs bounds how long to wait for the (start of the) response —
    // raise it for queries that block while the instrument measures.
    virtual bool read(QByteArray &out, int maxLen = 65536, QString *err = nullptr,
                      int timeoutMs = 3000) = 0;

    bool query(const QByteArray &cmd, QByteArray &resp, int maxLen = 65536,
               QString *err = nullptr, int timeoutMs = 3000)
    {
        if (!write(cmd, err))
            return false;
        return read(resp, maxLen, err, timeoutMs);
    }
};

#endif // COMM_INTERFACE_H
