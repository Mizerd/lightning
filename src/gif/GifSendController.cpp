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

QString GifSendController::safeFilename(const gif::GifResult &r, const QString &ext)
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
    const QString suffix = ext.isEmpty() ? QStringLiteral("gif") : ext;
    return id.isEmpty()
        ? QStringLiteral("animated.") + suffix
        : provider + QLatin1Char('-') + id + QLatin1Char('.') + suffix;
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
    if (!m_client || pending.roomId.isEmpty()) {
        Q_EMIT sendFailed(QStringLiteral("unavailable"), pending.isThread);
        return;
    }
    // A client-local starred GIF is a stored file, never a provider URL —
    // route it to the synchronous local-bytes send path instead of the
    // download pipeline below.
    if (pending.result.provider == QLatin1String("local")) {
        startLocal(std::move(pending));
        return;
    }
    if (pending.result.gifUrl.isEmpty()) {
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

void GifSendController::startLocal(Pending pending)
{
    if (pending.result.id.isEmpty() || !m_localGifReader) {
        Q_EMIT sendFailed(QStringLiteral("unavailable"), pending.isThread);
        return;
    }
    Q_EMIT sendStarted(pending.isThread);
    // Read fresh from disk and re-validate — never trusted from the
    // snapshot captured at activation: the file may have been unstarred or
    // caps refuse rather than evict, so the store never removes a file on
    // its own — but the user can have unstarred it since choosing it.
    const QByteArray bytes = m_localGifReader(pending.result.id);
    // 2026-08 media round: the general raster validator, not the GIF-only one — a saved
    // chat image can be GIF/PNG/JPEG/WebP now. `v.mime`/`v.ext` are the
    // TRUTH the bytes decided, never a guess or whatever the snapshot
    // claimed; a GIF still gets exactly its existing image/gif + animation
    // handling (Rust re-sniffs and only marks a payload animated for
    // image/gif — see rust/src/rooms.rs), and nothing here re-encodes the
    // bytes in any way.
    const gif::RasterByteValidation v = gif::validateRasterBytes(bytes);
    if (!v.ok) {
        Q_EMIT sendFailed(bytes.isEmpty() ? QStringLiteral("unavailable")
                                          : v.category,
                          pending.isThread);
        return;
    }
    if (pending.isThread && pending.rootId.isEmpty()) {
        // Never attempted — a captured thread destination with no root id
        // is an unavailable target, not a send that was tried and failed.
        Q_EMIT sendFailed(QStringLiteral("unavailable"), pending.isThread);
        return;
    }
    const QString filename = safeFilename(pending.result, v.ext);
    const quint64 sendOp = pending.isThread
        ? m_client->sendThreadAttachmentBytes(
              pending.roomId, pending.rootId, bytes, filename,
              v.mime, v.width, v.height)
        : m_client->sendAttachmentBytes(
              pending.roomId, bytes, filename, v.mime,
              v.width, v.height);
    if (sendOp == 0) {
        Q_EMIT sendFailed(QStringLiteral("send_failed"), pending.isThread);
        return;
    }
    // Deliberately NOT recorded into Recents: Recents is a global (not
    // account-scoped) store, while starred-GIF bytes live under the
    // account's own directory. Recording a "local:<hash>" reference there
    // would let a hint of one account's starred content leak into another
    // account's picker after switching on the same machine (the bytes stay
    // unreachable, but the reference itself should not cross accounts).
    // The item is already visible in the Saved tab, which the local-saved
    // store keeps account-scoped.
    Q_EMIT sendSucceeded(pending.isThread);
}

void GifSendController::cancelAll()
{
    if (m_pending.isEmpty())
        return;
    m_pending.clear();
    Q_EMIT activeCountChanged();
}
