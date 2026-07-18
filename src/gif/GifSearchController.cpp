#include "gif/GifSearchController.h"

#include "gif/GifTransport.h"

GifSearchController::GifSearchController(QObject *parent)
    : QObject(parent)
    , m_provider(gif::makeGifProvider(m_activeProviderId))
    , m_settings(std::make_unique<QSettings>())
    , m_favorites(std::make_unique<GifFavoritesModel>(m_settings.get(), this))
    , m_recent(std::make_unique<GifRecentModel>(m_settings.get(), this))
{
    // Environment keys. Read once; never logged. A settings override may call
    // setApiKey() later.
    m_apiKeys.insert(QStringLiteral("giphy"),
                     qEnvironmentVariable("LIGHTNING_GIPHY_API_KEY"));
    m_apiKeys.insert(QStringLiteral("klipy"),
                     qEnvironmentVariable("LIGHTNING_KLIPY_API_KEY"));

    // The grid's star reflects live favorite state; a toggle refreshes only the
    // affected tile, never the whole grid.
    m_results.setFavoriteResolver(
        [this](const QString &provider, const QString &id) {
            return m_favorites->isFavorite(provider, id);
        });
    connect(m_favorites.get(), &GifFavoritesModel::favoriteToggled, this,
            [this](const QString &provider, const QString &id, bool) {
                m_results.refreshFavorite(provider, id);
            });

    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        searchNow(m_pendingQuery);
    });
}

GifSearchController::~GifSearchController() = default;

void GifSearchController::setTransport(GifTransport *transport)
{
    if (m_transport == transport)
        return;
    if (m_transport)
        m_transport->disconnect(this);
    m_transport = transport;
    if (m_transport) {
        connect(m_transport, &GifTransport::finished, this,
                &GifSearchController::onFinished);
        connect(m_transport, &QObject::destroyed, this, [this] {
            m_transport = nullptr;
            Q_EMIT availableChanged();
        });
    }
    Q_EMIT availableChanged();
}

void GifSearchController::setApiKey(const QString &providerId, const QString &key)
{
    m_apiKeys.insert(providerId, key);
    if (providerId == m_activeProviderId)
        Q_EMIT providerChanged();
}

bool GifSearchController::available() const
{
    return m_transport && m_transport->available();
}

QString GifSearchController::apiKeyFor(const QString &providerId) const
{
    return m_apiKeys.value(providerId);
}

bool GifSearchController::providerConfigured(const QString &providerId) const
{
    return !apiKeyFor(providerId).isEmpty();
}

QString GifSearchController::providerDisplayName(const QString &providerId) const
{
    auto p = gif::makeGifProvider(providerId);
    return p ? p->displayName() : providerId;
}

QString GifSearchController::providerAttribution(const QString &providerId) const
{
    auto p = gif::makeGifProvider(providerId);
    return p ? p->attribution() : QString();
}

QString GifSearchController::providerName() const
{
    return m_provider ? m_provider->displayName() : m_activeProviderId;
}

QString GifSearchController::attribution() const
{
    return m_provider ? m_provider->attribution() : QString();
}

QStringList GifSearchController::categories() const
{
    return m_provider ? m_provider->categoryShortcuts() : QStringList();
}

void GifSearchController::setRating(int rating)
{
    const auto r = static_cast<gif::Rating>(
        qBound(0, rating, static_cast<int>(gif::Rating::R)));
    if (r == m_rating)
        return;
    m_rating = r;
    Q_EMIT ratingChanged();
    // Safe-search change re-runs the current view from the top.
    if (m_mode == Search && !m_query.isEmpty())
        searchNow(m_query);
    else if (m_mode == Category && !m_query.isEmpty())
        openCategory(m_query);
    else
        showTrending();
}

void GifSearchController::setActiveProvider(const QString &providerId)
{
    if (providerId == m_activeProviderId
        || !gif::knownGifProviderIds().contains(providerId))
        return;
    m_debounce.stop();
    m_activeOp = 0;              // drop any in-flight result
    m_activeProviderId = providerId;
    m_provider = gif::makeGifProvider(providerId);
    m_page = 0;
    m_hasMore = false;
    m_results.clear();
    Q_EMIT providerChanged();
    // Re-run the current mode on the new provider (pagination reset above).
    if (m_mode == Search && !m_query.isEmpty())
        searchNow(m_query);
    else if (m_mode == Category && !m_query.isEmpty())
        openCategory(m_query);
    else
        showTrending();
}

void GifSearchController::showTrending()
{
    setMode(Trending);
    m_query.clear();
    Q_EMIT queryChanged();
    runRequest(true, QString(), 0, false);
}

void GifSearchController::setQueryText(const QString &text)
{
    const QString trimmed = text.trimmed();
    m_pendingQuery = trimmed;
    // Empty query → back to trending immediately (no request storm).
    if (trimmed.isEmpty()) {
        m_debounce.stop();
        showTrending();
        return;
    }
    m_debounce.start(m_debounceMs);
}

void GifSearchController::searchNow(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        showTrending();
        return;
    }
    m_debounce.stop();
    setMode(Search);
    m_query = trimmed;
    Q_EMIT queryChanged();
    runRequest(false, trimmed, 0, false);
}

void GifSearchController::openCategory(const QString &term)
{
    const QString trimmed = term.trimmed();
    if (trimmed.isEmpty())
        return;
    m_debounce.stop();
    setMode(Category);
    m_query = trimmed;
    Q_EMIT queryChanged();
    // A category is a normal provider search under the hood.
    runRequest(false, trimmed, 0, false);
}

void GifSearchController::loadMore()
{
    if (!m_hasMore || m_state == RequestState::Loading
        || m_state == RequestState::LoadingMore)
        return;
    runRequest(m_mode == Trending, m_query, m_page + 1, true);
}

void GifSearchController::runRequest(bool trending, const QString &query,
                                     int page, bool appending)
{
    if (!m_provider)
        return;
    if (!available()) {
        m_activeOp = 0;
        setState(RequestState::Offline);
        return;
    }
    const QString key = apiKeyFor(m_activeProviderId);
    if (key.isEmpty()) {
        m_activeOp = 0;
        setState(RequestState::MissingKey);
        return;
    }

    QString url;
    if (trending)
        url = m_provider->trendingUrl(m_rating, page, 24, key);
    else
        url = m_provider->searchUrl(query, m_rating, page, 24, key);
    if (url.isEmpty()) {
        m_activeOp = 0;
        setState(RequestState::NoResults);
        return;
    }

    m_appending = appending;
    if (!appending)
        m_page = 0;
    // The URL carries the key — never logged. Only the op id is tracked.
    const quint64 op = m_transport->get(url);
    if (op == 0) {
        m_activeOp = 0;
        setState(RequestState::Offline);
        return;
    }
    m_activeOp = op;
    setState(appending ? RequestState::LoadingMore : RequestState::Loading);
}

gif::RequestState GifSearchController::categoryToState(const QString &category) const
{
    if (category == QLatin1String("rate_limited"))
        return RequestState::RateLimited;
    if (category == QLatin1String("network")
        || category == QLatin1String("timeout")
        || category == QLatin1String("blocked"))
        return RequestState::Offline;
    return RequestState::ProviderError; // provider_error / too_large / malformed
}

void GifSearchController::onFinished(quint64 opId, bool ok, int /*httpStatus*/,
                                     const QByteArray &body,
                                     const QString &category)
{
    if (opId == 0 || opId != m_activeOp)
        return; // stale: superseded request / provider switch / reset
    m_activeOp = 0;

    if (!ok) {
        setState(categoryToState(category));
        return;
    }

    const gif::ParseOutcome outcome =
        m_provider->parse(body, m_rating, m_appending ? m_page + 1 : 0, 24);
    if (!outcome.ok) {
        setState(RequestState::ProviderError);
        return;
    }

    if (m_appending) {
        m_results.append(outcome.results);
        ++m_page;
    } else {
        m_results.reset(outcome.results);
        m_page = 0;
    }
    m_hasMore = outcome.hasMore;

    setState(m_results.count() == 0 ? RequestState::NoResults
                                    : RequestState::Ready);
}

void GifSearchController::reset()
{
    m_debounce.stop();
    m_activeOp = 0;
    m_page = 0;
    m_hasMore = false;
    m_query.clear();
    m_pendingQuery.clear();
    m_results.clear();
    setMode(Trending);
    setState(RequestState::Idle);
    Q_EMIT queryChanged();
}

bool GifSearchController::toggleFavorite(const QVariantMap &resultMap)
{
    const bool now = m_favorites->toggle(resultMap);
    // Reflect the change in whichever grid is showing this item.
    const QString provider = resultMap.value(QStringLiteral("provider")).toString();
    const QString id = resultMap.value(QStringLiteral("gifId")).toString();
    m_results.refreshFavorite(provider, id);
    return now;
}

void GifSearchController::recordSent(const QVariantMap &resultMap)
{
    m_recent->recordSentMap(resultMap);
}

void GifSearchController::setState(RequestState state)
{
    if (m_state == state) {
        // hasMore may still have changed on a Ready→Ready append.
        Q_EMIT stateChanged();
        return;
    }
    m_state = state;
    Q_EMIT stateChanged();
}

void GifSearchController::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    Q_EMIT modeChanged();
}
