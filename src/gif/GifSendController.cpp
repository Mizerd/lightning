#include "gif/GifSendController.h"

#include "gif/GifRecentModel.h"
#include "gif/GifStoredModel.h"
#include "matrix/MatrixClient.h"

GifSendController::GifSendController(QObject *parent)
    : QObject(parent)
{
}

void GifSendController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::gifDownloadFinished, this,
                &GifSendController::onGifDownloadFinished);
        // Logout invalidates every captured destination.
        connect(m_client, &MatrixClient::loggedOut, this,
                &GifSendController::cancelAll);
    }
}

QString GifSendController::safeFilename(const gif::GifResult &r)
{
    QString id;
    for (const QChar c : r.id) {
        if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'))
            id.append(c);
        if (id.size() >= 40)
            break;
    }
    const QString provider = r.provider.isEmpty() ? QStringLiteral("gif")
                                                  : r.provider;
    return id.isEmpty() ? QStringLiteral("animated.gif")
                        : provider + QLatin1Char('-') + id + QStringLiteral(".gif");
}

void GifSendController::sendToRoom(const QString &roomId,
                                   const QVariantMap &resultMap)
{
    Pending p;
    p.roomId = roomId;
    p.isThread = false;
    p.result = GifStoredModel::fromVariantMap(resultMap);
    start(std::move(p));
}

void GifSendController::sendToThread(const QString &roomId,
                                     const QString &rootId,
                                     const QVariantMap &resultMap)
{
    Pending p;
    p.roomId = roomId;
    p.isThread = true;
    p.rootId = rootId;
    p.result = GifStoredModel::fromVariantMap(resultMap);
    start(std::move(p));
}

bool GifSendController::hasIdenticalPending(const Pending &candidate) const
{
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        const Pending &p = it.value();
        if (p.roomId == candidate.roomId && p.isThread == candidate.isThread
            && p.rootId == candidate.rootId
            && p.result.provider == candidate.result.provider
            && p.result.id == candidate.result.id)
            return true;
    }
    return false;
}

void GifSendController::start(Pending pending)
{
    if (!m_client || pending.roomId.isEmpty()
        || pending.result.gifUrl.isEmpty()) {
        Q_EMIT sendFailed(QStringLiteral("unavailable"), pending.isThread);
        return;
    }
    // Only a validated provider-CDN https .gif is ever downloaded/sent. Rust
    // re-checks the host and the GIF magic bytes.
    if (!gif::isSendableGifUrlForHosts(
            pending.result.gifUrl,
            { QStringLiteral(".giphy.com"), QStringLiteral(".klipy.com") })) {
        Q_EMIT sendFailed(QStringLiteral("unavailable"), pending.isThread);
        return;
    }
    // De-duplication backstop — see hasIdenticalPending's declaration
    // comment. Silently dropped, not surfaced as sendFailed: the first,
    // identical, still-in-flight request already owns this send, so
    // nothing has actually failed.
    if (hasIdenticalPending(pending))
        return;
    const quint64 opId = m_client->gifDownload(pending.result.gifUrl);
    if (opId == 0) {
        Q_EMIT sendFailed(QStringLiteral("unavailable"), pending.isThread);
        return;
    }
    m_pending.insert(opId, std::move(pending));
    Q_EMIT activeCountChanged();
    Q_EMIT sendStarted(m_pending.value(opId).isThread);
}

void GifSendController::onGifDownloadFinished(quint64 opId, bool ok,
                                              const QByteArray &bytes,
                                              const QString &mime, int width,
                                              int height, qint64 /*size*/,
                                              const QString &category)
{
    const auto it = m_pending.constFind(opId);
    if (it == m_pending.constEnd())
        return; // stale / cancelled / another consumer
    const Pending p = it.value();
    m_pending.erase(it);
    Q_EMIT activeCountChanged();

    if (!ok) {
        Q_EMIT sendFailed(category.isEmpty() ? QStringLiteral("failed")
                                             : category,
                          p.isThread);
        return;
    }
    // Rust guarantees image/gif; refuse anything else defensively.
    if (mime != QLatin1String("image/gif") || bytes.isEmpty()) {
        Q_EMIT sendFailed(QStringLiteral("not_a_gif"), p.isThread);
        return;
    }

    const QString filename = safeFilename(p.result);
    quint64 sendOp = 0;
    if (p.isThread) {
        if (!p.rootId.isEmpty())
            sendOp = m_client->sendThreadAttachmentBytes(
                p.roomId, p.rootId, bytes, filename, QStringLiteral("image/gif"),
                width, height);
    } else {
        sendOp = m_client->sendAttachmentBytes(
            p.roomId, bytes, filename, QStringLiteral("image/gif"), width,
            height);
    }
    if (sendOp == 0) {
        Q_EMIT sendFailed(QStringLiteral("send_failed"), p.isThread);
        return;
    }
    // Successful handoff → Recents (never on preview or a failed/cancelled
    // send). The SDK local echo now owns upload/send state and Retry.
    if (m_recent)
        m_recent->recordSent(p.result);
    Q_EMIT sendSucceeded(p.isThread);
}

void GifSendController::cancelAll()
{
    if (m_pending.isEmpty())
        return;
    m_pending.clear();
    Q_EMIT activeCountChanged();
}
