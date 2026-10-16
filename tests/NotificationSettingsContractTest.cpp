// Notification settings: the device-only schema flags, the stored-mode probe,
// the tray-refresh signal for the unsynced flag, and Settings search coverage
// of the account notification rules. Each case fails on the code before it.

#include "app/SettingsManager.h"
#include "settings/SettingsPortable.h"

#include <QRegularExpression>
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

    // B017/B018: AppComboBox itself re-applies the committed index after a
    // retranslation (qsTr models reset currentIndex to 0 without `activated`),
    // through a Binding that does not destroy the caller's own binding.
    void theComboBoxRestoresItsIndexAfterAModelReset()
    {
        const QString src = qml(QStringLiteral("AppComboBox.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral("restoreMode: Binding.RestoreNone")));
        QVERIFY(src.contains(QStringLiteral("root.count === root._committedCount")));
        QVERIFY(src.contains(QStringLiteral("root._modelTurn = true")));
        // No imperative write to currentIndex in the generic path.
        const int at = src.indexOf(QStringLiteral("function _endTurn"));
        QVERIFY(at > 0);
        const int end = src.indexOf(QStringLiteral("Binding {"), at);
        QVERIFY(end > at);
        QVERIFY(!src.mid(at, end - at).contains(QRegularExpression(QStringLiteral("currentIndex\\s*=[^=]"))));
        QVERIFY(src.contains(QStringLiteral("restoreAfterModelReset")));
        QVERIFY(src.contains(QStringLiteral("height: entry.availableHeight")));
    }

    // B078/B080: Verify needs an unverified session and an identity to ask.
    void verifyIsOfferedOnlyWhenItCanSucceed()
    {
        const QString src = qml(QStringLiteral("SettingsScreen.qml"));
        const int at = src.indexOf(QStringLiteral("showVerify: !app.verificationActive"));
        QVERIFY(at > 0);
        const QString block = src.mid(at, 700);
        QVERIFY(block.contains(QStringLiteral("app.sessionTrustState !== \"Verified\"")));
        QVERIFY(block.contains(QStringLiteral("identityAvailable")));
        QVERIFY(src.contains(QStringLiteral("crossSigningSetup !== \"not_set_up\"")));
    }

    // B081: the set-up button lands on the real card, which is the first anchor.
    void crossSigningSetupButtonJumpsToTheCard()
    {
        const QString src = qml(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(src.contains(QStringLiteral("anchors: [\"crossSigningSetupCard\", \"recoveryEntry\",")));
        QVERIFY(src.contains(QStringLiteral("anchor: \"crossSigningSetupCard\" })")));
        QVERIFY(qml(QStringLiteral("CrossSigningSetupCard.qml"))
                    .contains(QStringLiteral("objectName: \"crossSigningSetupCard\"")));
    }

    // B094/B138: Escape peels the query, then the picker, then Settings.
    void escapePeelsLayersBeforeLeavingSettings()
    {
        const QString src = qml(QStringLiteral("SettingsScreen.qml"));
        const int at = src.indexOf(QStringLiteral("settingsSearchField.text.length > 0"));
        QVERIFY(at > 0);
        const QString block = src.mid(at, 400);
        QVERIFY(block.contains(QStringLiteral("nameColorPicker.open = false")));
        QVERIFY(block.contains(QStringLiteral("root.goBack()")));
        QVERIFY(block.indexOf(QStringLiteral("nameColorPicker")) < block.indexOf(QStringLiteral("root.goBack()")));
    }

    // B102, B088: plural-aware footer, no example chips under results, and a
    // spell-check switch that is disabled when the engine is unavailable.
    void searchFooterIsPluralAwareAndSpellSwitchHonest()
    {
        const QString src = qml(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(src.contains(QStringLiteral("qsTr(\"%n section(s)\", \"\", sections)")));
        QVERIFY(!src.contains(QStringLiteral("%1 sections")));
        QVERIFY(src.contains(QStringLiteral("visible: root.matchedSearchResults.length === 0\n                            Layout.fillWidth: true\n                            Layout.topMargin: AppTheme.spacing4")));
        const int at = src.indexOf(QStringLiteral("objectName: \"spellCheckEnabledCheck\""));
        QVERIFY(at > 0);
        QVERIFY(src.mid(at, 700).contains(QStringLiteral("enabled: !app.spell || app.spell.available")));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(NotificationSettingsContractTest)
#include "NotificationSettingsContractTest.moc"
