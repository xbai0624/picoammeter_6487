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

    // Read one IEEE-488.2 definite-length block response
    // ('#' + digit-count + length + binary payload + CR/LF) and return the
    // payload. Required for binary data formats, whose bytes may contain
    // newline characters that would truncate a line-oriented read().
    virtual bool readBlock(QByteArray &payload, QString *err = nullptr,
                           int timeoutMs = 3000) = 0;

    bool queryBlock(const QByteArray &cmd, QByteArray &payload,
                    QString *err = nullptr, int timeoutMs = 3000)
    {
        if (!write(cmd, err))
            return false;
        return readBlock(payload, err, timeoutMs);
    }

protected:
    // For backends that receive the whole message in one piece (GPIB, mock):
    // strip the '#nlll...' header and trailing terminators in place.
    static bool stripBlockHeader(QByteArray &msg, QString *err)
    {
        const int hash = msg.indexOf('#');
        if (hash < 0 || hash + 2 > msg.size()) {
            if (err)
                *err = QStringLiteral("malformed binary block (no '#' header)");
            return false;
        }
        const int nDigits = msg[hash + 1] - '0';
        if (nDigits < 1 || nDigits > 9 || hash + 2 + nDigits > msg.size()) {
            if (err)
                *err = QStringLiteral("malformed binary block header");
            return false;
        }
        bool ok = false;
        const int len = msg.mid(hash + 2, nDigits).toInt(&ok);
        const int dataStart = hash + 2 + nDigits;
        if (!ok || dataStart + len > msg.size()) {
            if (err)
                *err = QStringLiteral("binary block shorter than its declared length");
            return false;
        }
        msg = msg.mid(dataStart, len);
        return true;
    }
};

#endif // COMM_INTERFACE_H
