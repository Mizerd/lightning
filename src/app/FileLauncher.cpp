#include "app/FileLauncher.h"

#include "app/SaveNaming.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QTimer>

#include <memory>
#include <utility>

#if defined(Q_OS_LINUX)
#include <sys/xattr.h>
#endif

#ifdef HAVE_QT_DBUS
#include "calls/PortalRequest.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QMap>
#include <QVariantMap>
#endif

namespace {
Q_LOGGING_CATEGORY(lcLauncher, "lightning.files.launch")

#ifdef HAVE_QT_DBUS
// Generous: the file manager may be D-Bus activated, which starts it.
constexpr int kRevealTimeoutMs = 8000;
// OpenURI answers its Request once the file manager or the handler has been
// asked. A portal that never answers had accepted the call, so the request
// counts as handed over then rather than opening a second window over it.
constexpr int kResponseTimeoutMs = 10000;
constexpr auto kPortalService = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kOpenUri = "org.freedesktop.portal.OpenURI";
constexpr auto kRequest = "org.freedesktop.portal.Request";
#endif

// The document portal records each document's real location in this
// attribute (xdg-desktop-portal's Documents interface documentation).
[[maybe_unused]] constexpr auto kHostPathAttribute =
    "user.document-portal.host-path";

// One answer, however many paths try to give it.
[[maybe_unused]] std::function<void(bool)> once(std::function<void(bool)> done)
{
    auto shared = std::make_shared<std::function<void(bool)>>(std::move(done));
    return [shared](bool ok) {
        if (!*shared)
            return;
        const std::function<void(bool)> f = std::exchange(*shared, nullptr);
        f(ok);
    };
}

[[maybe_unused]] QString hostPathFromBytes(QByteArray bytes)
{
    // `ay` paths come NUL-terminated.
    while (bytes.endsWith('\0'))
        bytes.chop(1);
    const QString path = QFile::decodeName(bytes);
    return QDir::isAbsolutePath(path) ? QDir::cleanPath(path) : QString();
}
} // namespace

#ifdef HAVE_QT_DBUS
/// Subscribes to one OpenURI Request's Response, delivers it once,
/// unsubscribes. The same shape as FileChooserPortalStep (file-local moc
/// types, distinct names).
class FileLauncherPortalStep : public QObject
{
    Q_OBJECT
public:
    FileLauncherPortalStep(QObject *owner, const QString &requestPath)
        : QObject(owner), m_path(requestPath)
    {
        QDBusConnection::sessionBus().connect(
            QString(), m_path, kRequest, QStringLiteral("Response"), this,
            SLOT(onResponse(uint, QVariantMap)));
    }
    ~FileLauncherPortalStep() override
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

FileLauncher::FileLauncher(QObject *parent)
    : QObject(parent), m_platform(hostPlatform())
{
    installDefaults();
}

FileLauncher::FileLauncher(Platform platform, Hooks hooks, QObject *parent)
    : QObject(parent), m_platform(platform), m_hooks(std::move(hooks))
{
    installDefaults();
}

FileLauncher::Platform FileLauncher::hostPlatform()
{
#if defined(Q_OS_WIN)
    return Platform::Windows;
#elif defined(Q_OS_MACOS)
    return Platform::MacOS;
#else
    return Platform::Linux;
#endif
}

void FileLauncher::installDefaults()
{
    if (!m_hooks.openUrl) {
        m_hooks.openUrl = [](const QUrl &url) {
            return QDesktopServices::openUrl(url);
        };
    }
    if (!m_hooks.startDetached) {
        m_hooks.startDetached = [](const QString &program,
                                   const QStringList &arguments) {
            return QProcess::startDetached(program, arguments);
        };
    }
    if (!m_hooks.showItems) {
        m_hooks.showItems = [this](const QUrl &uri,
                                   std::function<void(bool)> done) {
#ifdef HAVE_QT_DBUS
            QDBusConnection bus = QDBusConnection::sessionBus();
            if (!bus.isConnected()) {
                done(false);
                return;
            }
            QDBusMessage call = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.FileManager1"),
                QStringLiteral("/org/freedesktop/FileManager1"),
                QStringLiteral("org.freedesktop.FileManager1"),
                QStringLiteral("ShowItems"));
            call << QStringList{QString::fromUtf8(uri.toEncoded())}
                 << QString();
            auto *watcher = new QDBusPendingCallWatcher(
                bus.asyncCall(call, kRevealTimeoutMs), this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this,
                    [done](QDBusPendingCallWatcher *w) {
                        w->deleteLater();
                        if (w->isError()) {
                            // The error NAME only: messages can carry paths.
                            qCInfo(lcLauncher)
                                << "FileManager1 unavailable error="
                                << w->error().name();
                        }
                        done(!w->isError());
                    });
#else
            (void)this;
            Q_UNUSED(uri);
            done(false);
#endif
        };
    }
    if (!m_hooks.portalOpenDirectory) {
        m_hooks.portalOpenDirectory = [this](const QString &path,
                                             std::function<void(bool)> done) {
            callOpenUriWithFd(QStringLiteral("OpenDirectory"), path,
                              std::move(done));
        };
    }
    if (!m_hooks.portalOpenFile) {
        m_hooks.portalOpenFile = [this](const QString &path,
                                        std::function<void(bool)> done) {
            callOpenUriWithFd(QStringLiteral("OpenFile"), path,
                              std::move(done));
        };
    }
    if (!m_hooks.documentHostPath) {
        m_hooks.documentHostPath = [this](const QString &path,
                                          const QString &docId,
                                          std::function<void(const QString &)>
                                              done) {
#if defined(Q_OS_LINUX)
            // The attribute first: no bus round trip. A portal that does not
            // set it answers ENODATA and the bus is asked.
            {
                QByteArray value(4096, '\0');
                const ssize_t n =
                    ::getxattr(QFile::encodeName(path).constData(),
                               kHostPathAttribute, value.data(),
                               static_cast<size_t>(value.size()));
                if (n > 0) {
                    value.truncate(static_cast<qsizetype>(n));
                    const QString host = hostPathFromBytes(value);
                    if (!host.isEmpty()) {
                        done(host);
                        return;
                    }
                }
            }
#else
            Q_UNUSED(path);
#endif
#ifdef HAVE_QT_DBUS
            QDBusConnection bus = QDBusConnection::sessionBus();
            if (docId.isEmpty() || !bus.isConnected()) {
                done(QString());
                return;
            }
            // Version 5 of the Documents portal; an older one answers
            // UnknownMethod and the folder simply stays unnamed.
            QDBusMessage call = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.portal.Documents"),
                QStringLiteral("/org/freedesktop/portal/documents"),
                QStringLiteral("org.freedesktop.portal.Documents"),
                QStringLiteral("GetHostPaths"));
            call << QStringList{docId};
            auto *watcher = new QDBusPendingCallWatcher(
                bus.asyncCall(call, kRevealTimeoutMs), this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this,
                    [done, docId](QDBusPendingCallWatcher *w) {
                        w->deleteLater();
                        const QDBusMessage reply = w->reply();
                        if (reply.type() != QDBusMessage::ReplyMessage
                            || reply.arguments().isEmpty()) {
                            qCInfo(lcLauncher)
                                << "document host path unavailable error="
                                << reply.errorName();
                            done(QString());
                            return;
                        }
                        QMap<QString, QByteArray> paths;
                        const QVariant first = reply.arguments().constFirst();
                        if (first.userType() == qMetaTypeId<QDBusArgument>())
                            first.value<QDBusArgument>() >> paths;
                        else
                            paths = first.value<QMap<QString, QByteArray>>();
                        done(hostPathFromBytes(paths.value(docId)));
                    });
#else
            (void)this;
            Q_UNUSED(docId);
            done(QString());
#endif
        };
    }
}

void FileLauncher::callOpenUriWithFd(const QString &method,
                                     const QString &path,
                                     std::function<void(bool)> done)
{
#ifdef HAVE_QT_DBUS
    const std::function<void(bool)> answer = once(std::move(done));
    QDBusConnection bus = QDBusConnection::sessionBus();
    // Read-only and close-on-exec (Qt opens every file O_CLOEXEC), which is
    // what both methods accept; the portal resolves a document-portal
    // descriptor to the real file itself.
    QFile file(path);
    if (!bus.isConnected() || !file.open(QIODevice::ReadOnly)) {
        answer(false);
        return;
    }
    const QString token = QStringLiteral("lightning_%1")
        .arg(QRandomGenerator::system()->generate64(), 0, 16);
    const auto onAnswer = [answer, method](uint response, const QVariantMap &) {
        // 1 is the user cancelling a chooser the portal showed: handled,
        // and nothing to fall back to.
        if (response > 1)
            qCInfo(lcLauncher) << "OpenURI" << method << "failed response="
                               << response;
        answer(response <= 1);
    };
    auto subscription = portal::subscribeBeforeCall<FileLauncherPortalStep>(
        this, bus.baseService(), token, onAnswer);

    QDBusMessage call = QDBusMessage::createMethodCall(
        kPortalService, kPortalPath, kOpenUri, method);
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    // The descriptor is duplicated into the message, so the QFile may close
    // when this scope ends.
    call << QString()
         << QVariant::fromValue(QDBusUnixFileDescriptor(file.handle()))
         << options;
    auto *watcher = new QDBusPendingCallWatcher(
        bus.asyncCall(call, kRevealTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, subscription, onAnswer, answer, method](
                QDBusPendingCallWatcher *w) mutable {
                w->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *w;
                if (reply.isError()) {
                    portal::dropSubscription(subscription);
                    // The error NAME only: messages can carry paths.
                    qCInfo(lcLauncher) << "OpenURI" << method
                                       << "refused error="
                                       << reply.error().name();
                    answer(false);
                    return;
                }
                portal::reconcileRequestPath(this, subscription,
                                             reply.value().path(), onAnswer);
                const QPointer<FileLauncherPortalStep> step = subscription.step;
                QTimer::singleShot(kResponseTimeoutMs, this, [step, answer] {
                    if (!step)
                        return; // answered (steps delete themselves)
                    step->deleteLater();
                    answer(true);
                });
            });
#else
    Q_UNUSED(method);
    Q_UNUSED(path);
    done(false);
#endif
}

bool FileLauncher::openFile(const QString &path)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile())
        return false;
    const QString absolute = info.absoluteFilePath();
    if (m_platform == Platform::Linux
        && savenaming::isDocumentPortalPath(absolute)) {
        // The portal's own route for a file it granted. Qt's is the same
        // call plus xdg-open, so it is the fallback.
        QPointer<FileLauncher> self(this);
        m_hooks.portalOpenFile(absolute, [self, absolute](bool ok) {
            if (!self || ok)
                return;
            self->m_hooks.openUrl(QUrl::fromLocalFile(absolute));
        });
        return true;
    }
    return m_hooks.openUrl(QUrl::fromLocalFile(absolute));
}

void FileLauncher::resolveHostPath(const QString &path,
                                   std::function<void(const QString &)> done)
{
    if (!done)
        return;
    const QString docId = savenaming::documentPortalId(path);
    if (docId.isEmpty()) {
        done(savenaming::isDocumentPortalPath(path) ? QString() : path);
        return;
    }
    m_hooks.documentHostPath(path, docId, std::move(done));
}

void FileLauncher::finishReveal(const QString &how,
                                const std::function<void(const QString &)> &done)
{
    Q_EMIT revealFinished(how);
    if (done)
        done(how);
}

void FileLauncher::openContainingFolder(
    const QString &path, const std::function<void(const QString &)> &done)
{
    const QString folder = QFileInfo(path).absolutePath();
    const bool ok = m_hooks.openUrl(QUrl::fromLocalFile(folder));
    finishReveal(ok ? QStringLiteral("folder") : QStringLiteral("failed"), done);
}

void FileLauncher::revealDocument(const QString &path,
                                  std::function<void(const QString &)> done)
{
    // FileManager1 with this path would show the host the document portal's
    // own folder (named by its id) at best, so the portal goes first: it
    // resolves the descriptor to the real file and selects it.
    QPointer<FileLauncher> self(this);
    m_hooks.portalOpenDirectory(path, [self, path, done](bool ok) {
        if (!self)
            return;
        if (ok) {
            self->finishReveal(QStringLiteral("portal"), done);
            return;
        }
        self->resolveHostPath(path, [self, done](const QString &host) {
            if (!self)
                return;
            if (host.isEmpty()) {
                qCInfo(lcLauncher)
                    << "no way to show a document-portal file's folder";
                self->finishReveal(QStringLiteral("failed"), done);
                return;
            }
            self->m_hooks.showItems(
                QUrl::fromLocalFile(host), [self, done](bool shown) {
                    if (!self)
                        return;
                    self->finishReveal(shown ? QStringLiteral("file-manager")
                                             : QStringLiteral("failed"),
                                       done);
                });
        });
    });
}

bool FileLauncher::showInFolder(const QString &path,
                                std::function<void(const QString &)> done)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists())
        return false;
    const QString absolute = info.absoluteFilePath();

    switch (m_platform) {
    case Platform::Windows: {
        // Qt Creator's form: "/select," and the path as separate arguments.
        const bool ok = m_hooks.startDetached(
            QStringLiteral("explorer.exe"),
            {QStringLiteral("/select,"), QDir::toNativeSeparators(absolute)});
        if (ok)
            finishReveal(QStringLiteral("explorer"), done);
        else
            openContainingFolder(absolute, done);
        return true;
    }
    case Platform::MacOS: {
        const bool ok = m_hooks.startDetached(
            QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), absolute});
        if (ok)
            finishReveal(QStringLiteral("finder"), done);
        else
            openContainingFolder(absolute, done);
        return true;
    }
    case Platform::Linux:
        break;
    }

    if (savenaming::isDocumentPortalPath(absolute)) {
        revealDocument(absolute, std::move(done));
        return true;
    }

    // Linux: FileManager1, then the portal, then the folder. Each step answers
    // asynchronously; a guard keeps the object alive-checked.
    QPointer<FileLauncher> self(this);
    m_hooks.showItems(
        QUrl::fromLocalFile(absolute), [self, absolute, done](bool ok) {
            if (!self)
                return;
            if (ok) {
                self->finishReveal(QStringLiteral("file-manager"), done);
                return;
            }
            self->m_hooks.portalOpenDirectory(absolute, [self, absolute, done](
                                                            bool portalOk) {
                if (!self)
                    return;
                if (portalOk) {
                    self->finishReveal(QStringLiteral("portal"), done);
                    return;
                }
                self->openContainingFolder(absolute, done);
            });
        });
    return true;
}

#ifdef HAVE_QT_DBUS
#include "FileLauncher.moc"
#endif
