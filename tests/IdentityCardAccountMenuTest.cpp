// v0.6.5 (SPEC 1h, modified): offscreen proof for the redesigned account
// switcher — vertical IdentityCard stack (no carousel/pagination dots), the
// fixed 320px popover width, the LIVE active-account guard (b8df062), the
// accountSwitching lockout, both destructive confirmations still reachable
// with Cancel focused, and that no meta text is fabricated (real
// connection state / real space count only, omitted when absent, never a
// per-account unread count). Drives a real AppController on the mock
// backend with two saved accounts, exactly like AccountSwitchTest.cpp.

#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QStyleHints>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QSet>

#include <cmath>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "storage/SecretStore.h"

namespace {

class FakeSecretStore final : public SecretStore
{
    Q_OBJECT
public:
    explicit FakeSecretStore(QObject *parent = nullptr) : SecretStore(parent) {}
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }
    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        m_values.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    QString readSecret(const QString &userId, const QString &key) const override
    { return m_values.value(userId + QLatin1Char('/') + key); }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        m_values.remove(userId + QLatin1Char('/') + key);
        return true;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { return {}; }

private:
    QHash<QString, QString> m_values;
};

double channelLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor &c)
{
    return 0.2126 * channelLinear(c.redF()) + 0.7152 * channelLinear(c.greenF())
        + 0.0722 * channelLinear(c.blueF());
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

int channelDelta(const QColor &a, const QColor &b)
{
    return qMax(qMax(qAbs(a.red() - b.red()), qAbs(a.green() - b.green())),
                qAbs(a.blue() - b.blue()));
}

// Source-over composite of a possibly TRANSLUCENT fill onto its ground. A
// row's selection chip is not always opaque — Storm's `hover` is
// `Qt.alpha(_stoHover, 0.22)` — so the pixels under the MXID are the chip
// over the popover canvas, never the chip's own colour property.
QColor over(const QColor &fg, const QColor &bg)
{
    const double a = fg.alphaF();
    return QColor::fromRgbF(a * fg.redF() + (1.0 - a) * bg.redF(),
                            a * fg.greenF() + (1.0 - a) * bg.greenF(),
                            a * fg.blueF() + (1.0 - a) * bg.blueF());
}

const QString kAlice = QStringLiteral("@alice:one.example");
const QString kBob = QStringLiteral("@bob:two.example");

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 500
    height: 700
    visible: true
    color: AppTheme.background

    // The palette is driven by writing AppTheme.mode on the SINGLETON.
    // Setting `settings.theme` instead reaches nothing here: `mode` is
    // written by a Binding in Main.qml, which no test scene loads, so a
    // loop over eleven themes would measure ONE palette eleven times and
    // still count to eleven (the 2026-09-19 lesson).
    property int themeMode: 11
    Binding { target: AppTheme; property: "mode"; value: win.themeMode }

    Rectangle { objectName: "tokStormCanvas"; visible: false; color: AppTheme.stormCanvas }
    Rectangle { objectName: "tokStormText"; visible: false; color: AppTheme.stormText }
    Rectangle { objectName: "tokStormTextMuted"; visible: false; color: AppTheme.stormTextMuted }
    Rectangle { objectName: "tokModalScrim"; visible: false; color: AppTheme.modalScrim }
    // The four grounds a row paints, and the two derived inks that have to
    // clear all four of them.
    Rectangle { objectName: "tokHover"; visible: false; color: AppTheme.hover }
    Rectangle { objectName: "tokSelected"; visible: false; color: AppTheme.selected }
    Rectangle { objectName: "tokSelectedHover"; visible: false; color: AppTheme.selectedHover }
    Rectangle { objectName: "tokStormTextSecondary"; visible: false; color: AppTheme.stormTextSecondary }
    Rectangle { objectName: "tokRowMutedInk"; visible: false; color: AppTheme.stormTextMuted }
    Rectangle { objectName: "tokRowSecondaryInk"; visible: false; color: AppTheme.stormTextSecondary }

    AccountMenu {
        id: menu
        objectName: "menu"
        x: 40
        // The popover is short enough to sit inside this window now, so the
        // pointer test's events land on real rows instead of being reported
        // outside the target window.
        y: 300
    }
    function openMenu() { menu.open() }
}
)QML";

} // namespace

// Test functions run in declaration order (QTest iterates the compiled
// slot table, which preserves source order) and deliberately share ONE
// AppController/QML scene across the whole class — booting a fresh window
// per test would be expensive, and the sequence below is a coherent
// scenario walkthrough (Alice active -> switch to Bob -> switch back),
// exactly like AccountSwitchTest.cpp's ordered multi-hop test.
class IdentityCardAccountMenuTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    QObject *find(const QString &name) const
    {
        return m_root->findChild<QObject *>(name);
    }

    // ListView-created delegates are model-owned, not QObject-parented, so
    // findChild cannot see them — resolve cards through itemAtIndex on the
    // (static, findable) list.
    QObject *findCard(const QString &userId) const
    {
        auto *list = m_root->findChild<QQuickItem *>(
            QStringLiteral("identityCardList"));
        if (!list)
            return nullptr;
        const int count = list->property("count").toInt();
        for (int i = 0; i < count; ++i) {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(list, "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem *, item),
                                      Q_ARG(int, i));
            if (item && item->objectName()
                            == QStringLiteral("identityCard_") + userId)
                return item;
        }
        return nullptr;
    }

    void openMenu()
    {
        QMetaObject::invokeMethod(m_root, "openMenu");
        auto *menu = find(QStringLiteral("menu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        // Delegate instantiation happens on the ListView's next layout
        // pass, not synchronously with `opened` — wait for the cards to
        // materialize before any lookup.
        QTRY_VERIFY(findCard(kAlice) != nullptr);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("identity-card-account-menu-test"));
        QSettings().clear();

        // Controls derive hoverEnabled from this hint, and the offscreen
        // platform may leave it off — a harness in which no button is ever
        // hovered cannot see a hover-revealed control hide under the pointer.
        QGuiApplication::styleHints()->setUseHoverEffects(true);
        m_controller = new AppController(AppController::MockBackend);
        auto *secrets = new FakeSecretStore(m_controller);
        m_controller->settings()->setSecretStore(secrets);
        m_controller->settings()->saveSession(
            QStringLiteral("https://one.example"), kAlice,
            QStringLiteral("ALICEDEV"), QStringLiteral("alice-token-fixture"));
        m_controller->settings()->saveSession(
            QStringLiteral("https://two.example"), kBob,
            QStringLiteral("BOBDEV"), QStringLiteral("bob-token-fixture"));
        m_controller->switchToAccount(kAlice);
        QTRY_VERIFY(!m_controller->accountSwitching());
        QCOMPARE(m_controller->accounts()->activeUserId(), kAlice);

        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                     m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("identitycardscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
        delete m_controller;
    }

    void popoverIsFixed320WideWithNoCarouselArtifacts()
    {
        openMenu();
        auto *menu = find(QStringLiteral("menu"));
        QCOMPARE(menu->property("width").toInt(), 320);

        QFile file(QStringLiteral(QML_DIR "/AccountMenu.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(!content.contains(QStringLiteral("PathView")));
        QVERIFY(!content.contains(QStringLiteral("SwipeView")));
        QVERIFY(!content.contains(QStringLiteral("PageIndicator")));
    }

    void activeAccountRendersFirstAsAVerticallyStackedCard()
    {
        openMenu();
        auto *aliceCard = findCard(kAlice);
        auto *bobCard = findCard(kBob);
        QVERIFY(aliceCard);
        QVERIFY(bobCard);
        QVERIFY(aliceCard->property("active").toBool());
        QVERIFY(!bobCard->property("active").toBool());

        auto *aliceItem = qobject_cast<QQuickItem *>(aliceCard);
        auto *bobItem = qobject_cast<QQuickItem *>(bobCard);
        QVERIFY(aliceItem && bobItem);
        // Vertical stack, same column: identical x, different y — never a
        // horizontal carousel.
        QCOMPARE(aliceItem->x(), bobItem->x());
        QVERIFY(aliceItem->y() != bobItem->y());
        // Active-first ordering.
        QVERIFY(aliceItem->y() < bobItem->y());

        // Decision: no real per-account unread source exists, so it is
        // never fabricated on the inactive card.
        QCOMPARE(bobCard->property("unreadCount").toInt(), 0);
    }

    // The meta line used to be stamped on the ACTIVE card and left empty on
    // every other one. It is now ONE strip below the list, which is the
    // same guarantee expressed structurally: there is no per-row carrier
    // for it at all, so an inactive account cannot acquire one.
    void statusStripSpeaksOnlyWhenSomethingIsWrong()
    {
        openMenu();
        auto *strip = find(QStringLiteral("accountStatusStrip"));
        QVERIFY(strip);
        QVERIFY(strip->property("visible").toBool());
        auto *meta = find(QStringLiteral("accountStatusMeta"));
        QVERIFY(meta);
        const QString text = meta->property("text").toString();

        // THE SPACE COUNT IS GONE. A switcher answers "which account am I,
        // switch me"; how many Spaces the account joined is not part of
        // either question, and the rail beside it already shows them.
        // Reported as a line whose purpose could not be guessed.
        QVERIFY2(!text.contains(QStringLiteral("space")),
                 qPrintable(QStringLiteral("the space count came back: \"%1\"")
                                .arg(text)));

        // AND A HEALTHY CONNECTION SAYS NOTHING. "Connected" on a working
        // client is the same noise as a warning that fires on a stock
        // theme: a line that always says "fine" teaches people not to read
        // it, and then it cannot say "not fine".
        const QString status = m_controller->connectionStatus();
        if (status == QStringLiteral("Connected")) {
            QVERIFY2(!text.contains(status),
                     qPrintable(QStringLiteral("the strip announced a healthy "
                                               "connection: \"%1\"")
                                    .arg(text)));
        } else {
            // …and an UNHEALTHY one still does. This is the half that was
            // doing real work all along: the reported screenshot read
            // "Idle", which is the word for disconnected-while-logged-in.
            QVERIFY2(text.contains(status),
                     qPrintable(QStringLiteral("the strip swallowed \"%1\"")
                                    .arg(status)));
        }
        QVERIFY(!text.contains(QStringLiteral("Online")));

        // Exactly one strip, however many accounts are listed: this is a
        // property of the attached session, not a column.
        QCOMPARE(m_root->findChildren<QObject *>(
                     QStringLiteral("accountStatusStrip")).size(), 1);

        // No row carries presence/crypto state any more, so none can
        // fabricate it. (The old defect was the other way round: the ACTIVE
        // row carried three of them and the probe that sized the list
        // carried one.)
        QFile cardFile(QStringLiteral(QML_DIR "/IdentityCard.qml"));
        QVERIFY(cardFile.open(QIODevice::ReadOnly));
        const QString card = QString::fromUtf8(cardFile.readAll());
        QVERIFY(!card.contains(QStringLiteral("property string metaText")));
        QVERIFY(!card.contains(QStringLiteral("property int trustCompleted")));
        QVERIFY(!card.contains(QStringLiteral("property bool e2eeReady")));
        QVERIFY(!card.contains(QStringLiteral("TrustMeter")));
    }

    void liveActiveUserIdGuardIgnoresReactivatingTheActiveCard()
    {
        openMenu();
        auto *aliceCard = findCard(kAlice);
        QVERIFY(aliceCard);
        QMetaObject::invokeMethod(aliceCard, "activated");
        QCoreApplication::processEvents();
        // Alice is already active: re-activating her own card is a silent
        // no-op — never a redundant switch/detach cycle.
        QCOMPARE(m_controller->accounts()->activeUserId(), kAlice);
        QVERIFY(!m_controller->accountSwitching());
    }

    void switchingIsSynchronousAndConfirmationsStayReachable()
    {
        openMenu();
        auto *bobCard = findCard(kBob);
        QVERIFY(bobCard);
        QMetaObject::invokeMethod(bobCard, "activated");
        // Switching state is set synchronously (existing lifecycle
        // contract) and the popover closes immediately.
        QVERIFY(m_controller->accountSwitching());
        QTRY_VERIFY(!m_controller->accountSwitching());
        QCOMPARE(m_controller->accounts()->activeUserId(), kBob);

        openMenu();
        auto *signOutButton = find(QStringLiteral("accountFooterSignOut"));
        QVERIFY(signOutButton);
        QMetaObject::invokeMethod(signOutButton, "clicked");
        auto *signOutDialog = find(QStringLiteral("signOutConfirmDialog"));
        QVERIFY(signOutDialog);
        QTRY_VERIFY(signOutDialog->property("opened").toBool());
        // Cancel is the focused, default-safe action.
        bool foundFocusedCancel = false;
        const auto buttons = signOutDialog->findChildren<QObject *>();
        for (QObject *candidate : buttons) {
            if (candidate->property("text").toString() == QStringLiteral("Cancel")
                && candidate->property("focus").toBool()) {
                foundFocusedCancel = true;
                break;
            }
        }
        QVERIFY(foundFocusedCancel);
        QMetaObject::invokeMethod(signOutDialog, "close");
        QVERIFY(m_controller->auth()->isLoggedIn());

        // Re-open the (freshly reopened) menu and drive per-card removal —
        // Bob is now active, so exercise removal through the Add-account
        // fixture instead: reuse Alice's (inactive) row.
        openMenu();
        auto *aliceCard = findCard(kAlice);
        QVERIFY(aliceCard);
        QVERIFY(!aliceCard->property("active").toBool());
        QMetaObject::invokeMethod(aliceCard, "removeRequested");
        auto *removeDialog = find(QStringLiteral("removeAccountConfirmDialog"));
        QVERIFY(removeDialog);
        QTRY_VERIFY(removeDialog->property("opened").toBool());
        QCOMPARE(removeDialog->property("targetUserId").toString(), kAlice);
        bool foundFocusedCancelRemove = false;
        for (QObject *candidate : removeDialog->findChildren<QObject *>()) {
            if (candidate->property("text").toString() == QStringLiteral("Cancel")
                && candidate->property("focus").toBool()) {
                foundFocusedCancelRemove = true;
                break;
            }
        }
        QVERIFY(foundFocusedCancelRemove);
        QMetaObject::invokeMethod(removeDialog, "close");
        // Cancelled: the account is untouched.
        QVERIFY(m_controller->settings()->hasSavedAccount(kAlice));
    }

    void accountSwitchingDisablesEveryRowAndFooterButton()
    {
        // Bob is active from the previous test; switch back to Alice and
        // check the mid-flight disabled state on the OTHER row/footer.
        openMenu();
        auto *aliceCard = findCard(kAlice);
        QVERIFY(aliceCard);
        QMetaObject::invokeMethod(aliceCard, "activated");
        QVERIFY(m_controller->accountSwitching());
        // Every row (including the one just activated, mid-flight) and the
        // whole footer must read disabled while a switch is in progress.
        auto *bobCard = findCard(kBob);
        QVERIFY(bobCard);
        QVERIFY(!bobCard->property("enabled").toBool());
        auto *addButton = find(QStringLiteral("accountFooterAdd"));
        QVERIFY(addButton);
        QVERIFY(!addButton->property("enabled").toBool());
        QTRY_VERIFY(!m_controller->accountSwitching());
    }

    // ── The remove X must survive the pointer reaching it ─────────────────
    //
    // Reported 2026-09-06: "the x doesnt work it just kinda starts to
    // flicker and never signs me out". The X was revealed by the card's
    // MouseArea `containsMouse`, and a hover-enabled ToolButton ABOVE that
    // MouseArea takes the hover the moment the pointer reaches it, so the
    // MouseArea reports a leave, the X hides, the pointer is back over the
    // MouseArea alone, the X shows, the button takes the hover again — the
    // flicker — and a click lands on a button that is hidden half the time.
    // Driven with a REAL pointer: hover the card, move onto the X, and the
    // X must still be there for the click that opens the confirm dialog.
    void removeButtonStaysUnderThePointerAndAClickReachesTheDialog()
    {
        m_controller->switchToAccount(kAlice);
        QTRY_VERIFY(!m_controller->accountSwitching());
        openMenu();
        auto *bobCard = qobject_cast<QQuickItem *>(findCard(kBob));
        QVERIFY(bobCard);
        QVERIFY(!bobCard->property("active").toBool());
        auto *x = bobCard->findChild<QQuickItem *>(
            QStringLiteral("identityCardRemoveButton"));
        QVERIFY(x);

        // At rest the X holds its layout slot and paints nothing; it is
        // not clickable and not a tab stop (see the no-reflow case below).
        QVERIFY2(qFuzzyIsNull(x->opacity()),
                 "the X is painted on a row nobody is pointing at");
        QVERIFY2(!x->isEnabled(),
                 "a transparent X that still takes clicks removes an "
                 "account nobody aimed at");

        // Hover the card, away from the X: the affordance reveals.
        const QPoint onCard = bobCard->mapToScene(
            QPointF(bobCard->width() * 0.3, bobCard->height() * 0.5)).toPoint();
        QTest::mouseMove(m_window, onCard);
        QTRY_VERIFY2(x->isVisible() && !qFuzzyIsNull(x->opacity()),
                     "hovering the card must reveal the X");
        QVERIFY(x->isEnabled());

        // Onto the X itself: it must stay, or nothing can ever click it.
        // A Layout places a newly visible item in its next polish, so the
        // X's position is not valid the instant it becomes visible; aim only
        // once it sits in the card's right half (its slot at the row's end).
        auto xCentre = [x]() {
            return x->mapToScene(QPointF(x->width() / 2, x->height() / 2)).toPoint();
        };
        const qreal cardMidX = bobCard->mapToScene(
            QPointF(bobCard->width() / 2, 0)).x();
        QTRY_VERIFY2(xCentre().x() > cardMidX,
                     qPrintable(QStringLiteral("X still at scene x %1, card mid %2")
                                    .arg(xCentre().x()).arg(cardMidX)));
        const QPoint onX = xCentre();
        QTest::mouseMove(m_window, onX);
        QTest::qWait(60);
        QVERIFY2(x->isVisible() && !qFuzzyIsNull(x->opacity()),
                 "the X hid the moment the pointer reached it");
        QVERIFY2(x->property("hovered").toBool(),
                 "the pointer is on the X but the X is not hovered");
        // "make the x hitbox bigger" — a 22 px target was the report.
        QVERIFY2(x->width() >= 28 && x->height() >= 28,
                 qPrintable(QStringLiteral("hit box %1x%2")
                                .arg(x->width()).arg(x->height())));

        auto *removeDialog = find(QStringLiteral("removeAccountConfirmDialog"));
        QVERIFY(removeDialog);
        QSignalSpy requested(bobCard, SIGNAL(removeRequested()));
        QSignalSpy dialogClosed(removeDialog, SIGNAL(closed()));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, onX);
        QTRY_VERIFY2(removeDialog->property("opened").toBool(),
                     "the click on the X did not open the confirm dialog");
        QCOMPARE(requested.count(), 1);
        QCOMPARE(removeDialog->property("targetUserId").toString(), kBob);
        // And it STAYS: the popover closing must not take the dialog with it.
        QTest::qWait(600);
        QVERIFY2(removeDialog->property("opened").toBool(),
                 "the confirm dialog did not stay open");
        QCOMPARE(dialogClosed.count(), 0);
        auto *menu = find(QStringLiteral("menu"));
        QVERIFY(menu);
        QTRY_VERIFY(!menu->property("opened").toBool());
        // The click reached the X and not the card beneath it, which would
        // have switched accounts instead (the first cut of this very test
        // aimed before the row had placed the X, and did exactly that).
        QCOMPARE(m_controller->accounts()->activeUserId(), kAlice);
        QVERIFY(!m_controller->accountSwitching());
        QMetaObject::invokeMethod(removeDialog, "close");
        QTRY_VERIFY(!removeDialog->property("opened").toBool());
        QVERIFY(m_controller->settings()->hasSavedAccount(kBob));
        QTest::mouseMove(m_window, QPoint(1, 1));
    }

    // ── THE REGRESSION ───────────────────────────────────────────────────
    //
    // Reported 2026-09-19: "you can even scroll about with one account since
    // it doesn't fit". The list used to be sized from an off-layout PROBE of
    // IdentityCard times the model count — sound reasoning (contentHeight is
    // 0 until delegates exist, and delegates only instantiate inside a
    // nonzero viewport), broken by resemblance: the probe declared neither
    // the trust meter nor the E2EE badge that the ACTIVE card renders, so it
    // under-measured that card by exactly 23 px. Measured at the 296 px
    // content width on the unfixed tree: short card 113, tall probe 136,
    // real active card 159.
    //
    // Rows are now one height this file can name, so the viewport and the
    // content are the same arithmetic. Asserted as UNIFORMITY plus FIT,
    // because either alone would have passed the old tree in some
    // configuration: the mock backend reports no crypto state, so the old
    // list fitted here while overflowing by 23 px on every real install.
    void everyRowIsOneRowHighAndTheViewportFitsItsContent()
    {
        m_controller->switchToAccount(kAlice);
        QTRY_VERIFY(!m_controller->accountSwitching());

        const QStringList extra{QStringLiteral("@carol:three.example"),
                                QStringLiteral("@dave:four.example"),
                                QStringLiteral("@erin:five.example"),
                                QStringLiteral("@frank:six.example"),
                                QStringLiteral("@grace:seven.example"),
                                QStringLiteral("@heidi:eight.example")};

        auto checkAt = [&](int expectedCount) {
            openMenu();
            auto *menu = find(QStringLiteral("menu"));
            QVERIFY(menu);
            auto *list = m_root->findChild<QQuickItem *>(
                QStringLiteral("identityCardList"));
            QVERIFY(list);
            QTRY_COMPARE(list->property("count").toInt(), expectedCount);
            // The rows are laid out on the next polish, not synchronously
            // with the model change.
            QTest::qWait(80);

            // 1. Every row is exactly as tall as the FIRST one — the
            //    ACTIVE row is first, and it is the one that used to be
            //    taller. Derived from the list rather than from the menu's
            //    own number on purpose: this assertion must be able to fail
            //    on a tree that has no such number, which is the tree that
            //    had the defect (measured there: active 136, the rest 113
            //    on the mock; 159 and 113 with a real crypto backend).
            QQuickItem *firstRow = nullptr;
            QMetaObject::invokeMethod(list, "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem *, firstRow),
                                      Q_ARG(int, 0));
            QVERIFY(firstRow);
            const qreal oneRow = firstRow->height();
            QVERIFY(oneRow > 0);
            for (int i = 1; i < expectedCount; ++i) {
                QQuickItem *item = nullptr;
                QMetaObject::invokeMethod(list, "itemAtIndex",
                                          Q_RETURN_ARG(QQuickItem *, item),
                                          Q_ARG(int, i));
                if (!item)
                    continue;   // beyond the viewport: not instantiated
                QVERIFY2(qFuzzyCompare(item->height(), oneRow),
                         qPrintable(QStringLiteral(
                             "row %1 of %2 is %3 px, the first row is %4 px "
                             "— rows are not uniform")
                                        .arg(i).arg(expectedCount)
                                        .arg(item->height()).arg(oneRow)));
            }

            // 2. The viewport is the content, up to the cap: nothing
            //    scrolls until there are genuinely more accounts than fit.
            const qreal viewport = list->height();
            const qreal content = list->property("contentHeight").toDouble();
            const int rowH = menu->property("rowH").toInt();
            const int maxRows = menu->property("maxVisibleRows").toInt();
            QVERIFY2(rowH > 0 && maxRows > 0,
                     "the menu must name one row height and one row cap");
            QCOMPARE(oneRow, qreal(rowH));
            const qreal expectedViewport =
                expectedCount <= maxRows
                    ? qreal(expectedCount * rowH)
                    : qreal(qRound((maxRows + 0.5) * rowH));
            QCOMPARE(viewport, expectedViewport);
            if (expectedCount <= maxRows) {
                QVERIFY2(content <= viewport,
                         qPrintable(QStringLiteral(
                             "%1 account(s): content %2 > viewport %3")
                                        .arg(expectedCount).arg(content)
                                        .arg(viewport)));
            } else {
                QVERIFY2(content > viewport,
                         "past the cap the list must scroll");
            }

            auto *m = find(QStringLiteral("menu"));
            QMetaObject::invokeMethod(m, "close");
            QTRY_VERIFY(!m->property("opened").toBool());
        };

        checkAt(2);
        for (int i = 0; i < extra.size(); ++i) {
            m_controller->settings()->saveSession(
                QStringLiteral("https://x%1.example").arg(i), extra.at(i),
                QStringLiteral("DEV"), QStringLiteral("tok"));
            checkAt(3 + i);
        }
        // And the reported case: one account, which must not scroll either.
        for (const QString &u : extra)
            m_controller->settings()->clearSessionForAccount(u);
        m_controller->settings()->clearSessionForAccount(kBob);
        checkAt(1);
        // Restore the fixture for the source-scan test that follows.
        m_controller->settings()->saveSession(
            QStringLiteral("https://two.example"), kBob,
            QStringLiteral("BOBDEV"), QStringLiteral("bob-token-fixture"));
    }

    // The other half of the same guarantee: no state a row can carry may
    // change its height. This is what the probe could not know and what a
    // future property must not be able to break.
    void noRowStateCanChangeARowsHeight()
    {
        QQmlComponent c(m_engine);
        c.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 400; height: 400
    property alias plain: plain
    property alias loaded: loaded
    property alias scaler: scaler
    Item {
        id: scaler
        property real uiScale: AppTheme.textScale
        onUiScaleChanged: AppTheme.textScale = uiScale
        readonly property int scaledRow: AppTheme.scaled(44)
    }
    IdentityCard {
        id: plain
        rowHeight: 44
        displayName: "Mizerd"
        userId: "@mizerd:matrix.org"
    }
    IdentityCard {
        id: loaded
        rowHeight: 44
        active: true
        displayName: "A rather long display name that will not fit"
        userId: "@mizerd:matrix.smetonis.net"
        unreadCount: 128
        needsSignIn: true
        healthWarning: true
    }
}
)QML"), QUrl(QStringLiteral("rowheight.qml")));
        QScopedPointer<QObject> scene(c.create());
        QVERIFY2(scene, qPrintable(c.errorString()));
        // Parented into the real window: a QQuickLayout's implicit size is
        // refreshed on a POLISH pass, and polish only runs for items in a
        // window. Measured off-window, the column's height reads back the
        // value it was first given whatever the font does — which is a
        // harness that answers the same number for every input.
        if (auto *sceneItem = qobject_cast<QQuickItem *>(scene.data()))
            sceneItem->setParentItem(m_window->contentItem());
        auto *plain = qvariant_cast<QQuickItem *>(scene->property("plain"));
        auto *loaded = qvariant_cast<QQuickItem *>(scene->property("loaded"));
        QVERIFY(plain && loaded);
        plain->setWidth(296);
        loaded->setWidth(296);
        QCOMPARE(plain->implicitHeight(), qreal(44));
        QCOMPARE(loaded->implicitHeight(), plain->implicitHeight());

        // And the ladder actually fits the row it is given, at every
        // interface scale. A row that clips its own id would trade away the
        // only thing telling two same-named accounts apart, and
        // `AppTheme.scaled()` drives BOTH the row height and the two font
        // sizes — so this asserts the proportion rather than assuming it.
        auto *scale = qvariant_cast<QQuickItem *>(scene->property("scaler"));
        QVERIFY(scale);
        const qreal original = scale->property("uiScale").toReal();
        for (const qreal factor : {0.85, 1.0, 1.25, 1.5, 1.75, 2.0}) {
            scale->setProperty("uiScale", factor);
            const int row = scale->property("scaledRow").toInt();
            loaded->setProperty("rowHeight", row);
            plain->setProperty("rowHeight", row);
            QTest::qWait(60);
            const qreal text = loaded->property("textColumnHeight").toReal();
            // Headroom measured 2026-09-19 at 7/9/10/13/16/19 px for
            // 0.85/1.0/1.25/1.5/1.75/2.0 — tightest at the SMALLEST scale,
            // because the two font sizes round up against a row that
            // rounds down.
            QVERIFY2(text <= row,
                     qPrintable(QStringLiteral(
                         "scale %1: the name+id column is %2 px in a %3 px "
                         "row — the id would be clipped")
                                    .arg(factor).arg(text).arg(row)));
            QCOMPARE(loaded->implicitHeight(), qreal(row));
            QCOMPARE(plain->implicitHeight(), qreal(row));
        }
        scale->setProperty("uiScale", original);
    }

    // Two accounts can share a display name on different homeservers — the
    // reported screenshot had two "Mizerd"s — so the id beneath the name is
    // the only disambiguator and must survive the popover's 320 px width.
    void theIdentityLineSurvivesThePopoverWidth()
    {
        QQmlComponent c(m_engine);
        c.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 400; height: 200
    property alias row: row
    IdentityCard {
        id: row
        rowHeight: 44
        active: true
        displayName: "Mizerd"
        userId: "@mizerd:matrix.smetonis.net"
    }
}
)QML"), QUrl(QStringLiteral("idwidth.qml")));
        QScopedPointer<QObject> scene(c.create());
        QVERIFY2(scene, qPrintable(c.errorString()));
        auto *row = qvariant_cast<QQuickItem *>(scene->property("row"));
        QVERIFY(row);
        // 320 popover - 2 x 12 padding.
        row->setWidth(296);
        QCoreApplication::processEvents();
        auto *label = qvariant_cast<QQuickItem *>(
            row->property("identityLabel"));
        QVERIFY(label);
        QTRY_VERIFY(label->width() > 0);
        QVERIFY2(!label->property("truncated").toBool(),
                 qPrintable(QStringLiteral(
                     "'@mizerd:matrix.smetonis.net' is elided at %1 px "
                     "(needs %2)").arg(label->width())
                        .arg(label->property("contentWidth").toReal())));
    }

    // ── F3: THE REVEAL MUST NOT MOVE WHAT IT IS REVEALED NEXT TO ─────────
    //
    // The remove (x) is a real RowLayout child. Revealing it with `visible`
    // took 30 px plus 8 px of spacing out of the text column the moment the
    // pointer arrived, and the id beneath the name re-elided UNDER THE
    // CURSOR — measured on an 8-account fixture,
    // `@dave:chat.very…ame.example.net` at rest became
    // `@dave:chat.v….example.net` on hover. Five characters of the one
    // string that tells two accounts sharing a display name apart, removed
    // at exactly the moment someone is reading it.
    //
    // Asserted on the LABEL'S GEOMETRY, not on the button's: a width
    // assertion on the X would pass on any implementation that keeps the
    // button the same size while still stealing the column.
    void revealingTheRemoveXDoesNotReElideTheIdUnderThePointer()
    {
        m_controller->switchToAccount(kAlice);
        QTRY_VERIFY(!m_controller->accountSwitching());
        openMenu();
        auto *bobCard = qobject_cast<QQuickItem *>(findCard(kBob));
        QVERIFY(bobCard);
        QVERIFY(!bobCard->property("active").toBool());
        auto *id = qvariant_cast<QQuickItem *>(
            bobCard->property("identityLabel"));
        QVERIFY(id);
        QTRY_VERIFY(id->width() > 0);

        QTest::mouseMove(m_window, QPoint(2, 2));
        QTest::qWait(80);
        const qreal restWidth = id->width();
        const QString restText = id->property("text").toString();
        const bool restTruncated = id->property("truncated").toBool();
        QVERIFY(restWidth > 0);

        const QPoint onCard = bobCard->mapToScene(
            QPointF(bobCard->width() * 0.3, bobCard->height() * 0.5)).toPoint();
        QTest::mouseMove(m_window, onCard);
        auto *x = bobCard->findChild<QQuickItem *>(
            QStringLiteral("identityCardRemoveButton"));
        QVERIFY(x);
        QTRY_VERIFY(!qFuzzyIsNull(x->opacity()));
        QTest::qWait(80);

        QVERIFY2(qFuzzyCompare(id->width(), restWidth),
                 qPrintable(QStringLiteral(
                     "the id column is %1 px at rest and %2 px under the "
                     "pointer — the reveal reflowed the row")
                        .arg(restWidth).arg(id->width())));
        QCOMPARE(id->property("text").toString(), restText);
        QCOMPARE(id->property("truncated").toBool(), restTruncated);

        QTest::mouseMove(m_window, QPoint(2, 2));
        QTest::qWait(60);
        QTRY_VERIFY(qFuzzyIsNull(x->opacity()));
        QCOMPARE(id->width(), restWidth);

        auto *menu = find(QStringLiteral("menu"));
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("opened").toBool());
    }

    // Holding the slot costs the text column 38 px on every inactive row,
    // so the id that the previous rounds fought for must still fit at the
    // popover's real content width. The ACTIVE row is covered by
    // theIdentityLineSurvivesThePopoverWidth; this is the inactive one,
    // which is the row that now carries the reserve.
    void theIdentityLineStillFitsOnAnInactiveRowWithTheSlotReserved()
    {
        QQmlComponent c(m_engine);
        c.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 400; height: 200
    property alias row: row
    IdentityCard {
        id: row
        rowHeight: 44
        active: false
        displayName: "Mizerd"
        userId: "@mizerd:matrix.smetonis.net"
    }
}
)QML"), QUrl(QStringLiteral("idwidthinactive.qml")));
        QScopedPointer<QObject> scene(c.create());
        QVERIFY2(scene, qPrintable(c.errorString()));
        auto *row = qvariant_cast<QQuickItem *>(scene->property("row"));
        QVERIFY(row);
        row->setWidth(296); // 320 popover - 2 x 12 padding.
        QCoreApplication::processEvents();
        auto *label = qvariant_cast<QQuickItem *>(
            row->property("identityLabel"));
        QVERIFY(label);
        QTRY_VERIFY(label->width() > 0);
        QVERIFY2(!label->property("truncated").toBool(),
                 qPrintable(QStringLiteral(
                     "'@mizerd:matrix.smetonis.net' is elided at %1 px on an "
                     "inactive row (needs %2)").arg(label->width())
                        .arg(label->property("contentWidth").toReal())));
    }

    // ── F4: A DESTRUCTIVE CONFIRMATION MUST DIM WHAT IT IS OVER ──────────
    //
    // Both dialogs are `modal: true`, and on the unfixed tree that drew
    // NOTHING: three background pixels sampled before and after "Sign out?"
    // opened were byte-identical. The Basic style's Overlay.modal tints
    // `palette.shadow`, a role nothing in this application sets, so the
    // scrim has to be named. Measured as PIXELS, because the property is
    // exactly the kind of thing that can be present and paint nothing.
    void bothDestructiveConfirmationsPaintAModalScrim()
    {
        auto *menu = find(QStringLiteral("menu"));
        QVERIFY(menu);
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("opened").toBool());
        QTest::mouseMove(m_window, QPoint(2, 2));
        QTest::qWait(120);

        const QImage before = m_window->grabWindow();
        QVERIFY(!before.isNull());
        // Corners, which neither dialog covers: both are centred, at most
        // 420 px wide and a few rows tall in a 500x700 window.
        const QPoint samples[] = {
            QPoint(3, 3),
            QPoint(before.width() - 4, 3),
            QPoint(3, before.height() - 4),
        };

        const char *dialogs[] = { "removeAccountConfirmDialog",
                                  "signOutConfirmDialog" };
        for (const char *name : dialogs) {
            auto *dialog = find(QLatin1String(name));
            QVERIFY2(dialog, name);
            QMetaObject::invokeMethod(dialog, "open");
            QTRY_VERIFY(dialog->property("opened").toBool());
            QTest::qWait(200);
            const QImage after = m_window->grabWindow();
            QVERIFY(!after.isNull());
            QCOMPARE(after.size(), before.size());
            for (const QPoint &p : samples) {
                const int delta = channelDelta(before.pixelColor(p),
                                               after.pixelColor(p));
                QVERIFY2(delta > 2,
                         qPrintable(QStringLiteral(
                             "%1: background at (%2,%3) is %4 before and %5 "
                             "after the dialog opened — nothing was dimmed")
                                .arg(QString::fromLatin1(name))
                                .arg(p.x()).arg(p.y())
                                .arg(before.pixelColor(p).name(),
                                     after.pixelColor(p).name())));
            }
            QMetaObject::invokeMethod(dialog, "close");
            QTRY_VERIFY(!dialog->property("opened").toBool());
            QTest::qWait(150);
        }
    }

    // ── F6: A ROW'S TWO LINES MUST READ AS A HIERARCHY ───────────────────
    //
    // The inactive name inked `stormTextSecondary`, which in the LIGHT
    // palettes is all but the same colour as the id's `stormTextMuted`
    // under it — Lightning Light #4c5661 against #525c68, 5.63:1 against
    // 5.12:1 on the popover canvas, a ratio of 1.10 where Storm reads 1.63.
    // The name and the id stopped being a hierarchy and read as one block
    // of grey.
    //
    // Eleven palettes are DEMANDED to be distinct rather than counted, for
    // the reason in the scene's own comment.
    void theInactiveRowsNameAndIdAreAHierarchyOnEveryPalette()
    {
        m_controller->switchToAccount(kAlice);
        QTRY_VERIFY(!m_controller->accountSwitching());
        openMenu();
        auto *bobCard = qobject_cast<QQuickItem *>(findCard(kBob));
        QVERIFY(bobCard);
        QVERIFY(!bobCard->property("active").toBool());
        auto *name = bobCard->findChild<QQuickItem *>(
            QStringLiteral("identityCardName"));
        auto *id = bobCard->findChild<QQuickItem *>(
            QStringLiteral("identityCardUserId"));
        QVERIFY(name);
        QVERIFY(id);

        const int restore = m_root->property("themeMode").toInt();
        QSet<QRgb> canvases;
        for (int mode = 1; mode <= 11; ++mode) {
            m_root->setProperty("themeMode", mode);
            auto *canvasProbe = m_root->findChild<QQuickItem *>(
                QStringLiteral("tokStormCanvas"));
            QVERIFY(canvasProbe);
            QTest::qWait(20);
            const QColor canvas =
                canvasProbe->property("color").value<QColor>();
            canvases.insert(canvas.rgb());
            const QColor nameInk = name->property("color").value<QColor>();
            const QColor idInk = id->property("color").value<QColor>();
            const double nameRatio = contrastRatio(nameInk, canvas);
            const double idRatio = contrastRatio(idInk, canvas);
            // The id is the only thing telling two same-named accounts
            // apart; it was 1.60:1 in the light palette once.
            QVERIFY2(idRatio >= 4.5,
                     qPrintable(QStringLiteral("theme %1: the MXID %2 is "
                                               "%3:1 on %4")
                                    .arg(mode).arg(idInk.name())
                                    .arg(idRatio, 0, 'f', 2)
                                    .arg(canvas.name())));
            QVERIFY2(nameRatio / idRatio >= 1.6,
                     qPrintable(QStringLiteral(
                         "theme %1: name %2 at %3:1 over id %4 at %5:1 is a "
                         "ratio of %6 — the two lines do not read as a "
                         "hierarchy")
                            .arg(mode).arg(nameInk.name())
                            .arg(nameRatio, 0, 'f', 2).arg(idInk.name())
                            .arg(idRatio, 0, 'f', 2)
                            .arg(nameRatio / idRatio, 0, 'f', 2)));
        }
        QCOMPARE(canvases.size(), 11);
        m_root->setProperty("themeMode", restore);
        QTest::qWait(20);
        auto *menu = find(QStringLiteral("menu"));
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("opened").toBool());
    }

    // ── A ROW IS FOUR SURFACES, AND ONLY ONE OF THEM HAD BEEN MEASURED ───
    //
    // The case above measures an inactive row AT REST on the popover canvas.
    // A row also paints `hover`, `selected` and `selectedHover` over that
    // canvas depending on what the pointer is doing, and two of those are
    // translucent in some palettes, so the pixels under the MXID are the chip
    // OVER the canvas and not the chip colour. Measured 2026-09-20 on real
    // hovered rows, the MXID was below 4.5:1 AA on:
    //   hovered INACTIVE row, six palettes  — Graphite 3.09, Indigo Night
    //     3.44, Deep Teal 3.45, Nordic 3.46, Midnight 3.58, Lightning Dark
    //     3.61;
    //   ACTIVE row, four palettes           — Indigo Night 4.12, Graphite
    //     4.16, Lightning Dark 4.33, Warm 4.40;
    //   hovered ACTIVE row, NINE palettes   — worst Graphite 3.37. Nobody had
    //     measured this state at all, and it is the worst of the four.
    // The display NAME fails too, on one: `stormText` is 4.31:1 on Nordic
    // hovered-active, whose `selectedHover` flattens to the mid slate
    // #587197.
    //
    // The resting inactive row clears on every palette (floor 4.59), which is
    // exactly why measuring only that state found nothing — and why the
    // derivation leaves it bit-identical, so the hierarchy case above is
    // unaffected.
    //
    // This case drives a REAL pointer onto a REAL row: `pointerWithin` has to
    // become true and `identityCardRowChip` has to paint, because a probe
    // that composites what it THINKS the row would paint shares none of the
    // row.
    void theMxidClearsEveryFillItsRowCanPaintOnEveryPalette()
    {
        m_controller->switchToAccount(kAlice);
        QTRY_VERIFY(!m_controller->accountSwitching());
        openMenu();
        auto *aliceCard = qobject_cast<QQuickItem *>(findCard(kAlice));
        auto *bobCard = qobject_cast<QQuickItem *>(findCard(kBob));
        QVERIFY(aliceCard);
        QVERIFY(bobCard);
        QVERIFY(aliceCard->property("active").toBool());
        QVERIFY(!bobCard->property("active").toBool());

        // The ground the row derives its inks against must be the ground the
        // popover actually paints, or every number below is about a surface
        // nobody draws.
        auto *popoverBg = m_root->findChild<QQuickItem *>(
            QStringLiteral("accountPopoverBackground"));
        QVERIFY(popoverBg);

        auto tok = [this](const char *n) {
            auto *it = m_root->findChild<QQuickItem *>(QLatin1String(n));
            return it ? it->property("color").value<QColor>() : QColor();
        };
        auto aimAt = [](QQuickItem *c) {
            return c->mapToScene(QPointF(c->width() * 0.3, c->height() * 0.5))
                .toPoint();
        };

        struct State { QQuickItem *card; const char *what; bool hover;
                       bool active; };
        const State states[] = {
            { bobCard, "inactive at rest", false, false },
            { bobCard, "inactive hovered", true, false },
            { aliceCard, "active at rest", false, true },
            { aliceCard, "active hovered", true, true },
        };

        const int restore = m_root->property("themeMode").toInt();
        QSet<QRgb> distinctFills;
        QSet<QRgb> distinctCanvases;
        int measured = 0;
        int rawTokenFailures = 0;
        for (int mode = 1; mode <= 11; ++mode) {
            m_root->setProperty("themeMode", mode);
            QTRY_COMPARE(popoverBg->property("color").value<QColor>(),
                         tok("tokStormCanvas"));
            const QColor canvas = popoverBg->property("color").value<QColor>();
            distinctCanvases.insert(canvas.rgb());
            for (const State &st : states) {
                QCOMPARE(st.card->property("hostSurface").value<QColor>(),
                         canvas);
                QTest::mouseMove(m_window, st.hover ? aimAt(st.card)
                                                    : QPoint(1, 1));
                QTRY_COMPARE(st.card->property("pointerWithin").toBool(),
                             st.hover);
                auto *chip = st.card->findChild<QQuickItem *>(
                    QStringLiteral("identityCardRowChip"));
                auto *id = st.card->findChild<QQuickItem *>(
                    QStringLiteral("identityCardUserId"));
                auto *name = st.card->findChild<QQuickItem *>(
                    QStringLiteral("identityCardName"));
                QVERIFY(chip && id && name);
                const QColor fill =
                    over(chip->property("color").value<QColor>(), canvas);
                QCOMPARE(QColor(st.card->property("rowFill").value<QColor>()
                                    .rgb()),
                         QColor(fill.rgb()));
                distinctFills.insert(fill.rgb());

                const QColor idInk = id->property("color").value<QColor>();
                const QColor nameInk = name->property("color").value<QColor>();
                const double idRatio = contrastRatio(idInk, fill);
                const double nameRatio = contrastRatio(nameInk, fill);
                QVERIFY2(idRatio >= 4.5,
                         qPrintable(QStringLiteral(
                             "theme %1, %2: the MXID %3 is %4:1 on the fill "
                             "%5 this row paints")
                                .arg(mode)
                                .arg(QString::fromLatin1(st.what))
                                .arg(idInk.name())
                                .arg(idRatio, 0, 'f', 2)
                                .arg(fill.name())));
                QVERIFY2(nameRatio >= 4.5,
                         qPrintable(QStringLiteral(
                             "theme %1, %2: the display name %3 is %4:1 on "
                             "the fill %5 this row paints")
                                .arg(mode)
                                .arg(QString::fromLatin1(st.what))
                                .arg(nameInk.name())
                                .arg(nameRatio, 0, 'f', 2)
                                .arg(fill.name())));
                ++measured;

                // NOT VACUOUS: the raw token this ink is derived FROM fails
                // on nineteen of these forty-four (palette, state) pairs, so
                // a case that passed because every palette was already fine
                // would count zero here and be caught.
                const QColor raw = st.active ? tok("tokStormTextSecondary")
                                             : tok("tokStormTextMuted");
                if (contrastRatio(raw, fill) < 4.5)
                    ++rawTokenFailures;
            }
        }
        // Four states times eleven palettes, all forty-four a different
        // fill: the palettes really moved AND the four states really are
        // four surfaces.
        QCOMPARE(distinctCanvases.size(), 11);
        QCOMPARE(distinctFills.size(), 44);
        QCOMPARE(measured, 44);
        QVERIFY2(rawTokenFailures >= 15,
                 qPrintable(QStringLiteral(
                     "only %1 of 44 (palette, state) pairs still fail with "
                     "the RAW token — this case can no longer prove the "
                     "derivation is doing anything")
                        .arg(rawTokenFailures)));

        QTest::mouseMove(m_window, QPoint(1, 1));
        m_root->setProperty("themeMode", restore);
        QTest::qWait(20);
        auto *menu = find(QStringLiteral("menu"));
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("opened").toBool());
    }

    // ── ESCAPE MUST DISMISS THE SWITCHER, AND IT NEVER HAS ──────────────
    //
    // The popover has always declared `closePolicy: Popup.CloseOnEscape |
    // Popup.CloseOnPressOutside`, and half of that declaration did nothing:
    // `QQuickPopup::keyPressEvent` gates its Escape branch on
    // `hasActiveFocus()`, and a Popup only takes active focus when `focus` is
    // true — which DEFAULTS TO FALSE. Measured on Windows against the
    // published 0.9.8: four Escape presses, zero differing pixels, with a
    // hover/park control proving the screen was live. Press-outside worked
    // the whole time because the overlay handles that with the mouse,
    // independently of focus, and that asymmetry is what disguised it.
    //
    // The two confirmations inherit the same rule and were broken the same
    // way — worse, because each carries `focus: true` on its Cancel BUTTON,
    // which sets focus WITHIN the popup focus scope and cannot become ACTIVE
    // focus while the scope itself has none. So the safe default action was
    // not the focused one either. The case above asserts the declared
    // `focus`; this one asserts `activeFocus`, which is the property that
    // decides whether a keystroke arrives.
    //
    // UNFIXED TREE: `activeFocus` is false on the popover and neither the
    // popover nor either dialog closes on Escape.
    void escapeDismissesTheSwitcherAndBothConfirmations()
    {
        openMenu();
        auto *menu = find(QStringLiteral("menu"));
        QVERIFY(menu);
        QVERIFY(menu->property("opened").toBool());
        // Read the flag out of QML rather than pinning its numeric value
        // here: an enum spelled by hand in a test is a second source of
        // truth that can be wrong on its own.
        {
            auto *menuItem = menu->property("contentItem")
                                 .value<QQuickItem *>();
            QVERIFY(menuItem);
            QQmlExpression asksForEscape(
                qmlContext(menu), menu,
                QStringLiteral("(closePolicy & Popup.CloseOnEscape) !== 0"));
            QVERIFY2(asksForEscape.evaluate().toBool(),
                     "the popover does not ask to close on Escape");
        }
        QTRY_VERIFY2(menu->property("activeFocus").toBool(),
                     "the account switcher never takes active focus, so no "
                     "key press can reach it and CloseOnEscape is inert");
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY2(!menu->property("opened").toBool(),
                     "Escape did not dismiss the account switcher");

        // SIGN OUT. Escape must abandon it, and the session must survive.
        openMenu();
        auto *signOutButton = find(QStringLiteral("accountFooterSignOut"));
        QVERIFY(signOutButton);
        QMetaObject::invokeMethod(signOutButton, "clicked");
        auto *signOutDialog = find(QStringLiteral("signOutConfirmDialog"));
        QVERIFY(signOutDialog);
        QTRY_VERIFY(signOutDialog->property("opened").toBool());
        QTRY_VERIFY2(signOutDialog->property("activeFocus").toBool(),
                     "the sign-out confirmation never takes active focus");
        // And now Cancel is really the focused action rather than merely
        // the one that asked to be.
        bool cancelHasActiveFocus = false;
        for (QObject *candidate : signOutDialog->findChildren<QObject *>()) {
            if (candidate->property("text").toString()
                    == QStringLiteral("Cancel")
                && candidate->property("activeFocus").toBool()) {
                cancelHasActiveFocus = true;
                break;
            }
        }
        QVERIFY2(cancelHasActiveFocus,
                 "Cancel declares focus but does not hold ACTIVE focus, so "
                 "the safe default action is not the focused one");
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY2(!signOutDialog->property("opened").toBool(),
                     "Escape did not abandon the sign-out confirmation");
        QVERIFY(m_controller->auth()->isLoggedIn());

        // REMOVE ACCOUNT. Same, and the account must survive.
        auto *menuAgain = find(QStringLiteral("menu"));
        if (!menuAgain->property("opened").toBool())
            openMenu();
        const QString inactive =
            m_controller->accounts()->activeUserId() == kAlice ? kBob : kAlice;
        auto *card = qobject_cast<QQuickItem *>(findCard(inactive));
        QVERIFY(card);
        QVERIFY(!card->property("active").toBool());
        QMetaObject::invokeMethod(card, "removeRequested");
        auto *removeDialog = find(QStringLiteral("removeAccountConfirmDialog"));
        QVERIFY(removeDialog);
        QTRY_VERIFY(removeDialog->property("opened").toBool());
        QCOMPARE(removeDialog->property("targetUserId").toString(), inactive);
        QTRY_VERIFY2(removeDialog->property("activeFocus").toBool(),
                     "the remove-account confirmation never takes active "
                     "focus");
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY2(!removeDialog->property("opened").toBool(),
                     "Escape did not abandon the remove-account "
                     "confirmation");
        QVERIFY2(m_controller->settings()->hasSavedAccount(inactive),
                 "abandoning the confirmation removed the account anyway");

        auto *leftOpen = find(QStringLiteral("menu"));
        if (leftOpen->property("opened").toBool()) {
            QMetaObject::invokeMethod(leftOpen, "close");
            QTRY_VERIFY(!leftOpen->property("opened").toBool());
        }
    }
    void noTokenOrPathEverBoundIntoTheUi()
    {
        // Source-level guarantee alongside the live checks above: neither
        // secret material nor a filesystem path is ever interpolated into a
        // label/Accessible string.
        QFile file(QStringLiteral(QML_DIR "/AccountMenu.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(!content.contains(QStringLiteral("accessToken")));
        QVERIFY(!content.contains(QStringLiteral("Token")));
        QVERIFY(!content.contains(QStringLiteral("crypto-store")));

        // R16: no `name:` property on IdentityCard's own API (Avatar's own
        // `name` binding inside the file is a different component's
        // property and is not what this checks).
        QFile cardFile(QStringLiteral(QML_DIR "/IdentityCard.qml"));
        QVERIFY(cardFile.open(QIODevice::ReadOnly));
        const QString cardContent = QString::fromUtf8(cardFile.readAll());
        QVERIFY(!cardContent.contains(QStringLiteral("property string name")));
        QVERIFY(!cardContent.contains(QStringLiteral("property var name")));
    }

    // ── THE SWITCHER MUST BE ON SCREEN ON THE FIRST OPEN ─────────────
    //
    // 2026-09-19: the account switcher opened with its top level with the
    // rail avatar tile's TOP and grew downward, so ~200 px of a 279 px
    // popover was off the bottom of the window and nothing below the header
    // could be reached. It was wrong on EVERY open until the window's height
    // changed once, and right for the rest of the session afterwards — which
    // is why it survived review and several GUI audits: anything that resizes
    // the window before looking sees a working switcher.
    //
    // The cause was `SpacesRail.qml`'s placement reading
    // `parent.mapFromItem(null, 0, 0).y` into a cached property to clamp the
    // popover against the window's top edge. mapFromItem() is not reactive,
    // and the snapshot was taken while the rail's own ColumnLayout had not
    // had its first pass — the account tile was still 12 px from the rail's
    // TOP rather than at its foot — so the clamp term came out 0 and beat the
    // real -239 for the rest of the session. The list's height was never
    // involved: measured in the running app, `implicitHeight` was already at
    // its final 279 when that one evaluation happened.
    //
    // WHY THIS CASE BUILDS ITS OWN SCENE. Every other case here pins the
    // popover at a fixed `x`/`y`, which is exactly the placement under test.
    // This one loads the REAL compiled `SpacesRail.qml` on the same real
    // AppController, in its own window, and NEVER RESIZES IT.
    //
    // WHY IT READS `y` BEFORE SHOWING. In the running app the placement
    // binding is evaluated during start-up, before the rail's layout has run
    // (measured: the cached value changes to -12 before the popover's own
    // Component.onCompleted). An offscreen scene that nobody reads settles
    // its layout first and would therefore take the snapshot at the RIGHT
    // moment and pass over the defect. The read below is what makes the
    // harness evaluate the placement as early as production does; it names
    // no private property, so it stays valid whatever the placement is
    // written in.
    void theSwitcherOpensFullyInsideTheWindowOnAFirstOpenWithNoResize()
    {
        const char *railScene = R"QML(
import QtQuick
import MatrixClient

Window {
    id: win
    width: 1000
    height: 700
    // An EXPLICIT rail height, not an anchor: the defect's one escape hatch
    // was that a change to the rail's own height re-ran the stale snapshot,
    // so a rail whose height is still settling would repair itself and the
    // case would pass on the unfixed tree.
    SpacesRail {
        objectName: "placementRail"
        x: 0
        y: 0
        width: 68
        height: 700
    }
}
)QML";
        QQmlComponent component(m_engine);
        component.setData(QByteArray(railScene),
                          QUrl(QStringLiteral("railplacementscene.qml")));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *win = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(win);

        auto *popup = root->findChild<QObject *>(
            QStringLiteral("accountSwitcherPopover"));
        QVERIFY(popup);
        // See above: evaluate the placement now, before the rail has been
        // laid out, exactly as the running app does.
        popup->property("y");

        win->show();
        QVERIFY(QTest::qWaitForWindowExposed(win));
        QCoreApplication::processEvents();

        auto *tile = root->findChild<QQuickItem *>(
            QStringLiteral("railAccountTile"));
        QVERIFY(tile);
        QTRY_VERIFY(tile->isVisible());
        // The tile must have reached the foot of the rail, or this case is
        // measuring a rail that never laid out rather than a placement.
        QTRY_VERIFY(tile->mapToScene(QPointF(0, 0)).y() > win->height() / 2);

        // FIRST OPEN. Nothing has resized this window and nothing will.
        QMetaObject::invokeMethod(popup, "open");
        QTRY_VERIFY(popup->property("opened").toBool());
        auto *list = root->findChild<QQuickItem *>(
            QStringLiteral("identityCardList"));
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), 2);
        QCoreApplication::processEvents();

        // The popup's own item in the overlay, not the Popup object: this is
        // the rectangle the user can actually press.
        auto *content = popup->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        QQuickItem *popupItem = content->parentItem();
        QVERIFY(popupItem);
        QTRY_VERIFY(popupItem->height() > tile->height());
        const QRectF scene(popupItem->mapToScene(QPointF(0, 0)),
                           popupItem->size());

        QVERIFY2(scene.height() > 0, "the popover has no height to place");
        QVERIFY2(scene.top() >= 0.0 && scene.bottom() <= win->height(),
                 qPrintable(QStringLiteral(
                     "the account switcher opened at scene y %1..%2 in a %3 px "
                     "window — %4 px of it is outside. The rail avatar tile is "
                     "at y %5, so a popover %6 px tall must sit at %7. A "
                     "placement that is only right after the window has been "
                     "resized once is the defect this case exists for; do not "
                     "make it pass by resizing before measuring.")
                     .arg(scene.top(), 0, 'f', 1)
                     .arg(scene.bottom(), 0, 'f', 1)
                     .arg(win->height())
                     .arg(qMax(0.0, scene.bottom() - win->height())
                              + qMax(0.0, -scene.top()), 0, 'f', 1)
                     .arg(tile->mapToScene(QPointF(0, 0)).y(), 0, 'f', 1)
                     .arg(scene.height(), 0, 'f', 1)
                     .arg(tile->mapToScene(QPointF(0, tile->height())).y()
                              - scene.height(), 0, 'f', 1)));

        QMetaObject::invokeMethod(popup, "close");
        QTRY_VERIFY(!popup->property("visible").toBool());
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    IdentityCardAccountMenuTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "IdentityCardAccountMenuTest.moc"
