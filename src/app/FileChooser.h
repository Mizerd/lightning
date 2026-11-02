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
//   Linux, no portal (or it fails): Lightning's themed FileChooserDialog.qml.
//   Windows and macOS: QFileDialog, using the native system dialog.
//
// Never QtQuick.Dialogs: its non-native FileDialog fills the name field only
// from a file that ALREADY EXISTS (QQuickFileDialogImplPrivate::
// updateFileNameTextEdit checks QFileInfo::isFile), so every Save as came up
// with an empty name. Every route is non-blocking: open() returns a request
// id at once and the answer arrives on finished().
//
// LIGHTNING_FILE_DIALOG=portal|app|qt|widget forces a route (diagnostics,
// and the way to compare them on one machine).

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

class QFileDialog;

class FileChooser : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString route READ routeName NOTIFY routeChanged)

public:
    enum class Mode { OpenFile, OpenFiles, SaveFile, Folder };
    enum class Route { Portal, QtDialog, QtWidgetDialog, InApp };

    struct Request
    {
        int id = 0;
        Mode mode = Mode::OpenFile;
        QString title;
        QStringList nameFilters;
        QString selectedNameFilter;
        QUrl folder;
        QString currentName;
        QString acceptLabel;
        QString purpose;
    };

    struct Environment
    {
        bool linuxDesktop = false;
        bool windows = false;
        bool sessionBus = false;
        QString override;
    };
    static Route initialRoute(const Environment &env);
    static Route fallbackRoute(const Environment &env);
    static Environment hostEnvironment();
    static void configureQtDialog(QFileDialog &dialog, const Request &request,
                                  bool widgetOnly);

    using Completion =
        std::function<void(bool accepted, const QList<QUrl> &urls,
                            const QString &selectedNameFilter)>;
    using QtRunner =
        std::function<void(const Request &, bool widgetOnly, Completion)>;

    explicit FileChooser(QObject *parent = nullptr);
    FileChooser(const Environment &env, QObject *parent = nullptr);
    ~FileChooser() override;

    void setQtRunner(QtRunner runner) { m_qtRunner = std::move(runner); }
    void setFolderMemory(std::function<QUrl(const QString &)> recall,
                         std::function<void(const QString &, const QUrl &)>
                             remember);
    void setDefaultFolder(std::function<QUrl()> folder);

    Q_INVOKABLE int open(const QVariantMap &options);
    int open(const Request &request, Completion done);

    QString routeName() const;
    Route route() const { return m_route; }
    Route lastUsedRoute() const { return m_lastUsedRoute; }

    static QPair<QString, QStringList> parseNameFilter(const QString &filter);
    static int baseNameLength(const QString &leaf);
    Q_INVOKABLE int selectionLength(const QString &leaf) const
    {
        return baseNameLength(leaf);
    }
    static QVariantMap describeRequest(const Request &request);

    Q_INVOKABLE void answerInApp(int requestId, bool accepted,
                                 const QVariantList &urls,
                                 const QString &selectedNameFilter);

Q_SIGNALS:
    void finished(int requestId, bool accepted, const QList<QUrl> &urls,
                  const QString &selectedNameFilter);
    void routeChanged();
    void inAppRequested(int requestId, const QVariantMap &request);

private:
    void dispatch(const Request &request);
    void runPortal(const Request &request);
    void runQt(const Request &request);
    void runInApp(const Request &request);
    void runFallback(const Request &request);
    void fallBack();
    void complete(int id, Route used, bool accepted, const QList<QUrl> &urls,
                  const QString &filter);
    void probePortal();
    void onPortalProbed(bool available);
    void failPendingPortalRequests();
    QString parentWindowHandle() const;
    void initialise();

    Route m_route = Route::QtDialog;
    Route m_fallback = Route::QtDialog;
    Route m_lastUsedRoute = Route::QtDialog;
    enum class Probe { NotStarted, Running, Done };
    Probe m_probe = Probe::NotStarted;
    int m_nextId = 1;
    QList<Request> m_waitingForProbe;
    QHash<int, Request> m_requests;
    QSet<int> m_portalInFlight;
    QSet<int> m_inAppInFlight;
    QHash<int, Completion> m_completions;
    QtRunner m_qtRunner;
    std::function<QUrl(const QString &)> m_recall;
    std::function<void(const QString &, const QUrl &)> m_remember;
    std::function<QUrl()> m_defaultFolder;
    QObject *m_serviceWatcher = nullptr;
};
