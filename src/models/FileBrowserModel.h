#pragma once

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

    Q_PROPERTY(QUrl folder READ folder WRITE setFolder NOTIFY folderChanged)
    Q_PROPERTY(QUrl parentFolder READ parentFolder NOTIFY folderChanged)
    Q_PROPERTY(QVariantList breadcrumbs READ breadcrumbs NOTIFY folderChanged)
    Q_PROPERTY(QStringList nameFilters READ nameFilters WRITE setNameFilters
                   NOTIFY nameFiltersChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden
                   NOTIFY showHiddenChanged)
    Q_PROPERTY(bool foldersOnly READ foldersOnly WRITE setFoldersOnly
                   NOTIFY foldersOnlyChanged)
    Q_PROPERTY(QString sortField READ sortField WRITE setSortField
                   NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending WRITE setSortAscending
                   NOTIFY sortChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY loadingChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
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

    Q_INVOKABLE void refresh();
    Q_INVOKABLE int indexOfName(const QString &name) const;
    Q_INVOKABLE int findPrefix(const QString &prefix, int after) const;
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QUrl resolve(const QString &typed) const;
    Q_INVOKABLE QVariantMap stat(const QUrl &url) const;
    Q_INVOKABLE QVariantMap createFolder(const QString &name);
    Q_INVOKABLE QVariantList places() const;
    Q_INVOKABLE QVariantList drives() const;
    Q_INVOKABLE QString displayPath(const QUrl &url) const;
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
    using Lister = std::function<Listing(const QString &path)>;
    static void setListerForTesting(Lister lister);
    using Stater = std::function<QVariantMap(const QString &path)>;
    static void setStaterForTesting(Stater stater);
    static int runningListings();

    static bool matchesFilters(const QString &name, const QStringList &globs);
    static QString kindForName(const QString &name);
    /// The root the path bar starts from for a path outside home: the path's
    /// own drive ("D:/") or UNC share ("//server/share/"), so a folder on
    /// another Windows drive is not spelled under C:/, else
    /// QDir::rootPath(). String arithmetic only.
    static QString pathRoot(const QString &cleanPath);
    static QVariantList driveEntries(const QStringList &roots);
    static QString displayPathFor(const QString &localPath, bool windows);
    static bool isValidLeaf(const QString &name, bool windows);
    static bool isValidWindowsLeaf(const QString &name);

Q_SIGNALS:
    void folderChanged();
    void nameFiltersChanged();
    void showHiddenChanged();
    void foldersOnlyChanged();
    void sortChanged();
    void loadingChanged();
    void countChanged();
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
