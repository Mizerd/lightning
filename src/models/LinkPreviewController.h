#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;

// v0.5.12: nonvisual backend for rich client-side URL previews.
//
// Rust performs a bounded, DNS-validated client-side fetch — QML never contacts the
// target URL itself, from C++ or from QML. This controller owns the policy
// and state around that fetcher:
//
//   * one previewable URL per message (the first eligible one, extracted by
//     matrix::link_preview::firstPreviewableUrl with its scheme allow-list
//     and code-span/userinfo exclusions);
//   * privacy gating — encrypted rooms NEVER auto-request a preview unless
//     the user opted in; requesting contacts the linked website directly, so
//     the default answer for an encrypted room is "requires_action" and the
//     fetch happens only through the explicit requestPreview() gesture.
//     Unencrypted rooms follow the auto-load setting;
//   * a bounded in-memory result cache keyed by URL, shared across
//     messages, deduplicating simultaneous requests;
//   * op-id stale-result rejection and full account partitioning (all state
//     clears on sign-out; nothing is ever persisted — encrypted-room URLs
//     never touch CacheStore or any other disk store);
//   * GIF classification from the Rust-validated response MIME —
//     never from the URL suffix.
//
// A failed preview only changes this controller's state; the message row it
// belongs to is untouched. Logs carry the sanitized hostname at most.
//
// AppController binds the two policy booleans from SettingsManager and
// keeps them live; the controller itself stays settings-agnostic so tests
// can drive it directly. The final preview card QML belongs to 0.5.11 UI
// work.
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
    // Explicit user consent: load the preview even where automatic loading
    // is disabled (this is the encrypted-room opt-in gesture).
    Q_INVOKABLE void requestPreview(const QString &itemKey);
    Q_INVOKABLE void requestPreviewForEvent(const QString &roomId,
                                            const QString &stableEventId);
    // Re-request after a failure.
    Q_INVOKABLE void retry(const QString &itemKey);
    Q_INVOKABLE void retryForEvent(const QString &roomId,
                                   const QString &stableEventId);
    Q_INVOKABLE void clear();
    Q_INVOKABLE QString linkifiedBody(const QString &body) const;

    // Test hooks.
    void setUrlCacheLimit(int limit) { m_urlCacheLimit = limit; }
    int cachedUrlCount() const { return m_urls.size(); }

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

    MatrixClient *m_client = nullptr;
    bool m_autoLoadUnencrypted = true;
    bool m_allowEncrypted = false; // privacy default; see SettingsManager

    QHash<QString, ItemEntry> m_items;    // by timeline item key
    QHash<QString, UrlEntry> m_urls;      // by URL (bounded)
    QList<QString> m_urlOrder;            // insertion order for eviction
    QHash<quint64, QString> m_inflight;   // opId -> URL
    QHash<QString, QStringList> m_urlItems; // URL -> interested item keys
    int m_urlCacheLimit = 256;
    static constexpr int kMaxTrackedItems = 4096;
};
