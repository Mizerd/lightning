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

    // Download one provider-CDN GIF (a picker preview or still) through the
    // same hardened path as a send. Returns 0 when unavailable; the result
    // arrives via downloadFinished().
    virtual quint64 download(const QString &url)
    {
        Q_UNUSED(url);
        return 0;
    }

Q_SIGNALS:
    // `body` is the bounded provider JSON on success (empty on failure);
    // `category` is a coarse safe state (ok/rate_limited/provider_error/
    // timeout/network/too_large/blocked). No URL or key is ever carried.
    void finished(quint64 opId, bool ok, int httpStatus, const QByteArray &body,
                  const QString &category);
    // Result of download(): the bytes on success, else a coarse category.
    void downloadFinished(quint64 opId, bool ok, const QByteArray &bytes,
                          const QString &category);
    // The Matrix session ended; anything fetched for it should be dropped.
    void sessionEnded();
};
