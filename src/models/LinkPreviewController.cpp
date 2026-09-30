#include "models/LinkPreviewController.h"

#include "matrix/MatrixClient.h"
#include "models/LinkPreview.h"
#include "models/MessageHtml.h"

#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QUrl>

Q_LOGGING_CATEGORY(lcPreview, "lightning.timeline.linkpreview")

using matrix::link_preview::GifClass;

namespace {
const QString kDirectMedia = QStringLiteral("direct_media");
const QString kDirectVideo = QStringLiteral("direct_video");

// The link's last path segment, for a caption. Sender-chosen text: control
// and bidi-override characters are dropped so "a\u202Egnp.exe" cannot pose as
// another name, and it is clipped.
QString fileNameForUrl(const QString &url)
{
    QString leaf = QUrl(url).fileName(QUrl::FullyDecoded);
    QString out;
    out.reserve(leaf.size());
    for (const QChar c : std::as_const(leaf)) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7f || u == 0x061c
            || (u >= 0x200e && u <= 0x200f) || (u >= 0x2028 && u <= 0x202e)
            || (u >= 0x2066 && u <= 0x2069))
            continue;
        out.append(c);
    }
    out = out.trimmed();
    if (out.size() > 80) {
        // Never between the halves of a surrogate pair.
        qsizetype cut = 79;
        if (out.at(cut - 1).isHighSurrogate())
            --cut;
        out = out.left(cut) + QChar(0x2026);
    }
    return out;
}

// What a loaded entry is: "image", "video" or "".
QString mediaKindOf(const QVariantMap &fields)
{
    const QString kind = fields.value(QStringLiteral("previewKind")).toString();
    if (kind == kDirectMedia)
        return QStringLiteral("image");
    if (kind == kDirectVideo)
        return QStringLiteral("video");
    return {};
}

// Memory a preview's image text holds (QString is UTF-16).
qint64 heldBytesOf(const QVariantMap &fields)
{
    const QString src = fields.value(QStringLiteral("imageSource")).toString();
    return src.startsWith(QLatin1String("data:"))
        ? static_cast<qint64>(src.size()) * 2 : 0;
}
} // namespace

LinkPreviewController::LinkPreviewController(QObject *parent)
    : QObject(parent)
{
}

QString LinkPreviewController::linkifiedBody(const QString &body) const
{
    // Render path for a message without a formatted body, the counterpart of
    // MessageHtml::sanitize(). Emoji are enlarged by the same function so both
    // paths agree. linkifiedMessageHtml() escapes everything it does not build,
    // so markEmoji() receives safe input.
    return MessageHtml::markEmoji(
        matrix::link_preview::linkifiedMessageHtml(body));
}

QString LinkPreviewController::linkifiedTopic(const QString &topic) const
{
    return matrix::link_preview::linkifiedTopicHtml(topic);
}

void LinkPreviewController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    clear();
    if (m_client) {
        connect(m_client, &MatrixClient::urlPreviewFinished,
                this, &LinkPreviewController::onPreviewFinished);
        connect(m_client, &MatrixClient::loggedOut,
                this, &LinkPreviewController::onLoggedOut);
    }
    Q_EMIT supportedChanged();
}

bool LinkPreviewController::supported() const
{
    return m_client && m_client->supportsUrlPreview();
}

void LinkPreviewController::setAutoLoadUnencrypted(bool value)
{
    if (m_autoLoadUnencrypted == value)
        return;
    m_autoLoadUnencrypted = value;
    Q_EMIT policyChanged();
}

void LinkPreviewController::setAllowEncrypted(bool value)
{
    if (m_allowEncrypted == value)
        return;
    m_allowEncrypted = value;
    Q_EMIT policyChanged();
}

void LinkPreviewController::setInlineMedia(bool value)
{
    if (m_inlineMedia == value)
        return;
    m_inlineMedia = value;
    Q_EMIT inlineMediaChanged();
}

QString LinkPreviewController::mediaKeyForUrl(const QString &url)
{
    if (url.isEmpty())
        return {};
    return QStringLiteral("link:")
        + QString::fromLatin1(QCryptographicHash::hash(
                                  url.toUtf8(), QCryptographicHash::Sha256)
                                  .toHex()
                                  .left(40));
}

QVariantMap LinkPreviewController::resolveLinkMedia(const QString &mediaKey) const
{
    if (!m_inlineMedia)
        return {};
    const QString url = m_mediaKeys.value(mediaKey);
    if (url.isEmpty())
        return {};
    const auto it = m_urls.constFind(url);
    if (it == m_urls.constEnd() || it->state != QLatin1String("loaded"))
        return {};
    const QVariantMap &fields = it->fields;
    const QString kind = mediaKindOf(fields);
    const bool tooLarge =
        fields.value(QStringLiteral("mediaTooLarge")).toBool();
    if (kind == QLatin1String("video")) {
        if (tooLarge)
            return {};
        return { { QStringLiteral("url"), url },
                 { QStringLiteral("expect"), 1 } };
    }
    if (kind != QLatin1String("image"))
        return {};
    const QString src = fields.value(QStringLiteral("imageSource")).toString();
    const QString mime = fields.value(QStringLiteral("imageMime")).toString();
    if (src.startsWith(QLatin1String("mxc://")))
        return { { QStringLiteral("mxc"), src } };
    if (!src.isEmpty()) {
        // Only the exact shape Rust builds: data:<validated mime>;base64,...
        const QString prefix = QStringLiteral("data:") + mime
            + QStringLiteral(";base64,");
        if (!mime.startsWith(QLatin1String("image/")) || !src.startsWith(prefix))
            return {};
        const QByteArray bytes = QByteArray::fromBase64(
            QStringView(src).mid(prefix.size()).toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
        if (bytes.isEmpty())
            return {};
        return { { QStringLiteral("bytes"), bytes },
                 { QStringLiteral("mime"), mime } };
    }
    // Too large to inline: the viewer fetches it, within the cap.
    if (tooLarge)
        return {};
    return { { QStringLiteral("url"), url }, { QStringLiteral("expect"), 0 } };
}

bool LinkPreviewController::linkMediaAvailable(const QString &mediaKey) const
{
    if (!m_inlineMedia)
        return false;
    const QString url = m_mediaKeys.value(mediaKey);
    if (url.isEmpty())
        return false;
    const auto it = m_urls.constFind(url);
    return it != m_urls.constEnd() && it->state == QLatin1String("loaded");
}

QVariantMap LinkPreviewController::viewerEntry(const QString &mediaKey) const
{
    const QString url = m_mediaKeys.value(mediaKey);
    if (url.isEmpty())
        return {};
    const auto it = m_urls.constFind(url);
    QString mime;
    if (it != m_urls.constEnd())
        mime = it->fields.value(QStringLiteral("imageMime")).toString();
    return {
        { QStringLiteral("url"), url },
        { QStringLiteral("host"), matrix::link_preview::sanitizedHost(url) },
        { QStringLiteral("fileName"), fileNameForUrl(url) },
        { QStringLiteral("mime"), mime },
    };
}

QVariantMap LinkPreviewController::previewFor(const QString &itemKey,
                                              const QString &body,
                                              bool roomEncrypted)
{
    if (itemKey.isEmpty() || !supported())
        return { { QStringLiteral("state"), QStringLiteral("none") } };

    auto it = m_items.find(itemKey);
    if (it == m_items.end()) {
        if (m_items.size() >= kMaxTrackedItems)
            m_items.clear(); // defensive bound; a view never tracks this many
        ItemEntry item;
        item.url = matrix::link_preview::firstPreviewableUrl(body);
        item.encrypted = roomEncrypted;
        it = m_items.insert(itemKey, item);
    }

    ItemEntry &item = it.value();
    if (item.url.isEmpty())
        return { { QStringLiteral("state"), QStringLiteral("none") } };

    // Dismissed rows resolve to "none" so the card deactivates. Checked before
    // dispatch so a rebuilt row does not re-fetch; url/host are still reported
    // so the row can offer to restore the preview.
    if (m_dismissed.contains(itemKey)) {
        return {
            { QStringLiteral("state"), QStringLiteral("none") },
            { QStringLiteral("dismissed"), true },
            { QStringLiteral("url"), item.url },
            { QStringLiteral("host"),
              matrix::link_preview::sanitizedHost(item.url) },
        };
    }

    const bool autoAllowed =
        item.encrypted ? m_allowEncrypted : m_autoLoadUnencrypted;
    const bool known = m_urls.contains(item.url);
    if (!known && (autoAllowed || item.consented)) {
        if (!m_urlItems[item.url].contains(itemKey))
            m_urlItems[item.url].append(itemKey);
        dispatch(item.url);
    } else if (known && !m_urlItems[item.url].contains(itemKey)) {
        m_urlItems[item.url].append(itemKey);
    }

    return stateFor(item);
}

QString LinkPreviewController::ownershipKey(const QString &roomId,
                                            const QString &stableEventId)
{
    if (roomId.isEmpty() || stableEventId.isEmpty())
        return {};
    return roomId + QChar(0x1f) + stableEventId;
}

QVariantMap LinkPreviewController::previewForEvent(const QString &roomId,
                                                   const QString &stableEventId,
                                                   const QString &body,
                                                   bool roomEncrypted)
{
    const QString key = ownershipKey(roomId, stableEventId);
    if (key.isEmpty())
        return { { QStringLiteral("state"), QStringLiteral("none") } };

    const QString canonicalUrl = matrix::link_preview::firstPreviewableUrl(body);
    auto existing = m_items.find(key);
    if (existing != m_items.end()
        && (existing->url != canonicalUrl || existing->encrypted != roomEncrypted)) {
        m_urlItems[existing->url].removeAll(key);
        m_items.erase(existing);
        // The row now points elsewhere (edited link or changed encryption
        // flag). Consent and dismissal both applied to the old URL, so drop
        // them. Silent, since previewFor() is about to return the fresh state
        // to this caller.
        forgetDismissal(key);
    }
    return previewFor(key, body, roomEncrypted);
}

void LinkPreviewController::requestPreview(const QString &itemKey)
{
    auto it = m_items.find(itemKey);
    if (it == m_items.end() || it->url.isEmpty() || !supported())
        return;
    // Explicit user gesture (the encrypted-room consent path); the site is
    // contacted only from here on.
    it->consented = true;
    if (!m_urlItems[it->url].contains(itemKey))
        m_urlItems[it->url].append(itemKey);
    if (!m_urls.contains(it->url))
        dispatch(it->url);
    Q_EMIT previewChanged(itemKey);
}

void LinkPreviewController::requestPreviewForEvent(const QString &roomId,
                                                   const QString &stableEventId)
{
    requestPreview(ownershipKey(roomId, stableEventId));
}

void LinkPreviewController::retry(const QString &itemKey)
{
    auto it = m_items.find(itemKey);
    if (it == m_items.end() || it->url.isEmpty() || !supported())
        return;
    const auto urlIt = m_urls.constFind(it->url);
    if (urlIt == m_urls.constEnd()
        || urlIt->state != QLatin1String("failed")
        || !stateFor(it.value()).value(QStringLiteral("retryable")).toBool())
        return;
    m_heldBytes -= urlIt->heldBytes;
    m_urls.remove(it->url);
    m_urlOrder.removeOne(it->url);
    m_mediaKeys.remove(mediaKeyForUrl(it->url));
    it->consented = true; // retry is always an explicit gesture
    dispatch(it->url);
    Q_EMIT previewChanged(itemKey);
}

void LinkPreviewController::retryForEvent(const QString &roomId,
                                          const QString &stableEventId)
{
    retry(ownershipKey(roomId, stableEventId));
}

void LinkPreviewController::dismissPreview(const QString &itemKey)
{
    if (itemKey.isEmpty() || m_dismissed.contains(itemKey))
        return;
    m_dismissed.insert(itemKey);
    m_dismissedOrder.append(itemKey);
    // At the cap the oldest dismissal is released; its card is announced so it
    // comes back live.
    while (m_dismissedOrder.size() > kMaxDismissed) {
        const QString evicted = m_dismissedOrder.takeFirst();
        if (m_dismissed.remove(evicted))
            Q_EMIT previewChanged(evicted);
    }
    Q_EMIT previewChanged(itemKey);
}

void LinkPreviewController::dismissPreviewForEvent(const QString &roomId,
                                                   const QString &stableEventId)
{
    dismissPreview(ownershipKey(roomId, stableEventId));
}

void LinkPreviewController::restorePreview(const QString &itemKey)
{
    if (itemKey.isEmpty() || !m_dismissed.remove(itemKey))
        return;
    m_dismissedOrder.removeAll(itemKey);
    // Does not set consented: undoing a dismissal is not consent to contact the
    // site.
    Q_EMIT previewChanged(itemKey);
}

void LinkPreviewController::restorePreviewForEvent(const QString &roomId,
                                                   const QString &stableEventId)
{
    restorePreview(ownershipKey(roomId, stableEventId));
}

bool LinkPreviewController::isPreviewDismissed(const QString &itemKey) const
{
    return !itemKey.isEmpty() && m_dismissed.contains(itemKey);
}

void LinkPreviewController::forgetDismissal(const QString &itemKey)
{
    if (itemKey.isEmpty() || !m_dismissed.remove(itemKey))
        return;
    m_dismissedOrder.removeAll(itemKey);
}

void LinkPreviewController::dispatch(const QString &url)
{
    // Deduplicate simultaneous requests for the same URL.
    for (auto it = m_inflight.constBegin(); it != m_inflight.constEnd(); ++it) {
        if (it.value() == url)
            return;
    }
    const quint64 opId = m_client->fetchUrlPreview(url);
    const QString host = matrix::link_preview::sanitizedHost(url);
    if (opId == 0) {
        UrlEntry entry;
        entry.state = QStringLiteral("failed");
        entry.category = QStringLiteral("rejected");
        m_urls.insert(url, entry);
        m_urlOrder.append(url);
        evictIfNeeded();
        qCWarning(lcPreview) << "url preview dispatch rejected host=" << host;
        return;
    }
    m_urls.insert(url, UrlEntry{}); // state "loading"
    m_urlOrder.append(url);
    evictIfNeeded();
    m_inflight.insert(opId, url);
    qCInfo(lcPreview) << "url preview requested host=" << host;
}

void LinkPreviewController::evictIfNeeded(const QString &keep)
{
    for (;;) {
        const bool overCount = m_urls.size() > m_urlCacheLimit;
        const bool overBytes = m_heldBytes > m_heldBytesBudget;
        if (!overCount && !overBytes)
            break;
        // Oldest entry not in flight; in-flight entries must stay resolvable,
        // and the one just filled is what its rows are about to draw. Over the
        // byte budget alone, an entry holding no image frees nothing.
        int victimIndex = -1;
        for (int i = 0; i < m_urlOrder.size(); ++i) {
            const QString &candidate = m_urlOrder.at(i);
            if (candidate == keep)
                continue;
            const auto it = m_urls.constFind(candidate);
            if (it == m_urls.constEnd()
                || it->state == QLatin1String("loading")
                || (!overCount && it->heldBytes == 0))
                continue;
            victimIndex = i;
            break;
        }
        if (victimIndex < 0)
            break; // everything is loading; the cap is exceeded briefly
        dropUrl(m_urlOrder.takeAt(victimIndex));
    }
}

void LinkPreviewController::dropUrl(const QString &url)
{
    const auto it = m_urls.find(url);
    if (it != m_urls.end()) {
        m_heldBytes -= it->heldBytes;
        m_urls.erase(it);
    }
    m_urlOrder.removeOne(url);
    m_urlItems.remove(url);
    m_mediaKeys.remove(mediaKeyForUrl(url));
}

QVariantMap LinkPreviewController::stateFor(const ItemEntry &item) const
{
    QVariantMap out;
    out.insert(QStringLiteral("url"), item.url);
    out.insert(QStringLiteral("host"),
               matrix::link_preview::sanitizedHost(item.url));

    const auto urlIt = m_urls.constFind(item.url);
    if (urlIt == m_urls.constEnd()) {
        // Not requested: awaiting consent, or auto-loading is off for this room
        // class.
        out.insert(QStringLiteral("state"), QStringLiteral("requires_action"));
        return out;
    }

    const UrlEntry &entry = urlIt.value();
    out.insert(QStringLiteral("state"), entry.state);
    if (entry.state == QLatin1String("failed")) {
        const bool retryable = entry.category == QLatin1String("network")
            || entry.category == QLatin1String("dns_failure")
            || entry.category == QLatin1String("request_failure")
            || entry.category == QLatin1String("timeout")
            || entry.category == QLatin1String("http_transient");
        if (!retryable) {
            out.insert(QStringLiteral("state"), QStringLiteral("none"));
            return out;
        }
        out.insert(QStringLiteral("retryable"), true);
        out.insert(QStringLiteral("category"), entry.category);
        return out;
    }
    if (entry.state != QLatin1String("loaded"))
        return out;

    for (auto fieldIt = entry.fields.constBegin();
         fieldIt != entry.fields.constEnd(); ++fieldIt)
        out.insert(fieldIt.key(), fieldIt.value());

    const GifClass gif = matrix::link_preview::classifyGif(
        entry.fields.value(QStringLiteral("imageMime")).toString(),
        entry.fields.value(QStringLiteral("imageSize")).toLongLong(),
        entry.fields.value(QStringLiteral("imageWidth")).toInt(),
        entry.fields.value(QStringLiteral("imageHeight")).toInt());
    out.insert(QStringLiteral("isGif"), gif != GifClass::NotGif);
    out.insert(QStringLiteral("gifOversized"), gif == GifClass::Oversized);
    out.insert(QStringLiteral("animationExpected"), gif == GifClass::Gif);
    const QString mediaKind = mediaKindOf(entry.fields);
    out.insert(QStringLiteral("isDirectMedia"), !mediaKind.isEmpty());
    out.insert(QStringLiteral("mediaKind"), mediaKind);
    if (!mediaKind.isEmpty()) {
        const bool video = mediaKind == QLatin1String("video");
        out.insert(QStringLiteral("mediaKey"), mediaKeyForUrl(item.url));
        // An image already in hand (inline bytes or the server's copy) draws
        // at once; otherwise the card offers the viewer or the player.
        out.insert(QStringLiteral("mediaHeld"),
                   !video
                       && !entry.fields.value(QStringLiteral("imageSource"))
                               .toString().isEmpty());
        out.insert(QStringLiteral("mediaSize"),
                   entry.fields.value(video ? QStringLiteral("videoSize")
                                            : QStringLiteral("imageSize"))
                       .toLongLong());
        out.insert(QStringLiteral("mediaTooLarge"),
                   entry.fields.value(QStringLiteral("mediaTooLarge")).toBool());
        out.insert(QStringLiteral("fileName"), fileNameForUrl(item.url));
        // Loaded is per URL; contacting the site again is per row.
        out.insert(QStringLiteral("mediaAllowed"),
                   item.consented
                       || (item.encrypted ? m_allowEncrypted
                                          : m_autoLoadUnencrypted));
    }
    return out;
}

void LinkPreviewController::onPreviewFinished(quint64 opId, bool ok,
                                              const QVariantMap &fields,
                                              const QString &category,
                                              int httpStatus, int redirectCount)
{
    const auto it = m_inflight.find(opId);
    if (it == m_inflight.end())
        return; // stale (cleared on sign-out) or foreign op
    const QString url = it.value();
    m_inflight.erase(it);

    auto urlIt = m_urls.find(url);
    if (urlIt == m_urls.end())
        return; // evicted or cleared meanwhile
    if (ok) {
        urlIt->state = QStringLiteral("loaded");
        urlIt->fields = fields;
        urlIt->heldBytes = heldBytesOf(fields);
        m_heldBytes += urlIt->heldBytes;
        if (!mediaKindOf(fields).isEmpty())
            m_mediaKeys.insert(mediaKeyForUrl(url), url);
    } else {
        urlIt->state = QStringLiteral("failed");
        urlIt->category = category;
    }
    // Sanitized diagnostics: hostname, HTTP status, redirect count and failure
    // category; never the URL path/query, body or headers. Status and redirect
    // count are 0 on success or when no HTTP response was reached.
    qCInfo(lcPreview) << "url preview completed host="
                      << matrix::link_preview::sanitizedHost(url)
                      << "result=" << (ok ? QStringLiteral("loaded") : category)
                      << "httpStatus=" << httpStatus
                      << "redirects=" << redirectCount;

    // Read before evicting: a budget eviction drops other URLs' interest.
    const QStringList interested = m_urlItems.value(url);
    evictIfNeeded(url);
    for (const QString &itemKey : interested)
        Q_EMIT previewChanged(itemKey);
}

void LinkPreviewController::clear()
{
    m_items.clear();
    m_urls.clear();
    m_urlOrder.clear();
    m_inflight.clear();
    m_urlItems.clear();
    m_mediaKeys.clear();
    m_heldBytes = 0;
    // Dismissals do not carry over to another account (reached from
    // onLoggedOut() and setClient()).
    m_dismissed.clear();
    m_dismissedOrder.clear();
}

void LinkPreviewController::onLoggedOut()
{
    // Account partition: no URL, preview text or pending completion survives
    // into the next session.
    clear();
    qCInfo(lcPreview) << "url previews cleared on sign-out";
}
