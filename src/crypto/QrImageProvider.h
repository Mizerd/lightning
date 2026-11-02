#pragma once

#include <QByteArray>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

// Show-QR verification rendering. Lightning displays a verification QR for
// another device and never scans one, so `m.qr_code.scan.v1` is not
// advertised. The Rust bridge hands over only the module grid, never the
// payload (cross-signing key material and the flow's shared secret).
//
// Memory-only and single-slot (one verification flow at a time). Cleared when
// the flow ends, is cancelled or reset, or the session goes away. Nothing is
// written to disk or logged. Clearing is not a secure erase (see clear()).
class QrCodeStore
{
public:
    bool setCode(const QString &token, int modules, const QByteArray &bits);
    void clear();

    QByteArray gridFor(const QString &token, int *modules) const;

    static constexpr int kMaxModules = 200;

private:
    mutable QMutex m_mutex;
    QString m_token;
    int m_modules = 0;
    QByteArray m_bits;
};

class QrImageProvider : public QQuickImageProvider
{
public:
    explicit QrImageProvider(QrCodeStore *store);

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

    static constexpr int kQuietZoneModules = 4;

private:
    QrCodeStore *m_store;
};
