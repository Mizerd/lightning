// "Listening to" music activity (Labs). The Spotify rules run on plain data;
// the policy (settle, pause grace, priority, gating, withdrawal) runs through
// the real PresenceManager against a fake source and a client that records
// what would be published; the Windows caption source runs against a fake
// caption provider; and, where QtDBus exists, the real MPRIS source runs
// against a fake Spotify on a PRIVATE dbus-daemon started by this suite.
// Nothing here touches the user's session bus, a real Spotify, a keyring or
// the network. Live behaviour (a real Spotify, the Flatpak proxy, Windows,
// other clients rendering the line) is NOT covered.

#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#endif

#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "presence/MusicActivity.h"
#include "presence/MusicDetection.h"
#include "presence/MusicSource.h"
#include "presence/PresenceManager.h"
#ifdef HAVE_QT_DBUS
#include "presence/MprisMusicSource.h"
#endif

using musicdetect::NowPlaying;

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

// Stands in for the platform layer: what it "plays" is set by the test.
class FakeSource : public MusicSource
{
    Q_OBJECT
public:
    explicit FakeSource(int *alive) : m_alive(alive) { ++*m_alive; }
    ~FakeSource() override { --*m_alive; }
    void start() override { started = true; }
    void stop() override { started = false; }
    NowPlaying nowPlaying() const override { return np; }
    void play(const NowPlaying &next)
    {
        np = next;
        Q_EMIT nowPlayingChanged();
    }
    bool started = false;
    NowPlaying np;

private:
    int *m_alive;
};

NowPlaying track(const QString &title, const QString &artist,
                 const QString &album = QStringLiteral("Album"), bool playing = true)
{
    NowPlaying np;
    np.title = title;
    if (!artist.isEmpty())
        np.artists = { artist };
    np.album = album;
    np.trackId = QStringLiteral("/com/spotify/track/") + title.toLower().remove(QLatin1Char(' '));
    np.playing = playing;
    return np;
}

NowPlaying paused(NowPlaying np)
{
    np.playing = false;
    return np;
}

QString readSource(const QString &rel)
{
    QFile f(rel);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// A syncing presence client with no own status, Labs ON, a fake source, and
// short settle/grace. The keep-alive stays at its real 25 s unless a case
// shortens it, so every publish a case sees was caused by that case.
struct Fixture {
    RecordingClient client;
    SettingsManager settings;
    PresenceManager presence;
    MusicActivity activity;
    int alive = 0;
    QPointer<FakeSource> source;

    explicit Fixture(bool enable = true)
    {
        settings.setSharePresence(true);
        settings.setMusicActivityEnabled(false);
        presence.setSettings(&settings);
        presence.setClient(&client);
        presence.setMinPublishGapForTest(0);
        presence.clearOwnStatus();
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
        activity.setSettleForTest(60);
        activity.setPauseGraceForTest(150);
        activity.setSourceFactoryForTest([this] {
            auto s = std::make_unique<FakeSource>(&alive);
            source = s.get();
            return std::unique_ptr<MusicSource>(std::move(s));
        });
        activity.setPresence(&presence);
        activity.setSettings(&settings);
        if (enable)
            settings.setMusicActivityEnabled(true);
    }
    ~Fixture()
    {
        // The store is shared by every case in this process.
        settings.setMusicActivityEnabled(false);
        presence.clearOwnStatus();
    }
};

const QString kLine = QStringLiteral("Listening to Song A by Artist A");

// The fake client never answers on its own: answer every PUT still in
// flight, as the server would have long since.
void answerAll(PresenceManager &presence, MatrixClient &client)
{
    while (presence.publishesInFlight() > 0)
        Q_EMIT client.presencePublished();
}

}   // namespace

class MusicActivityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    // Rules (pure).
    void mprisMetadataBecomesNowPlaying();
    void advertisementsAndUntitledEntriesAreNeverShown();
    void statusLineWordingAndBounds();
    void theLineIsInTheSendersLanguage();
    void windowsCaptionsAreParsedLikeSpotifyNamesThem();
    void windowsCaptionSourcePollsOnlyWhileStarted();
    void qmlCopyIsPlainTextAndSendsNoArt();
    void aFlatpakListensOnlyWhenItWasGrantedSpotify();

    // Policy (fake source, real PresenceManager).
    void labsIsOffByDefault();
    void offMeansNoSourceAndTurningItOffWithdrawsAtOnce();
    void aTrackIsAdoptedAfterTheSettleAndRidesTheKeepAlive();
    void skippingThroughSongsAdoptsOnlyTheOneKept();
    void pauseClearsAfterTheGraceAndAQuickResumeChangesNothing();
    void spotifyQuittingClearsLikeAStop();
    void theUsersOwnStatusAndAGameBothWin();
    void aWithdrawalBehindTheOwnStatusSendsNothing();
    void sharingOffWithdrawsAndStopsTheSource();
    void quitWithdrawsWhileTheClientCanStillSend();
    void logoutStopsTheSourceAndForgetsTheTrack();
    void onlyTitleAndArtistLeaveThisMachine();
    void aTitleThatLooksLikeMarkupIsStillShownAsTheStatus();
    void cleaningKeepsHowTextIsWrittenAndDropsBidi();
    void artistsAreCappedAndDeduplicated();
    void aLanguageChangeRecomposesTheLine();
    void noPresenceBackendMeansNothingIsWatched();

    // Withdrawing the line at sign-out, account switch and quit.
    void quitWithGameAndMusicSendsExactlyOnePut();
    void aWithdrawnLineIgnoresLaterOverlaysUntilRestored();
    void theFarewellSettlesOnTheServersAnswer();
    void aRateLimitedFarewellIsRetriedOnceInsideTheBudget();
    void aFarewellRetryThatDoesNotFitGivesUp();
    void theFarewellWaitIsBounded();
    void signOutSwitchAndQuitAllWithdrawTheLine();
    void aSwitchDuringASignOutDoesNotBringTheSongBack();
    void aSuccessThenA429StillRetriesTheFarewell();
    void theSignOutBudgetFitsARateLimitRetry();
    void aRestoredLineLeavesNoFarewellBehind();
    void anOutOfOrderAnswerGetsTheFarewellSentAgain();

    // The MPRIS layer on a private bus.
    void mprisSourceReadsSpotifyOnAPrivateBus();
    void mprisSourceIgnoresEveryOtherPlayer();
    void aStoppedMprisSourceIsSilent();

private:
    QTemporaryDir m_configHome;
    QProcess m_daemon;
    QByteArray m_address;
};

// ---------------------------------------------------------------------------
// A fake Spotify: the MPRIS Player interface's two properties this client
// reads, exported through QtDBus's own org.freedesktop.DBus.Properties.
// ---------------------------------------------------------------------------
#ifdef HAVE_QT_DBUS
class FakeSpotifyPlayer : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
public:
    explicit FakeSpotifyPlayer(const QDBusConnection &bus) : m_bus(bus) {}
    QString playbackStatus() const { return m_status; }
    QVariantMap metadata() const { return m_metadata; }

    void set(const QString &status, const QString &title, const QStringList &artists,
             const QString &album, const QString &trackPath, bool announce = true)
    {
        m_status = status;
        m_metadata = {
            { QStringLiteral("xesam:title"), title },
            { QStringLiteral("xesam:artist"), artists },
            { QStringLiteral("xesam:album"), album },
            { QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(trackPath)) },
            { QStringLiteral("mpris:length"), qlonglong(215000000) },
            { QStringLiteral("mpris:artUrl"),
              QStringLiteral("https://i.scdn.co/image/should-never-be-fetched") },
        };
        if (!announce)
            return;
        QDBusMessage signal = QDBusMessage::createSignal(
            QStringLiteral("/org/mpris/MediaPlayer2"),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal << QStringLiteral("org.mpris.MediaPlayer2.Player")
               << QVariantMap { { QStringLiteral("PlaybackStatus"), m_status },
                                { QStringLiteral("Metadata"), m_metadata } }
               << QStringList();
        m_bus.send(signal);
    }
    // A PropertiesChanged for some OTHER interface of the same object.
    void announceOn(const QString &interfaceName, const QVariantMap &changed)
    {
        QDBusMessage signal = QDBusMessage::createSignal(
            QStringLiteral("/org/mpris/MediaPlayer2"),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal << interfaceName << changed << QStringList();
        m_bus.send(signal);
    }
    void setStatus(const QString &status)
    {
        m_status = status;
        QDBusMessage signal = QDBusMessage::createSignal(
            QStringLiteral("/org/mpris/MediaPlayer2"),
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal << QStringLiteral("org.mpris.MediaPlayer2.Player")
               << QVariantMap { { QStringLiteral("PlaybackStatus"), m_status } }
               << QStringList();
        m_bus.send(signal);
    }

private:
    QDBusConnection m_bus;
    QString m_status = QStringLiteral("Stopped");
    QVariantMap m_metadata;
};
#endif

void MusicActivityTest::initTestCase()
{
    QVERIFY(m_configHome.isValid());
    qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
    // Not the user's data or cache either (SettingsManager counts the account
    // stores under the data root).
    qputenv("XDG_DATA_HOME", (m_configHome.path() + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_CACHE_HOME", (m_configHome.path() + QStringLiteral("/cache")).toUtf8());
    // Never the user's session bus: forget it before anything can connect,
    // and point it at the private daemon once one runs.
    qunsetenv("DBUS_SESSION_BUS_ADDRESS");
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(QStringLiteral("music-activity-test"));

#ifdef HAVE_QT_DBUS
    const QString daemon = QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"));
    if (daemon.isEmpty())
        return;   // the MPRIS cases skip
    // The daemon's own session.conf rather than `--session`, which reads
    // /etc/dbus-1/session.conf (absent on dbus-broker systems).
    QStringList args { QStringLiteral("--nofork"), QStringLiteral("--print-address=1") };
    const QString packaged = QFileInfo(daemon).absolutePath()
        + QStringLiteral("/../share/dbus-1/session.conf");
    if (QFileInfo::exists(packaged))
        args << QStringLiteral("--config-file=%1").arg(QFileInfo(packaged).canonicalFilePath());
    else
        args << QStringLiteral("--session");
    m_daemon.start(daemon, args);
    QVERIFY(m_daemon.waitForStarted(5000));
    QVERIFY(m_daemon.waitForReadyRead(5000));
    m_address = m_daemon.readLine().trimmed();
    QVERIFY(!m_address.isEmpty());
    qputenv("DBUS_SESSION_BUS_ADDRESS", m_address);
#endif
}

void MusicActivityTest::cleanupTestCase()
{
#ifdef HAVE_QT_DBUS
    QDBusConnection::disconnectFromBus(QStringLiteral("music-test-player"));
    QDBusConnection::disconnectFromBus(QStringLiteral("music-test-client"));
    if (m_daemon.state() != QProcess::NotRunning) {
        m_daemon.kill();
        m_daemon.waitForFinished(3000);
    }
#endif
}

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------

void MusicActivityTest::mprisMetadataBecomesNowPlaying()
{
    const QVariantMap md {
        { QStringLiteral("xesam:title"), QStringLiteral("  Never\tGonna  Give You Up ") },
        { QStringLiteral("xesam:artist"),
          QStringList { QStringLiteral("Rick Astley"), QStringLiteral("Rick Astley"),
                        QStringLiteral("Guest") } },
        { QStringLiteral("xesam:album"), QStringLiteral("Whenever You Need Somebody") },
        { QStringLiteral("mpris:trackid"), QStringLiteral("/com/spotify/track/4cOdK2wGLETKBW3PvgPWqT") },
    };
    NowPlaying np = musicdetect::fromMpris(QStringLiteral("Playing"), md);
    QCOMPARE(np.title, QStringLiteral("Never Gonna Give You Up"));
    QCOMPARE(np.artists, (QStringList { QStringLiteral("Rick Astley"), QStringLiteral("Guest") }));
    QCOMPARE(np.album, QStringLiteral("Whenever You Need Somebody"));
    QVERIFY(np.playing);
    QVERIFY(np.showable());

    QVERIFY(!musicdetect::fromMpris(QStringLiteral("Paused"), md).showable());
    QVERIFY(!musicdetect::fromMpris(QStringLiteral("Stopped"), md).showable());
    QVERIFY(!musicdetect::fromMpris(QString(), md).showable());

    // A lone string artist is tolerated; a bidi override never survives.
    QVariantMap single = md;
    single.insert(QStringLiteral("xesam:artist"), QStringLiteral("Solo\u202EArtist"));
    np = musicdetect::fromMpris(QStringLiteral("Playing"), single);
    QCOMPARE(np.artists, QStringList { QStringLiteral("SoloArtist") });
}

void MusicActivityTest::advertisementsAndUntitledEntriesAreNeverShown()
{
    NowPlaying ad = track(QStringLiteral("Spotify"), QStringLiteral("Advertiser"));
    ad.trackId = QStringLiteral("/com/spotify/ad/123");
    QVERIFY(!ad.showable());
    ad.trackId = QStringLiteral("spotify:ad:000000012c603a6600000020316a17a1");
    QVERIFY(!ad.showable());
    QCOMPARE(musicdetect::statusTextFor(ad), QString());

    NowPlaying untitled = track(QString(), QStringLiteral("Somebody"));
    QVERIFY(!untitled.showable());
    // A podcast episode (no artist) is still music activity on Spotify.
    NowPlaying episode = track(QStringLiteral("Episode 12"), QString());
    episode.trackId = QStringLiteral("/com/spotify/episode/abc");
    QVERIFY(episode.showable());
}

void MusicActivityTest::statusLineWordingAndBounds()
{
    QCOMPARE(musicdetect::statusTextFor(track(QStringLiteral("Song A"), QStringLiteral("Artist A"))),
             kLine);
    NowPlaying two = track(QStringLiteral("Duet"), QStringLiteral("One"));
    two.artists << QStringLiteral("Two");
    QCOMPARE(musicdetect::statusTextFor(two), QStringLiteral("Listening to Duet by One, Two"));
    QCOMPARE(musicdetect::statusTextFor(track(QStringLiteral("Episode 12"), QString())),
             QStringLiteral("Listening to Episode 12"));
    QCOMPARE(musicdetect::statusTextFor(paused(track(QStringLiteral("X"), QStringLiteral("Y")))),
             QString());
    // A title that looks like a placeholder is text, not a second argument.
    QCOMPARE(musicdetect::statusTextFor(track(QStringLiteral("100%2 Pure"), QStringLiteral("DJ %1"))),
             QStringLiteral("Listening to 100%2 Pure by DJ %1"));
    // Bounded: one status line, never a paragraph.
    const NowPlaying huge = musicdetect::fromMpris(
        QStringLiteral("Playing"),
        { { QStringLiteral("xesam:title"), QString(500, QLatin1Char('t')) },
          { QStringLiteral("xesam:artist"), QStringList { QString(500, QLatin1Char('a')) } } });
    QCOMPARE(huge.title.size(), 100);
    QVERIFY(musicdetect::statusTextFor(huge).size() <= 200);
    QVERIFY(musicdetect::statusTextFor(huge).startsWith(QLatin1String("Listening to ttt")));
}

namespace {
// Stands in for a loaded catalogue: answers only the two music lines.
class MusicLineTranslator : public QTranslator
{
public:
    QString translate(const char *context, const char *sourceText, const char *,
                      int) const override
    {
        if (qstrcmp(context, "MusicActivity") != 0)
            return {};
        if (qstrcmp(sourceText, "Listening to %1 by %2") == 0)
            return QStringLiteral("Klausosi: %2 – %1");
        if (qstrcmp(sourceText, "Listening to %1") == 0)
            return QStringLiteral("Klausosi: %1");
        return {};
    }
    bool isEmpty() const override { return false; }
};
}   // namespace

void MusicActivityTest::aFlatpakListensOnlyWhenItWasGrantedSpotify()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto write = [&](const char *name, const QByteArray &body) {
        const QString path = dir.filePath(QLatin1String(name));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write(body);
        return path;
    };
    // The stock Flatpak policy: nothing for MPRIS, so the feature is off.
    QVERIFY(!musicdetect::flatpakGrantsSpotifyMpris(write("none",
        "[Application]\nname=org.lightning_matrix.Lightning\n\n"
        "[Session Bus Policy]\norg.freedesktop.secrets=talk\n")));
    // A user override, or the old manifest, grants the one name.
    QVERIFY(musicdetect::flatpakGrantsSpotifyMpris(write("talk",
        "[Session Bus Policy]\norg.freedesktop.secrets=talk\n"
        "org.mpris.MediaPlayer2.spotify=talk\n")));
    // The same text in another group, or at a lower level, does not count.
    QVERIFY(!musicdetect::flatpakGrantsSpotifyMpris(write("other",
        "[Context]\norg.mpris.MediaPlayer2.spotify=talk\n")));
    QVERIFY(!musicdetect::flatpakGrantsSpotifyMpris(write("see",
        "[Session Bus Policy]\norg.mpris.MediaPlayer2.spotify=see\n")));
    QVERIFY(!musicdetect::flatpakGrantsSpotifyMpris(
        dir.filePath(QStringLiteral("missing"))));
}

void MusicActivityTest::theLineIsInTheSendersLanguage()
{
    // A status line is the user's own words, written in the app's language
    // (Matrix cannot localise it per reader); a translation may reorder.
    MusicLineTranslator lt;
    QVERIFY(QCoreApplication::installTranslator(&lt));
    const auto cleanup = qScopeGuard([&lt] { QCoreApplication::removeTranslator(&lt); });
    QCOMPARE(musicdetect::statusTextFor(track(QStringLiteral("Song A"), QStringLiteral("Artist A"))),
             QStringLiteral("Klausosi: Artist A – Song A"));
    QCOMPARE(musicdetect::statusTextFor(track(QStringLiteral("Episode 12"), QString())),
             QStringLiteral("Klausosi: Episode 12"));
}

void MusicActivityTest::windowsCaptionsAreParsedLikeSpotifyNamesThem()
{
    using musicdetect::fromSpotifyWindowTitles;
    NowPlaying np = fromSpotifyWindowTitles(
        { QStringLiteral("GDI+ Window (Spotify.exe)"),
          QStringLiteral("The Beatles - Let It Be - Remastered 2009") });
    QVERIFY(np.showable());
    QCOMPARE(np.artists, QStringList { QStringLiteral("The Beatles") });
    QCOMPARE(np.title, QStringLiteral("Let It Be - Remastered 2009"));
    QCOMPARE(np.album, QString());   // the caption has no album

    // Paused or idle: the window is named after the app.
    for (const char *idle : { "Spotify", "Spotify Free", "Spotify Premium", "spotify premium" })
        QVERIFY2(!fromSpotifyWindowTitles({ QString::fromLatin1(idle) }).showable(), idle);
    // Advertisements.
    QVERIFY(!fromSpotifyWindowTitles({ QStringLiteral("Advertisement") }).showable());
    QVERIFY(!fromSpotifyWindowTitles({ QStringLiteral("Spotify - Advertisement") }).showable());
    QVERIFY(!fromSpotifyWindowTitles({ QStringLiteral("Spotify - Listen to more") }).showable());
    // No caption at all.
    QVERIFY(!fromSpotifyWindowTitles({}).showable());
    QVERIFY(!fromSpotifyWindowTitles({ QStringLiteral(" - Title only") }).showable());
}

void MusicActivityTest::windowsCaptionSourcePollsOnlyWhileStarted()
{
    // The Windows layer behind its interface: the caption provider is the
    // only Win32 part (src/presence/SpotifyWindowTitles_win.cpp).
    QStringList captions { QStringLiteral("Spotify Premium") };
    int calls = 0;
    WindowTitleMusicSource source([&] {
        ++calls;
        return captions;
    }, 20);
    QSignalSpy changed(&source, &MusicSource::nowPlayingChanged);
    QTest::qWait(80);
    QCOMPARE(calls, 0);   // not started: never polls

    source.start();
    QVERIFY(source.running());
    QCOMPARE(calls, 1);   // reads at once
    QVERIFY(!source.nowPlaying().showable());
    QCOMPARE(changed.count(), 0);

    captions = { QStringLiteral("Artist A - Song A") };
    QTRY_COMPARE(changed.count(), 1);
    QCOMPARE(source.nowPlaying().title, QStringLiteral("Song A"));
    QTest::qWait(80);
    QCOMPARE(changed.count(), 1);   // unchanged captions emit nothing

    captions = { QStringLiteral("Spotify Premium") };   // paused
    QTRY_COMPARE(changed.count(), 2);
    QVERIFY(!source.nowPlaying().playing);

    source.stop();
    QVERIFY(!source.running());
    const int stoppedAt = calls;
    captions = { QStringLiteral("Artist B - Song B") };
    QTest::qWait(100);
    QCOMPARE(calls, stoppedAt);
    QCOMPARE(changed.count(), 2);
}

void MusicActivityTest::qmlCopyIsPlainTextAndSendsNoArt()
{
    const QString qml = readSource(QStringLiteral("qml/MusicActivitySettings.qml"));
    QVERIFY2(!qml.isEmpty(), "run from the source directory");
    // Song titles are other people's text, and the copy contains a literal
    // "<song>": every Label is PlainText.
    const QStringList blocks = qml.split(QStringLiteral("Label {"));
    int labels = 0;
    for (int i = 1; i < blocks.size(); ++i) {
        ++labels;
        const QString b = blocks[i].left(blocks[i].indexOf(QLatin1String("\n        }")) > 0
                                             ? blocks[i].indexOf(QLatin1String("\n        }"))
                                             : blocks[i].size());
        QVERIFY2(b.contains(QLatin1String("textFormat: Text.PlainText")),
                 qPrintable(QStringLiteral("Label #%1 is not PlainText").arg(i)));
    }
    QCOMPARE(labels, 11);
    // The Private Session warning is its own line, shown while the feature
    // is on, and the copy never promises a reliable withdrawal.
    QVERIFY(qml.contains(QLatin1String("musicActivityPrivateSessionNote")));
    QVERIFY(qml.contains(QLatin1String("Private Session is not detected")));
    QVERIFY(qml.contains(QLatin1String("closed by force")));
    QVERIFY(!qml.contains(QLatin1String("reliabl")));
    // No album art: loading it would contact Spotify's CDN from Lightning.
    QVERIFY(!qml.contains(QLatin1String("Image {")));
    QVERIFY(!qml.contains(QLatin1String("artUrl")));
    QVERIFY(qml.contains(QLatin1String("activity.supported")));

    const QString cpp = readSource(QStringLiteral("src/presence/MusicDetection.cpp"));
    QVERIFY(!cpp.contains(QLatin1String("mpris:artUrl")));
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

void MusicActivityTest::labsIsOffByDefault()
{
    // Sets NOTHING: the default of a store no case has left a value in.
    RecordingClient client;
    SettingsManager settings;
    QVERIFY(!settings.musicActivityEnabled());
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    MusicActivity activity;
    int alive = 0;
    activity.setSourceFactoryForTest([&alive] {
        return std::unique_ptr<MusicSource>(std::make_unique<FakeSource>(&alive));
    });
    activity.setPresence(&presence);
    activity.setSettings(&settings);
    QVERIFY(!activity.active());
    QVERIFY(!activity.sourceRunning());
    QCOMPARE(activity.sourcesCreated(), 0);
    QCOMPARE(alive, 0);
    QCOMPARE(presence.musicActivity(), QString());
}

void MusicActivityTest::offMeansNoSourceAndTurningItOffWithdrawsAtOnce()
{
    Fixture f(false);
    QVERIFY(!f.activity.active());
    QCOMPARE(f.alive, 0);

    f.settings.setMusicActivityEnabled(true);
    QVERIFY(f.activity.active());
    QCOMPARE(f.alive, 1);
    QVERIFY(f.source && f.source->started);

    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.effectiveStatusText(), kLine);
    const int sent = f.client.published.size();

    // Turning it off: no source left, and the line is withdrawn NOW, with a
    // publish (not at the next keep-alive).
    f.settings.setMusicActivityEnabled(false);
    QCOMPARE(f.alive, 0);
    QVERIFY(!f.activity.listening());
    QCOMPARE(f.presence.effectiveStatusText(), QString());
    QCOMPARE(f.client.published.size(), sent + 1);
    QCOMPARE(f.client.published.last().second, QString());
}

void MusicActivityTest::aTrackIsAdoptedAfterTheSettleAndRidesTheKeepAlive()
{
    Fixture f;
    QSignalSpy changed(&f.activity, &MusicActivity::nowPlayingChanged);
    const int sent = f.client.published.size();
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    // Not before the settle.
    QCOMPARE(f.presence.musicActivity(), QString());
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(f.activity.title(), QStringLiteral("Song A"));
    QCOMPARE(f.activity.album(), QStringLiteral("Album"));
    QVERIFY(f.activity.shownAsStatus());
    // A track change costs NO presence PUT of its own (rc_presence is per
    // user, burst 1)...
    QTest::qWait(100);
    QCOMPARE(f.client.published.size(), sent);
    // ...the keep-alive carries it.
    f.presence.setPublishIntervalForTest(40);
    QTRY_VERIFY(f.client.published.size() > sent);
    QCOMPARE(f.client.published.last().second, kLine);
}

void MusicActivityTest::skippingThroughSongsAdoptsOnlyTheOneKept()
{
    Fixture f;
    f.activity.setSettleForTest(150);
    QSignalSpy changed(&f.activity, &MusicActivity::nowPlayingChanged);
    for (int i = 1; i <= 5; ++i) {
        f.source->play(track(QStringLiteral("Skip %1").arg(i), QStringLiteral("Artist")));
        QTest::qWait(30);
        QCOMPARE(f.presence.musicActivity(), QString());
    }
    QTRY_COMPARE(f.presence.musicActivity(), QStringLiteral("Listening to Skip 5 by Artist"));
    QCOMPARE(changed.count(), 1);   // nothing in between was adopted
    // A repeat report of the same track does not restart anything.
    f.source->play(track(QStringLiteral("Skip 5"), QStringLiteral("Artist")));
    QTest::qWait(200);
    QCOMPARE(changed.count(), 1);
}

void MusicActivityTest::pauseClearsAfterTheGraceAndAQuickResumeChangesNothing()
{
    Fixture f;
    const NowPlaying a = track(QStringLiteral("Song A"), QStringLiteral("Artist A"));
    f.source->play(a);
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    QSignalSpy changed(&f.activity, &MusicActivity::nowPlayingChanged);

    // A quick pause and resume: nothing changes anywhere.
    f.source->play(paused(a));
    QTest::qWait(60);
    QCOMPARE(f.presence.musicActivity(), kLine);
    f.source->play(a);
    QTest::qWait(250);   // longer than the grace
    QCOMPARE(f.presence.musicActivity(), kLine);
    QCOMPARE(changed.count(), 0);

    // A real pause: still shown inside the grace, gone after it.
    f.source->play(paused(a));
    QTest::qWait(60);
    QCOMPARE(f.presence.musicActivity(), kLine);
    QTRY_COMPARE(f.presence.musicActivity(), QString());
    QVERIFY(!f.activity.listening());
    QCOMPARE(changed.count(), 1);
    // A stop goes the same way, and an advertisement counts as not playing.
    f.source->play(a);
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    NowPlaying ad = track(QStringLiteral("Buy things"), QStringLiteral("Brand"));
    ad.trackId = QStringLiteral("/com/spotify/ad/1");
    f.source->play(ad);
    QTRY_COMPARE(f.presence.musicActivity(), QString());
}

void MusicActivityTest::spotifyQuittingClearsLikeAStop()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    f.source->play(NowPlaying {});   // the source reports nothing at all
    QTRY_COMPARE(f.presence.musicActivity(), QString());
}

void MusicActivityTest::theUsersOwnStatusAndAGameBothWin()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.effectiveStatusText(), kLine);
    QVERIFY(f.activity.shownAsStatus());

    // A status the user typed takes the line (Discord: a custom status
    // overrides activity statuses) and is never replaced by music.
    f.presence.setOwnStatus(QStringLiteral("☕"), QStringLiteral("Busy"), 0);
    QCOMPARE(f.presence.effectiveStatusText(), QStringLiteral("☕ Busy"));
    QCOMPARE(f.client.published.last().second, QStringLiteral("☕ Busy"));
    QVERIFY(f.activity.listening());
    QVERIFY(!f.activity.shownAsStatus());
    // Clearing it gives the line back to the music.
    f.presence.clearOwnStatus();
    QCOMPARE(f.presence.effectiveStatusText(), kLine);
    QCOMPARE(f.client.published.last().second, kLine);
    QVERIFY(f.activity.shownAsStatus());

    // A detected game wins over music (Discord: "the game will take
    // precedence and Spotify won't show as your status").
    f.presence.setGameActivity(QStringLiteral("Portal 2"));
    QCOMPARE(f.presence.effectiveStatusText(), QStringLiteral("Playing Portal 2"));
    QVERIFY(!f.activity.shownAsStatus());
    // And the user's own status outranks the game too: own > game > music.
    f.presence.setOwnStatus(QStringLiteral("☕"), QStringLiteral("Busy"), 0);
    QCOMPARE(f.presence.effectiveStatusText(), QStringLiteral("☕ Busy"));
    f.presence.clearOwnStatus();
    QCOMPARE(f.presence.effectiveStatusText(), QStringLiteral("Playing Portal 2"));
    f.presence.setGameActivity(QString());
    QCOMPARE(f.presence.effectiveStatusText(), kLine);
    QVERIFY(f.activity.shownAsStatus());
}

void MusicActivityTest::aWithdrawalBehindTheOwnStatusSendsNothing()
{
    Fixture f;
    f.presence.setOwnStatus(QStringLiteral("☕"), QStringLiteral("Busy"), 0);
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    const int sent = f.client.published.size();
    // The music never held the line, so taking it away changes nothing on
    // the wire and costs no PUT.
    f.settings.setMusicActivityEnabled(false);
    QCOMPARE(f.presence.musicActivity(), QString());
    QCOMPARE(f.client.published.size(), sent);
}

void MusicActivityTest::sharingOffWithdrawsAndStopsTheSource()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    f.settings.setSharePresence(false);
    QVERIFY(!f.activity.active());
    QCOMPARE(f.alive, 0);
    QCOMPARE(f.presence.musicActivity(), QString());
    // Whatever went out last (the final offline) carries no song.
    for (const auto &p : f.client.published) {
        if (p.first == 2)
            QVERIFY(!p.second.contains(QLatin1String("Listening")));
    }
    QVERIFY(!f.client.published.last().second.contains(QLatin1String("Listening")));
    f.settings.setSharePresence(true);
    QCOMPARE(f.alive, 1);   // a fresh source once sharing is back
}

void MusicActivityTest::quitWithdrawsWhileTheClientCanStillSend()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    const int sent = f.client.published.size();

    f.activity.shutdown();   // AppController::prepareForShutdown
    QCOMPARE(f.alive, 0);
    // A status_msg outlives the session on the server: the empty line is
    // published now, not left to a keep-alive that will never come.
    QCOMPARE(f.client.published.size(), sent + 1);
    QCOMPARE(f.client.published.last().second, QString());
    // Nothing can restart it.
    f.settings.setMusicActivityEnabled(false);
    f.settings.setMusicActivityEnabled(true);
    QCOMPARE(f.alive, 0);
    QVERIFY(readSource(QStringLiteral("src/app/AppController.cpp"))
                .contains(QLatin1String("m_musicActivity->shutdown()")));
}

void MusicActivityTest::logoutStopsTheSourceAndForgetsTheTrack()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    Q_EMIT f.client.loggedOut();
    QCOMPARE(f.alive, 0);
    QVERIFY(!f.activity.listening());
    QCOMPARE(f.presence.musicActivity(), QString());
    // The next session watches again, only once it is live.
    Q_EMIT f.client.connectionStateChanged(MatrixClient::Syncing);
    QCOMPARE(f.alive, 1);
}

void MusicActivityTest::onlyTitleAndArtistLeaveThisMachine()
{
    Fixture f;
    f.presence.setPublishIntervalForTest(30);
    NowPlaying np = track(QStringLiteral("Song A"), QStringLiteral("Artist A"),
                          QStringLiteral("Secret Album"));
    f.source->play(np);
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    const int sent = f.client.published.size();
    QTRY_VERIFY(f.client.published.size() >= sent + 2);
    for (const auto &p : f.client.published) {
        QVERIFY(!p.second.contains(QLatin1String("Secret Album")));
        QVERIFY(!p.second.contains(QLatin1String("spotify")));
    }
    QCOMPARE(f.activity.album(), QStringLiteral("Secret Album"));   // local preview only
}

void MusicActivityTest::cleaningKeepsHowTextIsWrittenAndDropsBidi()
{
    using musicdetect::cleanField;
    // An emoji ZWJ sequence (woman technologist) survives whole.
    const QString coder = QStringLiteral("\U0001F469\u200D\U0001F4BB Coder");
    QCOMPARE(cleanField(coder, 100), coder);
    // Persian writes with ZWNJ: "می\u200Cخواهم".
    const QString persian = QStringLiteral("می\u200Cخواهم");
    QCOMPARE(cleanField(persian, 100), persian);
    // A subdivision flag is tag characters (Cf) after a black flag.
    const QString scotland = QStringLiteral(
        "\U0001F3F4\U000E0067\U000E0062\U000E0073\U000E0063\U000E0074\U000E007F");
    QCOMPARE(cleanField(scotland, 100), scotland);
    // Bidi controls are dropped without leaving a gap; other invisible
    // format characters too; a tab separates words.
    QCOMPARE(cleanField(QStringLiteral("ab\u202Ecd\u2066ef\u2069\u200Fgh\u061C"), 100),
             QStringLiteral("abcdefgh"));
    QCOMPARE(cleanField(QStringLiteral("soft\u00ADhy\uFEFFphen"), 100),
             QStringLiteral("softhyphen"));
    QCOMPARE(cleanField(QStringLiteral("a\tb\nc"), 100), QStringLiteral("a b c"));
    // The published line keeps them too.
    QCOMPARE(musicdetect::statusTextFor(track(coder, persian)),
             QStringLiteral("Listening to %1 by %2").arg(coder, persian));
}

void MusicActivityTest::artistsAreCappedAndDeduplicated()
{
    QStringList many;
    for (int i = 0; i < 15; ++i)
        many << QStringLiteral("Artist %1").arg(i % 12) << QStringLiteral("Artist %1").arg(i % 12);
    const NowPlaying np = musicdetect::fromMpris(
        QStringLiteral("Playing"),
        { { QStringLiteral("xesam:title"), QStringLiteral("Crowd") },
          { QStringLiteral("xesam:artist"), many } });
    QCOMPARE(np.artists.size(), 10);
    QCOMPARE(np.artists.first(), QStringLiteral("Artist 0"));
    QCOMPARE(np.artists.last(), QStringLiteral("Artist 9"));
}

void MusicActivityTest::aLanguageChangeRecomposesTheLine()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    const int sent = f.client.published.size();
    MusicLineTranslator lt;
    QVERIFY(QCoreApplication::installTranslator(&lt));   // sends LanguageChange
    QCOMPARE(f.presence.musicActivity(), QStringLiteral("Klausosi: Artist A – Song A"));
    QCOMPARE(f.activity.statusLine(), QStringLiteral("Klausosi: Artist A – Song A"));
    QVERIFY(f.activity.shownAsStatus());
    QCOMPARE(f.client.published.size(), sent);   // rides the keep-alive
    QVERIFY(QCoreApplication::removeTranslator(&lt));
    QCOMPARE(f.presence.musicActivity(), kLine);
}

void MusicActivityTest::noPresenceBackendMeansNothingIsWatched()
{
    class NoPresenceClient : public RecordingClient
    {
    public:
        bool supportsPresence() const override { return false; }
    };
    NoPresenceClient client;
    SettingsManager settings;
    settings.setSharePresence(true);
    settings.setMusicActivityEnabled(true);
    const auto restore = qScopeGuard([&settings] { settings.setMusicActivityEnabled(false); });
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    MusicActivity activity;
    int alive = 0;
    activity.setSourceFactoryForTest([&alive] {
        return std::unique_ptr<MusicSource>(std::make_unique<FakeSource>(&alive));
    });
    activity.setPresence(&presence);
    activity.setSettings(&settings);
    QVERIFY(!presence.active());
    QVERIFY(!activity.active());
    QCOMPARE(activity.sourcesCreated(), 0);
    QCOMPARE(alive, 0);
}

// --- withdrawing the line at sign-out, account switch and quit -------------

void MusicActivityTest::quitWithGameAndMusicSendsExactlyOnePut()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    f.presence.setGameActivity(QStringLiteral("Portal 2"));   // GameActivity's push
    QCOMPARE(f.client.published.last().second, QStringLiteral("Playing Portal 2"));
    const int sent = f.client.published.size();

    // AppController::prepareForShutdown's order.
    QVERIFY(f.presence.withdrawActivityLine());
    f.presence.setGameActivity(QString());   // GameActivity::shutdown()
    f.activity.shutdown();                   // MusicActivity::shutdown()
    QCOMPARE(f.client.published.size(), sent + 1);
    QCOMPARE(f.client.published.last().second, QString());
    for (int i = sent; i < f.client.published.size(); ++i)
        QVERIFY(!f.client.published[i].second.contains(QLatin1String("Listening")));
    QVERIFY(f.presence.farewellPending());
}

void MusicActivityTest::aWithdrawnLineIgnoresLaterOverlaysUntilRestored()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    QVERIFY(f.presence.withdrawActivityLine());
    QVERIFY(!f.presence.withdrawActivityLine());   // a second call sends nothing
    const int sent = f.client.published.size();
    // A game starting during the sign-out wait is remembered, not shown.
    f.presence.setGameActivity(QStringLiteral("Portal 2"));
    QCOMPARE(f.presence.effectiveStatusText(), QString());
    QCOMPARE(f.client.published.size(), sent);
    // A refused account switch puts the LATEST overlays back in one PUT.
    f.presence.restoreActivityLine();
    QCOMPARE(f.client.published.size(), sent + 1);
    QCOMPARE(f.client.published.last().second, QStringLiteral("Playing Portal 2"));
    f.presence.setGameActivity(QString());
    QCOMPARE(f.presence.effectiveStatusText(), kLine);
    // After a session ends, the next one starts unsuspended.
    QVERIFY(f.presence.withdrawActivityLine());
    Q_EMIT f.client.loggedOut();
    QVERIFY(!f.presence.farewellPending());
    Q_EMIT f.client.connectionStateChanged(MatrixClient::Syncing);
    f.presence.setGameActivity(QStringLiteral("Hades II"));
    QCOMPARE(f.presence.effectiveStatusText(), QStringLiteral("Playing Hades II"));
    f.presence.setGameActivity(QString());
}

void MusicActivityTest::theFarewellSettlesOnTheServersAnswer()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    answerAll(f.presence, f.client);
    QVERIFY(f.presence.withdrawActivityLine());
    QCOMPARE(f.presence.publishesInFlight(), 1);
    bool ran = false;
    QElapsedTimer clock;
    clock.start();
    f.presence.whenFarewellSettled(PresenceManager::kFarewellBudgetMs, &f.presence,
                                   [&ran] { ran = true; });
    QTimer::singleShot(50, &f.client, [&f] { Q_EMIT f.client.presencePublished(); });
    QTRY_VERIFY(ran);
    QVERIFY(clock.elapsed() < 1000);   // not the whole budget
    QVERIFY(!f.presence.farewellPending());
    // With nothing pending, `then` still runs (queued).
    bool ranAgain = false;
    f.presence.whenFarewellSettled(1000, &f.presence, [&ranAgain] { ranAgain = true; });
    QVERIFY(!ranAgain);
    QTRY_VERIFY(ranAgain);
}

void MusicActivityTest::aRateLimitedFarewellIsRetriedOnceInsideTheBudget()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    answerAll(f.presence, f.client);
    QVERIFY(f.presence.withdrawActivityLine());
    const int sent = f.client.published.size();
    bool ran = false;
    f.presence.whenFarewellSettled(PresenceManager::kFarewellBudgetMs, &f.presence,
                                   [&ran] { ran = true; });
    // rc_presence refused it and asked for 300 ms: one retry, same line.
    Q_EMIT f.client.presencePublishFailed(QStringLiteral("rate_limited"), 300);
    QVERIFY(!ran);
    QTRY_COMPARE(f.client.published.size(), sent + 1);
    QCOMPARE(f.client.published.last().second, QString());
    // A second refusal is not retried again: the wait ends.
    Q_EMIT f.client.presencePublishFailed(QStringLiteral("rate_limited"), 300);
    QTRY_VERIFY(ran);
    QTest::qWait(400);
    QCOMPARE(f.client.published.size(), sent + 1);
}

void MusicActivityTest::aFarewellRetryThatDoesNotFitGivesUp()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    answerAll(f.presence, f.client);
    QVERIFY(f.presence.withdrawActivityLine());
    const int sent = f.client.published.size();
    bool ran = false;
    f.presence.whenFarewellSettled(PresenceManager::kFarewellBudgetMs, &f.presence,
                                   [&ran] { ran = true; });
    Q_EMIT f.client.presencePublishFailed(QStringLiteral("rate_limited"), 10000);
    QTRY_VERIFY(ran);
    QTest::qWait(200);
    QCOMPARE(f.client.published.size(), sent);
    QVERIFY(!f.presence.farewellPending());
}

void MusicActivityTest::theFarewellWaitIsBounded()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    // The quit form: a nested loop that ends at the budget with no answer.
    QVERIFY(f.presence.withdrawActivityLine());
    QElapsedTimer clock;
    clock.start();
    QVERIFY(!f.presence.settleFarewell(200));
    QVERIFY(clock.elapsed() >= 150 && clock.elapsed() < 1500);
    QVERIFY(!f.presence.farewellPending());
    // ...and ends early on the answer.
    f.presence.restoreActivityLine();
    answerAll(f.presence, f.client);
    QVERIFY(f.presence.withdrawActivityLine());
    QTimer::singleShot(30, &f.client, [&f] { Q_EMIT f.client.presencePublished(); });
    clock.restart();
    QVERIFY(f.presence.settleFarewell(PresenceManager::kFarewellBudgetMs));
    QVERIFY(clock.elapsed() < 1500);
    // Nothing pending: returns at once.
    QVERIFY(f.presence.settleFarewell(PresenceManager::kFarewellBudgetMs));
}

void MusicActivityTest::signOutSwitchAndQuitAllWithdrawTheLine()
{
    const QString src = readSource(QStringLiteral("src/app/AppController.cpp"));
    QVERIFY2(!src.isEmpty(), "run from the source directory");
    // Quit: one withdrawal before both pollers stop, then the bounded wait.
    const qsizetype quitWithdraw = src.indexOf(QLatin1String(
        "const bool farewell = m_presence && m_presence->withdrawActivityLine();"));
    QVERIFY(quitWithdraw > 0);
    QVERIFY(src.indexOf(QLatin1String("m_gameActivity->shutdown()")) > quitWithdraw);
    QVERIFY(src.indexOf(QLatin1String("m_musicActivity->shutdown()")) > quitWithdraw);
    QVERIFY(src.indexOf(QLatin1String("m_presence->settleFarewell(")) > quitWithdraw);
    // Sign-out: inside the before-logout hook, with the bounded wait.
    const qsizetype hook = src.indexOf(QLatin1String("*beforeLogout = [this"));
    QVERIFY(hook > 0);
    // The read marker and the call leave go out in the FIRST pass, then the
    // withdrawal, with the sign-out budget (one rate-limit retry fits).
    const qsizetype hookWithdraw = src.indexOf(QLatin1String("withdrawActivityLine()"), hook);
    QVERIFY(hookWithdraw > src.indexOf(QLatin1String("flushFullyReadMarker"), hook));
    QVERIFY(hookWithdraw > src.indexOf(QLatin1String("call->leave();"), hook));
    QVERIFY(hookWithdraw < src.indexOf(QLatin1String("membershipWritesPending()"), hook));
    QVERIFY(src.indexOf(QLatin1String("PresenceManager::kSignOutFarewellBudgetMs"), hook) > hook);
    QVERIFY(PresenceManager::kSignOutFarewellBudgetMs >= 11000);
    // Account switch: before the client is released, and put back if refused.
    const qsizetype detach = src.indexOf(QLatin1String("!m_client->detachSession();\n    trace::stepEnd(trace::Step::S3);"));
    QVERIFY(detach > 0);
    const qsizetype switchWithdraw = src.lastIndexOf(
        QLatin1String("m_presence->withdrawActivityLine(&withdrewForSwitch);"), detach);
    QVERIFY(switchWithdraw > 0 && detach - switchWithdraw < 400);
    // ...and restores only what it withdrew itself.
    const qsizetype restore = src.indexOf(
        QLatin1String("if (m_presence && withdrewForSwitch)\n            m_presence->restoreActivityLine();"), detach);
    QVERIFY(restore > detach && restore - detach < 300);
}

void MusicActivityTest::aSwitchDuringASignOutDoesNotBringTheSongBack()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    // The sign-out withdraws first and is waiting on its answer.
    bool signOutWithdrew = false;
    QVERIFY(f.presence.withdrawActivityLine(&signOutWithdrew));
    QVERIFY(signOutWithdrew);
    QVERIFY(f.presence.activityLineWithdrawn());
    const int sent = f.client.published.size();
    // A switch now: its own call withdrew nothing, so its refusal must not
    // restore anything (AppController's guard, mirrored here).
    bool switchWithdrew = true;
    QVERIFY(!f.presence.withdrawActivityLine(&switchWithdrew));
    QVERIFY(!switchWithdrew);
    if (switchWithdrew)
        f.presence.restoreActivityLine();
    QCOMPARE(f.client.published.size(), sent);
    QCOMPARE(f.presence.effectiveStatusText(), QString());
    QVERIFY(f.presence.activityLineWithdrawn());
}

void MusicActivityTest::aSuccessThenA429StillRetriesTheFarewell()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    answerAll(f.presence, f.client);
    // A publish still in flight when the withdrawal goes out.
    f.presence.setGameActivity(QStringLiteral("Portal 2"));
    QCOMPARE(f.presence.publishesInFlight(), 1);
    QVERIFY(f.presence.withdrawActivityLine());
    QCOMPARE(f.presence.publishesInFlight(), 2);
    const int sent = f.client.published.size();
    bool ran = false;
    f.presence.whenFarewellSettled(PresenceManager::kFarewellBudgetMs, &f.presence,
                                   [&ran] { ran = true; });
    // The first answer is the EARLIER publish's success: not the farewell's.
    Q_EMIT f.client.presencePublished();
    QTest::qWait(50);
    QVERIFY(!ran);
    QVERIFY(f.presence.farewellPending());
    // Then the farewell is refused: it is retried, not given up.
    Q_EMIT f.client.presencePublishFailed(QStringLiteral("rate_limited"), 200);
    QVERIFY(!ran);
    QTRY_COMPARE(f.client.published.size(), sent + 1);
    QCOMPARE(f.client.published.last().second, QString());
    QTest::qWait(50);
    QVERIFY(!ran);
    // The retry is accepted: now it is over.
    Q_EMIT f.client.presencePublished();
    QTRY_VERIFY(ran);
}

void MusicActivityTest::theSignOutBudgetFitsARateLimitRetry()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    answerAll(f.presence, f.client);
    QVERIFY(f.presence.withdrawActivityLine());
    const int sent = f.client.published.size();
    bool ran = false;
    f.presence.whenFarewellSettled(PresenceManager::kSignOutFarewellBudgetMs, &f.presence,
                                   [&ran] { ran = true; });
    // 3.5 s does not fit the 3 s quit budget, and does fit the sign-out one.
    Q_EMIT f.client.presencePublishFailed(QStringLiteral("rate_limited"), 3500);
    QTest::qWait(100);
    QVERIFY(!ran);
    QTRY_VERIFY_WITH_TIMEOUT(f.client.published.size() == sent + 1, 6000);
    Q_EMIT f.client.presencePublished();
    QTRY_VERIFY(ran);
}

void MusicActivityTest::aRestoredLineLeavesNoFarewellBehind()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    answerAll(f.presence, f.client);
    QVERIFY(f.presence.withdrawActivityLine());
    QVERIFY(f.presence.farewellPending());
    // A refused switch puts the line back: there is no farewell any more.
    f.presence.restoreActivityLine();
    QVERIFY(!f.presence.farewellPending());
    QCOMPARE(f.client.published.last().second, kLine);
    // So a later refusal takes the ORDINARY retry chain, not the farewell's.
    QCOMPARE(f.presence.retryChainForTest(), 0);
    Q_EMIT f.client.presencePublishFailed(QStringLiteral("rate_limited"), 300);
    QCOMPARE(f.presence.retryChainForTest(), 1);
    QVERIFY(!f.presence.farewellPending());
}

void MusicActivityTest::anOutOfOrderAnswerGetsTheFarewellSentAgain()
{
    Fixture f;
    f.source->play(track(QStringLiteral("Song A"), QStringLiteral("Artist A")));
    QTRY_COMPARE(f.presence.musicActivity(), kLine);
    answerAll(f.presence, f.client);
    // A PUT carrying an activity is still in flight at the withdrawal.
    f.presence.setGameActivity(QStringLiteral("Portal 2"));
    QVERIFY(f.presence.withdrawActivityLine());
    QCOMPARE(f.presence.publishesInFlight(), 2);
    const int sent = f.client.published.size();
    bool ran = false;
    f.presence.whenFarewellSettled(PresenceManager::kSignOutFarewellBudgetMs, &f.presence,
                                   [&ran] { ran = true; });
    // Both answered (in whatever order the server applied them): the server
    // may have ended on the game, so the empty line is sent once more...
    Q_EMIT f.client.presencePublished();
    QCOMPARE(f.client.published.size(), sent);
    Q_EMIT f.client.presencePublished();
    QCOMPARE(f.client.published.size(), sent + 1);
    QCOMPARE(f.client.published.last().second, QString());
    QTest::qWait(50);
    QVERIFY(!ran);
    // ...and only its answer ends the wait. Exactly once.
    Q_EMIT f.client.presencePublished();
    QTRY_VERIFY(ran);
    QCOMPARE(f.client.published.size(), sent + 1);
}

void MusicActivityTest::aTitleThatLooksLikeMarkupIsStillShownAsTheStatus()
{
    // PresenceManager neutralises markup-like status text, so the line it
    // publishes is not byte-equal to the composed one. The Settings preview
    // must still say the line is shown, and quote what is actually sent.
    Fixture f;
    f.source->play(track(QStringLiteral("Song <b>A</b>"), QStringLiteral("Artist A")));
    QTRY_VERIFY(f.activity.listening());
    const QString sent = f.presence.effectiveStatusText();
    QVERIFY(sent.startsWith(QLatin1String("Listening to Song ")));
    QVERIFY(!sent.contains(QLatin1Char('<')));
    QVERIFY(f.activity.shownAsStatus());
    QCOMPARE(f.activity.statusLine(), sent);
}

// ---------------------------------------------------------------------------
// MPRIS on a private bus
// ---------------------------------------------------------------------------

void MusicActivityTest::mprisSourceReadsSpotifyOnAPrivateBus()
{
#ifndef HAVE_QT_DBUS
    QSKIP("built without QtDBus");
#else
    if (m_address.isEmpty())
        QSKIP("dbus-daemon not found; the MPRIS cases need a private bus");
    QDBusConnection playerBus = QDBusConnection::connectToBus(
        QString::fromLatin1(m_address), QStringLiteral("music-test-player"));
    QDBusConnection clientBus = QDBusConnection::connectToBus(
        QString::fromLatin1(m_address), QStringLiteral("music-test-client"));
    QVERIFY(playerBus.isConnected() && clientBus.isConnected());

    FakeSpotifyPlayer player(playerBus);
    player.set(QStringLiteral("Playing"), QStringLiteral("Song A"),
               { QStringLiteral("Artist A"), QStringLiteral("Artist B") },
               QStringLiteral("Album A"), QStringLiteral("/com/spotify/track/aaa"), false);
    QVERIFY(playerBus.registerObject(QStringLiteral("/org/mpris/MediaPlayer2"), &player,
                                     QDBusConnection::ExportAllProperties));
    QVERIFY(playerBus.registerService(QString::fromLatin1(musicdetect::kSpotifyMprisService)));
    // Leaves the private bus clean for the next case even when this one fails.
    const auto cleanup = qScopeGuard([&playerBus] {
        playerBus.unregisterService(QString::fromLatin1(musicdetect::kSpotifyMprisService));
        playerBus.unregisterObject(QStringLiteral("/org/mpris/MediaPlayer2"));
    });

    MprisMusicSource source(clientBus, QString::fromLatin1(musicdetect::kSpotifyMprisService));
    source.setPollIntervalForTest(60 * 1000);   // signals only, unless a case says so
    QSignalSpy changed(&source, &MusicSource::nowPlayingChanged);
    source.start();
    // The first read is asynchronous; a blocking call to a player served on
    // this very thread would have dead-locked until its timeout instead.
    QTRY_VERIFY(source.nowPlaying().showable());
    QCOMPARE(source.nowPlaying().title, QStringLiteral("Song A"));
    QCOMPARE(source.nowPlaying().artists,
             (QStringList { QStringLiteral("Artist A"), QStringLiteral("Artist B") }));
    QCOMPARE(source.nowPlaying().album, QStringLiteral("Album A"));
    QCOMPARE(source.nowPlaying().trackId, QStringLiteral("/com/spotify/track/aaa"));
    QCOMPARE(musicdetect::statusTextFor(source.nowPlaying()),
             QStringLiteral("Listening to Song A by Artist A, Artist B"));

    // PropertiesChanged: pause, then a skip.
    player.setStatus(QStringLiteral("Paused"));
    QTRY_VERIFY(!source.nowPlaying().playing);
    // Only the Player interface counts: the same property names announced
    // for another interface of the object change nothing.
    QSignalSpy beforeForeign(&source, &MusicSource::nowPlayingChanged);
    player.announceOn(QStringLiteral("org.mpris.MediaPlayer2"),
                      { { QStringLiteral("PlaybackStatus"), QStringLiteral("Playing") } });
    QTest::qWait(150);
    QVERIFY(!source.nowPlaying().playing);
    QCOMPARE(beforeForeign.count(), 0);
    player.set(QStringLiteral("Playing"), QStringLiteral("Song B"), { QStringLiteral("Artist C") },
               QStringLiteral("Album B"), QStringLiteral("/com/spotify/track/bbb"));
    QTRY_COMPARE(source.nowPlaying().title, QStringLiteral("Song B"));
    QVERIFY(source.nowPlaying().playing);

    // An advertisement is read but never showable.
    player.set(QStringLiteral("Playing"), QStringLiteral("Advertisement"), {},
               QString(), QStringLiteral("/com/spotify/ad/1"));
    QTRY_COMPARE(source.nowPlaying().trackId, QStringLiteral("/com/spotify/ad/1"));
    QVERIFY(!source.nowPlaying().showable());

    // Spotify quits: nothing plays. It comes back: read again.
    player.set(QStringLiteral("Playing"), QStringLiteral("Song C"), { QStringLiteral("Artist D") },
               QString(), QStringLiteral("/com/spotify/track/ccc"), false);
    QVERIFY(playerBus.unregisterService(QString::fromLatin1(musicdetect::kSpotifyMprisService)));
    QTRY_VERIFY(source.nowPlaying().title.isEmpty());
    QVERIFY(playerBus.registerService(QString::fromLatin1(musicdetect::kSpotifyMprisService)));
    QTRY_COMPARE(source.nowPlaying().title, QStringLiteral("Song C"));
    QVERIFY(changed.count() >= 5);
    source.stop();
#endif
}

void MusicActivityTest::mprisSourceIgnoresEveryOtherPlayer()
{
#ifndef HAVE_QT_DBUS
    QSKIP("built without QtDBus");
#else
    if (m_address.isEmpty())
        QSKIP("dbus-daemon not found; the MPRIS cases need a private bus");
    QDBusConnection playerBus = QDBusConnection::connectToBus(
        QString::fromLatin1(m_address), QStringLiteral("music-test-player"));
    QDBusConnection clientBus = QDBusConnection::connectToBus(
        QString::fromLatin1(m_address), QStringLiteral("music-test-client"));
    FakeSpotifyPlayer vlc(playerBus);
    vlc.set(QStringLiteral("Playing"), QStringLiteral("A browser tab"), { QStringLiteral("Someone") },
            QString(), QStringLiteral("/org/videolan/track/1"), false);
    QVERIFY(playerBus.registerObject(QStringLiteral("/org/mpris/MediaPlayer2"), &vlc,
                                     QDBusConnection::ExportAllProperties));
    QVERIFY(playerBus.registerService(QStringLiteral("org.mpris.MediaPlayer2.vlc")));
    QVERIFY(playerBus.registerService(QStringLiteral("org.mpris.MediaPlayer2.spotify_player")));
    const auto cleanup = qScopeGuard([&playerBus] {
        playerBus.unregisterService(QStringLiteral("org.mpris.MediaPlayer2.vlc"));
        playerBus.unregisterService(QStringLiteral("org.mpris.MediaPlayer2.spotify_player"));
        playerBus.unregisterService(QString::fromLatin1(musicdetect::kSpotifyMprisService));
        playerBus.unregisterObject(QStringLiteral("/org/mpris/MediaPlayer2"));
    });

    MprisMusicSource source(clientBus, QString::fromLatin1(musicdetect::kSpotifyMprisService));
    source.setPollIntervalForTest(30);
    source.start();
    QTRY_VERIFY(source.requestsSent() >= 1);   // it did ask
    vlc.setStatus(QStringLiteral("Playing"));
    QTest::qWait(150);   // five poll periods
    QVERIFY(!source.nowPlaying().showable());
    QCOMPARE(source.nowPlaying().title, QString());
    // Nobody owns Spotify's name: the poll does not ask again...
    QCOMPARE(source.requestsSent(), 1);
    // ...until an owner appears, which is read at once.
    QVERIFY(playerBus.registerService(QString::fromLatin1(musicdetect::kSpotifyMprisService)));
    QTRY_VERIFY(source.requestsSent() >= 2);
    source.stop();
#endif
}

void MusicActivityTest::aStoppedMprisSourceIsSilent()
{
#ifndef HAVE_QT_DBUS
    QSKIP("built without QtDBus");
#else
    if (m_address.isEmpty())
        QSKIP("dbus-daemon not found; the MPRIS cases need a private bus");
    QDBusConnection playerBus = QDBusConnection::connectToBus(
        QString::fromLatin1(m_address), QStringLiteral("music-test-player"));
    QDBusConnection clientBus = QDBusConnection::connectToBus(
        QString::fromLatin1(m_address), QStringLiteral("music-test-client"));
    FakeSpotifyPlayer player(playerBus);
    player.set(QStringLiteral("Playing"), QStringLiteral("Song A"), { QStringLiteral("Artist A") },
               QString(), QStringLiteral("/com/spotify/track/aaa"), false);
    QVERIFY(playerBus.registerObject(QStringLiteral("/org/mpris/MediaPlayer2"), &player,
                                     QDBusConnection::ExportAllProperties));
    QVERIFY(playerBus.registerService(QString::fromLatin1(musicdetect::kSpotifyMprisService)));
    const auto cleanup = qScopeGuard([&playerBus] {
        playerBus.unregisterService(QString::fromLatin1(musicdetect::kSpotifyMprisService));
        playerBus.unregisterObject(QStringLiteral("/org/mpris/MediaPlayer2"));
    });

    MprisMusicSource source(clientBus, QString::fromLatin1(musicdetect::kSpotifyMprisService));
    source.setPollIntervalForTest(30);
    QSignalSpy changed(&source, &MusicSource::nowPlayingChanged);
    source.start();
    QTRY_VERIFY(source.nowPlaying().showable());
    source.stop();
    QVERIFY(!source.running());
    QVERIFY(!source.nowPlaying().showable());
    const int requests = source.requestsSent();
    const int emitted = changed.count();
    // No poll, no subscription, no owner watch after stop().
    player.set(QStringLiteral("Playing"), QStringLiteral("Song B"), { QStringLiteral("Artist B") },
               QString(), QStringLiteral("/com/spotify/track/bbb"));
    QVERIFY(playerBus.unregisterService(QString::fromLatin1(musicdetect::kSpotifyMprisService)));
    QTest::qWait(200);
    QCOMPARE(source.requestsSent(), requests);
    QCOMPARE(changed.count(), emitted);
#endif
}

QTEST_MAIN(MusicActivityTest)
#include "MusicActivityTest.moc"
