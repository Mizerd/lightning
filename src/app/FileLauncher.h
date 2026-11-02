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
        std::function<bool(const QUrl &)> openUrl;
        std::function<bool(const QString &, const QStringList &)> startDetached;
        std::function<void(const QUrl &, std::function<void(bool)>)> showItems;
        std::function<void(const QString &, std::function<void(bool)>)>
            portalOpenDirectory;
        std::function<void(const QString &, std::function<void(bool)>)>
            portalOpenFile;
        std::function<void(const QString &path, const QString &docId,
                           std::function<void(const QString &)>)>
            documentHostPath;
    };

    explicit FileLauncher(QObject *parent = nullptr);
    FileLauncher(Platform platform, Hooks hooks, QObject *parent = nullptr);

    static Platform hostPlatform();

    bool openFile(const QString &path);
    bool showInFolder(const QString &path,
                      std::function<void(const QString &how)> done = {});
    void resolveHostPath(const QString &path,
                         std::function<void(const QString &)> done);

    Platform platform() const { return m_platform; }

Q_SIGNALS:
    void revealFinished(const QString &how);

private:
    void installDefaults();
    void openContainingFolder(const QString &path,
                              const std::function<void(const QString &)> &done);
    void revealDocument(const QString &path,
                        std::function<void(const QString &)> done);
    void finishReveal(const QString &how,
                      const std::function<void(const QString &)> &done);
    void callOpenUriWithFd(const QString &method, const QString &path,
                           std::function<void(bool)> done);

    Platform m_platform;
    Hooks m_hooks;
};
