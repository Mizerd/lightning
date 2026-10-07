#include "app/FileChooser.h"

#include "app/SaveNaming.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QWindow>

#include <utility>

#ifdef HAVE_QT_DBUS
#include "calls/PortalRequest.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#endif

namespace {
Q_LOGGING_CATEGORY(lcChooser, "lightning.files.chooser")

#ifdef HAVE_QT_DBUS
constexpr auto kService = "org.freedesktop.portal.Desktop";
constexpr auto kPath = "/org/freedesktop/portal/desktop";
constexpr auto kFileChooser = "org.freedesktop.portal.FileChooser";
constexpr auto kRequest = "org.freedesktop.portal.Request";
// The portal is D-Bus activated, and starting it (and its desktop backend)
// the first time can take a second or two.
constexpr int kProbeTimeoutMs = 4000;
// OpenFile/SaveFile return their Request path at once; the dialog's answer
// comes later on Response, with no timeout (the user may take minutes).
constexpr int kCallTimeoutMs = 10000;
#endif

QString modeName(FileChooser::Mode mode)
{
    switch (mode) {
    case FileChooser::Mode::OpenFile: return QStringLiteral("open");
    case FileChooser::Mode::OpenFiles: return QStringLiteral("openMany");
    case FileChooser::Mode::SaveFile: return QStringLiteral("save");
    case FileChooser::Mode::Folder: return QStringLiteral("folder");
    }
    return {};
}
} // namespace

#ifdef HAVE_QT_DBUS
namespace lightning_portal {
// org.freedesktop.portal.FileChooser's filter type: a(sa(us)), where each rule
// is (0 = glob, 1 = MIME type, pattern).
struct FilterRule
{
    uint type = 0;
    QString pattern;
};
struct Filter
{
    QString name;
    QList<FilterRule> rules;
};

QDBusArgument &operator<<(QDBusArgument &arg, const FilterRule &rule)
{
    arg.beginStructure();
    arg << rule.type << rule.pattern;
    arg.endStructure();
    return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, FilterRule &rule)
{
    arg.beginStructure();
    arg >> rule.type >> rule.pattern;
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
const QDBusArgument &operator>>(const QDBusArgument &arg, Filter &filter)
{
    arg.beginStructure();
    arg >> filter.name >> filter.rules;
    arg.endStructure();
    return arg;
}
} // namespace lightning_portal

Q_DECLARE_METATYPE(lightning_portal::FilterRule)
Q_DECLARE_METATYPE(lightning_portal::Filter)

/// Subscribes to one Request's Response, delivers it once, unsubscribes. The
/// same shape as CameraPortalStep (file-local moc types, distinct names).
class FileChooserPortalStep : public QObject
{
    Q_OBJECT
public:
    FileChooserPortalStep(QObject *owner, const QString &requestPath)
        : QObject(owner), m_path(requestPath)
    {
        QDBusConnection::sessionBus().connect(
            QString(), m_path, kRequest, QStringLiteral("Response"), this,
            SLOT(onResponse(uint, QVariantMap)));
    }
    ~FileChooserPortalStep() override
    {
        QDBusConnection::sessionBus().disconnect(
            QString(), m_path, kRequest, QStringLiteral("Response"), this,
            SLOT(onResponse(uint, QVariantMap)));
    }

Q_SIGNALS:
    void answered(uint response, const QVariantMap &results);

private Q_SLOTS:
    void onResponse(uint response, const QVariantMap &results)
    {
        Q_EMIT answered(response, results);
        deleteLater();
    }

private:
    QString m_path;
};
#endif

FileChooser::FileChooser(QObject *parent)
    : FileChooser(hostEnvironment(), parent)
{
}

FileChooser::FileChooser(const Environment &env, QObject *parent)
    : QObject(parent), m_route(initialRoute(env))
{
    initialise();
}

FileChooser::~FileChooser() = default;

void FileChooser::initialise()
{
#ifdef HAVE_QT_DBUS
    static const bool registered = [] {
        qDBusRegisterMetaType<lightning_portal::FilterRule>();
        qDBusRegisterMetaType<QList<lightning_portal::FilterRule>>();
        qDBusRegisterMetaType<lightning_portal::Filter>();
        qDBusRegisterMetaType<QList<lightning_portal::Filter>>();
        return true;
    }();
    Q_UNUSED(registered);
#endif
    m_qtRunner = [](const Request &request, bool widgetOnly,
                    Completion done) {
        auto *dialog = new QFileDialog(nullptr, request.title,
                                       request.folder.isLocalFile()
                                           ? request.folder.toLocalFile()
                                           : QString());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::ApplicationModal);
        if (widgetOnly)
            dialog->setOption(QFileDialog::DontUseNativeDialog, true);
        switch (request.mode) {
        case Mode::OpenFile:
            dialog->setFileMode(QFileDialog::ExistingFile);
            dialog->setAcceptMode(QFileDialog::AcceptOpen);
            break;
        case Mode::OpenFiles:
            dialog->setFileMode(QFileDialog::ExistingFiles);
            dialog->setAcceptMode(QFileDialog::AcceptOpen);
            break;
        case Mode::SaveFile:
            dialog->setFileMode(QFileDialog::AnyFile);
            dialog->setAcceptMode(QFileDialog::AcceptSave);
            break;
        case Mode::Folder:
            dialog->setFileMode(QFileDialog::Directory);
            dialog->setOption(QFileDialog::ShowDirsOnly, true);
            dialog->setAcceptMode(QFileDialog::AcceptOpen);
            break;
        }
        if (!request.nameFilters.isEmpty() && request.mode != Mode::Folder) {
            dialog->setNameFilters(request.nameFilters);
            if (!request.selectedNameFilter.isEmpty())
                dialog->selectNameFilter(request.selectedNameFilter);
        }
        if (request.mode == Mode::SaveFile && !request.currentName.isEmpty())
            dialog->selectFile(request.currentName);
        if (!request.acceptLabel.isEmpty())
            dialog->setLabelText(QFileDialog::Accept, request.acceptLabel);
        // Keep the widget dialog above the window it was opened from.
        if (QWindow *parent = QGuiApplication::focusWindow()) {
            dialog->winId();
            if (QWindow *own = dialog->windowHandle())
                own->setTransientParent(parent);
        }
        QObject::connect(dialog, &QFileDialog::finished, dialog,
                         [dialog, done](int result) {
                             done(result == QDialog::Accepted,
                                  dialog->selectedUrls(),
                                  dialog->selectedNameFilter());
                         });
        dialog->open();
    };
}

FileChooser::Route FileChooser::initialRoute(const Environment &env)
{
    const QString forced = env.override.trimmed().toLower();
    if (forced == QLatin1String("qt") || forced == QLatin1String("native"))
        return Route::QtDialog;
    if (forced == QLatin1String("widget"))
        return Route::QtWidgetDialog;
    if (forced == QLatin1String("portal"))
        return env.sessionBus ? Route::Portal : Route::QtDialog;
    if (env.linuxDesktop && env.sessionBus)
        return Route::Portal;
    return Route::QtDialog;
}

FileChooser::Environment FileChooser::hostEnvironment()
{
    Environment env;
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID)
    env.linuxDesktop = true;
#endif
#ifdef HAVE_QT_DBUS
    env.sessionBus = QDBusConnection::sessionBus().isConnected();
#endif
    env.override = qEnvironmentVariable("LIGHTNING_FILE_DIALOG");
    return env;
}

void FileChooser::setFolderMemory(
    std::function<QUrl(const QString &)> recall,
    std::function<void(const QString &, const QUrl &)> remember)
{
    m_recall = std::move(recall);
    m_remember = std::move(remember);
}

void FileChooser::setDefaultFolder(std::function<QUrl()> folder)
{
    m_defaultFolder = std::move(folder);
}

QString FileChooser::routeName() const
{
    switch (m_route) {
    case Route::Portal: return QStringLiteral("portal");
    case Route::QtDialog: return QStringLiteral("qt");
    case Route::QtWidgetDialog: return QStringLiteral("widget");
    }
    return {};
}

QPair<QString, QStringList> FileChooser::parseNameFilter(const QString &filter)
{
    // Qt's own format: "Description (*.a *.b)". Anything else is a bare
    // pattern list.
    static const QRegularExpression re(
        QStringLiteral("^(.*)\\(([^()]*)\\)\\s*$"));
    const QRegularExpressionMatch m = re.match(filter.trimmed());
    QString name;
    QString patterns;
    if (m.hasMatch()) {
        name = m.captured(1).trimmed();
        patterns = m.captured(2);
    } else {
        patterns = filter;
    }
    QStringList globs = patterns.split(QRegularExpression(QStringLiteral("[\\s;]+")),
                                       Qt::SkipEmptyParts);
    if (name.isEmpty())
        name = globs.join(QLatin1Char(' '));
    return {name, globs};
}

int FileChooser::open(const QVariantMap &options)
{
    Request request;
    const QString mode = options.value(QStringLiteral("mode")).toString();
    if (mode == QLatin1String("openMany"))
        request.mode = Mode::OpenFiles;
    else if (mode == QLatin1String("save"))
        request.mode = Mode::SaveFile;
    else if (mode == QLatin1String("folder"))
        request.mode = Mode::Folder;
    else
        request.mode = Mode::OpenFile;
    request.title = options.value(QStringLiteral("title")).toString();
    request.nameFilters =
        options.value(QStringLiteral("nameFilters")).toStringList();
    request.selectedNameFilter =
        options.value(QStringLiteral("selectedNameFilter")).toString();
    const QVariant folder = options.value(QStringLiteral("folder"));
    request.folder = folder.typeId() == QMetaType::QUrl
        ? folder.toUrl()
        : QUrl::fromUserInput(folder.toString());
    if (!request.folder.isLocalFile())
        request.folder = QUrl();
    // A save dialog's name may arrive as a url ("file:///name.pdf"); only its
    // leaf is a suggestion.
    QString name = options.value(QStringLiteral("currentName")).toString();
    if (name.startsWith(QLatin1String("file:")))
        name = QUrl(name).fileName();
    request.currentName = QFileInfo(name).fileName();
    request.acceptLabel = options.value(QStringLiteral("acceptLabel")).toString();
    request.purpose = options.value(QStringLiteral("purpose")).toString();
    return open(request, {});
}

int FileChooser::open(const Request &in, Completion done)
{
    Request request = in;
    request.id = m_nextId++;
    if (!request.purpose.isEmpty() && m_recall) {
        const QUrl remembered = m_recall(request.purpose);
        // A document-portal folder (remembered by an older build) is one
        // document's mount point, not a place to start a dialog in.
        if (remembered.isLocalFile()
            && QFileInfo(remembered.toLocalFile()).isDir()
            && !savenaming::isDocumentPortalPath(remembered.toLocalFile()))
            request.folder = remembered;
    }
    if ((!request.folder.isLocalFile()
         || !QFileInfo(request.folder.toLocalFile()).isDir())
        && m_defaultFolder)
        request.folder = m_defaultFolder();
    m_requests.insert(request.id, request);
    if (done)
        m_completions.insert(request.id, std::move(done));
    qCInfo(lcChooser) << "file chooser request id=" << request.id
                      << "mode=" << modeName(request.mode)
                      << "route=" << routeName();
    // Started on the next turn, so the caller always holds the id before any
    // answer can arrive (QML assigns it from open()'s return value).
    QTimer::singleShot(0, this, [this, request] { dispatch(request); });
    return request.id;
}

void FileChooser::dispatch(const Request &request)
{
    if (m_route != Route::Portal) {
        runQt(request);
        return;
    }
    if (m_probe == Probe::Done) {
        runPortal(request);
        return;
    }
    m_waitingForProbe.append(request);
    probePortal();
}

void FileChooser::probePortal()
{
#ifdef HAVE_QT_DBUS
    if (m_probe == Probe::Running)
        return;
    m_probe = Probe::Running;
    QDBusMessage probe = QDBusMessage::createMethodCall(
        kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    probe << QString::fromLatin1(kFileChooser) << QStringLiteral("version");
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(probe, kProbeTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                const QDBusMessage reply = w->reply();
                bool ok = reply.type() == QDBusMessage::ReplyMessage
                    && !reply.arguments().isEmpty();
                if (ok) {
                    const uint version = reply.arguments()
                                             .constFirst()
                                             .value<QDBusVariant>()
                                             .variant()
                                             .toUInt();
                    ok = version >= 1;
                    qCInfo(lcChooser) << "file chooser portal version"
                                      << version;
                } else {
                    // Error name only: a message can carry paths.
                    qCInfo(lcChooser)
                        << "no file chooser portal; using Qt's dialog error="
                        << reply.errorName();
                }
                onPortalProbed(ok);
            });
#else
    onPortalProbed(false);
#endif
}

void FileChooser::onPortalProbed(bool available)
{
    m_probe = Probe::Done;
    if (!available && m_route == Route::Portal) {
        m_route = Route::QtDialog;
        Q_EMIT routeChanged();
    }
#ifdef HAVE_QT_DBUS
    if (available && !m_serviceWatcher) {
        auto *watcher = new QDBusServiceWatcher(
            QString::fromLatin1(kService), QDBusConnection::sessionBus(),
            QDBusServiceWatcher::WatchForUnregistration, this);
        connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this,
                [this] { failPendingPortalRequests(); });
        m_serviceWatcher = watcher;
    }
#endif
    const QList<Request> waiting = std::exchange(m_waitingForProbe, {});
    for (const Request &request : waiting)
        dispatch(request);
}

void FileChooser::failPendingPortalRequests()
{
    // The portal went away with a dialog open: nothing will ever answer
    // those requests, so answer them (as cancelled; reopening a dialog the
    // user did not ask for again would be stranger).
    const QList<int> ids = m_portalInFlight.values();
    m_portalInFlight.clear();
    for (int id : ids)
        complete(id, Route::Portal, false, {}, {});
}

void FileChooser::runQt(const Request &request)
{
    const Route used = m_route == Route::QtWidgetDialog ? Route::QtWidgetDialog
                                                        : Route::QtDialog;
    QPointer<FileChooser> self(this);
    const int id = request.id;
    m_qtRunner(request, used == Route::QtWidgetDialog,
               [self, id, used](bool accepted, const QList<QUrl> &urls,
                                const QString &filter) {
                   if (self)
                       self->complete(id, used, accepted, urls, filter);
               });
}

QString FileChooser::parentWindowHandle() const
{
    // X11 only: a Wayland parent needs an xdg-foreign export handle, which Qt
    // has no public API for. The portal shows an unparented dialog then.
    if (QGuiApplication::platformName() != QLatin1String("xcb"))
        return {};
    QWindow *window = QGuiApplication::focusWindow();
    if (!window) {
        const auto windows = QGuiApplication::topLevelWindows();
        for (QWindow *w : windows) {
            if (w->isVisible() && w->type() == Qt::Window) {
                window = w;
                break;
            }
        }
    }
    while (window && window->transientParent())
        window = window->transientParent();
    if (!window)
        return {};
    return QStringLiteral("x11:%1").arg(window->winId(), 0, 16);
}

void FileChooser::runPortal(const Request &request)
{
#ifdef HAVE_QT_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString token = QStringLiteral("lightning_%1")
        .arg(QRandomGenerator::system()->generate64(), 0, 16);
    const int id = request.id;
    m_portalInFlight.insert(id);

    const QStringList filters = request.nameFilters;
    const auto onAnswer = [this, id, filters](uint response,
                                              const QVariantMap &results) {
        if (!m_portalInFlight.contains(id))
            return; // already answered (the portal vanished meanwhile)
        m_portalInFlight.remove(id);
        QList<QUrl> urls;
        const QStringList uris =
            results.value(QStringLiteral("uris")).toStringList();
        for (const QString &uri : uris) {
            const QUrl url = QUrl::fromEncoded(uri.toUtf8());
            if (url.isValid())
                urls.append(url);
        }
        QString chosenFilter;
        const QVariant current = results.value(QStringLiteral("current_filter"));
        if (current.userType() == qMetaTypeId<QDBusArgument>()) {
            const auto filter = qdbus_cast<lightning_portal::Filter>(
                current.value<QDBusArgument>());
            for (const QString &f : filters) {
                if (parseNameFilter(f).first == filter.name) {
                    chosenFilter = f;
                    break;
                }
            }
        }
        if (response == 2)
            qCInfo(lcChooser) << "file chooser portal ended the request id="
                              << id;
        complete(id, Route::Portal, response == 0 && !urls.isEmpty(), urls,
                 chosenFilter);
    };

    auto subscription = portal::subscribeBeforeCall<FileChooserPortalStep>(
        this, bus.baseService(), token, onAnswer);

    const bool save = request.mode == Mode::SaveFile;
    QDBusMessage call = QDBusMessage::createMethodCall(
        kService, kPath, kFileChooser,
        save ? QStringLiteral("SaveFile") : QStringLiteral("OpenFile"));
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    options.insert(QStringLiteral("modal"), true);
    if (!request.acceptLabel.isEmpty())
        options.insert(QStringLiteral("accept_label"), request.acceptLabel);
    if (request.mode == Mode::OpenFiles)
        options.insert(QStringLiteral("multiple"), true);
    if (request.mode == Mode::Folder)
        options.insert(QStringLiteral("directory"), true);
    if (request.folder.isLocalFile()) {
        // `ay`, NUL-terminated, in the filesystem encoding.
        QByteArray folder = QFile::encodeName(request.folder.toLocalFile());
        folder.append('\0');
        options.insert(QStringLiteral("current_folder"), folder);
    }
    if (save && !request.currentName.isEmpty())
        options.insert(QStringLiteral("current_name"), request.currentName);
    if (!request.nameFilters.isEmpty() && request.mode != Mode::Folder) {
        QList<lightning_portal::Filter> list;
        lightning_portal::Filter selected;
        for (const QString &f : request.nameFilters) {
            const auto parsed = parseNameFilter(f);
            lightning_portal::Filter filter;
            filter.name = parsed.first;
            for (const QString &glob : parsed.second)
                filter.rules.append({0u, glob});
            if (filter.rules.isEmpty())
                continue;
            if (selected.name.isEmpty()
                && (request.selectedNameFilter.isEmpty()
                    || request.selectedNameFilter == f))
                selected = filter;
            list.append(filter);
        }
        if (!list.isEmpty()) {
            options.insert(QStringLiteral("filters"), QVariant::fromValue(list));
            if (!selected.name.isEmpty())
                options.insert(QStringLiteral("current_filter"),
                               QVariant::fromValue(selected));
        }
    }
    call << parentWindowHandle() << request.title << options;

    auto *watcher = new QDBusPendingCallWatcher(
        bus.asyncCall(call, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, id, request, subscription, onAnswer](
                QDBusPendingCallWatcher *w) mutable {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (reply.isError()) {
                    portal::dropSubscription(subscription);
                    if (!m_portalInFlight.contains(id))
                        return;
                    m_portalInFlight.remove(id);
                    // The portal answered the probe and refused the call
                    // (no backend for this desktop, a broken one): this
                    // request and every later one take Qt's dialog.
                    qCWarning(lcChooser)
                        << "file chooser portal call failed error="
                        << reply.error().name() << "; using Qt's dialog";
                    if (m_route == Route::Portal) {
                        m_route = Route::QtDialog;
                        Q_EMIT routeChanged();
                    }
                    runQt(request);
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
            });
#else
    runQt(request);
#endif
}

void FileChooser::complete(int id, Route used, bool accepted,
                           const QList<QUrl> &urls, const QString &filter)
{
    const auto it = m_requests.find(id);
    if (it == m_requests.end())
        return; // answered already
    const Request request = it.value();
    m_requests.erase(it);
    m_lastUsedRoute = used;
    if (accepted && !request.purpose.isEmpty() && m_remember && !urls.isEmpty()
        && urls.constFirst().isLocalFile()) {
        const QString local = urls.constFirst().toLocalFile();
        const QString folder = request.mode == Mode::Folder
            ? local
            : QFileInfo(local).absolutePath();
        // A file the portal granted to a Snap or Flatpak comes back inside
        // the document portal's mount (/run/user/<uid>/doc/<id>/<name>): its
        // folder exists for that one document only, and the next dialog
        // opened there would start in a folder named by its id.
        if (QFileInfo(folder).isDir()
            && !savenaming::isDocumentPortalPath(folder))
            m_remember(request.purpose, QUrl::fromLocalFile(folder));
    }
    const Completion done = m_completions.take(id);
    Q_EMIT finished(id, accepted, urls, filter);
    if (done)
        done(accepted, urls, filter);
}

#ifdef HAVE_QT_DBUS
#include "FileChooser.moc"
#endif
