#include "app/ForwardController.h"

#include <QDateTime>

#include <QBuffer>

#include <cstring>
#include <QImageReader>
#include <QSize>

#include "matrix/MatrixClient.h"
#include "media/ImageFormatSupport.h"
#include "media/MediaBridge.h"

ForwardController::ForwardController(QObject *parent)
    : QObject(parent)
{
}

void ForwardController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        // Logout, including an account switch, invalidates any forward.
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            // Notify too, or an open picker keeps showing the previous
            // account's decrypted preview.
            resetToIdle();
            m_dispatchedSends.clear();
            Q_EMIT changed();
        });
        // Connected here, since it is a client signal and must survive a
        // second setClient().
        connect(m_client, &MatrixClient::attachmentQueueFinished, this,
                &ForwardController::onAttachmentQueueFinished);
    }
}

void ForwardController::setMediaBridge(MediaBridge *bridge)
{
    if (m_mediaBridge == bridge)
        return;
    if (m_mediaBridge)
        m_mediaBridge->disconnect(this);
    m_mediaBridge = bridge;
    if (m_mediaBridge) {
        connect(m_mediaBridge, &MediaBridge::mediaBytesForStar, this,
                &ForwardController::onMediaBytesForStar);
    }
}

bool ForwardController::snapshotIsMedia(const QVariantMap &snapshot)
{
    return snapshot.value(QStringLiteral("isImage")).toBool()
        || snapshot.value(QStringLiteral("isVideo")).toBool()
        || snapshot.value(QStringLiteral("isAudio")).toBool()
        || snapshot.value(QStringLiteral("isSticker")).toBool()
        || snapshot.value(QStringLiteral("isFile")).toBool();
}

QString ForwardController::buildPreview(const QVariantMap &snapshot)
{
    const QString sender =
        snapshot.value(QStringLiteral("senderDisplayName")).toString();

    QString kind;
    // Stickers forward as ordinary image attachments.
    if (snapshot.value(QStringLiteral("isImage")).toBool())
        kind = tr("Photo");
    else if (snapshot.value(QStringLiteral("isVideo")).toBool())
        kind = tr("Video");
    else if (snapshot.value(QStringLiteral("isAudio")).toBool())
        kind = snapshot.value(QStringLiteral("mediaIsVoice")).toBool()
                   ? tr("Voice message") : tr("Audio");
    else if (snapshot.value(QStringLiteral("isSticker")).toBool())
        kind = tr("Sticker");
    else if (snapshot.value(QStringLiteral("isFile")).toBool())
        kind = tr("File");

    if (!kind.isEmpty()) {
        const QString filename =
            snapshot.value(QStringLiteral("mediaFilename")).toString().trimmed();
        if (!filename.isEmpty())
            kind = tr("%1 (%2)").arg(kind, filename);
    } else {
        // A one-line, capped caption for the dialog.
        QString body =
            snapshot.value(QStringLiteral("body")).toString().trimmed();
        body.replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (body.size() > 140) {
            body.truncate(140);
            body += QStringLiteral("…");
        }
        kind = body;
    }

    return sender.isEmpty() ? kind : tr("%1 — %2").arg(sender, kind);
}

void ForwardController::begin(const QString &sourceRoomId,
                              const QString &sourceEventId,
                              const QVariantMap &snapshot)
{
    resetToIdle();

    if (!m_client || sourceRoomId.isEmpty() || sourceEventId.isEmpty()) {
        setError(tr("This message can't be forwarded."));
        return;
    }

    // Nothing here is safe to re-send. The menu already hides the action for
    // these rows; this is defense in depth.
    if (snapshot.value(QStringLiteral("redacted")).toBool()
        || snapshot.value(QStringLiteral("isLocalEcho")).toBool()
        || snapshot.value(QStringLiteral("undecryptable")).toBool()
        || snapshot.value(QStringLiteral("isVirtual")).toBool()) {
        setError(tr("This message can't be forwarded."));
        return;
    }

    const bool isMedia = snapshotIsMedia(snapshot);
    if (isMedia) {
        if (snapshot.value(QStringLiteral("mediaKey")).toString().isEmpty()) {
            setError(tr("This message can't be forwarded."));
            return;
        }
    } else if (snapshot.value(QStringLiteral("body")).toString().trimmed().isEmpty()) {
        setError(tr("This message can't be forwarded."));
        return;
    }

    m_sourceRoomId = sourceRoomId;
    m_sourceEventId = sourceEventId;
    m_snapshot = snapshot;
    m_previewText = buildPreview(snapshot);
    m_error.clear();
    m_busy = false;
    m_active = true;
    Q_EMIT changed();
}

void ForwardController::cancel()
{
    resetToIdle();
    Q_EMIT changed();
}

// ── Multi-message, multi-destination forwarding ─────────────────────────

void ForwardController::beginSelecting(const QString &sourceRoomId)
{
    m_selecting = true;
    m_sourceRoomId = sourceRoomId;
    m_selectedIds.clear();
    m_selectionSnapshots.clear();
    m_selectionActive = false;
    m_failures.clear();
    m_failedPairs.clear();
    m_progressDone = 0;
    m_progressTotal = 0;
    m_error.clear();
    Q_EMIT changed();
}

void ForwardController::cancelSelecting()
{
    if (!m_selecting && m_selectedIds.isEmpty())
        return;
    m_selecting = false;
    m_selectedIds.clear();
    m_selectionSnapshots.clear();
    m_selectionActive = false;
    Q_EMIT changed();
}

bool ForwardController::isSelected(const QString &eventId) const
{
    return m_selectedIds.contains(eventId);
}

void ForwardController::toggleSelected(const QString &eventId,
                                       const QVariantMap &snapshot)
{
    if (eventId.isEmpty())
        return;
    const int at = m_selectedIds.indexOf(eventId);
    if (at >= 0) {
        m_selectedIds.removeAt(at);
        m_selectionSnapshots.removeAt(at);
    } else {
        // Bounds how much one gesture can send.
        constexpr int kMaxSelected = 50;
        if (m_selectedIds.size() >= kMaxSelected) {
            setError(tr("You can forward up to %1 messages at once.")
                         .arg(kMaxSelected));
            return;
        }
        QVariantMap captured = snapshot;
        captured.insert(QStringLiteral("eventId"), eventId);
        m_selectedIds.append(eventId);
        m_selectionSnapshots.append(captured);
        m_error.clear();
    }
    m_selectionActive = !m_selectionSnapshots.isEmpty();
    Q_EMIT changed();
}

void ForwardController::beginSelection(const QString &sourceRoomId,
                                       const QVariantList &snapshots)
{
    resetToIdle();
    m_failures.clear();
    m_failedPairs.clear();
    m_queue.clear();
    m_progressDone = 0;
    m_progressTotal = 0;
    m_sourceRoomId = sourceRoomId;
    m_selectionSnapshots = snapshots;
    m_selectionActive = !snapshots.isEmpty() && !sourceRoomId.isEmpty();
    if (!m_selectionActive)
        setError(tr("Nothing was selected to forward."));
    Q_EMIT changed();
}

void ForwardController::setForwardMode(const QString &mode)
{
    // Unknown values fall back to "content", which discloses nothing.
    m_mode = (mode == QLatin1String("context")) ? mode
                                                : QStringLiteral("content");
    Q_EMIT changed();
}

/// Plain-text attribution for "context" mode. Deliberately not a permalink:
/// the recipient may not be in the source room.
QString ForwardController::contextPrefixFor(const QVariantMap &snapshot) const
{
    const QString sender =
        snapshot.value(QStringLiteral("senderName")).toString();
    const QString room = snapshot.value(QStringLiteral("sourceRoomName"))
                             .toString();
    const qint64 ts = snapshot.value(QStringLiteral("timestampMs")).toLongLong();
    QStringList bits;
    if (!sender.isEmpty())
        bits << sender;
    if (!room.isEmpty())
        bits << tr("in %1").arg(room);
    if (ts > 0) {
        bits << QDateTime::fromMSecsSinceEpoch(ts)
                    .toLocalTime()
                    .toString(QStringLiteral("d MMM yyyy hh:mm"));
    }
    if (bits.isEmpty())
        return {};
    return tr("Forwarded from %1").arg(bits.join(QStringLiteral(" · ")))
        + QLatin1Char('\n');
}

void ForwardController::sendSelection(const QVariantList &targets)
{
    if (!m_selectionActive || !m_client || targets.isEmpty())
        return;
    m_queue.clear();
    m_failures.clear();
    m_failedPairs.clear();
    m_progressDone = 0;
    // Each (message, destination) pair succeeds or fails on its own.
    for (const QVariant &snapValue : m_selectionSnapshots) {
        const QVariantMap snapshot = snapValue.toMap();
        for (const QVariant &targetValue : targets) {
            const QVariantMap target = targetValue.toMap();
            const QString roomId =
                target.value(QStringLiteral("roomId")).toString();
            if (roomId.isEmpty())
                continue;
            m_queue.append(Pending{
                snapshot,
                snapshot.value(QStringLiteral("eventId")).toString(),
                roomId,
                target.value(QStringLiteral("threadRootId")).toString(),
            });
        }
    }
    m_progressTotal = int(m_queue.size());
    m_busy = m_progressTotal > 0;
    Q_EMIT changed();
    pumpQueue();
}

void ForwardController::retryFailures()
{
    if (m_failedPairs.isEmpty() || !m_client)
        return;
    m_queue = m_failedPairs;
    m_failedPairs.clear();
    m_failures.clear();
    m_progressDone = 0;
    m_progressTotal = int(m_queue.size());
    m_busy = true;
    Q_EMIT changed();
    pumpQueue();
}

void ForwardController::notePairResult(const Pending &pair, bool ok,
                                       const QString &message)
{
    ++m_progressDone;
    if (!ok) {
        m_failedPairs.append(pair);
        m_failures.append(QVariantMap{
            { QStringLiteral("roomId"), pair.targetRoomId },
            { QStringLiteral("eventId"), pair.eventId },
            { QStringLiteral("message"), message },
        });
    }
    if (m_progressDone >= m_progressTotal)
        m_busy = false;
    Q_EMIT changed();
}

void ForwardController::pumpQueue()
{
    if (m_pumpBusy)
        return;
    while (!m_queue.isEmpty()) {
        const Pending pair = m_queue.takeFirst();
        if (!m_client) {
            notePairResult(pair, false, tr("Not signed in."));
            continue;
        }
        // Text only: concurrent fetch-then-upload media sends would be
        // unbounded. Attachments are reported as failures with a reason.
        if (snapshotIsMedia(pair.snapshot)) {
            notePairResult(pair, false,
                           tr("Attachments can only be forwarded one at a "
                              "time — use Forward on the message itself."));
            continue;
        }
        QString body = pair.snapshot.value(QStringLiteral("body")).toString();
        if (body.isEmpty()) {
            notePairResult(pair, false, tr("Nothing to send."));
            continue;
        }
        if (m_mode == QLatin1String("context"))
            body = contextPrefixFor(pair.snapshot) + body;

        if (!pair.threadRootId.isEmpty()) {
            // A thread in the target room, not the source's relation.
            m_client->sendThreadReplyTo(pair.targetRoomId, pair.threadRootId,
                                        QString(), body);
        } else {
            m_client->sendTextMessage(pair.targetRoomId, body);
        }
        // Text sends are fire-and-forget; dispatched counts as done.
        notePairResult(pair, true, QString());
    }
}

void ForwardController::forwardTo(const QString &targetRoomId)
{
    if (!m_active || m_busy || targetRoomId.isEmpty() || !m_client)
        return;

    m_error.clear();
    m_busy = true;
    Q_EMIT changed();

    if (!snapshotIsMedia(m_snapshot)) {
        // formatted_body is never read: its pills point into the source
        // room. The shared send path renders Markdown, so the copy is not
        // byte-faithful; do not "fix" that by reusing formatted_body.
        // sendTextMessage attaches no relation.
        const QString body = m_snapshot.value(QStringLiteral("body")).toString();
        m_client->sendTextMessage(targetRoomId, body);
        // No op id for text sends; reaching here is "dispatched".
        const QString target = targetRoomId;
        resetToIdle();
        Q_EMIT changed();
        Q_EMIT forwarded(target);
        return;
    }

    // Media: fetch fresh bytes through the decrypting save/star path.
    if (!m_mediaBridge) {
        m_busy = false;
        setError(tr("Media isn't available right now."));
        return;
    }
    ++m_generation;
    m_pendingGeneration = m_generation;
    m_pendingMediaKey = m_snapshot.value(QStringLiteral("mediaKey")).toString();
    m_pendingTargetRoomId = targetRoomId;
    m_mediaBridge->fetchFullForStar(m_pendingMediaKey);
}

void ForwardController::onMediaBytesForStar(const QString &mediaKey, bool ok,
                                            const QByteArray &bytes,
                                            const QString &category)
{
    if (m_pendingMediaKey.isEmpty() || mediaKey != m_pendingMediaKey)
        return;

    const quint64 generation = m_pendingGeneration;
    const QString targetRoomId = m_pendingTargetRoomId;
    m_pendingMediaKey.clear();
    m_pendingTargetRoomId.clear();

    // The forward was cancelled or replaced while the fetch was outstanding.
    if (generation != m_generation)
        return;

    if (!ok || bytes.isEmpty()) {
        m_busy = false;
        setError(category == QLatin1String("unavailable")
                     ? tr("This media is no longer available.")
                     : tr("Couldn't download this media."));
        return;
    }

    // The source's filename and MIME were chosen by its sender; they are
    // re-originated under this account, so sanitize rather than copy.
    QString filename = sanitizedForwardFilename(
        m_snapshot.value(QStringLiteral("mediaFilename")).toString());
    QString mime = m_snapshot.value(QStringLiteral("mediaMimetype")).toString();
    int width = m_snapshot.value(QStringLiteral("mediaWidth")).toInt();
    int height = m_snapshot.value(QStringLiteral("mediaHeight")).toInt();

    // Correct image metadata from the bytes. The type comes from magic bytes
    // (the table shared with rooms::sniff_image_mime), not from
    // QImageReader::format(), which depends on which image plugins a package
    // ships. Identification is not decodability: a format this build cannot
    // display still forwards. Not gif::validateRasterBytes, whose size caps
    // would refuse ordinary large photos. The Rust send path re-checks magic
    // bytes regardless. Video, audio and files keep their declared type.
    const QString identified = lightning::imagefmt::sniffRasterMime(bytes);

    if (!identified.isEmpty()) {
        mime = identified;
        // Dimensions only; a missing plugin just keeps the declared size.
        QByteArray probe = bytes;
        QBuffer buffer(&probe);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        const QSize size = reader.size();
        if (size.isValid() && size.width() > 0 && size.height() > 0) {
            width = size.width();
            height = size.height();
        }
    }

    // Claims to be an image but is not a known raster format (SVG included,
    // which must never enter a media path): refuse.
    if (identified.isEmpty()
        && mime.startsWith(QLatin1String("image/"))) {
        m_busy = false;
        setError(tr("This media can't be forwarded."));
        return;
    }

    // Prefer the room-scoped send: the timeline-scoped one refuses any room
    // but the open one, and the target is usually not open.
    const quint64 opId = m_client->supportsRoomScopedAttachmentSend()
        ? m_client->sendAttachmentBytesToRoom(
              targetRoomId, bytes,
              filename.isEmpty() ? QStringLiteral("forwarded") : filename,
              mime, width, height)
        : m_client->sendAttachmentBytes(
              targetRoomId, bytes,
              filename.isEmpty() ? QStringLiteral("forwarded") : filename,
              mime, width, height);
    if (opId == 0) {
        m_busy = false;
        setError(tr("Couldn't forward this message."));
        return;
    }

    // The send is a direct upload with no local echo in the target, so track
    // it to report a server refusal. One entry per send, so a second forward
    // cannot hide the first one's failure.
    m_dispatchedSends.insert(opId, targetRoomId);
    resetToIdle();
    Q_EMIT changed();
    Q_EMIT forwarded(targetRoomId);
}

// Keeps the leaf name only and strips leading dots, so a forward cannot carry
// path structure or produce a hidden file.
QString ForwardController::sanitizedForwardFilename(const QString &raw)
{
    QString name = raw;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        name = name.mid(slash + 1);
    // Strip control characters and whitespace before the dot strip, or
    // " .bashrc" would survive it.
    name.removeIf([](QChar c) { return c.category() == QChar::Other_Control; });
    name = name.trimmed();
    while (name.startsWith(QLatin1Char('.')))
        name.remove(0, 1);
    name = name.trimmed();
    if (name.size() > 128)
        name = name.left(128);
    return name;
}

void ForwardController::onAttachmentQueueFinished(quint64 opId,
                                                 const QString &roomId,
                                                 bool ok,
                                                 const QString &category)
{
    const auto entry = m_dispatchedSends.constFind(opId);
    if (entry == m_dispatchedSends.constEnd())
        return;
    const QString target = entry.value();
    m_dispatchedSends.erase(entry);
    if (ok)
        return;
    Q_UNUSED(category);
    Q_EMIT forwardFailed(roomId.isEmpty() ? target : roomId,
                         tr("That message could not be forwarded."));
}

void ForwardController::resetToIdle()
{
    // m_dispatchedSends is kept: those sends outlive the picker and are
    // cleared only on sign-out.
    ++m_generation;
    m_pendingGeneration = 0;
    m_pendingMediaKey.clear();
    m_pendingTargetRoomId.clear();
    m_active = false;
    m_busy = false;
    m_error.clear();
    m_sourceRoomId.clear();
    m_sourceEventId.clear();
    m_previewText.clear();
    m_snapshot.clear();
}

void ForwardController::setError(const QString &message)
{
    m_error = message;
    Q_EMIT changed();
}
