#include "models/LinkPreviewController.h"

#include "matrix/MatrixClient.h"
#include "models/LinkPreview.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcPreview, "lightning.timeline.linkpreview")

using matrix::link_preview::GifClass;

LinkPreviewController::LinkPreviewController(QObject *parent)
    : QObject(parent)
{
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
    }
    return previewFor(key, body, roomEncrypted);
}

void LinkPreviewController::requestPreview(const QString &itemKey)
{
    auto it = m_items.find(itemKey);
    if (it == m_items.end() || it->url.isEmpty() || !supported())
        return;
    // Explicit user gesture: this is the encrypted-room consent path. The
    // linked website is contacted only from here on.
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
    m_urls.remove(it->url);
    m_urlOrder.removeOne(it->url);
    it->consented = true; // retry is always an explicit gesture
    dispatch(it->url);
    Q_EMIT previewChanged(itemKey);
}

void LinkPreviewController::retryForEvent(const QString &roomId,
                                          const QString &stableEventId)
{
    retry(ownershipKey(roomId, stableEventId));
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

void LinkPreviewController::evictIfNeeded()
{
    while (m_urls.size() > m_urlCacheLimit) {
        // Oldest entry that is not in flight; in-flight entries must stay
        // resolvable for their completion.
        int victimIndex = -1;
        for (int i = 0; i < m_urlOrder.size(); ++i) {
            if (m_urls.value(m_urlOrder.at(i)).state
                != QLatin1String("loading")) {
                victimIndex = i;
                break;
            }
        }
        if (victimIndex < 0)
            break; // everything is loading; the cap is exceeded briefly
        const QString victim = m_urlOrder.takeAt(victimIndex);
        m_urls.remove(victim);
        m_urlItems.remove(victim);
    }
}

QVariantMap LinkPreviewController::stateFor(const ItemEntry &item) const
{
    QVariantMap out;
    out.insert(QStringLiteral("url"), item.url);
    out.insert(QStringLiteral("host"),
               matrix::link_preview::sanitizedHost(item.url));

    const auto urlIt = m_urls.constFind(item.url);
    if (urlIt == m_urls.constEnd()) {
        // Not requested: either awaiting explicit consent or auto-loading
        // is disabled for this room class.
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
    return out;
}

void LinkPreviewController::onPreviewFinished(quint64 opId, bool ok,
                                              const QVariantMap &fields,
                                              const QString &category)
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
    } else {
        urlIt->state = QStringLiteral("failed");
        urlIt->category = category;
    }
    qCInfo(lcPreview) << "url preview completed host="
                      << matrix::link_preview::sanitizedHost(url)
                      << "result=" << (ok ? QStringLiteral("loaded") : category);

    const QStringList interested = m_urlItems.value(url);
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
}

void LinkPreviewController::onLoggedOut()
{
    // Account partition: no URL, preview text, or pending completion may
    // survive into the next session. Nothing was ever persisted to disk.
    clear();
    qCInfo(lcPreview) << "url previews cleared on sign-out";
}
