#pragma once

#include "storage/AppDataPaths.h"

#include <QObject>
#include <QRect>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
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
    /// How the room-list column is organised: 0 = Classic (one activity-
    /// ordered list with DM/Rooms sections), 1 = Channels (the active
    /// Space's own hierarchy, categories and channels, in the order its
    /// admin built).
    ///
    /// Account-scoped through appearanceValue like the other Appearance
    /// choices, so someone whose work account is a Space-heavy workspace and
    /// whose personal account is a handful of DMs is not forced into one
    /// shape for both.
    Q_PROPERTY(int roomNavigationLayout READ roomNavigationLayout
                   WRITE setRoomNavigationLayout
                   NOTIFY roomNavigationLayoutChanged)
    // Room-list filter chips (0 All, 1 People, 2 Rooms, 3 Unreads) —
    // per-account with global fallback, like the other appearance state.
    Q_PROPERTY(int roomFilterMode READ roomFilterMode WRITE setRoomFilterMode
                   NOTIFY roomFilterModeChanged)
    Q_PROPERTY(int textScale READ textScale WRITE setTextScale
                   NOTIFY textScaleChanged)
    // UI font family (per-account with global fallback, like the rest of
    // Appearance). Stored VERBATIM after a syntactic check and nothing more:
    // this class is linked against Qt6::Core alone by ~20 test targets, so it
    // cannot ask QFontDatabase whether a family exists and must not pretend
    // to. FontManager (Qt6::Gui) resolves the name against the host and falls
    // back to the bundled face when it is missing — WITHOUT rewriting this
    // value, so a font that is uninstalled and reinstalled comes back.
    Q_PROPERTY(QString uiFont READ uiFont WRITE setUiFont
                   NOTIFY uiFontChanged)
    // The monospace family (code blocks, keycaps, Matrix identifiers). Same
    // storage and same resolution rules as uiFont; a separate setting because
    // "the face I read prose in" and "the face I read code in" are different
    // choices and always have been.
    Q_PROPERTY(QString monoFont READ monoFont WRITE setMonoFont
                   NOTIFY monoFontChanged)
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
    // 2026-08-18 round 2: whether an incoming voice call rings (the
    // repeating call notification sound). Device-wide like the other
    // notification switches. The call BANNER and the plain notification
    // are governed by notificationsEnabled; this only silences the ring.
    Q_PROPERTY(bool ringForCalls READ ringForCalls WRITE setRingForCalls
                   NOTIFY ringForCallsChanged)
    // Call device preferences. DEVICE-scoped, not account-scoped: a
    // microphone belongs to the machine, and two accounts on one desktop
    // share the same hardware. Stored as the PipeWire/Pulse node name; an
    // empty value means "system default", which is a real choice and is
    // stored as such rather than as the resolved id of the day.
    Q_PROPERTY(QString preferredMicrophoneId READ preferredMicrophoneId
                   WRITE setPreferredMicrophoneId
                   NOTIFY callDevicePreferenceChanged)
    Q_PROPERTY(QString preferredSpeakerId READ preferredSpeakerId
                   WRITE setPreferredSpeakerId
                   NOTIFY callDevicePreferenceChanged)
    Q_PROPERTY(QString preferredCameraId READ preferredCameraId
                   WRITE setPreferredCameraId
                   NOTIFY callDevicePreferenceChanged)
    // v0.5.11: link previews. Encrypted-room previews default OFF (privacy).
    Q_PROPERTY(bool autoLoadLinkPreviews READ autoLoadLinkPreviews
                   WRITE setAutoLoadLinkPreviews NOTIFY autoLoadLinkPreviewsChanged)
    Q_PROPERTY(bool loadPreviewsInEncryptedRooms READ loadPreviewsInEncryptedRooms
                   WRITE setLoadPreviewsInEncryptedRooms
                   NOTIFY loadPreviewsInEncryptedRoomsChanged)
    Q_PROPERTY(bool animateGifPreviews READ animateGifPreviews
                   WRITE setAnimateGifPreviews NOTIFY animateGifPreviewsChanged)
    // v0.7.x Matrix presence: publish this account's own online/idle state
    // to its homeserver. Default ON (the Matrix ecosystem norm — Element
    // publishes presence wherever the server enables it); disclosed and
    // switchable under Privacy & security. Viewing OTHERS' presence is
    // passive (reads against the user's own homeserver) and has no toggle.
    // Screen-share quality. Global to the computer, not the account.
    Q_PROPERTY(int shareMaxHeight READ shareMaxHeight WRITE setShareMaxHeight
                   NOTIFY shareQualityChanged)
    Q_PROPERTY(int shareFps READ shareFps WRITE setShareFps
                   NOTIFY shareQualityChanged)
    /// True when the chosen combination asks more of the encoder than it can
    /// deliver in real time. ONE predicate, read by every surface that
    /// offers the choice, so two menus cannot warn differently.
    Q_PROPERTY(bool shareQualityDemanding READ shareQualityDemanding
                   NOTIFY shareQualityChanged)
    Q_PROPERTY(bool sharePresence READ sharePresence
                   WRITE setSharePresence NOTIFY sharePresenceChanged)
    // Shell layout. Device-level, not per-account: it describes this
    // window on this screen, and an account switch must not resize it.
    //
    // Requested by a tester on Windows — "option to resize and/or hide all
    // panels, like the member list panel but for the servers/rooms panel
    // too, along with the left-most one. Screen real estate wise."
    Q_PROPERTY(bool spacesRailVisible READ spacesRailVisible
                   WRITE setSpacesRailVisible NOTIFY spacesRailVisibleChanged)
    // A Space's banner: shown at all, and shown whole or cropped to a strip.
    // Both are app-wide rather than per-Space, which is how Sable scopes the
    // same two choices — someone who does not want a 400px picture above
    // every Space does not want to say so once per Space.
    Q_PROPERTY(bool spaceBannersVisible READ spaceBannersVisible
                   WRITE setSpaceBannersVisible NOTIFY spaceBannersVisibleChanged)
    Q_PROPERTY(bool spaceBannerExpanded READ spaceBannerExpanded
                   WRITE setSpaceBannerExpanded NOTIFY spaceBannerExpandedChanged)
    Q_PROPERTY(bool roomListVisible READ roomListVisible
                   WRITE setRoomListVisible NOTIFY roomListVisibleChanged)
    Q_PROPERTY(int roomListWidth READ roomListWidth
                   WRITE setRoomListWidth NOTIFY roomListWidthChanged)
    Q_PROPERTY(int sidePanelWidth READ sidePanelWidth
                   WRITE setSidePanelWidth NOTIFY sidePanelWidthChanged)
    // The clamps, exposed so a slider cannot invent its own bounds. Written
    // because the first version of the Appearance sliders guessed 220–480
    // and 240–520 against real clamps of 200–560 and 240–640: a slider whose
    // range is NARROWER than the setter's does not snap back visibly, it
    // silently forbids widths the app supports, and a stored 560 renders the
    // handle pinned at a position that is not the stored value. CONSTANT —
    // these are compile-time bounds, not settings.
    Q_PROPERTY(int roomListMinWidth READ roomListMinWidth CONSTANT)
    Q_PROPERTY(int roomListMaxWidth READ roomListMaxWidth CONSTANT)
    Q_PROPERTY(int sidePanelMinWidth READ sidePanelMinWidth CONSTANT)
    Q_PROPERTY(int sidePanelMaxWidth READ sidePanelMaxWidth CONSTANT)
    // Closing the window puts Lightning in the system tray instead of
    // quitting. OFF by default and gated on the platform actually having a
    // tray: closing a window into a tray that does not exist is closing it
    // into nothing.
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray
                   NOTIFY closeToTrayChanged)
    Q_PROPERTY(bool startInTray READ startInTray WRITE setStartInTray
                   NOTIFY startInTrayChanged)
    // The window's own size, position and maximized state, restored on the
    // next launch.
    //
    // Read-only and CONSTANT on purpose. The window reads these in the
    // bindings that declare its geometry, and Qt writes x/y/width/height back
    // as the user drags — so a notifying property would feed the saved value
    // back into the binding that produced it. Saving goes through the
    // invokables below instead, and nothing re-reads mid-session: this is
    // "where the window was last time", answered once.
    //
    // An INVALID rect means "never saved", which is what lets the first launch
    // use the declared default size rather than a fabricated corner. The
    // stored SIZE is validated here; whether the stored POSITION still lands
    // on a connected screen is a display-layout question and is answered by
    // AppController::restorableWindowGeometry, which is the only reader.
    //
    // Only the NORMAL geometry is stored; maximized is its own flag, because a
    // maximized window's frame is the screen, and restoring that as a normal
    // size would lose the size the user actually chose.
    Q_PROPERTY(QRect initialWindowGeometry READ initialWindowGeometry CONSTANT)
    Q_PROPERTY(bool initialWindowMaximized READ initialWindowMaximized CONSTANT)
    // v0.7.x: the user dismissed the "verify this session" warning badges.
    // STRICTLY account-scoped — dismissing on one account must not silence
    // the warning for another, so this deliberately does NOT use
    // appearanceValue(), which mirrors into a shared global fallback.
    // Cleared automatically the moment the session becomes verified, so a
    // later unverified session warns again instead of inheriting a
    // dismissal that answered a different question.
    Q_PROPERTY(bool verificationWarningDismissed
                   READ verificationWarningDismissed
                   WRITE setVerificationWarningDismissed
                   NOTIFY verificationWarningDismissedChanged)
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
    // showRoomActivity SPLIT INTO ITS TWO HALVES (2026-08-26). The single
    // coarse toggle above already described itself as "joins, leaves,
    // profile changes, and room setting updates" — four different things
    // behind one switch, and the two people who ask for this ask for
    // opposite halves of it: one wants the join/leave churn of a big room
    // gone, the other wants to stop seeing "X changed their avatar" fifty
    // times a day. The Rust bridge has distinguished them all along
    // (rust/src/timeline.rs emits state_kind "membership" and
    // "member_profile"); only the FILTER conflated them, by testing that
    // the kind was non-empty rather than testing its value.
    //
    // showRoomActivity stays the master switch, so an existing user's stored
    // choice keeps meaning exactly what it meant. These two only narrow it —
    // with the master off, nothing is shown regardless.
    Q_PROPERTY(bool showMembershipEvents READ showMembershipEvents
                   WRITE setShowMembershipEvents
                   NOTIFY showMembershipEventsChanged)
    Q_PROPERTY(bool showProfileChangeEvents READ showProfileChangeEvents
                   WRITE setShowProfileChangeEvents
                   NOTIFY showProfileChangeEventsChanged)
    // Reduced motion. AppTheme has declared `reducedMotion` since the design
    // round and roughly twenty animation sites across ten QML files already
    // read it — and NOTHING ever assigned it, so every one of those branches
    // was dead. This is the assignment. Per account with a global fallback
    // like the rest of Appearance; vestibular sensitivity belongs to the
    // person, and the global value is what the logged-out shell uses.
    Q_PROPERTY(bool reducedMotion READ reducedMotion WRITE setReducedMotion
                   NOTIFY reducedMotionChanged)

    // Smooth scrolling. SEPARATE from reducedMotion on purpose: that setting
    // is an accessibility one covering every animation in the shell, and a
    // reader who simply wants the wheel to land where the OS says should not
    // have to turn the whole design's motion off to get it. Default ON, which
    // is the behaviour every build so far has had.
    Q_PROPERTY(bool smoothScrolling READ smoothScrolling WRITE setSmoothScrolling
                   NOTIFY smoothScrollingChanged)
    // Clock format for every timestamp Lightning renders: 0 = follow the
    // system locale (the previous, fixed behaviour), 1 = 12-hour, 2 =
    // 24-hour. Per account with a global fallback.
    //
    // WHY A THREE-WAY AND NOT A BOOL: "24-hour" as a bool has no state that
    // means "whatever this machine is set to", so a user in a 24-hour locale
    // would have to tick a box to keep what they already had, and a locale
    // change would stop being followed. 0 is the default and is the old
    // behaviour exactly.
    Q_PROPERTY(int clockFormat READ clockFormat WRITE setClockFormat
                   NOTIFY clockFormatChanged)
    // The Qt time-format STRING the clock setting resolves to, so every
    // timestamp in QML reads one property instead of branching on the mode.
    //
    // WHY A PROPERTY AND NOT AN INVOKABLE HELPER: a function call creates no
    // binding dependency Qt can track, so `text: app.settings.formatClock(t)`
    // would keep rendering the OLD format until the item was next created —
    // the same trap the media-cache handlers hit by assigning Image.source
    // imperatively. A property read is a real dependency, so changing the
    // setting re-renders every timestamp on screen.
    //
    // Fixes a pre-existing inconsistency at the same time: message rows
    // formatted with a literal "hh:mm" (always 24-hour, whatever the locale)
    // while the room list, threads and Home used the locale's short format.
    // Both now resolve here.
    Q_PROPERTY(QString clockTimeFormat READ clockTimeFormat
                   NOTIFY clockFormatChanged)
    // Composer: Enter inserts a newline and Ctrl+Enter sends, instead of the
    // default (Enter sends, Shift+Enter inserts a newline). Device-global —
    // it describes how a keyboard is used, not who is logged in.
    Q_PROPERTY(bool enterInsertsNewline READ enterInsertsNewline
                   WRITE setEnterInsertsNewline
                   NOTIFY enterInsertsNewlineChanged)
    // v0.9 composer mode: "markdown" (the historical source editor) or
    // "rich" (the WYSIWYG editor). Device-global like the Enter behaviour —
    // it describes how this keyboard composes, not who is logged in. Any
    // value but "rich" reads as markdown, so a downgrade can never strand
    // the composer in a mode the build does not have.
    Q_PROPERTY(QString composerMode READ composerMode WRITE setComposerMode
                   NOTIFY composerModeChanged)
    // Composer: text typed alongside an attachment is sent as that
    // attachment's CAPTION (one event) rather than as a separate message.
    // The caption parameter has been plumbed to the SDK the whole time and
    // the composer passed an empty string; this is the switch that fills it.
    // Device-global for the same reason as the Enter behaviour.
    Q_PROPERTY(bool sendTextAsCaption READ sendTextAsCaption
                   WRITE setSendTextAsCaption
                   NOTIFY sendTextAsCaptionChanged)
    // v0.5.19: discrete mouse-wheel scroll speed for the timeline. Stored as a
    // stable integer matching TimelineScrollController::WheelSpeed
    // (0=Standard, 1=Fast, 2=Very fast). Default and safe fallback: Fast.
    Q_PROPERTY(int timelineWheelSpeed READ timelineWheelSpeed
                   WRITE setTimelineWheelSpeed NOTIFY timelineWheelSpeedChanged)
    // 2026-08-18 tester report ("neatsimena audio preferencu uzdeda default
    // visada"): inline media playback volume and speed are remembered across
    // cards, rooms and restarts. GLOBAL, like the other playback policy
    // settings — a per-account playback volume is not a thing users expect.
    // Volume is a linear 0..1 factor; the rate is clamped to the same
    // 0.25..4.0 band the player UI offers.
    Q_PROPERTY(qreal mediaVolume READ mediaVolume WRITE setMediaVolume
                   NOTIFY mediaVolumeChanged)
    Q_PROPERTY(qreal mediaPlaybackRate READ mediaPlaybackRate
                   WRITE setMediaPlaybackRate NOTIFY mediaPlaybackRateChanged)
    // Whole-interface zoom percent (75..150). GLOBAL: main() turns it into
    // QT_SCALE_FACTOR before the app object exists, so it cannot be
    // per-account and only takes effect on the next launch — Qt reads the
    // scale factor exactly once at startup.
    Q_PROPERTY(int interfaceZoom READ interfaceZoom WRITE setInterfaceZoom
                   NOTIFY interfaceZoomChanged)
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
        // A user-authored palette: a sparse set of role overrides on top of
        // one of the presets above. The values live in CustomThemeStore, not
        // here — this is only the id the picker persists.
        CustomTheme = 12,
    };
    Q_ENUM(Theme)

    // Highest valid Theme id; an out-of-range stored value falls back to
    // SystemTheme (see theme()).
    static constexpr int kMaxThemeId = CustomTheme;

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
    /// 0 Classic, 1 Channels.
    static constexpr int kMaxRoomNavigationLayout = 1;
    int messageLayout() const;
    int roomNavigationLayout() const;
    void setRoomNavigationLayout(int layout);
    void setMessageLayout(int layout);
    int roomFilterMode() const;
    void setRoomFilterMode(int mode);

    // Text scale percent. Out-of-range values read back as 100.
    static constexpr int kMinTextScale = 90;
    static constexpr int kMaxTextScale = 140;
    int textScale() const;
    QString uiFont() const;
    void setUiFont(const QString &family);
    QString monoFont() const;
    void setMonoFont(const QString &family);
    // The curated selectable UI families (bundled, OFL). Still the list the
    // picker shows FIRST; it is no longer the only thing that may be stored.
    Q_INVOKABLE static QStringList uiFontChoices();
    // A family name this class is willing to persist: trimmed, non-empty,
    // bounded, and free of control characters and of the punctuation that
    // would let a name mean something to a markup or style parser downstream.
    // Returns the accepted name, or empty when the input is refused. It is
    // deliberately NOT a "does this font exist" test — see the property.
    static QString acceptableFontFamily(const QString &family);

    // File names (basenames only) of the fonts the user imported by hand.
    // DEVICE-GLOBAL, not per-account: an application font is process-wide and
    // is registered before any account restores, exactly like the custom app
    // icon. The files live in FontManager's own app-data directory; nothing
    // here is ever a path the user typed.
    QStringList importedFontFiles() const;
    void setImportedFontFiles(const QStringList &fileNames);
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
    bool ringForCalls() const;
    QString preferredMicrophoneId() const;
    void setPreferredMicrophoneId(const QString &id);
    QString preferredSpeakerId() const;
    void setPreferredSpeakerId(const QString &id);
    QString preferredCameraId() const;
    void setPreferredCameraId(const QString &id);
    void setRingForCalls(bool enabled);
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

    // v0.7.x composer drafts, UNENCRYPTED rooms only — DraftStore enforces
    // that policy and never routes encrypted-room plaintext here (QSettings
    // is weaker than even the CacheStore this project already refuses to
    // put such plaintext in). Strictly account-scoped keys with NO global
    // fallback (`accounts/<slug>/drafts/<sha16>`), bounded by an LRU index
    // (the videoDims discipline); the whole family is wiped with the
    // account group on removal/sign-out. An empty map removes the entry.
    QVariantMap roomDraft(const QString &draftKey) const;
    void setRoomDraft(const QString &draftKey, const QVariantMap &draft);

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
    /// Screen-share ceiling, in scanlines: 720, 1080 or 1440. Width follows
    /// from 16:9, and the source is never upscaled, so a smaller screen
    /// still sends its own size under a larger ceiling.
    int shareMaxHeight() const;
    void setShareMaxHeight(int v);
    /// Screen-share frame rate: 15, 30 or 60.
    int shareFps() const;
    void setShareFps(int v);
    /// Whether the chosen height and rate together are beyond what a
    /// software VP8 encoder can sustain. See the definition for the sums.
    bool shareQualityDemanding() const;
    /// The same rule asked about a combination that is not the current one,
    /// so a menu row can mark ITSELF rather than each surface re-deriving
    /// the policy.
    Q_INVOKABLE bool shareQualityDemandingAt(int maxHeight, int fps) const;

    bool sharePresence() const;
    bool spacesRailVisible() const;
    bool spaceBannersVisible() const;
    bool spaceBannerExpanded() const;
    void setSpacesRailVisible(bool v);
    void setSpaceBannersVisible(bool v);
    void setSpaceBannerExpanded(bool v);
    bool roomListVisible() const;
    void setRoomListVisible(bool v);
    int roomListWidth() const;
    void setRoomListWidth(int px);
    int sidePanelWidth() const;
    void setSidePanelWidth(int px);
    bool closeToTray() const;
    void setCloseToTray(bool v);
    bool startInTray() const;
    void setStartInTray(bool v);
    QRect initialWindowGeometry() const { return m_initialWindowGeometry; }
    bool initialWindowMaximized() const { return m_initialWindowMaximized; }
    // Both refuse a value that could not be restored, rather than storing one
    // that would be discarded on the next read. Qt reports transient 0x0
    // geometry while a window is being shown, hidden into the tray or
    // restored from minimized, and the tray path fires exactly when the last
    // good value has to survive.
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height);
    Q_INVOKABLE void saveWindowMaximized(bool maximized);
    // The bounds QML resizes within, so the clamp lives in ONE place rather
    // than being retyped in a Layout binding that can drift from it.
    static constexpr int kRoomListMinWidth = 200;
    static constexpr int kRoomListMaxWidth = 560;
    static constexpr int kSidePanelMinWidth = 240;
    static constexpr int kSidePanelMaxWidth = 640;
    static constexpr int roomListMinWidth() { return kRoomListMinWidth; }
    static constexpr int roomListMaxWidth() { return kRoomListMaxWidth; }
    static constexpr int sidePanelMinWidth() { return kSidePanelMinWidth; }
    static constexpr int sidePanelMaxWidth() { return kSidePanelMaxWidth; }
    bool verificationWarningDismissed() const;
    void setVerificationWarningDismissed(bool v);
    void setSharePresence(bool v);
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
    bool showMembershipEvents() const;
    void setShowMembershipEvents(bool v);
    bool showProfileChangeEvents() const;
    void setShowProfileChangeEvents(bool v);
    bool reducedMotion() const;
    bool smoothScrolling() const;
    void setSmoothScrolling(bool v);

    // ── Call volumes ──────────────────────────────────────────────────
    //
    // KEYED BY MATRIX USER ID, never by the SFU participant identity. An
    // identity is `@user:server:DEVICE` in the legacy format and an unpadded
    // base64 sha256 in the sticky one — per DEVICE and, for the sticky form,
    // effectively per session. Keying by it would forget the setting the
    // moment the same person rejoined, which is the opposite of what was
    // asked for: "if a user A sets user B volume to 70% it stays the same in
    // next call or other room".
    //
    // STRICTLY account-scoped, with NO global fallback — unlike
    // appearanceValue, which deliberately mirrors into one. What you think a
    // person's voice should sound like is your opinion from your account; it
    // is not a fact about them, and it must not leak into another account's
    // view of the same person.
    //
    // 0..200. Above 100 is real amplification, as Discord allows: the
    // GStreamer `volume` element takes a linear factor and 2.0 is legal.
    // Clipping above 100 is the user's own choice and is theirs to hear.
    // Which images this account has hidden in the timeline.
    //
    // STRICTLY account-scoped with NO global fallback, for the same reason
    // the per-person call volume is: what you chose not to look at is your
    // choice from your account, and another account signing in on this
    // machine must not inherit it. appearanceValue would mirror it into a
    // shared fallback and do exactly that.
    //
    // Stored as a plain list of media keys. Bounded by the caller
    // (MediaVisibilityStore's cap), so the store cannot grow without end.
    QStringList hiddenMediaKeys() const;
    void setHiddenMediaKeys(const QStringList &keys);

    /// This account's playback volume for one person, 0..200. 100 when unset.
    Q_INVOKABLE int callParticipantVolume(const QString &userId) const;
    /// Persists it. Setting exactly 100 REMOVES the key rather than storing
    /// the default, so "reset" is a real reset and the store does not grow a
    /// row per person ever seen in a call.
    Q_INVOKABLE void setCallParticipantVolume(const QString &userId,
                                              int percent);

    /// Own microphone gain, 0..200, applied to what OTHERS hear. Account
    /// scoped WITH the global fallback, because unlike a per-person volume
    /// this is a fact about your own hardware and is the same on every
    /// account you sign into on this machine.
    Q_PROPERTY(int microphoneGain READ microphoneGain WRITE setMicrophoneGain
                   NOTIFY microphoneGainChanged)
    int microphoneGain() const;
    void setMicrophoneGain(int percent);

    void setReducedMotion(bool v);

    // Clock format ids. Kept as named constants so the QML combo, the
    // formatter and the clamp cannot drift apart.
    static constexpr int kClockFormatSystem = 0;
    static constexpr int kClockFormat12Hour = 1;
    static constexpr int kClockFormat24Hour = 2;
    int clockFormat() const;
    void setClockFormat(int mode);
    QString clockTimeFormat() const;

    bool enterInsertsNewline() const;
    void setEnterInsertsNewline(bool v);
    QString composerMode() const;
    void setComposerMode(const QString &mode);
    bool sendTextAsCaption() const;
    void setSendTextAsCaption(bool v);

    // ── Rebindable keyboard shortcuts ────────────────────────────────────
    // Stored per action id as QKeySequence::PortableText, per account with a
    // global fallback (the same rule theme/layout/text-scale use). An EMPTY
    // return means "no override, use the default" — which is also what an
    // id containing anything outside [A-Za-z0-9._-] returns, because such an
    // id could otherwise walk out of its own QSettings group. ShortcutRegistry
    // owns every default and every validation rule; this is storage only and
    // deliberately validates nothing about the SEQUENCE, so a future registry
    // can widen what it accepts without a migration.
    QString shortcutSequence(const QString &actionId) const;
    void setShortcutSequence(const QString &actionId, const QString &portable);
    void clearShortcutSequence(const QString &actionId);

    // v0.5.19: timeline discrete-wheel speed. 0=Standard, 1=Fast, 2=Very fast.
    // An out-of-range or legacy value reads back as Fast (1).
    static constexpr int kDefaultTimelineWheelSpeed = 1; // Fast
    static constexpr int kMinInterfaceZoom = 75;
    static constexpr int kMaxInterfaceZoom = 150;
    int interfaceZoom() const;
    void setInterfaceZoom(int percent);

    int timelineWheelSpeed() const;
    void setTimelineWheelSpeed(int v);

    qreal mediaVolume() const;
    void setMediaVolume(qreal v);
    qreal mediaPlaybackRate() const;
    void setMediaPlaybackRate(qreal v);

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
    // OAuth session material. Both are CREDENTIALS kept in the SecretStore
    // beside the access token; neither is a Q_PROPERTY and neither may reach
    // QML. Empty is a normal answer — password sessions usually have no
    // refresh token, and only OAuth accounts have a client id.
    // Write back tokens the SDK rotated during an automatic refresh. Narrow
    // on purpose — touches only the two credentials, never the sync token or
    // the active-account pointer.
    bool updateSessionTokens(const QString &userId,
                             const QString &accessToken,
                             const QString &refreshToken);
    QString refreshToken() const;
    QString refreshTokenFor(const QString &userId) const;
    QString oauthClientIdFor(const QString &userId) const;
    // Which SDK API restores this account: "password" (matrix_auth) or
    // "oauth". Not a secret — restore must be able to route correctly even
    // when the keyring cannot be read, so this lives in QSettings. Accounts
    // saved before OAuth existed report "password".
    QString authTypeFor(const QString &userId) const;
    bool isOAuthAccount(const QString &userId) const;

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

    // `refreshToken` may be empty (a password session on a server that issues
    // none). `authType` is "password" or "oauth" and decides which SDK API
    // restores the account. `oauthClientId` is the dynamic-registration id and
    // is only meaningful for OAuth accounts. The three trailing arguments are
    // defaulted so every existing password-login call site keeps its meaning:
    // password, no refresh token, no client id.
    //
    // refreshToken and oauthClientId are CREDENTIALS: they go to the
    // SecretStore, never to QSettings, never to QML, and are never logged.
    void saveSession(const QString &homeserverUrl,
                     const QString &userId,
                     const QString &deviceId,
                     const QString &accessToken,
                     const QString &refreshToken = QString(),
                     const QString &authType = QStringLiteral("password"),
                     const QString &oauthClientId = QString());
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
    void roomNavigationLayoutChanged();
    void roomFilterModeChanged();
    void textScaleChanged();
    void uiFontChanged();
    void monoFontChanged();
    void importedFontFilesChanged();
    void languageChanged();
    void startMinimizedChanged();
    void customAppIconEnabledChanged();
    void notificationsEnabledChanged();
    void notificationPreviewChanged();
    void notificationSoundChanged();
    void ringForCallsChanged();
    void callDevicePreferenceChanged();
    void roomNotificationModeChanged(const QString &roomId);
    void autoLoadLinkPreviewsChanged();
    void shareQualityChanged();
    void loadPreviewsInEncryptedRoomsChanged();
    void animateGifPreviewsChanged();
    void sharePresenceChanged();
    void spacesRailVisibleChanged();
    void spaceBannersVisibleChanged();
    void spaceBannerExpandedChanged();
    void roomListVisibleChanged();
    void roomListWidthChanged();
    void sidePanelWidthChanged();
    void closeToTrayChanged();
    void startInTrayChanged();
    void verificationWarningDismissedChanged();
    void gifAutoplayChanged();
    void gifSafeSearchChanged();
    void storeRecentGifsChanged();
    void gifPreferredProviderChanged();
    void showRoomActivityChanged();
    void showMembershipEventsChanged();
    void showProfileChangeEventsChanged();
    void reducedMotionChanged();
    void smoothScrollingChanged();
    void microphoneGainChanged();
    /// One person's stored volume changed. Carries the USER ID.
    void callParticipantVolumeChanged(const QString &userId, int percent);
    void clockFormatChanged();
    void enterInsertsNewlineChanged();
    void composerModeChanged();
    void sendTextAsCaptionChanged();
    void timelineWheelSpeedChanged();
    void mediaVolumeChanged();
    void mediaPlaybackRateChanged();
    void interfaceZoomChanged();
    void sessionChanged();
    void secretBackendChanged();
    // A saved-account record was added, removed, or updated.
    void accountsChanged();

private:
    // The custom theme is per-account appearance state like the theme, the
    // layout and the text scale, so it goes through the same
    // account-preferred / global-fallback helpers below rather than inventing
    // a second storage rule. It is a friend instead of those helpers being
    // made public, because nothing else should reach them.
    friend class CustomThemeStore;
    // The rail's arrangement is per-account appearance state for the same
    // reason: which Spaces an account has, and how someone grouped them, are
    // the same question. Same access, same rule.
    friend class RailLayoutStore;
    // Which Space folders the Channels layout has collapsed is per-account
    // appearance state for the same reason: it describes how one account's
    // Spaces are arranged on screen, and applying it to the next account's
    // rooms would be meaningless. Same access, same rule.
    friend class SpaceChannelModel;

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
    // Reads and validates the stored window geometry once, from the
    // constructor. See the CONSTANT properties above.
    void loadWindowGeometry();
    // Main.qml's own minimums. Duplicated here rather than plumbed through,
    // because the validation has to answer "could this be restored?" before
    // any window exists to ask.
    static constexpr int kWindowMinWidth = 640;
    static constexpr int kWindowMinHeight = 420;
    bool upsertAccountRecord(const QString &userId,
                             const QString &homeserver,
                             const QString &deviceId);

    std::unique_ptr<QSettings> m_store;
    SecretStore *m_secretStore = nullptr; // not owned; lifetime = process
    // Captured and validated once, in the constructor: see the CONSTANT
    // properties above for why these are not re-read.
    QRect m_initialWindowGeometry;
    bool m_initialWindowMaximized = false;
    mutable QString m_activeSlugCacheUserId;
    mutable QString m_activeSlugCache;
};
