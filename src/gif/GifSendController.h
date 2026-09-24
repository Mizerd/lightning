#pragma once

#include "gif/GifResponseParser.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantMap>

#include <functional>

class MatrixClient;
class GifRecentModel;

// Sends a chosen GIF as real Matrix media (`app.gifSend`). The destination
// (room or thread) is captured at selection time, the GIF is downloaded and
// validated through the hardened Rust path, and the bytes go to the SDK
// attachment pipeline for that captured destination, so switching rooms
// mid-download cannot reroute the send and thread GIFs are always m.thread
// replies. A successful handoff is recorded in Recents; the SDK local echo
// owns send state and Retry. Never sends a bare provider URL.
class GifSendController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("GifSendController is exposed via app.gifSend")
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged)

public:
    enum State { Idle, Downloading, Sending, Sent, Failed, Cancelled };
    Q_ENUM(State)

    explicit GifSendController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setRecentModel(GifRecentModel *recent) { m_recent = recent; }
    // Reads a saved local GIF by content hash, fresh from disk at send time, so
    // a GIF removed after activation is refused. Empty means unavailable.
    using LocalGifReader = std::function<QByteArray(const QString &hash)>;
    void setLocalGifReader(LocalGifReader reader)
    { m_localGifReader = std::move(reader); }

    int activeCount() const { return m_pending.size(); }

    // Send `resultMap` (a GifResultModel role map) to a room or a thread.
    Q_INVOKABLE void sendToRoom(const QString &roomId,
                                const QVariantMap &resultMap);
    Q_INVOKABLE void sendToThread(const QString &roomId, const QString &rootId,
                                  const QVariantMap &resultMap);
    // Drop all in-flight sends (their downloads complete into nothing).
    Q_INVOKABLE void cancelAll();

Q_SIGNALS:
    void sendStarted(bool thread);
    void sendSucceeded(bool thread);
    void sendFailed(const QString &category, bool thread);
    void activeCountChanged();

private:
    struct Pending {
        QString roomId;
        bool isThread = false;
        QString rootId;
        gif::GifResult result;
    };

    void start(Pending pending);
    // Sends a client-local saved file. Synchronous end to end, so the in-flight
    // dedup below does not apply.
    void startLocal(Pending pending);
    // True when an identical send (same destination and GIF) is already in
    // flight. The picker's own activation latch is per instance; this holds for
    // every caller. A repeat after the first send resolved is not blocked.
    bool hasIdenticalPending(const Pending &candidate) const;
    void onGifDownloadFinished(quint64 opId, bool ok, const QByteArray &bytes,
                               const QString &mime, int width, int height,
                               qint64 size, const QString &category);
    // `ext` defaults to "gif" (the provider path is GIF-only; Rust re-checks).
    // The local path passes the byte-validated suffix so saved PNG/JPEG/WebP
    // files keep their real extension.
    static QString safeFilename(const gif::GifResult &r,
                                const QString &ext = QStringLiteral("gif"));

    MatrixClient *m_client = nullptr;
    GifRecentModel *m_recent = nullptr;
    LocalGifReader m_localGifReader;
    QHash<quint64, Pending> m_pending; // download op id -> captured destination
};
