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
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(bool startMinimized READ startMinimized WRITE setStartMinimized NOTIFY startMinimizedChanged)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsEnabledChanged)
    // v0.6.0 checkpoint 11: notification privacy. 0 = sender and message,
    // 1 = sender only (default), 2 = private ("New Matrix notification").
    Q_PROPERTY(int notificationPreview READ notificationPreview
                   WRITE setNotificationPreview NOTIFY notificationPreviewChanged)
    // v0.5.11: link previews. Encrypted-room previews default OFF (privacy).
    Q_PROPERTY(bool autoLoadLinkPreviews READ autoLoadLinkPreviews
                   WRITE setAutoLoadLinkPreviews NOTIFY autoLoadLinkPreviewsChanged)
    Q_PROPERTY(bool loadPreviewsInEncryptedRooms READ loadPreviewsInEncryptedRooms
                   WRITE setLoadPreviewsInEncryptedRooms
                   NOTIFY loadPreviewsInEncryptedRoomsChanged)
    Q_PROPERTY(bool animateGifPreviews READ animateGifPreviews
                   WRITE setAnimateGifPreviews NOTIFY animateGifPreviewsChanged)
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
    // v0.5.11: semantic appearance presets. Values 0–2 are retained for
    // backward compatibility with sessions saved before 0.5.11 (DarkTheme
    // renders as the Midnight Blue palette). AppTheme.qml resolves each id
    // to a full palette.
    enum Theme {
        SystemTheme = 0,
        LightTheme = 1,
        DarkTheme = 2,          // legacy alias → Midnight Blue
        GraphiteTheme = 3,
        MidnightBlueTheme = 4,
        NordTheme = 5,
        PurpleDuskTheme = 6,
    };
    Q_ENUM(Theme)

    // Highest valid Theme id; an out-of-range stored value falls back to
    // SystemTheme (see theme()).
    static constexpr int kMaxThemeId = PurpleDuskTheme;

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

    QString language() const;
    void setLanguage(const QString &lang);

    bool startMinimized() const;
    void setStartMinimized(bool v);

    bool notificationsEnabled() const;
    int notificationPreview() const;
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
    // (userId, deviceId, homeserverUrl, syncToken) stays in QSettings —
    // syncToken is not a credential but restart-recoverable state.
    bool hasSession() const;
    QString accessToken() const;
    QString userId() const;
    QString deviceId() const;
    QString syncToken() const;

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
    void languageChanged();
    void startMinimizedChanged();
    void notificationsEnabledChanged();
    void notificationPreviewChanged();
    void roomNotificationModeChanged(const QString &roomId);
    void autoLoadLinkPreviewsChanged();
    void loadPreviewsInEncryptedRoomsChanged();
    void animateGifPreviewsChanged();
    void showRoomActivityChanged();
    void timelineWheelSpeedChanged();
    void sessionChanged();
    void secretBackendChanged();

private:
    void migratePlaintextTokenIfPresent();

    std::unique_ptr<QSettings> m_store;
    SecretStore *m_secretStore = nullptr; // not owned; lifetime = process
};
