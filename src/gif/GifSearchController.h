#pragma once

#include "gif/GifProvider.h"
#include "gif/GifResultModel.h"
#include "gif/GifFavoritesModel.h"
#include "gif/GifRecentModel.h"

#include <QHash>
#include <QSet>
#include <QObject>
#include <QQmlEngine>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

class GifTransport;

// v0.6.1: shared, provider-agnostic GIF search controller (exposed to QML as
// app.gif). Owns request lifecycle, debounce, cancellation, stale-result
// rejection, the result-grid model, pagination and the active-provider +
// safe-search state — everything shared across providers. Provider-specific
// concerns (endpoints, key injection, parsing, attribution) live in GifProvider.
//
// API keys resolve as: runtime override (LIGHTNING_GIPHY_API_KEY /
// LIGHTNING_KLIPY_API_KEY) -> key compiled into an official release build ->
// unconfigured. Keys are never logged or exposed to QML; a provider with no key
// is reported as unconfigured (MissingKey) and its picker section is disabled,
// while the rest of messaging is unaffected. See gif::resolveProviderKeyDetailed
// (process environment > local env file > build key > unconfigured).
class GifSearchController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("GifSearchController is exposed via app.gif")
    Q_PROPERTY(GifResultModel *results READ results CONSTANT)
    Q_PROPERTY(GifFavoritesModel *favorites READ favorites CONSTANT)
    Q_PROPERTY(GifRecentModel *recent READ recent CONSTANT)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QStringList providerIds READ providerIds CONSTANT)
    Q_PROPERTY(QString providerId READ providerId NOTIFY providerChanged)
    Q_PROPERTY(QString providerName READ providerName NOTIFY providerChanged)
    Q_PROPERTY(QString attribution READ attribution NOTIFY providerChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY providerChanged)
    Q_PROPERTY(QStringList categories READ categories NOTIFY providerChanged)
    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(int mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(QString query READ query NOTIFY queryChanged)
    Q_PROPERTY(int rating READ rating WRITE setRating NOTIFY ratingChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY stateChanged)

public:
    // QML-facing state (mirrors gif::RequestState order) and picker mode.
    enum State {
        Idle, Loading, LoadingMore, Ready, NoResults,
        Offline, RateLimited, ProviderError, MissingKey, Cancelled,
    };
    Q_ENUM(State)
    enum Mode { Trending, Search, Category };
    Q_ENUM(Mode)

    using RequestState = gif::RequestState;

    explicit GifSearchController(QObject *parent = nullptr);
    ~GifSearchController() override;

    // Owned externally (AppController). Reconnecting is allowed (logout/login).
    void setTransport(GifTransport *transport);
    // Test / settings seam: override the environment key for a provider.
    // Overridden providers are pinned — refreshProviderKeys() skips them.
    void setApiKey(const QString &providerId, const QString &key);
    // Re-resolve provider keys through the configuration source of truth
    // (environment > local env file > build key). Invoked when the picker
    // opens, so availability never sticks at "off" because the controller
    // was constructed before the environment was ready.
    Q_INVOKABLE void refreshProviderKeys();
    // Debounce window (ms). Exposed for deterministic tests.
    void setDebounceMs(int ms) { m_debounceMs = ms; }

    GifResultModel *results() { return &m_results; }
    GifFavoritesModel *favorites() { return m_favorites.get(); }
    GifRecentModel *recent() { return m_recent.get(); }
    bool available() const;
    QStringList providerIds() const { return gif::knownGifProviderIds(); }
    QString providerId() const { return m_activeProviderId; }
    QString providerName() const;
    QString attribution() const;
    bool configured() const { return providerConfigured(m_activeProviderId); }
    QStringList categories() const;
    int state() const { return static_cast<int>(m_state); }
    int mode() const { return static_cast<int>(m_mode); }
    QString query() const { return m_query; }
    int rating() const { return static_cast<int>(m_rating); }
    void setRating(int rating);
    bool hasMore() const { return m_hasMore; }

    Q_INVOKABLE bool providerConfigured(const QString &providerId) const;
    Q_INVOKABLE QString providerDisplayName(const QString &providerId) const;
    Q_INVOKABLE QString providerAttribution(const QString &providerId) const;

    Q_INVOKABLE void setActiveProvider(const QString &providerId);
    Q_INVOKABLE void showTrending();
    Q_INVOKABLE void setQueryText(const QString &text); // debounced
    Q_INVOKABLE void searchNow(const QString &text);    // immediate
    Q_INVOKABLE void openCategory(const QString &term);
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void reset();  // cancel + clear (picker closed / logout)

    // Favorite the search/favorites/recent result described by `resultMap`
    // (a GifResultModel role map). Returns the new favorite state. Refreshes
    // the grids so the star updates without a rebuild.
    Q_INVOKABLE bool toggleFavorite(const QVariantMap &resultMap);
    // Record a successful send handoff into Recents (Phase GIF-7 calls this).
    Q_INVOKABLE void recordSent(const QVariantMap &resultMap);

Q_SIGNALS:
    // Provider key availability was re-resolved (labels and enabled states
    // must re-read providerConfigured()).
    void providerConfigurationChanged();
    void availableChanged();
    void providerChanged();
    void stateChanged();
    void modeChanged();
    void queryChanged();
    void ratingChanged();

private:
    void onFinished(quint64 opId, bool ok, int httpStatus,
                    const QByteArray &body, const QString &category);
    // Run a request at `page`, appending when page>0. `trending` selects the
    // trending endpoint; otherwise the search endpoint (also used by category).
    void runRequest(bool trending, const QString &query, int page,
                    bool appending);
    void setState(RequestState state);
    void setMode(Mode mode);
    gif::GifProvider *provider() const { return m_provider.get(); }
    QString apiKeyFor(const QString &providerId) const;
    RequestState categoryToState(const QString &category) const;

    GifResultModel m_results;
    std::unique_ptr<QSettings> m_settings;
    std::unique_ptr<GifFavoritesModel> m_favorites;
    std::unique_ptr<GifRecentModel> m_recent;
    GifTransport *m_transport = nullptr;

    QString m_activeProviderId = QStringLiteral("giphy");
    std::unique_ptr<gif::GifProvider> m_provider;
    QHash<QString, QString> m_apiKeys; // provider id -> key (never logged)
    QSet<QString> m_apiKeyOverrides;   // providers pinned via setApiKey()

    RequestState m_state = RequestState::Idle;
    Mode m_mode = Trending;
    gif::Rating m_rating = gif::Rating::PG13;
    QString m_query;          // current search / category term
    int m_page = 0;
    bool m_hasMore = false;

    quint64 m_activeOp = 0;   // in-flight op; results for other ops are stale
    bool m_appending = false; // current request appends (loadMore) vs resets

    QTimer m_debounce;
    int m_debounceMs = 300;
    QString m_pendingQuery;
};
