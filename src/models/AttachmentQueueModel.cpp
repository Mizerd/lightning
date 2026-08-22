#include "models/AttachmentQueueModel.h"

#include "media/StagedImageStore.h"

#include "matrix/MatrixClient.h"
#include "media/VideoPosterExtractor.h"

#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QLoggingCategory>
#include <QSize>

AttachmentQueueModel::AttachmentQueueModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void AttachmentQueueModel::setClient(MatrixClient *client)
{
    m_client = client;
}

// 0 means the limit is UNKNOWN — the homeserver advertises no m.upload.size,
// the capability lookup has not answered yet, or it failed. It does NOT mean
// "unlimited", and it is deliberately not replaced by a client-side default:
// an invented ceiling would refuse files this server would have accepted, and
// would be indistinguishable downstream from a real advertised limit. When
// the limit is unknown, no preflight happens and the SDK/server decides.
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

// Upload-path diagnostics. Quiet by default (warnings only fire on a
// refusal); enable everything with QT_LOGGING_RULES='lightning.attach=true'.
Q_LOGGING_CATEGORY(lcAttach, "lightning.attach")

namespace {

// Why an attachment was refused, in a form that can be pasted into a bug
// report. Deliberately NOT the path: a Windows path contains the user's
// account name, and this is the one upload log a user is likely to share.
//
// What it carries instead is the path's SHAPE, which is what the outstanding
// "uploads fail from the MSI, but the Setup EXE and the portable ZIP work"
// report needs to distinguish: spaces, non-ASCII and UNC prefixes are the
// classic Windows path hazards, and knowing which of the five rejections
// fired says whether the file was even reachable.
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
        // The one most likely to be an environment problem rather than a user
        // mistake: exists() and isReadable() disagreeing is a permissions or
        // path-translation failure, not a wrong click.
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
        if (!existing.localPath.isEmpty() && existing.localPath == path)
            return tr("That file is already attached.");
    }

    Entry entry;
    entry.localPath = path;
    entry.fileName = info.fileName();
    entry.sizeBytes = info.size();

    // MIME from content first, extension as tie-breaker — an mislabelled
    // extension must not pick the send path.
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
    if (entry.isImage) {
        // Header-only read; never decodes the full image here.
        QImageReader reader(path);
        const QSize size = reader.size();
        if (size.isValid()) {
            entry.width = size.width();
            entry.height = size.height();
        }
    }
    if (entry.isVideo) {
        entry.posterPending = true;
        entry.posterTag = QStringLiteral("send:%1").arg(m_nextPosterTag++);
    }

    const int row = static_cast<int>(m_entries.size());
    beginInsertRows({}, row, row);
    m_entries.append(entry);
    endInsertRows();
    Q_EMIT countChanged();
    // Started only AFTER the row exists: a poster outcome may come back
    // synchronously (the test hook, or an extractor that fails on the
    // spot), and applyPoster() has to find the entry it belongs to.
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

int AttachmentQueueModel::rowForPosterTag(const QString &tag) const
{
    for (int row = 0; row < m_entries.size(); ++row) {
        if (m_entries.at(row).posterTag == tag)
            return row;
    }
    return -1;
}

void AttachmentQueueModel::applyPoster(const QString &tag,
                                       const QByteArray &jpeg,
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
    if (!jpeg.isEmpty() && posterSize.isValid() && !posterSize.isEmpty()) {
        entry.poster = jpeg;
        entry.posterWidth = posterSize.width();
        entry.posterHeight = posterSize.height();
    }
    // The decoded frame is the only honest source of the video's own
    // dimensions on the send side; QMimeDatabase cannot supply them and
    // fabricating them would make every receiver lay the video out wrong.
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
    // No file exists for a paste, so the bytes are registered for preview.
    // A full store just means no thumbnail — never a refused paste.
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
    entry.state = QStringLiteral("queued");
    entry.error.clear();
    entry.opId = 0;
    // A retry is a fresh send decision: the entry waits for the user to
    // press send again rather than dispatching off a stale request.
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
