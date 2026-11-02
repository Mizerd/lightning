#pragma once

#include "gif/GifProvider.h"
#include "gif/GifResultModel.h"
#include "gif/GifFavoritesModel.h"
#include "gif/GifPreviewCache.h"
#include "gif/GifRecentModel.h"
#include "gif/GifSavedModel.h"
#include "gif/GifStarredStore.h"

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

class GifSearchController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("GifSearchController is exposed via app.gif")
    Q_PROPERTY(GifResultModel *results READ results CONSTANT)
    Q_PROPERTY(GifFavoritesModel *favorites READ favorites CONSTANT)
    Q_PROPERTY(GifRecentModel *recent READ recent CONSTANT)
    Q_PROPERTY(GifStarredStore *starredStore READ starredStore CONSTANT)
    Q_PROPERTY(GifSavedModel *saved READ saved CONSTANT)
    Q_PROPERTY(GifPreviewCache *previews READ previews CONSTANT)
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

    void setTransport(GifTransport *transport);
    // Test / settings seam: override the environment key for a provider.
    // Overridden providers are pinned — refreshProviderKeys() skips them.
    void setApiKey(const QString &providerId, const QString &key);
    Q_INVOKABLE void refreshProviderKeys();
    void setDebounceMs(int ms) { m_debounceMs = ms; }

    GifResultModel *results() { return &m_results; }
    GifFavoritesModel *favorites() { return m_favorites.get(); }
    GifRecentModel *recent() { return m_recent.get(); }
    GifStarredStore *starredStore() const { return m_starred.get(); }
    GifSavedModel *saved() const { return m_saved.get(); }
    GifPreviewCache *previews() const { return m_previews.get(); }

    // Opens/closes the local saved store for an account-scoped, validated
    // directory (matrix::app_data::accountRoot()). Called only by AppController
    // from the account lifecycle.
    void openStarredStoreFor(const QString &accountDir)
    { m_starred->openFor(accountDir); }
    void closeStarredStore() { m_starred->close(); }
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
    Q_INVOKABLE void setQueryText(const QString &text);
    Q_INVOKABLE void searchNow(const QString &text);
    Q_INVOKABLE void openCategory(const QString &term);
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void reset();

    Q_INVOKABLE void notifyPickerOpening(const QString &target);

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    void seedDemoCatalogue(const QList<gif::GifResult> &rows);
    bool demoCatalogueActive() const { return m_demoCatalogue; }
#endif

    Q_INVOKABLE bool toggleFavorite(const QVariantMap &resultMap);
    Q_INVOKABLE void recordSent(const QVariantMap &resultMap);

Q_SIGNALS:
    void providerConfigurationChanged();
    void pickerOpenRequested(const QString &target);
    void availableChanged();
    void providerChanged();
    void stateChanged();
    void modeChanged();
    void queryChanged();
    void ratingChanged();

private:
    void onFinished(quint64 opId, bool ok, int httpStatus,
                    const QByteArray &body, const QString &category);
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
    std::unique_ptr<GifStarredStore> m_starred;
    std::unique_ptr<GifSavedModel> m_saved;
    std::unique_ptr<GifPreviewCache> m_previews;
    GifTransport *m_transport = nullptr;

    QString m_activeProviderId = QStringLiteral("giphy");
    std::unique_ptr<gif::GifProvider> m_provider;
    QHash<QString, QString> m_apiKeys;
    QSet<QString> m_apiKeyOverrides;

    RequestState m_state = RequestState::Idle;
    Mode m_mode = Trending;
    gif::Rating m_rating = gif::Rating::PG13;
    QString m_query;
    int m_page = 0;
    bool m_hasMore = false;

    quint64 m_activeOp = 0;
    bool m_appending = false;

    QTimer m_debounce;
    int m_debounceMs = 300;
    QString m_pendingQuery;

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    bool m_demoCatalogue = false;
    QList<gif::GifResult> m_demoRows;
#endif
};
