// The Notifications page's account half: push-rule settings and the
// troubleshooter. Pins:
//   * the Rust event -> snapshot normalisation (levels, absent rules, drift);
//   * PushRulesController: load, optimistic write then the server's answer, the
//     put-back on failure, rows the server lacks being unwritable, keyword
//     validation, single-source reset on account change;
//   * NotificationTroubleshooter::evaluate for every verdict, including that
//     the operating-system check is never "pass" (it cannot be read);
//   * the real QML section against the real controller (stub `app`): rows
//     hidden for absent rules, pickers writing the right command, the drift
//     note, the error line and the troubleshooter list.
//
// Policy and wiring only: real homeserver push rules, a real notification
// service and every OS permission are not exercised here (NOT TESTED).

#include "app/NotificationTroubleshooter.h"
#include "app/PushRulesController.h"
#include "matrix/MockMatrixClient.h"
#include "matrix/PushRulesSnapshot.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

constexpr int kWaitMs = 2000;

// Records every command so a test can say what crossed the boundary.
class RecordingClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    struct Write { QString kind, key, value; };
    QList<Write> writes;
    quint64 writePushRule(const QString &kind, const QString &key,
                          const QString &value) override
    {
        writes.append({kind, key, value});
        return MockMatrixClient::writePushRule(kind, key, value);
    }
};

QVariantMap serverRules()
{
    QVariantMap m = PushRulesSnapshot::empty();
    m.insert(QStringLiteral("mentionUserId"), 2);
    m.insert(QStringLiteral("mentionDisplayName"), 2);
    m.insert(QStringLiteral("mentionUsername"), -1); // the server lacks it
    m.insert(QStringLiteral("mentionRoom"), 1);
    m.insert(QStringLiteral("eventReactions"), 0);
    m.insert(QStringLiteral("eventInvites"), 1);
    m.insert(QStringLiteral("eventNotices"), 0);
    return m;
}

QJsonObject rustEvent()
{
    const QByteArray json = R"({
      "type":"push_rules_snapshot","op_id":7,"ok":true,
      "master_mute":true,
      "dm":{"level":"all","encrypted_level":"all","drift":false,"present":true},
      "rooms":{"level":"mentions","encrypted_level":"all","drift":true,"present":true},
      "mentions":{
        "user_id":{"present":true,"level":"loud"},
        "display_name":{"present":true,"level":"notify"},
        "username":{"present":false,"level":"off"},
        "room":{"present":true,"level":"off"}},
      "events":{
        "reactions":{"present":true,"level":"off"},
        "invites":{"present":true,"level":"notify"},
        "notices":{"present":false,"level":"off"}},
      "keywords":[{"keyword":"lightning","level":"loud"},
                  {"keyword":"matrix","level":"off"},
                  {"keyword":"","level":"notify"}]})";
    return QJsonDocument::fromJson(json).object();
}

// Only what the section reads from `app`.
class StubApp final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *pushRules READ pushRules CONSTANT)
    Q_PROPERTY(QObject *notificationTroubleshooter
                   READ troubleshooter CONSTANT)
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
    Q_PROPERTY(QString currentRoomId READ currentRoomId CONSTANT)
public:
    StubApp(QObject *rules, QObject *shoot, QObject *settings)
        : m_rules(rules), m_shoot(shoot), m_settings(settings) {}
    QObject *pushRules() const { return m_rules; }
    QObject *troubleshooter() const { return m_shoot; }
    QObject *settings() const { return m_settings; }
    QString currentRoomId() const { return QStringLiteral("!open:mock.local"); }
private:
    QObject *m_rules, *m_shoot, *m_settings;
};

class StubSettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool notificationsEnabled MEMBER enabled NOTIFY changed)
public:
    bool enabled = false;
Q_SIGNALS:
    void changed();
};

QVariantMap checkById(const QVariantList &checks, const QString &id)
{
    for (const QVariant &c : checks) {
        if (c.toMap().value(QStringLiteral("id")).toString() == id)
            return c.toMap();
    }
    return {};
}

} // namespace

class PushRulesTest : public QObject
{
    Q_OBJECT

private:
    static bool login(MockMatrixClient &client)
    {
        QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        return spy.wait(kWaitMs);
    }

    static bool loadRules(PushRulesController &ctl)
    {
        QSignalSpy spy(&ctl, &PushRulesController::stateChanged);
        ctl.refresh();
        return ctl.loaded() || (spy.wait(kWaitMs) && ctl.loaded());
    }

private Q_SLOTS:
    void theRustEventBecomesTheFlatSnapshot()
    {
        const QVariantMap s = PushRulesSnapshot::fromRustEvent(rustEvent());
        QCOMPARE(s.value("masterMute").toBool(), true);
        QCOMPARE(s.value("dmAll").toBool(), true);
        QCOMPARE(s.value("roomsAll").toBool(), false);
        QCOMPARE(s.value("roomsDrift").toBool(), true);
        QCOMPARE(s.value("dmDrift").toBool(), false);
        QCOMPARE(s.value("mentionUserId").toInt(), 2);
        QCOMPARE(s.value("mentionDisplayName").toInt(), 1);
        // A rule the server lacks is -1, not Off: Off is a real setting.
        QCOMPARE(s.value("mentionUsername").toInt(), -1);
        QCOMPARE(s.value("mentionRoom").toInt(), 0);
        QCOMPARE(s.value("eventNotices").toInt(), -1);
        QCOMPARE(s.value("eventInvites").toInt(), 1);
        const QVariantList kw = s.value("keywords").toList();
        QCOMPARE(kw.size(), 2); // the empty keyword is dropped
        QCOMPARE(kw.at(0).toMap().value("keyword").toString(),
                 QStringLiteral("lightning"));
        QCOMPARE(kw.at(0).toMap().value("level").toInt(), 2);
        QCOMPARE(kw.at(1).toMap().value("level").toInt(), 0);
    }

    void aFailedEventLeavesTheEmptySnapshotNotACrash()
    {
        const QVariantMap s = PushRulesSnapshot::fromRustEvent(QJsonObject());
        QCOMPARE(s.value("mentionUserId").toInt(), -1);
        QCOMPARE(s.value("keywords").toList().size(), 0);
    }

    void refreshLoadsTheServersRules()
    {
        RecordingClient client;
        QVERIFY(login(client));
        client.mockPushRules = serverRules();
        PushRulesController ctl;
        ctl.setClient(&client);
        QVERIFY(ctl.supported());
        QVERIFY(!ctl.loaded());
        QVERIFY(loadRules(ctl));
        QCOMPARE(ctl.mentionUserId(), 2);
        QCOMPARE(ctl.mentionUsername(), -1);
        QCOMPARE(ctl.eventInvites(), 1);
        QVERIFY(!ctl.busy());
    }

    void aWriteIsShownAtOnceThenConfirmedByTheServer()
    {
        RecordingClient client;
        QVERIFY(login(client));
        client.mockPushRules = serverRules();
        PushRulesController ctl;
        ctl.setClient(&client);
        QVERIFY(loadRules(ctl));

        ctl.setMasterMute(true);
        // Optimistic: visible before the event loop turns.
        QVERIFY(ctl.masterMute());
        QVERIFY(ctl.busy());
        QCOMPARE(client.writes.size(), 1);
        QCOMPARE(client.writes.at(0).kind, QStringLiteral("master_mute"));
        QCOMPARE(client.writes.at(0).value, QStringLiteral("on"));
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);
        QVERIFY(ctl.masterMute());
        QVERIFY(ctl.errorText().isEmpty());

        ctl.setDefaultAll(true, false);
        QVERIFY(!ctl.dmAll());
        QCOMPARE(client.writes.last().kind, QStringLiteral("default_level"));
        QCOMPARE(client.writes.last().key, QStringLiteral("dm"));
        QCOMPARE(client.writes.last().value, QStringLiteral("mentions"));
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);

        ctl.setMention(QStringLiteral("displayName"), 1);
        QCOMPARE(client.writes.last().key, QStringLiteral("display_name"));
        QCOMPARE(client.writes.last().value, QStringLiteral("notify"));
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);
        QCOMPARE(ctl.mentionDisplayName(), 1);

        // An event rule is two-way: Loud collapses to Notify.
        ctl.setEventRule(QStringLiteral("reactions"), 2);
        QCOMPARE(client.writes.last().value, QStringLiteral("notify"));
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);
        QCOMPARE(ctl.eventReactions(), 1);
    }

    void aRefusedWritePutsTheServersStateBack()
    {
        RecordingClient client;
        QVERIFY(login(client));
        client.mockPushRules = serverRules();
        PushRulesController ctl;
        ctl.setClient(&client);
        QVERIFY(loadRules(ctl));

        client.mockPushRuleFailCategory = QStringLiteral("forbidden");
        QSignalSpy failed(&ctl, &PushRulesController::writeFailed);
        ctl.setMention(QStringLiteral("userId"), 0);
        QCOMPARE(ctl.mentionUserId(), 0); // shown optimistically
        QVERIFY(failed.wait(kWaitMs));
        QCOMPARE(ctl.mentionUserId(), 2); // and put back
        QVERIFY(!ctl.errorText().isEmpty());
        QVERIFY(!ctl.busy());
    }

    void aRowTheServerLacksCannotBeWritten()
    {
        RecordingClient client;
        QVERIFY(login(client));
        client.mockPushRules = serverRules();
        PushRulesController ctl;
        ctl.setClient(&client);
        QVERIFY(loadRules(ctl));
        ctl.setMention(QStringLiteral("username"), 2);
        ctl.setEventRule(QStringLiteral("notices"), 1);
        ctl.setMention(QStringLiteral("nonsense"), 1);
        QCOMPARE(client.writes.size(), 0);
    }

    void keywordsAreValidatedBeforeAnythingIsSent()
    {
        RecordingClient client;
        QVERIFY(login(client));
        PushRulesController ctl;
        ctl.setClient(&client);
        QVERIFY(loadRules(ctl));
        for (const QString &bad : {QString(), QStringLiteral("   "),
                                   QStringLiteral(".m.rule.master"),
                                   QStringLiteral("a/b"),
                                   QStringLiteral("a\nb"),
                                   QString(121, QLatin1Char('x'))}) {
            QVERIFY2(!ctl.keywordProblem(bad).isEmpty(), qPrintable(bad));
            QVERIFY(!ctl.addKeyword(bad, 1));
        }
        QVERIFY(!ctl.errorText().isEmpty());
        QCOMPARE(client.writes.size(), 0);

        QVERIFY(ctl.addKeyword(QStringLiteral("  lightning  "), 2));
        QCOMPARE(client.writes.size(), 1);
        QCOMPARE(client.writes.at(0).kind, QStringLiteral("keyword_set"));
        QCOMPARE(client.writes.at(0).key, QStringLiteral("lightning"));
        QCOMPARE(client.writes.at(0).value, QStringLiteral("loud"));
        QCOMPARE(ctl.keywords().size(), 1); // optimistic
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);
        QCOMPARE(ctl.keywords().size(), 1);

        ctl.setKeywordLevel(QStringLiteral("lightning"), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);
        QCOMPARE(ctl.keywords().size(), 1);
        QCOMPARE(ctl.keywords().at(0).toMap().value("level").toInt(), 0);

        ctl.removeKeyword(QStringLiteral("lightning"));
        QCOMPARE(ctl.keywords().size(), 0);
        QCOMPARE(client.writes.last().kind, QStringLiteral("keyword_remove"));
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);
        QCOMPARE(ctl.keywords().size(), 0);
    }

    void signingOutForgetsTheAccountsRules()
    {
        RecordingClient client;
        QVERIFY(login(client));
        client.mockPushRules = serverRules();
        PushRulesController ctl;
        ctl.setClient(&client);
        QVERIFY(loadRules(ctl));
        QVERIFY(ctl.loaded());
        ctl.resetForAccountChange();
        QVERIFY(!ctl.loaded());
        QCOMPARE(ctl.mentionUserId(), -1);
        QVERIFY(!ctl.masterMute());
    }

    // ---- Troubleshooter -------------------------------------------------

    void theTroubleshooterNeverPassesWhatItCannotRead()
    {
        NotificationTroubleshooter::Inputs in;
        in.delivery = NotificationTroubleshooter::DesktopService;
        in.appEnabled = true;
        in.rulesKnown = true;
        in.platform = QStringLiteral("linux");
        const QVariantList checks = NotificationTroubleshooter::evaluate(in);
        QCOMPARE(checkById(checks, "delivery").value("status").toInt(),
                 int(NotificationTroubleshooter::Pass));
        QCOMPARE(checkById(checks, "system").value("status").toInt(),
                 int(NotificationTroubleshooter::Unknown));
        QCOMPARE(checkById(checks, "app").value("status").toInt(),
                 int(NotificationTroubleshooter::Pass));
        QCOMPARE(checkById(checks, "account").value("status").toInt(),
                 int(NotificationTroubleshooter::Pass));
        // No room open: no room check.
        QVERIFY(checkById(checks, "room").isEmpty());
        for (const QString &platform :
             {QStringLiteral("windows"), QStringLiteral("macos")}) {
            in.platform = platform;
            QCOMPARE(checkById(NotificationTroubleshooter::evaluate(in),
                               "system").value("status").toInt(),
                     int(NotificationTroubleshooter::Unknown));
        }
    }

    void eachFailureNamesItsCauseAndOffersTheRightFix()
    {
        NotificationTroubleshooter::Inputs in;
        in.delivery = NotificationTroubleshooter::NoPath;
        in.appEnabled = false;
        in.rulesKnown = true;
        in.masterMute = true;
        in.roomId = QStringLiteral("!r:mock.local");
        in.roomMode = 2;
        in.platform = QStringLiteral("linux");
        QVariantList checks = NotificationTroubleshooter::evaluate(in);
        QCOMPARE(checkById(checks, "delivery").value("status").toInt(),
                 int(NotificationTroubleshooter::Fail));
        QCOMPARE(checkById(checks, "app").value("status").toInt(),
                 int(NotificationTroubleshooter::Fail));
        QCOMPARE(checkById(checks, "app").value("fix").toString(),
                 QStringLiteral("enableApp"));
        QCOMPARE(checkById(checks, "account").value("status").toInt(),
                 int(NotificationTroubleshooter::Fail));
        QCOMPARE(checkById(checks, "account").value("fix").toString(),
                 QStringLiteral("unmute"));
        QCOMPARE(checkById(checks, "room").value("status").toInt(),
                 int(NotificationTroubleshooter::Fail));

        // Mentions-only is a warning, not a failure; an unloaded account is
        // unknown, never a pass.
        in.delivery = NotificationTroubleshooter::TrayBalloon;
        in.appEnabled = true;
        in.rulesKnown = false;
        in.masterMute = false;
        in.roomMode = 1;
        checks = NotificationTroubleshooter::evaluate(in);
        QCOMPARE(checkById(checks, "delivery").value("status").toInt(),
                 int(NotificationTroubleshooter::Pass));
        QCOMPARE(checkById(checks, "account").value("status").toInt(),
                 int(NotificationTroubleshooter::Unknown));
        QCOMPARE(checkById(checks, "room").value("status").toInt(),
                 int(NotificationTroubleshooter::Warn));
        in.roomMode = 0;
        QCOMPARE(checkById(NotificationTroubleshooter::evaluate(in), "room")
                     .value("status").toInt(),
                 int(NotificationTroubleshooter::Pass));
    }

    void runAndTestUseTheirProviders()
    {
        NotificationTroubleshooter shoot;
        QString askedRoom;
        int sent = 0;
        shoot.setInputsProvider([&](const QString &room) {
            askedRoom = room;
            NotificationTroubleshooter::Inputs in;
            in.delivery = NotificationTroubleshooter::DesktopService;
            in.platform = QStringLiteral("linux");
            return in;
        });
        shoot.setTestSender([&] { ++sent; });
        QVERIFY(!shoot.hasRun());
        QSignalSpy changed(&shoot, &NotificationTroubleshooter::checksChanged);
        shoot.run(QStringLiteral("!x:mock.local"));
        QCOMPARE(askedRoom, QStringLiteral("!x:mock.local"));
        QVERIFY(shoot.hasRun());
        QCOMPARE(changed.size(), 1);
        QVERIFY(!shoot.checks().isEmpty());
        shoot.sendTest();
        QCOMPARE(sent, 1);
    }

    // ---- The real QML ---------------------------------------------------

    void theSectionShowsAndDrivesTheRealController()
    {
        RecordingClient client;
        QVERIFY(login(client));
        QVariantMap rules = serverRules();
        rules.insert("roomsDrift", true);
        rules.insert("eventNotices", -1);
        client.mockPushRules = rules;
        PushRulesController ctl;
        ctl.setClient(&client);
        NotificationTroubleshooter shoot;
        shoot.setInputsProvider([&](const QString &room) {
            NotificationTroubleshooter::Inputs in;
            in.delivery = NotificationTroubleshooter::DesktopService;
            in.appEnabled = false;
            in.rulesKnown = ctl.loaded();
            in.roomId = room;
            in.roomMode = 1;
            in.platform = QStringLiteral("linux");
            return in;
        });
        int tests = 0;
        shoot.setTestSender([&] { ++tests; });
        StubSettings settings;
        StubApp app(&ctl, &shoot, &settings);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &app);
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        warnings << e.toString();
                });
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient
Item {
    width: 700; height: 2000
    property alias section: s
    NotificationRulesSettings { id: s; width: 680 }
}
)"), QUrl(QStringLiteral("qrc:/pushrulestest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner != nullptr);
        auto *section = owner->property("section").value<QObject *>();
        QVERIFY(section != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(ctl.loaded(), kWaitMs);

        auto find = [&](const char *name) {
            return section->findChild<QObject *>(QString::fromLatin1(name));
        };
        for (const char *name :
             {"pushMasterMuteSwitch", "pushDmDefaultCombo",
              "pushRoomsDefaultCombo", "pushMentionUserIdCombo",
              "pushMentionDisplayNameCombo", "pushMentionRoomCombo",
              "pushEventReactionsCombo", "pushEventInvitesCombo",
              "pushKeywordField", "pushKeywordAddButton",
              "troubleshooterRunButton", "troubleshooterTestButton"}) {
            QVERIFY2(find(name) != nullptr, name);
        }
        // Rows for rules the server lacks are not offered.
        auto *username = find("pushMentionUsernameCombo");
        QVERIFY(username != nullptr);
        QVERIFY(!username->property("visible").toBool()
                || !username->parent()->property("visible").toBool());
        auto *notices = find("pushEventNoticesCombo");
        QVERIFY(notices != nullptr);
        QVERIFY(!notices->parent()->property("visible").toBool());

        // The picker shows the stored level (user id: Loud).
        QCOMPARE(find("pushMentionUserIdCombo")->property("currentIndex").toInt(), 2);
        QCOMPARE(find("pushMentionRoomCombo")->property("currentIndex").toInt(), 1);

        // The drift note is visible for a default whose variants disagree.
        auto *drift = find("pushDriftNote");
        QVERIFY(drift != nullptr);
        QVERIFY(drift->property("visible").toBool());

        // Choosing a level writes it.
        QVERIFY(QMetaObject::invokeMethod(find("pushMentionUserIdCombo"),
                                          "activated", Q_ARG(int, 1)));
        QCOMPARE(client.writes.last().kind, QStringLiteral("mention"));
        QCOMPARE(client.writes.last().key, QStringLiteral("user_id"));
        QCOMPARE(client.writes.last().value, QStringLiteral("notify"));
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);

        QVERIFY(QMetaObject::invokeMethod(find("pushDmDefaultCombo"),
                                          "activated", Q_ARG(int, 1)));
        QCOMPARE(client.writes.last().key, QStringLiteral("dm"));
        QCOMPARE(client.writes.last().value, QStringLiteral("mentions"));
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);

        // The master switch.
        QVERIFY(QMetaObject::invokeMethod(find("pushMasterMuteSwitch"), "toggled"));
        QCOMPARE(client.writes.last().kind, QStringLiteral("master_mute"));
        QCOMPARE(client.writes.last().value, QStringLiteral("on"));
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);
        QVERIFY(ctl.masterMute());

        // A keyword is typed, added and appears.
        auto *field = find("pushKeywordField");
        field->setProperty("text", QStringLiteral("lightning"));
        QVERIFY(QMetaObject::invokeMethod(find("pushKeywordAddButton"), "clicked"));
        QCOMPARE(client.writes.last().kind, QStringLiteral("keyword_set"));
        QCOMPARE(client.writes.last().key, QStringLiteral("lightning"));
        QVERIFY(field->property("text").toString().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!ctl.busy(), kWaitMs);
        QCOMPARE(ctl.keywords().size(), 1);

        // A refused keyword stays in the field and says why.
        field->setProperty("text", QStringLiteral(".bad"));
        QVERIFY(QMetaObject::invokeMethod(find("pushKeywordAddButton"), "clicked"));
        QCOMPARE(field->property("text").toString(), QStringLiteral(".bad"));
        auto *err = find("pushRulesError");
        QVERIFY(err != nullptr);
        QVERIFY(err->property("visible").toBool());

        // The troubleshooter lists its checks and offers the app switch.
        QVERIFY(QMetaObject::invokeMethod(find("troubleshooterRunButton"), "clicked"));
        QCOMPARE(shoot.checks().size(), 5);
        QVERIFY(QMetaObject::invokeMethod(find("troubleshooterTestButton"), "clicked"));
        QCOMPARE(tests, 1);

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(PushRulesTest)
#include "PushRulesTest.moc"
