#pragma once

// The one place Lightning asks the user for a file or a folder.
//
// Qt Quick's FileDialog draws its own non-native chooser whenever the Qt
// platform theme offers no native one, which is the case for every build that
// does not load the desktop's Qt integration plugin (the Nix dev shell, the
// AppImage, any Qt that is not the desktop's): a fake path bar, no
// thumbnails, no bookmarks. So QML never uses it; it asks this object
// (`app.files`), through NativeFileDialog.qml.
//
// Routes, in order:
//
//   Linux    the XDG desktop portal's FileChooser over D-Bus. KDE's portal
//            shows Dolphin's dialog, GNOME's the GTK one, COSMIC its own,
//            and it is the only chooser that can grant a Flatpak or Snap
//            access to a file. Asynchronous by construction.
//   Linux, no portal (or it fails): QFileDialog, which uses the platform
//            theme's native dialog when there is one and Qt's widget dialog
//            otherwise. The process is a QApplication, so that always works.
//   Windows, macOS: QFileDialog, which is the system dialog there.
//
// Never QtQuick.Dialogs. Every route is non-blocking: open() returns a request
// id at once and the answer arrives on finished().
//
// LIGHTNING_FILE_DIALOG=portal|qt|widget forces a route (diagnostics, and the
// way to compare them on one machine).

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <functional>

class FileChooser : public QObject
{
    Q_OBJECT
    /// "portal", "qt" or "widget": the route the next request will try first.
    /// Changes once the portal has been probed.
    Q_PROPERTY(QString route READ routeName NOTIFY routeChanged)

public:
    enum class Mode { OpenFile, OpenFiles, SaveFile, Folder };
    enum class Route { Portal, QtDialog, QtWidgetDialog };

    struct Request
    {
        int id = 0;
        Mode mode = Mode::OpenFile;
        QString title;
        QStringList nameFilters;
        QString selectedNameFilter;
        QUrl folder;
        /// SaveFile: the suggested leaf name.
        QString currentName;
        QString acceptLabel;
        /// Last-folder memory bucket ("attach", "image", "save", ...); empty
        /// for none.
        QString purpose;
    };

    /// What the route decision is made from; pure, so it is testable on any
    /// host.
    struct Environment
    {
        bool linuxDesktop = false; // Linux/BSD: a portal can exist
        bool sessionBus = false;   // a session bus is connected
        QString override;          // LIGHTNING_FILE_DIALOG
    };
    static Route initialRoute(const Environment &env);
    static Environment hostEnvironment();

    using Completion =
        std::function<void(bool accepted, const QList<QUrl> &urls,
                            const QString &selectedNameFilter)>;
    /// Runs one request on the Qt route. The default shows a QFileDialog;
    /// tests replace it.
    using QtRunner =
        std::function<void(const Request &, bool widgetOnly, Completion)>;

    explicit FileChooser(QObject *parent = nullptr);
    FileChooser(const Environment &env, QObject *parent = nullptr);
    ~FileChooser() override;

    void setQtRunner(QtRunner runner) { m_qtRunner = std::move(runner); }
    /// Last-folder memory, per purpose. `recall` answers an empty URL when
    /// nothing is remembered or the folder is gone.
    void setFolderMemory(std::function<QUrl(const QString &)> recall,
                         std::function<void(const QString &, const QUrl &)>
                             remember);
    /// Where a request with no folder and no memory starts.
    void setDefaultFolder(std::function<QUrl()> folder);

    /// Opens a chooser and returns its request id at once. Options:
    ///   mode         "open" (default) | "openMany" | "save" | "folder"
    ///   title        window title
    ///   nameFilters  ["Images (*.png *.jpg)", "All files (*)"]
    ///   selectedNameFilter  one of nameFilters
    ///   folder       starting folder (url), used when `purpose` remembers
    ///                none
    ///   currentName  save: the suggested file name (a leaf; a url's leaf is
    ///                taken)
    ///   acceptLabel  the accept button's text
    ///   purpose      last-folder memory bucket
    Q_INVOKABLE int open(const QVariantMap &options);
    /// C++ callers: the same, with the answer delivered to `done` as well as
    /// to finished().
    int open(const Request &request, Completion done);

    QString routeName() const;
    Route route() const { return m_route; }
    /// The route the last FINISHED request actually used.
    Route lastUsedRoute() const { return m_lastUsedRoute; }

    /// "Images (*.png *.jpg)" -> {"Images", {"*.png", "*.jpg"}}.
    static QPair<QString, QStringList> parseNameFilter(const QString &filter);

Q_SIGNALS:
    void finished(int requestId, bool accepted, const QList<QUrl> &urls,
                  const QString &selectedNameFilter);
    void routeChanged();

private:
    void dispatch(const Request &request);
    void runPortal(const Request &request);
    void runQt(const Request &request);
    void complete(int id, Route used, bool accepted, const QList<QUrl> &urls,
                  const QString &filter);
    void probePortal();
    void onPortalProbed(bool available);
    void failPendingPortalRequests();
    QString parentWindowHandle() const;
    void initialise();

    Route m_route = Route::QtDialog;
    Route m_lastUsedRoute = Route::QtDialog;
    enum class Probe { NotStarted, Running, Done };
    Probe m_probe = Probe::NotStarted;
    int m_nextId = 1;
    QList<Request> m_waitingForProbe;
    // Every request not yet answered, by id.
    QHash<int, Request> m_requests;
    // The ids the portal holds right now, so a portal that goes away
    // mid-dialog can be answered instead of leaving the caller waiting.
    QSet<int> m_portalInFlight;
    QHash<int, Completion> m_completions;
    QtRunner m_qtRunner;
    std::function<QUrl(const QString &)> m_recall;
    std::function<void(const QString &, const QUrl &)> m_remember;
    std::function<QUrl()> m_defaultFolder;
    QObject *m_serviceWatcher = nullptr;
};
