// "Playing X" game activity (Labs). Detection runs against FIXTURE
// directories standing in for /proc and a Steam library; the publish half
// runs through the real PresenceManager against a fake client that records
// the status text. Nothing here reads the real process table, Steam library,
// keyring or network. Live behaviour (a real game, a real homeserver, other
// clients rendering the text) is NOT covered.

#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTranslator>
#include <QTemporaryDir>
#include <QtTest>

#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "presence/GameActivity.h"
#include "presence/GameDetection.h"
#include "presence/PresenceManager.h"

using namespace gamedetect;

namespace {

class RecordingClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;
    bool supportsPresence() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    void requestPresence(const QStringList &, quint64) override {}
    void publishPresence(int state) override { published.append({ state, QString() }); }
    void publishPresence(int state, const QString &msg) override
    {
        published.append({ state, msg });
    }
    QList<QPair<int, QString>> published;
};

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    f.write(data);
}

QByteArray acf(const QString &id, const QString &name, const QString &dir)
{
    return QStringLiteral("\"AppState\"\n{\n\t\"appid\"\t\t\"%1\"\n\t\"Universe\"\t\t\"1\"\n"
                          "\t\"name\"\t\t\"%2\"\n\t\"installdir\"\t\t\"%3\"\n}\n")
        .arg(id, name, dir).toUtf8();
}

// /proc/<pid>/cmdline is NUL separated.
void fakeProc(const QString &root, int pid, const QStringList &argv)
{
    QByteArray raw;
    for (const QString &a : argv)
        raw += a.toUtf8() + '\0';
    writeFile(root + QStringLiteral("/%1/cmdline").arg(pid), raw);
}

// A syncing presence client with NO user status (one would outrank the game,
// as on Discord), Labs on, and the poller on a
// fast clock watching `proc`. Construction ends with the Portal 2 game
// published (one confirming scan) when `proc` has it running.
struct Fixture {
    RecordingClient client;
    SettingsManager settings;
    PresenceManager presence;
    GameActivity activity;

    Fixture(const QString &proc, const QString &steam, int pollMs = 20, bool inlineScan = true)
    {
        settings.setSharePresence(true);
        settings.setGameActivityHidden({});
        settings.setGameActivityEnabled(false);
        presence.setSettings(&settings);
        presence.setClient(&client);
        presence.setMinPublishGapForTest(0);
        presence.clearOwnStatus();
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
        activity.setProcRootForTest(proc);
        activity.setSteamRootsForTest({ steam });
        activity.setPollIntervalForTest(pollMs);
        activity.setSynchronousForTest(inlineScan);
        activity.setConfirmScansForTest(1);
        activity.setPresence(&presence);
        activity.setSettings(&settings);
        settings.setGameActivityEnabled(true);
    }
};

}   // namespace

class GameActivityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("game-activity-test"));
    }

    void appManifestAndLibraryFoldersParse();
    void steamCatalogReadsEveryLibraryAndSkipsRedistributables();
    void procScanFindsSteamReaperAndWineExecutables();
    void steamAppEnvironmentAndInstallPathAreFallbacks();
    void unknownSteamIdIsDroppedNotShown();
    void userEntriesAndHideList();
    void statusText();
    void steamToolsAreNeverTheGame();
    void foregroundWindowOnlyConfirmsAKnownGame();
    void procScanCapIsInPidOrder();
    void oldFormatLibraryFoldersAreParsed();
    void theLibraryIsCheckedOncePerScan();
    void gameListsAreCleanedAndBounded();
    void qmlCopyIsPlainTextAndHonestAboutPlatforms();

    void labsIsOffByDefault();
    void labsOffMeansNoTimerNoScanNoPublish();
    void gameIsPublishedAfterTwoScansAndTheLineComesBack();
    void aStatusTheUserTypedOutranksTheGame();
    void theGameLineIsInTheSendersLanguage();
    void hidingTheCurrentGameClearsItAtOnce();
    void sharingOffTakesTheGameBackAndStopsScanning();
    void gameNeverRidesAnOfflinePublish();
    void aGameChangeIsNotDroppedByTheDuplicateStateWindow();
    void quitTakesTheGameBackAndStopsPolling();
    void logoutStopsThePollerAndForgetsTheGame();
    void anOwnStatusSetWhilePlayingTakesTheLine();
    void theKeepAliveCarriesTheGame();
    void aRateLimitedRetryResendsTheGame();
    void theRealThreadPoolPathPublishes();

private:
    // A fixture world: proc root, one Steam root with a second library.
    struct World {
        QTemporaryDir dir;
        QString proc() const { return dir.path() + QStringLiteral("/proc"); }
        QString steam() const { return dir.path() + QStringLiteral("/steam"); }
        QString lib2() const { return dir.path() + QStringLiteral("/games/lib2"); }
        void build()
        {
            writeFile(steam() + QStringLiteral("/steamapps/libraryfolders.vdf"),
                      QStringLiteral("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"%1\"\n\t}\n}\n")
                          .arg(lib2()).toUtf8());
            writeFile(steam() + QStringLiteral("/steamapps/appmanifest_228980.acf"),
                      acf(QStringLiteral("228980"), QStringLiteral("Steamworks Common Redistributables"),
                          QStringLiteral("Steamworks Shared")));
            writeFile(lib2() + QStringLiteral("/steamapps/appmanifest_1145350.acf"),
                      acf(QStringLiteral("1145350"), QStringLiteral("Hades II"),
                          QStringLiteral("Hades II")));
            writeFile(steam() + QStringLiteral("/steamapps/appmanifest_620.acf"),
                      acf(QStringLiteral("620"), QStringLiteral("Portal 2"),
                          QStringLiteral("Portal 2")));
        }
    };

    QTemporaryDir m_configHome;
};

void GameActivityTest::appManifestAndLibraryFoldersParse()
{
    const SteamApp app = parseAppManifest(acf(QStringLiteral("1145350"),
                                              QStringLiteral("Hades II"),
                                              QStringLiteral("Hades II")));
    QCOMPARE(app.appId, QStringLiteral("1145350"));
    QCOMPARE(app.name, QStringLiteral("Hades II"));
    QCOMPARE(app.installDir, QStringLiteral("Hades II"));
    // No name = not usable.
    QVERIFY(parseAppManifest("\"AppState\"\n{\n\t\"appid\"\t\"5\"\n}\n").appId.isEmpty());
    // An escaped quote and backslash survive.
    QCOMPARE(parseAppManifest(acf(QStringLiteral("7"), QStringLiteral("A \\\"B\\\" C"),
                                  QStringLiteral("x"))).name,
             QStringLiteral("A \"B\" C"));

    const QStringList libs = parseLibraryFolders(
        "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"/home/u/.local/share/Steam\"\n"
        "\t\t\"label\"\t\t\"\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"/mnt/games/Steam Library\"\n\t}\n}\n");
    QCOMPARE(libs, (QStringList{ QStringLiteral("/home/u/.local/share/Steam"),
                                 QStringLiteral("/mnt/games/Steam Library") }));
}

void GameActivityTest::steamCatalogReadsEveryLibraryAndSkipsRedistributables()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    SteamCatalog cat({ w.steam() });
    QCOMPARE(cat.nameForAppId(QStringLiteral("620")), QStringLiteral("Portal 2"));
    // The second library comes from libraryfolders.vdf.
    QCOMPARE(cat.nameForAppId(QStringLiteral("1145350")), QStringLiteral("Hades II"));
    QCOMPARE(cat.nameForAppId(QStringLiteral("228980")), QString());
    QCOMPARE(cat.nameForAppId(QStringLiteral("0")), QString());
    QCOMPARE(cat.appCount(), 2);
    QCOMPARE(cat.nameForPath(QStringLiteral("/x/steamapps/common/Hades II/Ship/Hades2.exe")),
             QStringLiteral("Hades II"));
    QCOMPARE(cat.nameForPath(QStringLiteral("D:\\SteamLibrary\\steamapps\\common\\Portal 2\\portal2.exe")),
             QStringLiteral("Portal 2"));
    QCOMPARE(cat.nameForPath(QStringLiteral("/usr/bin/bash")), QString());

    // A game installed mid-session shows without a restart.
    QTest::qSleep(30);
    writeFile(w.steam() + QStringLiteral("/steamapps/appmanifest_9.acf"),
              acf(QStringLiteral("9"), QStringLiteral("Late Game"), QStringLiteral("Late")));
    QCOMPARE(cat.nameForAppId(QStringLiteral("9")), QStringLiteral("Late Game"));
}

void GameActivityTest::procScanFindsSteamReaperAndWineExecutables()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("/home/u/.steam/ubuntu12_32/reaper"),
                              QStringLiteral("SteamLaunch"), QStringLiteral("AppId=1145350"),
                              QStringLiteral("--"), QStringLiteral("proton") });
    fakeProc(w.proc(), 150, { QStringLiteral("wine64"),
                              QStringLiteral("Z:\\games\\Valheim.exe") });
    fakeProc(w.proc(), 300, { QStringLiteral("/usr/bin/bash") });
    // Not a process directory, and an unreadable/empty one.
    fakeProc(w.proc(), 400, {});
    QDir().mkpath(w.proc() + QStringLiteral("/self"));
    writeFile(w.proc() + QStringLiteral("/self/cmdline"), QByteArray("Factorio\0", 9));

    const auto procs = scanProcRoot(w.proc());
    QCOMPARE(procs.size(), 3);   // 150, 200, 300; "self" skipped, 400 empty
    QCOMPARE(procs.first().pid, qint64(150));

    SteamCatalog cat({ w.steam() });
    const QStringList names = detectGames(procs, cat, Catalog::builtIn());
    // pid order: wine's Valheim first, then the Steam reaper.
    QCOMPARE(names, (QStringList{ QStringLiteral("Valheim"), QStringLiteral("Hades II") }));
}

void GameActivityTest::steamAppEnvironmentAndInstallPathAreFallbacks()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    SteamCatalog cat({ w.steam() });
    ProcessInfo viaEnv;
    viaEnv.pid = 1;
    viaEnv.args = { QStringLiteral("game.exe") };
    viaEnv.steamAppIdEnv = QStringLiteral("620");
    ProcessInfo viaPath;
    viaPath.pid = 2;
    viaPath.exePath = QStringLiteral("/mnt/x/steamapps/common/Hades II/Hades2");
    viaPath.args = { viaPath.exePath };
    QCOMPARE(detectGames({ viaEnv, viaPath }, cat, Catalog{}),
             (QStringList{ QStringLiteral("Portal 2"), QStringLiteral("Hades II") }));

    // The env fixture is real: a process under steamapps/common has its
    // environ read, others do not.
    writeFile(w.proc() + QStringLiteral("/77/cmdline"),
              QByteArray("/x/steamapps/common/Foo/foo\0", 28));
    writeFile(w.proc() + QStringLiteral("/77/environ"),
              QByteArray("HOME=/h\0SteamAppId=620\0", 23));
    writeFile(w.proc() + QStringLiteral("/78/cmdline"), QByteArray("/usr/bin/vim\0", 13));
    writeFile(w.proc() + QStringLiteral("/78/environ"), QByteArray("SteamAppId=620\0", 15));
    const auto procs = scanProcRoot(w.proc());
    QCOMPARE(procs.size(), 2);
    QCOMPARE(procs[0].steamAppIdEnv, QStringLiteral("620"));
    QVERIFY(procs[1].steamAppIdEnv.isEmpty());   // never read for a plain process
}

void GameActivityTest::unknownSteamIdIsDroppedNotShown()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    SteamCatalog cat({ w.steam() });
    ProcessInfo reaper;
    reaper.pid = 1;
    reaper.args = { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                    QStringLiteral("AppId=3000000000"), QStringLiteral("--") };
    // A non-Steam shortcut has a huge id with no manifest: nothing is
    // published, and certainly not the number.
    QVERIFY(detectGames({ reaper }, cat, Catalog::builtIn()).isEmpty());
}

void GameActivityTest::userEntriesAndHideList()
{
    Catalog c = Catalog::builtIn();
    c.addUserEntries({ QStringLiteral("MyGame.x86_64=My Game"), QStringLiteral("bare"),
                       QStringLiteral("=nothing"), QStringLiteral("noname=") });
    ProcessInfo p;
    p.pid = 1;
    p.args = { QStringLiteral("/opt/MyGame/mygame.X86_64") };
    ProcessInfo q;
    q.pid = 2;
    q.args = { QStringLiteral("/bin/bare") };
    SteamCatalog cat({});
    QCOMPARE(detectGames({ p, q }, cat, c),
             (QStringList{ QStringLiteral("My Game"), QStringLiteral("bare") }));
    QVERIFY(!c.exeToName.contains(QString()));
    QVERIFY(!c.exeToName.contains(QStringLiteral("noname")));

    QVERIFY(isHidden(QStringLiteral(" my game "), { QStringLiteral("My Game") }));
    QVERIFY(!isHidden(QStringLiteral("Hades II"), { QStringLiteral("My Game") }));
    QVERIFY(!isHidden(QStringLiteral("Hades II"), {}));
}

void GameActivityTest::statusText()
{
    QCOMPARE(statusTextFor(QStringLiteral("Hades II")), QStringLiteral("Playing Hades II"));
    QCOMPARE(statusTextFor(QString()), QString());
    QVERIFY(statusTextFor(QString(500, QLatin1Char('x'))).size() <= 140);
}

// --- gating and publishing -------------------------------------------------

void GameActivityTest::labsOffMeansNoTimerNoScanNoPublish()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=620") });
    RecordingClient client;
    SettingsManager settings;
    settings.setSharePresence(true);
    settings.setGameActivityEnabled(false);
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.clearOwnStatus();   // shared store; an own status would outrank the game
    Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    client.published.clear();

    GameActivity activity;
    activity.setProcRootForTest(w.proc());
    activity.setSteamRootsForTest({ w.steam() });
    activity.setPollIntervalForTest(20);
    activity.setSynchronousForTest(true);
    activity.setConfirmScansForTest(1);
    activity.setPresence(&presence);
    activity.setSettings(&settings);
    QVERIFY(!activity.active());
    QVERIFY(!activity.polling());
    QTest::qWait(150);
    QCOMPARE(activity.scansStarted(), 0);
    QCOMPARE(activity.currentGame(), QString());
    for (const auto &p : client.published)
        QVERIFY2(!p.second.contains(QStringLiteral("Playing")), "published while Labs is off");
    QCOMPARE(presence.gameActivity(), QString());
}

void GameActivityTest::gameIsPublishedAfterTwoScansAndTheLineComesBack()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    RecordingClient client;
    SettingsManager settings;
    settings.setSharePresence(true);
    settings.setGameActivityHidden({});
    settings.setGameActivityEnabled(false);
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.clearOwnStatus();   // shared store; an own status would outrank the game
    presence.setMinPublishGapForTest(0);
    Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    QCOMPARE(client.published.last().second, QString());

    GameActivity activity;
    activity.setProcRootForTest(w.proc());
    activity.setSteamRootsForTest({ w.steam() });
    activity.setPollIntervalForTest(30);
    activity.setSynchronousForTest(true);
    activity.setPresence(&presence);
    activity.setSettings(&settings);
    QSignalSpy changed(&activity, &GameActivity::currentGameChanged);

    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=1145350") });
    settings.setGameActivityEnabled(true);
    // First scan only nominates the game (debounce): nothing published yet.
    QCOMPARE(activity.scansStarted(), 1);
    QCOMPARE(activity.currentGame(), QString());
    QCOMPARE(client.published.last().second, QString());
    QTRY_COMPARE(activity.currentGame(), QStringLiteral("Hades II"));
    QCOMPARE(client.published.last().second, QStringLiteral("Playing Hades II"));
    QVERIFY(client.published.last().first == 0 || client.published.last().first == 1);
    // The local read shows the same thing every other client will.
    QCOMPARE(presence.statusMessageFor(QStringLiteral("@me:example.org")),
             QStringLiteral("Playing Hades II"));
    // The game is not the user's status: nothing was stored as one.
    QCOMPARE(presence.ownStatusText(), QString());

    // The game exits: after two scans the line is published empty again.
    QDir(w.proc() + QStringLiteral("/200")).removeRecursively();
    QTRY_COMPARE(activity.currentGame(), QString());
    QCOMPARE(client.published.last().second, QString());
    QCOMPARE(changed.size(), 2);
}

void GameActivityTest::aStatusTheUserTypedOutranksTheGame()
{
    // Discord: "Custom statuses override other activity statuses". A game
    // detected while the user has a status of their own changes nothing on
    // the wire and costs no PUT; clearing the status hands the line to the
    // game, and setting one takes it back.
    RecordingClient client;
    SettingsManager settings;
    settings.setSharePresence(true);
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.setMinPublishGapForTest(0);
    presence.setOwnStatus(QStringLiteral("☕"), QStringLiteral("Busy"), 0);
    const auto cleanup = qScopeGuard([&presence] { presence.clearOwnStatus(); });
    Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    QCOMPARE(client.published.last().second, QStringLiteral("☕ Busy"));
    const int sent = client.published.size();

    presence.setGameActivity(QStringLiteral("Hades II"));
    QCOMPARE(presence.effectiveStatusText(), QStringLiteral("☕ Busy"));
    QCOMPARE(presence.statusMessageFor(QStringLiteral("@me:example.org")),
             QStringLiteral("☕ Busy"));
    QCOMPARE(client.published.size(), sent);   // no PUT for a hidden change

    presence.clearOwnStatus();
    QCOMPARE(client.published.last().second, QStringLiteral("Playing Hades II"));
    presence.setOwnStatus(QStringLiteral("🍕"), QStringLiteral("Lunch"), 0);
    QCOMPARE(client.published.last().second, QStringLiteral("🍕 Lunch"));
    const int beforeExit = client.published.size();
    presence.setGameActivity(QString());   // the game exits behind the status
    QCOMPARE(client.published.size(), beforeExit);
    QCOMPARE(presence.effectiveStatusText(), QStringLiteral("🍕 Lunch"));
}

namespace {
// Stands in for a loaded catalogue: answers only the game's line.
class GameLineTranslator : public QTranslator
{
public:
    QString translate(const char *context, const char *sourceText, const char *,
                      int) const override
    {
        if (qstrcmp(context, "GameActivity") == 0 && qstrcmp(sourceText, "Playing %1") == 0)
            return QStringLiteral("Žaidžia %1");
        return {};
    }
    bool isEmpty() const override { return false; }
};
}   // namespace

void GameActivityTest::theGameLineIsInTheSendersLanguage()
{
    // A status line is the user's own words: it is written in the app's
    // language (Matrix cannot localise it per reader), through the
    // translator like every other string.
    GameLineTranslator lt;
    QVERIFY(QCoreApplication::installTranslator(&lt));
    const auto cleanup = qScopeGuard([&lt] { QCoreApplication::removeTranslator(&lt); });
    QCOMPARE(statusTextFor(QStringLiteral("Hades II")), QStringLiteral("Žaidžia Hades II"));
}

void GameActivityTest::hidingTheCurrentGameClearsItAtOnce()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=620") });
    RecordingClient client;
    SettingsManager settings;
    settings.setSharePresence(true);
    settings.setGameActivityHidden({});
    settings.setGameActivityEnabled(false);
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.clearOwnStatus();   // shared store; an own status would outrank the game
    presence.setMinPublishGapForTest(0);
    Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    GameActivity activity;
    activity.setProcRootForTest(w.proc());
    activity.setSteamRootsForTest({ w.steam() });
    activity.setPollIntervalForTest(20);
    activity.setSynchronousForTest(true);
    activity.setConfirmScansForTest(1);
    activity.setPresence(&presence);
    activity.setSettings(&settings);
    settings.setGameActivityEnabled(true);
    QTRY_COMPARE(activity.currentGame(), QStringLiteral("Portal 2"));
    QCOMPARE(client.published.last().second, QStringLiteral("Playing Portal 2"));

    settings.setGameActivityHidden({ QStringLiteral("portal 2") });
    QCOMPARE(activity.currentGame(), QString());   // no scan wait
    QCOMPARE(client.published.last().second, QString());
    // And it stays hidden across later scans.
    const int scans = activity.scansStarted();
    QTRY_VERIFY(activity.scansStarted() >= scans + 3);
    QCOMPARE(activity.currentGame(), QString());
    QVERIFY(client.published.last().second.isEmpty());
}

void GameActivityTest::sharingOffTakesTheGameBackAndStopsScanning()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=620") });
    RecordingClient client;
    SettingsManager settings;
    settings.setSharePresence(true);
    settings.setGameActivityHidden({});
    settings.setGameActivityEnabled(false);
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.clearOwnStatus();   // shared store; an own status would outrank the game
    presence.setMinPublishGapForTest(0);
    Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    GameActivity activity;
    activity.setProcRootForTest(w.proc());
    activity.setSteamRootsForTest({ w.steam() });
    activity.setPollIntervalForTest(20);
    activity.setSynchronousForTest(true);
    activity.setConfirmScansForTest(1);
    activity.setPresence(&presence);
    activity.setSettings(&settings);
    settings.setGameActivityEnabled(true);
    QTRY_COMPARE(activity.currentGame(), QStringLiteral("Portal 2"));

    settings.setSharePresence(false);
    QCOMPARE(activity.currentGame(), QString());
    QVERIFY(!activity.polling());
    // The only thing the server hears is the final offline, with no text.
    QCOMPARE(client.published.last().first, 2);
    QVERIFY(client.published.last().second.isEmpty());
    const int scans = activity.scansStarted();
    const int sent = client.published.size();
    QTest::qWait(120);
    QCOMPARE(activity.scansStarted(), scans);
    QCOMPARE(client.published.size(), sent);
}

void GameActivityTest::gameNeverRidesAnOfflinePublish()
{
    RecordingClient client;
    SettingsManager settings;
    settings.setSharePresence(true);
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.clearOwnStatus();   // shared store; an own status would outrank the game
    presence.setMinPublishGapForTest(0);
    // Not syncing yet: a game is remembered, nothing is sent.
    presence.setGameActivity(QStringLiteral("Portal 2"));
    QVERIFY(client.published.isEmpty());
    // The first publish of the session then carries it.
    Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    QCOMPARE(client.published.last().second, QStringLiteral("Playing Portal 2"));
    // Sharing off: the final offline carries no text and later changes send
    // nothing.
    settings.setSharePresence(false);
    QCOMPARE(client.published.last().first, 2);
    QVERIFY(client.published.last().second.isEmpty());
    const int sent = client.published.size();
    presence.setGameActivity(QStringLiteral("Hades II"));
    presence.setGameActivity(QString());
    QCOMPARE(client.published.size(), sent);
    // A control character in a name cannot reach the status.
    presence.setGameActivity(QStringLiteral("Bad\nName\x01"));
    QCOMPARE(presence.effectiveStatusText(), QStringLiteral("Playing Bad Name"));
}

void GameActivityTest::aGameChangeIsNotDroppedByTheDuplicateStateWindow()
{
    RecordingClient client;
    SettingsManager settings;
    settings.setSharePresence(true);
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);   // the real 10 s window stays in force
    presence.clearOwnStatus();
    Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    const int before = client.published.size();
    QVERIFY(before >= 1);
    // The state did not change, only the text: still one PUT right now.
    presence.setGameActivity(QStringLiteral("Hades II"));
    QCOMPARE(client.published.size(), before + 1);
    QCOMPARE(client.published.last().second, QStringLiteral("Playing Hades II"));
    presence.setGameActivity(QString());
    QCOMPARE(client.published.size(), before + 2);
    QVERIFY(client.published.last().second.isEmpty());
    // Setting the same value twice sends nothing.
    presence.setGameActivity(QString());
    QCOMPARE(client.published.size(), before + 2);
}


// --- review round 1 ---------------------------------------------------------

namespace {
QString readSource(const QString &rel)
{
    QFile f(rel);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}
}   // namespace

void GameActivityTest::steamToolsAreNeverTheGame()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    const QString apps = w.steam() + QStringLiteral("/steamapps");
    writeFile(apps + QStringLiteral("/appmanifest_1493710.acf"),
              acf(QStringLiteral("1493710"), QStringLiteral("Proton Experimental"),
                  QStringLiteral("Proton - Experimental")));
    // A Proton release nobody listed by id: the family is matched by name.
    writeFile(apps + QStringLiteral("/appmanifest_2805730.acf"),
              acf(QStringLiteral("2805730"), QStringLiteral("Proton 9.0 (Beta)"),
                  QStringLiteral("Proton 9.0")));
    writeFile(apps + QStringLiteral("/appmanifest_1628350.acf"),
              acf(QStringLiteral("1628350"), QStringLiteral("Steam Linux Runtime 3.0 (sniper)"),
                  QStringLiteral("SteamLinuxRuntime_sniper")));
    // An unlisted runtime id, matched by installdir alone.
    writeFile(apps + QStringLiteral("/appmanifest_4000001.acf"),
              acf(QStringLiteral("4000001"), QStringLiteral("Runtime Thing"),
                  QStringLiteral("SteamLinuxRuntime_future")));
    // A real game whose name merely begins with the word must survive.
    writeFile(apps + QStringLiteral("/appmanifest_5000001.acf"),
              acf(QStringLiteral("5000001"), QStringLiteral("Proton Pulse"),
                  QStringLiteral("Proton Pulse")));

    SteamCatalog cat({ w.steam() });
    for (const char *id : { "1493710", "2805730", "1628350", "4000001" })
        QCOMPARE(cat.nameForAppId(QString::fromLatin1(id)), QString());
    QCOMPARE(cat.nameForAppId(QStringLiteral("5000001")), QStringLiteral("Proton Pulse"));

    // /proc fixture with REAL exe symlinks. A non-Steam title under Proton:
    // the processes that run are Proton's and the runtime's.
    const QString common = w.steam() + QStringLiteral("/steamapps/common");
    auto proc = [&](int pid, const QString &exe, const QStringList &argv, const QByteArray &env = {}) {
        fakeProc(w.proc(), pid, argv);
        QVERIFY(QFile::link(exe, w.proc() + QStringLiteral("/%1/exe").arg(pid)));
        if (!env.isEmpty())
            writeFile(w.proc() + QStringLiteral("/%1/environ").arg(pid), env);
    };
    proc(10, common + QStringLiteral("/Proton - Experimental/files/bin/wine64-preloader"),
         { QStringLiteral("wine64"), QStringLiteral("Z:\\home\\u\\games\\Unknown.exe") });
    proc(11, common + QStringLiteral("/SteamLinuxRuntime_sniper/pressure-vessel/bin/bwrap"),
         { QStringLiteral("bwrap") });
    proc(12, QStringLiteral("/usr/bin/python3"),
         { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
           QStringLiteral("AppId=1493710"), QStringLiteral("--") });
    proc(13, common + QStringLiteral("/Proton 9.0/proton"),
         { QStringLiteral("python3"), common + QStringLiteral("/Proton 9.0/proton"),
           QStringLiteral("waitforexitandrun") },
         "SteamAppId=2805730\0");
    // The same wrapper running a game that IS known by its executable name.
    proc(14, common + QStringLiteral("/Proton - Experimental/files/bin/wine64-preloader"),
         { QStringLiteral("wine64"), QStringLiteral("Z:\\home\\u\\games\\Valheim.exe") });

    const auto procs = scanProcRoot(w.proc());
    QCOMPARE(procs.size(), 5);
    QCOMPARE(detectGames(procs, cat, Catalog::builtIn()),
             (QStringList{ QStringLiteral("Valheim") }));
}

void GameActivityTest::foregroundWindowOnlyConfirmsAKnownGame()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    SteamCatalog cat({ w.steam() });
    const Catalog builtIn = Catalog::builtIn();
    // Fullscreen office suites, browsers and players are not games, however
    // the window looks.
    for (const char *exe : { "C:\\Program Files\\Microsoft Office\\WINWORD.EXE",
                             "C:\\Program Files\\Google\\Chrome\\chrome.exe",
                             "C:\\Tools\\Foo Game.exe", "" })
        QCOMPARE(knownGameForExePath(QString::fromLatin1(exe), cat, builtIn), QString());
    QCOMPARE(knownGameForExePath(
                 QStringLiteral("D:\\SteamLibrary\\steamapps\\common\\Portal 2\\portal2.exe"),
                 cat, builtIn),
             QStringLiteral("Portal 2"));
    QCOMPARE(knownGameForExePath(QStringLiteral("C:\\G\\Valheim.exe"), cat, builtIn),
             QStringLiteral("Valheim"));
    // It may only reorder what was already detected.
    const QStringList detected = { QStringLiteral("A"), QStringLiteral("B") };
    QCOMPARE(preferForeground(detected, QStringLiteral("B")),
             (QStringList{ QStringLiteral("B"), QStringLiteral("A") }));
    QCOMPARE(preferForeground(detected, QStringLiteral("C")), detected);
    QCOMPARE(preferForeground({}, QStringLiteral("C")), QStringList());
    // And the Windows call site has no other way to add a name.
    const QString src = readSource(QStringLiteral("src/presence/GameActivity.cpp"));
    QVERIFY2(!src.isEmpty(), "run from the source directory");
    QVERIFY(!src.contains(QLatin1String("nameFromFullscreenExe")));
    QVERIFY(!src.contains(QLatin1String("names <<")));
    QVERIFY(src.contains(QLatin1String("preferForeground")));
}

void GameActivityTest::procScanCapIsInPidOrder()
{
    World w;
    QVERIFY(w.dir.isValid());
    for (int pid : { 100, 11, 9, 10 })
        fakeProc(w.proc(), pid, { QStringLiteral("/bin/p%1").arg(pid) });
    // Lexical order would be 10, 100, 11, 9 and drop pid 9.
    const auto procs = scanProcRoot(w.proc(), 3);
    QCOMPARE(procs.size(), 3);
    QCOMPARE(procs[0].pid, qint64(9));
    QCOMPARE(procs[1].pid, qint64(10));
    QCOMPARE(procs[2].pid, qint64(11));
}

void GameActivityTest::oldFormatLibraryFoldersAreParsed()
{
    const QStringList libs = parseLibraryFolders(
        "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1580000000\"\n"
        "\t\"ContentStatsID\"\t\t\"-123456\"\n"
        "\t\"1\"\t\t\"/mnt/games/Steam Library\"\n"
        "\t\"2\"\t\t\"D:\\\\SteamLibrary\"\n}\n");
    QCOMPARE(libs, (QStringList{ QStringLiteral("/mnt/games/Steam Library"),
                                 QStringLiteral("D:\\SteamLibrary") }));
}

void GameActivityTest::theLibraryIsCheckedOncePerScan()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    SteamCatalog cat({ w.steam() });
    QList<ProcessInfo> procs;
    for (int i = 0; i < 50; ++i) {
        ProcessInfo p;
        p.pid = i + 1;
        p.exePath = QStringLiteral("/x/steamapps/common/Portal 2/p%1").arg(i);
        procs.append(p);
    }
    QCOMPARE(detectGames(procs, cat, Catalog{}), (QStringList{ QStringLiteral("Portal 2") }));
    // Once for the pass, not once per matching process.
    QCOMPARE(cat.checksForTest(), 1);
    // Outside a pass every query still looks (mid-session installs).
    cat.nameForAppId(QStringLiteral("620"));
    cat.nameForAppId(QStringLiteral("620"));
    QCOMPARE(cat.checksForTest(), 3);
}

void GameActivityTest::gameListsAreCleanedAndBounded()
{
    SettingsManager settings;
    settings.setGameActivityHidden({ QStringLiteral("  Hades   II "), QStringLiteral("hades ii"),
                                     QString(), QStringLiteral("   "), QStringLiteral("Portal 2") });
    QCOMPARE(settings.gameActivityHidden(),
             (QStringList{ QStringLiteral("Hades II"), QStringLiteral("Portal 2") }));
    QStringList many;
    for (int i = 0; i < 500; ++i)
        many << QStringLiteral("exe%1=Name %1").arg(i);
    many << QString(300, QLatin1Char('z'));
    settings.setGameActivityCustom(many);
    QCOMPARE(settings.gameActivityCustom().size(), 200);
    settings.setGameActivityCustom({ QString(300, QLatin1Char('z')) });
    QCOMPARE(settings.gameActivityCustom().first().size(), 120);
    settings.setGameActivityCustom({});
    settings.setGameActivityHidden({});
}

void GameActivityTest::qmlCopyIsPlainTextAndHonestAboutPlatforms()
{
    const QString qml = readSource(QStringLiteral("qml/GameActivitySettings.qml"));
    QVERIFY2(!qml.isEmpty(), "run from the source directory");
    // Every Label that shows static copy is PlainText, in particular the one
    // containing a literal "<game>" which Qt would otherwise parse as markup.
    const QStringList blocks = qml.split(QStringLiteral("Label {"));
    int labels = 0;
    for (int i = 1; i < blocks.size(); ++i) {
        const QString b = blocks[i].left(blocks[i].indexOf(QLatin1String("\n    }\n")) >= 0
                                             ? blocks[i].indexOf(QLatin1String("\n    }\n"))
                                             : blocks[i].size());
        ++labels;
        QVERIFY2(b.contains(QLatin1String("textFormat: Text.PlainText")),
                 qPrintable(QStringLiteral("Label #%1 is not PlainText").arg(i)));
    }
    QVERIFY(labels >= 8);
    // macOS has no detection and says so; no claim that other builds "detect
    // games fully".
    QVERIFY(qml.contains(QLatin1String("detectionSupported")));
    QVERIFY(!qml.contains(QLatin1String("detect games fully")));
}

void GameActivityTest::labsIsOffByDefault()
{
    // Sets NOTHING: the defaults of a store no case has left a value in
    // (the other cases that write settings put them back).
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=620") });
    RecordingClient client;
    SettingsManager settings;
    QVERIFY(!settings.gameActivityEnabled());
    QVERIFY(settings.gameActivityHidden().isEmpty());
    QVERIFY(settings.gameActivityCustom().isEmpty());
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.clearOwnStatus();   // shared store; an own status would outrank the game
    Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    GameActivity activity;
    activity.setProcRootForTest(w.proc());
    activity.setSteamRootsForTest({ w.steam() });
    activity.setPollIntervalForTest(20);
    activity.setPresence(&presence);
    activity.setSettings(&settings);
    QVERIFY(!activity.active());
    QVERIFY(!activity.polling());
    QTest::qWait(120);
    QCOMPARE(activity.scansStarted(), 0);
    QCOMPARE(activity.currentGame(), QString());
}


void GameActivityTest::quitTakesTheGameBackAndStopsPolling()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=620") });
    Fixture f(w.proc(), w.steam());
    QTRY_COMPARE(f.activity.currentGame(), QStringLiteral("Portal 2"));
    QCOMPARE(f.client.published.last().second, QStringLiteral("Playing Portal 2"));

    f.activity.shutdown();   // AppController::prepareForShutdown
    QCOMPARE(f.activity.currentGame(), QString());
    QVERIFY(!f.activity.polling());
    // The game is withdrawn while the client can still send: the server's
    // last word is the empty line, not the game.
    QCOMPARE(f.client.published.last().second, QString());
    const int scans = f.activity.scansStarted();
    const int sent = f.client.published.size();
    QTest::qWait(120);
    QCOMPARE(f.activity.scansStarted(), scans);
    QCOMPARE(f.client.published.size(), sent);
    // And a later settings change cannot restart it.
    f.settings.setGameActivityHidden({ QStringLiteral("x") });
    QVERIFY(!f.activity.polling());
    // The wiring exists in the controller.
    QVERIFY(readSource(QStringLiteral("src/app/AppController.cpp"))
                .contains(QLatin1String("m_gameActivity->shutdown()")));
}

void GameActivityTest::logoutStopsThePollerAndForgetsTheGame()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=620") });
    Fixture f(w.proc(), w.steam());
    QTRY_COMPARE(f.activity.currentGame(), QStringLiteral("Portal 2"));

    Q_EMIT f.client.loggedOut();
    QCOMPARE(f.activity.currentGame(), QString());
    QCOMPARE(f.presence.gameActivity(), QString());
    QVERIFY(!f.activity.polling());
    const int scans = f.activity.scansStarted();
    QTest::qWait(120);
    QCOMPARE(f.activity.scansStarted(), scans);   // not scanning while logged out

    // The next session starts the poller again, and only after it is live.
    Q_EMIT f.client.connectionStateChanged(MatrixClient::Syncing);
    QVERIFY(f.activity.polling());
    QTRY_COMPARE(f.activity.currentGame(), QStringLiteral("Portal 2"));
}

void GameActivityTest::anOwnStatusSetWhilePlayingTakesTheLine()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=620") });
    Fixture f(w.proc(), w.steam());
    QTRY_COMPARE(f.activity.currentGame(), QStringLiteral("Portal 2"));

    // Setting a status of one's own takes the line at once (Discord's order);
    // clearing it hands the line back to the game still running.
    f.presence.setOwnStatus(QStringLiteral("🍕"), QStringLiteral("Lunch"), 0);
    QCOMPARE(f.client.published.last().second, QStringLiteral("🍕 Lunch"));
    QCOMPARE(f.presence.ownStatusText(), QStringLiteral("🍕 Lunch"));
    f.presence.clearOwnStatus();
    QCOMPARE(f.client.published.last().second, QStringLiteral("Playing Portal 2"));
    f.presence.setOwnStatus(QStringLiteral("🍕"), QStringLiteral("Lunch"), 0);
    const auto cleanup = qScopeGuard([&f] { f.presence.clearOwnStatus(); });

    // The game exits behind the status: the status stays, no PUT needed.
    const int sent = f.client.published.size();
    QDir(w.proc() + QStringLiteral("/200")).removeRecursively();
    QTRY_COMPARE(f.activity.currentGame(), QString());
    QCOMPARE(f.client.published.size(), sent);
    QCOMPARE(f.client.published.last().second, QStringLiteral("🍕 Lunch"));
}

void GameActivityTest::theKeepAliveCarriesTheGame()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=620") });
    Fixture f(w.proc(), w.steam());
    QTRY_COMPARE(f.activity.currentGame(), QStringLiteral("Portal 2"));
    f.presence.setPublishIntervalForTest(40);
    const int before = f.client.published.size();
    QTRY_VERIFY(f.client.published.size() >= before + 3);
    for (int i = before; i < f.client.published.size(); ++i)
        QCOMPARE(f.client.published[i].second, QStringLiteral("Playing Portal 2"));
}

void GameActivityTest::aRateLimitedRetryResendsTheGame()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=620") });
    Fixture f(w.proc(), w.steam());
    f.presence.setPublishIntervalForTest(120 * 1000);   // only the retry can publish
    QTRY_COMPARE(f.activity.currentGame(), QStringLiteral("Portal 2"));
    const int before = f.client.published.size();
    Q_EMIT f.client.presencePublishFailed(QStringLiteral("rate_limited"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(f.client.published.size() > before, 6000);
    QCOMPARE(f.client.published.last().second, QStringLiteral("Playing Portal 2"));
}

void GameActivityTest::theRealThreadPoolPathPublishes()
{
    World w;
    QVERIFY(w.dir.isValid());
    w.build();
    fakeProc(w.proc(), 200, { QStringLiteral("reaper"), QStringLiteral("SteamLaunch"),
                              QStringLiteral("AppId=620") });
    Fixture f(w.proc(), w.steam(), 30, /*inlineScan=*/false);
    QTRY_COMPARE_WITH_TIMEOUT(f.activity.currentGame(), QStringLiteral("Portal 2"), 8000);
    QCOMPARE(f.client.published.last().second, QStringLiteral("Playing Portal 2"));
    // Leaving the feature off while a scan is in flight drops its answer.
    f.settings.setGameActivityEnabled(false);
    QCOMPARE(f.activity.currentGame(), QString());
    QTest::qWait(150);
    QCOMPARE(f.activity.currentGame(), QString());
    QCOMPARE(f.client.published.last().second, QString());
}

QTEST_MAIN(GameActivityTest)
#include "GameActivityTest.moc"
