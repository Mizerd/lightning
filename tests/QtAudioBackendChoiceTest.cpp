#include <QCoreApplication>
#include <QFile>
#include <QTest>

#include "calls/QtAudioBackendChoice.h"

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
    void softwareVideoDecodingUsesQtEmptyDeviceList()
    {
        const auto source = mainSource();
        QVERIFY(!source.isEmpty());
        QVERIFY(source.contains("if (!qEnvironmentVariableIsSet(\"QT_FFMPEG_DECODING_HW_DEVICE_TYPES\"))\n"
                                "        qputenv(\"QT_FFMPEG_DECODING_HW_DEVICE_TYPES\", \",\");"));
    }

    void theDecisionTable()
    {
        using namespace lightning::calls;
        QtAudioBackendInputs nix;
        nix.platform = AudioPlatform::Linux;
        nix.qtMajor = 6;
        nix.qtMinor = 11;
        nix.qtHasPulseBackend = true;
        nix.pulseServerReachable = true;

        QCOMPARE(decideQtAudioBackend(nix).value, QByteArrayLiteral("pulseaudio"));
        QtAudioBackendInputs at610 = nix;
        at610.qtMinor = 10;
        QCOMPARE(decideQtAudioBackend(at610).value, QByteArrayLiteral("pulseaudio"));
        QtAudioBackendInputs qt7 = nix;
        qt7.qtMajor = 7;
        qt7.qtMinor = 0;
        QCOMPARE(decideQtAudioBackend(qt7).value, QByteArrayLiteral("pulseaudio"));

        QtAudioBackendInputs user = nix;
        user.userSetBackend = true;
        QVERIFY2(decideQtAudioBackend(user).value.isEmpty(),
                 "the user's own QT_AUDIO_BACKEND was overridden");
        QtAudioBackendInputs userFlatpak = user;
        userFlatpak.flatpak = true;
        QVERIFY(decideQtAudioBackend(userFlatpak).value.isEmpty());

        QtAudioBackendInputs qt69 = nix;
        qt69.qtMinor = 9;
        QVERIFY(decideQtAudioBackend(qt69).value.isEmpty());
        QtAudioBackendInputs qt68flatpak = nix;
        qt68flatpak.qtMinor = 8;
        qt68flatpak.flatpak = true;
        QVERIFY(decideQtAudioBackend(qt68flatpak).value.isEmpty());

        for (AudioPlatform other : {AudioPlatform::Windows, AudioPlatform::MacOS,
                                    AudioPlatform::Other}) {
            QtAudioBackendInputs elsewhere = nix;
            elsewhere.platform = other;
            QVERIFY(decideQtAudioBackend(elsewhere).value.isEmpty());
        }

        QtAudioBackendInputs noServer = nix;
        noServer.pulseServerReachable = false;
        QVERIFY(decideQtAudioBackend(noServer).value.isEmpty());
        QVERIFY(QByteArray(decideQtAudioBackend(noServer).reason)
                    .contains("no PulseAudio server"));
        QtAudioBackendInputs noBackend = nix;
        noBackend.qtHasPulseBackend = false;
        QVERIFY(decideQtAudioBackend(noBackend).value.isEmpty());
        QVERIFY(QByteArray(decideQtAudioBackend(noBackend).reason)
                    .contains("no PulseAudio backend"));

        QtAudioBackendInputs flatpak = noServer;
        flatpak.flatpak = true;
        flatpak.qtHasPulseBackend = false;
        QCOMPARE(decideQtAudioBackend(flatpak).value, QByteArrayLiteral("pulseaudio"));

        for (const QtAudioBackendInputs &in :
             {nix, user, qt69, noServer, noBackend, flatpak}) {
            QVERIFY(QByteArray(decideQtAudioBackend(in).reason).size() > 5);
        }
    }

    void theLibraryListIsReadFromTheElf()
    {
        using namespace lightning::calls;
#ifdef Q_OS_LINUX
        const QString self = QCoreApplication::applicationFilePath();
        const QList<QByteArray> needed = elfNeededLibraries(self);
        bool core = false;
        for (const QByteArray &name : needed)
            core = core || name.startsWith("libQt6Core.so");
        QVERIFY2(core, qPrintable(QStringLiteral("DT_NEEDED of the test: %1")
                                      .arg(QString::fromLatin1(needed.join(' ')))));
        const QString qtCore = mappedObjectPath("libQt6Core.so");
        QVERIFY2(!qtCore.isEmpty() && qtCore.startsWith(QLatin1Char('/')),
                 qPrintable(qtCore));
        QVERIFY(!elfNeededLibraries(qtCore).isEmpty());
        QVERIFY(sharedObjectLoaded("libQt6Core"));
        QVERIFY(!sharedObjectLoaded("libNoSuchLibraryHere"));
        QVERIFY(elfNeededLibraries(QStringLiteral(SOURCE_DIR "/src/main.cpp")).isEmpty());
        QVERIFY(elfNeededLibraries(QStringLiteral("/nonexistent/lib.so")).isEmpty());
        const QString multimedia = mappedObjectPath("libQt6Multimedia.so");
        bool needsPulse = false;
        for (const QByteArray &name : elfNeededLibraries(multimedia))
            needsPulse = needsPulse || name.startsWith("libpulse.so");
        QCOMPARE(qtMultimediaHasPulseBackend(), !multimedia.isEmpty() && needsPulse);
        QVERIFY(mappedObjectPath("libNoSuchLibraryHere").isEmpty());
#else
        QSKIP("ELF only");
#endif
        const QByteArray source = mainSource();
        QVERIFY2(source.contains("in.qtHasPulseBackend = qtMultimediaHasPulseBackend();"),
                 "main() does not read the backend from libQt6Multimedia");
    }

    void theVariableIsReleasedOnlyWhenLightningSetIt()
    {
        using lightning::calls::releaseQtAudioBackendVariable;
        const QByteArray before = qgetenv("QT_AUDIO_BACKEND");
        const bool hadBefore = qEnvironmentVariableIsSet("QT_AUDIO_BACKEND");

        qputenv("QT_AUDIO_BACKEND", "pulseaudio");
        QVERIFY(releaseQtAudioBackendVariable(true));
        QVERIFY(!qEnvironmentVariableIsSet("QT_AUDIO_BACKEND"));
        QVERIFY(!releaseQtAudioBackendVariable(true));

        qputenv("QT_AUDIO_BACKEND", "pipewire");
        QVERIFY(!releaseQtAudioBackendVariable(false));
        QCOMPARE(qgetenv("QT_AUDIO_BACKEND"), QByteArrayLiteral("pipewire"));
        qputenv("QT_AUDIO_BACKEND", "");
        QVERIFY(!releaseQtAudioBackendVariable(false));
        QVERIFY(qEnvironmentVariableIsSet("QT_AUDIO_BACKEND"));

        if (hadBefore)
            qputenv("QT_AUDIO_BACKEND", before);
        else
            qunsetenv("QT_AUDIO_BACKEND");

        const QByteArray source = mainSource();
        const qsizetype sounds = source.indexOf("controller.enableCallSounds();");
        const qsizetype gate = source.indexOf("QtAudioGate::install()");
        const qsizetype released = source.indexOf(
            "releaseQtAudioBackendVariable(g_qtAudioBackendSetHere);");
        QVERIFY(sounds > 0 && gate > 0);
        QVERIFY2(released > sounds && released > gate,
                 "QT_AUDIO_BACKEND is released before Qt's audio is up, or never");
    }

    void mainAsksBeforeAnythingTouchesQtMultimedia()
    {
        const QByteArray source = mainSource();
        QVERIFY2(!source.isEmpty(), "src/main.cpp is not where this test looks");
        const qsizetype asked = source.indexOf("decideQtAudioBackend(");
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
        const qsizetype inputs = source.lastIndexOf("QtAudioBackendInputs in;", asked);
        QVERIFY2(inputs > 0 && asked - inputs < 3000,
                 "the decision's inputs are not gathered where it is made");
        const QByteArray window = source.mid(inputs, set - inputs);
        QVERIFY2(window.contains("in.flatpak = sandboxenv::isFlatpak()"),
                 "the Flatpak check does not feed the decision");
        QVERIFY2(window.contains(
                     "in.userSetBackend = qEnvironmentVariableIsSet(\"QT_AUDIO_BACKEND\")"),
                 "a user's own QT_AUDIO_BACKEND is not honoured");
        QVERIFY2(window.contains("qVersion()"),
                 "the RUNNING Qt's version does not feed the decision");
        QVERIFY2(window.contains("pulseServerReachable()"),
                 "a missing PulseAudio server is not considered");
        QVERIFY(source.mid(set, 1500).contains("g_qtAudioBackendLine ="));
        const qsizetype logged = source.indexOf(
            "qCInfo(lcQtAudioBackend).noquote() << g_qtAudioBackendLine;");
        QVERIFY2(logged > app, "the backend decision is never logged");
    }

    void theSoundsStatusReportsTheAudioBackend()
    {
        const QByteArray source = mainSource();
        const qsizetype soundsStatus = source.indexOf(
            "pf.action == PreflightResult::RunCallSoundsStatus");
        QVERIFY(soundsStatus > 0);
        const QByteArray branch = source.mid(soundsStatus, 6000);
        QVERIFY(branch.contains("qt audio backend requested: "));
        QVERIFY(branch.contains("set-by-lightning="));
        QVERIFY(branch.contains("g_qtAudioBackendReason"));
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
