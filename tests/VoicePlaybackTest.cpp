// The app-owned voice/audio player (app.voicePlayback), on a real
// AppController with the mock backend:
//   * a clip keeps its identity, play state and position through a room
//     switch, and holds audibility again afterwards;
//   * starting a second clip unloads the first, and a late fetch completion
//     for the first cannot take the player back;
//   * sign-out and account switch clear it, a redaction stops it;
//   * a timeline row rebuilt after the switch shows the live clip;
//   * auto-play-next follows the clip's OWN timeline, through a room switch;
//   * a call pauses the clip and its end resumes it; Space still reaches a
//     clip paused before a room switch; voice and file speeds are separate;
//   * an audio file's tags outlive its play; an idle row starts mid-clip;
//   * the popped-out player shows wherever the clip's row is not (Settings
//     included), the docked strip by default, and both choices persist.
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
#include "media/VoiceRecorder.h"
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

// A recorder that starts without a microphone (VoiceOwnershipTest has the
// full contract).
class FakeRecorder final : public VoiceRecorder
{
public:
    bool isRecording = false;
    bool recording() const override { return isRecording; }
    bool processing() const override { return false; }
    bool start() override
    {
        if (isRecording)
            return false;
        isRecording = true;
        return true;
    }
    void cancel() override { isRecording = false; }
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

// A silent WAV carrying RIFF INFO tags (INAM title, IART artist), which the
// media backend reports as the file's title and artist.
bool writeTaggedWav(const QString &path, int seconds, const QByteArray &title,
                    const QByteArray &artist)
{
    const quint32 rate = 8000;
    const quint32 dataBytes = rate * 2 * quint32(seconds);
    const auto chunk = [](const char *id, QByteArray text) {
        text.append('\0');
        if (text.size() % 2)
            text.append('\0');
        QByteArray out(id, 4);
        QDataStream s(&out, QIODevice::Append);
        s.setByteOrder(QDataStream::LittleEndian);
        s << quint32(text.size());
        out.append(text);
        return out;
    };
    QByteArray info("INFO");
    info.append(chunk("INAM", title));
    info.append(chunk("IART", artist));
    QByteArray list("LIST");
    {
        QDataStream s(&list, QIODevice::Append);
        s.setByteOrder(QDataStream::LittleEndian);
        s << quint32(info.size());
    }
    list.append(info);
    QByteArray body;
    QDataStream s(&body, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s.writeRawData("WAVE", 4);
    s.writeRawData("fmt ", 4);
    s << quint32(16) << quint16(1) << quint16(1) << rate << (rate * 2)
      << quint16(2) << quint16(16);
    s.writeRawData(list.constData(), int(list.size()));
    s.writeRawData("data", 4);
    s << dataBytes;
    body.append(QByteArray(int(dataBytes), '\0'));
    QByteArray out("RIFF");
    {
        QDataStream h(&out, QIODevice::Append);
        h.setByteOrder(QDataStream::LittleEndian);
        h << quint32(body.size());
    }
    out.append(body);
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
}

QVariantMap voiceMeta(const QString &sender)
{
    return { { QStringLiteral("senderName"), sender },
             { QStringLiteral("isVoice"), true } };
}

TimelineEvent voiceEvent(const QString &room, const QString &id,
                         const QString &mediaKey, bool voice = true)
{
    TimelineEvent e;
    e.type = TimelineEvent::Audio;
    e.roomId = room;
    e.eventId = id;
    e.sender = QStringLiteral("@alice:mock.local");
    e.body = QStringLiteral("clip.ogg");
    e.mediaKey = mediaKey;
    e.mediaSourceAvailable = true;
    e.mediaIsVoice = voice;
    return e;
}

TimelineEvent plainEvent(const QString &room, const QString &id,
                         TimelineEvent::Type type)
{
    TimelineEvent e;
    e.type = type;
    e.roomId = room;
    e.eventId = id;
    e.sender = QStringLiteral("@alice:mock.local");
    e.body = QStringLiteral("x");
    return e;
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

    // Auto-play next (GitHub #36). Voice messages only, in time order; rows
    // that draw no message are skipped, any real message ends the run.
    void aFinishedVoiceMessageStartsTheNextOne()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *client = app.findChild<MatrixClient *>();
        QVERIFY(client);
        const QString general = QStringLiteral("!general:mock.local");
        app.setCurrentRoomId(general);
        TimelineModel *tl = app.timeline();
        QTRY_VERIFY(tl->rowCount() > 4);

        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$v1"), QStringLiteral("mk-one")));
        Q_EMIT client->eventAppended(general,
            plainEvent(general, QStringLiteral("$state"),
                       TimelineEvent::StateChange));
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$v2"), QStringLiteral("mk-two")));
        Q_EMIT client->eventAppended(general,
            plainEvent(general, QStringLiteral("$text"),
                       TimelineEvent::TextMessage));
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$v3"), QStringLiteral("mk-one")));
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$file"),
                       QStringLiteral("mk-two"), /*voice=*/false));

        // The lookup: skips the state row, stops at the text and at a plain
        // audio file, and refuses another room.
        QCOMPARE(tl->nextVoiceMessage(general, QStringLiteral("$v1"))
                     .value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$v2"));
        QVERIFY(tl->nextVoiceMessage(general, QStringLiteral("$v2")).isEmpty());
        QVERIFY(tl->nextVoiceMessage(general, QStringLiteral("$v3")).isEmpty());
        QVERIFY(tl->nextVoiceMessage(QStringLiteral("!devs:mock.local"),
                                     QStringLiteral("$v1")).isEmpty());
        QVERIFY(tl->nextVoiceMessage(general, QStringLiteral("$gone")).isEmpty());

        // The wiring: the end of the clip starts the next one.
        vp->play(QStringLiteral("$v1"), general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        QVERIFY(vp->player());
        Q_EMIT vp->player()->mediaStatusChanged(QMediaPlayer::EndOfMedia);
        QTRY_COMPARE(vp->eventId(), QStringLiteral("$v2"));
        QVERIFY(vp->isVoice());

        // ...and the run ends at the text message: nothing starts.
        Q_EMIT vp->player()->mediaStatusChanged(QMediaPlayer::EndOfMedia);
        QTRY_VERIFY(!vp->active());
    }

    // B111: a clip the chain started that cannot be played (here: invalid
    // media) used to end the run silently. It is skipped, at most three in a
    // row; a clip the READER started that fails is not, and a real message
    // still ends the run.
    void anUnplayableClipInTheChainIsSkipped()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *client = app.findChild<MatrixClient *>();
        QVERIFY(client);
        const QString general = QStringLiteral("!general:mock.local");
        app.setCurrentRoomId(general);
        QTRY_VERIFY(app.timeline()->rowCount() > 4);
        const auto append = [&](const TimelineEvent &e) {
            Q_EMIT client->eventAppended(general, e);
        };
        // The resolver answers "" for any key but mk-one/mk-two: a fetch in
        // flight, which the invalid-media report below then fails.
        append(voiceEvent(general, QStringLiteral("$v1"), QStringLiteral("mk-one")));
        append(voiceEvent(general, QStringLiteral("$bad"), QStringLiteral("mk-bad")));
        append(voiceEvent(general, QStringLiteral("$v2"), QStringLiteral("mk-two")));
        append(plainEvent(general, QStringLiteral("$text"),
                          TimelineEvent::TextMessage));
        append(voiceEvent(general, QStringLiteral("$w1"), QStringLiteral("mk-one")));
        for (int i = 1; i <= 4; ++i)
            append(voiceEvent(general, QStringLiteral("$b%1").arg(i),
                              QStringLiteral("mk-b%1").arg(i)));
        append(voiceEvent(general, QStringLiteral("$w2"), QStringLiteral("mk-two")));
        append(plainEvent(general, QStringLiteral("$text2"),
                          TimelineEvent::TextMessage));
        append(voiceEvent(general, QStringLiteral("$c1"), QStringLiteral("mk-one")));
        append(voiceEvent(general, QStringLiteral("$c2"), QStringLiteral("mk-c2")));
        append(voiceEvent(general, QStringLiteral("$c3"), QStringLiteral("mk-two")));
        const auto failCurrent = [vp] {
            Q_EMIT vp->player()->mediaStatusChanged(QMediaPlayer::InvalidMedia);
        };

        vp->play(QStringLiteral("$v1"), general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        QVERIFY(vp->player());
        Q_EMIT vp->player()->mediaStatusChanged(QMediaPlayer::EndOfMedia);
        QTRY_COMPARE(vp->eventId(), QStringLiteral("$bad"));
        failCurrent();
        QTRY_COMPARE(vp->eventId(), QStringLiteral("$v2"));
        // The text message still ends the run.
        Q_EMIT vp->player()->mediaStatusChanged(QMediaPlayer::EndOfMedia);
        QTRY_VERIFY(!vp->active());

        // The reader's own choice failing stays on screen, failed.
        vp->play(QStringLiteral("$bad"), general, QStringLiteral("mk-bad"),
                 voiceMeta(QStringLiteral("A")));
        failCurrent();
        QTest::qWait(100);
        QCOMPARE(vp->eventId(), QStringLiteral("$bad"));
        QCOMPARE(vp->fetchState(), QStringLiteral("failed"));

        // Bounded: three skips, then the fourth broken clip stays.
        vp->play(QStringLiteral("$w1"), general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        Q_EMIT vp->player()->mediaStatusChanged(QMediaPlayer::EndOfMedia);
        QTRY_COMPARE(vp->eventId(), QStringLiteral("$b1"));
        for (int i = 2; i <= 4; ++i) {
            failCurrent();
            QTRY_COMPARE(vp->eventId(), QStringLiteral("$b%1").arg(i));
        }
        failCurrent();
        QTest::qWait(100);
        QCOMPARE(vp->eventId(), QStringLiteral("$b4"));
        QCOMPARE(vp->fetchState(), QStringLiteral("failed"));

        // A clip the chain started and the reader then stopped is the
        // reader's when they play it again: its failure is not skipped (the
        // chain's mark used to outlive the stop and move on to $c3).
        vp->play(QStringLiteral("$c1"), general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        Q_EMIT vp->player()->mediaStatusChanged(QMediaPlayer::EndOfMedia);
        QTRY_COMPARE(vp->eventId(), QStringLiteral("$c2"));
        vp->stop();
        vp->play(QStringLiteral("$c2"), general, QStringLiteral("mk-c2"),
                 voiceMeta(QStringLiteral("A")));
        failCurrent();
        QTest::qWait(100);
        QCOMPARE(vp->eventId(), QStringLiteral("$c2"));
        QCOMPARE(vp->fetchState(), QStringLiteral("failed"));
        vp->stop();
    }

    // A clip the reader paused or stopped never reaches its end, so it never
    // advances; a finished audio FILE and a thread clip do not advance either.
    void onlyANaturalEndOfAVoiceMessageAdvances()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *client = app.findChild<MatrixClient *>();
        QVERIFY(client);
        const QString general = QStringLiteral("!general:mock.local");
        app.setCurrentRoomId(general);
        QTRY_VERIFY(app.timeline()->rowCount() > 4);
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$v1"), QStringLiteral("mk-one")));
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$v2"), QStringLiteral("mk-two")));

        vp->play(QStringLiteral("$v1"), general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        vp->pause();
        QTest::qWait(50);
        QCOMPARE(vp->eventId(), QStringLiteral("$v1"));
        vp->stop();
        QTest::qWait(50);
        QVERIFY(!vp->active());

        // An audio file (isVoice false) ending does not chain.
        Q_EMIT vp->clipFinished(QStringLiteral("$v1"), general, QString(), false);
        // Nor does a clip from a thread whose run nobody knows (no thread
        // timeline loaded, no snapshot): it is the THREAD's run, never the
        // room's.
        Q_EMIT vp->clipFinished(QStringLiteral("$v1"), general,
                                QStringLiteral("$root"), true);
        QVERIFY(!vp->active());

        // The positive control: the same signal for a main-timeline voice clip
        // does start $v2, so the two refusals above are not vacuous.
        Q_EMIT vp->clipFinished(QStringLiteral("$v1"), general, QString(), true);
        QCOMPARE(vp->eventId(), QStringLiteral("$v2"));
    }

    // The clip auto-play-next would start is prefetched: one at a time, it
    // follows the playing clip, and goes when the clip stops or the room
    // changes. (Whether MediaBridge actually fetches needs a homeserver; the
    // mock bridge is unsupported, so this pins the decision, not the bytes.)
    void theNextVoiceMessageIsPrefetchedAndDroppedWithTheClip()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *client = app.findChild<MatrixClient *>();
        QVERIFY(client);
        const QString general = QStringLiteral("!general:mock.local");
        app.setCurrentRoomId(general);
        QTRY_VERIFY(app.timeline()->rowCount() > 4);
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$v1"), QStringLiteral("mk-one")));
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$v2"), QStringLiteral("mk-two")));
        Q_EMIT client->eventAppended(general,
            plainEvent(general, QStringLiteral("$text"),
                       TimelineEvent::TextMessage));

        QVERIFY(app.voicePrefetchKey().isEmpty());
        vp->play(QStringLiteral("$v1"), general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        QTRY_COMPARE(app.voicePrefetchKey(), QStringLiteral("mk-two"));
        // The last of the run has nothing to prefetch.
        vp->play(QStringLiteral("$v2"), general, QStringLiteral("mk-two"),
                 voiceMeta(QStringLiteral("A")));
        QTRY_VERIFY(app.voicePrefetchKey().isEmpty());
        // Stop drops it.
        vp->play(QStringLiteral("$v1"), general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        QTRY_COMPARE(app.voicePrefetchKey(), QStringLiteral("mk-two"));
        vp->stop();
        QTRY_VERIFY(app.voicePrefetchKey().isEmpty());
        // A room switch keeps it while the clip keeps playing: the run plays
        // on from its snapshot (design-media B3), so its next clip is still
        // the one to have ready.
        vp->play(QStringLiteral("$v1"), general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        QTRY_COMPARE(app.voicePrefetchKey(), QStringLiteral("mk-two"));
        app.setCurrentRoomId(QStringLiteral("!devs:mock.local"));
        QTest::qWait(100);
        QCOMPARE(app.voicePrefetchKey(), QStringLiteral("mk-two"));
        // A thread clip whose thread is not loaded prefetches nothing: its
        // run is the thread's, never the room's.
        vp->stop();
        app.setCurrentRoomId(general);
        vp->play(QStringLiteral("$t1"), general, QStringLiteral("mk-one"),
                 { { QStringLiteral("isVoice"), true },
                   { QStringLiteral("threadRootId"), QStringLiteral("$root") } });
        QTest::qWait(100);
        QVERIFY(app.voicePrefetchKey().isEmpty());

        // The chain advance must not drop the prefetch the next clip is about
        // to use: play($v2) stops $v1 first (currentChanged with nothing
        // loaded), and only then registers interest in $v2's bytes.
        // (The room switches above reloaded the timeline: seed it again.)
        vp->stop();
        app.setCurrentRoomId(general);
        QTRY_VERIFY(app.timeline()->rowCount() > 4);
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$w1"), QStringLiteral("mk-one")));
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$w2"), QStringLiteral("mk-two")));
        Q_EMIT client->eventAppended(general,
            plainEvent(general, QStringLiteral("$wtext"),
                       TimelineEvent::TextMessage));
        QTRY_VERIFY(app.voicePrefetchKey().isEmpty());
        const int dropsBefore = app.voicePrefetchDropsForTest();
        vp->play(QStringLiteral("$w1"), general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        QTRY_COMPARE(app.voicePrefetchKey(), QStringLiteral("mk-two"));
        Q_EMIT vp->player()->mediaStatusChanged(QMediaPlayer::EndOfMedia);
        QTRY_COMPARE(vp->eventId(), QStringLiteral("$w2"));
        // $w2 is the last of its run: nothing left to prefetch, and moving off
        // "mk-two" was NOT a drop of anything the new clip needs.
        QTRY_VERIFY(app.voicePrefetchKey().isEmpty());
        QTest::qWait(100);
        QCOMPARE(app.voicePrefetchDropsForTest(), dropsBefore);
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
        app.settings()->setVoicePlaybackRate(1.5);
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        vp->pause();
        QVERIFY(vp->audioOutput());
        QVERIFY(qAbs(vp->audioOutput()->volume() - 0.3f) < 0.001f);
        QCOMPARE(vp->player()->playbackRate(), 1.5);
        app.settings()->setMediaVolume(0.6);
        app.settings()->setVoicePlaybackRate(0.75);
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

    // B109: the microphone picked up a clip that kept playing through a
    // recording. Starting one pauses the clip (not stops: it can be resumed),
    // and a clip still fetching no longer starts by itself mid-recording.
    void startingARecordingPausesTheClip()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *rec = new FakeRecorder;
        app.setVoiceRecorderForTest(rec);

        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        QTRY_VERIFY(vp->playing());
        QVERIFY(app.startVoiceRecording(QStringLiteral("room")));
        QTRY_VERIFY2(!vp->playing(), "the clip played on through the recording");
        QVERIFY(vp->active());
        QCOMPARE(vp->eventId(), QStringLiteral("$one"));
        rec->cancel();

        // The resolver answers "" for this key: a fetch still in flight.
        vp->play(QStringLiteral("$pending"), kRoomA,
                 QStringLiteral("mk-pending"),
                 voiceMeta(QStringLiteral("Alice")));
        QCOMPARE(vp->fetchState(), QStringLiteral("fetching"));
        QVERIFY(app.startVoiceRecording(QStringLiteral("room")));
        QCOMPARE(vp->fetchState(), QStringLiteral("idle"));
        QVERIFY(vp->active());
        QVERIFY(!vp->playing());
        rec->cancel();
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

    // B107, then design-media P4/U5: a short waveform used to draw over only
    // part of the row (3 px a bar, left-aligned), leaving dead space after it,
    // and an idle row could not be seeked at all. The bars now stretch over
    // the whole row, a tap maps over that width, and on an idle row it starts
    // the clip at that point.
    void aWaveformFillsTheRowAndAnIdleTapStartsThere()
    {
        QQuickWindow window;
        window.resize(420, 200);
        QQmlApplicationEngine engine;
        auto *app = new AppController(AppController::MockBackend, false,
                                      &engine);
        engine.rootContext()->setContextProperty(QStringLiteral("app"), app);
        auto *mock = app->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->setSupportsMediaBridgeForTest(true);
        auto *vp = app->voicePlayback();
        installSources(vp);
        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import MatrixClient
AudioPlayerCard { width: 360; isVoice: true; durationMs: 4000
                  eventId: "$wave"; roomId: "!a:one.example"; mediaKey: "mk-one" }
)", QUrl(QStringLiteral("qrc:/voiceWaveformCard.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> card(component.create(engine.rootContext()));
        auto *cardItem = qobject_cast<QQuickItem *>(card.get());
        QVERIFY(cardItem);
        QVariantList buckets;
        for (int i = 0; i < 20; ++i)
            buckets.append(50);
        cardItem->setProperty("waveform", buckets);
        cardItem->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto *wave = cardItem->findChild<QQuickItem *>(
            QStringLiteral("voiceSeekWave"));
        QVERIFY(wave);
        QTRY_VERIFY(wave->isVisible());
        qreal right = 0;
        int bars = 0;
        const auto kids = wave->childItems();
        for (QQuickItem *bar : kids) {
            if (bar->width() <= 0 || bar->height() <= 0)
                continue;
            ++bars;
            right = qMax(right, bar->x() + bar->width());
        }
        // Slim bars across the whole width, each sampling its bucket.
        QCOMPARE(bars, int((wave->width() + 1) / 3));
        QVERIFY2(right >= wave->width() * 0.97,
                 qPrintable(QStringLiteral("20 bars end at %1 of %2 px")
                                .arg(right).arg(wave->width())));

        // A click at half the width of an idle row starts the clip there.
        auto *seek = cardItem->findChild<QQuickItem *>(
            QStringLiteral("audioWaveRow"));
        QVERIFY(seek);
        const QPointF at = seek->mapToScene(
            QPointF(seek->width() * 0.5, seek->height() / 2));
        QTest::mouseClick(&window, Qt::LeftButton, {}, at.toPoint());
        QTRY_COMPARE(vp->eventId(), QStringLiteral("$wave"));
        QTRY_VERIFY_WITH_TIMEOUT(vp->loaded() && vp->seekable(), 5000);
        QTRY_VERIFY2(qAbs(vp->position() - 2000) < 400,
                     qPrintable(QString::number(vp->position())));
        vp->stop();
    }

    // The popped-out player (design-media U1/B2/U2), on the real controller:
    // docked by default (no card), popped out it shows wherever the clip's row
    // is not on screen (in the clip's own room too, and in Settings), and the
    // choice and the card's place persist to the next launch; a corrupt
    // stored place reads as "never placed".
    void thePopOutCardShowsWhereverTheRowIsNotAndPersists()
    {
        QQuickWindow window;
        window.resize(900, 700);
        QQmlApplicationEngine engine;
        auto *app = new AppController(AppController::MockBackend, false,
                                      &engine);
        engine.rootContext()->setContextProperty(QStringLiteral("app"), app);
        app->showMain();
        QVERIFY2(app->settings()->voicePlayerDocked(),
                 "the player is not docked by default");
        auto *vp = app->voicePlayback();
        installSources(vp);

        QQmlComponent component(&engine);
        component.setData(R"(
import QtQuick
import MatrixClient
Item {
    width: 900; height: 700
    VoiceMiniPlayer { objectName: "host"; anchors.fill: parent }
}
)", QUrl(QStringLiteral("qrc:/voiceMiniPlayer.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> scene(component.create(engine.rootContext()));
        auto *sceneItem = qobject_cast<QQuickItem *>(scene.get());
        QVERIFY(sceneItem);
        sceneItem->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *host = sceneItem->findChild<QQuickItem *>(QStringLiteral("host"));
        auto *card = host->findChild<QQuickItem *>(
            QStringLiteral("voiceMiniPlayer"));
        QVERIFY(host && card);

        app->setCurrentRoomId(kRoomA);
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 { { QStringLiteral("senderName"), QStringLiteral("Alice") },
                   { QStringLiteral("isVoice"), true },
                   { QStringLiteral("roomName"), QStringLiteral("Lounge") } });
        vp->pause();
        QTest::qWait(250);
        QVERIFY2(!card->isVisible(), "the card showed while docked");

        app->settings()->setVoicePlayerDocked(false);
        // In the clip's own room, its row not on screen: the card (U2).
        QTRY_COMPARE(card->opacity(), 1.0);
        auto *room = card->findChild<QObject *>(
            QStringLiteral("voiceMiniPlayerRoom"));
        QVERIFY(room);
        QCOMPARE(room->property("text").toString(),
                 QStringLiteral("Voice message · Lounge"));
        // A row that says it is on screen hides it; its going away shows it.
        {
            QObject row;
            vp->reportRowVisible(&row, QStringLiteral("$one"), true);
            QVERIFY(vp->rowOnScreen());
            QVERIFY(!host->property("wanted").toBool());
            // Settings hides every row: the card again (B2).
            app->showSettings();
            QVERIFY(host->property("wanted").toBool());
            app->showMain();
            QVERIFY(!host->property("wanted").toBool());
        }
        QVERIFY2(!vp->rowOnScreen(), "a destroyed row still counted");
        QVERIFY(host->property("wanted").toBool());

        // A drag and drop is remembered as a place relative to the window.
        const QPointF from = card->mapToScene(QPointF(14, 14));
        QTest::mousePress(&window, Qt::LeftButton, {}, from.toPoint());
        for (int step = 1; step <= 10; ++step)
            QTest::mouseMove(&window,
                             (from - QPointF(30.0 * step, 30.0 * step))
                                 .toPoint());
        QTest::mouseRelease(&window, Qt::LeftButton, {},
                            (from - QPointF(300, 300)).toPoint());
        const QRectF placed = app->settings()->voiceMiniPlayerGeometry();
        QVERIFY2(placed.width() > 0, "the drop was not remembered");
        QVERIFY(placed.x() >= 0 && placed.x() <= 1);
        QVERIFY(placed.y() >= 0 && placed.y() <= 1);
        QVERIFY2(app->currentRoomId() == kRoomA,
                 "the drag also counted as a click on the title");

        vp->stop();
        QTRY_VERIFY(!card->isVisible());

        // The next launch reads both choices; a corrupt place is "unplaced",
        // never off screen.
        {
            AppController next(AppController::MockBackend);
            QCOMPARE(next.settings()->voicePlayerDocked(), false);
            QCOMPARE(next.settings()->voiceMiniPlayerGeometry(), placed);
            QSettings raw;
            raw.setValue(QStringLiteral("media/miniPlayerGeometry"),
                         QRectF(qInf(), 0.5, 300, 90));
            raw.sync();
            QVERIFY(!next.settings()->voiceMiniPlayerGeometry().isValid());
            raw.setValue(QStringLiteral("media/miniPlayerGeometry"),
                         QRectF(7, -2, 300, 90));
            raw.sync();
            const QRectF bounded = next.settings()->voiceMiniPlayerGeometry();
            QCOMPARE(bounded.x(), 1.0);
            QCOMPARE(bounded.y(), 0.0);
        }
        scene.reset();
    }

    // design-media B3: the chain played on only while its room was open; it
    // stopped after the first clip once the reader left. It follows the clip's
    // own run now, wherever the reader is.
    void theChainPlaysOnAfterLeavingTheRoom()
    {
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *client = app.findChild<MatrixClient *>();
        QVERIFY(client);
        const QString general = QStringLiteral("!general:mock.local");
        app.setCurrentRoomId(general);
        QTRY_VERIFY(app.timeline()->rowCount() > 4);
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$v1"), QStringLiteral("mk-one")));
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$v2"), QStringLiteral("mk-two")));
        Q_EMIT client->eventAppended(general,
            voiceEvent(general, QStringLiteral("$v3"), QStringLiteral("mk-one")));
        Q_EMIT client->eventAppended(general,
            plainEvent(general, QStringLiteral("$text"),
                       TimelineEvent::TextMessage));

        vp->play(QStringLiteral("$v1"), general, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        QTRY_COMPARE(vp->upNext().size(), 2);
        QVERIFY(vp->hasNext());
        // The reader leaves for another room straight away.
        app.setCurrentRoomId(QStringLiteral("!devs:mock.local"));
        QTRY_COMPARE(app.timeline()->roomId(),
                     QStringLiteral("!devs:mock.local"));
        QTest::qWait(50);
        QCOMPARE(vp->upNext().size(), 2);

        Q_EMIT vp->player()->mediaStatusChanged(QMediaPlayer::EndOfMedia);
        QTRY_COMPARE(vp->eventId(), QStringLiteral("$v2"));
        QCOMPARE(vp->roomId(), general);
        QCOMPARE(vp->upNext().size(), 1);
        // The bar's Next skips along the same run.
        QVERIFY(vp->playNext());
        QCOMPARE(vp->eventId(), QStringLiteral("$v3"));
        QVERIFY(!vp->hasNext());
        // ...and the run ends at the text message.
        Q_EMIT vp->player()->mediaStatusChanged(QMediaPlayer::EndOfMedia);
        QTRY_VERIFY(!vp->active());
    }

    // A redaction of a voice message still waiting in the run takes it out.
    void aRedactedClipLeavesTheRun()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *client = app.findChild<MatrixClient *>();
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("A")));
        vp->setUpNext({ QVariantMap{ { QStringLiteral("eventId"),
                                       QStringLiteral("$two") },
                                     { QStringLiteral("mediaKey"),
                                       QStringLiteral("mk-two") } } });
        QVERIFY(vp->hasNext());
        Q_EMIT client->eventRedacted(kRoomA, QStringLiteral("$two"));
        QVERIFY(!vp->hasNext());
        vp->stop();
    }

    // design-media U3: a clip a call paused stayed paused after the hang-up,
    // with no reason shown. The call's pause is marked, and its end resumes
    // the clip, unless the reader touched it meanwhile.
    void theEndOfACallResumesTheClipItPaused()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *call = app.groupCall();
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        QTRY_VERIFY(vp->playing());
        call->setCallStateForTest(SfuCallController::State::Connected);
        Q_EMIT call->stateChanged();
        QTRY_VERIFY(!vp->playing());
        QVERIFY2(vp->pausedForCall(), "the call's pause is not marked");
        call->setCallStateForTest(SfuCallController::State::Idle);
        Q_EMIT call->stateChanged();
        QTRY_VERIFY2(vp->playing(), "the hang-up did not resume the clip");
        QVERIFY(!vp->pausedForCall());

        // Paused by the reader during the call: the hang-up leaves it alone.
        call->setCallStateForTest(SfuCallController::State::Connected);
        Q_EMIT call->stateChanged();
        QTRY_VERIFY(!vp->playing());
        vp->resume();
        QTRY_VERIFY(vp->playing());
        vp->pause();
        QTRY_VERIFY(!vp->playing());
        QVERIFY(!vp->pausedForCall());
        call->setCallStateForTest(SfuCallController::State::Idle);
        Q_EMIT call->stateChanged();
        QTest::qWait(200);
        QVERIFY2(!vp->playing(), "the hang-up undid the reader's own pause");
        vp->stop();
    }

    // design-media U5: Space did nothing after a room switch if the clip was
    // PAUSED at the time: only a playing clip took audibility back.
    void aPausedClipStillAnswersSpaceAfterARoomSwitch()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        app.setCurrentRoomId(kRoomA);
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        QTRY_VERIFY(vp->playing());
        vp->pause();
        QTRY_VERIFY(!vp->playing());
        app.setCurrentRoomId(kRoomB);
        QCoreApplication::processEvents();
        QCOMPARE(app.playback()->audibleOwner(), vp->ownerKey());
        // Space, as Main.qml delivers it.
        app.playback()->requestTogglePlayPause();
        QTRY_VERIFY2(vp->playing(), "Space did not resume the paused clip");
        vp->stop();
    }

    // design-media U3: speed is per kind. Voice messages play at the
    // remembered voice speed; an audio file starts at 1x and its speed is
    // its own, never written over the voice speed.
    void voiceAndFileSpeedsAreSeparate()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        app.settings()->setVoicePlaybackRate(1.5);
        vp->play(QStringLiteral("$file"), kRoomA, QStringLiteral("mk-one"),
                 { { QStringLiteral("isVoice"), false },
                   { QStringLiteral("filename"), QStringLiteral("song.wav") } });
        QVERIFY(vp->player());
        QCOMPARE(vp->rate(), 1.0);
        QCOMPARE(vp->player()->playbackRate(), 1.0);
        vp->setRate(2.0);
        QCOMPARE(vp->player()->playbackRate(), 2.0);
        QCOMPARE(app.settings()->voicePlaybackRate(), 1.5);
        vp->play(QStringLiteral("$voice"), kRoomA, QStringLiteral("mk-two"),
                 voiceMeta(QStringLiteral("A")));
        QCOMPARE(vp->rate(), 1.5);
        QCOMPARE(vp->player()->playbackRate(), 1.5);
        // The next file starts at 1x again.
        vp->play(QStringLiteral("$file2"), kRoomA, QStringLiteral("mk-one"),
                 { { QStringLiteral("isVoice"), false } });
        QCOMPARE(vp->player()->playbackRate(), 1.0);
        vp->stop();
        // An earlier shared speed carries over to voice once.
        {
            QSettings raw;
            raw.remove(QStringLiteral("media/voicePlaybackRate"));
            raw.setValue(QStringLiteral("media/playbackRate"), 1.25);
            raw.sync();
            AppController fresh(AppController::MockBackend);
            QCOMPARE(fresh.settings()->voicePlaybackRate(), 1.25);
            // A later change of the shared key no longer moves it.
            fresh.settings()->setMediaPlaybackRate(2.0);
            QCOMPARE(fresh.settings()->voicePlaybackRate(), 1.25);
        }
    }

    // design-media B6: an audio file's tags (title, artist) came only from
    // the live player, so they appeared and vanished with playback. They are
    // remembered per file for the session (RAM) and outlive the play.
    void anAudioFilesTagsOutliveItsPlay()
    {
        const QString tagged = m_media.filePath(QStringLiteral("tagged.wav"));
        QVERIFY(writeTaggedWav(tagged, 2, "Short Test Song", "The Band"));
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        const QString url = QUrl::fromLocalFile(tagged).toString();
        vp->setSourceResolverForTest([url](const QString &key) {
            return key == QLatin1String("mk-tagged") ? url : QString();
        });
        QVERIFY(vp->clipInfo(QStringLiteral("mk-tagged")).isEmpty());
        vp->play(QStringLiteral("$tagged"), kRoomA, QStringLiteral("mk-tagged"),
                 { { QStringLiteral("isVoice"), false },
                   { QStringLiteral("filename"), QStringLiteral("t.wav") } });
        QTRY_COMPARE_WITH_TIMEOUT(vp->trackTitle(),
                                  QStringLiteral("Short Test Song"), 5000);
        QCOMPARE(vp->trackArtist(), QStringLiteral("The Band"));
        vp->stop();
        const QVariantMap info = vp->clipInfo(QStringLiteral("mk-tagged"));
        QCOMPARE(info.value(QStringLiteral("title")).toString(),
                 QStringLiteral("Short Test Song"));
        QCOMPARE(info.value(QStringLiteral("artist")).toString(),
                 QStringLiteral("The Band"));
    }

    // design-media U5: a seek on an idle row starts the clip there; a seek
    // while the file is still being fetched is kept until it opens.
    void aClipCanStartPartWayIn()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        QVariantMap meta = voiceMeta(QStringLiteral("A"));
        meta.insert(QStringLiteral("startAtMs"), 2500);
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"), meta);
        QTRY_VERIFY_WITH_TIMEOUT(vp->loaded() && vp->seekable(), 5000);
        QTRY_VERIFY2(qAbs(vp->position() - 2500) < 300,
                     qPrintable(QString::number(vp->position())));
        vp->stop();
        // Still fetching ("" from the resolver): the seek waits.
        vp->play(QStringLiteral("$pending"), kRoomA,
                 QStringLiteral("mk-pending"), voiceMeta(QStringLiteral("A")));
        QCOMPARE(vp->fetchState(), QStringLiteral("fetching"));
        vp->seek(1200);
        QCOMPARE(vp->position(), qint64(1200));
        vp->stop();
    }

    // Review round 1 (L3): what a session learned about its files (tags and
    // cover art, clipInfo) went with neither a sign-out nor an account
    // switch, so the next account's rows could show them.
    void anAccountSwitchForgetsTheFilesTags()
    {
        const QString tagged = m_media.filePath(QStringLiteral("tagged2.wav"));
        QVERIFY(writeTaggedWav(tagged, 2, "Private Title", "Private Artist"));
        AppController app(AppController::MockBackend);
        FakeSecretStore secrets;
        signIn(app, secrets);
        auto *vp = app.voicePlayback();
        const QString url = QUrl::fromLocalFile(tagged).toString();
        vp->setSourceResolverForTest([url](const QString &key) {
            return key == QLatin1String("mk-tagged") ? url : QString();
        });
        vp->play(QStringLiteral("$tagged"), kRoomA, QStringLiteral("mk-tagged"),
                 { { QStringLiteral("isVoice"), false } });
        QTRY_COMPARE_WITH_TIMEOUT(vp->trackTitle(),
                                  QStringLiteral("Private Title"), 5000);
        vp->stop();
        QVERIFY(!vp->clipInfo(QStringLiteral("mk-tagged")).isEmpty());
        app.switchToAccount(kBob);
        QVERIFY2(vp->clipInfo(QStringLiteral("mk-tagged")).isEmpty(),
                 "the previous account's file tags survived the switch");
        QTRY_VERIFY(!app.accountSwitching());
    }

    // Review round 1 (L4): leaving (or being removed from) the clip's room
    // stops the clip and drops its run, whether or not that room is open.
    void leavingTheClipsRoomStopsIt()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        auto *client = app.findChild<MatrixClient *>();
        QVERIFY(client);
        app.setCurrentRoomId(kRoomB);
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        vp->setUpNext({ QVariantMap{
            { QStringLiteral("eventId"), QStringLiteral("$two") },
            { QStringLiteral("mediaKey"), QStringLiteral("mk-two") } } });
        // Another room going: nothing happens.
        Q_EMIT client->roomMembershipLost(QStringLiteral("!other:x"),
                                          QStringLiteral("left"), QString());
        QVERIFY(vp->active());
        Q_EMIT client->roomMembershipLost(kRoomA, QStringLiteral("left"),
                                          QStringLiteral("A"));
        QVERIFY2(!vp->active(), "a clip from a left room kept playing");
        QVERIFY(!vp->hasNext());
    }

    // Review round 1 (L8): the visible row is reachable, so the window's
    // "focus the player" shortcut can land on its play button.
    void theVisibleRowIsExposed()
    {
        AppController app(AppController::MockBackend);
        auto *vp = app.voicePlayback();
        installSources(vp);
        vp->play(QStringLiteral("$one"), kRoomA, QStringLiteral("mk-one"),
                 voiceMeta(QStringLiteral("Alice")));
        QCOMPARE(vp->visibleRow(), nullptr);
        {
            QObject row;
            vp->reportRowVisible(&row, QStringLiteral("$one"), true);
            QCOMPARE(vp->visibleRow(), &row);
        }
        QCOMPARE(vp->visibleRow(), nullptr);
        vp->stop();
    }
};

QTEST_MAIN(VoicePlaybackTest)
#include "VoicePlaybackTest.moc"
