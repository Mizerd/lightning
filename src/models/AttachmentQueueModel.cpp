#include "models/AttachmentQueueModel.h"

#include "media/StagedImageStore.h"

#include "matrix/MatrixClient.h"
#include "media/SvgRasterJob.h"
#include "media/VideoPosterExtractor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QLoggingCategory>
#include <QPointer>
#include <QSize>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>

AttachmentQueueModel::AttachmentQueueModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

AttachmentQueueModel::~AttachmentQueueModel()
{
    // Before the directory goes, so no sweep is left holding a stale mark.
    const auto &release = lightning::svgraster::hooks().releaseScratchDir;
    if (m_convertedDir && release)
        release(m_convertedDir->path());
}

void AttachmentQueueModel::setClient(MatrixClient *client)
{
    m_client = client;
}

// 0 means the limit is unknown (not advertised, not answered yet, or failed),
// not unlimited. No client-side default is invented: it would refuse files
// the server accepts and be indistinguishable from a real limit. With no known
// limit there is no preflight and the server decides.
qint64 AttachmentQueueModel::uploadLimit() const
{
    const qint64 server = m_client ? m_client->maxUploadSize() : 0;
    return server > 0 ? server : 0;
}

bool AttachmentQueueModel::exceedsUploadLimit(qint64 bytes) const
{
    const qint64 limit = uploadLimit();
    // Exactly at the limit is allowed: m.upload.size is the largest accepted
    // payload, not the first rejected one.
    return limit > 0 && bytes > limit;
}

QString AttachmentQueueModel::uploadLimitMessage() const
{
    return tr("The file is larger than the server's upload limit (%1).")
        .arg(humanSize(uploadLimit()));
}

QString AttachmentQueueModel::humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    const double kb = bytes / 1024.0;
    if (kb < 1024)
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    const double mb = kb / 1024.0;
    if (mb < 1024)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
}

// Upload-path diagnostics; warnings fire only on refusal. Enable everything
// with QT_LOGGING_RULES='lightning.attach=true'.
Q_LOGGING_CATEGORY(lcAttach, "lightning.attach")

namespace {

// Why an attachment was refused, safe to paste into a bug report. Never the
// path (it contains the user's account name); only its shape: spaces,
// non-ASCII and UNC prefixes, the classic Windows path hazards.
QString pathShape(const QString &path)
{
    bool nonAscii = false;
    bool space = false;
    for (const QChar c : path) {
        if (c.unicode() > 127)
            nonAscii = true;
        else if (c == QLatin1Char(' '))
            space = true;
    }
    return QStringLiteral("len=%1 space=%2 nonAscii=%3 unc=%4")
        .arg(path.size())
        .arg(space ? QStringLiteral("yes") : QStringLiteral("no"),
             nonAscii ? QStringLiteral("yes") : QStringLiteral("no"),
             path.startsWith(QLatin1String("//"))
                     || path.startsWith(QLatin1String("\\\\"))
                 ? QStringLiteral("yes")
                 : QStringLiteral("no"));
}

} // namespace

QString AttachmentQueueModel::addFile(const QUrl &fileUrl)
{
    if (!fileUrl.isLocalFile()) {
        qCWarning(lcAttach) << "attachment refused reason=not_a_local_file";
        return tr("Only local files can be attached.");
    }
    const QString path = fileUrl.toLocalFile();
    const QFileInfo info(path);
    if (info.isDir()) {
        qCWarning(lcAttach) << "attachment refused reason=is_directory"
                            << qPrintable(pathShape(path));
        return tr("Folders cannot be attached.");
    }
    if (!info.isFile() || !info.isReadable()) {
        // exists() and isReadable() disagreeing points to permissions or path
        // translation, not a user mistake.
        qCWarning(lcAttach) << "attachment refused reason=unreadable"
                            << "exists=" << info.exists()
                            << "isFile=" << info.isFile()
                            << "readable=" << info.isReadable()
                            << qPrintable(pathShape(path));
        return tr("That file cannot be read.");
    }
    if (info.size() <= 0) {
        qCWarning(lcAttach) << "attachment refused reason=empty"
                            << qPrintable(pathShape(path));
        return tr("Empty files cannot be sent.");
    }
    if (exceedsUploadLimit(info.size())) {
        qCWarning(lcAttach) << "attachment refused reason=over_upload_limit"
                            << "bytes=" << info.size()
                            << "limit=" << uploadLimit();
        return uploadLimitMessage();
    }
    for (const Entry &existing : m_entries) {
        if ((!existing.localPath.isEmpty() && existing.localPath == path)
            || (!existing.sourcePath.isEmpty() && existing.sourcePath == path))
            return tr("That file is already attached.");
    }

    Entry entry;
    entry.localPath = path;
    entry.fileName = info.fileName();
    entry.sizeBytes = info.size();

    // MIME from content first, extension only as tie-breaker, so a mislabelled
    // extension cannot pick the send path.
    const QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(info, QMimeDatabase::MatchContent);
    entry.mime = mime.isValid() && !mime.isDefault()
        ? mime.name()
        : db.mimeTypeForFile(info).name();
    if (entry.mime.isEmpty())
        entry.mime = QStringLiteral("application/octet-stream");
    entry.isImage = entry.mime.startsWith(QLatin1String("image/"));
    entry.animated = entry.mime == QLatin1String("image/gif");
    entry.isVideo = entry.mime.startsWith(QLatin1String("video/"));
    entry.isSvg = lightning::svgraster::isSvgMime(entry.mime);
    // An SVG is not read here: QImageReader would hand it to Qt's SVG plugin
    // unscreened. Its size comes from the conversion's PNG.
    if (entry.isImage && !entry.isSvg) {
        // Header-only read; never decodes the full image here.
        QImageReader reader(path);
        const QSize size = reader.size();
        if (size.isValid()) {
            entry.width = size.width();
            entry.height = size.height();
        }
    }
    // Audio is decoded too, not for a picture but for its duration, which audio
    // attachments would otherwise lack. The extractor reports duration without
    // a frame, and applyPoster() stores it independently.
    entry.isAudio = entry.mime.startsWith(QLatin1String("audio/"));
    if (entry.isVideo || entry.isAudio || entry.isSvg) {
        entry.posterPending = true;
        entry.posterTag = QStringLiteral("send:%1").arg(m_nextPosterTag++);
    }

    const int row = static_cast<int>(m_entries.size());
    beginInsertRows({}, row, row);
    m_entries.append(entry);
    endInsertRows();
    Q_EMIT countChanged();
    // Started only after the row exists: an outcome may arrive synchronously
    // and applyPoster() must find its entry.
    if (m_entries.at(row).posterPending)
        startPosterJob(row);
    return {};
}

void AttachmentQueueModel::setPosterRequestHook(PosterRequestHook hook)
{
    m_posterHook = std::move(hook);
}

void AttachmentQueueModel::startPosterJob(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const Entry &entry = m_entries.at(row);
    // An SVG is converted, never postered, and the poster hook (a stand-in for
    // the video decoder) has nothing to say about it.
    if (entry.isSvg) {
        startSvgConversion(row);
        return;
    }
    if (m_posterHook) {
        m_posterHook(entry.posterTag, entry.localPath);
        return;
    }
    if (!m_posterExtractor) {
        m_posterExtractor = new VideoPosterExtractor(this);
        connect(m_posterExtractor, &VideoPosterExtractor::posterReady,
                this, &AttachmentQueueModel::applyPoster);
    }
    m_posterExtractor->requestPoster(entry.posterTag, entry.localPath);
}

QString AttachmentQueueModel::convertedDirectory()
{
    if (m_convertedDir && m_convertedDir->isValid())
        return m_convertedDir->path();
    namespace svg = lightning::svgraster;
    // The shared scratch root, never QDir::temp() when the app set one: keeps
    // these files inside a portable folder and within the startup sweep's reach
    // after a crash.
    const QString root = svg::hooks().scratchRoot ? svg::hooks().scratchRoot()
                                                  : QDir::tempPath();
    m_convertedDir = std::make_unique<QTemporaryDir>(
        root + QStringLiteral("/lightning-svgsend-XXXXXX"));
    if (!m_convertedDir->isValid()) {
        m_convertedDir.reset();
        return {};
    }
    QFile::setPermissions(m_convertedDir->path(),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    // Marked live so another instance's startup sweep cannot delete a picture
    // an upload is still reading.
    if (svg::hooks().holdScratchDir)
        svg::hooks().holdScratchDir(m_convertedDir->path());
    return m_convertedDir->path();
}

// "Rasterize on send": the SVG is converted to a PNG in a helper process
// (SvgRasterJob.h, killed on a timeout) and the entry turns into that PNG. The
// dispatch of this entry waits on posterPending, exactly as a video waits for
// its poster. A refusal fails the entry with a sentence naming the reason; a
// retry converts again.
void AttachmentQueueModel::startSvgConversion(int row)
{
    namespace svg = lightning::svgraster;
    const QString tag = m_entries.at(row).posterTag;
    const QString path = m_entries.at(row).localPath;
    QPointer<AttachmentQueueModel> self(this);
    svg::rasterizeFileAsync(
        path, svg::attachmentPolicy(), this,
        [self, tag](const svg::Result &result) {
            if (self)
                self->finishSvgConversion(tag, result);
        });
}

void AttachmentQueueModel::finishSvgConversion(
    const QString &tag, const lightning::svgraster::Result &result)
{
    namespace svg = lightning::svgraster;
    const int row = rowForPosterTag(tag);
    if (row < 0)
        return; // the entry was removed while converting
    Entry &entry = m_entries[row];
    if (!entry.posterPending)
        return;
    entry.posterPending = false;
    const auto fail = [&](const QString &message) {
        entry.state = QStringLiteral("failed");
        entry.error = message;
        updateEntry(row);
        Q_EMIT entryPrepared(row);
    };
    if (!result.refusal.isEmpty()) {
        qCInfo(lcAttach) << "svg conversion refused reason="
                         << qPrintable(result.refusal);
        fail(svg::userMessage(result.refusal));
        return;
    }
    if (exceedsUploadLimit(result.png.size())) {
        fail(uploadLimitMessage());
        return;
    }
    const QString dir = convertedDirectory();
    QString base = QFileInfo(entry.fileName).completeBaseName();
    if (base.isEmpty())
        base = QStringLiteral("image");
    const QString subdir =
        QDir(dir).filePath(QString::number(m_nextConverted++));
    const QString pngPath = QDir(subdir).filePath(base + QStringLiteral(".png"));
    QFile out(pngPath);
    if (dir.isEmpty() || !QDir().mkpath(subdir)
        || !out.open(QIODevice::WriteOnly)
        || out.write(result.png) != result.png.size()) {
        fail(tr("The converted picture couldn't be saved."));
        return;
    }
    out.close();
    out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);

    // The entry IS the PNG from here on: a file with its real size, type and
    // dimensions, which is all the send path reads.
    entry.sourcePath = entry.localPath;
    entry.localPath = pngPath;
    entry.converted = true;
    entry.fileName = base + QStringLiteral(".png");
    entry.mime = QStringLiteral("image/png");
    entry.sizeBytes = result.png.size();
    entry.width = result.size.width();
    entry.height = result.size.height();
    entry.isSvg = false;
    entry.isImage = true;
    entry.animated = false;
    entry.error.clear();
    updateEntry(row); // name, size, type and preview all changed
    Q_EMIT entryPrepared(row);
}

int AttachmentQueueModel::rowForPosterTag(const QString &tag) const
{
    for (int row = 0; row < m_entries.size(); ++row) {
        if (m_entries.at(row).posterTag == tag)
            return row;
    }
    return -1;
}

void AttachmentQueueModel::applyPoster(const QString &tag,
                                       const QByteArray &poster,
                                       const QSize &posterSize,
                                       const QSize &sourceSize,
                                       qint64 durationMs)
{
    const int row = rowForPosterTag(tag);
    if (row < 0)
        return; // the entry was removed while decoding
    Entry &entry = m_entries[row];
    if (!entry.posterPending)
        return; // already resolved; a second callback must not re-dispatch
    entry.posterPending = false;
    if (!poster.isEmpty() && posterSize.isValid() && !posterSize.isEmpty()) {
        entry.poster = poster;
        entry.posterWidth = posterSize.width();
        entry.posterHeight = posterSize.height();
    }
    // The decoded frame is the only source of the dimensions on the send side;
    // fabricated ones would make receivers lay it out wrong.
    if (sourceSize.isValid() && !sourceSize.isEmpty()) {
        entry.width = sourceSize.width();
        entry.height = sourceSize.height();
    }
    if (durationMs > 0)
        entry.durationMs = durationMs;
    Q_EMIT entryPrepared(row);
}

QString AttachmentQueueModel::addImageData(const QByteArray &bytes,
                                           const QString &mime,
                                           int width, int height)
{
    if (bytes.isEmpty())
        return tr("The clipboard image is empty.");
    if (exceedsUploadLimit(bytes.size()))
        return tr("The image is larger than the server's upload limit (%1).")
            .arg(humanSize(uploadLimit()));

    Entry entry;
    entry.data = bytes;
    entry.fileName = QStringLiteral("pasted-image.png");
    entry.mime = mime.isEmpty() ? QStringLiteral("image/png") : mime;
    entry.sizeBytes = bytes.size();
    entry.width = width;
    entry.height = height;
    entry.isImage = true;
    // Pasted bytes have no file, so register them for preview. A full store
    // only means no thumbnail.
    if (m_stagedImages)
        entry.stagedToken = m_stagedImages->add(bytes);

    beginInsertRows({}, m_entries.size(), m_entries.size());
    m_entries.append(entry);
    endInsertRows();
    Q_EMIT countChanged();
    return {};
}

void AttachmentQueueModel::releaseStaged(const Entry &entry)
{
    if (m_stagedImages && !entry.stagedToken.isEmpty())
        m_stagedImages->remove(entry.stagedToken);
    // A converted SVG's PNG is ours; a picked file never is.
    if (entry.converted && !entry.localPath.isEmpty())
        QFile::remove(entry.localPath);
}

void AttachmentQueueModel::removeAt(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    // Entries mid-dispatch cannot be removed; they leave on completion.
    if (m_entries.at(row).state == QLatin1String("dispatching"))
        return;
    releaseStaged(m_entries.at(row));
    beginRemoveRows({}, row, row);
    m_entries.removeAt(row);
    endRemoveRows();
    Q_EMIT countChanged();
}

void AttachmentQueueModel::clearAll()
{
    if (m_entries.isEmpty())
        return;
    for (const Entry &entry : m_entries)
        releaseStaged(entry);
    beginResetModel();
    m_entries.clear();
    endResetModel();
    Q_EMIT countChanged();
}

void AttachmentQueueModel::retryAt(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    Entry &entry = m_entries[row];
    if (entry.state != QLatin1String("failed"))
        return;
    if (entry.isSvg) {
        // Refused or unreadable conversion: convert again, the file may have
        // changed. It dispatches once it resolves, if the user already sent.
        entry.state = QStringLiteral("queued");
        entry.error.clear();
        entry.posterPending = true;
        entry.posterTag = QStringLiteral("send:%1").arg(m_nextPosterTag++);
        updateEntry(row);
        startSvgConversion(row);
        return;
    }
    entry.state = QStringLiteral("queued");
    entry.error.clear();
    entry.opId = 0;
    // A retry waits for the user to send again rather than dispatching off a
    // stale request.
    entry.sendRequested = false;
    updateEntry(row);
}

void AttachmentQueueModel::updateEntry(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    const QModelIndex idx = index(row, 0);
    Q_EMIT dataChanged(idx, idx);
}

int AttachmentQueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
}

QVariant AttachmentQueueModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case FileNameRole:  return e.fileName;
    case LocalUrlRole:
        return e.localPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(e.localPath);
    case MimeRole:      return e.mime;
    case SizeBytesRole: return e.sizeBytes;
    case SizeLabelRole: return humanSize(e.sizeBytes);
    case IsImageRole:   return e.isImage;
    case PreviewSourceRole: {
        if (!e.isImage && !e.isVideo)
            return QString();
        // Never the SVG file: QML would hand it to Qt's SVG plugin. Until its
        // conversion lands there is no picture; afterwards the entry is the
        // converted PNG.
        if (e.isSvg)
            return QString();
        if (!e.localPath.isEmpty())
            return QUrl::fromLocalFile(e.localPath).toString();
        if (!e.stagedToken.isEmpty())
            return QStringLiteral("image://lightning-staged/") + e.stagedToken;
        return QString();
    }
    case StateRole:     return e.state;
    case ErrorRole:     return e.error;
    default:            return {};
    }
}

QHash<int, QByteArray> AttachmentQueueModel::roleNames() const
{
    return {
        { FileNameRole,  "fileName" },
        { LocalUrlRole,  "localUrl" },
        { MimeRole,      "mime" },
        { SizeBytesRole, "sizeBytes" },
        { SizeLabelRole, "sizeLabel" },
        { IsImageRole,   "isImage" },
        { PreviewSourceRole, "previewSource" },
        { StateRole,     "state" },
        { ErrorRole,     "error" },
    };
}
