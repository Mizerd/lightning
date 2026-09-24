// CameraPortal against a fake xdg-desktop-portal on a private session bus.
// With the camera permission already stored, a real portal sends the
// `AccessCamera` reply and the Request's `Response` back to back, so
// CameraPortal must subscribe to `Response` before the reply is handled or
// the grant is lost. The fake reproduces that shape on the same connection.
// A private `dbus-daemon`, because the fake must own
// org.freedesktop.portal.Desktop, which a real desktop already does.

#include "calls/CameraPortal.h"
#include "calls/PortalRequest.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDBusVirtualObject>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

#include <unistd.h>

namespace {

constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kCamera = "org.freedesktop.portal.Camera";
constexpr auto kRequest = "org.freedesktop.portal.Request";

class FakeCameraPortal : public QDBusVirtualObject
{
public:
    enum class Answer {
        InstantGrant,     // Response 0 immediately after the reply (the race)
        InstantDecline,   // Response 1 immediately after the reply
        LateGrantOnOtherPath, // an older portal: unpredicted path, answered later
    };

    explicit FakeCameraPortal(QDBusConnection bus) : m_bus(bus) {}

    Answer answer = Answer::InstantGrant;
    int accessCalls = 0;
    int openCalls = 0;
    int fixtureErrors = 0; // the fake failed, not the code under test

    QString introspect(const QString &) const override { return {}; }

    bool handleMessage(const QDBusMessage &message,
                       const QDBusConnection &) override
    {
        if (message.interface() != QLatin1String(kCamera))
            return false;

        if (message.member() == QLatin1String("AccessCamera")) {
            ++accessCalls;
            // A virtual object receives the raw message, so the a{sv} is a
            // QDBusArgument here, not a QVariantMap; `toMap()` would lose the
            // token and the fake would reply with an invalid path.
            const QVariantMap options =
                qdbus_cast<QVariantMap>(message.arguments().value(0));
            const QString token =
                options.value(QStringLiteral("handle_token")).toString();
            const bool otherPath =
                answer == Answer::LateGrantOnOtherPath;
            const QString requestPath =
                otherPath ? QStringLiteral(
                                "/org/freedesktop/portal/desktop/request/"
                                "legacy/unpredicted")
                          : portal::predictedRequestPath(message.service(),
                                                         token);
            if (requestPath.isEmpty() || !m_bus.send(message.createReply(
                    QVariant::fromValue(QDBusObjectPath(requestPath))))) {
                ++fixtureErrors;
                return false;
            }

            const uint code = answer == Answer::InstantDecline ? 1u : 0u;
            QDBusMessage response = QDBusMessage::createTargetedSignal(
                message.service(), requestPath, QString::fromLatin1(kRequest),
                QStringLiteral("Response"));
            response << code << QVariantMap();
            if (otherPath) {
                // Late enough that subscribe-after-reply would also catch it:
                // this case pins the fallback, not the race.
                QDBusConnection bus = m_bus;
                QTimer::singleShot(200, [bus, response]() mutable {
                    bus.send(response);
                });
            } else {
                m_bus.send(response); // back to back with the reply
            }
            return true;
        }

        if (message.member() == QLatin1String("OpenPipeWireRemote")) {
            ++openCalls;
            int fds[2] = {-1, -1};
            if (::pipe(fds) != 0)
                return false;
            m_bus.send(message.createReply(
                QVariant::fromValue(QDBusUnixFileDescriptor(fds[0]))));
            ::close(fds[0]);
            ::close(fds[1]);
            return true;
        }
        return false;
    }

private:
    QDBusConnection m_bus;
};

} // namespace

class CameraPortalTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void thePredictedPathFollowsThePortalSpec();
    void anInstantGrantIsNotLost();
    void anInstantDeclineIsReported();
    void aPortalThatUsesAnotherPathIsStillFollowed();

private:
    QProcess m_daemon;
    FakeCameraPortal *m_fake = nullptr;
};

void CameraPortalTest::initTestCase()
{
    const QString daemon = QStandardPaths::findExecutable("dbus-daemon");
    if (daemon.isEmpty())
        QSKIP("dbus-daemon not found; this suite needs a private bus");

    // The daemon's own session.conf rather than `--session`, which reads
    // /etc/dbus-1/session.conf; Fedora (dbus-broker) lacks it and the daemon
    // exits with "Configuration file needs one or more <listen> elements".
    QStringList args{QStringLiteral("--nofork"),
                     QStringLiteral("--print-address=1")};
    const QString packaged = QFileInfo(daemon).absolutePath()
        + QStringLiteral("/../share/dbus-1/session.conf");
    if (QFileInfo::exists(packaged))
        args << QStringLiteral("--config-file=%1")
                    .arg(QFileInfo(packaged).canonicalFilePath());
    else
        args << QStringLiteral("--session");
    m_daemon.start(daemon, args);
    QVERIFY(m_daemon.waitForStarted(5000));
    QVERIFY(m_daemon.waitForReadyRead(5000));
    const QByteArray address = m_daemon.readLine().trimmed();
    QVERIFY(!address.isEmpty());

    // QDBusConnection::sessionBus() reads this on first use, which has not
    // happened yet.
    qputenv("DBUS_SESSION_BUS_ADDRESS", address);
    QVERIFY(QDBusConnection::sessionBus().isConnected());

    QDBusConnection fakeBus = QDBusConnection::connectToBus(
        QString::fromLatin1(address), QStringLiteral("fake-portal"));
    QVERIFY(fakeBus.isConnected());
    QVERIFY(fakeBus.registerService(QString::fromLatin1(kPortalService)));
    m_fake = new FakeCameraPortal(fakeBus);
    QVERIFY(fakeBus.registerVirtualObject(QString::fromLatin1(kPortalPath),
                                          m_fake));
}

void CameraPortalTest::cleanupTestCase()
{
    QDBusConnection::disconnectFromBus(QStringLiteral("fake-portal"));
    delete m_fake;
    m_fake = nullptr;
    if (m_daemon.state() != QProcess::NotRunning) {
        m_daemon.kill();
        m_daemon.waitForFinished(3000);
    }
}

void CameraPortalTest::thePredictedPathFollowsThePortalSpec()
{
    // The shape seen on a live portal: sender :1.189, token lightprobe27560
    // -> .../request/1_189/lightprobe27560.
    QCOMPARE(portal::predictedRequestPath(QStringLiteral(":1.189"),
                                          QStringLiteral("lightprobe27560")),
             QStringLiteral("/org/freedesktop/portal/desktop/request/1_189/"
                            "lightprobe27560"));
    QVERIFY(portal::predictedRequestPath(QString(), QStringLiteral("t"))
                .isEmpty());
    QVERIFY(portal::predictedRequestPath(QStringLiteral(":1.2"), QString())
                .isEmpty());
}

void CameraPortalTest::anInstantGrantIsNotLost()
{
    CameraPortal portal;
    QSignalSpy ready(&portal, &CameraPortal::ready);
    QSignalSpy failed(&portal, &CameraPortal::failed);
    QSignalSpy cancelled(&portal, &CameraPortal::cancelled);
    m_fake->answer = FakeCameraPortal::Answer::InstantGrant;
    const int opensBefore = m_fake->openCalls;

    portal.requestAccess();
    const int fixtureErrorsBefore = m_fake->fixtureErrors;

    // Generous: the race does not fail slowly, it never answers (only the
    // 120 s timeout would end it).
    QVERIFY2(ready.wait(5000),
             "the portal granted the camera in the same breath as its reply, "
             "and CameraPortal missed the grant");
    QCOMPARE(m_fake->fixtureErrors, fixtureErrorsBefore);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(cancelled.count(), 0);
    QCOMPARE(m_fake->openCalls, opensBefore + 1);
    const int fd = ready.takeFirst().at(0).toInt();
    QVERIFY(fd >= 0);
    ::close(fd);
}

void CameraPortalTest::anInstantDeclineIsReported()
{
    CameraPortal portal;
    QSignalSpy ready(&portal, &CameraPortal::ready);
    QSignalSpy cancelled(&portal, &CameraPortal::cancelled);
    m_fake->answer = FakeCameraPortal::Answer::InstantDecline;

    portal.requestAccess();

    QVERIFY2(cancelled.wait(5000),
             "a stored 'no' arrives as fast as a stored 'yes' and must not "
             "be lost either");
    QCOMPARE(ready.count(), 0);
}

void CameraPortalTest::aPortalThatUsesAnotherPathIsStillFollowed()
{
    CameraPortal portal;
    QSignalSpy ready(&portal, &CameraPortal::ready);
    m_fake->answer = FakeCameraPortal::Answer::LateGrantOnOtherPath;

    portal.requestAccess();

    QVERIFY2(ready.wait(5000),
             "a portal older than 0.9 returns a path nobody predicted; the "
             "returned path must still be followed");
    ::close(ready.takeFirst().at(0).toInt());
}

QTEST_GUILESS_MAIN(CameraPortalTest)
#include "CameraPortalTest.moc"
