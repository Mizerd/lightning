#pragma once

#include <QByteArray>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

// Show-QR verification rendering.
//
// Lightning DISPLAYS a verification QR code for another device to scan; it
// never scans one (no camera), so `m.qr_code.scan.v1` is never advertised.
// The Rust bridge hands the C++ side the code's MODULE GRID only — never
// the payload, which encodes cross-signing key material and the flow's
// shared secret.
//
// The grid is memory-only and single-slot: there is one verification flow
// at a time, so storing a second code replaces the first. It is cleared
// whenever the flow ends, is cancelled, is reset, or the session goes away,
// so the store stops serving a code once its flow is over and none can leak
// into the next account's UI. Nothing is written to disk and nothing is
// logged. Clearing is not a secure erase — see QrCodeStore::clear.
class QrCodeStore
{
public:
    // Replace the stored code. `token` is an opaque per-code key used in the
    // provider URL; flow ids never appear in a URL. Returns false (and
    // stores nothing) when the geometry does not describe a square grid.
    bool setCode(const QString &token, int modules, const QByteArray &bits);
    void clear();

    // Fetch the grid for `token`. Returns an empty QByteArray and leaves
    // `modules` at 0 when the token does not match the stored code — a
    // stale URL must render nothing rather than the current flow's code.
    QByteArray gridFor(const QString &token, int *modules) const;

    // Hard bound on the module count accepted. QR version 40 — far above
    // anything a verification payload needs — is 177 modules per side.
    static constexpr int kMaxModules = 200;

private:
    mutable QMutex m_mutex;
    QString m_token;
    int m_modules = 0;
    QByteArray m_bits;
};

// Serves the stored grid to QML under image://lightning-qr/<token>.
//
// Rendered black-on-white with a 4-module quiet zone regardless of the
// active theme: a QR code has to be scannable by another device's camera,
// which is a physical constraint, not a styling choice. Scaling is
// nearest-neighbour so module edges stay hard.
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
