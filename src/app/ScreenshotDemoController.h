#pragma once

// Development-only; compiled only with LIGHTNING_ENABLE_SCREENSHOT_DEMO.
// Drives the screenshot scenario catalogue and demo control panel through the
// same AppController/SettingsManager paths the normal UI uses.

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

    Q_PROPERTY(QVariantList scenarios READ scenarios CONSTANT)
    Q_PROPERTY(QVariantList accounts READ accounts CONSTANT)
    Q_PROPERTY(QVariantList themes READ themes CONSTANT)
    Q_PROPERTY(QVariantList appearances READ appearances CONSTANT)
    Q_PROPERTY(QVariantList windowSizes READ windowSizes CONSTANT)

    Q_PROPERTY(QString currentScenario READ currentScenario NOTIFY currentScenarioChanged)
    Q_PROPERTY(QString currentAccount READ currentAccount NOTIFY stateChanged)
    // The active account's non-space rooms ({id, name}).
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

    // Theme name to SettingsManager::Theme id, or -1 if unknown.
    static int themeIdForName(const QString &name);
    // Size preset or bounded "WxH" to width/height; false if unsupported.
    static bool sizeForPreset(const QString &preset, int *w, int *h);
    static bool isValidScenario(const QString &id);
    static QStringList scenarioIds();
    // The scenario's account, so the launcher can restore it directly at boot.
    static QString scenarioAccount(const QString &id);

    // Applied once on the first main-screen transition; empty strings are
    // ignored.
    void applyLaunchOptions(const QString &scenario, const QString &theme,
                            const QString &appearance, const QString &size,
                            bool hideControls);

public Q_SLOTS:
    // Performs all of the scenario's navigation and presentation state.
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
    // Applied imperatively in QML so the user can still resize afterwards.
    void windowSizeRequested(int width, int height);
    void accountSwitcherRequested();

    // Popup hooks fired once a scenario's navigation has settled. The owning
    // QML surface chooses which instance to open.
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
    /// Opens Room Information at a RoomInfoPanel section ("" = default).
    void demoOpenRoomInfo(const QString &section);
    /// Opens Find in History mode, which renders the local-index coverage row.
    void demoOpenFindBarHistory(const QString &query);

private:
    struct Scenario;
    static const QList<Scenario> &catalogue();
    const Scenario *findScenario(const QString &id) const;
    void applyScenarioNavigation(const Scenario &s);
    // Emits the popup signal one tick later unless a newer scenario took over.
    void dispatchScenarioPopup(const QString &scenarioId, const QString &popup,
                               const QString &query);
    // Idempotent seeds so picker scenarios are not empty.
    void seedDemoEmojiRecents();
    void seedDemoGifFavorite();
    void seedDemoGifCatalogue();
    void seedDemoSavedGif();
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
    // Stops the remembered-room restore overriding scenario navigation.
    bool m_suppressRoomRestore = false;
    // Scenario waiting for an in-flight account switch.
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
