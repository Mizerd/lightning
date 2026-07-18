#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

// v0.6.1: async transport the GIF controller uses to reach a provider. Kept
// abstract so the controller is unit-testable with a fake transport (canned
// responses) and never needs the Rust backend or a network in tests. The real
// implementation (MatrixGifTransport) routes through the hardened Rust
// safe-get. Callers pass a URL that carries the provider API key — it is
// secret and must never be logged.
class GifTransport : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~GifTransport() override = default;

    // False when no backend can service GIF requests (e.g. mock/http backend).
    virtual bool available() const = 0;

    // Issue a GET. Returns a non-zero op id whose result arrives later via
    // finished(); returns 0 when unavailable so the caller shows MissingKey /
    // Offline rather than spinning.
    virtual quint64 get(const QString &url) = 0;

Q_SIGNALS:
    // `body` is the bounded provider JSON on success (empty on failure);
    // `category` is a coarse safe state (ok/rate_limited/provider_error/
    // timeout/network/too_large/blocked). No URL or key is ever carried.
    void finished(quint64 opId, bool ok, int httpStatus, const QByteArray &body,
                  const QString &category);
};
