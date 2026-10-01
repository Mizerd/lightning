// The app-owned voice/audio player (app.voicePlayback), on a real
// AppController with the mock backend:
//   * a clip keeps its identity, play state and position through a room
//     switch, and holds audibility again afterwards;
//   * starting a second clip unloads the first, and a late fetch completion
//     for the first cannot take the player back;
//   * sign-out and account switch clear it, a redaction stops it;
//   * a timeline row rebuilt after the switch shows the live clip;
//   * the floating mini-player shows while the clip's room is elsewhere,
//     hides in that room, snaps to the corner it is dragged to, remembers it,
//     and moves off a corner the call PiP covers.
//
// Sources are silent WAV files from a test resolver, so no homeserver and no
// audible output are involved. MediaBridge's own fetch path is covered by its
// own suite.

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "matrix/MatrixClient.h"
#include "matrix/MockMatrixClient.h"
#include "media/MediaBridge.h"
#include "media/MediaPlaybackController.h"
#include "media/VoicePlaybackController.h"
#include "models/TimelineModel.h"
#include "threads/ThreadController.h"
#include "calls/SfuCallController.h"
#include "storage/SecretStore.h"

#include <QFile>
#include <QHash>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace {

class FakeSecretStore final : public SecretStore
{
    Q_OBJECT

public:
    explicit FakeSecretStore(QObject *parent = nullptr) : SecretStore(parent) {}
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("test"); }
    bool storeSecret(const QString &userId, const QString &key,
                     const QString &value) override
    {
        m_values.insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    QString readSecret(const QString &userId,
                       const QString &key) const override
    {
        return m_values.value(userId + QLatin1Char('/') + key);
    }
    bool deleteSecret(const QString &userId, const QString &key) override
    {
        m_values.remove(userId + QLatin1Char('/') + key);
        return true;
    }
    bool clearAccountSecrets(const QString &userId) override
    {
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it.key().startsWith(prefix))
                it = m_values.erase(it);
            else
                ++it;
        }
        return true;
    }
    QString lastError() const override { return {}; }

private:
    QHash<QString, QString> m_values;
};

const QString kAlice = QStringLiteral("@alice:one.example");
const QString kBob = QStringLiteral("@bob:two.example");
const QString kRoomA = QStringLiteral("!a:one.example");
const QString kRoomB = QStringLiteral("!b:one.example");

// A silent 16-bit mono PCM WAV of `seconds`.
bool writeSilentWav(const QString &path, int seconds)
{
    const quint32 rate = 8000;
    const quint32 dataBytes = rate * 2 * quint32(seconds);
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s.writeRawData("RIFF", 4);
    s << quint32(36 + dataBytes);
    s.writeRawData("WAVE", 4);
    s.writeRawData("fmt ", 4);
    s << quint32(16) << quint16(1) << quint16(1) << rate << (rate * 2)
      << quint16(2) << quint16(16);
    s.writeRawData("data", 4);
    s << dataBytes;
    out.append(QByteArray(int(dataBytes), '\0'));
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
}

QVariantMap voiceMeta(const QString &sender)
{
    return { { QStringLiteral("senderName"), sender },
             { QStringLiteral("isVoice"), true } };
}

} // namespace

class VoicePlaybackTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    QTemporaryDir m_media;
    QString m_clipOne;
    QString m_clipTwo;

    // Answers "mk-one"/"mk-two" with the silent files, anything else with ""
    // (a fetch still in flight).
    void installSources(VoicePlaybackController *vp)
    {
        const QString one = QUrl::fromLocalFile(m_clipOne).toString();
        const QString two = QUrl::fromLocalFile(m_clipTwo).toString();
        vp->setSourceResolverForTest([one, two](const QString &mediaKey) {
            if (mediaKey == QLatin1String("mk-one"))
                return one;
            if (mediaKey == QLatin1String("mk-two"))
                return two;
            return QString();
        });
    }

    static void signIn(AppController &app, FakeSecretStore &secrets)
    {
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(QStringLiteral("https://one.example"),
                                    kAlice, QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.settings()->saveSession(QStringLiteral("https://two.example"),
                                    kBob, QStringLiteral("BOBDEV"),
                                    QStringLiteral("bob-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        QVERIFY(m_media.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("voice-playback-test"));
        m_clipOne = m_media.filePath(QStringLiteral("one.wav"));
        m_clipTwo = m_media.filePath(QStringLiteral("two.wav"));
        QVERIFY(writeSilentWav(m_clipOne, 4));
        QVERIFY(writeSilentWav(m_clipTwo, 4));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // The tester's report: switching rooms reset a voice message to the start.
    // The clip, its paused state and its position all survive the switch, and
    // a clip that is PLAYING keeps playing and keeps the media key.
    void aClipSurvivesARoomSwitchWithItsPosition()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        QVERIFY(vp);
        installSources(vp);

        app.setCurrentRoomId(kRoomA);
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        QVERIFY(vp->active());
        QVERIFY(vp->loaded());
        QTRY_VERIFY_WITH_TIMEOUT(vp->duration() > 3000, 5000);
        vp->pause();
        QTRY_VERIFY(!vp->playing());
        QTRY_VERIFY(vp->seekable());
        vp->seek(1500);
        QTRY_VERIFY(qAbs(vp->position() - 1500) < 100);
        const qint64 before = vp->position();

        app.setCurrentRoomId(kRoomB);
        QCoreApplication::processEvents();
        QVERIFY2(vp->active(), "the room switch unloaded the clip");
        QCOMPARE(vp->eventId(), QStringLiteral("$one"));
        QCOMPARE(vp->roomId(), kRoomA);
        QCOMPARE(vp->senderName(), QStringLiteral("Alice"));
        QVERIFY(vp->loaded());
        QVERIFY2(qAbs(vp->position() - before) < 100,
                 qPrintable(QStringLiteral("position %1 after the switch, %2 "
                                           "before")
                                .arg(vp->position()).arg(before)));
        QVERIFY(!vp->playing());

        // Playing through a switch: still playing, and still the audible
        // owner so Space reaches it.
        vp->resume();
        QTRY_VERIFY(vp->playing());
        app.setCurrentRoomId(kRoomA);
        QCoreApplication::processEvents();
        QVERIFY(vp->playing());
        QCOMPARE(app.playback()->audibleOwner(), vp->ownerKey());
        vp->stop();
    }

    // One clip at a time: a second one unloads the first, and the first one's
    // fetch, landing late, cannot take the player back.
    void aSecondClipStopsTheFirst()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);

        // The first clip is still fetching when the second starts.
        vp->play(QStringLiteral("$pending"), kRoomA,
                 QStringLiteral("mk-pending"), voiceMeta(QStringLiteral("A")));
        QCOMPARE(vp->fetchState(), QStringLiteral("fetching"));

        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("B")));
        QCOMPARE(vp->eventId(), QStringLiteral("$one"));
        QVERIFY(!vp->isCurrent(QStringLiteral("$pending")));
        QCOMPARE(vp->fetchState(), QStringLiteral("idle"));
        QCOMPARE(vp->player()->source(), QUrl::fromLocalFile(m_clipOne));

        Q_EMIT app.mediaBridge()->playableMediaReady(
            QStringLiteral("full:mk-pending"));
        QCOMPARE(vp->eventId(), QStringLiteral("$one"));
        QCOMPARE(vp->player()->source(), QUrl::fromLocalFile(m_clipOne));

        // And a loaded clip is replaced, not mixed: one player, new source,
        // new audible owner.
        vp->play(QStringLiteral("$two"), kRoomB, QStringLiteral("mk-two"),
                 voiceMeta(QStringLiteral("C")));
        QCOMPARE(vp->eventId(), QStringLiteral("$two"));
        QCOMPARE(vp->roomId(), kRoomB);
        QVERIFY(!vp->isCurrent(QStringLiteral("$one")));
        QCOMPARE(vp->player()->source(), QUrl::fromLocalFile(m_clipTwo));
        QCOMPARE(app.playback()->audibleOwner(), vp->ownerKey());
        vp->stop();
        QVERIFY(!vp->active());
        QVERIFY(app.playback()->audibleOwner().isEmpty());
    }

    // Switching to another account clears the clip: its decrypted file is
    // about to be wiped with the previous account's cache.
    void anAccountSwitchClearsTheClip()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        auto *vp = app.voicePlayback();
        installSources(vp);

        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        QVERIFY(vp->active());
        app.switchToAccount(kBob);
        QVERIFY2(!vp->active(), "the clip survived an account switch");
        QVERIFY(vp->player()->source().isEmpty());
        QTRY_VERIFY(!app.accountSwitching());
    }

    // A plain sign-out (no other account to fall back to, so it does not go
    // through the account-switch path) clears the clip too.
    void aPlainSignOutClearsTheClip()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        app.settings()->setSecretStore(&secrets);
        app.settings()->saveSession(QStringLiteral("https://one.example"),
                                    kAlice, QStringLiteral("ALICEDEV"),
                                    QStringLiteral("alice-token-fixture"));
        app.switchToAccount(kAlice);
        QTRY_VERIFY(!app.accountSwitching());
        QTRY_COMPARE(app.auth()->currentUserId(), kAlice);
        auto *vp = app.voicePlayback();
        installSources(vp);

        vp->play(QStringLiteral("$two"), kRoomA, QStringLiteral("mk-two"),
                 voiceMeta(QStringLiteral("Alice")));
        QVERIFY(vp->active());
        // Paused, so the clip cannot end by itself inside the wait below and
        // pass this for the wrong reason.
        vp->pause();
        QTRY_VERIFY(!vp->playing());
        // The mock keeps the record on logout; drop it so nothing falls back.
        app.settings()->clearSessionForAccount(kAlice);
        app.auth()->logout();
        QTRY_VERIFY2_WITH_TIMEOUT(!vp->active(), "the clip survived sign-out",
                                  2000);
        QVERIFY(vp->player()->source().isEmpty());
    }

    // The remembered media level and speed apply to the app-owned player as
    // they did to every card's own, and follow a change.
    void thePlayerFollowsTheRememberedVolumeAndSpeed()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        app.settings()->setMediaVolume(0.3);
        app.settings()->setMediaPlaybackRate(1.5);
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        vp->pause();
        QVERIFY(vp->audioOutput());
        QVERIFY(qAbs(vp->audioOutput()->volume() - 0.3f) < 0.001f);
        QCOMPARE(vp->player()->playbackRate(), 1.5);
        app.settings()->setMediaVolume(0.6);
        app.settings()->setMediaPlaybackRate(0.75);
        QVERIFY(qAbs(vp->audioOutput()->volume() - 0.6f) < 0.001f);
        QCOMPARE(vp->player()->playbackRate(), 0.75);
        // The shared volume control writes `userUnmuted` on its output.
        QVERIFY(vp->audioOutput()->setProperty("userUnmuted", true));
        vp->stop();
    }

    // A redaction of the playing event stops it; one of another event does
    // not.
    void aRedactionStopsTheClip()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *client = app.findChild<MatrixClient *>();
        QVERIFY(client);

        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        Q_EMIT client->eventRedacted(kRoomA, QStringLiteral("$other"));
        QVERIFY(vp->active());
        Q_EMIT client->eventRedacted(kRoomB, QStringLiteral("$one"));
        QVERIFY2(vp->active(), "a redaction in another room stopped the clip");
        Q_EMIT client->eventRedacted(kRoomA, QStringLiteral("$one"));
        QVERIFY(!vp->active());
    }

    // The Rust backend never emits MatrixClient::eventRedacted: a redaction
    // reaches the app only as a rewritten timeline row. Driven here through
    // the timeline models themselves (their own redaction slot, the reload of
    // a reopened room), never through the client signal the Rust SDK does not
    // send:
    //   * a row rewritten in the open room's timeline;
    //   * a row rewritten in the open thread's timeline;
    //   * a clip whose event was already redacted when its room is reopened.
    void aRedactionTheRustSdkOnlyShowsInATimelineStillStopsTheClip()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *client = app.findChild<MatrixClient *>();
        QVERIFY(client);
        const QString general = QStringLiteral("!general:mock.local");
        const QString devs = QStringLiteral("!devs:mock.local");

        app.setCurrentRoomId(general);
        TimelineModel *tl = app.timeline();
        QTRY_VERIFY(tl->rowCount() > 4);
        // Two ordinary loaded events, and the thread root.
        QStringList plain;
        QString threadRoot;
        for (int row = 0; row < tl->rowCount(); ++row) {
            const QModelIndex idx = tl->index(row);
            const QString id = tl->eventIdAt(row);
            if (id.isEmpty() || idx.data(TimelineModel::RedactedRole).toBool())
                continue;
            if (idx.data(TimelineModel::IsThreadRootRole).toBool())
                threadRoot = id;
            else if (plain.size() < 2)
                plain << id;
        }
        QCOMPARE(plain.size(), 2);
        QVERIFY(!threadRoot.isEmpty());

        // 1. Rewritten in the open room's timeline.
        vp->play(plain[0], general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        QVERIFY(vp->active());
        QVERIFY(QMetaObject::invokeMethod(tl, "onEventRedacted",
                                          Q_ARG(QString, general),
                                          Q_ARG(QString, plain[0])));
        QVERIFY2(!vp->active(),
                 "a redaction the room timeline shows did not stop the clip");

        // 2. Rewritten in the open thread's timeline.
        app.thread()->openThread(general, threadRoot);
        TimelineModel *threadModel = app.thread()->model();
        QString reply;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (int row = 0; row < threadModel->rowCount(); ++row) {
                    const QString id = threadModel->eventIdAt(row);
                    if (!id.isEmpty() && id != threadRoot) {
                        reply = id;
                        return true;
                    }
                }
                return false;
            }(), 3000);
        vp->play(reply, general, QStringLiteral("mk-two"),
                 { { QStringLiteral("senderName"), QStringLiteral("Bob") },
                   { QStringLiteral("isVoice"), true },
                   { QStringLiteral("threadRootId"), threadRoot } });
        QVERIFY(vp->active());
        QVERIFY(QMetaObject::invokeMethod(threadModel, "onEventRedacted",
                                          Q_ARG(QString, threadModel->roomId()),
                                          Q_ARG(QString, reply)));
        QVERIFY2(!vp->active(),
                 "a redaction the thread timeline shows did not stop the clip");

        // 3. Already redacted when its room is reopened. The redaction lands
        // while nothing plays it; the clip is then playing from another room.
        client->redactEvent(general, plain[1], QString());
        app.setCurrentRoomId(devs);
        vp->play(plain[1], general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        QVERIFY(vp->active());
        app.setCurrentRoomId(general);
        // Checked at once, not after a wait: the mock later rewrites rows on
        // its own schedule, which the Rust SDK need not do, so only the
        // reload itself may count here.
        QVERIFY2(!vp->active(),
                 "reopening the room did not notice the clip was redacted");
    }

    // A call going live pauses the clip once. A clip the user resumes during
    // the call keeps playing through the call's later state changes; the
    // next call pauses it again.
    void aCallPausesTheClipOnlyOnItsWayIn()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *call = app.groupCall();
        QVERIFY(call);

        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        QTRY_VERIFY(vp->playing());

        call->setCallStateForTest(SfuCallController::State::Connected);
        QVERIFY(call->active());
        Q_EMIT call->stateChanged();
        QTRY_VERIFY2(!vp->playing(), "the call did not pause the clip");

        vp->resume();
        QTRY_VERIFY(vp->playing());
        Q_EMIT call->stateChanged(); // e.g. a reconnect within the same call
        QTest::qWait(150);
        QVERIFY2(vp->playing(),
                 "a clip resumed mid-call was paused again by the same call");

        call->setCallStateForTest(SfuCallController::State::Idle);
        Q_EMIT call->stateChanged();
        call->setCallStateForTest(SfuCallController::State::Connected);
        Q_EMIT call->stateChanged();
        QTRY_VERIFY2(!vp->playing(), "the next call did not pause the clip");
        call->setCallStateForTest(SfuCallController::State::Idle);
        Q_EMIT call->stateChanged();
        vp->stop();
    }

    // The QML side: a row rebuilt after a room switch (a new AudioPlayerCard
    // for the same event) shows the live clip at its position, and a row for
    // another event shows idle.
    void aRebuiltRowShowsTheLiveClip()
    {
        QQmlApplicationEngine engine;
        auto *app = new AppController(AppController::MockBackend, false,
                                      &engine);
        engine.rootContext()->setContextProperty(QStringLiteral("app"), app);
        auto *vp = app->voicePlayback();
        installSources(vp);
        // The card's Play goes in-process only when the bridge is supported.
        auto *mock = app->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->setSupportsMediaBridgeForTest(true);
        QVERIFY(app->mediaBridge()->supported());

        QQmlComponent cardComponent(&engine);
        cardComponent.setData(R"(
import QtQuick
import MatrixClient
AudioPlayerCard { width: 360; isVoice: true; durationMs: 4000 }
)", QUrl(QStringLiteral("qrc:/voicePlaybackCard.qml")));
        QVERIFY2(cardComponent.errors().isEmpty(),
                 qPrintable(cardComponent.errorString()));
        const auto makeCard = [&](const QString &eventId) {
            QObject *card = cardComponent.create(engine.rootContext());
            if (card) {
                card->setProperty("eventId", eventId);
                card->setProperty("mediaKey", QStringLiteral("mk-one"));
                card->setProperty("roomId", kRoomA);
                card->setProperty("senderName", QStringLiteral("Alice"));
            }
            return std::unique_ptr<QObject>(card);
        };

        app->setCurrentRoomId(kRoomA);
        auto first = makeCard(QStringLiteral("$one"));
        QVERIFY(first);
        QVERIFY(!first->property("isCurrent").toBool());
        // The row's own Play.
        QVERIFY(QMetaObject::invokeMethod(first.get(), "togglePlay"));
        QVERIFY(first->property("isCurrent").toBool());
        QCOMPARE(vp->senderName(), QStringLiteral("Alice"));
        QTRY_VERIFY_WITH_TIMEOUT(vp->duration() > 3000, 5000);
        vp->pause();
        QTRY_VERIFY(vp->seekable());
        vp->seek(2000);
        QTRY_VERIFY(qAbs(vp->position() - 2000) < 100);

        // The switch destroys the row; the clip lives on.
        first.reset();
        app->setCurrentRoomId(kRoomB);
        QVERIFY(vp->active());

        // Back in the room, a rebuilt row for the same event is live again at
        // the same position; a row for another event is idle.
        app->setCurrentRoomId(kRoomA);
        auto again = makeCard(QStringLiteral("$one"));
        auto other = makeCard(QStringLiteral("$other"));
        QVERIFY2(again->property("isCurrent").toBool(),
                 "the rebuilt row lost the playing clip");
        QVERIFY(qAbs(again->property("livePosition").toDouble() - 2000) < 100);
        QVERIFY(!other->property("isCurrent").toBool());
        QCOMPARE(other->property("livePosition").toDouble(), 0.0);
        vp->stop();
        QVERIFY(!again->property("isCurrent").toBool());
    }

    // The mini-player: shown while the clip's room is elsewhere, hidden in
    // that room; dragged towards a corner it snaps there and the corner is
    // remembered by the next launch; a PiP over the corner moves it.
    void theMiniPlayerFollowsTheRoomAndRemembersItsCorner()
    {
        QQuickWindow window;
        window.resize(900, 700);
        QQmlApplicationEngine engine;
        auto *app = new AppController(AppController::MockBackend, false,
                                      &engine);
        engine.rootContext()->setContextProperty(QStringLiteral("app"), app);
        app->showMain();
        QCOMPARE(app->currentScreen(), AppController::MainScreen);
        QCOMPARE(app->settings()->voiceMiniPlayerCorner(), 0);
        auto *vp = app->voicePlayback();
        installSources(vp);

        QQmlComponent component(&engine);
        // A stand-in for Main.qml's corner-prompt column, on the overlay as
        // there, hidden until the end.
        component.setData(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient
Item {
    width: 900; height: 700
    // Main.qml binds `parent: Overlay.overlay`; here the scene has no window
    // at creation, so the test places it once it has one.
    function placePrompts() { prompts.parent = Overlay.overlay }
    VoiceMiniPlayer { objectName: "host"; anchors.fill: parent }
    Rectangle {
        id: prompts
        objectName: "cornerPromptHost"
        visible: false
        x: 600; y: 400; width: 280; height: 280
    }
}
)", QUrl(QStringLiteral("qrc:/voiceMiniPlayer.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> scene(component.create(engine.rootContext()));
        auto *sceneItem = qobject_cast<QQuickItem *>(scene.get());
        QVERIFY(sceneItem);
        sceneItem->setParentItem(window.contentItem());
        auto *hostItem = sceneItem->findChild<QQuickItem *>(
            QStringLiteral("host"));
        QVERIFY(hostItem);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(QMetaObject::invokeMethod(sceneItem, "placePrompts"));
        auto *card = hostItem->findChild<QQuickItem *>(
            QStringLiteral("voiceMiniPlayer"));
        QVERIFY(card);
        QVERIFY(!card->isVisible());

        // Playing in the room on screen: no card, the row is the player.
        app->setCurrentRoomId(kRoomA);
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 { { QStringLiteral("senderName"), QStringLiteral("Alice") },
                   { QStringLiteral("isVoice"), true },
                   { QStringLiteral("roomName"), QStringLiteral("Lounge") } });
        vp->pause();
        QTest::qWait(250);
        QVERIFY2(!card->isVisible(), "the card showed in the clip's own room");

        // Leaving the room shows it, naming the sender and the room.
        app->setCurrentRoomId(kRoomB);
        QTRY_VERIFY(card->isVisible());
        QTRY_COMPARE(card->opacity(), 1.0);
        auto *sender = card->findChild<QObject *>(
            QStringLiteral("voiceMiniPlayerSender"));
        auto *room = card->findChild<QObject *>(
            QStringLiteral("voiceMiniPlayerRoom"));
        QVERIFY(sender && room);
        QCOMPARE(sender->property("text").toString(), QStringLiteral("Alice"));
        QCOMPARE(room->property("text").toString(), QStringLiteral("Lounge"));
        // Resting bottom-right, inside the safe area's margin.
        QTRY_COMPARE(card->x(), 900.0 - card->width() - 12.0);
        QTRY_COMPARE(card->y(), 700.0 - card->height() - 12.0);

        // Back in the clip's room: hidden again.
        app->setCurrentRoomId(kRoomA);
        QTRY_VERIFY(!card->isVisible());
        app->setCurrentRoomId(QString()); // Home
        QTRY_VERIFY(card->isVisible());

        // A real drag towards the top-left snaps there and is remembered.
        const QPointF start = card->mapToScene(
            QPointF(card->width() / 2, 6));
        QTest::mousePress(&window, Qt::LeftButton, {}, start.toPoint());
        auto *dragHandler = card->findChild<QObject *>(
            QStringLiteral("voiceMiniPlayerDrag"));
        QVERIFY(dragHandler);
        for (int step = 1; step <= 10; ++step) {
            QTest::mouseMove(&window,
                             (start - QPointF(60.0 * step, 50.0 * step))
                                 .toPoint());
            if (step == 2)
                QVERIFY(dragHandler->property("active").toBool());
        }
        QTest::mouseRelease(&window, Qt::LeftButton, {},
                            (start - QPointF(600, 500)).toPoint());
        QTRY_COMPARE(app->settings()->voiceMiniPlayerCorner(), 3);
        QTRY_COMPARE(card->x(), 12.0);
        QTRY_COMPARE(card->y(), 12.0);
        QVERIFY2(app->currentRoomId().isEmpty(),
                 "the drag also counted as a click that opened the room");

        // A PiP window (or the corner prompts) over that corner moves the card
        // to the nearest free one (the other corner on the same side first:
        // bottom-left), without forgetting the choice.
        QVERIFY(hostItem->setProperty(
            "avoidRects", QVariantList{ QRectF(0, 0, 400, 300) }));
        QTRY_COMPARE(card->y(), 700.0 - card->height() - 12.0);
        QCOMPARE(card->x(), 12.0);
        QCOMPARE(app->settings()->voiceMiniPlayerCorner(), 3);
        QVERIFY(hostItem->setProperty("avoidRects", QVariantList{}));
        QTRY_COMPARE(card->y(), 12.0);

        // The window's corner prompts (an incoming call, verification...)
        // own the bottom-right: a card remembered there moves up to the
        // top-right while they show, and back when they go.
        app->settings()->setVoiceMiniPlayerCorner(0);
        QTRY_COMPARE(card->x(), 900.0 - card->width() - 12.0);
        QTRY_COMPARE(card->y(), 700.0 - card->height() - 12.0);
        auto *prompts = scene->findChild<QQuickItem *>(
            QStringLiteral("cornerPromptHost"));
        QVERIFY(prompts);
        QVERIFY2(prompts->parentItem() && prompts->parentItem() != sceneItem,
                 "the stand-in never reached the overlay");
        prompts->setVisible(true);
        QTRY_COMPARE(card->y(), 12.0);
        QCOMPARE(card->x(), 900.0 - card->width() - 12.0);
        prompts->setVisible(false);
        QTRY_COMPARE(card->y(), 700.0 - card->height() - 12.0);
        app->settings()->setVoiceMiniPlayerCorner(3);
        QTRY_COMPARE(card->y(), 12.0);

        // A clip played from a thread panel: its room being open is not
        // enough, its thread panel must be. A closed panel (a room switch
        // closes it, and so can the reader) leaves only the card.
        app->setCurrentRoomId(kRoomA);
        vp->play(QStringLiteral("$reply"), kRoomA, QStringLiteral("mk-two"),
                 { { QStringLiteral("senderName"), QStringLiteral("Bob") },
                   { QStringLiteral("isVoice"), true },
                   { QStringLiteral("threadRootId"), QStringLiteral("$root") } });
        vp->pause();
        QVERIFY(!app->thread()->active());
        // `wanted`, not isVisible(): a card fading out from the previous step
        // is still visible for 160 ms and would pass this for the wrong
        // reason.
        QVERIFY2(hostItem->property("wanted").toBool(),
                 "a thread clip with its panel closed has no UI at all");
        QTRY_COMPARE(card->opacity(), 1.0);
        app->thread()->openThread(kRoomA, QStringLiteral("$root"));
        QVERIFY(app->thread()->active());
        QVERIFY2(!hostItem->property("wanted").toBool(),
                 "the card showed beside the thread panel playing it");
        QTRY_VERIFY(!card->isVisible());
        app->thread()->close();
        QVERIFY(hostItem->property("wanted").toBool());
        QTRY_COMPARE(card->opacity(), 1.0);

        // Stop from the card clears the clip and the card.
        auto *stopButton = card->findChild<QObject *>(
            QStringLiteral("voiceMiniPlayerStop"));
        QVERIFY(stopButton);
        QVERIFY(QMetaObject::invokeMethod(stopButton, "clicked"));
        QVERIFY(!vp->active());
        QTRY_VERIFY(!card->isVisible());

        // The next launch reads the remembered corner; an unknown stored value
        // reads as the default rather than the nearest corner.
        {
            AppController next(AppController::MockBackend);
            QCOMPARE(next.settings()->voiceMiniPlayerCorner(), 3);
            QSettings raw;
            raw.setValue(QStringLiteral("media/miniPlayerCorner"), 7);
            raw.sync();
            QCOMPARE(next.settings()->voiceMiniPlayerCorner(), 0);
        }
        scene.reset();
    }
};

QTEST_MAIN(VoicePlaybackTest)
#include "VoicePlaybackTest.moc"
