#include <QFile>
#include <QTest>

#include "calls/QtAudioBackendChoice.h"

// Which Qt Multimedia audio backend the Flatpak asks for, and that main()
// asks before anything touches Qt Multimedia (Qt picks the backend on first
// use).
class QtAudioBackendChoiceTest : public QObject
{
    Q_OBJECT

private:
    static QByteArray mainSource()
    {
        QFile file(QStringLiteral(SOURCE_DIR "/src/main.cpp"));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

private Q_SLOTS:
    void onlyAFlatpakIsMovedAndAUserChoiceWins()
    {
        using lightning::calls::qtAudioBackendOverride;
        // Qt 6.11 takes any value but "pipewire" as the PulseAudio backend.
        QCOMPARE(qtAudioBackendOverride(/*flatpak=*/true, /*userSet=*/false),
                 QByteArrayLiteral("pulseaudio"));
        QVERIFY2(qtAudioBackendOverride(true, true).isEmpty(),
                 "the user's own QT_AUDIO_BACKEND was overridden");
        QVERIFY(qtAudioBackendOverride(false, false).isEmpty());
        QVERIFY(qtAudioBackendOverride(false, true).isEmpty());
    }

    // The choice is made before the FIRST use of Qt Multimedia in main(),
    // which is the --call-sounds-status preflight (validate-flatpak.sh runs
    // it inside the Flatpak), not only before QApplication; and it really is
    // gated on the Flatpak check, so a forced or negated condition fails.
    void mainAsksBeforeAnythingTouchesQtMultimedia()
    {
        const QByteArray source = mainSource();
        QVERIFY2(!source.isEmpty(), "src/main.cpp is not where this test looks");
        const qsizetype asked = source.indexOf("qtAudioBackendOverride(");
        QVERIFY2(asked > 0, "main() never asks which audio backend to use");
        const qsizetype firstMultimedia = source.indexOf("QMediaDevices::");
        const qsizetype soundsStatus = source.indexOf(
            "pf.action == PreflightResult::RunCallSoundsStatus");
        const qsizetype app = source.indexOf("QApplication app(argc, argv);");
        QVERIFY(firstMultimedia > 0);
        QVERIFY(soundsStatus > 0);
        QVERIFY(app > 0);
        QVERIFY2(asked < firstMultimedia,
                 "Qt Multimedia is used before the audio backend is chosen");
        QVERIFY2(asked < soundsStatus,
                 "--call-sounds-status runs before the audio backend is "
                 "chosen, so the Flatpak validator reports a backend a real "
                 "launch does not use");
        QVERIFY(asked < app);
        const qsizetype set = source.indexOf("qputenv(\"QT_AUDIO_BACKEND\"",
                                             asked);
        QVERIFY2(set > asked && set < firstMultimedia,
                 "the chosen backend is not put in the environment in time");
        const QByteArray window = source.mid(asked, set - asked);
        QVERIFY2(window.contains("sandboxenv::isFlatpak()"),
                 "the backend choice is not gated on the Flatpak check");
        QVERIFY2(!window.contains("!sandboxenv::isFlatpak()"),
                 "the Flatpak check is negated");
        QVERIFY2(window.contains("qEnvironmentVariableIsSet(\"QT_AUDIO_BACKEND\")"),
                 "a user's own QT_AUDIO_BACKEND is not honoured");
    }

    // --call-sounds-status says which backend is in effect, so the Flatpak
    // validation and a live run can confirm the PulseAudio backend.
    void theSoundsStatusReportsTheAudioBackend()
    {
        const QByteArray source = mainSource();
        const qsizetype soundsStatus = source.indexOf(
            "pf.action == PreflightResult::RunCallSoundsStatus");
        QVERIFY(soundsStatus > 0);
        const QByteArray branch = source.mid(soundsStatus, 6000);
        QVERIFY(branch.contains("qt audio backend requested: "));
        QVERIFY(branch.contains("set-by-lightning="));
        // Qt's own answer, guarded like CallSoundPlayer's private call, and
        // a fallback that says it is one.
        const qsizetype asked = branch.indexOf("audioBackendName()");
        QVERIFY2(asked > 0, "Qt is never asked which backend it chose");
        QVERIFY(branch.contains("\"qt audio backend: \""));
        const qsizetype guard =
            branch.lastIndexOf("qstrcmp(qVersion(), QT_VERSION_STR) == 0", asked);
        QVERIFY2(guard > 0, "the private call is not guarded by the running "
                            "Qt being the one built against");
        QVERIFY(branch.contains("fallback (inferred, not proof)"));
        QVERIFY2(!branch.contains("sharedObjectLoaded(\"libpulse\")"),
                 "libpulse is always loaded with Qt Multimedia's PulseAudio "
                 "feature; it proves nothing about the backend");
    }
};

QTEST_GUILESS_MAIN(QtAudioBackendChoiceTest)
#include "QtAudioBackendChoiceTest.moc"
