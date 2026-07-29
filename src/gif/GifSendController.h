#pragma once

#include "gif/GifResponseParser.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantMap>

class MatrixClient;
class GifRecentModel;

// v0.6.1: sends a chosen provider GIF as real Matrix media (app.gifSend).
// It captures the exact destination (room or thread + ids) at selection time,
// downloads + validates the GIF through the hardened Rust path, then hands the
// bytes to the existing SDK attachment pipeline for the CAPTURED destination —
// so a room/thread switch during the download can never reroute the send, and
// a thread GIF always lands as a real m.thread reply (never an ordinary room
// message). On a successful handoff the GIF is recorded in Recents; the SDK
// local echo then owns upload/send state (and its own Retry). Never sends a
// bare provider URL, never renames non-GIF bytes.
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
    // Authoritative de-duplication backstop: true if an IDENTICAL send
    // (same destination + same provider-qualified GIF) is already in
    // flight. GifPicker.qml's own one-shot `activated` latch is the first
    // line of defense and closes the common case (a second click/Return
    // landing while the popup is still visually closing), but that latch
    // is QML-local, per-picker-instance state — it says nothing about any
    // OTHER caller of sendToRoom()/sendToThread() (a future call site, a
    // test, or two independently-instantiated pickers racing on the same
    // shared GifSearchController). This check is what actually holds
    // regardless of caller: two calls for the same target+GIF while the
    // first is still downloading/sending collapse into one network
    // operation. It does NOT block a genuine repeat send after the first
    // one has already resolved (succeeded or failed) — that is normal user
    // intent, not a duplicate activation.
    bool hasIdenticalPending(const Pending &candidate) const;
    void onGifDownloadFinished(quint64 opId, bool ok, const QByteArray &bytes,
                               const QString &mime, int width, int height,
                               qint64 size, const QString &category);
    static QString safeFilename(const gif::GifResult &r);

    MatrixClient *m_client = nullptr;
    GifRecentModel *m_recent = nullptr;
    QHash<quint64, Pending> m_pending; // download op id -> captured destination
};
