
#include "app/RoomKeyExportController.h"
#include "matrix/MatrixClient.h"

#include <QDir>
#include <QMetaProperty>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    bool supported = true;
    bool refuse = false;
    int exportCalls = 0;
    quint64 nextOp = 100;
    quint64 lastOp = 0;
    QString lastPath;
    QString lastPassphrase;

    bool supportsRoomKeyExport() const override { return supported; }
    quint64 exportRoomKeys(const QString &filePath,
                           const QString &passphrase) override
    {
        ++exportCalls;
        lastPath = filePath;
        lastPassphrase = passphrase;
        if (refuse)
            return 0;
        lastOp = nextOp++;
        return lastOp;
    }

    void progress(quint64 opId, int collected)
    {
        Q_EMIT roomKeyExportProgress(opId, collected);
    }
    void answer(quint64 opId, bool ok, int exported,
                const QString &category = {})
    {
        Q_EMIT roomKeyExportFinished(opId, ok, exported, category);
    }

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

const QString kPass = QStringLiteral("pass-SECRET-5c1e-horse");
const QString kPathMarker = QStringLiteral("PATHMARK-a77d");

const QStringList kCategories = {
    QStringLiteral("empty"),            QStringLiteral("mismatch"),
    QStringLiteral("too_short"),        QStringLiteral("too_weak"),
    QStringLiteral("invalid_path"),
    QStringLiteral("permission_denied"), QStringLiteral("disk_full"),
    QStringLiteral("write_failed"),     QStringLiteral("partial_file"),
    QStringLiteral("already_running"),  QStringLiteral("not_signed_in"),
    QStringLiteral("no_encryption"),    QStringLiteral("unavailable"),
    QStringLiteral("failed"),
};

QStringList *g_log = nullptr;
QtMessageHandler g_previous = nullptr;

void capture(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (g_log)
        g_log->append(QString::fromLatin1(ctx.category ? ctx.category : "")
                      + QLatin1Char(' ') + msg);
    if (g_previous)
        g_previous(type, ctx, msg);
}

}

class RoomKeyExportControllerTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QStringList m_log;

    QUrl target() const
    {
        return QUrl::fromLocalFile(
            m_dir.filePath(kPathMarker + QStringLiteral("-keys.txt")));
    }

    static QStringList everythingVisible(const RoomKeyExportController &ctl,
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
            if (text.contains(QStringLiteral("SECRET"))
                || text.contains(kPathMarker))
                return true;
        }
        return false;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QLoggingCategory::setFilterRules(QStringLiteral("lightning.*=true"));
        g_log = &m_log;
        g_previous = qInstallMessageHandler(capture);
    }

    void cleanupTestCase()
    {
        qInstallMessageHandler(g_previous);
        g_log = nullptr;
    }

    void emptyAndMismatchedPassphrasesAreRefusedBeforeAnyBackendCall()
    {
        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        QVERIFY(ctl.available());

        QVERIFY(!ctl.exportKeys(target(), QString(), QString()));
        QCOMPARE(ctl.resultMessage(),
                 RoomKeyExportController::describe(QStringLiteral("empty"), 0));

        QVERIFY(!ctl.exportKeys(target(), kPass, kPass + QLatin1Char('x')));
        QCOMPARE(ctl.resultMessage(),
                 RoomKeyExportController::describe(QStringLiteral("mismatch"), 0));
        QVERIFY(!ctl.exportKeys(target(), QString(), kPass));

        QCOMPARE(client.exportCalls, 0);
        QVERIFY(!ctl.busy());
        QVERIFY(!ctl.resultOk());
    }

    void aTooShortPassphraseIsRefusedCountingCharacters()
    {
        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);

        const QString seven = QStringLiteral("abcdefg");
        QVERIFY(!ctl.exportKeys(target(), seven, seven));
        QCOMPARE(ctl.resultMessage(),
                 RoomKeyExportController::describe(QStringLiteral("too_short"), 0));
        const QString emoji = QString::fromUtf8("😀😀😀😀");
        QCOMPARE(emoji.size(), 8);
        QCOMPARE(RoomKeyExportController::localProblem(emoji, emoji),
                 QStringLiteral("too_short"));
        QCOMPARE(RoomKeyExportController::strengthOf(emoji), 1);
        QCOMPARE(client.exportCalls, 0);

        const QString eight = QStringLiteral("abcdefgh");
        QCOMPARE(RoomKeyExportController::localProblem(eight, eight),
                 QStringLiteral("too_weak"));
        QCOMPARE(client.exportCalls, 0);
    }

    void aWeakPassphraseBlocksTheExport()
    {
        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        QCOMPARE(ctl.requiredStrength(), 3);
        const QString weakMessage =
            RoomKeyExportController::describe(QStringLiteral("too_weak"), 0);

        for (const QString &weak :
             { QStringLiteral("abcdefgh"),
               QStringLiteral("Abcdefg1"),
               QStringLiteral("abcdefghijk"),
               QStringLiteral("abababababababab"),
               QStringLiteral("aaaa1111aaaa1111") }) {
            QVERIFY2(RoomKeyExportController::strengthOf(weak) < 3,
                     qPrintable(weak));
            QVERIFY2(!ctl.exportKeys(target(), weak, weak), qPrintable(weak));
            QCOMPARE(ctl.resultMessage(), weakMessage);
        }
        QCOMPARE(client.exportCalls, 0);

        for (const QString &fair :
             { QStringLiteral("kzvxmwjqpdhr"), QStringLiteral("Kzvxmwj7!q") }) {
            QCOMPARE(RoomKeyExportController::strengthOf(fair), 3);
            QCOMPARE(RoomKeyExportController::localProblem(fair, fair), QString());
        }
        QVERIFY(ctl.exportKeys(target(), QStringLiteral("kzvxmwjqpdhr"),
                               QStringLiteral("kzvxmwjqpdhr")));
        QCOMPARE(client.exportCalls, 1);
    }

    void repetitionSequencesAndCommonWordsAreMarkedDown()
    {
        using C = RoomKeyExportController;
        QVERIFY(C::strengthOf(QStringLiteral("password123password123")) < 3);
        QVERIFY(C::strengthOf(QStringLiteral("Xk9#mQ2vXk9#mQ2vXk9#mQ2v")) < 3);
        QVERIFY(C::strengthOf(QStringLiteral("xk9mq2vzXK9MQ2VZ")) < 3);
        QVERIFY(C::strengthOf(QStringLiteral("abcdefghijklmnopqrstuvwx")) < 3);
        QVERIFY(C::strengthOf(QStringLiteral("1234567890987654321")) < 3);
        QVERIFY(C::strengthOf(QStringLiteral("qwertyuiopasdfgh")) < 3);
        QVERIFY(C::strengthOf(QStringLiteral("Welcome-Admin-Matrix")) < 3);
        QVERIFY(C::strengthOf(QStringLiteral("LETMEINiloveyou")) < 3);
        QVERIFY(C::strengthOf(QStringLiteral("lightning-lightning")) < 3);
        QCOMPARE(C::localProblem(QStringLiteral("password123password123"),
                                 QStringLiteral("password123password123")),
                 QStringLiteral("too_weak"));
        QCOMPARE(C::strengthOf(QStringLiteral("pass-SECRET-5c1e-horse")), 4);
        QCOMPARE(C::strengthOf(QStringLiteral("correct horse battery")), 4);
        QCOMPARE(C::strengthOf(QStringLiteral("Tr0ub4dor&3-purple-kettle")), 4);
    }

    void theStrengthHintGradesLengthAndVariety()
    {
        QCOMPARE(RoomKeyExportController::strengthOf(QString()), 0);
        QCOMPARE(RoomKeyExportController::strengthOf(QStringLiteral("short")), 1);
        QCOMPARE(RoomKeyExportController::strengthOf(QStringLiteral("abcdefgh")), 2);
        QCOMPARE(RoomKeyExportController::strengthOf(QStringLiteral("kzvxmwjqpdhr")), 3);
        QCOMPARE(RoomKeyExportController::strengthOf(QStringLiteral("Kzvxmwj7!q")), 3);
        QCOMPARE(RoomKeyExportController::strengthOf(QStringLiteral("Kzvxmwj7!qxy")), 4);
        QCOMPARE(RoomKeyExportController::strengthOf(
                     QStringLiteral("correct horse battery")), 4);
        QCOMPARE(RoomKeyExportController::strengthOf(
                     QStringLiteral("abcdabcdabcdabcdabcd")), 2);
        QCOMPARE(RoomKeyExportController::strengthOf(
                     QStringLiteral("abcdeabcdeabcdeabcde")), 2);
        RoomKeyExportController ctl;
        QCOMPARE(ctl.strength(QStringLiteral("abcdefgh")), 2);
    }

    void onlyALocalFileIsADestination()
    {
        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        const QString invalid =
            RoomKeyExportController::describe(QStringLiteral("invalid_path"), 0);

        QVERIFY(!ctl.exportKeys(QUrl(), kPass, kPass));
        QCOMPARE(ctl.resultMessage(), invalid);
        QVERIFY(!ctl.exportKeys(QUrl(QStringLiteral("https://example.org/k.txt")),
                                kPass, kPass));
        QCOMPARE(ctl.resultMessage(), invalid);
        QVERIFY(!ctl.exportKeys(QUrl::fromLocalFile(m_dir.path()), kPass, kPass));
        QCOMPARE(ctl.resultMessage(), invalid);
        QCOMPARE(client.exportCalls, 0);
    }

    void theBackendGetsExactlyThePassphraseAndPathGiven()
    {
        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        QSignalSpy state(&ctl, &RoomKeyExportController::stateChanged);

        QVERIFY(ctl.exportKeys(target(), kPass, kPass));
        QCOMPARE(client.exportCalls, 1);
        QCOMPARE(client.lastPassphrase, kPass);
        QCOMPARE(client.lastPath, target().toLocalFile());
        QVERIFY(ctl.busy());
        QVERIFY(state.count() > 0);
        QVERIFY(!ctl.exportKeys(target(), kPass, kPass));
        QCOMPARE(client.exportCalls, 1);
    }

    void progressFollowsOnlyTheLiveExportAndOnlyForwards()
    {
        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        QVERIFY(ctl.exportKeys(target(), kPass, kPass));
        const quint64 op = client.lastOp;

        client.progress(op + 1, 500);
        QCOMPARE(ctl.collected(), 0);
        client.progress(op, 500);
        QCOMPARE(ctl.collected(), 500);
        client.progress(op, 1000);
        QCOMPARE(ctl.collected(), 1000);
        client.progress(op, 500);
        QCOMPARE(ctl.collected(), 1000);

        client.answer(op, true, 1234);
        QVERIFY(!ctl.busy());
        QCOMPARE(ctl.collected(), 0);
    }

    void successReportsTheCountAndSaysHowToKeepTheFile()
    {
        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        QSignalSpy finished(&ctl, &RoomKeyExportController::finished);

        QVERIFY(ctl.exportKeys(target(), kPass, kPass));
        client.answer(client.lastOp, true, 1234);
        QVERIFY(ctl.resultOk());
        QCOMPARE(ctl.resultMessage(),
                 RoomKeyExportController::describe(QString(), 1234));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toBool(), true);
        QCOMPARE(finished.at(0).at(1).toInt(), 1234);

        QVERIFY(ctl.exportKeys(target(), kPass, kPass));
        client.answer(client.lastOp, true, 0);
        QVERIFY(ctl.resultOk());
        QVERIFY(ctl.resultMessage()
                != RoomKeyExportController::describe(QString(), 1234));
    }

    void everyCategoryHasItsOwnSentence()
    {
        QSet<QString> seen;
        for (const QString &category : kCategories) {
            const QString text = RoomKeyExportController::describe(category, 0);
            QVERIFY2(!text.isEmpty(), qPrintable(category));
            QVERIFY2(!seen.contains(text), qPrintable(category));
            seen.insert(text);
        }
        QCOMPARE(RoomKeyExportController::describe(QStringLiteral("brand_new"), 0),
                 RoomKeyExportController::describe(QStringLiteral("failed"), 0));

        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        for (const QString &category :
             { QStringLiteral("disk_full"), QStringLiteral("partial_file"),
               QStringLiteral("permission_denied"),
               QStringLiteral("already_running") }) {
            QVERIFY(ctl.exportKeys(target(), kPass, kPass));
            client.answer(client.lastOp, false, 0, category);
            QVERIFY(!ctl.resultOk());
            QCOMPARE(ctl.resultMessage(),
                     RoomKeyExportController::describe(category, 0));
        }
        QVERIFY(ctl.exportKeys(target(), kPass, kPass));
        client.answer(client.lastOp, true, 5, QStringLiteral("write_failed"));
        QVERIFY(!ctl.resultOk());
        QVERIFY(ctl.exportKeys(target(), kPass, kPass));
        client.answer(client.lastOp, false, 0);
        QCOMPARE(ctl.resultMessage(),
                 RoomKeyExportController::describe(QStringLiteral("failed"), 0));
    }

    void aRefusedStartAndAnUnsupportedBackendAreReported()
    {
        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        client.refuse = true;
        QVERIFY(!ctl.exportKeys(target(), kPass, kPass));
        QVERIFY(!ctl.busy());
        QVERIFY(!ctl.resultMessage().isEmpty());

        client.supported = false;
        QVERIFY(!ctl.available());
        QVERIFY(!ctl.exportKeys(target(), kPass, kPass));
        QCOMPARE(ctl.resultMessage(),
                 RoomKeyExportController::describe(QStringLiteral("unavailable"), 0));

        RoomKeyExportController noClient;
        QVERIFY(!noClient.available());
        QVERIFY(!noClient.exportKeys(target(), kPass, kPass));
    }

    void foreignAndLateAnswersAreDropped()
    {
        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        QSignalSpy finished(&ctl, &RoomKeyExportController::finished);

        QVERIFY(ctl.exportKeys(target(), kPass, kPass));
        const quint64 op = client.lastOp;
        client.answer(op + 7, true, 3);
        QVERIFY(ctl.busy());
        QCOMPARE(finished.count(), 0);

        client.logout();
        QVERIFY(!ctl.busy());
        client.progress(op, 500);
        client.answer(op, true, 3);
        QCOMPARE(ctl.collected(), 0);
        QVERIFY(!ctl.resultOk());
        QVERIFY(ctl.resultMessage().isEmpty());
        QCOMPARE(finished.count(), 0);

        QVERIFY(ctl.exportKeys(target(), kPass, kPass));
        const quint64 second = client.lastOp;
        Q_EMIT client.loginSucceeded(QStringLiteral("@other:example.org"));
        QVERIFY(!ctl.busy());
        client.answer(second, false, 0, QStringLiteral("disk_full"));
        QVERIFY(ctl.resultMessage().isEmpty());
    }

    void noPassphraseOrPathIsKeptOrLogged()
    {
        m_log.clear();
        FakeClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        QSignalSpy finished(&ctl, &RoomKeyExportController::finished);
        QSignalSpy progress(&client, &MatrixClient::roomKeyExportProgress);

        QVERIFY(!ctl.exportKeys(target(), kPass, kPass + QLatin1Char('!')));
        QVERIFY(ctl.exportKeys(target(), kPass, kPass));
        client.progress(client.lastOp, 500);
        QVERIFY(!leaks(everythingVisible(ctl, finished)));
        client.answer(client.lastOp, false, 0, QStringLiteral("disk_full"));
        QVERIFY(ctl.exportKeys(target(), kPass, kPass));
        client.answer(client.lastOp, true, 42);

        QVERIFY(!leaks(everythingVisible(ctl, finished)));
        for (const QList<QVariant> &args : progress) {
            for (const QVariant &arg : args)
                QVERIFY(!arg.toString().contains(QStringLiteral("SECRET")));
        }
        QVERIFY2(!m_log.isEmpty(), "the controller logs its outcomes");
        bool sawExport = false;
        for (const QString &line : std::as_const(m_log)) {
            sawExport = sawExport
                || line.startsWith(QStringLiteral("lightning.e2ee.keyexport"));
            QVERIFY2(!line.contains(QStringLiteral("SECRET"))
                         && !line.contains(kPathMarker),
                     qPrintable(line));
        }
        QVERIFY(sawExport);
    }
};

QTEST_GUILESS_MAIN(RoomKeyExportControllerTest)
#include "RoomKeyExportControllerTest.moc"
