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

    virtual quint64 get(const QString &url) = 0;

    virtual quint64 download(const QString &url)
    {
        Q_UNUSED(url);
        return 0;
    }

Q_SIGNALS:
    void finished(quint64 opId, bool ok, int httpStatus, const QByteArray &body,
                  const QString &category);
    void downloadFinished(quint64 opId, bool ok, const QByteArray &bytes,
                          const QString &category);
    void sessionEnded();
};
