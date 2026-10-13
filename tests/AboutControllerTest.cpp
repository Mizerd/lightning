// About page controller: the copyable version summary, the homeserver card,
// "Reset local cache" and the read-only account-data browser.
//
// Reset must report partial failure honestly, and every answer is matched to
// the request that asked for it, so a slow answer for one account never lands
// under another.

#include "app/AboutController.h"
#include "matrix/MockMatrixClient.h"
#include "models/MessageSearchController.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

/// A mock whose media clear answers when the test says so.
class ResetClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    quint64 mediaOp = 0;
    quint64 clearStoredMedia() override { return mediaOp = 4242; }
};

/// A backend that refuses every clear.
class RefusingClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    quint64 clearStoredMedia() override { return 0; }
    quint64 clearEventCache() override { return 0; }
    bool clearSearchIndex() override { return false; }
};

QString readSource(const QString &relative)
{
    QFile file(QStringLiteral(REPO_ROOT "/") + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

} // namespace

class AboutControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theVersionSummaryNamesVersionCommitPlatformAndPackage()
    {
        AboutController about;
        about.setPackageKind(QStringLiteral("AppImage"));
        const QString summary = about.versionSummary();
        const QStringList lines = summary.split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 4);
        QVERIFY(lines.at(0).startsWith(QStringLiteral("Lightning ")));
        QVERIFY(lines.at(0).contains(QStringLiteral(APP_VERSION)));
        QVERIFY(lines.at(1).startsWith(QStringLiteral("Commit: ")));
        QVERIFY(lines.at(2).startsWith(QStringLiteral("Platform: ")));
        QCOMPARE(lines.at(3), QStringLiteral("Package: AppImage"));
    }

    void copyVersionPutsTheSummaryOnTheClipboard()
    {
        AboutController about;
        QGuiApplication::clipboard()->setText(QStringLiteral("stale"));
        about.copyVersion();
        QCOMPARE(QGuiApplication::clipboard()->text(), about.versionSummary());
    }

    void reportIssueGoesToTheGitHubIssuesPage()
    {
        AboutController about;
        QCOMPARE(about.reportIssueUrl(),
                 QStringLiteral("https://github.com/Mizerd/lightning/issues"));
    }

    void theHomeserverCardLoadsAndIgnoresAStaleAnswer()
    {
        MockMatrixClient client;
        AboutController about;
        about.setClient(&client);
        QCOMPARE(about.homeserverState(), QStringLiteral("idle"));

        about.refreshHomeserver();
        QCOMPARE(about.homeserverState(), QStringLiteral("loading"));
        QCOMPARE(client.homeserverInfoRequests, 1);

        // An answer to some other request changes nothing.
        Q_EMIT client.homeserverInfoReceived(
            1, QVariantMap{{QStringLiteral("ok"), true}});
        QCOMPARE(about.homeserverState(), QStringLiteral("loading"));

        QTRY_COMPARE(about.homeserverState(), QStringLiteral("ready"));
        const QVariantMap hs = about.homeserver();
        QCOMPARE(hs.value(QStringLiteral("address")).toString(),
                 QStringLiteral("https://matrix.mock.local"));
        QCOMPARE(hs.value(QStringLiteral("softwareName")).toString(),
                 QStringLiteral("Synapse"));
        QCOMPARE(hs.value(QStringLiteral("features")).toList().size(), 2);
    }

    void aFailedHomeserverAnswerSaysWhy()
    {
        MockMatrixClient client;
        AboutController about;
        about.setClient(&client);
        about.refreshHomeserver();
        // Take the op the mock handed out, after its canned answer fires.
        QTRY_COMPARE(about.homeserverState(), QStringLiteral("ready"));
        about.refreshHomeserver();
        Q_EMIT client.homeserverInfoReceived(
            client.m_aboutOp - 1,
            QVariantMap{{QStringLiteral("ok"), false},
                        {QStringLiteral("category"), QStringLiteral("network")}});
        QCOMPARE(about.homeserverState(), QStringLiteral("failed"));
        QVERIFY(about.homeserver().value(QStringLiteral("error")).toString()
                    .contains(QStringLiteral("could not be reached")));
    }

    void noClientMeansUnsupportedNotLoadingForever()
    {
        AboutController about;
        about.refreshHomeserver();
        QCOMPARE(about.homeserverState(), QStringLiteral("unsupported"));
        about.loadAccountData();
        QCOMPARE(about.accountDataState(), QStringLiteral("unsupported"));
    }

    void resetClearsTheEventCacheAndTheSearchIndex()
    {
        MockMatrixClient client;
        MessageSearchController search;
        search.setClient(&client);
        AboutController about;
        about.setClient(&client);
        about.setSearch(&search);

        about.resetLocalCache();
        QCOMPARE(about.resetState(), QStringLiteral("running"));
        QCOMPARE(client.eventCacheClears, 1);
        QVERIFY(client.indexCleared);
        QTRY_COMPARE(about.resetState(), QStringLiteral("done"));
        QVERIFY(about.resetSummary().contains(QStringLiteral("not touched")));
    }

    void resetWaitsForTheMediaCacheToo()
    {
        ResetClient client;
        MessageSearchController search;
        search.setClient(&client);
        AboutController about;
        about.setClient(&client);
        about.setSearch(&search);

        about.resetLocalCache();
        // The event cache answered; the media cache has not.
        QTest::qWait(30);
        QCOMPARE(about.resetState(), QStringLiteral("running"));
        Q_EMIT client.storedMediaCleared(client.mediaOp, true, 3, 1024);
        QCOMPARE(about.resetState(), QStringLiteral("done"));
    }

    void aPartialFailureNamesWhatFailedAndStillSaysKeysAreSafe()
    {
        ResetClient client;
        MessageSearchController search;
        search.setClient(&client);
        AboutController about;
        about.setClient(&client);
        about.setSearch(&search);

        about.resetLocalCache();
        QTest::qWait(30);
        Q_EMIT client.storedMediaCleared(client.mediaOp, false, 0, 0);
        QCOMPARE(about.resetState(), QStringLiteral("failed"));
        QVERIFY(about.resetSummary().contains(
            QStringLiteral("Could not clear: media cache")));
        QVERIFY(about.resetSummary().contains(
            QStringLiteral("Cleared: event cache, search index")));
        QVERIFY(about.resetSummary().contains(QStringLiteral("not touched")));
    }

    void anAnswerForSomeOtherOperationDoesNotFinishTheReset()
    {
        ResetClient client;
        MessageSearchController search;
        search.setClient(&client);
        AboutController about;
        about.setClient(&client);
        about.setSearch(&search);
        about.resetLocalCache();
        QTest::qWait(30);
        Q_EMIT client.storedMediaCleared(client.mediaOp + 1, true, 0, 0);
        QCOMPARE(about.resetState(), QStringLiteral("running"));
    }

    // A refused request removed nothing; it must not read as success.
    void aRefusedStepIsAFailureNotASilentSuccess()
    {
        RefusingClient client;
        MessageSearchController search;
        search.setClient(&client);
        AboutController about;
        about.setClient(&client);
        about.setSearch(&search);

        about.resetLocalCache();
        QCOMPARE(about.resetState(), QStringLiteral("failed"));
        const QString summary = about.resetSummary();
        QVERIFY(summary.startsWith(QStringLiteral("Nothing was cleared")));
        for (const char *what : {"media cache", "event cache", "search index"})
            QVERIFY2(summary.contains(QLatin1String(what)), what);
        QVERIFY(!summary.startsWith(QStringLiteral("Cleared the")));
    }

    void withNoSearchControllerTheIndexStepFailsHonestly()
    {
        MockMatrixClient client;
        AboutController about;
        about.setClient(&client);
        about.resetLocalCache();
        QTRY_COMPARE(about.resetState(), QStringLiteral("failed"));
        QVERIFY(about.resetSummary().contains(
            QStringLiteral("Could not clear: search index")));
    }

    void resetWithoutAnAccountClearsNothingAndSaysSo()
    {
        AboutController about;
        about.resetLocalCache();
        QCOMPARE(about.resetState(), QStringLiteral("failed"));
        QVERIFY(about.resetSummary().contains(QStringLiteral("Nothing was cleared")));
    }

    void theAccountDataBrowserListsGlobalAndPerRoom()
    {
        MockMatrixClient client;
        AboutController about;
        about.setClient(&client);

        about.loadAccountData();
        QCOMPARE(about.accountDataState(), QStringLiteral("loading"));
        QTRY_COMPARE(about.accountDataState(), QStringLiteral("ready"));
        QCOMPARE(about.accountDataEntries().size(), 2);
        const QVariantMap secret = about.accountDataEntries().at(0).toMap();
        QCOMPARE(secret.value(QStringLiteral("type")).toString(),
                 QStringLiteral("m.cross_signing.master"));
        QVERIFY(secret.value(QStringLiteral("redacted")).toBool());
        QVERIFY(secret.value(QStringLiteral("json")).toString().isEmpty());

        about.loadAccountData(QStringLiteral("!room:mock.local"));
        QCOMPARE(about.accountDataRoomId(), QStringLiteral("!room:mock.local"));
        QCOMPARE(about.accountDataEntries().size(), 0);   // nothing stale shown
        QTRY_COMPARE(about.accountDataState(), QStringLiteral("ready"));
        QCOMPARE(about.accountDataEntries().size(), 1);
        QCOMPARE(client.accountDataRequests,
                 (QStringList{QString(), QStringLiteral("!room:mock.local")}));
    }

    void aLateListingForAnEarlierRequestIsDropped()
    {
        MockMatrixClient client;
        AboutController about;
        about.setClient(&client);
        about.loadAccountData();
        about.loadAccountData(QStringLiteral("!b:mock.local"));
        QTRY_COMPARE(about.accountDataState(), QStringLiteral("ready"));
        // Both canned answers have fired by now; only the second may have won.
        QCOMPARE(about.accountDataRoomId(), QStringLiteral("!b:mock.local"));
        QCOMPARE(about.accountDataEntries().size(), 1);
    }

    void signingOutDropsEverythingTheBrowserHeld()
    {
        MockMatrixClient client;
        AboutController about;
        about.setClient(&client);
        about.loadAccountData();
        QTRY_COMPARE(about.accountDataState(), QStringLiteral("ready"));
        about.refreshHomeserver();
        QTRY_COMPARE(about.homeserverState(), QStringLiteral("ready"));

        Q_EMIT client.loggedOut();
        QCOMPARE(about.accountDataState(), QStringLiteral("idle"));
        QVERIFY(about.accountDataEntries().isEmpty());
        QCOMPARE(about.homeserverState(), QStringLiteral("idle"));
        QVERIFY(about.homeserver().isEmpty());
    }

    // The confirmation is the only place the consequences are stated, so what
    // it promises must be there, and what Reset must never touch must be named.
    void theResetConfirmationSaysWhatIsAndIsNotCleared()
    {
        const QString qml = readSource(QStringLiteral("qml/AboutToolsSection.qml"));
        QVERIFY(!qml.isEmpty());
        for (const char *needle : {"event cache", "media cache", "search index",
                                   "does not touch your encryption keys",
                                   "your sign-in"})
            QVERIFY2(qml.contains(QLatin1String(needle)), needle);
    }

    // Reset must stay a cache action: the controller may call only the three
    // clears, never anything that names the crypto store or the credentials.
    void resetNeverReachesForTheCryptoStoreOrCredentials()
    {
        const QString cpp = readSource(QStringLiteral("src/app/AboutController.cpp"));
        QVERIFY(!cpp.isEmpty());
        for (const char *banned : {"resetCrypto", "CryptoStore", "logout(",
                                   "removeAccount", "deleteStore", "Credential"})
            QVERIFY2(!cpp.contains(QLatin1String(banned)), banned);
    }

    // The browser must never be handed a secret: the redaction is in Rust, and
    // the QML must not ask for anything but the listing.
    void theDeveloperPanelsAreReadOnly()
    {
        const QString qml =
            readSource(QStringLiteral("qml/DeveloperToolsSection.qml"));
        QVERIFY(!qml.isEmpty());
        QVERIFY(!qml.contains(QLatin1String("setAccountData")));
        QVERIFY(!qml.contains(QLatin1String("TextArea")));
        QVERIFY(qml.contains(QLatin1String("readOnly: true")));
        const QString rust = readSource(QStringLiteral("rust/src/aboutinfo.rs"));
        for (const char *prefix : {"m.secret_storage.", "m.cross_signing.",
                                   "m.megolm_backup."})
            QVERIFY2(rust.contains(QLatin1String(prefix)), prefix);
    }
};

QTEST_MAIN(AboutControllerTest)
#include "AboutControllerTest.moc"
