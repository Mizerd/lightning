#pragma once

// Development-only. Compiled ONLY in a LIGHTNING_ENABLE_SCREENSHOT_DEMO build
// (see CMakeLists.txt). Drives the screenshot-demo scenario catalogue, the
// in-app control panel, window presets, theme/appearance selection, and the
// per-account selected-room memory — all through the SAME AppController /
// SettingsManager paths the normal UI uses. Never present in a release binary.

#include <QObject>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantList>

class AppController;
class MockMatrixClient;

class ScreenshotDemoController : public QObject
{
    Q_OBJECT

    // Catalogue + current selection, all bindable by the control panel.
    Q_PROPERTY(QVariantList scenarios READ scenarios CONSTANT)
    Q_PROPERTY(QVariantList accounts READ accounts CONSTANT)
    Q_PROPERTY(QVariantList themes READ themes CONSTANT)
    Q_PROPERTY(QVariantList appearances READ appearances CONSTANT)
    Q_PROPERTY(QVariantList windowSizes READ windowSizes CONSTANT)

    Q_PROPERTY(QString currentScenario READ currentScenario NOTIFY currentScenarioChanged)
    Q_PROPERTY(QString currentAccount READ currentAccount NOTIFY stateChanged)
    // The active account's non-space rooms ({id, name}), for the panel's Room
    // selector. Refreshes on any state change (account/room switch).
    Q_PROPERTY(QVariantList currentRooms READ currentRooms NOTIFY stateChanged)
    Q_PROPERTY(QString currentAccountName READ currentAccountName NOTIFY stateChanged)
    Q_PROPERTY(QString currentRoom READ currentRoom NOTIFY stateChanged)
    Q_PROPERTY(int currentTheme READ currentTheme NOTIFY stateChanged)
    Q_PROPERTY(QString currentSizeLabel READ currentSizeLabel NOTIFY stateChanged)
    Q_PROPERTY(int requestedWidth READ requestedWidth NOTIFY windowSizeChanged)
    Q_PROPERTY(int requestedHeight READ requestedHeight NOTIFY windowSizeChanged)

    Q_PROPERTY(bool controlsVisible READ controlsVisible WRITE setControlsVisible
                   NOTIFY controlsVisibleChanged)
    Q_PROPERTY(bool typingEnabled READ typingEnabled NOTIFY toggleStateChanged)
    Q_PROPERTY(bool unreadBadgesEnabled READ unreadBadgesEnabled
                   NOTIFY toggleStateChanged)

public:
    ScreenshotDemoController(AppController *app, MockMatrixClient *mock,
                             QObject *parent = nullptr);

    QVariantList scenarios() const;
    QVariantList accounts() const;
    QVariantList themes() const;
    QVariantList appearances() const;
    QVariantList windowSizes() const;

    QString currentScenario() const { return m_currentScenario; }
    QString currentAccount() const;
    QString currentAccountName() const;
    QString currentRoom() const;
    QVariantList currentRooms() const;
    int currentTheme() const;
    QString currentSizeLabel() const { return m_sizeLabel; }
    int requestedWidth() const { return m_requestedWidth; }
    int requestedHeight() const { return m_requestedHeight; }
    bool controlsVisible() const { return m_controlsVisible; }
    bool typingEnabled() const { return m_typingEnabled; }
    bool unreadBadgesEnabled() const { return m_unreadEnabled; }

    // Map a theme name ("ocean"/"midnight"/"violet"/"dark"/…) to a
    // SettingsManager::Theme id, or -1 if unknown. Static so the launcher CLI
    // and the panel share one mapping.
    static int themeIdForName(const QString &name);
    // Map a size preset ("1440x900"/"narrow"/"wide"/…) to width/height, or
    // false if unsupported/unsafe. Static so the CLI can validate it too.
    static bool sizeForPreset(const QString &preset, int *w, int *h);
    static bool isValidScenario(const QString &id);
    static QStringList scenarioIds();
    // The account a scenario runs on (full user id), or empty if unknown. Lets
    // the launcher restore that account directly at boot so a cross-account
    // launch scenario lands instantly instead of switching after the fact.
    static QString scenarioAccount(const QString &id);

    // Apply the launcher/CLI-provided initial demo options once, after the
    // first frame is up. Empty strings are ignored.
    void applyLaunchOptions(const QString &scenario, const QString &theme,
                            const QString &appearance, const QString &size,
                            bool hideControls);

public Q_SLOTS:
    // One-click scenario activation: performs ALL navigation (account, room,
    // space, thread/settings/switcher page, theme, appearance, window size,
    // typing, controls) deterministically.
    void activateScenario(const QString &id);

    void setAccount(const QString &userId);
    void setRoom(const QString &roomId);
    void setSpace(const QString &spaceId);
    void setTheme(int themeId);
    void setThemeByName(const QString &name);
    void setAppearance(const QString &mode);   // "light"/"dark"/"system"
    void setWindowSize(const QString &preset);
    void setControlsVisible(bool visible);
    void toggleControls();
    void setTypingEnabled(bool on);
    void setUnreadBadgesEnabled(bool on);

    void resetScenario();
    void resetAllDemoState();

    void openSettings();
    void openSettingsAppearance();
    void openSecurity();
    void openAccountSwitcher();
    void openThreadPanel();

Q_SIGNALS:
    void currentScenarioChanged();
    void stateChanged();
    void controlsVisibleChanged();
    void toggleStateChanged();
    void windowSizeChanged();
    // The window can only be resized from QML (there is no C++ handle). The
    // panel/Main connects this to Window.window width/height (imperative, so
    // the user can still resize afterwards).
    void windowSizeRequested(int width, int height);
    // Open the real account-switcher popover (a QML-local element on the rail).
    void accountSwitcherRequested();

    // Development-only demo-scenario popup hooks. Each Scenario may name one
    // of these in its `popup` field; activateScenario() fires the matching
    // signal once navigation to the scenario's room/section has settled
    // (same spirit as accountSwitcherRequested above). A production build
    // never emits these (the whole class is compiled out); QML Connections
    // with `enabled: app.screenshotDemoActive` and a null target in a
    // non-demo build make wiring them an inert no-op there too. The surface
    // that owns each popup/dialog decides which item to target (e.g. which
    // message/room row, which room member) — the controller only says
    // "open now", never which instance.
    void demoOpenMessageContextMenu();
    void demoOpenRoomContextMenu();
    void demoOpenFindBar(const QString &query);
    void demoOpenQuickSwitcher(const QString &query);
    void demoOpenEmojiPicker();
    void demoOpenGifPicker();
    void demoOpenMemberProfile();
    void demoOpenMentionPopup(const QString &prefix);
    void demoOpenTrustCard();
    void demoOpenNewConversation();
    void demoFocusSettingsSearch(const QString &query);
    void demoOpenInvitePeople();
    void demoOpenCreatePoll();

private:
    struct Scenario;
    static const QList<Scenario> &catalogue();
    const Scenario *findScenario(const QString &id) const;
    void applyScenarioNavigation(const Scenario &s);
    // Fire the Scenario's `popup` signal (if any) one event-loop tick after
    // navigation, guarded against a newer scenario having taken over in the
    // meantime. `scenarioId` is the owning scenario id (staleness check),
    // `popup` the field value, `query` its optional seed text.
    void dispatchScenarioPopup(const QString &scenarioId, const QString &popup,
                               const QString &query);
    // Seed local, demo-only state so a popup scenario has something to show
    // (emoji recents / a favorited GIF) instead of an empty section. Both
    // are idempotent — safe to call on every activation/reset.
    void seedDemoEmojiRecents();
    void seedDemoGifFavorite();
    void applyLaunchNow();
    void rememberSelectedRoom();
    void restoreSelectedRoom();
    QString accountDisplayName(const QString &userId) const;

    AppController *m_app = nullptr;
    MockMatrixClient *m_mock = nullptr;
    QString m_currentScenario;
    QString m_sizeLabel;
    int m_requestedWidth = 1440;
    int m_requestedHeight = 900;
    bool m_controlsVisible = true;
    bool m_typingEnabled = true;
    bool m_unreadEnabled = true;
    // Guards the account-switch-complete auto-restore so scenario/explicit
    // navigation is not overridden by the remembered-room restore.
    bool m_suppressRoomRestore = false;
    // A scenario whose room/panel navigation is deferred until an in-flight
    // account switch completes.
    QString m_pendingScenario;
    QHash<QString, QString> m_selectedRoomPerAccount;
    // Launcher/CLI options, applied once on the first main-screen transition.
    QString m_launchScenario = QStringLiteral("home-overview");
    QString m_launchTheme;
    QString m_launchAppearance;
    QString m_launchSize;
    bool m_launchHideControls = false;
    bool m_launchApplied = false;
};
