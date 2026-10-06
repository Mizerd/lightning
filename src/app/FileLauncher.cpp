#include "app/FileLauncher.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusUnixFileDescriptor>
#include <QVariantMap>
#endif

namespace {
Q_LOGGING_CATEGORY(lcLauncher, "lightning.files.launch")

#ifdef HAVE_QT_DBUS
// Generous: the file manager may be D-Bus activated, which starts it.
constexpr int kRevealTimeoutMs = 8000;
#endif
} // namespace

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
#ifdef HAVE_QT_DBUS
            QDBusConnection bus = QDBusConnection::sessionBus();
            QFile file(path);
            if (!bus.isConnected() || !file.open(QIODevice::ReadOnly)) {
                done(false);
                return;
            }
            QDBusMessage call = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.portal.Desktop"),
                QStringLiteral("/org/freedesktop/portal/desktop"),
                QStringLiteral("org.freedesktop.portal.OpenURI"),
                QStringLiteral("OpenDirectory"));
            QVariantMap options;
            options.insert(
                QStringLiteral("handle_token"),
                QStringLiteral("lightning_%1")
                    .arg(QRandomGenerator::system()->generate64(), 0, 16));
            // The descriptor is duplicated into the message, so the QFile
            // may close when this scope ends.
            call << QString() << QVariant::fromValue(
                        QDBusUnixFileDescriptor(file.handle()))
                 << options;
            auto *watcher = new QDBusPendingCallWatcher(
                bus.asyncCall(call, kRevealTimeoutMs), this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this,
                    [done](QDBusPendingCallWatcher *w) {
                        w->deleteLater();
                        done(!w->isError());
                    });
#else
            (void)this;
            Q_UNUSED(path);
            done(false);
#endif
        };
    }
}

bool FileLauncher::openFile(const QString &path)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile())
        return false;
    return m_hooks.openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
}

void FileLauncher::openContainingFolder(const QString &path)
{
    const QString folder = QFileInfo(path).absolutePath();
    const bool ok = m_hooks.openUrl(QUrl::fromLocalFile(folder));
    Q_EMIT revealFinished(ok ? QStringLiteral("folder")
                             : QStringLiteral("failed"));
}

bool FileLauncher::showInFolder(const QString &path)
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
            Q_EMIT revealFinished(QStringLiteral("explorer"));
        else
            openContainingFolder(absolute);
        return true;
    }
    case Platform::MacOS: {
        const bool ok = m_hooks.startDetached(
            QStringLiteral("/usr/bin/open"), {QStringLiteral("-R"), absolute});
        if (ok)
            Q_EMIT revealFinished(QStringLiteral("finder"));
        else
            openContainingFolder(absolute);
        return true;
    }
    case Platform::Linux:
        break;
    }

    // Linux: FileManager1, then the portal, then the folder. Each step answers
    // asynchronously; a guard keeps the object alive-checked.
    QPointer<FileLauncher> self(this);
    m_hooks.showItems(
        QUrl::fromLocalFile(absolute), [self, absolute](bool ok) {
            if (!self)
                return;
            if (ok) {
                Q_EMIT self->revealFinished(QStringLiteral("file-manager"));
                return;
            }
            self->m_hooks.portalOpenDirectory(absolute, [self, absolute](
                                                            bool portalOk) {
                if (!self)
                    return;
                if (portalOk) {
                    Q_EMIT self->revealFinished(QStringLiteral("portal"));
                    return;
                }
                self->openContainingFolder(absolute);
            });
        });
    return true;
}
