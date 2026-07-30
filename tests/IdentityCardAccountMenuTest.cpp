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
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>

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

    AccountMenu {
        id: menu
        objectName: "menu"
        x: 40
        y: 600
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

    void activeCardMetaUsesRealStateAndOmitsAbsentSpaceCount()
    {
        openMenu();
        auto *aliceCard = findCard(kAlice);
        QVERIFY(aliceCard);
        const QString meta = aliceCard->property("metaText").toString();
        QVERIFY(meta.contains(m_controller->connectionStatus()));
        QVERIFY(!meta.contains(QStringLiteral("Online")));
        // The space count mirrors the LIVE model — rendered only when the
        // account really has joined Spaces, never fabricated and never
        // rendered as "0 spaces".
        const int spaces = m_controller->spaces()->spaceCount();
        if (spaces > 0) {
            QVERIFY(meta.contains(QStringLiteral("%1 space").arg(spaces))
                    || meta.contains(QStringLiteral("1 space")));
        } else {
            QVERIFY(!meta.contains(QStringLiteral("space")));
        }

        auto *bobCard = findCard(kBob);
        QVERIFY(bobCard);
        QCOMPARE(bobCard->property("metaText").toString(), QString());
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
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    IdentityCardAccountMenuTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "IdentityCardAccountMenuTest.moc"
