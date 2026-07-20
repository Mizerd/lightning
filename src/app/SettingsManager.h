#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <memory>

class SecretStore;

class SettingsManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString homeserverUrl READ homeserverUrl WRITE setHomeserverUrl NOTIFY homeserverUrlChanged)
    Q_PROPERTY(Theme theme READ theme WRITE setTheme NOTIFY themeChanged)
    // Design Appearance page: message layout (0 = Modern, 1 = Bubbles for
    // direct-message timelines, 2 = Compact/IRC) and text scale (percent,
    // 90–140, 100 = default). Both are per-account like the theme: the
    // active account's value wins, the global value doubles as the
    // logged-out default and the fallback for accounts without one.
    Q_PROPERTY(int messageLayout READ messageLayout WRITE setMessageLayout
                   NOTIFY messageLayoutChanged)
    Q_PROPERTY(int textScale READ textScale WRITE setTextScale
                   NOTIFY textScaleChanged)
    // v0.7: bundled UI font family (per-account with global fallback, like
    // the rest of Appearance). Values are clamped to uiFontChoices().
    Q_PROPERTY(QString uiFont READ uiFont WRITE setUiFont
                   NOTIFY uiFontChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(bool startMinimized READ startMinimized WRITE setStartMinimized NOTIFY startMinimizedChanged)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsEnabledChanged)
    // v0.6.0 checkpoint 11: notification privacy. 0 = sender and message,
    // 1 = sender only (default), 2 = private ("New Matrix notification").
    Q_PROPERTY(int notificationPreview READ notificationPreview
                   WRITE setNotificationPreview NOTIFY notificationPreviewChanged)
    // v0.6.1: notification sound. 0 = off, 1 = mentions and direct messages
    // (default), 2 = all displayed notifications. Sound rides on the same
    // decision as the notification, so muted / active-room / mentions-only
    // suppression suppresses the sound too.
    Q_PROPERTY(int notificationSound READ notificationSound
                   WRITE setNotificationSound NOTIFY notificationSoundChanged)
    // v0.5.11: link previews. Encrypted-room previews default OFF (privacy).
    Q_PROPERTY(bool autoLoadLinkPreviews READ autoLoadLinkPreviews
                   WRITE setAutoLoadLinkPreviews NOTIFY autoLoadLinkPreviewsChanged)
    Q_PROPERTY(bool loadPreviewsInEncryptedRooms READ loadPreviewsInEncryptedRooms
                   WRITE setLoadPreviewsInEncryptedRooms
                   NOTIFY loadPreviewsInEncryptedRoomsChanged)
    Q_PROPERTY(bool animateGifPreviews READ animateGifPreviews
                   WRITE setAnimateGifPreviews NOTIFY animateGifPreviewsChanged)
    // v0.6.1: GIF browser policy. gifAutoplay: 0=Always (while visible),
    // 1=OnHover, 2=Never. gifSafeSearch is a gif::Rating id (0=g,1=pg,2=pg-13,
    // 3=r). storeRecentGifs toggles Recents recording. gifPreferredProvider is
    // the picker's default provider id ("giphy"/"klipy").
    Q_PROPERTY(int gifAutoplay READ gifAutoplay WRITE setGifAutoplay
                   NOTIFY gifAutoplayChanged)
    Q_PROPERTY(int gifSafeSearch READ gifSafeSearch WRITE setGifSafeSearch
                   NOTIFY gifSafeSearchChanged)
    Q_PROPERTY(bool storeRecentGifs READ storeRecentGifs
                   WRITE setStoreRecentGifs NOTIFY storeRecentGifsChanged)
    Q_PROPERTY(QString gifPreferredProvider READ gifPreferredProvider
                   WRITE setGifPreferredProvider NOTIFY gifPreferredProviderChanged)
    Q_PROPERTY(bool showRoomActivity READ showRoomActivity
                   WRITE setShowRoomActivity NOTIFY showRoomActivityChanged)
    // v0.5.19: discrete mouse-wheel scroll speed for the timeline. Stored as a
    // stable integer matching TimelineScrollController::WheelSpeed
    // (0=Standard, 1=Fast, 2=Very fast). Default and safe fallback: Fast.
    Q_PROPERTY(int timelineWheelSpeed READ timelineWheelSpeed
                   WRITE setTimelineWheelSpeed NOTIFY timelineWheelSpeedChanged)
    Q_PROPERTY(bool hasSession READ hasSession NOTIFY sessionChanged)
    Q_PROPERTY(QString userId READ userId NOTIFY sessionChanged)
    Q_PROPERTY(QString secretBackendName READ secretBackendName NOTIFY secretBackendChanged)
    Q_PROPERTY(bool secretsAreSecure READ secretsAreSecure NOTIFY secretBackendChanged)

public:
    // Semantic appearance presets. Ids are stable across releases so stored
    // selections keep working; AppTheme.qml resolves each id to a full
    // palette. DarkTheme (2) was a legacy alias of Midnight Blue before 0.7
    // and is now the distinct Lightning Dark palette.
    enum Theme {
        SystemTheme = 0,
        LightTheme = 1,         // Lightning Light
        DarkTheme = 2,          // Lightning Dark
        GraphiteTheme = 3,
        MidnightBlueTheme = 4,  // Midnight
        NordTheme = 5,          // Nordic
        PurpleDuskTheme = 6,
        WarmTheme = 7,
        MossLightTheme = 8,     // design-handoff light
        IndigoNightTheme = 9,   // design-handoff dark
        DeepTealTheme = 10,     // design-handoff dark
    };
    Q_ENUM(Theme)

    // Highest valid Theme id; an out-of-range stored value falls back to
    // SystemTheme (see theme()).
    static constexpr int kMaxThemeId = DeepTealTheme;

    explicit SettingsManager(QObject *parent = nullptr);

    // Inject the process-wide SecretStore. Must be called once, immediately
    // after construction, before any accessToken read/save. When set, any
    // pre-existing plaintext access token in QSettings is migrated into the
    // store and the plaintext key is deleted.
    void setSecretStore(SecretStore *store);
    SecretStore *secretStore() const { return m_secretStore; }

    QString homeserverUrl() const;
    void setHomeserverUrl(const QString &url);

    Theme theme() const;
    void setTheme(Theme t);

    // Message layout ids (see Q_PROPERTY note). Out-of-range values read
    // back as Modern.
    static constexpr int kMaxMessageLayout = 2;
    int messageLayout() const;
    void setMessageLayout(int layout);

    // Text scale percent. Out-of-range values read back as 100.
    static constexpr int kMinTextScale = 90;
    static constexpr int kMaxTextScale = 140;
    int textScale() const;
    QString uiFont() const;
    void setUiFont(const QString &family);
    // The curated selectable UI families (bundled, OFL).
    Q_INVOKABLE static QStringList uiFontChoices();
    void setTextScale(int percent);

    QString language() const;
    void setLanguage(const QString &lang);

    bool startMinimized() const;
    void setStartMinimized(bool v);

    bool notificationsEnabled() const;
    int notificationPreview() const;
    int notificationSound() const;
    void setNotificationSound(int mode);
    void setNotificationPreview(int mode);
    // v0.6.0 checkpoint 11: LOCAL per-room notification mode (0 = all
    // messages, 1 = mentions only, 2 = mute). Explicitly this-device-only —
    // it is NOT synchronized to server push rules.
    Q_INVOKABLE int roomNotificationMode(const QString &roomId) const;
    Q_INVOKABLE void setRoomNotificationMode(const QString &roomId, int mode);
    void setNotificationsEnabled(bool v);

    // v0.5.11: link-preview policy (see Q_PROPERTY block).
    bool autoLoadLinkPreviews() const;
    void setAutoLoadLinkPreviews(bool v);
    bool loadPreviewsInEncryptedRooms() const;
    void setLoadPreviewsInEncryptedRooms(bool v);
    bool animateGifPreviews() const;
    void setAnimateGifPreviews(bool v);
    // v0.6.1: GIF browser policy.
    int gifAutoplay() const;
    void setGifAutoplay(int mode);
    int gifSafeSearch() const;
    void setGifSafeSearch(int rating);
    bool storeRecentGifs() const;
    void setStoreRecentGifs(bool v);
    QString gifPreferredProvider() const;
    void setGifPreferredProvider(const QString &id);
    bool showRoomActivity() const;
    void setShowRoomActivity(bool v);

    // v0.5.19: timeline discrete-wheel speed. 0=Standard, 1=Fast, 2=Very fast.
    // An out-of-range or legacy value reads back as Fast (1).
    static constexpr int kDefaultTimelineWheelSpeed = 1; // Fast
    int timelineWheelSpeed() const;
    void setTimelineWheelSpeed(int v);

    QStringList recentEmoji() const;
    void recordRecentEmoji(const QString &emoji);
    void clearRecentEmoji();
    QString preferredEmojiTone() const;
    void setPreferredEmojiTone(const QString &tone);

    // Session storage.
    //
    // v0.4: accessToken lives in the SecretStore (libsecret when available,
    // insecure QSettings fallback otherwise). Non-secret session metadata
    // stays in QSettings — syncToken is not a credential but
    // restart-recoverable state.
    //
    // v0.7: session metadata is stored per account under accounts/<slug>/
    // so several signed-in accounts coexist; accounts/active names the one
    // the UI is currently showing. userId()/deviceId()/syncToken() and
    // accessToken() are views of the ACTIVE account. Tokens remain in the
    // SecretStore keyed by the full Matrix user id, never in QSettings.
    bool hasSession() const;
    QString accessToken() const;
    QString userId() const;
    QString deviceId() const;
    QString syncToken() const;

    // Multi-account registry.
    //
    // Records are keyed by the safe account slug derived from the full MXID
    // (see matrix::app_data::safeUserSlug); a malformed or unsafe user id is
    // rejected rather than guessed at. Ordering is oldest-added first.
    QStringList savedAccountUserIds() const;
    bool hasSavedAccount(const QString &userId) const;
    // True when a DIFFERENT saved account occupies this identity's slug —
    // the slug substitution is not injective, and colliding identities
    // would otherwise alias one settings record and one on-disk SDK store.
    // Logins for a colliding identity must be refused.
    bool accountSlugConflicts(const QString &userId) const;
    // {userId, homeserver, deviceId, displayName, avatarUrl, addedAt}
    // — empty map when the account is unknown. syncToken is deliberately
    // not exposed here.
    QVariantMap accountRecord(const QString &userId) const;
    // Access token for a specific saved account (SecretStore lookup).
    QString accessTokenFor(const QString &userId) const;
    QString activeAccountUserId() const;
    // Selects which saved account the session accessors describe. An empty
    // id or an id without a saved record clears the selection.
    void setActiveAccountUserId(const QString &userId);
    // Cache the account's own display name / avatar for the account UI.
    void updateAccountProfile(const QString &userId,
                              const QString &displayName,
                              const QString &avatarUrl);

    // True iff the process is using a native, secure secret backend.
    bool secretsAreSecure() const;
    QString secretBackendName() const;

    void saveSession(const QString &homeserverUrl,
                     const QString &userId,
                     const QString &deviceId,
                     const QString &accessToken);
    void setSyncToken(const QString &token);
    // Clear the active session, including stale metadata whose token is
    // already absent. Returns false only when the account's SecretStore
    // entries could not be removed; non-secret metadata is still cleared.
    bool clearSession();

    // Account-scoped variant used by signed-out reset. It always clears
    // secrets for `userId`, but removes the global active-session metadata
    // only when that metadata belongs to the same account.
    bool clearSessionForAccount(const QString &userId);

Q_SIGNALS:
    void homeserverUrlChanged();
    void themeChanged();
    void messageLayoutChanged();
    void textScaleChanged();
    void uiFontChanged();
    void languageChanged();
    void startMinimizedChanged();
    void notificationsEnabledChanged();
    void notificationPreviewChanged();
    void notificationSoundChanged();
    void roomNotificationModeChanged(const QString &roomId);
    void autoLoadLinkPreviewsChanged();
    void loadPreviewsInEncryptedRoomsChanged();
    void animateGifPreviewsChanged();
    void gifAutoplayChanged();
    void gifSafeSearchChanged();
    void storeRecentGifsChanged();
    void gifPreferredProviderChanged();
    void showRoomActivityChanged();
    void timelineWheelSpeedChanged();
    void sessionChanged();
    void secretBackendChanged();
    // A saved-account record was added, removed, or updated.
    void accountsChanged();

private:
    void migratePlaintextTokenIfPresent();
    void migrateLegacySessionRecord();
    // Per-account appearance storage: reads prefer the active account's
    // value, writes update the account AND the global fallback (so the
    // logged-out shell keeps the most recent selection).
    QVariant appearanceValue(const char *globalKey,
                             const QVariant &fallback) const;
    void setAppearanceValue(const char *globalKey, const QVariant &value);
    QString accountKey(const QString &slug, const char *subKey) const;
    QString slugForSavedAccount(const QString &userId) const;
    bool upsertAccountRecord(const QString &userId,
                             const QString &homeserver,
                             const QString &deviceId);

    std::unique_ptr<QSettings> m_store;
    SecretStore *m_secretStore = nullptr; // not owned; lifetime = process
};
