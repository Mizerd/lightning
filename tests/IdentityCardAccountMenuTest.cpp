// Account switcher (AccountMenu + IdentityCard): a vertical card stack, the
// fixed 352px popover width, the live active-account guard, the
// accountSwitching lockout, both destructive confirmations reachable with
// Cancel focused, and no fabricated meta text (no per-account unread count).
// Drives a real AppController on the mock backend with two saved accounts,
// like AccountSwitchTest.cpp.

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
#include "crypto/CryptoHealthModel.h"
#include "matrix/MatrixClient.h"
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

// Source-over composite of a possibly translucent fill onto its ground: a
// row's chip may be translucent (Storm's `hover`), so the pixels under the
// MXID are the chip over the canvas.
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

}

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

        // Controls derive hoverEnabled from this hint, which offscreen may
        // leave off; without hover a hover-revealed control cannot be tested.
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

    void popoverIsFixedWidthWithNoCarouselArtifacts()
    {
        openMenu();
        auto *menu = find(QStringLiteral("menu"));
        QCOMPARE(menu->property("width").toInt(), 352);

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
        QCOMPARE(aliceItem->x(), bobItem->x());
        QVERIFY(aliceItem->y() != bobItem->y());
        QVERIFY(aliceItem->y() < bobItem->y());

        QCOMPARE(bobCard->property("unreadCount").toInt(), 0);
    }

    void statusStripSpeaksOnlyWhenSomethingIsWrong()
    {
        openMenu();
        auto *strip = find(QStringLiteral("accountStatusStrip"));
        QVERIFY(strip);
        QVERIFY(strip->property("visible").toBool());
        auto *meta = find(QStringLiteral("accountStatusMeta"));
        QVERIFY(meta);
        const QString text = meta->property("text").toString();

        QVERIFY2(!text.contains(QStringLiteral("space")),
                 qPrintable(QStringLiteral("the space count came back: \"%1\"")
                                .arg(text)));

        const QString status = m_controller->connectionStatus();
        if (status == QStringLiteral("Connected")) {
            QVERIFY2(!text.contains(status),
                     qPrintable(QStringLiteral("the strip announced a healthy "
                                               "connection: \"%1\"")
                                    .arg(text)));
        } else {
            QVERIFY2(text.contains(status),
                     qPrintable(QStringLiteral("the strip swallowed \"%1\"")
                                    .arg(status)));
        }
        QVERIFY(!text.contains(QStringLiteral("Online")));

        QCOMPARE(m_root->findChildren<QObject *>(
                     QStringLiteral("accountStatusStrip")).size(), 1);

        QFile cardFile(QStringLiteral(QML_DIR "/IdentityCard.qml"));
        QVERIFY(cardFile.open(QIODevice::ReadOnly));
        const QString card = QString::fromUtf8(cardFile.readAll());
        QVERIFY(!card.contains(QStringLiteral("property string metaText")));
        QVERIFY(!card.contains(QStringLiteral("property int trustCompleted")));
        QVERIFY(!card.contains(QStringLiteral("property bool e2eeReady")));
        QVERIFY(!card.contains(QStringLiteral("TrustMeter")));
    }

    void signOutWarnsWhenRoomKeysAreNotBackedUp()
    {
        auto *health = m_controller->cryptoHealth();
        health->setSupported(true);
        health->applySnapshot({ { "recovery_state", "enabled" } },
                              health->generation());
        auto *client = m_controller->findChild<MatrixClient *>();
        QVERIFY(client);

        openMenu();
        QMetaObject::invokeMethod(find(QStringLiteral("accountFooterSignOut")),
                                  "clicked");
        auto *dialog = find(QStringLiteral("signOutConfirmDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto *warning = find(QStringLiteral("signOutKeyWarning"));
        auto *confirm = find(QStringLiteral("signOutConfirmButton"));
        auto *setUp = find(QStringLiteral("signOutSetUpRecovery"));
        QVERIFY2(warning && confirm && setUp,
                 "the sign-out dialog has no key-loss warning");

        QVERIFY(warning->property("visible").toBool());
        QVERIFY(warning->property("text").toString().contains(
            QStringLiteral("Checking")));
        QCOMPARE(confirm->property("text").toString(), QStringLiteral("Sign out"));

        health->applySnapshot({ { "recovery_state", "enabled" },
                                { "backup_exists_on_server", false } },
                              health->generation());
        Q_EMIT client->backupProgress(QStringLiteral("unknown"),
                                      QStringLiteral("unknown"), 0, 0);
        QTRY_VERIFY(warning->property("text").toString().contains(
            QStringLiteral("not backed up")));
        QVERIFY(warning->property("text").toString().contains(
            QStringLiteral("cannot be read again")));
        QVERIFY(setUp->property("visible").toBool());
        QCOMPARE(confirm->property("text").toString(),
                 QStringLiteral("Sign out anyway"));

        health->applySnapshot({ { "recovery_state", "enabled" },
                                { "backup_exists_on_server", true } },
                              health->generation());
        Q_EMIT client->backupProgress(QStringLiteral("enabled"),
                                      QStringLiteral("uploading"), 3, 10);
        QTRY_VERIFY(warning->property("text").toString().contains(
            QStringLiteral("7 room key")));

        Q_EMIT client->backupProgress(QStringLiteral("enabled"),
                                      QStringLiteral("done"), 0, 0);
        QTRY_VERIFY(!warning->property("visible").toBool());
        QVERIFY(!setUp->property("visible").toBool());
        QCOMPARE(confirm->property("text").toString(), QStringLiteral("Sign out"));

        QMetaObject::invokeMethod(dialog, "close");
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QMetaObject::invokeMethod(dialog, "open");
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(warning->property("text").toString().contains(
            QStringLiteral("Checking")));
        QMetaObject::invokeMethod(dialog, "close");
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QVERIFY(m_controller->auth()->isLoggedIn());
        health->setSupported(false);
    }

    void liveActiveUserIdGuardIgnoresReactivatingTheActiveCard()
    {
        openMenu();
        auto *aliceCard = findCard(kAlice);
        QVERIFY(aliceCard);
        QMetaObject::invokeMethod(aliceCard, "activated");
        QCoreApplication::processEvents();
        QCOMPARE(m_controller->accounts()->activeUserId(), kAlice);
        QVERIFY(!m_controller->accountSwitching());
    }

    void switchingIsSynchronousAndConfirmationsStayReachable()
    {
        openMenu();
        auto *bobCard = findCard(kBob);
        QVERIFY(bobCard);
        QMetaObject::invokeMethod(bobCard, "activated");
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
        auto *accountName = find(QStringLiteral("signOutAccountName"));
        QVERIFY2(accountName, "the sign-out confirmation does not name the account");
        QVERIFY(accountName->property("visible").toBool());
        const QString named = accountName->property("text").toString();
        QVERIFY2(named.contains(kBob), qPrintable(named));
        QVERIFY2(!named.contains(kAlice), qPrintable(named));
        QMetaObject::invokeMethod(signOutDialog, "close");
        QVERIFY(m_controller->auth()->isLoggedIn());

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
        QVERIFY(m_controller->settings()->hasSavedAccount(kAlice));
    }

    void accountSwitchingDisablesEveryRowAndFooterButton()
    {
        openMenu();
        auto *aliceCard = findCard(kAlice);
        QVERIFY(aliceCard);
        QMetaObject::invokeMethod(aliceCard, "activated");
        QVERIFY(m_controller->accountSwitching());
        auto *bobCard = findCard(kBob);
        QVERIFY(bobCard);
        QVERIFY(!bobCard->property("enabled").toBool());
        auto *addButton = find(QStringLiteral("accountFooterAdd"));
        QVERIFY(addButton);
        QVERIFY(!addButton->property("enabled").toBool());
        QTRY_VERIFY(!m_controller->accountSwitching());
    }

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

        QVERIFY2(qFuzzyIsNull(x->opacity()),
                 "the X is painted on a row nobody is pointing at");
        QVERIFY2(!x->isEnabled(),
                 "a transparent X that still takes clicks removes an "
                 "account nobody aimed at");

        const QPoint onCard = bobCard->mapToScene(
            QPointF(bobCard->width() * 0.3, bobCard->height() * 0.5)).toPoint();
        QTest::mouseMove(m_window, onCard);
        QTRY_VERIFY2(x->isVisible() && !qFuzzyIsNull(x->opacity()),
                     "hovering the card must reveal the X");
        QVERIFY(x->isEnabled());

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
        QTest::qWait(600);
        QVERIFY2(removeDialog->property("opened").toBool(),
                 "the confirm dialog did not stay open");
        QCOMPARE(dialogClosed.count(), 0);
        auto *menu = find(QStringLiteral("menu"));
        QVERIFY(menu);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QCOMPARE(m_controller->accounts()->activeUserId(), kAlice);
        QVERIFY(!m_controller->accountSwitching());
        QMetaObject::invokeMethod(removeDialog, "close");
        QTRY_VERIFY(!removeDialog->property("opened").toBool());
        QVERIFY(m_controller->settings()->hasSavedAccount(kBob));
        QTest::mouseMove(m_window, QPoint(1, 1));
    }

    // Every row is one height and the list viewport equals its content, so a
    // single account never scrolls. The active row used to be taller than the
    // probe that sized the list. Asserted as uniformity plus fit: the mock has
    // no crypto state, so either alone could pass here and fail on real
    // installs.
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
            QTest::qWait(80);

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
                    continue;
                QVERIFY2(qFuzzyCompare(item->height(), oneRow),
                         qPrintable(QStringLiteral(
                             "row %1 of %2 is %3 px, the first row is %4 px "
                             "— rows are not uniform")
                                        .arg(i).arg(expectedCount)
                                        .arg(item->height()).arg(oneRow)));
            }

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
        for (const QString &u : extra)
            m_controller->settings()->clearSessionForAccount(u);
        m_controller->settings()->clearSessionForAccount(kBob);
        checkAt(1);
        m_controller->settings()->saveSession(
            QStringLiteral("https://two.example"), kBob,
            QStringLiteral("BOBDEV"), QStringLiteral("bob-token-fixture"));
    }

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
        if (auto *sceneItem = qobject_cast<QQuickItem *>(scene.data()))
            sceneItem->setParentItem(m_window->contentItem());
        auto *plain = qvariant_cast<QQuickItem *>(scene->property("plain"));
        auto *loaded = qvariant_cast<QQuickItem *>(scene->property("loaded"));
        QVERIFY(plain && loaded);
        plain->setWidth(296);
        loaded->setWidth(296);
        QCOMPARE(plain->implicitHeight(), qreal(44));
        QCOMPARE(loaded->implicitHeight(), plain->implicitHeight());

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
        row->setWidth(328);
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

    void aLongMatrixIdFitsTheRowsTheRealPopoverLaysOut()
    {
        openMenu();
        auto *card = qobject_cast<QQuickItem *>(findCard(kAlice));
        QVERIFY(card);
        QTRY_VERIFY(card->width() > 0);
        const qreal realRowWidth = card->width();
        auto *menu = find(QStringLiteral("menu"));
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("opened").toBool());

        QQmlComponent c(m_engine);
        c.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 600; height: 200
    property alias row: row
    IdentityCard {
        id: row
        rowHeight: 44
        active: true
        displayName: "Alexandra"
        userId: "@alexandra.morgan:chat.example.org"
    }
}
)QML"), QUrl(QStringLiteral("idwidthlong.qml")));
        QScopedPointer<QObject> scene(c.create());
        QVERIFY2(scene, qPrintable(c.errorString()));
        auto *row = qvariant_cast<QQuickItem *>(scene->property("row"));
        QVERIFY(row);
        row->setWidth(realRowWidth);
        QCoreApplication::processEvents();
        auto *label = qvariant_cast<QQuickItem *>(
            row->property("identityLabel"));
        QVERIFY(label);
        QTRY_VERIFY(label->width() > 0);
        QVERIFY2(!label->property("truncated").toBool(),
                 qPrintable(QStringLiteral(
                     "'@alexandra.morgan:chat.example.org' is elided at %1 px "
                     "in a %2 px row of the real popover (needs %3)")
                        .arg(label->width()).arg(realRowWidth)
                        .arg(label->property("contentWidth").toReal())));
    }

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
        row->setWidth(328);
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

                const QColor raw = st.active ? tok("tokStormTextSecondary")
                                             : tok("tokStormTextMuted");
                if (contrastRatio(raw, fill) < 4.5)
                    ++rawTokenFailures;
            }
        }
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

    void escapeDismissesTheSwitcherAndBothConfirmations()
    {
        openMenu();
        auto *menu = find(QStringLiteral("menu"));
        QVERIFY(menu);
        QVERIFY(menu->property("opened").toBool());
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

        openMenu();
        auto *signOutButton = find(QStringLiteral("accountFooterSignOut"));
        QVERIFY(signOutButton);
        QMetaObject::invokeMethod(signOutButton, "clicked");
        auto *signOutDialog = find(QStringLiteral("signOutConfirmDialog"));
        QVERIFY(signOutDialog);
        QTRY_VERIFY(signOutDialog->property("opened").toBool());
        QTRY_VERIFY2(signOutDialog->property("activeFocus").toBool(),
                     "the sign-out confirmation never takes active focus");
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
        QFile file(QStringLiteral(QML_DIR "/AccountMenu.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(!content.contains(QStringLiteral("accessToken")));
        QVERIFY(!content.contains(QStringLiteral("Token")));
        QVERIFY(!content.contains(QStringLiteral("crypto-store")));

        QFile cardFile(QStringLiteral(QML_DIR "/IdentityCard.qml"));
        QVERIFY(cardFile.open(QIODevice::ReadOnly));
        const QString cardContent = QString::fromUtf8(cardFile.readAll());
        QVERIFY(!cardContent.contains(QStringLiteral("property string name")));
        QVERIFY(!cardContent.contains(QStringLiteral("property var name")));
    }

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
        popup->property("y");

        win->show();
        QVERIFY(QTest::qWaitForWindowExposed(win));
        QCoreApplication::processEvents();

        auto *tile = root->findChild<QQuickItem *>(
            QStringLiteral("railAccountTile"));
        QVERIFY(tile);
        QTRY_VERIFY(tile->isVisible());
        QTRY_VERIFY(tile->mapToScene(QPointF(0, 0)).y() > win->height() / 2);

        QMetaObject::invokeMethod(popup, "open");
        QTRY_VERIFY(popup->property("opened").toBool());
        auto *list = root->findChild<QQuickItem *>(
            QStringLiteral("identityCardList"));
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), 2);
        QCoreApplication::processEvents();

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

    void addAccountOpensWithoutAnEarlierVisitsError()
    {
        auto *auth = m_controller->auth();
        auth->login(QStringLiteral("matrix .org"), QStringLiteral("a"), QStringLiteral("b"));
        QVERIFY(!auth->lastError().isEmpty());
        openMenu();
        auto *add = find(QStringLiteral("accountFooterAdd"));
        QVERIFY(add);
        QMetaObject::invokeMethod(add, "clicked");
        QCOMPARE(m_controller->currentScreen(), AppController::LoginScreen);
        QVERIFY2(auth->lastError().isEmpty(), qPrintable(auth->lastError()));
        m_controller->showMain();
        QCOMPARE(m_controller->currentScreen(), AppController::MainScreen);
        QVERIFY(auth->isLoggedIn());
    }

    void anActivityBadgeCannotChangeARowsHeight()
    {
        QQmlComponent c(m_engine);
        c.setData(QByteArray(R"QML(
import QtQuick
import MatrixClient
Item {
    width: 400; height: 400
    property alias plain: plain
    property alias badged: badged
    property alias dotted: dotted
    IdentityCard {
        id: plain
        rowHeight: 44
        displayName: "Mizerd"
        userId: "@mizerd:matrix.org"
    }
    IdentityCard {
        id: badged
        rowHeight: 44
        displayName: "A rather long display name that will not fit"
        userId: "@mizerd:matrix.smetonis.net"
        liveState: AccountActivityModel.Running
        mentionCount: 128
        mentionsLowerBound: true
        needsSignIn: true
    }
    IdentityCard {
        id: dotted
        rowHeight: 44
        displayName: "Dot"
        userId: "@dot:matrix.org"
        liveState: AccountActivityModel.Running
        activityDot: true
    }
}
)QML"), QUrl(QStringLiteral("activityrowheight.qml")));
        QScopedPointer<QObject> scene(c.create());
        QVERIFY2(scene, qPrintable(c.errorString()));
        if (auto *sceneItem = qobject_cast<QQuickItem *>(scene.data()))
            sceneItem->setParentItem(m_window->contentItem());
        auto *plain = qvariant_cast<QQuickItem *>(scene->property("plain"));
        auto *badged = qvariant_cast<QQuickItem *>(scene->property("badged"));
        auto *dotted = qvariant_cast<QQuickItem *>(scene->property("dotted"));
        QVERIFY(plain && badged && dotted);
        for (QQuickItem *card : { plain, badged, dotted })
            card->setWidth(296);
        QTest::mouseMove(m_window, QPoint(m_window->width() - 2, m_window->height() - 2));
        QTest::qWait(60);

        auto badgeIn = [](QQuickItem *card) -> QQuickItem * {
            for (QQuickItem *child : card->findChildren<QQuickItem *>()) {
                if (child->objectName()
                        == QStringLiteral("identityCardActivityBadge"))
                    return child;
            }
            return nullptr;
        };
        QVERIFY(badgeIn(plain) && !badgeIn(plain)->isVisible());
        QVERIFY(badgeIn(badged) && badgeIn(badged)->isVisible());
        QCOMPARE(badgeIn(badged)->property("text").toString(), QStringLiteral("@99+"));
        QVERIFY(badgeIn(dotted) && badgeIn(dotted)->isVisible());
        QVERIFY(badgeIn(dotted)->property("dotOnly").toBool());

        QCOMPARE(plain->implicitHeight(), qreal(44));
        QCOMPARE(badged->implicitHeight(), plain->implicitHeight());
        QCOMPARE(dotted->implicitHeight(), plain->implicitHeight());
        for (QQuickItem *card : { badged, dotted }) {
            QQuickItem *badge = badgeIn(card);
            const QRectF rowRect(card->mapToScene(QPointF(0, 0)), card->size());
            const QRectF mark(badge->mapToScene(QPointF(0, 0)), badge->size());
            QVERIFY2(rowRect.contains(mark),
                     qPrintable(QStringLiteral("the badge (%1 px tall) leaves "
                                               "a %2 px row")
                                    .arg(mark.height()).arg(rowRect.height())));
        }
    }

    void aBadgedRowKeepsItsIdWholeAndStillUnderThePointer()
    {
        if (auto *menu = find(QStringLiteral("menu"))) {
            QMetaObject::invokeMethod(menu, "close");
            QTRY_VERIFY(!menu->property("opened").toBool());
        }
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
        liveState: AccountActivityModel.Running
        mentionCount: 128
    }
}
)QML"), QUrl(QStringLiteral("badgedidwidth.qml")));
        QScopedPointer<QObject> scene(c.create());
        QVERIFY2(scene, qPrintable(c.errorString()));
        auto *sceneItem = qobject_cast<QQuickItem *>(scene.data());
        QVERIFY(sceneItem);
        sceneItem->setParentItem(m_window->contentItem());
        auto *row = qvariant_cast<QQuickItem *>(scene->property("row"));
        QVERIFY(row);
        row->setWidth(328);
        QTest::mouseMove(m_window, QPoint(m_window->width() - 3,
                                          m_window->height() - 3));
        QTest::mouseMove(m_window, QPoint(m_window->width() - 2,
                                          m_window->height() - 2));
        QTRY_VERIFY(!row->property("pointerWithin").toBool());

        auto *badge = row->findChild<QQuickItem *>(
            QStringLiteral("identityCardActivityBadge"));
        QVERIFY(badge);
        QTRY_VERIFY(badge->isVisible());
        QCOMPARE(badge->property("text").toString(), QStringLiteral("@99+"));
        QQmlExpression ignored(qmlContext(badge), badge,
                               QStringLiteral("Accessible.ignored"));
        QVERIFY2(ignored.evaluate().toBool(),
                 "the row's description already carries the badge's words");

        auto *id = qvariant_cast<QQuickItem *>(row->property("identityLabel"));
        QVERIFY(id);
        QTRY_VERIFY(id->width() > 0);
        QVERIFY2(!id->property("truncated").toBool(),
                 qPrintable(QStringLiteral(
                     "'@mizerd:matrix.smetonis.net' is elided at %1 px beside "
                     "a 99+ badge (needs %2)").arg(id->width())
                        .arg(id->property("contentWidth").toReal())));
        const qreal restWidth = id->width();

        const QPoint onRow = row->mapToScene(
            QPointF(row->width() * 0.3, row->height() * 0.5)).toPoint();
        QTest::mouseMove(m_window, onRow);
        QTRY_VERIFY(row->property("pointerWithin").toBool());
        QTRY_VERIFY(qFuzzyIsNull(badge->opacity()));
        QTest::qWait(60);
        QVERIFY2(badge->isVisible(), "the badge must keep its slot on hover");
        QVERIFY2(qFuzzyCompare(id->width(), restWidth),
                 qPrintable(QStringLiteral(
                     "the id column is %1 px at rest and %2 px under the "
                     "pointer — the badge's fade reflowed the row")
                        .arg(restWidth).arg(id->width())));

        QTest::mouseMove(m_window, QPoint(m_window->width() - 2,
                                          m_window->height() - 2));
        QTRY_COMPARE(badge->opacity(), qreal(1));
        sceneItem->setParentItem(nullptr);
    }

    void theAccountListIsKeyboardReachableAndFitsAShortWindow()
    {
        m_controller->switchToAccount(kAlice);
        QTRY_VERIFY(!m_controller->accountSwitching());
        const QStringList extra{QStringLiteral("@carol:three.example"),
                                QStringLiteral("@dave:four.example"),
                                QStringLiteral("@erin:five.example"),
                                QStringLiteral("@frank:six.example"),
                                QStringLiteral("@grace:seven.example"),
                                QStringLiteral("@heidi:eight.example")};
        const QSize originalSize = m_window->size();
        struct Restore {
            IdentityCardAccountMenuTest *t;
            QStringList extra;
            QSize size;
            ~Restore()
            {
                t->m_window->resize(size);
                if (auto *m = t->find(QStringLiteral("menu")))
                    m->setProperty("y", 300);
                for (const QString &u : extra)
                    t->m_controller->settings()->clearSessionForAccount(u);
            }
        } restore{this, extra, originalSize};
        for (int i = 0; i < extra.size(); ++i)
            m_controller->settings()->saveSession(
                QStringLiteral("https://y%1.example").arg(i), extra.at(i),
                QStringLiteral("DEV"), QStringLiteral("tok"));

        auto *menu = find(QStringLiteral("menu"));
        QVERIFY(menu);
        menu->setProperty("y", 0);
        openMenu();
        auto *list = m_root->findChild<QQuickItem *>(
            QStringLiteral("identityCardList"));
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), 8);
        QTest::qWait(80);

        auto rowAt = [&](int i) {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(list, "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem *, item),
                                      Q_ARG(int, i));
            return item;
        };
        auto fullyVisible = [&](int i) {
            QQuickItem *it = rowAt(i);
            if (!it)
                return false;
            const qreal top = it->y() - list->property("contentY").toReal();
            return top >= -0.5 && top + it->height() <= list->height() + 0.5;
        };
        auto focusedRow = [&]() {
            for (int i = 0; i < 8; ++i)
                if (QQuickItem *it = rowAt(i))
                    if (it->property("focusWithin").toBool())
                        return i;
            return -1;
        };

        QQuickItem *last = rowAt(7);
        QVERIFY(last);
        QVERIFY2(!fullyVisible(7), "fixture: row 8 should start below the fold");
        last->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY2(fullyVisible(7),
                     "focus moved to row 8 and the list did not scroll to it");

        QQuickItem *row3 = rowAt(3);
        QVERIFY(row3);
        row3->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(fullyVisible(3));
        auto *x = row3->findChild<QQuickItem *>(
            QStringLiteral("identityCardRemoveButton"));
        QVERIFY(x);
        x->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(x->hasActiveFocus());
        auto *ring = x->findChild<QQuickItem *>(
            QStringLiteral("identityCardRemoveFocusRing"));
        QVERIFY2(ring, "the remove button has no focus ring item");
        QVERIFY2(ring->isVisible(),
                 "focus is on the remove X and no ring is drawn");
        QCOMPARE(focusedRow(), 3);

        QTest::keyClick(m_window, Qt::Key_End);
        QTRY_COMPARE(focusedRow(), 7);
        QTest::keyClick(m_window, Qt::Key_Home);
        QTRY_COMPARE(focusedRow(), 0);
        QTRY_VERIFY(fullyVisible(0));
        QTest::keyClick(m_window, Qt::Key_PageDown);
        QTRY_COMPARE(focusedRow(), 6);
        QTest::keyClick(m_window, Qt::Key_PageUp);
        QTRY_COMPARE(focusedRow(), 0);

        QMetaObject::invokeMethod(list, "positionViewAtEnd");
        QVERIFY(list->property("contentY").toReal() > list->property("originY").toReal() + 1);
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("opened").toBool());
        openMenu();
        QTRY_COMPARE(list->property("contentY").toReal(),
                     list->property("originY").toReal());
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("opened").toBool());

        auto *chip = qobject_cast<QQuickItem *>(
            find(QStringLiteral("accountTrustChip")));
        auto *tip = find(QStringLiteral("accountTrustTip"));
        QVERIFY2(chip && tip, "the trust chip must own its tooltip");
        // A Popup's `y` reads back its last positioned value (0 while it has
        // never been open), and the chip is hidden on the mock, so the offset
        // is asserted where it is declared: below the chip, never above it.
        QFile menuFile(QStringLiteral(QML_DIR "/AccountMenu.qml"));
        QVERIFY(menuFile.open(QIODevice::ReadOnly));
        const QString menuSrc = QString::fromUtf8(menuFile.readAll());
        const int tipAt = menuSrc.indexOf(
            QStringLiteral("objectName: \"accountTrustTip\""));
        QVERIFY(tipAt > 0);
        const QString tipBlock = menuSrc.mid(tipAt, 200);
        QVERIFY2(tipBlock.contains(QStringLiteral("y: trustChip.height")),
                 "the trust tooltip is not placed below the chip");

        m_window->resize(500, 420);
        QTRY_COMPARE(m_window->height(), 420);
        openMenu();
        QTest::qWait(120);
        auto *signOut = qobject_cast<QQuickItem *>(
            find(QStringLiteral("accountFooterSignOut")));
        QVERIFY(signOut);
        const qreal bottom =
            signOut->mapToScene(QPointF(0, signOut->height())).y();
        QVERIFY2(bottom <= 420.0,
                 qPrintable(QStringLiteral(
                     "the Sign out button ends at y %1 in a 420 px window - "
                     "the popover's content is taller than the window")
                                .arg(bottom)));
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("opened").toBool());
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    IdentityCardAccountMenuTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "IdentityCardAccountMenuTest.moc"
