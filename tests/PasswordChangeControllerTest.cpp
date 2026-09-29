// Settings → Account → Change password: PasswordChangeController against a
// scripted client. Pins:
//   * mismatched, empty and too-short passwords are refused before anything
//     reaches the backend, and the length counts characters, not UTF-16 units;
//   * the sign-out choice reaches the backend as given, and the success
//     message says which it was;
//   * every backend category has its own message, and an unknown one is not
//     mistaken for any of them;
//   * one change at a time; a foreign answer is ignored, and an answer that
//     arrives after sign-out or an account switch is dropped;
//   * OAuth accounts and servers that turn m.change_password off get the
//     account page (or an explanation), never the form;
//   * no property or signal ever carries a password;
//   * a new session clears a change a store reset left pending.
//
// Policy and wiring only. The wire (session echo, logout_devices in both
// requests, nothing sent after the session ended, the error mapping) is
// pinned by rust/src/password.rs's round_trip_tests against a loopback
// server; a real Synapse only by the live check in the round note. The QML
// fields' masking and wiping: ChangePasswordSectionQmlTest.

#include "app/PasswordChangeController.h"
#include "matrix/MatrixClient.h"

#include <QMetaProperty>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    bool supported = true;
    int changeCalls = 0;
    int probeCalls = 0;
    quint64 nextOp = 100;
    quint64 lastChangeOp = 0;
    quint64 lastProbeOp = 0;
    QString lastCurrent;
    QString lastNew;
    bool lastLogoutDevices = false;

    bool supportsPasswordChange() const override { return supported; }
    quint64 changePassword(const QString &currentPassword,
                           const QString &newPassword,
                           bool logoutDevices) override
    {
        ++changeCalls;
        lastCurrent = currentPassword;
        lastNew = newPassword;
        lastLogoutDevices = logoutDevices;
        lastChangeOp = nextOp++;
        return lastChangeOp;
    }
    quint64 probePasswordChange() override
    {
        ++probeCalls;
        lastProbeOp = nextOp++;
        return lastProbeOp;
    }

    void answer(quint64 opId, bool ok, const QString &category = {})
    {
        Q_EMIT passwordChangeFinished(opId, ok, category);
    }
    void probed(quint64 opId, bool known, bool canChange, const QString &url)
    {
        Q_EMIT passwordChangeProbed(opId, known, canChange, url);
    }

    // Pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

// Distinctive, so the retention scan cannot trip on ordinary text.
const QString kCurrent = QStringLiteral("cur-SECRET-7f3a");
const QString kNew = QStringLiteral("new-SECRET-91bd");
const QString kAccountPage = QStringLiteral("https://auth.example.org/account/");

const QStringList kCategories = {
    QStringLiteral("wrong_password"), QStringLiteral("weak_password"),
    QStringLiteral("rate_limited"),   QStringLiteral("unsupported"),
    QStringLiteral("network"),        QStringLiteral("failed"),
};

} // namespace

class PasswordChangeControllerTest : public QObject
{
    Q_OBJECT

private:
    // Every property value and every captured signal argument, as text.
    static QStringList everythingVisible(const PasswordChangeController &ctl,
                                         const QSignalSpy &finished)
    {
        QStringList out;
        const QMetaObject *mo = ctl.metaObject();
        for (int i = 0; i < mo->propertyCount(); ++i)
            out << mo->property(i).read(&ctl).toString();
        for (const QList<QVariant> &args : finished) {
            for (const QVariant &arg : args)
                out << arg.toString();
        }
        return out;
    }

    static bool leaks(const QStringList &texts)
    {
        for (const QString &text : texts) {
            if (text.contains(kCurrent) || text.contains(kNew)
                || text.contains(QStringLiteral("SECRET"))) {
                return true;
            }
        }
        return false;
    }

private Q_SLOTS:
    void mismatchIsRefusedBeforeAnyBackendCall()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        QVERIFY(ctl.available());

        QVERIFY(!ctl.changePassword(kCurrent, kNew, kNew + QLatin1Char('x'),
                                    false));
        QCOMPARE(client.changeCalls, 0);
        QVERIFY(!ctl.busy());
        QVERIFY(!ctl.resultOk());
        QCOMPARE(ctl.resultMessage(),
                 PasswordChangeController::describe(QStringLiteral("mismatch"),
                                                    false));
    }

    void emptyAndShortPasswordsAreRefusedLocally()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);

        QVERIFY(!ctl.changePassword(QString(), kNew, kNew, false));
        QCOMPARE(PasswordChangeController::localProblem(QString(), kNew, kNew),
                 QStringLiteral("empty"));

        const QString seven = QStringLiteral("abcdefg");
        QVERIFY(!ctl.changePassword(kCurrent, seven, seven, false));
        QCOMPARE(ctl.resultMessage(),
                 PasswordChangeController::describe(QStringLiteral("too_short"),
                                                    false));
        // Four emoji are eight UTF-16 units but four characters.
        const QString emoji = QString::fromUtf8("😀😀😀😀");
        QCOMPARE(emoji.size(), 8);
        QCOMPARE(PasswordChangeController::localProblem(kCurrent, emoji, emoji),
                 QStringLiteral("too_short"));
        QCOMPARE(client.changeCalls, 0);

        // Exactly the minimum is enough.
        const QString eight = QStringLiteral("abcdefgh");
        QCOMPARE(PasswordChangeController::localProblem(kCurrent, eight, eight),
                 QString());
        QVERIFY(ctl.changePassword(kCurrent, eight, eight, false));
        QCOMPARE(client.changeCalls, 1);
    }

    void theSignOutChoiceReachesTheBackendAndTheMessageSaysWhich()
    {
        for (const bool signOut : { false, true }) {
            FakeClient client;
            PasswordChangeController ctl;
            ctl.setClient(&client);
            QSignalSpy finished(&ctl, &PasswordChangeController::finished);

            QVERIFY(ctl.changePassword(kCurrent, kNew, kNew, signOut));
            QVERIFY(ctl.busy());
            QCOMPARE(client.lastCurrent, kCurrent);
            QCOMPARE(client.lastNew, kNew);
            QCOMPARE(client.lastLogoutDevices, signOut);

            client.answer(client.lastChangeOp, true);
            QCOMPARE(finished.size(), 1);
            QVERIFY(finished.at(0).at(0).toBool());
            QVERIFY(!ctl.busy());
            QVERIFY(ctl.resultOk());
            QCOMPARE(ctl.resultMessage(),
                     PasswordChangeController::describe(QString(), signOut));
        }
        QVERIFY(PasswordChangeController::describe(QString(), true)
                != PasswordChangeController::describe(QString(), false));
    }

    void everyCategoryHasItsOwnMessage()
    {
        QStringList seen;
        for (const QString &category : kCategories) {
            FakeClient client;
            PasswordChangeController ctl;
            ctl.setClient(&client);
            QSignalSpy finished(&ctl, &PasswordChangeController::finished);
            QVERIFY(ctl.changePassword(kCurrent, kNew, kNew, false));
            client.answer(client.lastChangeOp, false, category);

            QCOMPARE(finished.size(), 1);
            QVERIFY(!finished.at(0).at(0).toBool());
            const QString message = finished.at(0).at(1).toString();
            QVERIFY2(!message.isEmpty(), qPrintable(category));
            QCOMPARE(ctl.resultMessage(), message);
            QVERIFY(!ctl.resultOk());
            QVERIFY2(!seen.contains(message),
                     qPrintable(category + QStringLiteral(" shares a message")));
            seen << message;
        }
        // The success texts are not reused for a failure.
        QVERIFY(!seen.contains(PasswordChangeController::describe({}, false)));
        QVERIFY(!seen.contains(PasswordChangeController::describe({}, true)));

        // An unknown category (or none) reads as the generic failure, not as
        // any specific cause.
        const QString generic =
            PasswordChangeController::describe(QStringLiteral("failed"), false);
        QCOMPARE(PasswordChangeController::describe(
                     QStringLiteral("M_SOMETHING_NEW"), false),
                 generic);
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        QVERIFY(ctl.changePassword(kCurrent, kNew, kNew, false));
        client.answer(client.lastChangeOp, false, QString());
        QCOMPARE(ctl.resultMessage(), generic);
        QVERIFY(!ctl.resultOk());
    }

    void oneChangeAtATime()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        QVERIFY(ctl.changePassword(kCurrent, kNew, kNew, false));
        QVERIFY(!ctl.changePassword(kCurrent, kNew, kNew, true));
        QCOMPARE(client.changeCalls, 1);
        QCOMPARE(client.lastLogoutDevices, false);
    }

    void aForeignAnswerIsIgnored()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        QSignalSpy finished(&ctl, &PasswordChangeController::finished);
        QVERIFY(ctl.changePassword(kCurrent, kNew, kNew, false));

        client.answer(client.lastChangeOp + 1, true);
        client.answer(0, true);
        QCOMPARE(finished.size(), 0);
        QVERIFY(ctl.busy());

        client.answer(client.lastChangeOp, false,
                      QStringLiteral("wrong_password"));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!ctl.busy());
    }

    void aLateAnswerAfterSignOutIsDropped()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        QSignalSpy finished(&ctl, &PasswordChangeController::finished);
        QVERIFY(ctl.changePassword(kCurrent, kNew, kNew, true));
        const quint64 op = client.lastChangeOp;

        // Sign-out and an account switch both end in loggedOut.
        client.logout();
        QVERIFY(!ctl.busy());

        client.answer(op, true);
        QCOMPARE(finished.size(), 0);
        QVERIFY(ctl.resultMessage().isEmpty());
        QVERIFY(!ctl.resultOk());
    }

    void passwordIsNeverRetained()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        QSignalSpy finished(&ctl, &PasswordChangeController::finished);

        // Refused locally.
        QVERIFY(!ctl.changePassword(kCurrent, kNew, kCurrent, false));
        QVERIFY(!leaks(everythingVisible(ctl, finished)));

        // Sent: in flight, failed, and succeeded.
        QVERIFY(ctl.changePassword(kCurrent, kNew, kNew, true));
        QVERIFY(!leaks(everythingVisible(ctl, finished)));
        client.answer(client.lastChangeOp, false,
                      QStringLiteral("wrong_password"));
        QVERIFY(!leaks(everythingVisible(ctl, finished)));
        QVERIFY(ctl.changePassword(kCurrent, kNew, kNew, false));
        client.answer(client.lastChangeOp, true);
        QCOMPARE(finished.size(), 2);
        QVERIFY(!leaks(everythingVisible(ctl, finished)));

        // The scan can see a leak: a property that did carry one is caught.
        QVERIFY(leaks({ QStringLiteral("x ") + kNew }));
    }

    void oauthAccountsGetTheAccountPage()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        bool oauth = true;
        ctl.setOAuthAccountCheck([&oauth] { return oauth; });

        QCOMPARE(ctl.mode(), QStringLiteral("external"));
        QVERIFY(!ctl.changePassword(kCurrent, kNew, kNew, false));
        QCOMPARE(client.changeCalls, 0);

        ctl.refresh();
        QCOMPARE(client.probeCalls, 1);
        client.probed(client.lastProbeOp, true, false, kAccountPage);
        QCOMPARE(ctl.mode(), QStringLiteral("external"));
        QCOMPARE(ctl.managementUrl(), kAccountPage);

        // A password account on the same server kind reads the probe.
        oauth = false;
        QCOMPARE(ctl.mode(), QStringLiteral("external"));
    }

    void theCapabilityDecidesForPasswordAccounts()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        ctl.setOAuthAccountCheck([] { return false; });

        // Before any answer, and when the server could not be asked, the form
        // is offered and the server decides.
        QCOMPARE(ctl.mode(), QStringLiteral("form"));
        ctl.refresh();
        client.probed(client.lastProbeOp, false, true, QString());
        QCOMPARE(ctl.mode(), QStringLiteral("form"));

        ctl.refresh();
        client.probed(client.lastProbeOp, true, true, QString());
        QCOMPARE(ctl.mode(), QStringLiteral("form"));

        // Turned off, with and without an account page.
        ctl.refresh();
        client.probed(client.lastProbeOp, true, false, QString());
        QCOMPARE(ctl.mode(), QStringLiteral("unavailable"));
        QVERIFY(!ctl.changePassword(kCurrent, kNew, kNew, false));
        QCOMPARE(client.changeCalls, 0);

        ctl.refresh();
        client.probed(client.lastProbeOp, true, false, kAccountPage);
        QCOMPARE(ctl.mode(), QStringLiteral("external"));
        QCOMPARE(ctl.managementUrl(), kAccountPage);
    }

    void aProbeFromAnEndedSessionIsIgnored()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        ctl.refresh();
        const quint64 op = client.lastProbeOp;
        ctl.refresh(); // one probe at a time
        QCOMPARE(client.probeCalls, 1);

        client.logout();
        client.probed(op, true, false, kAccountPage);
        QCOMPARE(ctl.mode(), QStringLiteral("form"));
        QVERIFY(ctl.managementUrl().isEmpty());
    }

    void aNewSessionIsProbedOnlyOnceTheSectionAsked()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);

        Q_EMIT client.loginSucceeded(QStringLiteral("@me:example.org"));
        QCOMPARE(client.probeCalls, 0);

        ctl.refresh();
        client.probed(client.lastProbeOp, true, false, kAccountPage);
        QCOMPARE(client.probeCalls, 1);

        // Account switch: the old answer is gone and the new session is asked.
        client.logout();
        QVERIFY(ctl.managementUrl().isEmpty());
        Q_EMIT client.loginSucceeded(QStringLiteral("@other:example.org"));
        QCOMPARE(client.probeCalls, 2);
    }

    void aStoreResetDoesNotLeaveAChangeStuck()
    {
        FakeClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        QSignalSpy finished(&ctl, &PasswordChangeController::finished);
        QVERIFY(ctl.changePassword(kCurrent, kNew, kNew, false));
        const quint64 op = client.lastChangeOp;
        ctl.refresh();
        QVERIFY(ctl.busy());

        // A store reset releases the session without loggedOut; the next
        // session starts with loginSucceeded.
        Q_EMIT client.loginSucceeded(QStringLiteral("@me:example.org"));
        QVERIFY(!ctl.busy());
        QVERIFY(ctl.changePassword(kCurrent, kNew, kNew, false));
        QCOMPARE(client.changeCalls, 2);
        // The old op can no longer finish the new one.
        client.answer(op, true);
        QCOMPARE(finished.size(), 0);
        QVERIFY(ctl.busy());
    }

    void aBackendWithoutTheFeatureOffersNothing()
    {
        FakeClient client;
        client.supported = false;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        QVERIFY(!ctl.available());
        QVERIFY(!ctl.changePassword(kCurrent, kNew, kNew, false));
        ctl.refresh();
        QCOMPARE(client.changeCalls, 0);
        QCOMPARE(client.probeCalls, 0);

        PasswordChangeController none;
        QVERIFY(!none.available());
        QVERIFY(!none.changePassword(kCurrent, kNew, kNew, false));
    }
};

QTEST_MAIN(PasswordChangeControllerTest)
#include "PasswordChangeControllerTest.moc"
