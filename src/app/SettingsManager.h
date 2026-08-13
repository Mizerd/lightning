#pragma once

#include "storage/AppDataPaths.h"

#include <QObject>
#include <QSettings>
#include <QSize>
#include <QString>
#include <memory>

class SecretStore;

class SettingsManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString homeserverUrl READ homeserverUrl WRITE setHomeserverUrl NOTIFY homeserverUrlChanged)
    // Login-screen homeserver prefill. Deliberately account-INDEPENDENT: the
    // add-account flow keeps the current account active, so binding the login
    // field to homeserverUrl (which returns the active account's server)
    // meant the field always reverted to "your own" server and could not be
    // pointed at a different homeserver. This reads/writes the same global
    // prefill key so the field reflects exactly what the user types.
    Q_PROPERTY(QString loginHomeserverPrefill READ loginHomeserverPrefill
                   WRITE setLoginHomeserverPrefill
                   NOTIFY loginHomeserverPrefillChanged)
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
    // Custom application icon (Settings -> Appearance). Device-global like
    // language/startMinimized — the window icon is process-wide and applies
    // before any account restores. The normalized image itself lives at
    // matrix::app_data::customAppIconFile(); this flag only records that the
    // user enabled it. AppController owns validation/application.
    Q_PROPERTY(bool customAppIconEnabled READ customAppIconEnabled
                   WRITE setCustomAppIconEnabled NOTIFY customAppIconEnabledChanged)
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
    //
    // NOTE the name is historical. `gifAutoplay` (stored key "gif/autoplay")
    // has governed ALL passive media since the 2026-08-12 perf round — GIF
    // animation, the picker's autoplay, and the speculative video/audio
    // prefetch — so 2=Never means "no passive downloads at all", not merely
    // "still GIFs". The UI presents it as "Autoplay and prefetch media". The
    // property and key keep their old names ON PURPOSE: renaming the stored
    // key would silently reset every existing user's preference to the
    // default, which is a worse outcome than a slightly stale identifier.
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
        StormTheme = 11,        // brand navy + bolt yellow (0.6.5 Storm)
    };
    Q_ENUM(Theme)

    // Highest valid Theme id; an out-of-range stored value falls back to
    // SystemTheme (see theme()).
    static constexpr int kMaxThemeId = StormTheme;

    explicit SettingsManager(QObject *parent = nullptr);

    // Inject the process-wide SecretStore. Must be called once, immediately
    // after construction, before any accessToken read/save. When set, any
    // pre-existing plaintext access token in QSettings is migrated into the
    // store and the plaintext key is deleted.
    void setSecretStore(SecretStore *store);
    SecretStore *secretStore() const { return m_secretStore; }

    QString homeserverUrl() const;
    void setHomeserverUrl(const QString &url);

    QString loginHomeserverPrefill() const;
    void setLoginHomeserverPrefill(const QString &url);

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
    bool customAppIconEnabled() const;
    void setCustomAppIconEnabled(bool enabled);
    void setStartMinimized(bool v);

    bool notificationsEnabled() const;
    int notificationPreview() const;
    int notificationSound() const;
    void setNotificationSound(int mode);
    void setNotificationPreview(int mode);
    // v0.6.0 checkpoint 11: per-room notification mode (0 = all messages,
    // 1 = mentions & keywords, 2 = mute). On backends WITHOUT server
    // push-rule support this stays a pure this-device setting. On the Rust
    // backend it is the device-local CACHE of the account's server
    // push-rule mode: AppController::setRoomNotificationMode writes it
    // optimistically with each user choice and reconciles it from the
    // backend's USER-DEFINED roomNotificationModeChanged reports (server
    // wins for explicit rules; resolved account defaults are never
    // persisted). Stored per account with a lazy read-fallback to the
    // legacy device-global key; mode 0 still removes the stored key where
    // no legacy value needs shadowing (compact settings file). The
    // explicit server rule lives in the account's push rules, never here.
    Q_INVOKABLE int roomNotificationMode(const QString &roomId) const;
    Q_INVOKABLE void setRoomNotificationMode(const QString &roomId, int mode);

    // v0.7: learned video dimensions for events whose Matrix metadata
    // declares none (every Lightning-sent video before the send-metadata
    // fix). Recorded when the poster extractor sees the real frame, so the
    // timeline card takes its true shape from the FIRST render on every
    // later visit instead of guessing 16:9 and resizing when the poster
    // lands. Account-scoped, keyed by a hash of the media key (raw event
    // ids never become settings keys), bounded by an LRU index — only
    // dimensions are stored, never content. Returns an empty size when
    // nothing is recorded.
    Q_INVOKABLE QSize knownVideoDimensions(const QString &mediaKey) const;
    void setKnownVideoDimensions(const QString &mediaKey, int width,
                                 int height);
    // Learned payload size (bytes) for media whose event declares none —
    // recorded from the first real fetch, so the bounded speculative
    // prefetch (and with it the poster) works on every later session for
    // the pre-metadata-fix backlog after a single play. Same hashed-key +
    // LRU discipline as the dimensions. 0 when unknown.
    Q_INVOKABLE double knownMediaSizeBytes(const QString &mediaKey) const;
    void setKnownMediaSizeBytes(const QString &mediaKey, qint64 bytes);

    // v0.6.7: remembered size of a user-resizable overlay picker (the GIF and
    // emoji pickers, which carry a drag grip), stored as a SHARE of the space
    // available to it — per mille, 50..1000 — never as a pixel count.
    //
    // A share is what makes the picker track the window as it is resized, keeps
    // a size chosen on one display sensible on another, and lets both pickers
    // remember ONE value despite having different proportions (they pass the
    // same id, so resizing either resizes both).
    //
    // `id` is checked against a small WHITELIST before any key is composed, so
    // a QML caller can never reach an arbitrary settings key; an unknown id
    // reads 0 and writes nothing at all. 0 means "never resized — use the
    // component's default share", which is also what an out-of-range stored
    // value degrades to: a corrupted or hand-edited store must not be able to
    // produce a degenerate or off-screen picker. These are plain local UI
    // preferences and carry no account, room or Matrix data.
    Q_INVOKABLE int pickerWidthShare(const QString &id) const;
    Q_INVOKABLE int pickerHeightShare(const QString &id) const;
    Q_INVOKABLE void setPickerShare(const QString &id, int widthPerMille,
                                    int heightPerMille);
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
    // Map a TYPED login identity onto the server-canonical user id already
    // saved for that account.
    //
    // Matrix localparts are case-sensitive, so `resolveAccountIdentity()`
    // preserves the typed case while the homeserver answers a login with its
    // own canonical id. A user who types "Mizerd" therefore produced a store
    // under `Mizerd_<server>` and a record under `mizerd_<server>`, and every
    // later restore looked for a store that was never there. Resolving the
    // typed id against the saved records first makes repeat logins land on
    // the account that already exists.
    //
    // Matching is exact on the (already normalized, lowercase) server name
    // and case-insensitive on the localpart. Returns an empty string when no
    // saved account matches OR when two or more do — an ambiguous match is
    // never resolved by guessing. `ambiguous` distinguishes the two.
    //
    // Deliberately NOT folded into slugForSavedAccount()/hasSavedAccount():
    // those must stay exact so deviceId(), syncToken() and the per-account
    // appearance accessors cannot silently read another record.
    QString canonicalUserIdForTypedIdentity(const QString &typedUserId,
                                            bool *ambiguous = nullptr) const;
    // {userId, homeserver, deviceId, displayName, avatarUrl, addedAt}
    // — empty map when the account is unknown. syncToken is deliberately
    // not exposed here.
    QVariantMap accountRecord(const QString &userId) const;
    // Access token for a specific saved account (SecretStore lookup).
    QString accessTokenFor(const QString &userId) const;

    // Where this account's Rust SDK store actually lives, as recorded at
    // login from the directory that was really opened. Empty means "never
    // recorded" — the canonical slug is then the best available guess.
    //
    // This mapping exists because it must NOT be derived twice: the store
    // path used to come from the typed login name and the account record from
    // the server-canonical user id, and when those disagreed (localpart
    // casing, or a delegated .well-known server name) restore looked for a
    // store that was never there, logout deleted a directory that did not
    // exist while the real one survived, and reset cleared the wrong slug.
    QString storeSlugFor(const QString &userId) const;
    // The saved account that could legitimately own the store directory
    // `storeSlug`, or empty when none can.
    //
    // Checks all three ways an account can be bound to a directory: its
    // canonical slug, its recorded storeSlug, and the slug an older build
    // would have derived for it from its homeserver URL
    // (delegatedHomeserverStoreSlug). Anything that deletes a store MUST
    // consult this first — "no record under the slug I derived" is not the
    // same as "no account owns this directory", and treating them as
    // equivalent is what destroyed a real crypto store.
    QString accountOwningStoreSlug(const QString &storeSlug) const;
    // True when the secret backend cannot answer AT ALL — no store injected,
    // keyring locked, session bus unavailable. Distinct from "this account
    // has no token": an unreadable backend makes every lookup come back
    // empty, and treating that as "the sign-in is gone" is the same
    // conflation that let the login path destroy a live crypto store.
    // Anything that classifies a missing token MUST consult this first.
    bool secretBackendUnavailable() const;
    void setStoreSlugFor(const QString &userId, const QString &storeSlug);
    // Resolve a saved account into a full identity whose on-disk paths point
    // at the recorded store. Use this anywhere an account's files are read,
    // deleted, or opened; re-deriving the path is the original defect.
    bool resolveSavedIdentity(const QString &userId,
                              matrix::app_data::AccountIdentity *out) const;
    QString activeAccountUserId() const;
    // Selects which saved account the session accessors describe. An empty
    // id or an id without a saved record clears the selection.
    void setActiveAccountUserId(const QString &userId);
    // Cache the account's own display name / avatar for the account UI.
    void updateAccountProfile(const QString &userId,
                              const QString &displayName,
                              const QString &avatarUrl);

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // Development-only (screenshot demo): register a fictional account as
    // NON-SECRET metadata only — it never writes a token and never touches the
    // SecretStore. `order` fixes a deterministic addedAt so the account-switcher
    // ordering is stable across launches. Everything lands in the isolated demo
    // QSettings profile (the demo applicationName). Compiled out of every
    // normal/release build.
    void registerDemoAccount(const QString &homeserverUrl, const QString &userId,
                             const QString &displayName, const QString &avatarUrl,
                             int order);
    // Development-only: drop all fictional demo account records (used before a
    // deterministic re-registration and by "reset all demo state").
    void clearDemoAccounts();
#endif

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
    // Same, but reports whether a saved record was actually matched and
    // removed. The plain overload cannot distinguish "cleared the account"
    // from "matched nothing and did nothing": SecretStore backends treat a
    // no-op clear as success, so a reset aimed at an unknown identity used to
    // report "Local Lightning session reset" while the real record, token and
    // active-account pointer stayed exactly where they were.
    bool clearSessionForAccount(const QString &userId, bool *matchedRecord);

Q_SIGNALS:
    void homeserverUrlChanged();
    void loginHomeserverPrefillChanged();
    void themeChanged();
    void messageLayoutChanged();
    void textScaleChanged();
    void uiFontChanged();
    void languageChanged();
    void startMinimizedChanged();
    void customAppIconEnabledChanged();
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
    void migrateInsecureSecretsGroup();
    void migrateLegacySessionRecord();
    // Per-account appearance storage: reads prefer the active account's
    // value, writes update the account AND the global fallback (so the
    // logged-out shell keeps the most recent selection).
    QVariant appearanceValue(const char *globalKey,
                             const QVariant &fallback) const;
    void setAppearanceValue(const char *globalKey, const QVariant &value);
    QString accountKey(const QString &slug, const char *subKey) const;
    QString slugForSavedAccount(const QString &userId) const;
    // Per-room notification-mode keys: the account-scoped key (empty when
    // no account is active) and the legacy device-global fallback key.
    static QString roomNotificationModeGlobalKey(const QString &roomId);
    QString roomNotificationModeScopedKey(const QString &roomId) const;
    // Learned-media store internals: one LRU index covers both the
    // dimension and payload-size keys of an entry.
    static QString mediaInfoIndexKeyForSlug(const QString &slug);
    void touchMediaInfoIndex(const QString &slug, const QString &hash);
    // Hot-path slug lookup: roomNotificationMode() runs for every appended
    // remote event (NotificationManager context), so the active account's
    // slug is cached keyed by the active user id. An account can only
    // become active while its record exists (setActiveAccountUserId
    // checks), and every removal path clears the active id, so keying by
    // the user id is sufficient invalidation; setActiveAccountUserId also
    // clears the cache explicitly, belt and braces.
    QString activeAccountSlugCached() const;
    bool upsertAccountRecord(const QString &userId,
                             const QString &homeserver,
                             const QString &deviceId);

    std::unique_ptr<QSettings> m_store;
    SecretStore *m_secretStore = nullptr; // not owned; lifetime = process
    mutable QString m_activeSlugCacheUserId;
    mutable QString m_activeSlugCache;
};
