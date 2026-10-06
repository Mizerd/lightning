// FileChooser (app.files): which chooser Lightning shows, and what it sends
// the XDG desktop portal.
//
// Reported 2026-10-06 ("this menu super sucks"): every file dialog was Qt
// Quick's built-in FileDialog, which draws a non-native imitation (a fake
// path bar, no thumbnails, a cramped filter box) whenever the Qt platform
// theme offers no native dialog — the case for every build that does not load
// the desktop's Qt plugin. Now every chooser goes through FileChooser: the
// portal on Linux (KDE's, GNOME's, COSMIC's own dialog, and the only one a
// Flatpak or Snap may use), QFileDialog otherwise.
//
// The portal here is a fake on a PRIVATE dbus-daemon whose configuration has
// no service directories, so nothing on this machine can be activated on it:
// a real portal, and with it a real dialog on the desktop, is unreachable
// from this test. The Qt route is an injected runner; no dialog is shown.

#include "app/FileChooser.h"
#include "calls/PortalRequest.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kFileChooser = "org.freedesktop.portal.FileChooser";
constexpr auto kRequest = "org.freedesktop.portal.Request";

} // namespace

// The portal's a(sa(us)) filter type, decoded on the fake's side.
namespace test_portal {
struct Rule
{
    uint type = 0;
    QString pattern;
};
struct Filter
{
    QString name;
    QList<Rule> rules;
};
const QDBusArgument &operator>>(const QDBusArgument &arg, Rule &rule)
{
    arg.beginStructure();
    arg >> rule.type >> rule.pattern;
    arg.endStructure();
    return arg;
}
QDBusArgument &operator<<(QDBusArgument &arg, const Rule &rule)
{
    arg.beginStructure();
    arg << rule.type << rule.pattern;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, Filter &filter)
{
    arg.beginStructure();
    arg >> filter.name >> filter.rules;
    arg.endStructure();
    return arg;
}
QDBusArgument &operator<<(QDBusArgument &arg, const Filter &filter)
{
    arg.beginStructure();
    arg << filter.name << filter.rules;
    arg.endStructure();
    return arg;
}
} // namespace test_portal
Q_DECLARE_METATYPE(test_portal::Rule)
Q_DECLARE_METATYPE(test_portal::Filter)

namespace {

class FakeFileChooserPortal : public QDBusVirtualObject
{
public:
    enum class Answer { Choose, Cancel, RefuseCall };

    explicit FakeFileChooserPortal(QDBusConnection bus) : m_bus(bus) {}

    Answer answer = Answer::Choose;
    QStringList uris;
    // What the last call carried.
    QString member;
    QString title;
    QVariantMap options;
    int calls = 0;

    QString introspect(const QString &) const override { return {}; }

    bool handleMessage(const QDBusMessage &message,
                       const QDBusConnection &) override
    {
        if (message.interface() == QLatin1String("org.freedesktop.DBus.Properties")
            && message.member() == QLatin1String("Get")) {
            const QStringList args{message.arguments().value(0).toString(),
                                   message.arguments().value(1).toString()};
            if (args.at(0) != QLatin1String(kFileChooser)
                || args.at(1) != QLatin1String("version"))
                return false;
            m_bus.send(message.createReply(QVariant::fromValue(
                QDBusVariant(QVariant::fromValue(uint(4))))));
            return true;
        }
        if (message.interface() != QLatin1String(kFileChooser))
            return false;
        ++calls;
        member = message.member();
        title = message.arguments().value(1).toString();
        // A virtual object gets the raw a{sv} as a QDBusArgument.
        options = qdbus_cast<QVariantMap>(message.arguments().value(2));
        if (answer == Answer::RefuseCall) {
            m_bus.send(message.createErrorReply(
                QStringLiteral("org.freedesktop.portal.Error.Failed"),
                QStringLiteral("no backend")));
            return true;
        }
        const QString token =
            options.value(QStringLiteral("handle_token")).toString();
        const QString path =
            portal::predictedRequestPath(message.service(), token);
        m_bus.send(message.createReply(
            QVariant::fromValue(QDBusObjectPath(path))));
        QDBusMessage response = QDBusMessage::createTargetedSignal(
            message.service(), path, QString::fromLatin1(kRequest),
            QStringLiteral("Response"));
        QVariantMap results;
        if (answer == Answer::Choose)
            results.insert(QStringLiteral("uris"), uris);
        response << (answer == Answer::Choose ? 0u : 1u) << results;
        m_bus.send(response);
        return true;
    }

    QList<test_portal::Filter> filters() const
    {
        const QVariant v = options.value(QStringLiteral("filters"));
        if (v.userType() != qMetaTypeId<QDBusArgument>())
            return {};
        return qdbus_cast<QList<test_portal::Filter>>(v.value<QDBusArgument>());
    }
    test_portal::Filter currentFilter() const
    {
        const QVariant v = options.value(QStringLiteral("current_filter"));
        if (v.userType() != qMetaTypeId<QDBusArgument>())
            return {};
        return qdbus_cast<test_portal::Filter>(v.value<QDBusArgument>());
    }

private:
    QDBusConnection m_bus;
};

FileChooser::Environment linuxWithBus()
{
    FileChooser::Environment env;
    env.linuxDesktop = true;
    env.sessionBus = true;
    return env;
}

} // namespace

class FileChooserTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void theRouteFollowsThePlatformAndTheOverride();
    void nameFiltersAreParsedLikeQtDoes();
    void theRequestIdIsReturnedBeforeAnyAnswer();
    void thePortalOpensSeveralFilesFromTheRememberedFolder();
    void thePortalSaveDialogIsPrefilledWithNameAndType();
    void aCancelledPortalDialogIsARejection();
    void aPortalThatRefusesTheCallFallsBackToQt();
    void withoutAPortalQtsDialogIsUsed(); // last: unregisters the fake

private:
    QProcess m_daemon;
    QTemporaryDir m_busDir;
    FakeFileChooserPortal *m_fake = nullptr;
    bool m_haveBus = false;
};

void FileChooserTest::initTestCase()
{
    qDBusRegisterMetaType<test_portal::Rule>();
    qDBusRegisterMetaType<QList<test_portal::Rule>>();
    qDBusRegisterMetaType<test_portal::Filter>();
    qDBusRegisterMetaType<QList<test_portal::Filter>>();

#ifndef HAVE_QT_DBUS
    return;
#else
    const QString daemon = QStandardPaths::findExecutable("dbus-daemon");
    if (daemon.isEmpty())
        return; // the portal cases skip
    QVERIFY(m_busDir.isValid());
    // Our own configuration: a session bus with NO service directories, so
    // no real portal (and no real dialog) can ever be activated on it.
    const QString config = m_busDir.filePath(QStringLiteral("bus.conf"));
    {
        QFile f(config);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(QStringLiteral(
                    "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus "
                    "Configuration 1.0//EN\" \"http://www.freedesktop.org/"
                    "standards/dbus/1.0/busconfig.dtd\">\n"
                    "<busconfig>\n"
                    "  <type>session</type>\n"
                    "  <listen>unix:dir=%1</listen>\n"
                    "  <policy context=\"default\">\n"
                    "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                    "    <allow eavesdrop=\"true\"/>\n"
                    "    <allow own=\"*\"/>\n"
                    "  </policy>\n"
                    "</busconfig>\n")
                    .arg(m_busDir.path())
                    .toUtf8());
    }
    m_daemon.start(daemon, {QStringLiteral("--nofork"),
                            QStringLiteral("--print-address=1"),
                            QStringLiteral("--config-file=%1").arg(config)});
    QVERIFY(m_daemon.waitForStarted(5000));
    QVERIFY(m_daemon.waitForReadyRead(5000));
    const QByteArray address = m_daemon.readLine().trimmed();
    QVERIFY(!address.isEmpty());

    // sessionBus() reads this on first use, which has not happened yet: this
    // is a QCoreApplication, so no platform theme has opened the real bus.
    qputenv("DBUS_SESSION_BUS_ADDRESS", address);
    QVERIFY(QDBusConnection::sessionBus().isConnected());

    QDBusConnection fakeBus = QDBusConnection::connectToBus(
        QString::fromLatin1(address), QStringLiteral("fake-file-portal"));
    QVERIFY(fakeBus.isConnected());
    QVERIFY(fakeBus.registerService(QString::fromLatin1(kPortalService)));
    m_fake = new FakeFileChooserPortal(fakeBus);
    QVERIFY(fakeBus.registerVirtualObject(QString::fromLatin1(kPortalPath),
                                          m_fake));
    // The bus FileChooser uses must be the one the fake owns the name on.
    QCOMPARE(QDBusConnection::sessionBus().interface()
                 ->serviceOwner(QString::fromLatin1(kPortalService)).value(),
             fakeBus.baseService());
    m_haveBus = true;
#endif
}

void FileChooserTest::cleanupTestCase()
{
    QDBusConnection::disconnectFromBus(QStringLiteral("fake-file-portal"));
    delete m_fake;
    m_fake = nullptr;
    if (m_daemon.state() != QProcess::NotRunning) {
        m_daemon.kill();
        m_daemon.waitForFinished(3000);
    }
}

void FileChooserTest::theRouteFollowsThePlatformAndTheOverride()
{
    using R = FileChooser::Route;
    FileChooser::Environment env;
    env.linuxDesktop = true;
    env.sessionBus = true;
    QCOMPARE(FileChooser::initialRoute(env), R::Portal);
    env.sessionBus = false; // no bus, no portal
    QCOMPARE(FileChooser::initialRoute(env), R::QtDialog);
    env = {};
    env.sessionBus = true; // Windows / macOS: the system dialog via Qt
    QCOMPARE(FileChooser::initialRoute(env), R::QtDialog);
    env = linuxWithBus();
    env.override = QStringLiteral("qt");
    QCOMPARE(FileChooser::initialRoute(env), R::QtDialog);
    env.override = QStringLiteral("widget");
    QCOMPARE(FileChooser::initialRoute(env), R::QtWidgetDialog);
    env.override = QStringLiteral("PORTAL");
    env.linuxDesktop = false;
    QCOMPARE(FileChooser::initialRoute(env), R::Portal);
    env.sessionBus = false;
    QCOMPARE(FileChooser::initialRoute(env), R::QtDialog);
}

void FileChooserTest::nameFiltersAreParsedLikeQtDoes()
{
    auto f = FileChooser::parseNameFilter(
        QStringLiteral("Images (*.png *.jpg *.jpeg)"));
    QCOMPARE(f.first, QStringLiteral("Images"));
    QCOMPARE(f.second, (QStringList{QStringLiteral("*.png"),
                                    QStringLiteral("*.jpg"),
                                    QStringLiteral("*.jpeg")}));
    f = FileChooser::parseNameFilter(QStringLiteral("All files (*)"));
    QCOMPARE(f.first, QStringLiteral("All files"));
    QCOMPARE(f.second, QStringList{QStringLiteral("*")});
    f = FileChooser::parseNameFilter(QStringLiteral("*.txt"));
    QCOMPARE(f.first, QStringLiteral("*.txt"));
    QCOMPARE(f.second, QStringList{QStringLiteral("*.txt")});
}

void FileChooserTest::theRequestIdIsReturnedBeforeAnyAnswer()
{
    // QML assigns the id open() returns and ignores answers for other ids, so
    // an answer delivered inside open() would be lost.
    FileChooser chooser{FileChooser::Environment{}};
    FileChooser::Request seen;
    chooser.setQtRunner([&seen](const FileChooser::Request &r, bool,
                                FileChooser::Completion done) {
        seen = r;
        done(true, {QUrl::fromLocalFile(QStringLiteral("/tmp/name.pdf"))}, {});
    });
    QSignalSpy finished(&chooser, &FileChooser::finished);
    const int id = chooser.open(QVariantMap{
        {QStringLiteral("mode"), QStringLiteral("save")},
        {QStringLiteral("title"), QStringLiteral("Export room")},
        {QStringLiteral("currentName"), QStringLiteral("file:///name.pdf")},
    });
    QVERIFY(id > 0);
    QCOMPARE(finished.count(), 0);
    QVERIFY(finished.wait(2000));
    QCOMPARE(finished.at(0).at(0).toInt(), id);
    QCOMPARE(finished.at(0).at(1).toBool(), true);
    QCOMPARE(seen.mode, FileChooser::Mode::SaveFile);
    QCOMPARE(seen.currentName, QStringLiteral("name.pdf"));
    QCOMPARE(chooser.lastUsedRoute(), FileChooser::Route::QtDialog);
}

void FileChooserTest::thePortalOpensSeveralFilesFromTheRememberedFolder()
{
    if (!m_haveBus)
        QSKIP("needs QtDBus and dbus-daemon for a private bus");
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    FileChooser chooser(linuxWithBus());
    int runnerCalls = 0;
    chooser.setQtRunner([&runnerCalls](const FileChooser::Request &, bool,
                                       FileChooser::Completion done) {
        ++runnerCalls;
        done(false, {}, {});
    });
    QString recalled;
    QList<QPair<QString, QUrl>> remembered;
    chooser.setFolderMemory(
        [&](const QString &purpose) {
            recalled = purpose;
            return QUrl::fromLocalFile(folder.path());
        },
        [&](const QString &purpose, const QUrl &dir) {
            remembered.append({purpose, dir});
        });

    m_fake->answer = FakeFileChooserPortal::Answer::Choose;
    const QString a = folder.filePath(QStringLiteral("a.png"));
    const QString b = folder.filePath(QStringLiteral("b c.png"));
    m_fake->uris = {QString::fromUtf8(QUrl::fromLocalFile(a).toEncoded()),
                    QString::fromUtf8(QUrl::fromLocalFile(b).toEncoded())};

    FileChooser::Request request;
    request.mode = FileChooser::Mode::OpenFiles;
    request.title = QStringLiteral("Attach files");
    request.nameFilters = {QStringLiteral("Images (*.png *.jpg)"),
                           QStringLiteral("All files (*)")};
    request.purpose = QStringLiteral("attach");
    QSignalSpy finished(&chooser, &FileChooser::finished);
    const int id = chooser.open(request, {});
    QVERIFY(finished.wait(5000));

    QCOMPARE(finished.at(0).at(0).toInt(), id);
    QCOMPARE(finished.at(0).at(1).toBool(), true);
    const auto urls = finished.at(0).at(2).value<QList<QUrl>>();
    QCOMPARE(urls, (QList<QUrl>{QUrl::fromLocalFile(a), QUrl::fromLocalFile(b)}));
    QCOMPARE(runnerCalls, 0);
    QCOMPARE(chooser.lastUsedRoute(), FileChooser::Route::Portal);

    QCOMPARE(m_fake->member, QStringLiteral("OpenFile"));
    QCOMPARE(m_fake->title, QStringLiteral("Attach files"));
    QCOMPARE(m_fake->options.value(QStringLiteral("multiple")).toBool(), true);
    QByteArray expectedFolder = QFile::encodeName(folder.path());
    expectedFolder.append('\0');
    QCOMPARE(recalled, QStringLiteral("attach"));
    QCOMPARE(m_fake->options.value(QStringLiteral("current_folder")).toByteArray(),
             expectedFolder);
    const auto filters = m_fake->filters();
    QCOMPARE(filters.size(), 2);
    QCOMPARE(filters.at(0).name, QStringLiteral("Images"));
    QCOMPARE(filters.at(0).rules.size(), 2);
    QCOMPARE(filters.at(0).rules.at(1).pattern, QStringLiteral("*.jpg"));
    QCOMPARE(filters.at(0).rules.at(1).type, 0u);
    QCOMPARE(filters.at(1).name, QStringLiteral("All files"));

    // The folder the files came from is remembered for "attach".
    QCOMPARE(remembered.size(), 1);
    QCOMPARE(remembered.at(0).first, QStringLiteral("attach"));
    QCOMPARE(remembered.at(0).second, QUrl::fromLocalFile(folder.path()));
}

void FileChooserTest::thePortalSaveDialogIsPrefilledWithNameAndType()
{
    if (!m_haveBus)
        QSKIP("needs QtDBus and dbus-daemon for a private bus");
    FileChooser chooser(linuxWithBus());
    m_fake->answer = FakeFileChooserPortal::Answer::Choose;
    m_fake->uris = {QStringLiteral("file:///tmp/report.pdf")};

    FileChooser::Request request;
    request.mode = FileChooser::Mode::SaveFile;
    request.title = QStringLiteral("Save file");
    request.currentName = QStringLiteral("report.pdf");
    request.nameFilters = {QStringLiteral("PDF file (*.pdf)"),
                           QStringLiteral("All files (*)")};
    request.selectedNameFilter = request.nameFilters.at(0);
    QSignalSpy finished(&chooser, &FileChooser::finished);
    chooser.open(request, {});
    QVERIFY(finished.wait(5000));

    QCOMPARE(m_fake->member, QStringLiteral("SaveFile"));
    QCOMPARE(m_fake->options.value(QStringLiteral("current_name")).toString(),
             QStringLiteral("report.pdf"));
    QCOMPARE(m_fake->currentFilter().name, QStringLiteral("PDF file"));
    QCOMPARE(m_fake->currentFilter().rules.value(0).pattern,
             QStringLiteral("*.pdf"));
    QVERIFY(!m_fake->options.contains(QStringLiteral("multiple")));
    QCOMPARE(finished.at(0).at(1).toBool(), true);
}

void FileChooserTest::aCancelledPortalDialogIsARejection()
{
    if (!m_haveBus)
        QSKIP("needs QtDBus and dbus-daemon for a private bus");
    FileChooser chooser(linuxWithBus());
    int remembered = 0;
    chooser.setFolderMemory([](const QString &) { return QUrl(); },
                            [&](const QString &, const QUrl &) { ++remembered; });
    m_fake->answer = FakeFileChooserPortal::Answer::Cancel;
    FileChooser::Request request;
    request.purpose = QStringLiteral("image");
    QSignalSpy finished(&chooser, &FileChooser::finished);
    chooser.open(request, {});
    QVERIFY(finished.wait(5000));
    QCOMPARE(finished.at(0).at(1).toBool(), false);
    QCOMPARE(remembered, 0);
}

void FileChooserTest::aPortalThatRefusesTheCallFallsBackToQt()
{
    if (!m_haveBus)
        QSKIP("needs QtDBus and dbus-daemon for a private bus");
    FileChooser chooser(linuxWithBus());
    QList<FileChooser::Request> ran;
    chooser.setQtRunner([&ran](const FileChooser::Request &r, bool,
                               FileChooser::Completion done) {
        ran.append(r);
        done(true, {QUrl::fromLocalFile(QStringLiteral("/tmp/x.png"))}, {});
    });
    m_fake->answer = FakeFileChooserPortal::Answer::RefuseCall;
    FileChooser::Request request;
    request.title = QStringLiteral("Choose a sticker");
    QSignalSpy finished(&chooser, &FileChooser::finished);
    chooser.open(request, {});
    QVERIFY(finished.wait(5000));
    QCOMPARE(ran.size(), 1);
    QCOMPARE(ran.at(0).title, QStringLiteral("Choose a sticker"));
    QCOMPARE(finished.at(0).at(1).toBool(), true);
    QCOMPARE(chooser.route(), FileChooser::Route::QtDialog);
    QCOMPARE(chooser.lastUsedRoute(), FileChooser::Route::QtDialog);
    m_fake->answer = FakeFileChooserPortal::Answer::Choose;
}

void FileChooserTest::withoutAPortalQtsDialogIsUsed()
{
    if (!m_haveBus)
        QSKIP("needs QtDBus and dbus-daemon for a private bus");
    QDBusConnection fakeBus(QStringLiteral("fake-file-portal"));
    fakeBus.unregisterObject(QString::fromLatin1(kPortalPath));
    QVERIFY(fakeBus.unregisterService(QString::fromLatin1(kPortalService)));
    const int portalCallsBefore = m_fake->calls;

    FileChooser chooser(linuxWithBus());
    QSignalSpy routeChanged(&chooser, &FileChooser::routeChanged);
    int runnerCalls = 0;
    chooser.setQtRunner([&runnerCalls](const FileChooser::Request &, bool,
                                       FileChooser::Completion done) {
        ++runnerCalls;
        done(false, {}, {});
    });
    QCOMPARE(chooser.route(), FileChooser::Route::Portal); // not probed yet
    QSignalSpy finished(&chooser, &FileChooser::finished);
    chooser.open(FileChooser::Request{}, {});
    QVERIFY(finished.wait(8000));
    QCOMPARE(runnerCalls, 1);
    QCOMPARE(m_fake->calls, portalCallsBefore);
    QCOMPARE(chooser.route(), FileChooser::Route::QtDialog);
    QCOMPARE(routeChanged.count(), 1);
}

QTEST_GUILESS_MAIN(FileChooserTest)
#include "FileChooserTest.moc"
