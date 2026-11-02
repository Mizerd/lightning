#include "calls/CallSoundPlayer.h"
#include "calls/CallSoundRebuild.h"
#include "calls/PulseServerProbe.h"

#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <map>
#include <memory>

Q_LOGGING_CATEGORY(lcCallSound, "lightning.calls.sound")

namespace {
#ifdef Q_OS_LINUX
int listenAt(const QByteArray &path)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= qsizetype(sizeof address.sun_path))
        return -1;
    memcpy(address.sun_path, path.constData(), size_t(path.size()));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0
        || ::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}
#endif

struct FakeEngine {
    bool dead = false;
};
struct FakeEnginePool {
    std::map<QString, std::weak_ptr<FakeEngine>> engines;
    std::shared_ptr<FakeEngine> engineFor(const QString &outputId)
    {
        if (std::shared_ptr<FakeEngine> live = engines[outputId].lock())
            return live;
        auto fresh = std::make_shared<FakeEngine>();
        engines[outputId] = fresh;
        return fresh;
    }
};
struct FakeEffect {
    std::shared_ptr<FakeEngine> engine;
};
}

class OutputListSource : public QObject
{
    Q_OBJECT
Q_SIGNALS:
    void changed();
};

class CallSoundPlayerTest : public QObject
{
    Q_OBJECT

private slots:
    void aReloadNeverHandsANewCueTheEngineThatDied()
    {
        const QStringList sounds{QStringLiteral("ring"),
                                 QStringLiteral("connected"),
                                 QStringLiteral("hangup")};
        const QString output = QStringLiteral("rdp-endpoint");

        {
            FakeEnginePool pool;
            QHash<QString, FakeEffect *> effects;
            for (const QString &sound : sounds)
                effects.insert(sound, new FakeEffect{pool.engineFor(output)});
            effects.value(sounds.first())->engine->dead = true;
            QList<FakeEffect *> later;
            for (const QString &sound : sounds) {
                later << effects.take(sound);
                effects.insert(sound, new FakeEffect{pool.engineFor(output)});
            }
            qDeleteAll(later);
            QVERIFY2(effects.value(sounds.first())->engine->dead,
                     "the model does not reproduce the pooled dead engine");
            qDeleteAll(effects);
        }

        FakeEnginePool pool;
        QHash<QString, FakeEffect *> effects;
        for (const QString &sound : sounds)
            effects.insert(sound, new FakeEffect{pool.engineFor(output)});
        effects.value(sounds.first())->engine->dead = true;
        const std::weak_ptr<FakeEngine> watch =
            effects.value(sounds.first())->engine;

        int destroyed = 0;
        const int created = callsound::replaceEveryEffect(
            effects, sounds,
            [&](FakeEffect *old) {
                ++destroyed;
                delete old;
            },
            [&](const QString &sound) {
                effects.insert(sound, new FakeEffect{pool.engineFor(output)});
                return true;
            });
        QCOMPARE(destroyed, sounds.size());
        QCOMPARE(created, sounds.size());
        QCOMPARE(effects.size(), sounds.size());
        QVERIFY2(watch.expired(), "an old effect outlived the reload");
        for (const QString &sound : sounds) {
            QVERIFY2(!effects.value(sound)->engine->dead,
                     qPrintable(sound + QStringLiteral(
                                    " was handed the engine that died")));
        }
        qDeleteAll(effects);
    }

    void theQSoundEffectCuesReplaceEveryEffectBeforeCreatingAny()
    {
        QFile file(QStringLiteral(SOUNDS_DIR "/../../src/calls/CallSoundPlayer.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString code = QString::fromUtf8(file.readAll());
        const int begin = code.indexOf(QStringLiteral("class QSoundEffectCues"));
        const int end = code.indexOf(QStringLiteral("\nclass AudioSinkOutput"), begin);
        QVERIFY2(begin >= 0 && end > begin, "no QSoundEffectCues");
        const QString cues = code.mid(begin, end - begin);
        const int reload = cues.indexOf(QStringLiteral("int reloadUnusable() override"));
        const int stop = cues.indexOf(QStringLiteral("void stopAll() override"), reload);
        QVERIFY(reload >= 0 && stop > reload);
        const QString body = cues.mid(reload, stop - reload);
        QVERIFY2(body.contains(QStringLiteral("callsound::replaceEveryEffect(")),
                 "the reload does not destroy every old effect first");
        QVERIFY2(body.contains(QStringLiteral("delete old;")),
                 "the reload does not free the old effects at once");
        QVERIFY2(!cues.contains(QStringLiteral("deleteLater")),
                 "an old effect is kept alive past the reload");
        const int route = cues.indexOf(QStringLiteral("static void route("));
        const int routeEnd = cues.indexOf(QStringLiteral("\n    }\n"), route);
        QVERIFY(route >= 0 && routeEnd > route);
        const QString routing = cues.mid(route, routeEnd - route);
        const int modern = routing.indexOf(
            QStringLiteral("#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)"));
        const int legacy = routing.indexOf(QStringLiteral("#else"), modern);
        QVERIFY2(modern >= 0 && legacy > modern,
                 "the cues pin an output on every Qt");
        QVERIFY2(!routing.mid(modern, legacy - modern)
                      .contains(QStringLiteral("resolveOutput(")),
                 "from Qt 6.10 the cues pin the default output by its ID");
    }

    void aStuckSoundThreadReportsNothingPlayable()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
true);
        player.markReadyForTest(QStringLiteral("ring"));
        QVERIFY(player.canPlay(QStringLiteral("ring")));

        QSemaphore quick;
        player.postForTest([&quick] { quick.release(); });
        QVERIFY(quick.tryAcquire(1, 2000));
        QTest::qWait(2300);
        QVERIFY(player.canPlay(QStringLiteral("ring")));

        QSemaphore unblock;
        player.postForTest([&unblock] { unblock.acquire(); });
        QVERIFY(player.canPlay(QStringLiteral("ring")));
        QTRY_VERIFY_WITH_TIMEOUT(!player.canPlay(QStringLiteral("ring")), 4000);
        player.postForTest([] {});
        QVERIFY(!player.canPlay(QStringLiteral("ring")));

        unblock.release();
        QTRY_VERIFY_WITH_TIMEOUT(player.canPlay(QStringLiteral("ring")), 4000);
    }

    void onTheGuiThreadReadinessIsTheLoadedState()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
false);
        QVERIFY(!player.canPlay(QStringLiteral("ring")));
        bool ran = false;
        player.postForTest([&ran] { ran = true; });
        QVERIFY(ran);
        player.markReadyForTest(QStringLiteral("ring"));
        QVERIFY(player.canPlay(QStringLiteral("ring")));
    }

    void aChangedOutputListReloadsTheCuesOncePerBurst()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
false);
        player.setOutputChangeDebounceForTest(150, 600);
        QCOMPARE(player.soundReloadsForTest(), 0);

        for (int i = 0; i < 5; ++i) {
            player.audioOutputsChanged();
            QTest::qWait(30);
        }
        QCOMPARE(player.soundReloadsForTest(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(player.soundReloadsForTest(), 1, 2000);
        QTest::qWait(400);
        QCOMPARE(player.soundReloadsForTest(), 1);

        player.audioOutputsChanged();
        QTRY_COMPARE_WITH_TIMEOUT(player.soundReloadsForTest(), 2, 2000);

        QElapsedTimer flapping;
        flapping.start();
        while (flapping.elapsed() < 1500) {
            player.audioOutputsChanged();
            QTest::qWait(100);
        }
        QVERIFY2(player.soundReloadsForTest() >= 3,
                 "a burst that never went quiet was never answered");
        QVERIFY2(player.soundReloadsForTest() <= 5,
                 qPrintable(QStringLiteral("%1 reloads for 1.5 s of flapping")
                                .arg(player.soundReloadsForTest() - 2)));
    }

    void theReloadFollowsTheSignalItIsHanded()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
false);
        player.setOutputChangeDebounceForTest(100, 500);
        OutputListSource source;
        bool anyOutput = false;
        int asked = 0;
        player.followOutputChanges(&source, &OutputListSource::changed,
                                   [&] {
                                       ++asked;
                                       return anyOutput;
                                   });

        for (int i = 0; i < 3; ++i) {
            Q_EMIT source.changed();
            QTest::qWait(20);
        }
        QTRY_COMPARE_WITH_TIMEOUT(player.soundReloadsForTest(), 1, 2000);
        QCOMPARE(asked, 1);
        QVERIFY(!player.lastReloadHadOutputForTest());

        anyOutput = true;
        Q_EMIT source.changed();
        QTRY_COMPARE_WITH_TIMEOUT(player.soundReloadsForTest(), 2, 2000);
        QCOMPARE(asked, 2);
        QVERIFY(player.lastReloadHadOutputForTest());
    }

    void theApplicationDrivesTheReloadFromTheDeviceController()
    {
        QFile file(QStringLiteral(SOUNDS_DIR "/../../src/app/AppController.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString code = QString::fromUtf8(file.readAll());
        const int begin =
            code.indexOf(QStringLiteral("void AppController::enableCallSounds()"));
        QVERIFY(begin >= 0);
        const int end =
            code.indexOf(QStringLiteral("\nvoid AppController::"), begin + 1);
        QVERIFY(end > begin);
        const QString body = code.mid(begin, end - begin);
        QVERIFY2(body.contains(QStringLiteral("followOutputChanges("))
                     && body.contains(QStringLiteral(
                         "&CallDeviceController::audioOutputsChanged")),
                 "the call sounds do not follow the device controller's "
                 "output-change signal");
    }

    void theNotificationChimesAreBundledInTheCallSoundFormat()
    {
        const QStringList known = CallSoundPlayer::knownSounds();
        for (const char *name : { "message", "mention" }) {
            const QString sound = QString::fromLatin1(name);
            QVERIFY2(known.contains(sound),
                     qPrintable(sound + " is not a known sound"));
            QFile file(QStringLiteral(SOUNDS_DIR "/") + sound
                       + QStringLiteral(".wav"));
            QVERIFY2(file.open(QIODevice::ReadOnly),
                     qPrintable(file.fileName() + " is missing"));
            const QByteArray wav = file.readAll();
            QVERIFY(wav.size() > 44);
            QCOMPARE(wav.left(4), QByteArrayLiteral("RIFF"));
            auto u16 = [&wav](int at) {
                return quint16(quint8(wav[at]) | (quint8(wav[at + 1]) << 8));
            };
            auto u32 = [&wav](int at) {
                return quint32(quint8(wav[at]) | (quint8(wav[at + 1]) << 8)
                               | (quint8(wav[at + 2]) << 16)
                               | (quint32(quint8(wav[at + 3])) << 24));
            };
            QCOMPARE(u16(22), quint16(2));
            QCOMPARE(u32(24), quint32(48000));
            QCOMPARE(u16(34), quint16(16));
            const double seconds = double(wav.size() - 44) / (48000.0 * 4.0);
            QVERIFY2(seconds >= 0.15 && seconds <= 0.5,
                     qPrintable(sound + QStringLiteral(" lasts %1 s")
                                    .arg(seconds)));
        }
    }

    void onLinuxFromQt610NoCueIsVoicedByQSoundEffect()
    {
        QFile file(QStringLiteral(SOUNDS_DIR "/../../src/calls/CallSoundPlayer.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString code = QString::fromUtf8(file.readAll());
        const QRegularExpression construct(
            QStringLiteral(R"(m_effects\s*=\s*new\s+(\w+)\s*\()"));
        QStringList built;
        for (auto it = construct.globalMatch(code); it.hasNext();)
            built << it.next().captured(1);
        QVERIFY2(!built.isEmpty(), "the player constructs no effects");
        QVERIFY2(!built.contains(QStringLiteral("CallSoundEffects")),
                 "the player constructs one implementation for every Qt "
                 "version (QSoundEffect, which Qt 6.10+ frees on the audio "
                 "thread)");
        QVERIFY2(code.contains(QStringLiteral(
                     "#if defined(Q_OS_LINUX) && QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)\n"
                     "constexpr bool kCuesMixed = true;\n"
                     "#else\n"
                     "constexpr bool kCuesMixed = false;\n"
                     "#endif")),
                 "the mixer is not chosen for exactly Linux with Qt 6.10+");
        QVERIFY2(code.contains(QStringLiteral(
                     "if constexpr (kCuesMixed)\n"
                     "        m_effects = new MixerCues(m_readiness);\n"
                     "    else\n"
                     "        m_effects = new QSoundEffectCues(m_readiness);")),
                 "the cues are not chosen by that constant");
        const int begin = code.indexOf(QStringLiteral("class AudioSinkOutput"));
        const int end = code.indexOf(QStringLiteral("\nvoid CallSoundEffects::setReady"), begin);
        QVERIFY(begin >= 0 && end > begin);
        const QRegularExpression usesQSoundEffect(
            QStringLiteral(R"(\bQSoundEffect\s*(\*|::|\(|\{)|new\s+QSoundEffect\b)"));
        QVERIFY2(!usesQSoundEffect.match(code.mid(begin, end - begin)).hasMatch(),
                 "the mixed cues use QSoundEffect");
    }

    void thePulseServerSocketIsFoundAsLibpulseFindsIt()
    {
        using lightning::calls::pulseServerSockets;
        QCOMPARE(pulseServerSockets("unix:/run/flatpak/pulse/native", "/x", "/y"),
                 QList<QByteArray>{"/run/flatpak/pulse/native"});
        QCOMPARE(pulseServerSockets("{abc}unix:/a/native tcp:host:4713 /b/native", {}, "/y"),
                 (QList<QByteArray>{"/a/native", "/b/native"}));
        QCOMPARE(pulseServerSockets("tcp:host:4713", "/x", "/y"), QList<QByteArray>{});
        QCOMPARE(pulseServerSockets({}, "/rt/pulse", "/run/user/1"),
                 QList<QByteArray>{"/rt/pulse/native"});
        QCOMPARE(pulseServerSockets("  ", {}, "/run/user/1"),
                 QList<QByteArray>{"/run/user/1/pulse/native"});
        QCOMPARE(pulseServerSockets({}, {}, {}), QList<QByteArray>{});
    }

    void aSocketAnswersOnlyWhileSomethingListensOnIt()
    {
#ifndef Q_OS_LINUX
        QSKIP("the PulseAudio gate is Linux only");
#else
        using lightning::calls::unixSocketListening;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray path = QFile::encodeName(dir.filePath(QStringLiteral("native")));
        const QByteArray bound = QFile::encodeName(dir.filePath(QStringLiteral("sandbox")));
        QVERIFY(!unixSocketListening(path));
        int server = listenAt(path);
        QVERIFY(server >= 0);
        QVERIFY(unixSocketListening(path));
        QCOMPARE(::link(path.constData(), bound.constData()), 0);
        QVERIFY(unixSocketListening(bound));
        ::close(server);
        QVERIFY(!unixSocketListening(path));
        QVERIFY(!unixSocketListening(bound));
        QCOMPARE(::unlink(path.constData()), 0);
        server = listenAt(path);
        QVERIFY(server >= 0);
        QVERIFY(unixSocketListening(path));
        QVERIFY2(!unixSocketListening(bound), "the old inode answered");
        ::close(server);
#endif
    }

    void aThreadIsFoundByItsNameWhileItRuns()
    {
#ifndef Q_OS_LINUX
        QSKIP("the PulseAudio gate is Linux only");
#else
        using lightning::calls::threadNamedRunning;
        QVERIFY(!threadNamedRunning("lt-gate-probe"));
        QSemaphore release;
        QScopedPointer<QThread> thread(QThread::create([&release] { release.acquire(); }));
        thread->setObjectName(QStringLiteral("lt-gate-probe"));
        thread->start();
        const auto stopThread = qScopeGuard([&] {
            release.release();
            thread->wait(5000);
        });
        QTRY_VERIFY(threadNamedRunning("lt-gate-probe"));
        release.release();
        QVERIFY(thread->wait(5000));
        QTRY_VERIFY(!threadNamedRunning("lt-gate-probe"));

        using lightning::calls::qtPulseThreadRunning;
        QCOMPARE(qtPulseThreadRunning(), threadNamedRunning("QPulseAudioEngi"));
        QSemaphore releaseQt;
        QScopedPointer<QThread> qtLike(QThread::create([&releaseQt] { releaseQt.acquire(); }));
        qtLike->setObjectName(QStringLiteral("QPulseAudioEngi"));
        qtLike->start();
        const auto stopQtLike = qScopeGuard([&] {
            releaseQt.release();
            qtLike->wait(5000);
        });
        QTRY_VERIFY(qtPulseThreadRunning());
        releaseQt.release();
        QVERIFY(qtLike->wait(5000));
#endif
    }

    void theMixedCuesGateOnTheSocketAndOnQtsConnection()
    {
        QFile file(QStringLiteral(SOUNDS_DIR "/../../src/calls/CallSoundPlayer.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString code = QString::fromUtf8(file.readAll());
        const int begin = code.indexOf(QStringLiteral("class MixerCues"));
        QVERIFY(begin >= 0);
        const QString cues = code.mid(begin);
        const int gate = cues.indexOf(QStringLiteral("if (qtAudioBackendMayBePulse()) {"));
        const int hook =
            cues.indexOf(QStringLiteral("h.serverReachable = lightning::calls::PulseGate("), gate);
        QVERIFY2(gate >= 0 && hook > gate, "the gate is not set behind the backend check");
        const QString body = cues.mid(hook, 300);
        const int qt = body.indexOf(QStringLiteral("lightning::calls::qtPulseThreadRunning()"));
        const int socket =
            body.indexOf(QStringLiteral("lightning::calls::pulseServerReachable()"));
        QVERIFY2(qt > 0 && socket > qt,
                 "the gate does not ask Qt's connection first and the socket second");
        QVERIFY2(cues.contains(QStringLiteral(
                     "const bool pulse = name == QLatin1String(\"PulseAudio\");")),
                 "the backend check is not keyed on Qt's own name for PulseAudio");
    }

    void theGateAsksQtFirstAndSaysOnceWhenItIsGone()
    {
        bool qt = false;
        bool socket = true;
        int socketAsked = 0;
        int said = 0;
        qint64 now = 0;
        lightning::calls::PulseGate gate([&] { return qt; },
                                         [&] { ++socketAsked; return socket; },
                                         [&] { return now; }, [&] { ++said; }, 30000);
        for (now = 0; now < 29000; now += 1000)
            QVERIFY(!gate());
        QCOMPARE(socketAsked, 0);
        QCOMPARE(said, 0);
        now = 30000;
        QVERIFY(!gate());
        QCOMPARE(said, 1);
        now = 90000;
        QVERIFY(!gate());
        QCOMPARE(said, 1);
        qt = true;
        QVERIFY(gate());
        QCOMPARE(socketAsked, 1);
        socket = false;
        QVERIFY(!gate());
        lightning::calls::PulseGate fresh([&] { return qt; }, [] { return true; },
                                          [&] { return now; }, [&] { ++said; }, 30000);
        qt = false;
        now = 0;
        QVERIFY(!fresh());
        now = 20000;
        qt = true;
        QVERIFY(fresh());
        qt = false;
        now = 40000;
        QVERIFY(!fresh());
        now = 60000;
        QVERIFY(!fresh());
        QCOMPARE(said, 1);
        QFile file(QStringLiteral(SOUNDS_DIR "/../../src/calls/CallSoundPlayer.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString code = QString::fromUtf8(file.readAll());
        const int hook = code.indexOf(QStringLiteral("lightning::calls::PulseGate("));
        QVERIFY(hook > 0);
        const QString sayGone = code.mid(hook, 900);
        QVERIFY(sayGone.contains(
            QStringLiteral("\"call sounds: Qt's PulseAudio connection did not come \"")));
        QVERIFY(sayGone.contains(
            QStringLiteral("\"back; call sounds stay off until Lightning restarts\"")));
        QVERIFY2(sayGone.contains(QStringLiteral("static bool said = false;"))
                     && sayGone.contains(QStringLiteral("if (!std::exchange(said, true)) {")),
                 "the line is not said once per process");
    }

    void theMixedCuesAskForTheNotificationRole()
    {
        QFile file(QStringLiteral(SOUNDS_DIR "/../../src/calls/CallSoundPlayer.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString code = QString::fromUtf8(file.readAll());
        const int begin = code.indexOf(QStringLiteral("class AudioSinkOutput"));
        const int end = code.indexOf(QStringLiteral("\nclass MixerCues"), begin);
        QVERIFY2(begin >= 0 && end > begin, "no AudioSinkOutput");
        const QString output = code.mid(begin, end - begin);
        const int role = output.indexOf(QStringLiteral(
            "setRole(\n                    QtMultimediaPrivate::AudioEndpointRole::SoundEffect)"));
        const int start = output.indexOf(QStringLiteral("m_sink->start(feed)"));
        QVERIFY2(role >= 0, "the cues' sink asks for no role");
        QVERIFY2(start > role, "the role is set after start() made the stream");
        const int guard = output.lastIndexOf(
            QStringLiteral("#if defined(LIGHTNING_HAVE_QT_AUDIO_ROLE)"), role);
        QVERIFY2(guard >= 0, "the private call is not behind the CMake check");
        const int sameQt = output.indexOf(
            QStringLiteral("qstrcmp(qVersion(), QT_VERSION_STR) == 0"), guard);
        QVERIFY2(sameQt > guard && sameQt < role,
                 "the private call does not check the running Qt");
        QVERIFY2(output.contains(QStringLiteral(
                     "QStringLiteral(\"call sounds: media.role not set (%1)\")"))
                     && output.contains(QStringLiteral(
                         "sayNoRole(QStringLiteral(\"no private Qt Multimedia headers\"));")),
                 "a build or a runtime without the role does not say so");
        QFile cmake(QStringLiteral(SOUNDS_DIR "/../../CMakeLists.txt"));
        QVERIFY(cmake.open(QIODevice::ReadOnly));
        const QString lists = QString::fromUtf8(cmake.readAll());
        QVERIFY2(lists.contains(QStringLiteral("find_package(Qt6 6.10 QUIET COMPONENTS MultimediaPrivate)"))
                     && lists.contains(QStringLiteral("#include <QtMultimedia/private/qaudiosystem_p.h>"))
                     && lists.contains(QStringLiteral("LIGHTNING_HAVE_QT_AUDIO_ROLE=1")),
                 "CMake does not check for the private header before using it");
        QFile flatpak(QStringLiteral(SOUNDS_DIR "/../../packaging-ci/packaging/flatpak/"
                                     "org.lightning_matrix.Lightning.yaml.in"));
        QVERIFY(flatpak.open(QIODevice::ReadOnly));
        QVERIFY2(QString::fromUtf8(flatpak.readAll())
                     .contains(QStringLiteral("-DLIGHTNING_REQUIRE_QT_AUDIO_ROLE=ON")),
                 "the Flatpak can ship without the cues' role");
#if defined(LIGHTNING_HAVE_QT_AUDIO_ROLE)
        qInfo("this build sets the cues' media.role");
#endif
    }
};

QTEST_GUILESS_MAIN(CallSoundPlayerTest)
#include "CallSoundPlayerTest.moc"
