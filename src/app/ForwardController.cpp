#include "app/ForwardController.h"

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
        // Logout (or an account switch, which also emits loggedOut — see
        // MatrixClient::detachSession) invalidates any forward in progress:
        // its source event, its target room, and any outstanding media
        // fetch all belonged to the account that just went away.
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            // Reset AND notify. resetToIdle() alone leaves every property
            // binding stale, so an open picker would stay on screen over
            // the login screen still showing the previous account's
            // decrypted preview text.
            resetToIdle();
            // Sends dispatched by the previous account must not report
            // into the next one.
            m_dispatchedSends.clear();
            Q_EMIT changed();
        });
        // Belongs HERE, not in setMediaBridge(): it is a CLIENT signal, and
        // guarding it on the media bridge instead made the N1 failure
        // reporting depend on setClient() happening first — an ordering
        // nothing in this API states — and silently dropped it entirely on
        // a second setClient(), which disconnects everything and would not
        // have re-established it.
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
    // Stickers have no dedicated Lightning send path — they forward as an ordinary image attachment, so
    // the preview honestly calls it "Sticker" while the actual send below
    // takes the exact same branch as an image.
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
        // Text-like (text/emote/notice, collapsed at send time per D4) —
        // this is a dialog caption, not the message itself, so it is
        // flattened to one line and capped.
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

    // D7: refuse redacted / local-echo / undecryptable content up front —
    // none of it has anything safe to re-send. The menu item that calls
    // begin() already withholds itself for these rows (MessageDelegate.qml);
    // this is defense in depth, not the only gate.
    if (snapshot.value(QStringLiteral("redacted")).toBool()
        || snapshot.value(QStringLiteral("isLocalEcho")).toBool()
        || snapshot.value(QStringLiteral("undecryptable")).toBool()
        || snapshot.value(QStringLiteral("isVirtual")).toBool()) {
        setError(tr("This message can't be forwarded."));
        return;
    }

    const bool isMedia = snapshotIsMedia(snapshot);
    if (isMedia) {
        // No usable source bytes (e.g. the row's media source became
        // unavailable between load and click) — refuse rather than send an
        // empty/broken attachment.
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

void ForwardController::forwardTo(const QString &targetRoomId)
{
    if (!m_active || m_busy || targetRoomId.isEmpty() || !m_client)
        return;

    m_error.clear();
    m_busy = true;
    Q_EMIT changed();

    if (!snapshotIsMedia(m_snapshot)) {
        // The SOURCE event's formatted_body is deliberately never read: its
        // pills and permalinks point into the source room and would be
        // misleading in the target.
        //
        // HONESTLY, though, the result is not a byte-faithful copy. This
        // rides the shared send path, which renders Markdown
        // (RoomMessageEventContent::text_markdown in rust/src/timeline.rs),
        // so a plain body containing "> quoting Bob" or "# 1" arrives
        // formatted. Receiving clients sanitize, so this is not an
        // injection — but do not describe forwarding as an exact copy, and
        // do not "fix" it by reintroducing the source's formatted_body.
        //
        // D5: sendTextMessage attaches no relation, so this can never
        // become a reply or an m.thread reply in the target room even
        // though the SOURCE event may have been one.
        const QString body = m_snapshot.value(QStringLiteral("body")).toString();
        m_client->sendTextMessage(targetRoomId, body);
        // No op id on this path (it never had one — same as the ordinary
        // composer send); reaching this line IS "dispatched" for D6.
        const QString target = targetRoomId;
        resetToIdle();
        Q_EMIT changed();
        Q_EMIT forwarded(target);
        return;
    }

    // Media (including a sticker forwarded as an image — see D1/scope):
    // re-fetch fresh bytes through the SAME decrypting path every other
    // save/star action uses (D1/D2). The snapshot never carries bytes,
    // only classification.
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
    // MediaBridge broadcasts this to every listener for every media key it
    // resolves (star, save, and now forward can all be outstanding at
    // once) — ignore anything that is not the exact key this generation is
    // waiting on.
    if (m_pendingMediaKey.isEmpty() || mediaKey != m_pendingMediaKey)
        return;

    const quint64 generation = m_pendingGeneration;
    const QString targetRoomId = m_pendingTargetRoomId;
    m_pendingMediaKey.clear();
    m_pendingTargetRoomId.clear();

    // begin()/cancel() ran again while this fetch was outstanding — this
    // answer belongs to a forward the user has already left. Never send it
    // into whatever the CURRENT forward's dialog now shows.
    if (generation != m_generation)
        return;

    if (!ok || bytes.isEmpty()) {
        m_busy = false;
        setError(category == QLatin1String("unavailable")
                     ? tr("This media is no longer available.")
                     : tr("Couldn't download this media."));
        return;
    }

    // The filename and MIME on the SOURCE event were chosen by whoever sent
    // it. Forwarding RE-ORIGINATES both under this account, so they are
    // sanitized rather than copied — the same standard the saved-media path
    // applies ("never a claimed MIME or file name").
    QString filename = sanitizedForwardFilename(
        m_snapshot.value(QStringLiteral("mediaFilename")).toString());
    QString mime = m_snapshot.value(QStringLiteral("mediaMimetype")).toString();
    int width = m_snapshot.value(QStringLiteral("mediaWidth")).toInt();
    int height = m_snapshot.value(QStringLiteral("mediaHeight")).toInt();

    // Correct the metadata from the BYTES where they can be identified.
    // QImageReader is a format+dimension probe with no policy of its own —
    // deliberately NOT gif::validateRasterBytes, which carries the saved-GIF
    // store's 4096px / 25 MiB caps and would refuse a 5K screenshot or a
    // large camera JPEG that Lightning displays and sends perfectly well.
    //
    // Refusal is NOT decided here. The send path already sniffs magic
    // Rust-side and rejects an `image/*` payload whose bytes disagree
    // (rooms::sniff_image_mime), so a mislabelled or SVG payload cannot be
    // uploaded regardless of what this block concludes. What this adds is
    // truthfulness: a forward re-originates the attachment under THIS
    // account, so it should not attest to a type or a shape it did not
    // check.
    //
    // Video, audio and arbitrary files keep their declared type. Lightning
    // cannot verify those containers here, and that is stated rather than
    // implied to be safe.
    // Identify from MAGIC BYTES, not from QImageReader::format(): that is
    // plugin-backed, and WebP lives in qtimageformats, which the packaged
    // DEB/RPM/AppImage builds did not carry at all until 2026-08-28. A build
    // without it would refuse ordinary WebP content as unidentifiable — the
    // exact class of defect this gate was corrected for once already, arriving
    // through a different door and invisible in the dev shell.
    //
    // The signatures now live in lightning::imagefmt::sniffRaster, ONE table
    // shared with the app-icon path and mirrored by rooms::sniff_image_mime,
    // so the C++ and Rust gates cannot disagree about what is acceptable. The
    // copy that used to sit here is gone rather than kept in sync by hand.
    //
    // IDENTIFICATION IS NOT DECODABILITY, and this call site deliberately does
    // not consult the decoder. A forward re-uploads the ORIGINAL BYTES with a
    // truthful type; drawing them is the receiving client's problem. So a
    // JPEG XL forwards correctly from a Windows build that cannot display it,
    // which is the honest outcome — refusing there would destroy a working
    // path to protect a preview nobody asked for.
    const QString identified = lightning::imagefmt::sniffRasterMime(bytes);

    if (!identified.isEmpty()) {
        mime = identified;
        // QImageReader is used ONLY to refine the shape, never to decide
        // acceptability — so a missing plugin costs at most the dimensions,
        // which the source event already declared.
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

    // Claims to be an image but cannot be identified as one of the raster
    // formats Lightning sends — SVG included, which must never enter a media
    // path. Refuse rather than re-upload an unverifiable payload under this
    // account's name. This is IDENTIFICATION only: a 5K screenshot or a
    // 40 MiB camera JPEG identifies fine and forwards normally.
    if (identified.isEmpty()
        && mime.startsWith(QLatin1String("image/"))) {
        m_busy = false;
        setError(tr("This media can't be forwarded."));
        return;
    }

    // The room-SCOPED send, when the backend has one. The timeline-scoped
    // variant refuses any room but the open one, and a forward's target is
    // by definition a room the user is not looking at — so using it here
    // made every real media forward fail while passing every test whose
    // fake accepted any room id.
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

    // The picker closes and the app navigates now, but the send is NOT
    // done: Room::send_attachment is a direct upload, not the send queue,
    // and the target timeline was not open, so there is no local echo to
    // stand in for it. A server refusal (no permission, rate limit, over
    // m.upload.size) would otherwise be completely silent — the user would
    // arrive in the target room, see nothing, and believe it worked.
    // A HASH, not one slot: forwarding two images in quick succession is
    // ordinary, and overwriting would make the first one's rejection
    // silent again — the exact failure this tracking exists to prevent.
    m_dispatchedSends.insert(opId, targetRoomId);
    resetToIdle();
    Q_EMIT changed();
    Q_EMIT forwarded(targetRoomId);
}

// A forwarded attachment's name is re-originated under THIS account, so it
// must not carry path structure a receiving client could act on when saving.
// Keeps the leaf only, drops anything that could traverse, and refuses a
// leading dot so a forward cannot silently produce a hidden file.
QString ForwardController::sanitizedForwardFilename(const QString &raw)
{
    QString name = raw;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        name = name.mid(slash + 1);
    // Control characters (NUL, newline, tab) must not reach the event's
    // filename. Removed BEFORE the dot strip, along with surrounding
    // whitespace: " .bashrc" does not start with '.', so trimming
    // afterwards would hand back exactly the hidden-file name this guard
    // exists to prevent.
    name.removeIf([](QChar c) { return c.category() == QChar::Other_Control; });
    name = name.trimmed();
    while (name.startsWith(QLatin1Char('.')))
        name.remove(0, 1);
    name = name.trimmed();
    // Bounded: a pathological name must not become the event body.
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
    // Carries the room it was aimed at so the surface can decide whether it
    // is still relevant — by the time this arrives the picker has closed
    // and the user has been navigated there, but they may have moved again.
    Q_EMIT forwardFailed(roomId.isEmpty() ? target : roomId,
                         tr("That message could not be forwarded."));
}

void ForwardController::resetToIdle()
{
    // NOT m_dispatchedSends: a dispatched send outlives the picker by
    // design, and its rejection must still be reportable. Those are dropped
    // only on sign-out, where there is no account left to report to.
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
