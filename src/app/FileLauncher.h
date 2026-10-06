#pragma once

// "Open" and "Show in folder" for a file the user downloaded.
//
//   Open            QDesktopServices::openUrl(file://...): the desktop's own
//                   handler (inside Flatpak, Qt routes it through the OpenURI
//                   portal).
//   Show in folder  Linux: org.freedesktop.FileManager1.ShowItems, which opens
//                   the folder with the file selected in Dolphin, Nautilus,
//                   Nemo, Thunar and COSMIC Files; then the OpenURI portal's
//                   OpenDirectory (a sandbox without the FileManager1 name);
//                   then the bare folder.
//                   Windows: `explorer.exe /select,<path>`.
//                   macOS: `open -R <path>`.
//
// The platform calls go through Hooks so a test can observe which path was
// taken on any host, without starting a file manager.
//
// Never opens anything on its own: both actions run only from a click, and
// the caller decides whether Open is offered at all (savenaming::isRiskyToOpen).

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class FileLauncher : public QObject
{
    Q_OBJECT

public:
    enum class Platform { Linux, Windows, MacOS };

    struct Hooks
    {
        /// QDesktopServices::openUrl.
        std::function<bool(const QUrl &)> openUrl;
        /// QProcess::startDetached(program, arguments).
        std::function<bool(const QString &, const QStringList &)> startDetached;
        /// FileManager1.ShowItems([uri]); answers asynchronously with whether
        /// the call succeeded.
        std::function<void(const QUrl &, std::function<void(bool)>)> showItems;
        /// The OpenURI portal's OpenDirectory for this file; asynchronous.
        std::function<void(const QString &, std::function<void(bool)>)>
            portalOpenDirectory;
    };

    explicit FileLauncher(QObject *parent = nullptr);
    /// For tests: a given platform and given hooks. Any hook left empty keeps
    /// its real implementation.
    FileLauncher(Platform platform, Hooks hooks, QObject *parent = nullptr);

    static Platform hostPlatform();

    /// Opens `path` with the desktop's handler. False when the file is gone or
    /// the desktop refused.
    bool openFile(const QString &path);
    /// Reveals `path` in the file manager. Asynchronous on Linux; the result,
    /// for logging and tests, arrives on revealFinished. Falls back to the
    /// containing folder. False only when the file is gone.
    bool showInFolder(const QString &path);

    Platform platform() const { return m_platform; }

Q_SIGNALS:
    /// How the last showInFolder() ended: "file-manager", "portal", "folder",
    /// "explorer", "finder" or "failed".
    void revealFinished(const QString &how);

private:
    void installDefaults();
    void openContainingFolder(const QString &path);

    Platform m_platform;
    Hooks m_hooks;
};
