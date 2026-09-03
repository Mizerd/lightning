// Widgets: the list, the consent data, and the one path out to the desktop.
//
// Lightning LISTS widgets and opens them in the user's browser rather than
// embedding them (docs/widgets.md). That makes this controller small, and it
// puts almost all of its weight on one property: no QML path may hand the
// desktop a URL that did not come from this model's own validated list.
//
// The URL rules themselves — https only, no userinfo, no templated authority,
// percent-encoded substitution — live in Rust and are tested there. What is
// tested here is the boundary: op-id and room matching, that a refused widget
// still appears with its reason, and that opening is BY ROW so an address can
// never be named from QML.

#include "matrix/MockMatrixClient.h"
#include "models/WidgetController.h"

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
} // namespace

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

    // ── A refused widget still appears ───────────────────────────────────
    //
    // Dropping it would make "Lightning will not open this" and "this room has
    // no widgets" the same observable — the shape §16 records over and over
    // as graceful absence being indistinguishable from success.
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
        // And it is refused at the door too, not merely drawn greyed out.
        QCOMPARE(model.openWidget(0), false);
    }

    // ── The one path out to the desktop ──────────────────────────────────
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
        // Out of range is refused rather than clamped: a clamp would turn a
        // wrong index into opening SOME widget, which is worse than nothing.
        QCOMPARE(model.openWidget(-1), false);
        QCOMPARE(model.openWidget(5), false);
        // Row 0 is openable; whether the desktop actually launches a browser
        // is the environment's business and is deliberately not asserted here.
        QVERIFY(model.rowAt(0).value(QStringLiteral("url")).toString()
                    .startsWith(QStringLiteral("https://")));
    }

    // ── Answers must not cross rooms or accounts ─────────────────────────
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
        // The user moves on before the answer lands.
        model.setRoomId(QStringLiteral("!second:mock.local"));
        QTest::qWait(80);
        QCOMPARE(model.rowCount({}), 0);
        QCOMPARE(model.state(), QStringLiteral("idle"));

        // A hand-delivered answer naming the wrong room is ignored even with a
        // matching op id — the id is unique per REQUEST, not per room.
        Q_EMIT client.roomWidgetsReceived(999, QStringLiteral("!elsewhere:x"),
                                          true, client.mockWidgets);
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

    // ── The notice has to say something for every key ────────────────────
    void everyDisclosureKeyRendersAsASentenceIncludingUnknownOnes()
    {
        WidgetController model;
        for (const char *key : { "user_id", "display_name", "avatar_url",
                                 "device_id", "room_id", "theme", "language",
                                 "homeserver", "connection" }) {
            const QString text = model.disclosureText(QLatin1String(key));
            QVERIFY2(!text.isEmpty(), key);
            // A SENTENCE, not the key echoed back. "theme" is allowed to
            // appear inside "Which theme you use" — what must not happen is
            // the key standing alone, which tells nobody anything.
            QVERIFY2(text != QLatin1String(key), key);
            QVERIFY2(text.contains(QLatin1Char(' ')),
                     qPrintable(QStringLiteral("%1 renders as a single word")
                                    .arg(QLatin1String(key))));
        }
        // An unknown key is disclosed HONESTLY. A widget API that grows a
        // variable this build does not know must not make the notice quietly
        // shorter — that would be the notice understating, which is the one
        // failure mode a consent screen may not have.
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
        model.refresh();   // must not crash with no client
        QCOMPARE(model.rowCount({}), 0);
        QCOMPARE(model.openWidget(0), false);
    }
};

QTEST_MAIN(WidgetControllerTest)
#include "WidgetControllerTest.moc"
