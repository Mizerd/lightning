#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;

// Nonvisual backend for client-side URL previews.
//
// Rust performs a bounded, DNS-validated fetch; neither C++ nor QML contacts
// the target URL. This controller owns the policy and state around it:
//
//   * one previewable URL per message (the first eligible one, from
//     matrix::link_preview::firstPreviewableUrl);
//   * privacy gating: fetching contacts the linked site directly, so
//     encrypted rooms never auto-request unless the user opted in; the
//     default there is "requires_action" and a fetch happens only through
//     requestPreview(). Unencrypted rooms follow the auto-load setting;
//   * a bounded in-memory result cache keyed by URL, deduplicating
//     simultaneous requests;
//   * stale-result rejection by op id and account partitioning: all state
//     clears on sign-out and nothing is persisted;
//   * GIF classification from the Rust-validated MIME, never the URL suffix.
//
// A failed preview never affects the message row. Logs carry the sanitized
// hostname at most. AppController pushes the policy booleans; the controller
// stays settings-agnostic so tests can drive it directly.
class LinkPreviewController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool autoLoadUnencrypted READ autoLoadUnencrypted
                   WRITE setAutoLoadUnencrypted NOTIFY policyChanged)
    Q_PROPERTY(bool allowEncrypted READ allowEncrypted
                   WRITE setAllowEncrypted NOTIFY policyChanged)

public:
    explicit LinkPreviewController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    bool supported() const;

    bool autoLoadUnencrypted() const { return m_autoLoadUnencrypted; }
    void setAutoLoadUnencrypted(bool value);
    bool allowEncrypted() const { return m_allowEncrypted; }
    void setAllowEncrypted(bool value);

    // Preview state for one timeline item. May dispatch a request when
    // policy allows automatic loading. Returned map:
    //   state: "none" | "requires_action" | "loading" | "loaded" | "failed"
    //   url, host                     — original URL + sanitized hostname
    //   title, description, siteName  — when loaded
    //   imageMxc/imageSource, imageMime, imageWidth, imageHeight, imageSize
    //   isDirectMedia, isGif, gifOversized, animationExpected
    //   retryable                     — when failed
    Q_INVOKABLE QVariantMap previewFor(const QString &itemKey,
                                       const QString &body,
                                       bool roomEncrypted);
    Q_INVOKABLE QVariantMap previewForEvent(const QString &roomId,
                                            const QString &stableEventId,
                                            const QString &body,
                                            bool roomEncrypted);
    // Explicit user consent: load even where automatic loading is off (the
    // encrypted-room opt-in gesture).
    Q_INVOKABLE void requestPreview(const QString &itemKey);
    Q_INVOKABLE void requestPreviewForEvent(const QString &roomId,
                                            const QString &stableEventId);
    // Re-request after a failure.
    Q_INVOKABLE void retry(const QString &itemKey);
    Q_INVOKABLE void retryForEvent(const QString &roomId,
                                   const QString &stableEventId);

    // The reader dismissed this row's preview card; the row collapses. Local
    // rendering state only; nothing is sent and the link stays in the body.
    // Keyed per (room, event), never per URL: the URL cache is shared, and a
    // per-URL dismissal would collapse cards elsewhere, including above the
    // reader. Checked before auto-load dispatch so it survives row rebuilds.
    Q_INVOKABLE void dismissPreview(const QString &itemKey);
    Q_INVOKABLE void dismissPreviewForEvent(const QString &roomId,
                                            const QString &stableEventId);
    // Undo. Clears the dismissal and lets the normal policy decide again; it
    // grants no consent.
    Q_INVOKABLE void restorePreview(const QString &itemKey);
    Q_INVOKABLE void restorePreviewForEvent(const QString &roomId,
                                            const QString &stableEventId);
    Q_INVOKABLE bool isPreviewDismissed(const QString &itemKey) const;
    Q_INVOKABLE void clear();

    /// Bounded because nothing else prunes the set. At the cap the oldest
    /// dismissal is released rather than the newest refused (as in
    /// MediaVisibilityStore::kMaxHidden).
    static constexpr int kMaxDismissed = 4096;
    Q_INVOKABLE QString linkifiedBody(const QString &body) const;

    // Test hooks.
    void setUrlCacheLimit(int limit) { m_urlCacheLimit = limit; }
    int cachedUrlCount() const { return m_urls.size(); }
    int dismissedCount() const { return int(m_dismissed.size()); }

Q_SIGNALS:
    void supportedChanged();
    void policyChanged();
    // The preview state for `itemKey` changed; QML re-reads previewFor().
    void previewChanged(const QString &itemKey);

private Q_SLOTS:
    void onPreviewFinished(quint64 opId, bool ok, const QVariantMap &fields,
                           const QString &category, int httpStatus = 0,
                           int redirectCount = 0);
    void onLoggedOut();

private:
    struct UrlEntry {
        QString state = QStringLiteral("loading");
        QString category;
        QVariantMap fields;
    };
    struct ItemEntry {
        QString url;           // empty = no previewable URL in the body
        bool encrypted = false;
        bool consented = false; // explicit requestPreview() happened
    };

    void dispatch(const QString &url);
    static QString ownershipKey(const QString &roomId,
                                const QString &stableEventId);
    void evictIfNeeded();
    QVariantMap stateFor(const ItemEntry &item) const;
    // Drop a dismissal without announcing it, for callers about to return the
    // row's fresh state anyway; emitting would re-enter previewForEvent() from
    // the delegate's own handler.
    void forgetDismissal(const QString &itemKey);

    MatrixClient *m_client = nullptr;
    // Fail closed: a fetch exposes the reader's IP to a sender-chosen host, so
    // without a policy nothing loads. AppController pushes the real setting.
    bool m_autoLoadUnencrypted = false;
    bool m_allowEncrypted = false; // privacy default; see SettingsManager

    QHash<QString, ItemEntry> m_items;    // by timeline item key
    QHash<QString, UrlEntry> m_urls;      // by URL (bounded)
    QList<QString> m_urlOrder;            // insertion order for eviction
    QHash<quint64, QString> m_inflight;   // opId -> URL
    QHash<QString, QStringList> m_urlItems; // URL -> interested item keys

    // Dismissed rows, kept apart from ItemEntry because m_items is wiped
    // wholesale when it overflows. At the cap the oldest dismissal is released.
    // Session-only: cleared on sign-out and client swap.
    QSet<QString> m_dismissed;
    QList<QString> m_dismissedOrder; // insertion order; a QSet has none

    int m_urlCacheLimit = 256;
    static constexpr int kMaxTrackedItems = 4096;
};
