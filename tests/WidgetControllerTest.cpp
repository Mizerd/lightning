
#include "matrix/MockMatrixClient.h"
#include "models/WidgetController.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 5000;

QVariantMap widget(const QString &id, const QString &url,
                   const QString &refusal = {},
                   const QStringList &discloses = {})
{
    return QVariantMap{
        { QStringLiteral("id"), id },
        { QStringLiteral("creator"), QStringLiteral("@alice:mock.local") },
        { QStringLiteral("kind"), QStringLiteral("jitsi") },
        { QStringLiteral("name"), QStringLiteral("Standup") },
        { QStringLiteral("url"), url },
        { QStringLiteral("refusal"), refusal },
        { QStringLiteral("discloses"), discloses },
    };
}

bool login(MockMatrixClient &client)
{
    QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
    client.login(QStringLiteral("https://mock.local"),
                 QStringLiteral("alice"), QStringLiteral("x"));
    return spy.wait(kSignalTimeoutMs);
}
}

class WidgetControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void listsWhatTheBackendAnswered()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockWidgets = {
            widget(QStringLiteral("w1"), QStringLiteral("https://ok.example/a"),
                   {}, { QStringLiteral("user_id"),
                         QStringLiteral("connection") }),
            widget(QStringLiteral("w2"), QStringLiteral("https://ok.example/b")),
        };
        WidgetController model;
        model.setClient(&client);
        QVERIFY(model.supported());
        model.setRoomId(QStringLiteral("!r:mock.local"));
        model.refresh();
        QVERIFY(QTest::qWaitFor([&] {
            return model.state() == QLatin1String("ready");
        }, kSignalTimeoutMs));
        QCOMPARE(model.rowCount({}), 2);
        QCOMPARE(model.rowAt(0).value(QStringLiteral("id")).toString(),
                 QStringLiteral("w1"));
        QCOMPARE(model.rowAt(0).value(QStringLiteral("discloses"))
                     .toStringList().size(), 2);
    }

    void aRefusedWidgetIsShownWithItsReasonRatherThanDropped()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockWidgets = {
            widget(QStringLiteral("bad"), QString(),
                   QStringLiteral("not_https")),
        };
        WidgetController model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:mock.local"));
        model.refresh();
        QVERIFY(QTest::qWaitFor([&] { return model.rowCount({}) == 1; },
                                kSignalTimeoutMs));
        const QModelIndex idx = model.index(0);
        QCOMPARE(model.data(idx, WidgetController::OpenableRole).toBool(), false);
        QCOMPARE(model.data(idx, WidgetController::RefusalRole).toString(),
                 QStringLiteral("not_https"));
        QCOMPARE(model.openWidget(0), false);
    }

    void openingIsByRowSoAnAddressCanNeverBeNamedFromQml()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockWidgets = {
            widget(QStringLiteral("w1"), QStringLiteral("https://ok.example/a")),
        };
        WidgetController model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:mock.local"));
        model.refresh();
        QVERIFY(QTest::qWaitFor([&] { return model.rowCount({}) == 1; },
                                kSignalTimeoutMs));
        QCOMPARE(model.openWidget(-1), false);
        QCOMPARE(model.openWidget(5), false);
        QVERIFY(model.rowAt(0).value(QStringLiteral("url")).toString()
                    .startsWith(QStringLiteral("https://")));
    }

    void anAnswerForAnotherRoomNeverRepaintsThisOne()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockWidgets = {
            widget(QStringLiteral("w1"), QStringLiteral("https://ok.example/a")),
        };
        WidgetController model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!first:mock.local"));
        model.refresh();
        model.setRoomId(QStringLiteral("!second:mock.local"));
        QTest::qWait(80);
        QCOMPARE(model.rowCount({}), 0);
        QCOMPARE(model.state(), QStringLiteral("idle"));

        Q_EMIT client.roomWidgetsReceived(999, QStringLiteral("!elsewhere:x"),
                                          true, true, client.mockWidgets);
        QTest::qWait(30);
        QCOMPARE(model.rowCount({}), 0);
    }

    void signingOutClearsTheList()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockWidgets = {
            widget(QStringLiteral("w1"), QStringLiteral("https://ok.example/a")),
        };
        WidgetController model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:mock.local"));
        model.refresh();
        QVERIFY(QTest::qWaitFor([&] { return model.rowCount({}) == 1; },
                                kSignalTimeoutMs));
        client.logout();
        QVERIFY(QTest::qWaitFor([&] { return model.rowCount({}) == 0; },
                                kSignalTimeoutMs));
    }

    void everyDisclosureKeyRendersAsASentenceIncludingUnknownOnes()
    {
        WidgetController model;
        for (const char *key : { "user_id", "display_name", "avatar_url",
                                 "device_id", "room_id", "theme", "language",
                                 "homeserver", "connection" }) {
            const QString text = model.disclosureText(QLatin1String(key));
            QVERIFY2(!text.isEmpty(), key);
            QVERIFY2(text != QLatin1String(key), key);
            QVERIFY2(text.contains(QLatin1Char(' ')),
                     qPrintable(QStringLiteral("%1 renders as a single word")
                                    .arg(QLatin1String(key))));
        }
        const QString unknown = model.disclosureText(
            QStringLiteral("org.example.something_new"));
        QVERIFY(!unknown.isEmpty());
        QVERIFY(unknown.contains(QStringLiteral("org.example.something_new")));
    }

    void everyRefusalReasonRendersAsASentence()
    {
        WidgetController model;
        for (const char *reason : { "not_https", "has_userinfo",
                                    "templated_authority", "no_host",
                                    "not_a_url" }) {
            const QString text = model.refusalText(QLatin1String(reason));
            QVERIFY2(!text.isEmpty(), reason);
            QVERIFY2(!text.contains(QLatin1String(reason)), reason);
        }
        QVERIFY(!model.refusalText(QStringLiteral("something-new")).isEmpty());
    }

    void aBackendWithoutWidgetsIsAbsentRatherThanEmpty()
    {
        WidgetController model;
        QCOMPARE(model.supported(), false);
        model.setRoomId(QStringLiteral("!r:mock.local"));
        model.refresh();
        QCOMPARE(model.rowCount({}), 0);
        QCOMPARE(model.openWidget(0), false);
    }

    void addingIsRefusedWhenTheRoomDidNotGrantIt()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockWidgetsCanManage = false;
        WidgetController model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:mock.local"));
        model.refresh();
        QVERIFY(QTest::qWaitFor([&] {
            return model.state() == QLatin1String("ready");
        }, kSignalTimeoutMs));
        QVERIFY2(!model.canManage(), "the read said no");
        model.addWidget(QStringLiteral("m.custom"), QStringLiteral("x"),
                        QStringLiteral("https://ok.example/"));
        model.removeWidget(0);
        QVERIFY2(client.widgetWrites.isEmpty(),
                 "a refused account must send nothing, not send and fail");
        WidgetController fresh;
        fresh.setClient(&client);
        fresh.setRoomId(QStringLiteral("!r:mock.local"));
        fresh.addWidget(QStringLiteral("m.custom"), QStringLiteral("x"),
                        QStringLiteral("https://ok.example/"));
        QVERIFY(client.widgetWrites.isEmpty());
    }

    void addingPublishesElementsShapeUnderAFreshIdAndRereads()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        WidgetController model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:mock.local"));
        model.refresh();
        QVERIFY(QTest::qWaitFor([&] {
            return model.state() == QLatin1String("ready");
        }, kSignalTimeoutMs));
        QVERIFY(model.canManage());

        QSignalSpy done(&model, &WidgetController::writeFinished);
        model.addWidget(QStringLiteral("m.jitsi"), QStringLiteral("Standup"),
                        QStringLiteral("https://meet.example/daily-sync"));
        QVERIFY(model.writing());
        QCOMPARE(client.widgetWrites.size(), 1);
        const auto &[room, id, json] = client.widgetWrites.first();
        QCOMPARE(room, QStringLiteral("!r:mock.local"));
        QVERIFY2(!id.isEmpty() && id != QLatin1String("Standup"),
                 "the state key is an opaque id, never the name");
        const QJsonObject content = QJsonDocument::fromJson(json.toUtf8()).object();
        QCOMPARE(content.value(QStringLiteral("type")).toString(),
                 QStringLiteral("m.jitsi"));
        QCOMPARE(content.value(QStringLiteral("url")).toString(),
                 QStringLiteral("https://meet.example/daily-sync"));
        QCOMPARE(content.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Standup"));
        QCOMPARE(content.value(QStringLiteral("creatorUserId")).toString(),
                 client.currentUserId());
        const QJsonObject data = content.value(QStringLiteral("data")).toObject();
        QCOMPARE(data.value(QStringLiteral("domain")).toString(),
                 QStringLiteral("meet.example"));
        QCOMPARE(data.value(QStringLiteral("conferenceId")).toString(),
                 QStringLiteral("daily-sync"));

        model.addWidget(QStringLiteral("m.custom"), QStringLiteral("y"),
                        QStringLiteral("https://ok.example/"));
        QCOMPARE(client.widgetWrites.size(), 1);

        QVERIFY(done.wait(kSignalTimeoutMs));
        QCOMPARE(done.first().at(0).toBool(), true);
        QVERIFY(!model.writing());
    }

    void removingWritesAnEmptyObjectUnderTheRowsOwnId()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockWidgets = {
            widget(QStringLiteral("w1"), QStringLiteral("https://ok.example/a")),
            widget(QStringLiteral("w2"), QStringLiteral("https://ok.example/b")),
        };
        WidgetController model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:mock.local"));
        model.refresh();
        QVERIFY(QTest::qWaitFor([&] {
            return model.state() == QLatin1String("ready");
        }, kSignalTimeoutMs));

        model.removeWidget(1);
        QCOMPARE(client.widgetWrites.size(), 1);
        const auto &[room, id, json] = client.widgetWrites.first();
        QCOMPARE(id, QStringLiteral("w2"));
        QCOMPARE(QJsonDocument::fromJson(json.toUtf8()).object().size(), 0);
        model.removeWidget(7);
        model.removeWidget(-1);
        QCOMPARE(client.widgetWrites.size(), 1);
    }

    void aWriteAnsweredAfterARoomSwitchDoesNotWedgeTheController()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        WidgetController model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!a:mock.local"));
        model.refresh();
        QVERIFY(QTest::qWaitFor([&] {
            return model.state() == QLatin1String("ready");
        }, kSignalTimeoutMs));
        QVERIFY(model.canManage());
        model.addWidget(QStringLiteral("m.custom"), QStringLiteral("x"),
                        QStringLiteral("https://ok.example/"));
        QVERIFY(model.writing());
        model.setRoomId(QStringLiteral("!b:mock.local"));
        QVERIFY2(!model.canManage(), "room B has granted nothing yet");
        QTRY_VERIFY2(!model.writing(), "the answer for A must free the controller");
        model.refresh();
        QVERIFY(QTest::qWaitFor([&] {
            return model.state() == QLatin1String("ready");
        }, kSignalTimeoutMs));
        model.addWidget(QStringLiteral("m.custom"), QStringLiteral("y"),
                        QStringLiteral("https://ok.example/"));
        QCOMPARE(client.widgetWrites.size(), 2);
    }

    void theAddressRuleIsTheOneTheBrowserExitApplies()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        WidgetController model;
        model.setClient(&client);
        QVERIFY(model.urlIsAcceptable(QStringLiteral("https://pad.example/p/x")));
        QVERIFY(model.urlIsAcceptable(QStringLiteral("  https://pad.example/  ")));
        QVERIFY(!model.urlIsAcceptable(QStringLiteral("http://pad.example/")));
        QVERIFY(!model.urlIsAcceptable(QStringLiteral("https://u:p@pad.example/")));
        QVERIFY(!model.urlIsAcceptable(QStringLiteral("javascript:alert(1)")));
        QVERIFY(!model.urlIsAcceptable(QStringLiteral("https://")));
        QVERIFY(!model.urlIsAcceptable(QString()));
        model.setRoomId(QStringLiteral("!r:mock.local"));
        model.refresh();
        QVERIFY(QTest::qWaitFor([&] {
            return model.state() == QLatin1String("ready");
        }, kSignalTimeoutMs));
        model.addWidget(QStringLiteral("m.custom"), QStringLiteral("x"),
                        QStringLiteral("http://pad.example/"));
        QVERIFY(client.widgetWrites.isEmpty());
    }

};

QTEST_MAIN(WidgetControllerTest)
#include "WidgetControllerTest.moc"
