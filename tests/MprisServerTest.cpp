// MPRIS for the app-owned voice/audio player (src/media/MprisServer): the
// desktop's media keys and "now playing" widgets reach the player through
// org.mpris.MediaPlayer2.lightning, which is on the bus ONLY while a clip is
// loaded. Everything runs on a PRIVATE dbus-daemon this test starts itself
// (no service directories), through named connections: nothing here ever
// touches the session bus of whoever runs it. A second connection plays the
// desktop shell: the methods move the real VoicePlaybackController, its
// properties and metadata answer, PropertiesChanged and Seeked are signalled,
// the metadata follows the notification preview privacy, a second Lightning
// takes the per-instance name, and LIGHTNING_DISABLE_MPRIS keeps it off.
// Sources are silent WAV files.

#include "media/MprisServer.h"
#include "media/VoicePlaybackController.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QFile>
#include <QMediaPlayer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#ifdef QT_WIDGETS_LIB
#include <QApplication>
#else
#include <QGuiApplication>
#endif

#include <cstdio>

namespace {

const QString kService = QStringLiteral("org.mpris.MediaPlayer2.lightning");
const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayer = QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString kRoot = QStringLiteral("org.mpris.MediaPlayer2");
const QString kServerBus = QStringLiteral("mpris-test-server");

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

} // namespace

class MprisServerTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_media;
    QTemporaryDir m_busDir;
    QProcess m_daemon;
    QString m_address;
    QString m_clip;
    QString m_clipTwo;
    QDBusConnection m_client{ QString() };

    // A server on the private bus; `level` is the preview policy's answer.
    void useTestBus(MprisServer &server, int level = 0)
    {
        server.setBusConnectionName(kServerBus);
        server.setPreviewPolicy([level](const QString &) { return level; });
    }
    QList<QVariantMap> m_changed;
    QList<qlonglong> m_seeked;

    void installSources(VoicePlaybackController *vp)
    {
        const QString one = QUrl::fromLocalFile(m_clip).toString();
        const QString two = QUrl::fromLocalFile(m_clipTwo).toString();
        vp->setSourceResolverForTest([one, two](const QString &key) {
            if (key == QLatin1String("mk-one"))
                return one;
            if (key == QLatin1String("mk-two"))
                return two;
            return QString();
        });
    }
    static QVariantMap voiceMeta()
    {
        return { { QStringLiteral("senderName"), QStringLiteral("Alice") },
                 { QStringLiteral("isVoice"), true },
                 { QStringLiteral("roomName"), QStringLiteral("Secret room") } };
    }

    // An asynchronous call from the client connection, waited for with the
    // event loop running: the server answers on this same thread.
    QDBusMessage call(const QString &service, const QString &interface,
                      const QString &method, const QVariantList &args = {})
    {
        QDBusMessage msg = QDBusMessage::createMethodCall(service, kPath,
                                                          interface, method);
        msg.setArguments(args);
        return wait(m_client.asyncCall(msg, 5000));
    }
    static QDBusMessage wait(const QDBusPendingCall &pending)
    {
        QDBusPendingCallWatcher watcher(pending);
        QElapsedTimer t;
        t.start();
        while (!watcher.isFinished() && t.elapsed() < 6000)
            QTest::qWait(10);
        return watcher.reply();
    }
    QVariant property(const QString &interface, const QString &name,
                      const QString &service = kService)
    {
        const QDBusMessage reply = call(
            service, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("Get"), { interface, name });
        if (reply.type() != QDBusMessage::ReplyMessage
            || reply.arguments().isEmpty())
            return {};
        return reply.arguments().constFirst().value<QDBusVariant>().variant();
    }
    QVariantMap metadata()
    {
        const QVariant raw = property(kPlayer, QStringLiteral("Metadata"));
        return qdbus_cast<QVariantMap>(raw.value<QDBusArgument>());
    }
    QDBusMessage nameHasOwnerMessage(const QString &name)
    {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"),
            QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"),
            QStringLiteral("NameHasOwner"));
        msg << name;
        return msg;
    }
    bool hasOwner(const QString &name)
    {
        const QDBusMessage reply =
            wait(m_client.asyncCall(nameHasOwnerMessage(name), 5000));
        return reply.type() == QDBusMessage::ReplyMessage
            && reply.arguments().value(0).toBool();
    }

private Q_SLOTS:
    void onPropertiesChanged(const QString &interface,
                             const QVariantMap &changed, const QStringList &)
    {
        if (interface == kPlayer)
            m_changed.append(changed);
    }
    void onSeeked(qlonglong positionUs) { m_seeked.append(positionUs); }

    void initTestCase()
    {
        qunsetenv("LIGHTNING_DISABLE_MPRIS");
        const QString daemon = QStandardPaths::findExecutable(
            QStringLiteral("dbus-daemon"));
        if (daemon.isEmpty())
            QSKIP("needs dbus-daemon for a private bus");
        QVERIFY(m_busDir.isValid());
        const QString config = m_busDir.filePath(QStringLiteral("bus.conf"));
        {
            QFile f(config);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write(QStringLiteral(
                        "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus "
                        "Bus Configuration 1.0//EN\" \"http://www.freedesktop."
                        "org/standards/dbus/1.0/busconfig.dtd\">\n"
                        "<busconfig>\n  <type>session</type>\n"
                        "  <listen>unix:dir=%1</listen>\n"
                        "  <policy context=\"default\">\n"
                        "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                        "    <allow eavesdrop=\"true\"/>\n"
                        "    <allow own=\"*\"/>\n  </policy>\n"
                        "</busconfig>\n")
                        .arg(m_busDir.path()).toUtf8());
        }
        m_daemon.start(daemon, { QStringLiteral("--nofork"),
                                 QStringLiteral("--print-address=1"),
                                 QStringLiteral("--config-file=%1").arg(config) });
        QVERIFY(m_daemon.waitForStarted(5000));
        QVERIFY(m_daemon.waitForReadyRead(5000));
        m_address = QString::fromLatin1(m_daemon.readLine().trimmed());
        QVERIFY(!m_address.isEmpty());
        QVERIFY(QDBusConnection::connectToBus(m_address, kServerBus)
                    .isConnected());
        QVERIFY(m_media.isValid());
        m_clip = m_media.filePath(QStringLiteral("one.wav"));
        m_clipTwo = m_media.filePath(QStringLiteral("two.wav"));
        QVERIFY(writeSilentWav(m_clip, 6));
        QVERIFY(writeSilentWav(m_clipTwo, 6));
        m_client = QDBusConnection::connectToBus(m_address,
                                                 QStringLiteral("mpris-client"));
        QVERIFY(m_client.isConnected());
        QVERIFY(m_client.connect(
            QString(), kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"), this,
            SLOT(onPropertiesChanged(QString,QVariantMap,QStringList))));
        QVERIFY(m_client.connect(QString(), kPath, kPlayer,
                                 QStringLiteral("Seeked"), this,
                                 SLOT(onSeeked(qlonglong))));
    }

    void cleanupTestCase()
    {
        QDBusConnection::disconnectFromBus(QStringLiteral("mpris-client"));
        QDBusConnection::disconnectFromBus(kServerBus);
        if (m_daemon.state() != QProcess::NotRunning) {
            m_daemon.kill();
            m_daemon.waitForFinished(3000);
        }
    }

    // On the bus only while a clip is loaded; the media keys' methods drive
    // the real player; the metadata says what plays and leaks no room name or
    // event id.
    void theServerAppearsWithAClipAndDrivesThePlayer()
    {
        VoicePlaybackController vp;
        installSources(&vp);
        MprisServer server(&vp);
        useTestBus(server);
        QTest::qWait(300);
        QVERIFY2(!hasOwner(kService), "on the bus with nothing loaded");

        vp.play(QStringLiteral("$secret-event"), QStringLiteral("!room:x"),
                QStringLiteral("mk-one"), voiceMeta());
        QTRY_VERIFY_WITH_TIMEOUT(hasOwner(kService), 5000);
        QCOMPARE(server.registeredServiceName(), kService);
        QTRY_VERIFY_WITH_TIMEOUT(vp.playing(), 5000);

        QCOMPARE(property(kRoot, QStringLiteral("Identity")).toString(),
                 QStringLiteral("Lightning"));
        QCOMPARE(property(kRoot, QStringLiteral("CanRaise")).toBool(), true);
        QCOMPARE(property(kRoot, QStringLiteral("CanQuit")).toBool(), false);
        QCOMPARE(property(kPlayer, QStringLiteral("PlaybackStatus")).toString(),
                 QStringLiteral("Playing"));
        QCOMPARE(property(kPlayer, QStringLiteral("CanGoNext")).toBool(), false);
        QTRY_VERIFY_WITH_TIMEOUT(
            property(kPlayer, QStringLiteral("CanSeek")).toBool(), 5000);

        const QVariantMap md = metadata();
        QCOMPARE(md.value(QStringLiteral("xesam:title")).toString(),
                 QStringLiteral("Voice message"));
        QCOMPARE(md.value(QStringLiteral("xesam:artist")).toStringList(),
                 QStringList{ QStringLiteral("Alice") });
        const QString trackId = md.value(QStringLiteral("mpris:trackid"))
                                    .value<QDBusObjectPath>().path();
        QVERIFY2(trackId.startsWith(QStringLiteral("/org/lightning/track/")),
                 qPrintable(trackId));
        // Nothing that names the room or the event leaves the process.
        for (auto it = md.constBegin(); it != md.constEnd(); ++it) {
            const QString text = it.value().toString()
                + it.value().toStringList().join(QLatin1Char(' '))
                + it.value().value<QDBusObjectPath>().path();
            QVERIFY2(!text.contains(QStringLiteral("Secret room"))
                         && !text.contains(QStringLiteral("secret-event"))
                         && !text.contains(QStringLiteral("!room")),
                     qPrintable(it.key()));
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            metadata().value(QStringLiteral("mpris:length")).toLongLong()
                > 5000000, 5000);

        // PlayPause pauses, and the change is signalled.
        m_changed.clear();
        call(kService, kPlayer, QStringLiteral("PlayPause"));
        QTRY_VERIFY(!vp.playing());
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(m_changed.cbegin(), m_changed.cend(),
                        [](const QVariantMap &c) {
                            return c.value(QStringLiteral("PlaybackStatus"))
                                       .toString() == QLatin1String("Paused");
                        }), 3000);
        // Seek and SetPosition move it, and a seek is announced.
        m_seeked.clear();
        call(kService, kPlayer, QStringLiteral("SetPosition"),
             { QVariant::fromValue(QDBusObjectPath(trackId)),
               QVariant::fromValue(qlonglong(2000000)) });
        QTRY_VERIFY(qAbs(vp.position() - 2000) < 150);
        call(kService, kPlayer, QStringLiteral("Seek"),
             { QVariant::fromValue(qlonglong(1500000)) });
        QTRY_VERIFY(qAbs(vp.position() - 3500) < 150);
        QTRY_VERIFY_WITH_TIMEOUT(!m_seeked.isEmpty(), 3000);
        // SetPosition for a track that is no longer current is ignored.
        call(kService, kPlayer, QStringLiteral("SetPosition"),
             { QVariant::fromValue(QDBusObjectPath(
                   QStringLiteral("/org/lightning/track/tother"))),
               QVariant::fromValue(qlonglong(0)) });
        QTest::qWait(150);
        QVERIFY(qAbs(vp.position() - 3500) < 150);
        // Previous: back to the start (and playing).
        call(kService, kPlayer, QStringLiteral("Previous"));
        QTRY_VERIFY(vp.position() < 500);
        QTRY_VERIFY(vp.playing());
        call(kService, kPlayer, QStringLiteral("Pause"));
        QTRY_VERIFY(!vp.playing());
        call(kService, kPlayer, QStringLiteral("Play"));
        QTRY_VERIFY(vp.playing());

        // Next walks the run; CanGoNext says so first.
        vp.setUpNext({ QVariantMap{
            { QStringLiteral("eventId"), QStringLiteral("$two") },
            { QStringLiteral("mediaKey"), QStringLiteral("mk-two") },
            { QStringLiteral("senderName"), QStringLiteral("Bob") },
            { QStringLiteral("isVoice"), true } } });
        QTRY_VERIFY(property(kPlayer, QStringLiteral("CanGoNext")).toBool());
        call(kService, kPlayer, QStringLiteral("Next"));
        QTRY_COMPARE(vp.eventId(), QStringLiteral("$two"));
        QTRY_COMPARE(metadata().value(QStringLiteral("xesam:artist"))
                         .toStringList(), QStringList{ QStringLiteral("Bob") });
        // ...and a chain advance does not drop the name off the bus.
        QVERIFY(hasOwner(kService));

        // Rate: within the in-app range; 0 means pause.
        call(kService, QStringLiteral("org.freedesktop.DBus.Properties"),
             QStringLiteral("Set"),
             { kPlayer, QStringLiteral("Rate"),
               QVariant::fromValue(QDBusVariant(1.5)) });
        QTRY_COMPARE(vp.rate(), 1.5);

        // Stop unloads the clip, and the name leaves the bus.
        call(kService, kPlayer, QStringLiteral("Stop"));
        QTRY_VERIFY(!vp.active());
        QTRY_VERIFY_WITH_TIMEOUT(!hasOwner(kService), 3000);
        QVERIFY(server.registeredServiceName().isEmpty());
    }

    // An audio file: its own title when it has one, else its file name; the
    // sender is the artist.
    void anAudioFileIsNamedByItsFile()
    {
        VoicePlaybackController vp;
        installSources(&vp);
        MprisServer server(&vp);
        useTestBus(server);
        vp.play(QStringLiteral("$file"), QStringLiteral("!room:x"),
                QStringLiteral("mk-one"),
                { { QStringLiteral("senderName"), QStringLiteral("Carol") },
                  { QStringLiteral("filename"), QStringLiteral("song.wav") },
                  { QStringLiteral("isVoice"), false } });
        QTRY_VERIFY_WITH_TIMEOUT(hasOwner(kService), 5000);
        const QVariantMap md = metadata();
        QCOMPARE(md.value(QStringLiteral("xesam:title")).toString(),
                 QStringLiteral("song.wav"));
        QCOMPARE(md.value(QStringLiteral("xesam:artist")).toStringList(),
                 QStringList{ QStringLiteral("Carol") });
        vp.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!hasOwner(kService), 3000);
    }

    // A second Lightning (another profile) on the same bus takes the spec's
    // per-instance name rather than failing or stealing the first one's.
    void aSecondInstanceTakesAnInstanceName()
    {
        VoicePlaybackController first;
        installSources(&first);
        MprisServer one(&first);
        useTestBus(one);
        first.play(QStringLiteral("$a"), QStringLiteral("!r:x"),
                   QStringLiteral("mk-one"), voiceMeta());
        QTRY_VERIFY_WITH_TIMEOUT(one.registeredServiceName() == kService,
                                 5000);

        // Its own connection, as a second process would have.
        QDBusConnection second = QDBusConnection::connectToBus(
            m_address, QStringLiteral("mpris-second"));
        QVERIFY(second.isConnected());
        VoicePlaybackController other;
        installSources(&other);
        MprisServer two(&other);
        useTestBus(two);
        two.setBusConnectionName(QStringLiteral("mpris-second"));
        other.play(QStringLiteral("$b"), QStringLiteral("!r:x"),
                   QStringLiteral("mk-two"), voiceMeta());
        QTRY_VERIFY_WITH_TIMEOUT(!two.registeredServiceName().isEmpty(), 5000);
        QVERIFY2(two.registeredServiceName().startsWith(
                     kService + QStringLiteral(".instance")),
                 qPrintable(two.registeredServiceName()));
        QCOMPARE(one.registeredServiceName(), kService);
        other.stop();
        first.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!hasOwner(kService), 3000);
        QDBusConnection::disconnectFromBus(QStringLiteral("mpris-second"));
    }

    // Review H1: a desktop widget and the lock screen are as public as a
    // notification, so the metadata follows the notification preview privacy
    // of the clip's room. The server used to publish the sender and the
    // file's own title whatever the user had chosen.
    void theMetadataFollowsTheNotificationPrivacy()
    {
        VoicePlaybackController vp;
        installSources(&vp);
        MprisServer server(&vp);
        int level = 2;
        server.setBusConnectionName(kServerBus);
        server.setPreviewPolicy([&level](const QString &roomId) {
            return roomId == QLatin1String("!secret:x") ? level : 0;
        });
        vp.play(QStringLiteral("$file"), QStringLiteral("!secret:x"),
                QStringLiteral("mk-one"),
                { { QStringLiteral("senderName"), QStringLiteral("Carol") },
                  { QStringLiteral("filename"),
                    QStringLiteral("holiday-plans.wav") },
                  { QStringLiteral("isVoice"), false } });
        QTRY_VERIFY_WITH_TIMEOUT(hasOwner(kService), 5000);
        // Private: a generic title and no artist.
        QVariantMap md = metadata();
        QCOMPARE(md.value(QStringLiteral("xesam:title")).toString(),
                 QStringLiteral("Audio"));
        QVERIFY2(!md.contains(QStringLiteral("xesam:artist")),
                 "the private level published the sender");
        // Sender only: the sender, and still no file name.
        level = 1;
        m_changed.clear();
        server.refreshMetadata();
        QTRY_VERIFY_WITH_TIMEOUT(!m_changed.isEmpty(), 3000);
        md = metadata();
        QCOMPARE(md.value(QStringLiteral("xesam:title")).toString(),
                 QStringLiteral("Audio"));
        QCOMPARE(md.value(QStringLiteral("xesam:artist")).toStringList(),
                 QStringList{ QStringLiteral("Carol") });
        // Full: the file's own name.
        level = 0;
        server.refreshMetadata();
        QTRY_COMPARE_WITH_TIMEOUT(
            metadata().value(QStringLiteral("xesam:title")).toString(),
            QStringLiteral("holiday-plans.wav"), 3000);
        // A voice message at the private level: no sender either.
        level = 2;
        vp.play(QStringLiteral("$voice"), QStringLiteral("!secret:x"),
                QStringLiteral("mk-two"), voiceMeta());
        QTRY_COMPARE_WITH_TIMEOUT(
            metadata().value(QStringLiteral("xesam:title")).toString(),
            QStringLiteral("Voice message"), 3000);
        QVERIFY(!metadata().contains(QStringLiteral("xesam:artist")));
        // The track id is salted per server: another server names the same
        // clip differently.
        MprisServer other(&vp);
        QVERIFY(other.trackSalt() != server.trackSalt());
        vp.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!hasOwner(kService), 3000);
    }

    // Review M4: the opt-out keeps a server off the bus entirely (the test
    // suites set it, so a ctest run never reaches a developer's desktop).
    void theOptOutKeepsItOffTheBus()
    {
        qputenv("LIGHTNING_DISABLE_MPRIS", "1");
        VoicePlaybackController vp;
        installSources(&vp);
        MprisServer server(&vp);
        useTestBus(server);
        vp.play(QStringLiteral("$x"), QStringLiteral("!r:x"),
                QStringLiteral("mk-one"), voiceMeta());
        QTest::qWait(600);
        QVERIFY(!hasOwner(kService));
        QVERIFY(server.registeredServiceName().isEmpty());
        vp.stop();
        qunsetenv("LIGHTNING_DISABLE_MPRIS");
    }
};

// Without dbus-daemon there is no private bus to test on: exit 77, which
// CTest reports as SKIPPED (SKIP_RETURN_CODE), rather than a QSKIP that
// counts as a pass.
int main(int argc, char *argv[])
{
    if (QStandardPaths::findExecutable(QStringLiteral("dbus-daemon")).isEmpty()) {
        std::fputs("mpris-server: dbus-daemon not found; skipping\n", stderr);
        return 77;
    }
#ifdef QT_WIDGETS_LIB
    QApplication app(argc, argv);
#else
    QGuiApplication app(argc, argv);
#endif
    app.setAttribute(Qt::AA_Use96Dpi, true);
    MprisServerTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "MprisServerTest.moc"
