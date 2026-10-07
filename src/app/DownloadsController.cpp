#include "app/DownloadsController.h"

#include "app/FileChooser.h"
#include "app/FileLauncher.h"
#include "app/SandboxEnvironment.h"
#include "app/SaveNaming.h"
#include "app/SettingsManager.h"
#include "media/MediaBridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

namespace {
Q_LOGGING_CATEGORY(lcDownloads, "lightning.files.downloads")

// Finished items kept for the card; older ones fall off.
constexpr int kMaxFinishedItems = 6;

// `path` (clean, absolute) is `root` or inside it.
bool isUnder(const QString &path, const QString &root)
{
    if (root.isEmpty() || path.isEmpty())
        return false;
    const QString r = QDir::cleanPath(root);
    if (r == QLatin1String("/"))
        return path.startsWith(QLatin1Char('/'));
    return path == r || path.startsWith(r + QLatin1Char('/'));
}

// The forms a path is compared in: as given, and with its deepest existing
// ancestor resolved through symlinks (/home -> /var/home on Silverblue; the
// bridge canonicalizes the folder it writes into).
QStringList pathForms(const QString &path)
{
    QStringList forms;
    if (path.isEmpty())
        return forms;
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    forms << clean;
    QString probe = clean;
    QString tail;
    while (!probe.isEmpty()) {
        const QString canonical = QFileInfo(probe).canonicalFilePath();
        if (!canonical.isEmpty()) {
            const QString resolved = tail.isEmpty()
                ? canonical
                : QDir::cleanPath(canonical + QLatin1Char('/') + tail);
            if (!forms.contains(resolved))
                forms << resolved;
            break;
        }
        if (probe == QLatin1String("/"))
            break;
        const qsizetype slash = probe.lastIndexOf(QLatin1Char('/'));
        const QString leaf = probe.mid(slash + 1);
        tail = tail.isEmpty() ? leaf : leaf + QLatin1Char('/') + tail;
        probe = slash <= 0 ? QStringLiteral("/") : probe.left(slash);
    }
    return forms;
}

// The one form that decides: the path with its deepest existing ancestor
// resolved through symlinks. The as-typed form must not: a symlink inside a
// grant that points into the sandbox-private tree would pass on its name
// while the bytes land in the tmpfs.
QString decidingForm(const QString &path)
{
    const QStringList forms = pathForms(path);
    return forms.isEmpty() ? QString() : forms.constLast();
}

// Flatpak's `host` grant exposes everything but these (its reserved list):
// the runtime's and the OS's own trees, a private /tmp, and /var and /run
// except the user-data parts of them.
bool reservedFromHostGrant(const QString &path)
{
    static const QStringList reserved{
        QStringLiteral("/app"),  QStringLiteral("/usr"),
        QStringLiteral("/proc"), QStringLiteral("/sys"),
        QStringLiteral("/dev"),  QStringLiteral("/tmp"),
        QStringLiteral("/etc"),  QStringLiteral("/root"),
        QStringLiteral("/boot"), QStringLiteral("/bin"),
        QStringLiteral("/sbin")};
    for (const QString &r : reserved) {
        if (isUnder(path, r))
            return true;
    }
    // /lib, /lib32, /lib64, /libx32, ...
    if (path.startsWith(QLatin1String("/lib")))
        return true;
    if (isUnder(path, QStringLiteral("/var"))
        && !isUnder(path, QStringLiteral("/var/home")))
        return true;
    if (isUnder(path, QStringLiteral("/run"))
        && !isUnder(path, QStringLiteral("/run/media")))
        return true;
    return false;
}

bool anyUnder(const QStringList &forms, const QString &root)
{
    if (root.isEmpty())
        return false;
    const QStringList roots = pathForms(root);
    for (const QString &form : forms) {
        for (const QString &r : roots) {
            if (isUnder(form, r))
                return true;
        }
    }
    return false;
}
} // namespace

bool DownloadsController::sandboxBlocksDownloads(const Sandbox &sandbox)
{
    if (sandbox.snap)
        return true;
    if (!sandbox.flatpak)
        return false;
    const QStringList entries =
        sandbox.flatpakFilesystems.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    bool writable = false;
    for (QString entry : entries) {
        entry = entry.trimmed();
        if (entry.isEmpty() || entry.startsWith(QLatin1Char('!')))
            continue;
        const QString base = entry.section(QLatin1Char(':'), 0, 0);
        const QString mode = entry.section(QLatin1Char(':'), 1);
        if (mode == QLatin1String("ro"))
            continue;
        if (base == QLatin1String("host") || base == QLatin1String("home")
            || base == QLatin1String("xdg-download"))
            writable = true;
    }
    // A negation later on the line ("!xdg-download") wins over a grant.
    for (QString entry : entries) {
        entry = entry.trimmed();
        if (entry == QLatin1String("!xdg-download")
            || entry == QLatin1String("!home")
            || entry == QLatin1String("!host"))
            writable = false;
    }
    return !writable;
}

bool DownloadsController::pathReachesHost(const Sandbox &sandbox,
                                          const QString &path)
{
    if (path.isEmpty())
        return false;
    // A Snap's $HOME is on the host, and what AppArmor does not allow fails
    // to write rather than landing somewhere private.
    if (!sandbox.flatpak)
        return true;
    const QString canonical = decidingForm(path);
    const QStringList forms{canonical};
    if (savenaming::isDocumentPortalPath(canonical))
        return true;
    // The app's own folder is bind-mounted from the host.
    if (!sandbox.appId.isEmpty() && !sandbox.home.isEmpty()
        && anyUnder(forms, sandbox.home + QStringLiteral("/.var/app/")
                               + sandbox.appId))
        return true;

    const QStringList entries =
        sandbox.flatpakFilesystems.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    QStringList negated;
    for (QString entry : entries) {
        entry = entry.trimmed();
        if (entry.startsWith(QLatin1Char('!')))
            negated << entry.mid(1).section(QLatin1Char(':'), 0, 0);
    }
    for (QString entry : entries) {
        entry = entry.trimmed();
        if (entry.isEmpty() || entry.startsWith(QLatin1Char('!')))
            continue;
        const QString base = entry.section(QLatin1Char(':'), 0, 0);
        const QString mode = entry.section(QLatin1Char(':'), 1);
        // A read-only grant cannot hold a save.
        if (mode == QLatin1String("ro") || negated.contains(base))
            continue;
        QString root;
        if (base == QLatin1String("host")) {
            if (!reservedFromHostGrant(canonical))
                return true;
            continue;
        } else if (base == QLatin1String("home") || base == QLatin1String("~")) {
            root = sandbox.home;
        } else if (base.startsWith(QLatin1String("xdg-"))) {
            const QString name = base.section(QLatin1Char('/'), 0, 0);
            const QString sub = base.section(QLatin1Char('/'), 1);
            const QString dir = sandbox.xdgDirs.value(name);
            if (dir.isEmpty())
                continue; // xdg-config, xdg-run, ...: not where a save goes
            root = sub.isEmpty() ? dir : dir + QLatin1Char('/') + sub;
        } else if (base.startsWith(QLatin1String("~/"))) {
            if (sandbox.home.isEmpty())
                continue;
            root = sandbox.home + base.mid(1);
        } else if (base.startsWith(QLatin1Char('/'))) {
            root = base;
        } else {
            continue;
        }
        if (anyUnder(forms, root))
            return true;
    }
    return false;
}

DownloadsController::Sandbox DownloadsController::hostSandbox()
{
    return hostSandbox(sandboxenv::flatpakInfoPath());
}

DownloadsController::Sandbox DownloadsController::hostSandbox(
    const QString &flatpakInfoPath)
{
    Sandbox sandbox;
#if defined(Q_OS_LINUX)
    // For this decision the CONTENTS of /.flatpak-info are authoritative and
    // the environment only a hint: a $FLATPAK_ID leaked into a native process
    // must not refuse its every save. The file is opened, never stat-gated
    // (SandboxEnvironment.h: Qt reports the unlinked bind mount as absent).
    const QByteArray info = sandboxenv::readFileUngated(flatpakInfoPath);
    const sandboxenv::Environment env = sandboxenv::processEnvironment();
    const sandboxenv::Detection detected =
        sandboxenv::detect(sandboxenv::Environment{}, info);
    sandbox.flatpak = detected.flatpak;
    sandbox.flatpakFilesystems = detected.flatpakFilesystems;
    sandbox.appId = detected.appId;
    if (!sandbox.flatpak
        && (!env.flatpakId.isEmpty()
            || env.container == QLatin1String("flatpak"))) {
        qCWarning(lcDownloads)
            << "the environment says Flatpak but no sandbox description is "
               "readable; saving as an unsandboxed process";
    }
    sandbox.snap = !sandbox.flatpak && !env.snap.isEmpty()
        && !env.snapName.isEmpty();
    sandbox.home = QDir::homePath();
    const auto location = [](QStandardPaths::StandardLocation which) {
        return QStandardPaths::writableLocation(which);
    };
    sandbox.xdgDirs = {
        {QStringLiteral("xdg-download"), location(QStandardPaths::DownloadLocation)},
        {QStringLiteral("xdg-documents"), location(QStandardPaths::DocumentsLocation)},
        {QStringLiteral("xdg-desktop"), location(QStandardPaths::DesktopLocation)},
        {QStringLiteral("xdg-music"), location(QStandardPaths::MusicLocation)},
        {QStringLiteral("xdg-pictures"), location(QStandardPaths::PicturesLocation)},
        {QStringLiteral("xdg-videos"), location(QStandardPaths::MoviesLocation)},
    };
    if (sandbox.flatpak) {
        qCInfo(lcDownloads) << "flatpak sandbox; downloads folder writable:"
                            << !sandboxBlocksDownloads(sandbox);
    }
#else
    Q_UNUSED(flatpakInfoPath);
#endif
    return sandbox;
}

DownloadsController::DownloadsController(MediaBridge *bridge,
                                         FileChooser *chooser,
                                         FileLauncher *launcher,
                                         QObject *parent)
    : DownloadsController(bridge, chooser, launcher, hostSandbox(), parent)
{
}

DownloadsController::DownloadsController(MediaBridge *bridge,
                                         FileChooser *chooser,
                                         FileLauncher *launcher,
                                         const Sandbox &sandbox,
                                         QObject *parent)
    : QObject(parent)
    , m_bridge(bridge)
    , m_chooser(chooser)
    , m_launcher(launcher)
    , m_sandbox(sandbox)
    , m_sandboxBlocks(sandboxBlocksDownloads(sandbox))
{
    m_defaultFolder = [] {
        QString dir =
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (dir.isEmpty())
            dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
        return dir;
    };
    if (m_bridge) {
        connect(m_bridge, &MediaBridge::saveCompleted, this,
                &DownloadsController::onSaveCompleted);
        connect(m_bridge, &MediaBridge::saveCancelled, this,
                &DownloadsController::onSaveCancelled);
        connect(m_bridge, &MediaBridge::saveFinished, this,
                &DownloadsController::onSaveFinished);
        // MediaBridge::clear() (sign-out, account switch) drops its saves
        // without finishing them. Swept a turn later, after any synchronous
        // saveFinished has been handled.
        connect(m_bridge, &MediaBridge::activeSavesChanged, this, [this] {
            QTimer::singleShot(0, this, [this] {
                if (!m_bridge)
                    return;
                const QStringList live = m_bridge->savingKeys();
                const qsizetype before = m_items.size();
                m_items.removeIf([&](const Item &item) {
                    return item.state == State::Saving
                        && !live.contains(item.mediaKey);
                });
                if (m_items.size() != before)
                    Q_EMIT itemsChanged();
            });
        });
    }
}

void DownloadsController::setSettings(SettingsManager *settings)
{
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = settings;
    if (m_settings) {
        connect(m_settings, &SettingsManager::alwaysAskWhereToSaveChanged,
                this, &DownloadsController::configurationChanged);
        connect(m_settings, &SettingsManager::downloadFolderChanged, this,
                &DownloadsController::configurationChanged);
    }
    Q_EMIT configurationChanged();
}

void DownloadsController::setDefaultFolderProvider(
    std::function<QString()> provider)
{
    m_defaultFolder = std::move(provider);
    Q_EMIT configurationChanged();
}

bool DownloadsController::asksWhereToSave() const
{
    return m_sandboxBlocks || (m_settings && m_settings->alwaysAskWhereToSave());
}

QString DownloadsController::downloadFolder() const
{
    const QString chosen = m_settings ? m_settings->downloadFolder() : QString();
    if (!chosen.isEmpty() && QFileInfo(chosen).isDir())
        return QFileInfo(chosen).absoluteFilePath();
    return m_defaultFolder ? m_defaultFolder() : QString();
}

bool DownloadsController::customFolder() const
{
    return m_settings && !m_settings->downloadFolder().isEmpty();
}

QString DownloadsController::folderDisplay() const
{
    QString folder = QDir::toNativeSeparators(downloadFolder());
#if !defined(Q_OS_WIN)
    const QString home = QDir::homePath();
    if (!home.isEmpty() && (folder == home || folder.startsWith(home + QLatin1Char('/'))))
        folder = QLatin1Char('~') + folder.mid(home.size());
#endif
    return folder;
}

void DownloadsController::download(const QString &mediaKey,
                                   const QString &rawName, const QString &mime)
{
    if (mediaKey.isEmpty())
        return;
    if (asksWhereToSave())
        startAsking(mediaKey, rawName, mime);
    else
        startDirect(mediaKey, rawName, mime);
}

void DownloadsController::saveAs(const QString &mediaKey,
                                 const QString &rawName, const QString &mime)
{
    if (mediaKey.isEmpty())
        return;
    startAsking(mediaKey, rawName, mime);
}

void DownloadsController::startDirect(const QString &mediaKey,
                                      const QString &rawName,
                                      const QString &mime)
{
    const QString folder = downloadFolder();
    // A folder only the sandbox can see is no place for it either: the file
    // would be gone when the app quits. Checked before mkpath, which would
    // otherwise create the folder in the sandbox's private home.
    if (folder.isEmpty() || !m_bridge || !pathReachesHost(m_sandbox, folder)
        || !QDir().mkpath(folder)) {
        // Nowhere to put it without asking.
        startAsking(mediaKey, rawName, mime);
        return;
    }
    const QString leaf = savenaming::suggestedFileName(rawName, mime);
    addItem(mediaKey, rawName, mime, false, leaf);
    m_bridge->saveInto(mediaKey, folder, leaf);
}

void DownloadsController::startAsking(const QString &mediaKey,
                                      const QString &rawName,
                                      const QString &mime)
{
    if (!m_chooser || !m_bridge)
        return;
    const QString leaf = savenaming::suggestedFileName(rawName, mime);
    const QString extension = savenaming::extensionOf(leaf);
    FileChooser::Request request;
    request.mode = FileChooser::Mode::SaveFile;
    request.title = tr("Save file");
    request.currentName = leaf;
    request.nameFilters = savenaming::saveDialogFilters(leaf);
    request.selectedNameFilter = request.nameFilters.value(0);
    request.purpose = QStringLiteral("save");
    const QString folder = downloadFolder();
    if (!folder.isEmpty() && QFileInfo(folder).isDir())
        request.folder = QUrl::fromLocalFile(folder);
    QPointer<DownloadsController> self(this);
    m_chooser->open(request, [self, mediaKey, rawName, mime, extension](
                                 bool accepted, const QList<QUrl> &urls,
                                 const QString &) {
        if (!self || !accepted || urls.isEmpty() || !self->m_bridge)
            return;
        const QUrl chosen = urls.constFirst();
        if (!chosen.isLocalFile()) {
            qCWarning(lcDownloads) << "save dialog returned a non-local url";
            return;
        }
        QString local = chosen.toLocalFile();
        const QFileInfo info(local);
        const QString leaf = info.fileName();
        const QString fixed = savenaming::ensureExtension(leaf, extension);
        // A file the save portal granted lives in the document portal's FUSE
        // mount, where only that exact name is writable.
        if (fixed != leaf && !savenaming::isDocumentPortalPath(local)) {
            // The user removed the extension; it goes back. The dialog
            // confirmed overwriting the name it saw, not this one, so a
            // taken name is numbered rather than replaced.
            const QString dir = info.absolutePath();
            local = QDir(dir).filePath(savenaming::uniqueFileName(dir, fixed));
        }
        // A sandbox with no portal shows Qt's own dialog INSIDE the sandbox,
        // whose home is a private tmpfs: a file saved there is lost at exit.
        // Refused and said, never written and called saved.
        if (!pathReachesHost(self->m_sandbox, local)) {
            const bool viaPortal = self->m_chooser
                && self->m_chooser->lastUsedRoute()
                    == FileChooser::Route::Portal;
            qCWarning(lcDownloads)
                << "save refused: the chosen location exists only inside the "
                   "sandbox; portal:"
                << viaPortal;
            self->refuseSandboxOnlySave(mediaKey, rawName, mime, true,
                                        QFileInfo(local).fileName());
            return;
        }
        self->addItem(mediaKey, rawName, mime, true,
                      QFileInfo(local).fileName());
        self->m_bridge->saveAs(mediaKey, QUrl::fromLocalFile(local));
    });
}

int DownloadsController::addItem(const QString &mediaKey, const QString &rawName,
                                 const QString &mime, bool asked,
                                 const QString &fileName)
{
    Item item;
    item.id = m_nextId++;
    item.mediaKey = mediaKey;
    item.rawName = rawName;
    item.mime = mime;
    item.asked = asked;
    item.fileName = fileName;
    m_items.append(item);
    Q_EMIT itemsChanged();
    return item.id;
}

QString DownloadsController::sandboxOnlyMessage()
{
    return tr("Lightning can't save outside its sandbox here. Install "
              "xdg-desktop-portal, or allow Lightning to access your "
              "Downloads folder.");
}

void DownloadsController::refuseSandboxOnlySave(const QString &mediaKey,
                                                const QString &rawName,
                                                const QString &mime, bool asked,
                                                const QString &fileName)
{
    const int id = addItem(mediaKey, rawName, mime, asked, fileName);
    if (Item *item = itemById(id)) {
        item->state = State::Failed;
        item->message = sandboxOnlyMessage();
    }
    trim();
    Q_EMIT itemsChanged();
}

DownloadsController::Item *DownloadsController::itemById(int id)
{
    for (Item &item : m_items) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

const DownloadsController::Item *DownloadsController::itemById(int id) const
{
    for (const Item &item : m_items) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

DownloadsController::Item *DownloadsController::oldestSaving(
    const QString &mediaKey)
{
    for (Item &item : m_items) {
        if (item.state == State::Saving && item.mediaKey == mediaKey)
            return &item;
    }
    return nullptr;
}

void DownloadsController::onSaveCompleted(const QString &mediaKey,
                                          const QString &path)
{
    Item *item = oldestSaving(mediaKey);
    if (!item)
        return;
    item->path = path;
    item->fileName = QFileInfo(path).fileName();
    item->hostPath.clear();
    if (!m_launcher || !savenaming::isDocumentPortalPath(path))
        return;
    // The folder the card names is the real one, once the portal says where
    // the document lives; its own parent is a document id.
    const int id = item->id;
    QPointer<DownloadsController> self(this);
    m_launcher->resolveHostPath(path, [self, id, path](const QString &host) {
        if (!self || host.isEmpty())
            return;
        Item *resolved = self->itemById(id);
        if (!resolved || resolved->path != path)
            return;
        resolved->hostPath = host;
        // Before saveFinished, the row is still "saving" and is published
        // with it; after it, this is news.
        if (resolved->state != State::Saving)
            Q_EMIT self->itemsChanged();
    });
}

void DownloadsController::onSaveCancelled(const QString &mediaKey)
{
    // cancelSave() abandons every request for the key; it is gone from the
    // list, not failed. The saveFinished that follows finds nothing.
    const qsizetype before = m_items.size();
    m_items.removeIf([&](const Item &item) {
        return item.state == State::Saving && item.mediaKey == mediaKey;
    });
    if (m_items.size() != before)
        Q_EMIT itemsChanged();
}

void DownloadsController::onSaveFinished(bool ok, const QString &message,
                                         const QString &mediaKey)
{
    Item *item = oldestSaving(mediaKey);
    if (!item)
        return;
    if (ok && item->path.isEmpty())
        ok = false; // written somewhere we were not told about
    item->state = ok ? State::Done : State::Failed;
    item->message = message;
    trim();
    Q_EMIT itemsChanged();
}

void DownloadsController::trim()
{
    int finished = 0;
    for (const Item &item : m_items)
        finished += item.state != State::Saving ? 1 : 0;
    for (int i = 0; i < m_items.size() && finished > kMaxFinishedItems;) {
        if (m_items.at(i).state != State::Saving) {
            m_items.removeAt(i);
            --finished;
        } else {
            ++i;
        }
    }
}

bool DownloadsController::canOpen(const Item &item) const
{
    return item.state == State::Done && !item.path.isEmpty()
        && !savenaming::isRiskyToOpen(item.fileName, item.mime)
        && QFileInfo(item.path).isFile();
}

QVariantList DownloadsController::items() const
{
    QVariantList out;
    const QString folder = downloadFolder();
    for (const Item &item : m_items) {
        QString state;
        switch (item.state) {
        case State::Saving: state = QStringLiteral("saving"); break;
        case State::Done: state = QStringLiteral("done"); break;
        case State::Failed: state = QStringLiteral("failed"); break;
        }
        // Never the document portal's id folder: savedFolderName() names
        // the real one when the portal said, and nothing otherwise.
        const QString folderName = !item.path.isEmpty()
            ? savenaming::savedFolderName(item.path, item.hostPath)
            : (item.asked || folder.isEmpty() ? QString()
                                              : QFileInfo(folder).fileName());
        out.append(QVariantMap{
            {QStringLiteral("id"), item.id},
            {QStringLiteral("mediaKey"), item.mediaKey},
            {QStringLiteral("fileName"), item.fileName},
            {QStringLiteral("folderName"), folderName},
            {QStringLiteral("state"), state},
            {QStringLiteral("message"), item.message},
            {QStringLiteral("risky"),
             savenaming::isRiskyToOpen(item.fileName, item.mime)},
            {QStringLiteral("canOpen"), canOpen(item)},
            {QStringLiteral("revealFailed"), item.revealFailed},
        });
    }
    return out;
}

bool DownloadsController::open(int id)
{
    const Item *item = itemById(id);
    if (!item || !canOpen(*item) || !m_launcher)
        return false;
    return m_launcher->openFile(item->path);
}

bool DownloadsController::showInFolder(int id)
{
    Item *item = itemById(id);
    if (!item || item->state != State::Done || item->path.isEmpty()
        || !m_launcher)
        return false;
    const QString path = item->path;
    if (item->revealFailed) {
        item->revealFailed = false;
        Q_EMIT itemsChanged();
    }
    QPointer<DownloadsController> self(this);
    return m_launcher->showInFolder(
        path, [self, id](const QString &how) {
            if (!self || how != QLatin1String("failed"))
                return;
            // Said on the row, not silently nothing (a sandbox with no
            // portal to show a document-portal file's folder).
            if (Item *shown = self->itemById(id)) {
                shown->revealFailed = true;
                Q_EMIT self->itemsChanged();
            }
        });
}

void DownloadsController::cancel(int id)
{
    const Item *item = itemById(id);
    if (!item || item->state != State::Saving || !m_bridge)
        return;
    m_bridge->cancelSave(item->mediaKey);
}

void DownloadsController::retry(int id)
{
    const Item *item = itemById(id);
    if (!item || item->state != State::Failed)
        return;
    const Item copy = *item;
    dismiss(id);
    if (copy.asked)
        startAsking(copy.mediaKey, copy.rawName, copy.mime);
    else
        startDirect(copy.mediaKey, copy.rawName, copy.mime);
}

void DownloadsController::dismiss(int id)
{
    const qsizetype before = m_items.size();
    m_items.removeIf([id](const Item &item) {
        return item.id == id && item.state != State::Saving;
    });
    if (m_items.size() != before)
        Q_EMIT itemsChanged();
}

void DownloadsController::chooseFolder()
{
    if (!m_chooser || !m_settings)
        return;
    FileChooser::Request request;
    request.mode = FileChooser::Mode::Folder;
    request.title = tr("Choose a downloads folder");
    request.folder = QUrl::fromLocalFile(downloadFolder());
    QPointer<DownloadsController> self(this);
    m_chooser->open(request, [self](bool accepted, const QList<QUrl> &urls,
                                    const QString &) {
        if (!self || !accepted || urls.isEmpty() || !self->m_settings)
            return;
        const QString local = urls.constFirst().toLocalFile();
        if (local.isEmpty() || !QFileInfo(local).isDir())
            return;
        self->m_settings->setDownloadFolder(QFileInfo(local).absoluteFilePath());
    });
}

void DownloadsController::resetFolder()
{
    if (m_settings)
        m_settings->setDownloadFolder(QString());
}

QString DownloadsController::pathForTest(int id) const
{
    const Item *item = itemById(id);
    return item ? item->path : QString();
}
