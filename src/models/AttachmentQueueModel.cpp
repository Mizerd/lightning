#include "models/AttachmentQueueModel.h"

#include "matrix/MatrixClient.h"

#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>

namespace {
// Conservative documented fallback when the server has not reported its
// m.upload.size yet. The real limit replaces it as soon as it is known.
constexpr qint64 kFallbackUploadLimit = 100 * 1024 * 1024;
} // namespace

AttachmentQueueModel::AttachmentQueueModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void AttachmentQueueModel::setClient(MatrixClient *client)
{
    m_client = client;
}

qint64 AttachmentQueueModel::uploadLimit() const
{
    const qint64 server = m_client ? m_client->maxUploadSize() : 0;
    return server > 0 ? server : kFallbackUploadLimit;
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

QString AttachmentQueueModel::addFile(const QUrl &fileUrl)
{
    if (!fileUrl.isLocalFile())
        return tr("Only local files can be attached.");
    const QString path = fileUrl.toLocalFile();
    const QFileInfo info(path);
    if (info.isDir())
        return tr("Folders cannot be attached.");
    if (!info.isFile() || !info.isReadable())
        return tr("That file cannot be read.");
    if (info.size() <= 0)
        return tr("Empty files cannot be sent.");
    if (info.size() > uploadLimit())
        return tr("The file is larger than the server's upload limit (%1).")
            .arg(humanSize(uploadLimit()));
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
    if (entry.isImage) {
        // Header-only read; never decodes the full image here.
        QImageReader reader(path);
        const QSize size = reader.size();
        if (size.isValid()) {
            entry.width = size.width();
            entry.height = size.height();
        }
    }

    beginInsertRows({}, m_entries.size(), m_entries.size());
    m_entries.append(entry);
    endInsertRows();
    Q_EMIT countChanged();
    return {};
}

QString AttachmentQueueModel::addImageData(const QByteArray &bytes,
                                           const QString &mime,
                                           int width, int height)
{
    if (bytes.isEmpty())
        return tr("The clipboard image is empty.");
    if (bytes.size() > uploadLimit())
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

    beginInsertRows({}, m_entries.size(), m_entries.size());
    m_entries.append(entry);
    endInsertRows();
    Q_EMIT countChanged();
    return {};
}

void AttachmentQueueModel::removeAt(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    // Entries mid-dispatch cannot be removed; they leave on completion.
    if (m_entries.at(row).state == QLatin1String("dispatching"))
        return;
    beginRemoveRows({}, row, row);
    m_entries.removeAt(row);
    endRemoveRows();
    Q_EMIT countChanged();
}

void AttachmentQueueModel::clearAll()
{
    if (m_entries.isEmpty())
        return;
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
        { StateRole,     "state" },
        { ErrorRole,     "error" },
    };
}
