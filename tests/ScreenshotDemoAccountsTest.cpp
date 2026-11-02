
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"
#include "storage/SecretStore.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {
const QString kAlex   = QStringLiteral("@alex:lightning.example");
const QString kTaylor = QStringLiteral("@taylor:workplace.example");
const QString kNova   = QStringLiteral("@nova:community.example");

bool roomsContain(const QList<RoomInfo> &rooms, const QString &id)
{
    for (const auto &r : rooms)
        if (r.id == id)
            return true;
    return false;
}
}

class ScreenshotDemoAccountsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("screenshot-demo-accounts-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void mockSeedsThreeAccountsInOrder()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        QCOMPARE(c.demoAccountUserIds(),
                 (QStringList{ kAlex, kTaylor, kNova }));
        QCOMPARE(c.activeDemoAccount(), kAlex);
        QVERIFY(roomsContain(c.rooms(),
                             QStringLiteral("!design-lounge:lightning.example")));
    }

    void activatingSwapsTheWholeScene()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        c.activateDemoAccount(kTaylor);
        QCOMPARE(c.activeDemoAccount(), kTaylor);
        QVERIFY(roomsContain(c.rooms(),
                             QStringLiteral("!aurora:workplace.example")));
        QVERIFY(!roomsContain(c.rooms(),
                              QStringLiteral("!design-lounge:lightning.example")));

        c.activateDemoAccount(kNova);
        QVERIFY(roomsContain(c.rooms(),
                             QStringLiteral("!general:community.example")));
        QVERIFY(!roomsContain(c.rooms(),
                              QStringLiteral("!aurora:workplace.example")));
    }

    void localMutationsSurviveARoundTrip()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const QString aurora = QStringLiteral("!aurora:workplace.example");
        c.activateDemoAccount(kTaylor);
        const int before = c.timeline(aurora).size();
        c.sendTextMessage(aurora, QStringLiteral("demo local echo"));
        QCOMPARE(c.timeline(aurora).size(), before + 1);

        c.activateDemoAccount(kNova);
        c.activateDemoAccount(kAlex);
        c.activateDemoAccount(kTaylor);
        const auto tl = c.timeline(aurora);
        QCOMPARE(tl.size(), before + 1);
        QCOMPARE(tl.last().body, QStringLiteral("demo local echo"));
    }

    void resetRestoresDeterministicState()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const QString design = QStringLiteral("!design-lounge:lightning.example");
        const int before = c.timeline(design).size();
        c.sendTextMessage(design, QStringLiteral("scratch"));
        QCOMPARE(c.timeline(design).size(), before + 1);
        c.resetDemoData();
        QCOMPARE(c.activeDemoAccount(), kAlex);
        QCOMPARE(c.timeline(design).size(), before);
    }

    void eventIdsAndDomainsAreDeterministicAndSafe()
    {
        MockMatrixClient a, b;
        a.setScreenshotDemoMode(true);
        b.setScreenshotDemoMode(true);
        auto safe = [](const QString &s) {
            return !s.contains(QStringLiteral("smetonis"))
                && !s.contains(QStringLiteral("matrix.org"))
                && !s.contains(QStringLiteral("mock.local"))
                && !s.contains(QStringLiteral("bankera"));
        };
        for (const QString &uid : a.demoAccountUserIds()) {
            a.activateDemoAccount(uid);
            b.activateDemoAccount(uid);
            const auto ra = a.rooms();
            const auto rb = b.rooms();
            QCOMPARE(ra.size(), rb.size());
            for (const auto &r : ra) {
                QVERIFY2(safe(r.id), qUtf8Printable("unsafe room id: " + r.id));
                const auto ta = a.timeline(r.id);
                const auto tb = b.timeline(r.id);
                QCOMPARE(ta.size(), tb.size());
                for (int i = 0; i < ta.size(); ++i) {
                    QCOMPARE(ta[i].eventId, tb[i].eventId);
                    QVERIFY2(safe(ta[i].sender),
                             qUtf8Printable("unsafe sender: " + ta[i].sender));
                    QVERIFY2(safe(ta[i].eventId),
                             qUtf8Printable("unsafe event id: " + ta[i].eventId));
                }
            }
        }
    }

    void appControllerRegistersThreeAccountsOnInMemoryStore()
    {
        AppController app(AppController::MockBackend,true);
        QVERIFY(!app.settings()->secretsAreSecure());
        QVERIFY(app.settings()->secretBackendName().contains(
            QStringLiteral("in-memory")));

        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        QCOMPARE(app.accounts()->activeUserId(), kAlex);

        const QStringList ids = app.settings()->savedAccountUserIds();
        QCOMPARE(ids, (QStringList{ kAlex, kTaylor, kNova }));
        for (const QString &uid : ids) {
            const QVariantMap rec = app.accounts()->account(uid);
            QVERIFY(rec.value(QStringLiteral("homeserver")).toString()
                        .contains(QStringLiteral(".example")));
            QVERIFY(!rec.value(QStringLiteral("displayName")).toString().isEmpty());
            QVERIFY(app.settings()->accessTokenFor(uid).isEmpty());
        }
        QVERIFY(!app.settings()->secretsAreSecure());
    }

    void realSwitcherPathSwitchesBetweenDemoAccounts()
    {
        AppController app(AppController::MockBackend,true);
        app.beginScreenshotDemo();
        QTRY_COMPARE(app.currentScreen(), AppController::MainScreen);
        QCOMPARE(app.accounts()->activeUserId(), kAlex);

        app.switchToAccount(kTaylor);
        QVERIFY(app.accountSwitching());
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.accounts()->activeUserId(), kTaylor);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);

        app.switchToAccount(kAlex);
        QTRY_VERIFY(!app.accountSwitching());
        QCOMPARE(app.accounts()->activeUserId(), kAlex);
        QCOMPARE(app.currentScreen(), AppController::MainScreen);
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(ScreenshotDemoAccountsTest)
#include "ScreenshotDemoAccountsTest.moc"
