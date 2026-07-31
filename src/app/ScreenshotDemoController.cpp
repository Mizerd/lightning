#include "app/ScreenshotDemoController.h"

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "gif/GifFavoritesModel.h"
#include "gif/GifSearchController.h"
#include "matrix/MockMatrixClient.h"
#include "spaces/SpaceManager.h"
#include "threads/ThreadController.h"

#include <QTimer>

// One deterministic screenshot scenario: which account/room/space, which real
// page or panel to open, and the recommended theme/appearance/window/typing/
// controls state. Activation performs ALL of this navigation.
struct ScreenshotDemoController::Scenario {
    QString id;
    QString account;      // full user id
    QString room;         // room id ("" = none)
    QString space;        // active space id ("" = all rooms)
    QString page;         // "" | "settings-appearance" | "settings-security"
                          // | "settings-sessions" | "account-switcher"
    bool openThread = false;
    int theme = -1;       // SettingsManager::Theme id (-1 = leave as is)
    bool typing = false;
    QString size;         // window preset
    bool controls = true; // demo controls visible on activation
    QString title;        // human-readable label for the panel

    // v0.6.5 (Wave 2): which demo-only popup/overlay to open once the above
    // navigation has settled, and an optional seed string for it (a quick-
    // switcher query/command, a mention prefix, a settings-search term).
    // Trailing fields with no explicit initializer default-construct to ""
    // via aggregate initialization, so every existing row above is unchanged.
    // See ScreenshotDemoController::dispatchScenarioPopup for the mapping
    // from `popup` to the matching demoOpen*/demoFocus* signal.
    QString popup;
    QString query;
};

namespace {
const QString kAlex   = QStringLiteral("@alex:lightning.example");
const QString kTaylor = QStringLiteral("@taylor:workplace.example");
const QString kNova   = QStringLiteral("@nova:community.example");

struct SizePreset { const char *id; const char *label; int w; int h; };
const SizePreset kSizes[] = {
    { "1024x768",  "1024 × 768",  1024, 768 },
    { "1280x800",  "1280 × 800",  1280, 800 },
    { "1440x900",  "1440 × 900",  1440, 900 },
    { "1600x1000", "1600 × 1000", 1600, 1000 },
    { "1920x1080", "1920 × 1080", 1920, 1080 },
    { "900x900",   "900 × 900",   900,  900 },
    { "narrow",    "Narrow (760 × 900)", 760, 900 },
    { "wide",      "Wide (1720 × 960)",  1720, 960 },
};

struct ThemeEntry { int id; const char *name; };
const ThemeEntry kThemes[] = {
    { 0,  "System" },        { 1,  "Lightning Light" }, { 2,  "Lightning Dark" },
    { 3,  "Graphite" },      { 4,  "Midnight" },        { 5,  "Nordic" },
    { 6,  "Purple Dusk" },   { 7,  "Warm" },            { 8,  "Moss Light" },
    { 9,  "Indigo Night" },  { 10, "Deep Teal" },
};
} // namespace

const QList<ScreenshotDemoController::Scenario> &
ScreenshotDemoController::catalogue()
{
    static const QList<Scenario> c = {
        { QStringLiteral("home-overview"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"), QString(),
          QString(), false, 9, true, QStringLiteral("1440x900"), true,
          QStringLiteral("Home overview") },
        { QStringLiteral("main-chat"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 4, true, QStringLiteral("1440x900"), true,
          QStringLiteral("Main chat — Design Lounge") },
        { QStringLiteral("direct-message"), kAlex,
          QStringLiteral("!dm-maya:lightning.example"),
          QStringLiteral("!space-friends:lightning.example"),
          QString(), false, 4, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Direct message — Maya Chen") },
        { QStringLiteral("development"), kAlex,
          QStringLiteral("!dev:lightning.example"),
          QStringLiteral("!space-community:lightning.example"),
          QString(), false, 9, false, QStringLiteral("1440x900"), true,
          QStringLiteral("Development — code & files") },
        { QStringLiteral("media-gallery"), kAlex,
          QStringLiteral("!photography:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 9, false, QStringLiteral("1440x900"), true,
          QStringLiteral("Media gallery — Photography") },
        { QStringLiteral("thread-view"), kAlex,
          QStringLiteral("!dev:lightning.example"),
          QStringLiteral("!space-community:lightning.example"),
          QString(), true, 6, false, QStringLiteral("1600x1000"), true,
          QStringLiteral("Thread view") },
        { QStringLiteral("poll"), kAlex,
          QStringLiteral("!feedback:lightning.example"),
          QStringLiteral("!space-community:lightning.example"),
          QString(), false, 9, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Poll — Product Feedback") },
        { QStringLiteral("settings-themes"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"), QString(),
          QStringLiteral("settings-appearance"), false, 9, false,
          QStringLiteral("1280x800"), true,
          QStringLiteral("Settings — Appearance") },
        { QStringLiteral("account-switching"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"), QString(),
          QStringLiteral("account-switcher"), false, 9, false,
          QStringLiteral("1280x800"), true,
          QStringLiteral("Account switcher") },
        { QStringLiteral("security"), kAlex,
          QStringLiteral("!dm-maya:lightning.example"), QString(),
          QStringLiteral("settings-security"), false, 9, false,
          QStringLiteral("1280x800"), true,
          QStringLiteral("Security & privacy") },
        { QStringLiteral("invite"), kAlex,
          QStringLiteral("!invite-founders:lightning.example"), QString(),
          QString(), false, 9, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Invite") },
        { QStringLiteral("work-overview"), kTaylor,
          QStringLiteral("!aurora:workplace.example"), QString(),
          QString(), false, 8, true, QStringLiteral("1440x900"), true,
          QStringLiteral("Work overview — Project Aurora") },
        { QStringLiteral("community-overview"), kNova,
          QStringLiteral("!general:community.example"), QString(),
          QString(), false, 9, true, QStringLiteral("1440x900"), true,
          QStringLiteral("Community overview — General") },
        { QStringLiteral("responsive-chat"), kAlex,
          QStringLiteral("!dm-maya:lightning.example"),
          QStringLiteral("!space-friends:lightning.example"),
          QString(), false, 9, false, QStringLiteral("narrow"), true,
          QStringLiteral("Responsive chat (narrow)") },

        // ── v0.6.5 (Wave 2): menu/popup/dialog surface scenarios ─────────
        // All reuse existing fictional rooms/members (no new mock data).
        // Theme per row matches the design-handoff mock for that surface, so
        // these captures line up in a side-by-side comparison pass.
        { QStringLiteral("menu-message"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 9, false, QStringLiteral("1440x900"), true,
          QStringLiteral("Message context menu"),
          QStringLiteral("message-menu"), QString() },
        { QStringLiteral("menu-room"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 9, false, QStringLiteral("1440x900"), true,
          QStringLiteral("Room context menu"),
          QStringLiteral("room-menu"), QString() },
        { QStringLiteral("find-in-room"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 9, false, QStringLiteral("1440x900"), true,
          // 0.6.5 (C7): the floating composer-family find card, pre-filled
          // so the match counter and prev/next controls are live in the
          // capture ("layout" matches the seeded Design Lounge messages).
          QStringLiteral("Find in loaded messages (in-room search card)"),
          QStringLiteral("find-in-room"), QStringLiteral("layout") },
        { QStringLiteral("quick-switcher"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 9, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Quick switcher"),
          // "de" matches Design Lounge/Development/… (Rooms) and Design
          // Lounge members whose name contains "de" if any (People) — a
          // sectioned-results capture rather than the unfiltered full list.
          QStringLiteral("quick-switcher"), QStringLiteral("de") },
        { QStringLiteral("quick-switcher-command"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 10, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Quick switcher — command mode"),
          QStringLiteral("quick-switcher"), QStringLiteral(">theme") },
        { QStringLiteral("emoji-picker"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 9, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Emoji picker"),
          QStringLiteral("emoji-picker"), QString() },
        { QStringLiteral("gif-picker"), kAlex,
          QStringLiteral("!weekend:lightning.example"),
          QStringLiteral("!space-friends:lightning.example"),
          QString(), false, 8, false, QStringLiteral("1280x800"), true,
          QStringLiteral("GIF picker"),
          QStringLiteral("gif-picker"), QString() },
        { QStringLiteral("member-profile"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 9, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Member profile popover"),
          QStringLiteral("member-profile"), QString() },
        { QStringLiteral("mention-popup"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 10, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Mention popup"),
          QStringLiteral("mention-popup"), QStringLiteral("ma") },
        { QStringLiteral("trust-card"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"), QString(),
          QStringLiteral("settings-sessions"), false, 9, false,
          QStringLiteral("1280x800"), true,
          QStringLiteral("Trust card (Settings — Sessions; brand-fixed card)"),
          QStringLiteral("trust-card"), QString() },
        { QStringLiteral("new-conversation"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 3, false, QStringLiteral("1280x800"), true,
          QStringLiteral("New conversation"),
          QStringLiteral("new-conversation"), QString() },
        { QStringLiteral("settings-search"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"), QString(),
          QStringLiteral("settings-appearance"), false, 9, false,
          QStringLiteral("1280x800"), true,
          QStringLiteral("Settings search"),
          QStringLiteral("settings-search"), QStringLiteral("security") },
        { QStringLiteral("invite-people"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 9, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Invite people"),
          QStringLiteral("invite-people"), QString() },
        { QStringLiteral("create-poll"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, 8, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Create poll"),
          QStringLiteral("create-poll"), QString() },
    };
    return c;
}

ScreenshotDemoController::ScreenshotDemoController(AppController *app,
                                                   MockMatrixClient *mock,
                                                   QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_mock(mock)
{
    sizeForPreset(QStringLiteral("1440x900"),
                  &m_requestedWidth, &m_requestedHeight);
    m_sizeLabel = QStringLiteral("1440 × 900");

    if (m_app) {
        connect(m_app, &AppController::currentRoomIdChanged, this,
                &ScreenshotDemoController::rememberSelectedRoom);
        // When an account switch settles, either finish a deferred scenario
        // activation or restore that account's remembered room.
        connect(m_app, &AppController::accountSwitchingChanged, this, [this] {
            if (m_app->accountSwitching())
                return;   // only act on the completed (false) edge
            if (!m_pendingScenario.isEmpty()) {
                const QString id = m_pendingScenario;
                m_pendingScenario.clear();
                // Defer the nav to after startSync repopulates the models (the
                // switch sets accountSwitching=false BEFORE startSync). Skip it
                // if a newer switch changed the account out from under us.
                QTimer::singleShot(0, this, [this, id] {
                    const Scenario *sc = findScenario(id);
                    if (sc && !m_app->accountSwitching()
                        && sc->account == currentAccount())
                        applyScenarioNavigation(*sc);
                });
            } else if (!m_suppressRoomRestore) {
                const QString acct = currentAccount();
                QTimer::singleShot(0, this, [this, acct] {
                    if (!m_app->accountSwitching() && acct == currentAccount())
                        restoreSelectedRoom();
                });
            }
        });
        // Apply the launch scenario/overrides once, the first time the app
        // reaches the main screen after the demo boot restore. Applied
        // synchronously (startSync has already populated the models by the
        // MainScreen transition) so the boot scene is fully settled before any
        // caller drives a scenario — otherwise a queued launch could fire late
        // and fight an already-started navigation.
        connect(m_app, &AppController::currentScreenChanged, this, [this] {
            if (m_launchApplied
                || m_app->currentScreen() != AppController::MainScreen)
                return;
            m_launchApplied = true;
            applyLaunchNow();
        });
    }
}

// ── Catalogue exposure ──────────────────────────────────────────────────

QVariantList ScreenshotDemoController::scenarios() const
{
    QVariantList out;
    for (const Scenario &s : catalogue()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), s.id);
        m.insert(QStringLiteral("title"), s.title);
        m.insert(QStringLiteral("account"), s.account);
        m.insert(QStringLiteral("room"), s.room);
        m.insert(QStringLiteral("theme"), s.theme);
        m.insert(QStringLiteral("size"), s.size);
        out.append(m);
    }
    return out;
}

QVariantList ScreenshotDemoController::accounts() const
{
    QVariantList out;
    if (!m_mock)
        return out;
    for (const QString &uid : m_mock->demoAccountUserIds()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), uid);
        m.insert(QStringLiteral("name"), accountDisplayName(uid));
        out.append(m);
    }
    return out;
}

QVariantList ScreenshotDemoController::themes() const
{
    QVariantList out;
    for (const ThemeEntry &t : kThemes) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), t.id);
        m.insert(QStringLiteral("name"), QString::fromLatin1(t.name));
        out.append(m);
    }
    return out;
}

QVariantList ScreenshotDemoController::appearances() const
{
    QVariantList out;
    const char *modes[][2] = { { "system", "Match system" },
                               { "light", "Light" }, { "dark", "Dark" } };
    for (const auto &m : modes) {
        QVariantMap e;
        e.insert(QStringLiteral("id"), QString::fromLatin1(m[0]));
        e.insert(QStringLiteral("name"), QString::fromLatin1(m[1]));
        out.append(e);
    }
    return out;
}

QVariantList ScreenshotDemoController::windowSizes() const
{
    QVariantList out;
    for (const SizePreset &s : kSizes) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), QString::fromLatin1(s.id));
        m.insert(QStringLiteral("label"), QString::fromLatin1(s.label));
        m.insert(QStringLiteral("w"), s.w);
        m.insert(QStringLiteral("h"), s.h);
        out.append(m);
    }
    return out;
}

// ── State accessors ─────────────────────────────────────────────────────

QString ScreenshotDemoController::currentAccount() const
{
    return m_app && m_app->accounts() ? m_app->accounts()->activeUserId()
                                      : QString();
}

QString ScreenshotDemoController::currentAccountName() const
{
    return accountDisplayName(currentAccount());
}

QString ScreenshotDemoController::currentRoom() const
{
    return m_app ? m_app->currentRoomId() : QString();
}

QVariantList ScreenshotDemoController::currentRooms() const
{
    QVariantList out;
    if (!m_mock)
        return out;
    for (const RoomInfo &r : m_mock->rooms()) {
        if (r.isSpace)
            continue;   // the Room selector lists rooms, not Spaces
        QVariantMap m;
        m.insert(QStringLiteral("id"), r.id);
        m.insert(QStringLiteral("name"),
                 r.name.isEmpty() ? r.id : r.name);
        out.append(m);
    }
    return out;
}

int ScreenshotDemoController::currentTheme() const
{
    return m_app && m_app->settings() ? int(m_app->settings()->theme()) : 0;
}

QString ScreenshotDemoController::accountDisplayName(const QString &userId) const
{
    if (!m_app || !m_app->accounts() || userId.isEmpty())
        return userId;
    const QVariantMap rec = m_app->accounts()->account(userId);
    const QString name = rec.value(QStringLiteral("displayName")).toString();
    return name.isEmpty() ? userId : name;
}

// ── Static helpers ──────────────────────────────────────────────────────

int ScreenshotDemoController::themeIdForName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n.isEmpty()) return -1;
    if (n == QLatin1String("system")) return 0;
    if (n == QLatin1String("light") || n == QLatin1String("lightning light")) return 1;
    if (n == QLatin1String("dark") || n == QLatin1String("lightning dark")) return 2;
    if (n == QLatin1String("graphite")) return 3;
    if (n == QLatin1String("midnight") || n == QLatin1String("ocean")) return 4;
    if (n == QLatin1String("nordic") || n == QLatin1String("nord")) return 5;
    if (n == QLatin1String("purple dusk") || n == QLatin1String("purple")
        || n == QLatin1String("violet")) return 6;
    if (n == QLatin1String("warm")) return 7;
    if (n == QLatin1String("moss") || n == QLatin1String("moss light")) return 8;
    if (n == QLatin1String("indigo") || n == QLatin1String("indigo night")) return 9;
    if (n == QLatin1String("deep teal") || n == QLatin1String("teal")
        || n == QLatin1String("deepteal")) return 10;
    if (n == QLatin1String("storm")) return 11;
    return -1;
}

bool ScreenshotDemoController::sizeForPreset(const QString &preset, int *w, int *h)
{
    const QString p = preset.trimmed().toLower();
    for (const SizePreset &s : kSizes) {
        if (p == QLatin1String(s.id)) {
            if (w) *w = s.w;
            if (h) *h = s.h;
            return true;
        }
    }
    // Accept an explicit WxH within safe bounds (never absurd/unsafe).
    const int x = p.indexOf(QLatin1Char('x'));
    if (x > 0) {
        bool okW = false, okH = false;
        const int pw = p.left(x).toInt(&okW);
        const int ph = p.mid(x + 1).toInt(&okH);
        if (okW && okH && pw >= 480 && pw <= 3840 && ph >= 480 && ph <= 2160) {
            if (w) *w = pw;
            if (h) *h = ph;
            return true;
        }
    }
    return false;
}

bool ScreenshotDemoController::isValidScenario(const QString &id)
{
    for (const Scenario &s : catalogue())
        if (s.id == id)
            return true;
    return false;
}

QStringList ScreenshotDemoController::scenarioIds()
{
    QStringList ids;
    for (const Scenario &s : catalogue())
        ids << s.id;
    return ids;
}

QString ScreenshotDemoController::scenarioAccount(const QString &id)
{
    for (const Scenario &s : catalogue())
        if (s.id == id)
            return s.account;
    return {};
}

const ScreenshotDemoController::Scenario *
ScreenshotDemoController::findScenario(const QString &id) const
{
    for (const Scenario &s : catalogue())
        if (s.id == id)
            return &s;
    return nullptr;
}

// ── Activation ──────────────────────────────────────────────────────────

void ScreenshotDemoController::activateScenario(const QString &id)
{
    const Scenario *s = findScenario(id);
    if (!s || !m_app)
        return;
    m_currentScenario = id;

    // Account-independent presentation is safe to apply now.
    setWindowSize(s->size);
    setControlsVisible(s->controls);
    // Theme is PER-ACCOUNT, so it (and typing) are applied in
    // applyScenarioNavigation — after any account switch completes — so they
    // land on the target account, not the one being switched away from.

    if (s->account != currentAccount()) {
        // Defer the room/panel navigation until the async switch completes.
        m_pendingScenario = id;
        m_suppressRoomRestore = true;
        m_app->switchToAccount(s->account);
    } else {
        applyScenarioNavigation(*s);
    }
    Q_EMIT currentScenarioChanged();
    Q_EMIT stateChanged();
}

void ScreenshotDemoController::applyScenarioNavigation(const Scenario &s)
{
    if (!m_app)
        return;
    // Per-account presentation, now that the target account is active.
    if (s.theme >= 0)
        m_app->settings()->setTheme(SettingsManager::Theme(s.theme));
    if (m_mock)
        m_mock->setDemoTypingSuppressed(!s.typing);
    m_typingEnabled = s.typing;
    Q_EMIT toggleStateChanged();

    // Space filter (empty = all rooms).
    if (m_app->spaces())
        m_app->spaces()->setActiveSpaceId(s.space);

    // Reset any open thread panel, then select the room.
    if (m_app->thread())
        m_app->thread()->close();
    if (!s.room.isEmpty())
        m_app->openRoom(s.room);
    else
        m_app->setCurrentRoomId(QString());

    // Page / panel.
    if (s.page == QLatin1String("settings-appearance")) {
        m_app->showSettingsSection(QStringLiteral("appearance"));
        m_app->showSettings();
    } else if (s.page == QLatin1String("settings-security")) {
        m_app->showSettingsSection(QStringLiteral("security"));
        m_app->showSettings();
    } else if (s.page == QLatin1String("settings-sessions")) {
        m_app->showSettingsSection(QStringLiteral("sessions"));
        m_app->showSettings();
    } else if (s.page == QLatin1String("account-switcher")) {
        m_app->showMain();
        Q_EMIT accountSwitcherRequested();
    } else {
        m_app->showMain();
        if (s.openThread && !s.room.isEmpty() && m_mock && m_app->thread()) {
            const QString root = m_mock->demoThreadRoot(s.room);
            if (!root.isEmpty())
                m_app->thread()->openThread(s.room, root);
        }
    }
    m_suppressRoomRestore = false;
    if (!s.room.isEmpty())
        m_selectedRoomPerAccount[s.account] = s.room;

    // Seed local, demo-only state synchronously (no layout dependency) before
    // the popup itself opens, so a picker that needs recents/favorites to
    // look real has them the instant it's visible.
    if (s.popup == QLatin1String("emoji-picker"))
        seedDemoEmojiRecents();
    else if (s.popup == QLatin1String("gif-picker"))
        seedDemoGifFavorite();
    dispatchScenarioPopup(s.id, s.popup, s.query);

    Q_EMIT stateChanged();
}

// ── Demo-only popup dispatch ────────────────────────────────────────────

void ScreenshotDemoController::dispatchScenarioPopup(const QString &scenarioId,
                                                     const QString &popup,
                                                     const QString &query)
{
    if (popup.isEmpty())
        return;
    // Deferred one event-loop tick so a popup anchored to a delegate item
    // (a message row, a room-list row) opens only after that item has
    // actually been instantiated by its ListView, and guarded against a
    // newer scenario having taken over before the tick fires (the demo panel
    // lets a user pick a different scenario faster than one tick).
    QTimer::singleShot(0, this, [this, scenarioId, popup, query] {
        if (m_currentScenario != scenarioId)
            return;
        if (popup == QLatin1String("message-menu"))
            Q_EMIT demoOpenMessageContextMenu();
        else if (popup == QLatin1String("room-menu"))
            Q_EMIT demoOpenRoomContextMenu();
        else if (popup == QLatin1String("quick-switcher"))
            Q_EMIT demoOpenQuickSwitcher(query);
        else if (popup == QLatin1String("emoji-picker"))
            Q_EMIT demoOpenEmojiPicker();
        else if (popup == QLatin1String("gif-picker"))
            Q_EMIT demoOpenGifPicker();
        else if (popup == QLatin1String("member-profile"))
            Q_EMIT demoOpenMemberProfile();
        else if (popup == QLatin1String("mention-popup"))
            Q_EMIT demoOpenMentionPopup(query);
        else if (popup == QLatin1String("trust-card"))
            Q_EMIT demoOpenTrustCard();
        else if (popup == QLatin1String("find-in-room"))
            Q_EMIT demoOpenFindBar(query);
        else if (popup == QLatin1String("new-conversation"))
            Q_EMIT demoOpenNewConversation();
        else if (popup == QLatin1String("settings-search"))
            Q_EMIT demoFocusSettingsSearch(query);
        else if (popup == QLatin1String("invite-people"))
            Q_EMIT demoOpenInvitePeople();
        else if (popup == QLatin1String("create-poll"))
            Q_EMIT demoOpenCreatePoll();
    });
}

void ScreenshotDemoController::seedDemoEmojiRecents()
{
    if (!m_app || !m_app->emojiCatalog())
        return;
    // Fixed, deterministic call order — recordRecentEmoji dedups+prepends,
    // so the final list is identical on every activation regardless of
    // whatever was recorded before (idempotent for repeat/reset scenarios).
    //
    // Routed through EmojiCatalog::recordUse(), NOT SettingsManager::
    // recordRecentEmoji() directly: recordUse() is the only path that also
    // rebuild()s the live "Recently Used" bucket and emits
    // recentEmojiChanged(), which is what the picker's GridView and the
    // message-menu quick-react strip are actually bound to. Calling
    // SettingsManager's setter directly persists the same data but leaves
    // every already-constructed QML view showing a stale (empty) list —
    // this was caught precisely that way in an early screenshot-demo
    // capture (the picker's Recently Used section stayed empty despite the
    // underlying settings value being seeded correctly).
    static const QStringList kRecents = {
        QStringLiteral("\U0001F389"),   // 🎉 party popper
        QStringLiteral("\U0001F525"),   // 🔥 fire
        QStringLiteral("\U0001F602"),   // 😂 joy
        QStringLiteral("\U0001F44D"),   // 👍 thumbs up
        QStringLiteral("❤️"), // ❤️ red heart
    };
    for (const QString &emoji : kRecents)
        m_app->emojiCatalog()->recordUse(emoji);
}

void ScreenshotDemoController::seedDemoGifFavorite()
{
    if (!m_app || !m_app->gif() || !m_app->gif()->favorites())
        return;
    GifFavoritesModel *favorites = m_app->gif()->favorites();
    const QString provider = QStringLiteral("giphy");
    const QString gifId = QStringLiteral("demo-favorite-loop");
    // toggle() flips state, so guard with isFavorite() — otherwise a second
    // activation (panel re-click, resetScenario) would delete the seed it
    // just added instead of leaving it in place.
    if (favorites->isFavorite(provider, gifId))
        return;
    QVariantMap result;
    result.insert(QStringLiteral("provider"), provider);
    result.insert(QStringLiteral("gifId"), gifId);
    result.insert(QStringLiteral("title"), QStringLiteral("Retro dance loop"));
    result.insert(QStringLiteral("rating"), QStringLiteral("g"));
    // Fictional, non-resolving *.example CDN domain — no network in demo
    // mode ever fetches these; the offline broken-thumbnail state is an
    // accepted, deliberate limitation (see docs/screenshot-demo.md).
    result.insert(QStringLiteral("previewUrl"),
                  QStringLiteral("https://media.giphy.example/demo-favorite-loop/preview.gif"));
    result.insert(QStringLiteral("stillUrl"),
                  QStringLiteral("https://media.giphy.example/demo-favorite-loop/still.jpg"));
    result.insert(QStringLiteral("gifUrl"),
                  QStringLiteral("https://media.giphy.example/demo-favorite-loop/original.gif"));
    result.insert(QStringLiteral("gifWidth"), 480);
    result.insert(QStringLiteral("gifHeight"), 480);
    result.insert(QStringLiteral("previewWidth"), 240);
    result.insert(QStringLiteral("previewHeight"), 240);
    favorites->toggle(result);
}

// ── Panel actions ───────────────────────────────────────────────────────

void ScreenshotDemoController::setAccount(const QString &userId)
{
    if (!m_app || userId.isEmpty() || userId == currentAccount())
        return;
    // A manual account change through the panel restores that account's room.
    m_app->switchToAccount(userId);
    Q_EMIT stateChanged();
}

void ScreenshotDemoController::setRoom(const QString &roomId)
{
    if (m_app && !roomId.isEmpty())
        m_app->openRoom(roomId);
}

void ScreenshotDemoController::setSpace(const QString &spaceId)
{
    if (m_app && m_app->spaces())
        m_app->spaces()->setActiveSpaceId(spaceId);
}

void ScreenshotDemoController::setTheme(int themeId)
{
    if (m_app && themeId >= 0 && themeId <= SettingsManager::kMaxThemeId) {
        m_app->settings()->setTheme(SettingsManager::Theme(themeId));
        Q_EMIT stateChanged();
    }
}

void ScreenshotDemoController::setThemeByName(const QString &name)
{
    const int id = themeIdForName(name);
    if (id >= 0)
        setTheme(id);
}

void ScreenshotDemoController::setAppearance(const QString &mode)
{
    const QString m = mode.trimmed().toLower();
    if (m == QLatin1String("system"))
        setTheme(0);
    else if (m == QLatin1String("light"))
        setTheme(8);   // Moss Light
    else if (m == QLatin1String("dark"))
        setTheme(9);   // Indigo Night
}

void ScreenshotDemoController::setWindowSize(const QString &preset)
{
    int w = 0, h = 0;
    if (!sizeForPreset(preset, &w, &h))
        return;
    m_requestedWidth = w;
    m_requestedHeight = h;
    // A friendly label (prefer the preset's own label).
    m_sizeLabel = QStringLiteral("%1 × %2").arg(w).arg(h);
    for (const SizePreset &s : kSizes)
        if (preset.trimmed().toLower() == QLatin1String(s.id))
            m_sizeLabel = QString::fromLatin1(s.label);
    Q_EMIT windowSizeRequested(w, h);
    Q_EMIT windowSizeChanged();
    Q_EMIT stateChanged();
}

void ScreenshotDemoController::setControlsVisible(bool visible)
{
    if (m_controlsVisible == visible)
        return;
    m_controlsVisible = visible;
    Q_EMIT controlsVisibleChanged();
}

void ScreenshotDemoController::toggleControls()
{
    setControlsVisible(!m_controlsVisible);
}

void ScreenshotDemoController::setTypingEnabled(bool on)
{
    if (m_typingEnabled == on)
        return;
    m_typingEnabled = on;
    if (m_mock)
        m_mock->setDemoTypingSuppressed(!on);
    Q_EMIT toggleStateChanged();
}

void ScreenshotDemoController::setUnreadBadgesEnabled(bool on)
{
    if (m_unreadEnabled == on)
        return;
    m_unreadEnabled = on;
    if (m_mock)
        m_mock->setDemoUnreadHidden(!on);
    Q_EMIT toggleStateChanged();
}

void ScreenshotDemoController::resetScenario()
{
    if (m_mock)
        m_mock->resetDemoAccount(currentAccount());
    if (!m_currentScenario.isEmpty())
        activateScenario(m_currentScenario);
    Q_EMIT stateChanged();
}

void ScreenshotDemoController::resetAllDemoState()
{
    m_selectedRoomPerAccount.clear();
    if (m_mock) {
        m_mock->setDemoTypingSuppressed(false);
        m_mock->setDemoUnreadHidden(false);
        m_mock->resetDemoData();
    }
    m_typingEnabled = true;
    m_unreadEnabled = true;
    Q_EMIT toggleStateChanged();
    activateScenario(QStringLiteral("home-overview"));
}

void ScreenshotDemoController::openSettings()
{
    if (m_app) m_app->showSettings();
}

void ScreenshotDemoController::openSettingsAppearance()
{
    if (!m_app) return;
    m_app->showSettingsSection(QStringLiteral("appearance"));
    m_app->showSettings();
}

void ScreenshotDemoController::openSecurity()
{
    if (!m_app) return;
    m_app->showSettingsSection(QStringLiteral("security"));
    m_app->showSettings();
}

void ScreenshotDemoController::openAccountSwitcher()
{
    if (m_app) m_app->showMain();
    Q_EMIT accountSwitcherRequested();
}

void ScreenshotDemoController::openThreadPanel()
{
    if (!m_app || !m_mock || !m_app->thread())
        return;
    const QString room = currentRoom();
    if (room.isEmpty())
        return;
    const QString root = m_mock->demoThreadRoot(room);
    if (!root.isEmpty())
        m_app->thread()->openThread(room, root);
}

// ── Selected-room memory ────────────────────────────────────────────────

void ScreenshotDemoController::rememberSelectedRoom()
{
    if (!m_app || m_app->accountSwitching() || m_suppressRoomRestore)
        return;
    const QString room = m_app->currentRoomId();
    const QString acct = currentAccount();
    if (!room.isEmpty() && !acct.isEmpty())
        m_selectedRoomPerAccount[acct] = room;
    Q_EMIT stateChanged();
}

void ScreenshotDemoController::restoreSelectedRoom()
{
    if (!m_app)
        return;
    const QString acct = currentAccount();
    if (acct.isEmpty())
        return;
    QString room = m_selectedRoomPerAccount.value(acct);
    if (room.isEmpty() && m_mock)
        room = m_mock->demoDefaultRoom(acct);
    if (!room.isEmpty())
        m_app->openRoom(room);
}

// ── Launch options (CLI) ────────────────────────────────────────────────

void ScreenshotDemoController::applyLaunchOptions(const QString &scenario,
                                                  const QString &theme,
                                                  const QString &appearance,
                                                  const QString &size,
                                                  bool hideControls)
{
    if (!scenario.isEmpty() && isValidScenario(scenario))
        m_launchScenario = scenario;
    m_launchTheme = theme;
    m_launchAppearance = appearance;
    m_launchSize = size;
    m_launchHideControls = hideControls;
    // If the app is already on the main screen, apply immediately.
    if (m_app && m_app->currentScreen() == AppController::MainScreen
        && !m_launchApplied) {
        m_launchApplied = true;
        applyLaunchNow();
    }
}

void ScreenshotDemoController::applyLaunchNow()
{
    activateScenario(m_launchScenario);
    // CLI overrides win over the scenario defaults.
    if (!m_launchTheme.isEmpty())
        setThemeByName(m_launchTheme);
    if (!m_launchAppearance.isEmpty())
        setAppearance(m_launchAppearance);
    if (!m_launchSize.isEmpty())
        setWindowSize(m_launchSize);
    if (m_launchHideControls)
        setControlsVisible(false);
}
