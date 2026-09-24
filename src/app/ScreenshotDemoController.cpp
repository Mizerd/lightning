#include "app/ScreenshotDemoController.h"

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "gif/GifFavoritesModel.h"
#include "gif/GifSearchController.h"
#include "gif/GifStarredStore.h"
#include "matrix/MockMatrixClient.h"
#include "spaces/SpaceManager.h"
#include "threads/ThreadController.h"

#include <QFile>
#include <QTimer>

#include <iterator>

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

    // Demo-only popup to open once navigation has settled, plus an optional
    // seed string for it. See dispatchScenarioPopup() for the mapping.
    QString popup;
    QString query;

    // Navigation layout: -1 keeps the account's choice, 0 Classic, 1 Channels.
    int navLayout = -1;
    // Process-local staged call: "" none, "call", or "call-share" (screen
    // share spotlighted). Nothing is published; see
    // SfuCallController::startDemoCall.
    QString call;
    // Channels view to select: "" leaves it, "@people" is Direct Messages, a
    // `!` id is that Space, "@home" is Home. Ignored in Classic.
    QString channelsScope;
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
// Storm leads: it is the brand theme and the demo's default.
const ThemeEntry kThemes[] = {
    { 11, "Storm" },
    { 0,  "System" },        { 1,  "Lightning Light" }, { 2,  "Lightning Dark" },
    { 3,  "Graphite" },      { 4,  "Midnight" },        { 5,  "Nordic" },
    { 6,  "Purple Dusk" },   { 7,  "Warm" },            { 8,  "Moss Light" },
    { 9,  "Indigo Night" },  { 10, "Deep Teal" },
};

constexpr int kStormTheme = 11;
} // namespace

const QList<ScreenshotDemoController::Scenario> &
ScreenshotDemoController::catalogue()
{
    static const QList<Scenario> c = {
        // Designated initializers for new rows: the struct is long enough
        // that a positional row silently shifts values when a field is added.
        //
        // Call scenarios stage a process-local call (no membership, SFU or
        // devices) so the call surfaces can be captured offline.
        { .id = QStringLiteral("call-grid"), .account = kAlex,
          .room = QStringLiteral("!design-lounge:lightning.example"),
          .theme = kStormTheme, .size = QStringLiteral("1440x900"),
          .title = QStringLiteral("Call — participant grid"),
          .call = QStringLiteral("call") },
        { .id = QStringLiteral("call-screen-share"), .account = kAlex,
          .room = QStringLiteral("!design-lounge:lightning.example"),
          .theme = kStormTheme, .size = QStringLiteral("1600x1000"),
          .title = QStringLiteral("Call — screen share"),
          .call = QStringLiteral("call-share") },

        { .id = QStringLiteral("channels-home"), .account = kAlex,
          .room = QStringLiteral("!design-lounge:lightning.example"),
          .theme = kStormTheme, .typing = true,
          .size = QStringLiteral("1440x900"),
          .title = QStringLiteral("Channels — Home"),
          .navLayout = 1, .channelsScope = QStringLiteral("@home") },
        { .id = QStringLiteral("channels-space"), .account = kAlex,
          .room = QStringLiteral("!design-lounge:lightning.example"),
          .space = QStringLiteral("!space-studio:lightning.example"),
          .theme = kStormTheme, .typing = true,
          .size = QStringLiteral("1440x900"),
          .title = QStringLiteral("Channels — a Space"),
          .navLayout = 1,
          .channelsScope = QStringLiteral("!space-studio:lightning.example") },
        { .id = QStringLiteral("channels-people"), .account = kAlex,
          .room = QStringLiteral("!dm-maya:lightning.example"),
          .theme = kStormTheme, .size = QStringLiteral("1280x800"),
          .title = QStringLiteral("Channels — Direct Messages"),
          .navLayout = 1, .channelsScope = QStringLiteral("@people") },
        // The find bar is seeded with a query: the result area (source
        // toggle, coverage line, index button) only renders during a search.
        { .id = QStringLiteral("find-in-room"), .account = kAlex,
          .room = QStringLiteral("!design-lounge:lightning.example"),
          .theme = kStormTheme, .size = QStringLiteral("1600x1000"),
          .title = QStringLiteral("Find in room — local search"),
          .popup = QStringLiteral("find-in-room"),
          .query = QStringLiteral("theme") },
        { .id = QStringLiteral("find-in-room-history"), .account = kAlex,
          .room = QStringLiteral("!design-lounge:lightning.example"),
          .theme = kStormTheme, .size = QStringLiteral("1600x1000"),
          .title = QStringLiteral("Find in room — searching history"),
          .popup = QStringLiteral("find-in-room-history"),
          .query = QStringLiteral("theme") },
        { .id = QStringLiteral("room-widgets"), .account = kAlex,
          .room = QStringLiteral("!design-lounge:lightning.example"),
          .theme = kStormTheme, .size = QStringLiteral("1600x1000"),
          .title = QStringLiteral("Room Information — widgets"),
          // The widget list is on the panel's default "overview" section.
          .popup = QStringLiteral("room-info") },

        // Classic is set explicitly so the shot does not depend on the
        // profile's last layout.
        { .id = QStringLiteral("classic-home"), .account = kAlex,
          .room = QStringLiteral("!design-lounge:lightning.example"),
          .theme = kStormTheme, .typing = true,
          .size = QStringLiteral("1440x900"),
          .title = QStringLiteral("Classic — conversation list"),
          .navLayout = 0 },

        { QStringLiteral("home-overview"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"), QString(),
          QString(), false, kStormTheme, true, QStringLiteral("1440x900"), true,
          QStringLiteral("Home overview") },
        { QStringLiteral("main-chat"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, kStormTheme, true, QStringLiteral("1440x900"), true,
          QStringLiteral("Main chat — Design Lounge") },
        { QStringLiteral("direct-message"), kAlex,
          QStringLiteral("!dm-maya:lightning.example"),
          QStringLiteral("!space-friends:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Direct message — Maya Chen") },
        { QStringLiteral("development"), kAlex,
          QStringLiteral("!dev:lightning.example"),
          QStringLiteral("!space-community:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1440x900"), true,
          QStringLiteral("Development — code & files") },
        { QStringLiteral("media-gallery"), kAlex,
          QStringLiteral("!photography:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1440x900"), true,
          QStringLiteral("Media gallery — Photography") },
        { QStringLiteral("thread-view"), kAlex,
          QStringLiteral("!dev:lightning.example"),
          QStringLiteral("!space-community:lightning.example"),
          QString(), true, kStormTheme, false, QStringLiteral("1600x1000"), true,
          QStringLiteral("Thread view") },
        { QStringLiteral("poll"), kAlex,
          QStringLiteral("!feedback:lightning.example"),
          QStringLiteral("!space-community:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
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
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Invite") },
        { QStringLiteral("work-overview"), kTaylor,
          QStringLiteral("!aurora:workplace.example"), QString(),
          QString(), false, kStormTheme, true, QStringLiteral("1440x900"), true,
          QStringLiteral("Work overview — Project Aurora") },
        { QStringLiteral("community-overview"), kNova,
          QStringLiteral("!general:community.example"), QString(),
          QString(), false, kStormTheme, true, QStringLiteral("1440x900"), true,
          QStringLiteral("Community overview — General") },
        { QStringLiteral("responsive-chat"), kAlex,
          QStringLiteral("!dm-maya:lightning.example"),
          QStringLiteral("!space-friends:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("narrow"), true,
          QStringLiteral("Responsive chat (narrow)") },

        // ── Menu, popup and dialog scenarios ────────────────────────────
        { QStringLiteral("menu-message"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1440x900"), true,
          QStringLiteral("Message context menu"),
          QStringLiteral("message-menu"), QString() },
        { QStringLiteral("menu-room"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1440x900"), true,
          QStringLiteral("Room context menu"),
          QStringLiteral("room-menu"), QString() },
        { QStringLiteral("find-in-room"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1440x900"), true,
          // Pre-filled so the match counter and prev/next controls are live.
          QStringLiteral("Find in loaded messages (in-room search card)"),
          QStringLiteral("find-in-room"), QStringLiteral("layout") },
        { QStringLiteral("quick-switcher"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Quick switcher"),
          // "de" yields sectioned results rather than the full list.
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
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Emoji picker"),
          QStringLiteral("emoji-picker"), QString() },
        { QStringLiteral("gif-picker"), kAlex,
          QStringLiteral("!weekend:lightning.example"),
          QStringLiteral("!space-friends:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
          QStringLiteral("GIF picker"),
          QStringLiteral("gif-picker"), QString() },
        { QStringLiteral("member-profile"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Member profile popover"),
          QStringLiteral("member-profile"), QString() },
        { QStringLiteral("mention-popup"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
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
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
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
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
          QStringLiteral("Invite people"),
          QStringLiteral("invite-people"), QString() },
        { QStringLiteral("create-poll"), kAlex,
          QStringLiteral("!design-lounge:lightning.example"),
          QStringLiteral("!space-studio:lightning.example"),
          QString(), false, kStormTheme, false, QStringLiteral("1280x800"), true,
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

    // One widget that opens and one that is refused, in the Rust backend's
    // payload shape (see MockMatrixClient::mockWidgets).
    if (m_mock) {
        m_mock->mockWidgets = {
            QVariantMap{
                { QStringLiteral("id"), QStringLiteral("standup") },
                { QStringLiteral("creator"), QStringLiteral("@maya:lightning.demo") },
                { QStringLiteral("kind"), QStringLiteral("jitsi") },
                { QStringLiteral("name"), QStringLiteral("Design standup") },
                { QStringLiteral("url"),
                  QStringLiteral("https://meet.example.org/design-standup"
                                 "?user=%40alex%3Alightning.demo&theme=storm") },
                { QStringLiteral("refusal"), QString() },
                { QStringLiteral("discloses"),
                  QStringList{ QStringLiteral("user_id"),
                               QStringLiteral("theme"),
                               QStringLiteral("connection") } },
            },
            QVariantMap{
                { QStringLiteral("id"), QStringLiteral("notes") },
                { QStringLiteral("creator"), QStringLiteral("@sam:lightning.demo") },
                { QStringLiteral("kind"), QStringLiteral("custom") },
                { QStringLiteral("name"), QStringLiteral("Launch notes") },
                { QStringLiteral("url"), QString() },
                { QStringLiteral("refusal"), QStringLiteral("not_https") },
                { QStringLiteral("discloses"), QStringList{} },
            },
        };
    }

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
                // accountSwitching drops before startSync repopulates the
                // models, so defer; skip if a newer switch has taken over.
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
        // Apply the launch scenario once, synchronously, on the first
        // MainScreen transition; a queued apply could fight a navigation
        // that has already started.
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

    setWindowSize(s->size);
    setControlsVisible(s->controls);
    // Theme and typing are per-account, so applyScenarioNavigation() sets
    // them after any account switch completes.

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
    if (s.theme >= 0)
        m_app->settings()->setTheme(SettingsManager::Theme(s.theme));
    // Layout before the room: Channels resolves its view from the scope.
    if (s.navLayout >= 0)
        m_app->settings()->setRoomNavigationLayout(s.navLayout);
    // Passed verbatim: the model classifies the scope string itself
    // (docs/navigation-layouts.md).
    if (!s.channelsScope.isEmpty() && m_app->spaceChannels())
        m_app->spaceChannels()->setScopeSpaceId(s.channelsScope);
    if (m_mock)
        m_mock->setDemoTypingSuppressed(!s.typing);
    m_typingEnabled = s.typing;
    Q_EMIT toggleStateChanged();

    if (m_app->spaces())
        m_app->spaces()->setActiveSpaceId(s.space);

    if (m_app->thread())
        m_app->thread()->close();
    if (!s.room.isEmpty())
        m_app->openRoom(s.room);
    else
        m_app->setCurrentRoomId(QString());

    // After the room is open: the call panel is keyed on the current room.
    // A scenario without a call ends any staged one, which would otherwise
    // persist into every later screenshot.
    if (m_app->groupCall()) {
        if (s.call.isEmpty()) {
            m_app->groupCall()->endDemoCall();
        } else {
            m_app->groupCall()->startDemoCall(
                s.room, s.call == QLatin1String("call-share"));
        }
    }

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

    // Seed picker state before the popup opens.
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
    // Deferred one tick so the delegate a popup anchors to exists; dropped
    // if a newer scenario took over in the meantime.
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
        else if (popup == QLatin1String("room-info"))
            Q_EMIT demoOpenRoomInfo(query);
        else if (popup == QLatin1String("find-in-room-history"))
            Q_EMIT demoOpenFindBarHistory(query);
    });
}

void ScreenshotDemoController::seedDemoEmojiRecents()
{
    if (!m_app || !m_app->emojiCatalog())
        return;
    // Recording dedups and prepends, so a fixed order is idempotent. Use
    // EmojiCatalog::recordUse(), not the SettingsManager setter: only it
    // rebuilds the live "Recently Used" bucket the views are bound to.
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
    seedDemoGifCatalogue();
    seedDemoSavedGif();
}

// Seeds the browse grid from bundled demo fixtures, since the mock has no
// provider. Nothing is persisted and no validation is relaxed: the stored
// collections still accept https only.
void ScreenshotDemoController::seedDemoGifCatalogue()
{
    if (!m_app || !m_app->gif())
        return;

    struct Fixture { const char *file; const char *title; int w; int h;
                     qint64 bytes; const char *provider; };
    // Mixed across both providers for variety in tags and badges.
    static const Fixture kFixtures[] = {
        { "loop.gif",          "Retro dance loop",     480, 480, 66172, "giphy" },
        { "gif-coast.gif",     "Coast at golden hour", 220, 138, 98929, "klipy" },
        { "gif-artwork.gif",   "Studio artwork",       220, 220, 139749, "giphy" },
        { "gif-city.gif",      "City timelapse",       220, 124, 86896, "giphy" },
        { "gif-portrait.gif",  "Portrait study",       147, 220, 130827, "klipy" },
        { "gif-square.gif",    "Album spin",           220, 220, 118734, "giphy" },
        { "gif-palette.gif",   "Palette cycle",        220, 220, 37519, "klipy" },
        { "gif-interface.gif", "Interface capture",    220, 138, 63245, "giphy" },
    };


    QList<gif::GifResult> rows;
    rows.reserve(int(std::size(kFixtures)));
    int n = 0;
    for (const Fixture &f : kFixtures) {
        const QString url =
            QStringLiteral("qrc:/qt/qml/MatrixClient/resources/screenshot-demo/")
            + QLatin1String(f.file);
        gif::GifResult r;
        r.provider = QLatin1String(f.provider);
        r.id = QStringLiteral("demo-%1").arg(++n);
        r.title = QLatin1String(f.title);
        r.rating = QStringLiteral("g");
        r.previewUrl = url;
        r.stillUrl = url;
        r.gifUrl = url;
        r.gifWidth = f.w;
        r.gifHeight = f.h;
        r.previewWidth = f.w;
        r.previewHeight = f.h;
        r.gifBytes = f.bytes;
        rows.append(r);
    }
    // A second pass fills the grid past one screenful.
    const int firstPass = rows.size();
    for (int i = 0; i < firstPass; ++i) {
        gif::GifResult r = rows.at(i);
        r.id = QStringLiteral("demo-%1").arg(++n);
        rows.append(r);
    }
    m_app->gif()->seedDemoCatalogue(rows);
}

// Local saved GIFs resolve by content hash from the account-scoped store, so
// real bytes go through starBytes(). The store lives under the demo profile.
void ScreenshotDemoController::seedDemoSavedGif()
{
    if (!m_app || !m_app->gif() || !m_app->gif()->starredStore())
        return;
    GifStarredStore *store = m_app->gif()->starredStore();
    if (!store->isOpen() || store->count() > 0)
        return;
    QFile f(QStringLiteral(
        ":/qt/qml/MatrixClient/resources/screenshot-demo/loop.gif"));
    if (!f.open(QIODevice::ReadOnly))
        return;
    store->starBytes(QStringLiteral("demo-saved-loop"), f.readAll());
}

// ── Panel actions ───────────────────────────────────────────────────────

void ScreenshotDemoController::setAccount(const QString &userId)
{
    if (!m_app || userId.isEmpty() || userId == currentAccount())
        return;
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
        // Indigo Night: what System resolves to on a dark desktop.
        setTheme(9);
}

void ScreenshotDemoController::setWindowSize(const QString &preset)
{
    int w = 0, h = 0;
    if (!sizeForPreset(preset, &w, &h))
        return;
    m_requestedWidth = w;
    m_requestedHeight = h;
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
    if (m_app && m_app->currentScreen() == AppController::MainScreen
        && !m_launchApplied) {
        m_launchApplied = true;
        applyLaunchNow();
    }
}

void ScreenshotDemoController::applyLaunchNow()
{
    activateScenario(m_launchScenario);
    // CLI overrides win over scenario defaults.
    if (!m_launchTheme.isEmpty())
        setThemeByName(m_launchTheme);
    if (!m_launchAppearance.isEmpty())
        setAppearance(m_launchAppearance);
    if (!m_launchSize.isEmpty())
        setWindowSize(m_launchSize);
    if (m_launchHideControls)
        setControlsVisible(false);
}
