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
// A file the document portal granted (a Snap or Flatpak that saved through
// the FileChooser portal: /run/user/<uid>/doc/<id>/<name>) is a path only the
// sandbox can use, so neither action hands it to anyone by name:
//   Open            the OpenURI portal's OpenFile with a descriptor on the
//                   file; Qt's own route if the portal fails.
//   Show in folder  the OpenURI portal's OpenDirectory with a descriptor on
//                   the file (the portal resolves the document to the real
//                   folder and selects the file); then FileManager1 with the
//                   REAL path when the Documents portal could name it; then
//                   an honest failure. Never the document's own folder, whose
//                   name is an id the user has never seen.
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
        /// True once the portal's Request answered success (or was cancelled
        /// by the user), false when the call or its Response failed.
        std::function<void(const QString &, std::function<void(bool)>)>
            portalOpenDirectory;
        /// The OpenURI portal's OpenFile for this file; asynchronous, answers
        /// like portalOpenDirectory.
        std::function<void(const QString &, std::function<void(bool)>)>
            portalOpenFile;
        /// The real location of a document-portal file (`path`, document id
        /// `docId`): the portal's host-path attribute, then
        /// org.freedesktop.portal.Documents.GetHostPaths. Empty when neither
        /// says; asynchronous.
        std::function<void(const QString &path, const QString &docId,
                           std::function<void(const QString &)>)>
            documentHostPath;
    };

    explicit FileLauncher(QObject *parent = nullptr);
    /// For tests: a given platform and given hooks. Any hook left empty keeps
    /// its real implementation.
    FileLauncher(Platform platform, Hooks hooks, QObject *parent = nullptr);

    static Platform hostPlatform();

    /// Opens `path` with the desktop's handler. False when the file is gone or
    /// the desktop refused. A document-portal file goes to the OpenURI portal
    /// (asynchronous; true once handed over).
    bool openFile(const QString &path);
    /// Reveals `path` in the file manager. Asynchronous on Linux; the result
    /// arrives on revealFinished and on `done`, if given. Falls back to the
    /// containing folder, except for a document-portal file (see above).
    /// False only when the file is gone.
    bool showInFolder(const QString &path,
                      std::function<void(const QString &how)> done = {});
    /// The real location of a document-portal file, asynchronously; empty
    /// when the portal does not say. Any other path answers itself at once.
    void resolveHostPath(const QString &path,
                         std::function<void(const QString &)> done);

    Platform platform() const { return m_platform; }

Q_SIGNALS:
    /// How the last showInFolder() ended: "file-manager", "portal", "folder",
    /// "explorer", "finder" or "failed".
    void revealFinished(const QString &how);

private:
    void installDefaults();
    void openContainingFolder(const QString &path,
                              const std::function<void(const QString &)> &done);
    void revealDocument(const QString &path,
                        std::function<void(const QString &)> done);
    void finishReveal(const QString &how,
                      const std::function<void(const QString &)> &done);
    /// OpenURI.<method>(parent, fd on `path`, options) and its Response.
    void callOpenUriWithFd(const QString &method, const QString &path,
                           std::function<void(bool)> done);

    Platform m_platform;
    Hooks m_hooks;
};
