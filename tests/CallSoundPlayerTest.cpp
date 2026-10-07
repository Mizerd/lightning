// CallSoundPlayer's liveness: a sound thread stuck on the sound server must
// stop reporting sounds as playable, or the ringer keeps choosing its own
// (silent) ring over the desktop's call sound. No sound effect is ever
// created here, so no audio device is needed.
#include "calls/CallSoundPlayer.h"
#include "calls/CallSoundRebuild.h"

#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QLoggingCategory>
#include <QSemaphore>
#include <QtTest/QtTest>

#include <map>
#include <memory>

Q_LOGGING_CATEGORY(lcCallSound, "lightning.calls.sound")

namespace {
// A model of Qt 6.10+'s QSoundEffect engine pool
// (QSoundEffectPrivateWithPlayer::getEngineFor): one engine per output ID,
// held by weak reference, so a live engine is handed to every effect that
// asks for that ID, dead or not.
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
// An effect attaches to an engine as soon as it exists (its sample is
// already decoded: Qt's sample cache answers at once).
struct FakeEffect {
    std::shared_ptr<FakeEngine> engine;
};
} // namespace

// Stands in for CallDeviceController::audioOutputsChanged.
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
    // Windows 2026-10-07 (build 300): an RDP audio endpoint went away for
    // 28 s and came back under the same ID; the reload rebuilt all 17 cues
    // ("reloaded= 17") and every one stayed silent until a restart. The
    // rebuilt effects were handed the engine whose stream had died, because
    // an old effect still held it in Qt's pool when they asked. The reload
    // must leave no old effect alive when it creates the first new one.
    void aReloadNeverHandsANewCueTheEngineThatDied()
    {
        const QStringList sounds{QStringLiteral("ring"),
                                 QStringLiteral("connected"),
                                 QStringLiteral("hangup")};
        const QString output = QStringLiteral("rdp-endpoint");

        // The control: the order the reload used before (each old effect
        // released only after its successor exists, as deleteLater() did).
        // It must reproduce the fault, or this model proves nothing.
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

    // The QSoundEffect cues use that order, free the old effects at once
    // (deleteLater() keeps them, and their engine, alive past the creates),
    // and with no speaker chosen leave the device unset so Qt 6.10+ follows
    // the default instead of pinning the dying endpoint by its ID.
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

    // Measured 2026-09-25 on Qt 6.8.2: after a sound-server stall the sound
    // thread never comes back. canPlay() must turn false within ~2 s of a job
    // going unanswered, and true again if the thread catches up.
    void aStuckSoundThreadReportsNothingPlayable()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
                               /*offTheGuiThread=*/true);
        player.markReadyForTest(QStringLiteral("ring"));
        QVERIFY(player.canPlay(QStringLiteral("ring")));

        // A quick job is answered and changes nothing.
        QSemaphore quick;
        player.postForTest([&quick] { quick.release(); });
        QVERIFY(quick.tryAcquire(1, 2000));
        QTest::qWait(2300);
        QVERIFY(player.canPlay(QStringLiteral("ring")));

        // A job that blocks, as an open on a stalled server does.
        QSemaphore unblock;
        player.postForTest([&unblock] { unblock.acquire(); });
        QVERIFY(player.canPlay(QStringLiteral("ring"))); // not yet 2 s
        QTRY_VERIFY_WITH_TIMEOUT(!player.canPlay(QStringLiteral("ring")), 4000);
        // Later cues queue behind it and do not make it look alive.
        player.postForTest([] {});
        QVERIFY(!player.canPlay(QStringLiteral("ring")));

        // The thread catches up: the sound is playable again.
        unblock.release();
        QTRY_VERIFY_WITH_TIMEOUT(player.canPlay(QStringLiteral("ring")), 4000);
    }

    // Qt 6.10+ keeps the effects on the GUI thread, where a job runs at once:
    // there is nothing to wait on and readiness is the loaded state alone.
    void onTheGuiThreadReadinessIsTheLoadedState()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
                               /*offTheGuiThread=*/false);
        QVERIFY(!player.canPlay(QStringLiteral("ring")));
        bool ran = false;
        player.postForTest([&ran] { ran = true; });
        QVERIFY(ran);
        player.markReadyForTest(QStringLiteral("ring"));
        QVERIFY(player.canPlay(QStringLiteral("ring")));
    }

    // Measured live on Windows 2026-10-07: the audio endpoint went away, every
    // cue went to Error ("call sound unusable"), and when the device came
    // back nothing reloaded them, so the ringer stayed on the desktop's sound
    // for the rest of the session. A changed output list must reload them,
    // once per burst of changes (devices flap), and a burst that never goes
    // quiet must still be answered.
    void aChangedOutputListReloadsTheCuesOncePerBurst()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
                               /*offTheGuiThread=*/false);
        player.setOutputChangeDebounceForTest(150, 600);
        QCOMPARE(player.soundReloadsForTest(), 0);

        // A burst: one reload, after it goes quiet.
        for (int i = 0; i < 5; ++i) {
            player.audioOutputsChanged();
            QTest::qWait(30);
        }
        QCOMPARE(player.soundReloadsForTest(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(player.soundReloadsForTest(), 1, 2000);
        QTest::qWait(400);
        QCOMPARE(player.soundReloadsForTest(), 1);

        // The next change is a new burst.
        player.audioOutputsChanged();
        QTRY_COMPARE_WITH_TIMEOUT(player.soundReloadsForTest(), 2, 2000);

        // Changes every 100 ms for 1.5 s never go quiet for 150 ms, and are
        // still answered: at the longest wait, then again.
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

    // Live Windows FAIL 2026-10-07 (pipeline 290): the call's audio came back
    // on CallDeviceController's output-change signal, and no cue (nor the
    // notification chime) ever did: the player's own QMediaDevices was never
    // told, and nothing was logged. The reload must follow a signal it is
    // handed, ask the given answer about outputs when it is due, and skip
    // while there are none.
    void theReloadFollowsTheSignalItIsHanded()
    {
        CallSoundPlayer player(CallSoundPlayer::ForTest{},
                               /*offTheGuiThread=*/false);
        player.setOutputChangeDebounceForTest(100, 500);
        OutputListSource source;
        bool anyOutput = false;
        int asked = 0;
        player.followOutputChanges(&source, &OutputListSource::changed,
                                   [&] {
                                       ++asked;
                                       return anyOutput;
                                   });

        // The device went: a burst, one reload, told there is no output.
        for (int i = 0; i < 3; ++i) {
            Q_EMIT source.changed();
            QTest::qWait(20);
        }
        QTRY_COMPARE_WITH_TIMEOUT(player.soundReloadsForTest(), 1, 2000);
        QCOMPARE(asked, 1);
        QVERIFY(!player.lastReloadHadOutputForTest());

        // It came back.
        anyOutput = true;
        Q_EMIT source.changed();
        QTRY_COMPARE_WITH_TIMEOUT(player.soundReloadsForTest(), 2, 2000);
        QCOMPARE(asked, 2);
        QVERIFY(player.lastReloadHadOutputForTest());
    }

    // ...and the application hands it the device controller's signal, the
    // one measured to arrive. Source scan: enableCallSounds() opens audio, so
    // no test runs it.
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

    // The notification chimes are bundled like every other sound, in the same
    // format (48 kHz, 16-bit, stereo), and short enough to repeat all day.
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
            QCOMPARE(u16(22), quint16(2));       // stereo
            QCOMPARE(u32(24), quint32(48000));   // 48 kHz
            QCOMPARE(u16(34), quint16(16));      // 16-bit
            const double seconds = double(wav.size() - 44) / (48000.0 * 4.0);
            QVERIFY2(seconds >= 0.15 && seconds <= 0.5,
                     qPrintable(sound + QStringLiteral(" lasts %1 s")
                                    .arg(seconds)));
        }
    }

    // Live FAIL 2026-10-07 (Flatpak, Qt 6.11.2): QSoundEffect freed a
    // finished voice on the audio thread, and the GUI thread's glib event
    // loop then spun for ever on the voice's closed eventfd ("Invalid socket
    // ... disabling", 95 million lines, the call dead). Reproduced with
    // QSoundEffect alone. On Linux from Qt 6.10 the cues are mixed into one
    // output instead (CallSoundMixer; its behaviour is call-sound-pcm's).
    // Source scan, because only a sound server shows the defect: what the
    // player constructs is chosen by a constant true exactly there.
    void onLinuxFromQt610NoCueIsVoicedByQSoundEffect()
    {
        QFile file(QStringLiteral(SOUNDS_DIR "/../../src/calls/CallSoundPlayer.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString code = QString::fromUtf8(file.readAll());
        // Every construction of the effects in the player.
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
        // ...and the mixer's side owns no QSoundEffect.
        const int begin = code.indexOf(QStringLiteral("class AudioSinkOutput"));
        const int end = code.indexOf(QStringLiteral("\nvoid CallSoundEffects::setReady"), begin);
        QVERIFY(begin >= 0 && end > begin);
        const QRegularExpression usesQSoundEffect(
            QStringLiteral(R"(\bQSoundEffect\s*(\*|::|\(|\{)|new\s+QSoundEffect\b)"));
        QVERIFY2(!usesQSoundEffect.match(code.mid(begin, end - begin)).hasMatch(),
                 "the mixed cues use QSoundEffect");
    }

    // Rokas 2026-10-07: the mixed cues keep the stream role QSoundEffect's
    // engine gave them (media.role "Notification", so the desktop's
    // notification volume governs them). Only Qt's private QPlatformAudioSink
    // sets it, before start() creates the stream; a build without the
    // private headers says so once. Measured on a live PipeWire node (pw-dump).
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
        // Private ABI: only into the very Qt it was built against.
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
        // The Flatpak may not ship without it (the KDE SDK 6.11 has it).
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
