#include "models/FileBrowserModel.h"

#include <QCollator>
#include <QDir>
#include <QHash>
#include <QFileInfo>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <utility>

// The worker reaches the model through this, never through a raw pointer it
// captured: the destructor clears `owner` under the mutex, and a listing is
// posted to the model only while holding it. A posted event whose receiver is
// then destroyed is discarded by ~QObject, so nothing runs on a dead model.
struct FileBrowserModel::Shared
{
    QMutex mutex;
    FileBrowserModel *owner = nullptr;
};

namespace {

// Test seams (FileBrowserModelTest): a lister and a stater that can block,
// standing in for a hung mount. Empty means the real ones.
QMutex g_seamMutex;
FileBrowserModel::Lister g_lister;
FileBrowserModel::Stater g_stater;
std::atomic<int> g_runningListings{0};
std::atomic<int> g_runningStats{0};

// The listings still outstanding, by path, and every model generation that
// wants each one's answer. A path already being read never gets a second
// thread: re-opening a folder on a hung mount joins the read already stuck
// there instead of leaking another thread into it.
struct ListingWaiter
{
    std::shared_ptr<void> keep; // the model's Shared, kept alive
    QMutex *mutex = nullptr;
    FileBrowserModel *const *owner = nullptr;
    int generation = 0;
};
struct OutstandingListing
{
    // Asked before the read began: its answer is theirs.
    QList<ListingWaiter> waiting;
    // Asked while it was running (a refresh after createFolder, a reopen):
    // that read may predate what they want to see, so the same thread reads
    // the path once more for them when it finishes.
    QList<ListingWaiter> again;
};
// Allocated once and never freed: a thread stuck on a hung mount may wake
// while the process is exiting, and must not find these destroyed.
QMutex &outstandingMutex()
{
    static QMutex *const mutex = new QMutex;
    return *mutex;
}
QHash<QString, OutstandingListing> &outstanding()
{
    static auto *const table = new QHash<QString, OutstandingListing>;
    return *table;
}

FileBrowserModel::Lister currentLister()
{
    QMutexLocker lock(&g_seamMutex);
    return g_lister;
}

FileBrowserModel::Stater currentStater()
{
    QMutexLocker lock(&g_seamMutex);
    return g_stater;
}

QString cleanLocalPath(const QUrl &url)
{
    if (!url.isLocalFile())
        return {};
    const QString local = url.toLocalFile();
    if (local.isEmpty())
        return {};
    return QDir::cleanPath(QFileInfo(local).absoluteFilePath());
}

QString kindForMime(const QString &mime)
{
    if (mime.startsWith(QLatin1String("image/")))
        return QStringLiteral("image");
    if (mime.startsWith(QLatin1String("video/")))
        return QStringLiteral("video");
    if (mime.startsWith(QLatin1String("audio/")))
        return QStringLiteral("audio");
    if (mime == QLatin1String("application/pdf"))
        return QStringLiteral("pdf");
    static const QStringList archives{
        QStringLiteral("application/zip"),
        QStringLiteral("application/x-tar"),
        QStringLiteral("application/gzip"),
        QStringLiteral("application/x-compressed-tar"),
        QStringLiteral("application/x-bzip2-compressed-tar"),
        QStringLiteral("application/x-xz-compressed-tar"),
        QStringLiteral("application/x-zstd-compressed-tar"),
        QStringLiteral("application/x-7z-compressed"),
        QStringLiteral("application/vnd.rar"),
        QStringLiteral("application/x-rar"),
        QStringLiteral("application/x-bzip2"),
        QStringLiteral("application/x-xz"),
        QStringLiteral("application/zstd"),
    };
    if (archives.contains(mime))
        return QStringLiteral("archive");
    if (mime.startsWith(QLatin1String("text/")))
        return QStringLiteral("text");
    return QStringLiteral("file");
}

} // namespace

FileBrowserModel::FileBrowserModel(QObject *parent)
    : QAbstractListModel(parent), m_shared(std::make_shared<Shared>())
{
    m_shared->owner = this;
}

FileBrowserModel::~FileBrowserModel()
{
    QMutexLocker lock(&m_shared->mutex);
    m_shared->owner = nullptr;
}

int FileBrowserModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant FileBrowserModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Entry &e = m_rows.at(index.row());
    switch (role) {
    case NameRole: return e.name;
    case UrlRole: return QUrl::fromLocalFile(QDir(m_path).filePath(e.name));
    case IsDirRole: return e.isDir;
    case SizeRole: return double(e.size);
    case ModifiedRole: return e.modified;
    case KindRole: return e.kind;
    case HiddenRole: return e.hidden;
    case SymlinkRole: return e.symlink;
    default: return {};
    }
}

QHash<int, QByteArray> FileBrowserModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {UrlRole, "url"},
        {IsDirRole, "isDir"},
        {SizeRole, "size"},
        {ModifiedRole, "modified"},
        {KindRole, "kind"},
        {HiddenRole, "hidden"},
        {SymlinkRole, "symlink"},
    };
}

QUrl FileBrowserModel::folder() const
{
    return m_path.isEmpty() ? QUrl() : QUrl::fromLocalFile(m_path);
}

void FileBrowserModel::setFolder(const QUrl &folder)
{
    const QString path = cleanLocalPath(folder);
    if (path == m_path && !path.isEmpty())
        return;
    m_path = path;
    Q_EMIT folderChanged();
    startListing();
}

QUrl FileBrowserModel::parentFolder() const
{
    if (m_path.isEmpty())
        return {};
    QDir dir(m_path);
    if (dir.isRoot() || !dir.cdUp())
        return {};
    return QUrl::fromLocalFile(dir.absolutePath());
}

QVariantList FileBrowserModel::breadcrumbs() const
{
    QVariantList crumbs;
    if (m_path.isEmpty())
        return crumbs;
    const QString home = QDir::cleanPath(QDir::homePath());
    QString built;
    QStringList parts;
    const bool underHome = !home.isEmpty() && home != QLatin1String("/")
        && (m_path == home || m_path.startsWith(home + QLatin1Char('/')));
    if (underHome) {
        crumbs.append(QVariantMap{
            {QStringLiteral("name"), QFileInfo(home).fileName()},
            {QStringLiteral("url"), QUrl::fromLocalFile(home)},
            {QStringLiteral("kind"), QStringLiteral("home")}});
        built = home;
        parts = m_path.mid(home.size()).split(QLatin1Char('/'),
                                              Qt::SkipEmptyParts);
    } else {
        const QString root = pathRoot(m_path);
        crumbs.append(QVariantMap{
            {QStringLiteral("name"), root},
            {QStringLiteral("url"), QUrl::fromLocalFile(root)},
            {QStringLiteral("kind"), QStringLiteral("root")}});
        built = QDir::cleanPath(root);
        parts = m_path.mid(root.size()).split(QLatin1Char('/'),
                                              Qt::SkipEmptyParts);
    }
    for (const QString &part : std::as_const(parts)) {
        built = QDir::cleanPath(built + QLatin1Char('/') + part);
        crumbs.append(QVariantMap{
            {QStringLiteral("name"), part},
            {QStringLiteral("url"), QUrl::fromLocalFile(built)},
            {QStringLiteral("kind"), QStringLiteral("dir")}});
    }
    return crumbs;
}

namespace {
#ifdef Q_OS_WIN
constexpr bool kWindowsHost = true;
#else
constexpr bool kWindowsHost = false;
#endif
} // namespace

QVariantList FileBrowserModel::drives() const
{
    if (!kWindowsHost)
        return {};
    QStringList roots;
    const QFileInfoList drives = QDir::drives();
    for (const QFileInfo &drive : drives)
        roots << drive.absoluteFilePath();
    return driveEntries(roots);
}

QVariantList FileBrowserModel::driveEntries(const QStringList &roots)
{
    QVariantList list;
    for (const QString &root : roots) {
        QString label = QDir::cleanPath(root);
        if (label.endsWith(QLatin1Char('/')) && label.size() > 1)
            label.chop(1);
        list.append(QVariantMap{{QStringLiteral("label"), label},
                                {QStringLiteral("url"),
                                 QUrl::fromLocalFile(root)}});
    }
    return list;
}

QString FileBrowserModel::displayPath(const QUrl &url) const
{
    return url.isLocalFile() ? displayPathFor(url.toLocalFile(), kWindowsHost)
                             : QString();
}

QString FileBrowserModel::displayPathFor(const QString &localPath,
                                         bool windows)
{
    if (!windows)
        return localPath;
    QString path = localPath;
    // A URL path's leading slash before a drive ("/C:/x") is not part of it.
    if (path.size() >= 3 && path.at(0) == QLatin1Char('/')
        && path.at(2) == QLatin1Char(':') && path.at(1).isLetter())
        path.remove(0, 1);
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return path;
}

bool FileBrowserModel::isValidLeafName(const QString &name) const
{
    return isValidLeaf(name, kWindowsHost);
}

bool FileBrowserModel::isValidLeaf(const QString &name, bool windows)
{
    if (name.isEmpty() || name == QLatin1String(".")
        || name == QLatin1String("..") || name.contains(QLatin1Char('/'))
        || name.contains(QChar(0)))
        return false;
    return !windows || isValidWindowsLeaf(name);
}

bool FileBrowserModel::isValidWindowsLeaf(const QString &name)
{
    if (name.isEmpty())
        return false;
    for (const QChar c : name) {
        if (c.unicode() < 0x20
            || QStringView(u"<>:\"/\\|?*").contains(c))
            return false;
    }
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        return false;
    // The device names are reserved with any extension, and before one
    // with trailing spaces ("NUL .txt").
    QString stem = name.section(QLatin1Char('.'), 0, 0);
    while (stem.endsWith(QLatin1Char(' ')))
        stem.chop(1);
    stem = stem.toUpper();
    static const QStringList reserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"),
    };
    if (reserved.contains(stem))
        return false;
    if (stem.size() == 4
        && (stem.startsWith(QLatin1String("COM"))
            || stem.startsWith(QLatin1String("LPT")))) {
        const QChar digit = stem.at(3);
        // 1-9, and the superscripts Windows also treats as digits there.
        if ((digit >= QLatin1Char('1') && digit <= QLatin1Char('9'))
            || digit == QChar(0x00B9) || digit == QChar(0x00B2)
            || digit == QChar(0x00B3))
            return false;
    }
    return true;
}

QString FileBrowserModel::pathRoot(const QString &cleanPath)
{
    // "D:/..." (and a bare "D:"): that drive.
    if (cleanPath.size() >= 2 && cleanPath.at(1) == QLatin1Char(':')
        && cleanPath.at(0).isLetter()
        && (cleanPath.size() == 2 || cleanPath.at(2) == QLatin1Char('/')))
        return cleanPath.left(2) + QLatin1Char('/');
    // "//server/share/...": that share.
    if (cleanPath.startsWith(QLatin1String("//"))) {
        const QStringList parts =
            cleanPath.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() >= 2)
            return QStringLiteral("//%1/%2/").arg(parts.at(0), parts.at(1));
    }
    return QDir::rootPath();
}

void FileBrowserModel::setNameFilters(const QStringList &filters)
{
    if (filters == m_nameFilters)
        return;
    m_nameFilters = filters;
    Q_EMIT nameFiltersChanged();
    rebuild();
}

void FileBrowserModel::setShowHidden(bool show)
{
    if (show == m_showHidden)
        return;
    m_showHidden = show;
    Q_EMIT showHiddenChanged();
    rebuild();
}

void FileBrowserModel::setFoldersOnly(bool only)
{
    if (only == m_foldersOnly)
        return;
    m_foldersOnly = only;
    Q_EMIT foldersOnlyChanged();
    rebuild();
}

void FileBrowserModel::setSortField(const QString &field)
{
    const QString valid = (field == QLatin1String("size")
                           || field == QLatin1String("modified"))
        ? field
        : QStringLiteral("name");
    if (valid == m_sortField)
        return;
    m_sortField = valid;
    Q_EMIT sortChanged();
    rebuild();
}

void FileBrowserModel::setSortAscending(bool ascending)
{
    if (ascending == m_sortAscending)
        return;
    m_sortAscending = ascending;
    Q_EMIT sortChanged();
    rebuild();
}

void FileBrowserModel::refresh()
{
    startListing();
}

FileBrowserModel::Listing FileBrowserModel::listFolder(const QString &path)
{
    Listing listing;
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        listing.error = QStringLiteral("notFound");
        return listing;
    }
    QDir dir(path);
    if (!info.isReadable() || !info.isExecutable()) {
        listing.error = QStringLiteral("denied");
        return listing;
    }
    // Hidden entries are always read; showing them is a filter, so the
    // toggle needs no second read.
    const QFileInfoList infos = dir.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::NoSort);
    // A local handle rather than a function-static one. It still reaches
    // QMimeDatabase's own Q_GLOBAL_STATIC, and QDir/QFileInfo use Qt's
    // globals, so a read that returns from a hung mount at the very moment
    // the process is tearing its statics down could touch one. That window
    // is a few instructions wide and needs a mount that answers at exit;
    // it is negligible, not zero.
    const QMimeDatabase mimes;
    listing.entries.reserve(infos.size());
    for (const QFileInfo &fi : infos) {
        Entry e;
        e.name = fi.fileName();
        e.symlink = fi.isSymLink();
        // A symlink to a folder is a folder to browse into, as in every file
        // manager; a dangling one is listed as the file it claims to be.
        e.isDir = fi.isDir();
        e.hidden = e.name.startsWith(QLatin1Char('.')) || fi.isHidden();
        e.size = e.isDir ? 0 : fi.size();
        e.modified = fi.lastModified();
        e.kind = e.isDir ? QStringLiteral("folder")
                         : kindForMime(mimes.mimeTypeForFile(
                               fi, QMimeDatabase::MatchExtension).name());
        listing.entries.append(std::move(e));
    }
    return listing;
}

void FileBrowserModel::startListing()
{
    const int generation = ++m_generation;
    // Rows from the previous folder go at once: an Enter pressed before the
    // new listing lands must not act on a file from the folder just left.
    beginResetModel();
    m_all.clear();
    m_rows.clear();
    m_filteredOut = 0;
    endResetModel();
    Q_EMIT countChanged();
    m_error.clear();
    m_loading = !m_path.isEmpty();
    Q_EMIT loadingChanged();
    if (m_path.isEmpty())
        return;
    const QString path = m_path;
    const ListingWaiter me{m_shared, &m_shared->mutex, &m_shared->owner,
                           generation};
    {
        QMutexLocker lock(&outstandingMutex());
        const auto it = outstanding().find(path);
        if (it != outstanding().end()) {
            // Already being read (a refresh, the same folder reopened, a
            // second chooser). That read may have started before whatever
            // this request wants to see, so it is read once more when it
            // finishes, on the same thread.
            it->again.append(me);
            return;
        }
        // Each leaked thread is a read stuck on a mount that never answered.
        // Past a ceiling, a new folder is refused rather than another thread
        // spent on it.
        if (g_runningListings.load() >= kMaxListingThreads) {
            lock.unlock();
            Listing refused;
            refused.error = QStringLiteral("unavailable");
            applyListing(generation, std::move(refused));
            return;
        }
        OutstandingListing entry;
        entry.waiting.append(me);
        outstanding().insert(path, entry);
        ++g_runningListings;
    }
    const Lister lister = currentLister();
    // Hands one answer to each waiting generation, on its own model's
    // thread, if that model still exists.
    const auto deliver = [](const QList<ListingWaiter> &waiters,
                            const Listing &listing) {
        for (const ListingWaiter &w : waiters) {
            QMutexLocker lock(w.mutex);
            if (FileBrowserModel *owner = *w.owner) {
                const int generation = w.generation;
                QMetaObject::invokeMethod(
                    owner,
                    [owner, generation, listing]() mutable {
                        owner->applyListing(generation, std::move(listing));
                    },
                    Qt::QueuedConnection);
            }
        }
    };
    // A detached thread, never a pool: see the header. Nobody joins it, so a
    // read stuck on a hung mount delays neither quit nor any other listing.
    try {
        std::thread([path, lister, deliver] {
            for (;;) {
                const Listing listing =
                    lister ? lister(path) : listFolder(path);
                QList<ListingWaiter> answered;
                bool readAgain = false;
                {
                    QMutexLocker lock(&outstandingMutex());
                    const auto it = outstanding().find(path);
                    if (it != outstanding().end()) {
                        answered = std::move(it->waiting);
                        if (it->again.isEmpty()) {
                            outstanding().erase(it);
                        } else {
                            it->waiting = std::move(it->again);
                            it->again.clear();
                            readAgain = true;
                        }
                    }
                }
                deliver(answered, listing);
                if (!readAgain)
                    break;
            }
            --g_runningListings;
        }).detach();
    } catch (...) {
        // No thread to be had (a process at its limit, or anything else the
        // constructor throws): the path's entry goes, and every request on
        // it is told, rather than read on the GUI thread.
        QList<ListingWaiter> everyone;
        {
            QMutexLocker lock(&outstandingMutex());
            const OutstandingListing entry = outstanding().take(path);
            everyone = entry.waiting + entry.again;
        }
        Listing failed;
        failed.error = QStringLiteral("unavailable");
        deliver(everyone, failed);
        --g_runningListings;
    }
}

void FileBrowserModel::setListerForTesting(Lister lister)
{
    QMutexLocker lock(&g_seamMutex);
    g_lister = std::move(lister);
}

void FileBrowserModel::setStaterForTesting(Stater stater)
{
    QMutexLocker lock(&g_seamMutex);
    g_stater = std::move(stater);
}

int FileBrowserModel::runningListings()
{
    return g_runningListings.load();
}

void FileBrowserModel::applyListing(int generation, Listing listing)
{
    if (generation != m_generation)
        return; // an older folder's answer
    m_all = std::move(listing.entries);
    m_error = listing.error;
    m_loading = false;
    rebuild();
    Q_EMIT loadingChanged();
    Q_EMIT listed();
}

bool FileBrowserModel::matchesFilters(const QString &name,
                                      const QStringList &globs)
{
    if (globs.isEmpty())
        return true;
    for (const QString &glob : globs) {
        const QString g = glob.trimmed();
        if (g.isEmpty() || g == QLatin1String("*") || g == QLatin1String("*.*"))
            return true;
        const QRegularExpression re(
            QRegularExpression::wildcardToRegularExpression(
                g, QRegularExpression::NonPathWildcardConversion),
            QRegularExpression::CaseInsensitiveOption);
        if (re.match(name).hasMatch())
            return true;
    }
    return false;
}

QString FileBrowserModel::kindForName(const QString &name)
{
    static const QMimeDatabase mimes;
    return kindForMime(
        mimes.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name());
}

void FileBrowserModel::rebuild()
{
    QList<Entry> rows;
    rows.reserve(m_all.size());
    int filteredOut = 0;
    for (const Entry &e : std::as_const(m_all)) {
        if (e.hidden && !m_showHidden)
            continue;
        if (!e.isDir && (m_foldersOnly || !matchesFilters(e.name, m_nameFilters))) {
            ++filteredOut;
            continue;
        }
        rows.append(e);
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    const QString field = m_sortField;
    const bool ascending = m_sortAscending;
    std::stable_sort(rows.begin(), rows.end(),
                     [&](const Entry &a, const Entry &b) {
        // Folders first whichever way the list is sorted, as every file
        // manager does.
        if (a.isDir != b.isDir)
            return a.isDir;
        int c = 0;
        if (field == QLatin1String("size") && !a.isDir)
            c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        else if (field == QLatin1String("modified"))
            c = a.modified < b.modified ? -1 : (a.modified > b.modified ? 1 : 0);
        if (c == 0)
            c = collator.compare(a.name, b.name);
        if (c == 0)
            c = QString::compare(a.name, b.name);
        return ascending ? c < 0 : c > 0;
    });
    beginResetModel();
    m_rows = std::move(rows);
    m_filteredOut = filteredOut;
    endResetModel();
    Q_EMIT countChanged();
}

int FileBrowserModel::indexOfName(const QString &name) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).name == name)
            return i;
    }
    return -1;
}

int FileBrowserModel::findPrefix(const QString &prefix, int after) const
{
    const int n = int(m_rows.size());
    if (prefix.isEmpty() || n == 0)
        return -1;
    const int start = (after < -1 || after >= n) ? -1 : after;
    for (int step = 1; step <= n; ++step) {
        const int i = (start + step) % n;
        if (m_rows.at(i).name.startsWith(prefix, Qt::CaseInsensitive))
            return i;
    }
    return -1;
}

QVariantMap FileBrowserModel::get(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    const Entry &e = m_rows.at(row);
    return {
        {QStringLiteral("name"), e.name},
        {QStringLiteral("url"),
         QUrl::fromLocalFile(QDir(m_path).filePath(e.name))},
        {QStringLiteral("isDir"), e.isDir},
        {QStringLiteral("size"), double(e.size)},
        {QStringLiteral("modified"), e.modified},
        {QStringLiteral("kind"), e.kind},
    };
}

QUrl FileBrowserModel::resolve(const QString &typed) const
{
    QString text = typed.trimmed();
    if (text.isEmpty())
        return {};
    if (text.startsWith(QLatin1String("file:")))
        return QUrl(text).isLocalFile()
            ? QUrl::fromLocalFile(QDir::cleanPath(QUrl(text).toLocalFile()))
            : QUrl();
    if (text == QLatin1String("~"))
        text = QDir::homePath();
    else if (text.startsWith(QLatin1String("~/")))
        text = QDir::homePath() + text.mid(1);
    const QString absolute = QDir::isAbsolutePath(text)
        ? text
        : QDir(m_path.isEmpty() ? QDir::homePath() : m_path).filePath(text);
    return QUrl::fromLocalFile(QDir::cleanPath(absolute));
}

QVariantMap FileBrowserModel::statPath(const QString &path)
{
    const QFileInfo info(path);
    const QFileInfo parent(info.absolutePath());
    return {
        {QStringLiteral("exists"), info.exists()},
        {QStringLiteral("isDir"), info.isDir()},
        {QStringLiteral("parentExists"), parent.exists() && parent.isDir()},
        {QStringLiteral("parentWritable"),
         parent.isDir() && parent.isWritable()},
    };
}

QVariantMap FileBrowserModel::stat(const QUrl &url) const
{
    QVariantMap answer{{QStringLiteral("exists"), false},
                       {QStringLiteral("isDir"), false},
                       {QStringLiteral("parentExists"), false},
                       {QStringLiteral("parentWritable"), false},
                       {QStringLiteral("timedOut"), false}};
    const QString path = cleanLocalPath(url);
    if (path.isEmpty())
        return answer;
    // The path is typed by the user and may sit on a hung mount: read it on
    // a detached thread and wait a bounded time. A promise's future does not
    // block in its destructor, so giving up costs nothing but the thread.
    const Stater stater = currentStater();
    // Each timed-out stat leaves a thread behind on the mount that hung;
    // past a ceiling the answer is "not responding" without another one.
    if (g_runningStats.load() >= kMaxListingThreads) {
        answer.insert(QStringLiteral("timedOut"), true);
        return answer;
    }
    auto promise = std::make_shared<std::promise<QVariantMap>>();
    std::future<QVariantMap> result = promise->get_future();
    try {
        ++g_runningStats;
        std::thread([promise, stater, path] {
            promise->set_value(stater ? stater(path) : statPath(path));
            --g_runningStats;
        }).detach();
    } catch (...) {
        --g_runningStats;
        answer.insert(QStringLiteral("timedOut"), true);
        return answer;
    }
    if (result.wait_for(std::chrono::milliseconds(kStatTimeoutMs))
        != std::future_status::ready) {
        answer.insert(QStringLiteral("timedOut"), true);
        return answer;
    }
    const QVariantMap found = result.get();
    for (auto it = found.cbegin(); it != found.cend(); ++it)
        answer.insert(it.key(), it.value());
    return answer;
}

QVariantMap FileBrowserModel::createFolder(const QString &rawName)
{
    const QString name = rawName.trimmed();
    QVariantMap result{{QStringLiteral("ok"), false},
                       {QStringLiteral("url"), QUrl()},
                       {QStringLiteral("error"), QString()}};
    if (m_path.isEmpty() || !isValidLeafName(name)) {
        result.insert(QStringLiteral("error"), QStringLiteral("invalid"));
        return result;
    }
    QDir dir(m_path);
    if (QFileInfo::exists(dir.filePath(name))) {
        result.insert(QStringLiteral("error"), QStringLiteral("exists"));
        return result;
    }
    if (!dir.mkdir(name)) {
        result.insert(QStringLiteral("error"), QStringLiteral("failed"));
        return result;
    }
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("url"),
                  QUrl::fromLocalFile(QDir::cleanPath(dir.filePath(name))));
    refresh();
    return result;
}

QVariantList FileBrowserModel::places() const
{
    QVariantList list;
    const QString home = QDir::cleanPath(QDir::homePath());
    QStringList seen;
    const auto add = [&](const QString &kind, const QString &path) {
        if (path.isEmpty())
            return;
        const QString clean = QDir::cleanPath(path);
        if (seen.contains(clean) || !QFileInfo(clean).isDir())
            return;
        // XDG falls back to home for a folder the user never had; that is
        // "Home" already, not a second entry called "Desktop".
        if (kind != QLatin1String("home") && clean == home)
            return;
        seen.append(clean);
        list.append(QVariantMap{{QStringLiteral("kind"), kind},
                                {QStringLiteral("url"),
                                 QUrl::fromLocalFile(clean)}});
    };
    add(QStringLiteral("home"), home);
    add(QStringLiteral("desktop"),
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
    add(QStringLiteral("documents"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    add(QStringLiteral("downloads"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    add(QStringLiteral("pictures"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    add(QStringLiteral("root"), QDir::rootPath());
    return list;
}
