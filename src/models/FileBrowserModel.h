#pragma once

// The folder listing behind Lightning's own file chooser
// (FileChooserDialog.qml), which is what Linux shows when no XDG desktop
// portal FileChooser exists. It replaced Qt's widget QFileDialog there, which
// ignored the app's theme and came up with an empty File name field.
//
// One folder at a time, listed on a detached thread of its own so a stalled
// network mount cannot freeze the window, then filtered and sorted here:
// folders first, natural name order (QCollator), or by size or date. Every
// answer to an older folder is dropped by a generation check.
//
// Not QThreadPool::globalInstance(): a read stuck on a hung mount would hold
// one of its threads for good, ~QCoreApplication waits for that pool at
// quit, and the call media engine shares it. A detached thread is waited on
// by nobody; when the mount answers (or never does) it finds its model gone
// and drops the result. A path already being read gets no second thread: a
// request made while it runs is served by the same thread reading it once
// more afterwards. Past kMaxListingThreads threads still out, a new folder
// answers "unavailable" instead of leaking another; so after 16 hung folders,
// every new folder is "unavailable" until one of those mounts answers.
//
// What still touches the filesystem on the GUI thread, deliberately:
//   * stat() runs on a detached thread too and waits at most kStatTimeoutMs;
//     past that it answers {timedOut: true} instead of freezing the window.
//   * resolve() is string arithmetic only (QDir::cleanPath, homePath from
//     the environment); it never touches the disk.
//   * places() stats the user's own XDG folders (home, Desktop, ...). A home
//     on a hung mount has already frozen everything else in the app.
//   * createFolder() makes one directory in the folder just listed, which
//     answered moments ago.
//
// Why not Qt.labs.folderlistmodel: it is a separate QML module that every
// package would have to stage (Debian ships it as its own package), and a
// missing QML module fails only at runtime, in the package, which is the trap
// docs/packaging-traps.md records four times. This is a few hundred lines of
// QtCore.
//
// Nothing here follows a path the user did not browse to: the model lists
// the folder it is given and answers questions about paths the dialog
// resolved from what the user typed. It refuses nothing the OS allows.

#include <QAbstractListModel>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>

class FileBrowserModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    /// The folder listed (a file: url). Setting it lists it.
    Q_PROPERTY(QUrl folder READ folder WRITE setFolder NOTIFY folderChanged)
    /// The parent of `folder`; empty at the filesystem root.
    Q_PROPERTY(QUrl parentFolder READ parentFolder NOTIFY folderChanged)
    /// The path bar, root first: [{name, url, kind: "root"|"home"|"dir"}].
    /// Under the home folder it starts at home.
    Q_PROPERTY(QVariantList breadcrumbs READ breadcrumbs NOTIFY folderChanged)
    /// Shell globs ("*.pdf"); empty or "*" shows every file. Folders are
    /// always shown. Matched case-insensitively.
    Q_PROPERTY(QStringList nameFilters READ nameFilters WRITE setNameFilters
                   NOTIFY nameFiltersChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden
                   NOTIFY showHiddenChanged)
    /// Folder mode: files are not listed at all.
    Q_PROPERTY(bool foldersOnly READ foldersOnly WRITE setFoldersOnly
                   NOTIFY foldersOnlyChanged)
    /// "name" | "size" | "modified"
    Q_PROPERTY(QString sortField READ sortField WRITE setSortField
                   NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending WRITE setSortAscending
                   NOTIFY sortChanged)
    /// True while the folder is being read.
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    /// "" | "notFound" | "denied" | "unavailable": why the folder could not
    /// be listed ("unavailable": too many reads are stuck on a hung mount,
    /// or no thread could be started).
    Q_PROPERTY(QString error READ error NOTIFY loadingChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    /// Entries in the folder that the filters hide (not hidden files), so
    /// an empty list can say "no files match" instead of "empty".
    Q_PROPERTY(int filteredOut READ filteredOut NOTIFY countChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        UrlRole,
        IsDirRole,
        SizeRole,
        ModifiedRole,
        KindRole,
        HiddenRole,
        SymlinkRole,
    };

    struct Entry
    {
        QString name;
        bool isDir = false;
        bool hidden = false;
        bool symlink = false;
        qint64 size = 0;
        QDateTime modified;
        /// "folder" | "image" | "video" | "audio" | "pdf" | "archive" |
        /// "text" | "file"
        QString kind;
    };

    explicit FileBrowserModel(QObject *parent = nullptr);
    ~FileBrowserModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QUrl folder() const;
    void setFolder(const QUrl &folder);
    QUrl parentFolder() const;
    QVariantList breadcrumbs() const;
    QStringList nameFilters() const { return m_nameFilters; }
    void setNameFilters(const QStringList &filters);
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);
    bool foldersOnly() const { return m_foldersOnly; }
    void setFoldersOnly(bool only);
    QString sortField() const { return m_sortField; }
    void setSortField(const QString &field);
    bool sortAscending() const { return m_sortAscending; }
    void setSortAscending(bool ascending);
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    int filteredOut() const { return m_filteredOut; }

    /// Lists the folder again.
    Q_INVOKABLE void refresh();
    /// The row named exactly `name`, or -1.
    Q_INVOKABLE int indexOfName(const QString &name) const;
    /// Type-to-select: the first row after `after` (wrapping) whose name
    /// starts with `prefix`, case-insensitively; -1 when none does.
    Q_INVOKABLE int findPrefix(const QString &prefix, int after) const;
    /// One row as {name, url, isDir, size, modified, kind}; empty when out
    /// of range.
    Q_INVOKABLE QVariantMap get(int row) const;
    /// What the user typed, as a url: relative to `folder`, "~" and "~/..."
    /// to home, absolute as given. Empty for empty input.
    Q_INVOKABLE QUrl resolve(const QString &typed) const;
    /// {exists, isDir, parentExists, parentWritable, timedOut} for a local
    /// url, read off the GUI thread and waited for at most kStatTimeoutMs.
    /// On a timeout every flag is false and `timedOut` is true.
    Q_INVOKABLE QVariantMap stat(const QUrl &url) const;
    /// Creates `name` in `folder`: {ok, url, error: "" | "invalid" |
    /// "exists" | "failed"}. The listing is refreshed on success.
    Q_INVOKABLE QVariantMap createFolder(const QString &name);
    /// The places column: [{kind, url}] for home, desktop, documents,
    /// downloads, pictures (each only when it exists and is not home
    /// itself) and the filesystem root.
    Q_INVOKABLE QVariantList places() const;
    /// The drive roots, for a places section on Windows: [{label: "C:",
    /// url}]. GetLogicalDrives only (QDir::drives), so no drive is touched.
    /// Empty elsewhere.
    Q_INVOKABLE QVariantList drives() const;
    /// A local path as the user should read and type it: native separators
    /// on Windows (C:\Users\x, \\nas\share), never the URL's
    /// "/C:/...". Unchanged elsewhere.
    Q_INVOKABLE QString displayPath(const QUrl &url) const;
    /// Whether `name` can be created as a file or folder here: never empty,
    /// ".", "..", a separator or NUL; on Windows also isValidWindowsLeaf().
    Q_INVOKABLE bool isValidLeafName(const QString &name) const;

    static constexpr int kStatTimeoutMs = 2000;
    /// Listing threads (and, separately, stat threads) allowed out at once;
    /// only reads stuck on a hung mount ever accumulate.
    static constexpr int kMaxListingThreads = 16;

    struct Listing
    {
        QList<Entry> entries;
        QString error;
    };
    /// Reads one folder. Runs on the listing thread; the default reads the
    /// disk. Tests substitute one that blocks, to stand in for a hung mount.
    using Lister = std::function<Listing(const QString &path)>;
    static void setListerForTesting(Lister lister);
    /// The same for stat()'s worker: {exists, isDir, parentExists,
    /// parentWritable} of a cleaned absolute path.
    using Stater = std::function<QVariantMap(const QString &path)>;
    static void setStaterForTesting(Stater stater);
    /// Listing threads started and not yet finished, process-wide.
    static int runningListings();

    /// Whether `name` matches one of `globs` (an empty list, or "*", matches
    /// everything). Case-insensitive.
    static bool matchesFilters(const QString &name, const QStringList &globs);
    /// The kind of a file name, from its extension.
    static QString kindForName(const QString &name);
    /// The root the path bar starts from for a path outside home: the path's
    /// own drive ("D:/") or UNC share ("//server/share/"), so a folder on
    /// another Windows drive is not spelled under C:/, else
    /// QDir::rootPath(). String arithmetic only.
    static QString pathRoot(const QString &cleanPath);
    /// The pure halves of the invokables above, testable on any host.
    static QVariantList driveEntries(const QStringList &roots);
    static QString displayPathFor(const QString &localPath, bool windows);
    static bool isValidLeaf(const QString &name, bool windows);
    /// Windows' own rules for one path component: none of <>:"/\|?* or a
    /// control character, no trailing dot or space, and not a reserved
    /// device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9, also with an
    /// extension, "nul.txt"), in any case.
    static bool isValidWindowsLeaf(const QString &name);

Q_SIGNALS:
    void folderChanged();
    void nameFiltersChanged();
    void showHiddenChanged();
    void foldersOnlyChanged();
    void sortChanged();
    void loadingChanged();
    void countChanged();
    /// A listing arrived (also after refresh()). The dialog restores its
    /// selection on it.
    void listed();

private:
    struct Shared;

    static Listing listFolder(const QString &path);
    static QVariantMap statPath(const QString &path);
    void startListing();
    void applyListing(int generation, Listing listing);
    void rebuild();

    QString m_path;
    QList<Entry> m_all;
    QList<Entry> m_rows;
    QStringList m_nameFilters;
    bool m_showHidden = false;
    bool m_foldersOnly = false;
    QString m_sortField = QStringLiteral("name");
    bool m_sortAscending = true;
    bool m_loading = false;
    QString m_error;
    int m_filteredOut = 0;
    int m_generation = 0;
    std::shared_ptr<Shared> m_shared;
};
