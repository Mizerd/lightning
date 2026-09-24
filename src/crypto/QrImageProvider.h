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
    // Replace the stored code. `token` is an opaque per-code key used in the
    // provider URL; flow ids never appear in a URL. Returns false (and
    // stores nothing) when the geometry does not describe a square grid.
    bool setCode(const QString &token, int modules, const QByteArray &bits);
    void clear();

    // The grid for `token`, or empty with `modules` 0 when the token does not
    // match: a stale URL renders nothing.
    QByteArray gridFor(const QString &token, int *modules) const;

    // QR version 40, far beyond any verification payload, is 177 modules.
    static constexpr int kMaxModules = 200;

private:
    mutable QMutex m_mutex;
    QString m_token;
    int m_modules = 0;
    QByteArray m_bits;
};

// Serves the stored grid to QML under image://lightning-qr/<token>.
//
// Always black on white with a 4-module quiet zone, whatever the theme: it
// must scan. Nearest-neighbour scaling keeps module edges hard.
class QrImageProvider : public QQuickImageProvider
{
public:
    explicit QrImageProvider(QrCodeStore *store);

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

    // Quiet zone required by the QR specification, in modules.
    static constexpr int kQuietZoneModules = 4;

private:
    QrCodeStore *m_store; // not owned; outlives the QML engine
};
