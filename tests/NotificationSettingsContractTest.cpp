
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
        s.setRoomNotificationModeUnsynced(room, true, 0);
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

    void theRecoveryKeyOutlineEntryLandsOnACardTitledRecoveryKey()
    {
        const QString src = qml(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral(
            "{ label: qsTr(\"Recovery key\"),\n"
            "              anchors: [\"recoveryKeyCard\", \"recoveryEntry\"] }")));
        const QString card = QStringLiteral("objectName: \"recoveryKeyCard\"");
        QCOMPARE(src.count(card), 1);
        const int at = src.indexOf(card);
        const int title = src.indexOf(
            QStringLiteral("MenuSectionLabel { text: qsTr(\"Recovery key\") }"), at);
        const int entry = src.indexOf(QStringLiteral("id: recoveryEntry"), at);
        QVERIFY(title > at && title < entry);
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

    void theComboBoxRestoresItsIndexAfterAModelReset()
    {
        const QString src = qml(QStringLiteral("AppComboBox.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(src.contains(QStringLiteral("restoreMode: Binding.RestoreNone")));
        QVERIFY(src.contains(QStringLiteral("root.count === root._committedCount")));
        QVERIFY(src.contains(QStringLiteral("root._modelTurn = true")));
        const int at = src.indexOf(QStringLiteral("function _endTurn"));
        QVERIFY(at > 0);
        const int end = src.indexOf(QStringLiteral("Binding {"), at);
        QVERIFY(end > at);
        QVERIFY(!src.mid(at, end - at).contains(QRegularExpression(QStringLiteral("currentIndex\\s*=[^=]"))));
        QVERIFY(src.contains(QStringLiteral("restoreAfterModelReset")));
        QVERIFY(src.contains(QStringLiteral("height: entry.availableHeight")));
    }

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

    void crossSigningSetupButtonJumpsToTheCard()
    {
        const QString src = qml(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(src.contains(QStringLiteral("anchors: [\"crossSigningSetupCard\", \"recoveryEntry\",")));
        QVERIFY(src.contains(QStringLiteral("anchor: \"crossSigningSetupCard\" })")));
        QVERIFY(qml(QStringLiteral("CrossSigningSetupCard.qml"))
                    .contains(QStringLiteral("objectName: \"crossSigningSetupCard\"")));
    }

    void escapePeelsLayersBeforeLeavingSettings()
    {
        const QString src = qml(QStringLiteral("SettingsScreen.qml"));
        const int at = src.indexOf(QStringLiteral("settingsSearchField.text.length > 0"));
        QVERIFY(at > 0);
        const QString block = src.mid(at, 1000);
        QVERIFY(block.contains(QStringLiteral("nameColorPickerRef.open = false")));
        QVERIFY(block.contains(QStringLiteral("root.goBack()")));
        QVERIFY(block.indexOf(QStringLiteral("nameColorPicker")) < block.indexOf(QStringLiteral("root.goBack()")));
    }

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
