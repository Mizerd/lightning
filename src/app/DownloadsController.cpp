#include "app/DownloadsController.h"

#include "app/FileChooser.h"
#include "app/FileLauncher.h"
#include "app/SaveNaming.h"
#include "app/SettingsManager.h"
#include "media/MediaBridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

namespace {
Q_LOGGING_CATEGORY(lcDownloads, "lightning.files.downloads")

// Finished items kept for the card; older ones fall off.
constexpr int kMaxFinishedItems = 6;
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

DownloadsController::Sandbox DownloadsController::hostSandbox()
{
    Sandbox sandbox;
#if defined(Q_OS_LINUX)
    QFile info(QStringLiteral("/.flatpak-info"));
    if (info.exists()) {
        sandbox.flatpak = true;
        if (info.open(QIODevice::ReadOnly | QIODevice::Text)) {
            bool inContext = false;
            while (!info.atEnd()) {
                const QString line =
                    QString::fromUtf8(info.readLine()).trimmed();
                if (line.startsWith(QLatin1Char('['))) {
                    inContext = line == QLatin1String("[Context]");
                    continue;
                }
                if (inContext && line.startsWith(QLatin1String("filesystems=")))
                    sandbox.flatpakFilesystems = line.mid(12);
            }
        }
    }
    sandbox.snap = !qEnvironmentVariableIsEmpty("SNAP");
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
    if (folder.isEmpty() || !QDir().mkpath(folder) || !m_bridge) {
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
