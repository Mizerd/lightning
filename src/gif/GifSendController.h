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

    Q_INVOKABLE void sendToRoom(const QString &roomId,
                                const QVariantMap &resultMap,
                                const QString &replyToEventId = QString());
    Q_INVOKABLE void sendToThread(const QString &roomId, const QString &rootId,
                                  const QVariantMap &resultMap,
                                  const QString &replyToEventId = QString());
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
        QString replyToEventId;
        gif::GifResult result;
    };

    void start(Pending pending);
    void startLocal(Pending pending);
    bool hasIdenticalPending(const Pending &candidate) const;
    void onGifDownloadFinished(quint64 opId, bool ok, const QByteArray &bytes,
                               const QString &mime, int width, int height,
                               qint64 size, const QString &category);
    static QString safeFilename(const gif::GifResult &r,
                                const QString &ext = QStringLiteral("gif"));

    MatrixClient *m_client = nullptr;
    GifRecentModel *m_recent = nullptr;
    LocalGifReader m_localGifReader;
    QHash<quint64, Pending> m_pending;
};
