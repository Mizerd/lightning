// Notification settings: the device-only schema flags, the stored-mode probe,
// the tray-refresh signal for the unsynced flag, and Settings search coverage
// of the account notification rules. Each case fails on the code before it.

#include "app/SettingsManager.h"
#include "settings/SettingsPortable.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace settingsport;

class NotificationSettingsContractTest : public QObject
{
    Q_OBJECT

    static QString qml(const QString &name)
    {
        QFile f(QStringLiteral(QML_DIR "/") + name);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString{};
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("NotificationSettingsContractTest"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
    }

    // A laptop switched to quiet for a presentation must not silence the desktop.
    void desktopNotificationSwitchesDoNotSync()
    {
        for (const char *n : { "notificationsEnabled", "notificationSound" }) {
            const Field *f = nullptr;
            for (const Field &x : fields())
                if (x.name == QLatin1String(n))
                    f = &x;
            QVERIFY2(f, n);
            QVERIFY2(!(f->flags & Sync), n);
            QVERIFY2(f->flags & Export, n);
        }
    }

    void storedModeProbeSeparatesNeverSetFromAllMessages()
    {
        SettingsManager s;
        const QString room = QStringLiteral("!probe:example.org");
        QVERIFY(!s.hasStoredRoomNotificationMode(room));
        QVERIFY(!s.hasStoredRoomNotificationMode(QString()));
        QCOMPARE(s.roomNotificationMode(room), 0);
        s.setRoomNotificationMode(room, 3);
        QVERIFY(s.hasStoredRoomNotificationMode(room));
    }

    void unsyncedFlagChangeRefreshesTheTray()
    {
        SettingsManager s;
        const QString room = QStringLiteral("!unsynced:example.org");
        QSignalSpy spy(&s, &SettingsManager::roomNotificationModeChanged);
        s.setRoomNotificationModeUnsynced(room, true, 0);
        QCOMPARE(spy.count(), 1);
        s.setRoomNotificationModeUnsynced(room, true, 0); // no change
        QCOMPARE(spy.count(), 1);
        s.setRoomNotificationModeUnsynced(room, false);
        QCOMPARE(spy.count(), 2);
    }

    void searchAndOutlineReachTheAccountRules()
    {
        const QString src = qml(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(!src.isEmpty());
        for (const char *a : { "pushMasterMuteSwitch", "pushDmDefaultCombo",
                               "pushMentionUserIdCombo", "pushKeywordField",
                               "pushEventReactionsCombo", "troubleshooterRunButton" }) {
            QVERIFY2(src.count(QStringLiteral("anchor: \"%1\"").arg(QLatin1String(a))) == 1, a);
            QVERIFY2(src.count(QStringLiteral("anchors: [\"%1\"]").arg(QLatin1String(a))) == 1, a);
        }
        QVERIFY(src.contains(QStringLiteral("keyword keywords word")));
        QVERIFY(src.contains(QStringLiteral("troubleshoot troubleshooter")));
    }

    void menuBarCheckboxIsHonestWhereTheIconCarriesNotifications()
    {
        const QString src = qml(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(src.contains(QStringLiteral("Notifications need the menu bar icon")));
        QVERIFY(!src.contains(QStringLiteral("This only controls the menu bar icon")));
    }

    void ringDescriptionSaysItIsIndependentOfTheMessageSound()
    {
        const QString src = qml(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(src.contains(QStringLiteral("independent of")));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(NotificationSettingsContractTest)
#include "NotificationSettingsContractTest.moc"
