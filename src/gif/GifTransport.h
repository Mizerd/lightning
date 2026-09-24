#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

// Async transport the GIF controller uses to reach a provider; abstract so
// tests can use canned responses. The real implementation (MatrixGifTransport)
// uses the hardened Rust safe-get. URLs carry the provider key and must never
// be logged.
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
