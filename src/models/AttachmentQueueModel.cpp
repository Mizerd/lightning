#include "models/AttachmentQueueModel.h"

#include "media/StagedImageStore.h"
#include "media/AnimatedImageSniff.h"

#include "matrix/MatrixClient.h"
#include "matrix/PresentableName.h"
#include "media/SvgRasterJob.h"
#include "media/VideoPosterExtractor.h"
#include "storage/ScratchFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QBuffer>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QMimeData>
#include <QMimeDatabase>
#include <QLoggingCategory>
#include <QPointer>
#include <QSize>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>

#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

#include <memory>
#include <cmath>
#include <utility>

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

const char kPortalFileTransfer[] = "application/vnd.portal.filetransfer";
const char kPortalFilesLegacy[] = "application/vnd.portal.files";

AttachmentQueueModel::PortalRetrieveHook &portalRetrieveHook()
{
    static AttachmentQueueModel::PortalRetrieveHook hook;
    return hook;
}

QString &sandboxKindOverride()
{
    static QString kind;
    return kind;
}

// org.freedesktop.portal.FileTransfer.RetrieveFiles: exports the files behind
// a drop's key into the document portal and returns their paths there
// (/run/user/<uid>/doc/...), which a sandbox can open. Bounded; any failure
// is reported as one, never as an empty success.
void retrieveFromPortal(
    const QString &key,
    const std::function<void(bool, const QStringList &)> &done,
    QObject *context)
{
    if (const auto &hook = portalRetrieveHook()) {
        hook(key, done);
        return;
    }
#ifdef HAVE_QT_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCWarning(lcAttach) << "drop portal unavailable reason=no_session_bus";
        done(false, {});
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Documents"),
        QStringLiteral("/org/freedesktop/portal/documents"),
        QStringLiteral("org.freedesktop.portal.FileTransfer"),
        QStringLiteral("RetrieveFiles"));
    call << key << QVariantMap{};
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 5000),
                                                context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
                     [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QStringList> reply = *w;
        if (reply.isError()) {
            // The error NAME only: its message may carry a path.
            qCWarning(lcAttach) << "drop portal refused error="
                                << qPrintable(reply.error().name());
            done(false, {});
            return;
        }
        done(true, reply.value());
    });
#else
    Q_UNUSED(key)
    Q_UNUSED(context)
    done(false, {});
#endif
}

} // namespace

QString AttachmentQueueModel::detectMime(const QString &path)
{
    const QFileInfo info(path);
    const QMimeDatabase db;
    const QMimeType content =
        db.mimeTypeForFile(info, QMimeDatabase::MatchContent);
    const QMimeType byName =
        db.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
    // Content proved nothing: the extension decides.
    if (!content.isValid() || content.isDefault()) {
        const QString name = byName.isValid() && !byName.isDefault()
            ? byName.name()
            : db.mimeTypeForFile(info).name();
        return name.isEmpty() ? QStringLiteral("application/octet-stream")
                              : name;
    }
    // Content proved a CONTAINER that carries either sound or picture, and the
    // shared MIME database names it generically: Matroska is
    // application/x-matroska, ASF is application/vnd.ms-asf. Taken as is, an
    // .mkv recording (OBS's default) or a .wmv went out as m.file and played
    // nowhere. The extension may only narrow one of these containers to an
    // audio or video type that INHERITS from what the bytes proved, so a .mkv
    // name on PNG bytes stays a PNG. A closed list, because other generic
    // parents have audio/video children that are not media at all: text/plain
    // (.m4u and .vlc playlists) and application/zip (.yt).
    static const QStringList kGenericMediaContainers = {
        QStringLiteral("application/x-matroska"),
        QStringLiteral("application/vnd.ms-asf"),
        QStringLiteral("application/ogg"),
        QStringLiteral("application/x-riff"),
        QStringLiteral("application/x-iff"),
    };
    const QString proven = content.name();
    if (kGenericMediaContainers.contains(proven) && byName.isValid()
        && (byName.name().startsWith(QLatin1String("video/"))
            || byName.name().startsWith(QLatin1String("audio/")))
        && byName.inherits(proven))
        return byName.name();
    return proven;
}

QString AttachmentQueueModel::portalKeyFrom(const QByteArray &raw)
{
    // GTK writes the key with its terminating NUL, KDE without.
    QByteArray key = raw;
    while (!key.isEmpty() && key.back() == '\0')
        key.chop(1);
    return QString::fromUtf8(key).trimmed();
}

QString AttachmentQueueModel::portalKeyFromMimeData(const QMimeData *mime)
{
    if (!mime)
        return {};
    for (const char *format : {kPortalFileTransfer, kPortalFilesLegacy}) {
        const QString name = QString::fromLatin1(format);
        if (mime->hasFormat(name)) {
            const QString key = portalKeyFrom(mime->data(name));
            if (!key.isEmpty())
                return key;
        }
    }
    return {};
}

QUrl AttachmentQueueModel::normalizeDroppedUrl(const QUrl &url)
{
    if (!url.isLocalFile())
        return url;
    QUrl out = url;
    const QString host = url.host();
    if (!host.isEmpty()) {
        const bool localhost =
            host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0;
#ifdef Q_OS_WIN
        // \\HOST\share is a real UNC path on Windows, so only "localhost"
        // (whose "C:" is no share name) is folded.
        const bool ownHost = false;
#else
        const bool ownHost = host.compare(QSysInfo::machineHostName(),
                                          Qt::CaseInsensitive) == 0;
#endif
        if (localhost || ownHost)
            out.setHost(QString());
    }
    if ((out.hasFragment() || out.hasQuery())
        && !QFileInfo::exists(out.toLocalFile())) {
        QString literal = out.path(QUrl::FullyDecoded);
        if (out.hasQuery())
            literal += QLatin1Char('?') + out.query(QUrl::FullyDecoded);
        if (out.hasFragment())
            literal += QLatin1Char('#') + out.fragment(QUrl::FullyDecoded);
        const QUrl rebuilt = QUrl::fromLocalFile(
            out.host().isEmpty()
                ? literal
                : QStringLiteral("//") + out.host() + literal);
        if (QFileInfo::exists(rebuilt.toLocalFile()))
            out = rebuilt;
    }
    return out;
}

void AttachmentQueueModel::setPortalRetrieveHook(PortalRetrieveHook hook)
{
    portalRetrieveHook() = std::move(hook);
}

QString AttachmentQueueModel::sandboxKind()
{
    if (!sandboxKindOverride().isEmpty())
        return sandboxKindOverride();
    if (QFileInfo::exists(QStringLiteral("/.flatpak-info")))
        return QStringLiteral("flatpak");
    if (!qEnvironmentVariableIsEmpty("SNAP"))
        return QStringLiteral("snap");
    return QStringLiteral("none");
}

void AttachmentQueueModel::setSandboxKindForTesting(const QString &kind)
{
    sandboxKindOverride() = kind;
}

void AttachmentQueueModel::resolveDroppedUrls(
    const QList<QUrl> &urls, const QString &portalKey, QObject *context,
    std::function<void(const QList<QUrl> &)> done)
{
    // QML reads the key with DragEvent.getDataAsString, which keeps GTK's
    // terminating NUL inside the string; the portal would refuse that key.
    const QString key = portalKeyFrom(portalKey.toUtf8());
    QList<QUrl> normalized;
    normalized.reserve(urls.size());
    bool unreadable = false;
    for (const QUrl &url : urls) {
        const QUrl local = normalizeDroppedUrl(url);
        if (local != url) {
            // Which repair applied, never the path.
            const bool hostFolded = url.host() != local.host();
            const bool literalRestored = (url.hasFragment() || url.hasQuery())
                && !local.hasFragment() && !local.hasQuery();
            qCInfo(lcAttach) << "drop url normalized"
                             << "hostFolded=" << hostFolded
                             << "literalHashOrQuery=" << literalRestored;
        }
        if (local.isLocalFile()
            && !QFileInfo(local.toLocalFile()).isReadable())
            unreadable = true;
        normalized.append(local);
    }
    // The ordinary case, sandboxed or not: the dropped paths are readable here
    // and the portal is never asked.
    if (key.isEmpty() || (!unreadable && !normalized.isEmpty())) {
        if (unreadable)
            qCWarning(lcAttach) << "drop unreadable portal_key=none sandbox="
                                << qPrintable(sandboxKind());
        done(normalized);
        return;
    }
    QPointer<QObject> guard(context);
    retrieveFromPortal(
        key,
        [guard, normalized, done](bool ok, const QStringList &paths) {
            if (!guard)
                return;
            if (!ok || paths.isEmpty()) {
                qCWarning(lcAttach) << "drop resolved via=portal_failed"
                                    << "count=" << normalized.size()
                                    << "sandbox=" << qPrintable(sandboxKind());
                done(normalized);
                return;
            }
            QList<QUrl> resolved;
            resolved.reserve(paths.size());
            for (const QString &path : paths)
                resolved.append(QUrl::fromLocalFile(path));
            qCInfo(lcAttach) << "drop resolved via=portal"
                             << "count=" << resolved.size();
            done(resolved);
        },
        context);
}

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
        const QString sandbox = sandboxKind();
        qCWarning(lcAttach) << "attachment refused reason=unreadable"
                            << "exists=" << info.exists()
                            << "isFile=" << info.isFile()
                            << "readable=" << info.isReadable()
                            << "sandbox=" << qPrintable(sandbox)
                            << qPrintable(pathShape(path));
        // A path that does not exist inside a sandbox is almost always a host
        // path the sandbox cannot see (a drop from a file manager that offered
        // no portal key). Say so, and name the way that works: the picker goes
        // through the file chooser portal.
        if (!info.exists() && sandbox != QLatin1String("none"))
            return tr("Lightning's sandbox can't see that file. Attach it "
                      "with the + button instead.");
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
    // The chip shows this name and the send reuses it: format characters
    // (bidi overrides) would let "r<RLO>txt.exe" read as a .txt.
    entry.fileName =
        matrix::presentable_name::withoutFormatCharacters(info.fileName());
    entry.sizeBytes = info.size();

    // MIME from content first, extension only as tie-breaker or to narrow a
    // generic container, so a mislabelled extension cannot pick the send path.
    entry.mime = detectMime(path);
    qCDebug(lcAttach) << "attachment classified mime=" << entry.mime;
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
    const auto fail = [&](const QString &message, bool retryable = true) {
        entry.state = QStringLiteral("failed");
        entry.error = message;
        entry.retryable = retryable;
        updateEntry(row);
        Q_EMIT entryPrepared(row);
    };
    if (!result.refusal.isEmpty()) {
        qCInfo(lcAttach) << "svg conversion refused reason="
                         << qPrintable(result.refusal);
        fail(svg::userMessage(result.refusal),
             svg::isRetryable(result.refusal));
        return;
    }
    if (exceedsUploadLimit(result.png.size())) {
        fail(uploadLimitMessage(), false);
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
    entry.retryable = true;
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

namespace {
void removeOwnedRendition(const QString &path)
{
    if (path.isEmpty())
        return;
    QFile::remove(path);
    const QDir parent = QFileInfo(path).dir();
    if (parent.dirName().startsWith(QLatin1String("edit-")))
        QDir().rmdir(parent.absolutePath());
}
}

void AttachmentQueueModel::releaseStaged(const Entry &entry)
{
    if (m_stagedImages && !entry.stagedToken.isEmpty())
        m_stagedImages->remove(entry.stagedToken);
    // A converted SVG's PNG is ours; a picked file never is.
    if (entry.converted)
        removeOwnedRendition(entry.localPath);
    removeOwnedRendition(entry.editPreviewPath);
}

QVariantMap AttachmentQueueModel::renderImageEdit(
    int row, qreal left, qreal top, qreal right, qreal bottom,
    int quarterTurns, bool mirror, const QVariantList &strokes)
{
    const auto fail = [this](const QString &reason) {
        return QVariantMap{{QStringLiteral("error"), reason}};
    };
    if (row < 0 || row >= m_entries.size())
        return fail(tr("The attachment is no longer available."));
    Entry &entry = m_entries[row];
    if (!entry.isImage || entry.isSvg || entry.posterPending
        || entry.state == QLatin1String("dispatching"))
        return fail(tr("This image cannot be edited yet."));
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right)
        || !std::isfinite(bottom) || left < 0 || top < 0 || right > 1
        || bottom > 1 || right - left < 0.01 || bottom - top < 0.01
        || quarterTurns < 0 || quarterTurns > 3 || strokes.size() > 128)
        return fail(tr("Invalid image edit."));

    // Limit compressed input and decoded pixels. Qt may retain a full image
    // while scaling, so refuse pathological dimensions before decoding.
    constexpr qint64 maxInput = 64LL * 1024 * 1024;
    constexpr int maxEdge = 4096;
    if (entry.data.size() > maxInput
        || (!entry.localPath.isEmpty()
            && QFileInfo(entry.localPath).size() > maxInput))
        return fail(tr("This image is too large to edit."));
    QByteArray bytes = entry.data;
    QBuffer buffer(&bytes);
    QImageReader reader;
    if (entry.localPath.isEmpty()) {
        buffer.open(QIODevice::ReadOnly);
        reader.setDevice(&buffer);
    } else {
        reader.setFileName(entry.localPath);
    }
    // A MIME guess alone does not permit SVG into Qt's image handlers.
    const QByteArray format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg"
        && format != "gif" && format != "webp" && format != "bmp"
        && format != "jxl")
        return fail(tr("This image format cannot be edited."));
    reader.setAutoTransform(true);
    bool wasAnimated = reader.imageCount() > 1;
    if (!wasAnimated && (format == "gif" || format == "webp")) {
        QByteArray animationBytes = bytes;
        if (!entry.localPath.isEmpty()) {
            QFile sourceFile(entry.localPath);
            if (sourceFile.open(QIODevice::ReadOnly))
                animationBytes = sourceFile.readAll();
        }
        wasAnimated = lightning::animsniff::isAnimation(animationBytes);
    }
    const QSize sourceSize = reader.size();
    if (!sourceSize.isValid() || sourceSize.width() > 8192
        || sourceSize.height() > 8192
        || qint64(sourceSize.width()) * sourceSize.height()
               > 32LL * 1024 * 1024)
        return fail(tr("This image is too large to edit."));
    if (qMax(sourceSize.width(), sourceSize.height()) > maxEdge)
        reader.setScaledSize(sourceSize.scaled(maxEdge, maxEdge,
                                              Qt::KeepAspectRatio));
    QImage source = reader.read();
    if (source.isNull() || source.width() > maxEdge
        || source.height() > maxEdge || qint64(source.width()) * source.height()
                                     > 16LL * 1024 * 1024)
        return fail(tr("This image could not be decoded safely."));
    const QRect bounds(QPoint(), source.size());
    const QRect crop(qRound(left * source.width()), qRound(top * source.height()),
                     qRound((right - left) * source.width()),
                     qRound((bottom - top) * source.height()));
    const QRect clipped = crop.intersected(bounds);
    if (clipped.isEmpty())
        return fail(tr("The crop is empty."));
    QImage output = source.copy(clipped);
    source = QImage();
    QTransform transform;
    if (mirror)
        transform.scale(-1, 1);
    transform.rotate(quarterTurns * 90);
    output = output.transformed(transform, Qt::FastTransformation);
    if (output.isNull())
        return fail(tr("The image edit failed."));
    // A fresh pixel-only surface prevents QImage text fields from being
    // carried into the PNG by a format plugin. Alpha remains intact.
    QImage clean(output.size(), QImage::Format_ARGB32_Premultiplied);
    if (clean.isNull())
        return fail(tr("The image edit failed."));
    clean.fill(Qt::transparent);
    {
        QPainter flatten(&clean);
        flatten.drawImage(QPoint(), output);
    }
    output = std::move(clean);
    if (!strokes.isEmpty()) {
        QPainter painter(&output);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(255, 45, 45), qMax(2, output.width() / 180),
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const QVariant &strokeValue : strokes) {
            const QVariantList points = strokeValue.toList();
            if (points.size() > 2048)
                return fail(tr("Too many annotation points."));
            QPainterPath path;
            bool first = true;
            for (const QVariant &pointValue : points) {
                const QVariantMap point = pointValue.toMap();
                const qreal x = point.value(QStringLiteral("x")).toDouble();
                const qreal y = point.value(QStringLiteral("y")).toDouble();
                if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1
                    || y < 0 || y > 1)
                    return fail(tr("Invalid annotation point."));
                const QPointF pixel(x * output.width(), y * output.height());
                if (first) { path.moveTo(pixel); first = false; }
                else path.lineTo(pixel);
            }
            if (points.size() == 1)
                painter.drawPoint(path.currentPosition());
            else
                painter.drawPath(path);
        }
    }
    const QString dir = convertedDirectory();
    if (dir.isEmpty())
        return fail(tr("The image edit could not be saved."));
    const QString workDir = QDir(dir).filePath(
        QStringLiteral("edit-%1").arg(m_nextPosterTag++));
    if (!QDir().mkpath(workDir))
        return fail(tr("The image edit could not be saved."));
    QString base = QFileInfo(entry.fileName).completeBaseName();
    if (base.isEmpty())
        base = QStringLiteral("image");
    const QString path = QDir(workDir).filePath(base + QStringLiteral(".png"));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly))
        return fail(tr("The image edit could not be saved."));
    QImageWriter writer(&buffer, "png");
    if (!writer.write(output))
        return fail(tr("The image edit could not be saved."));
    lightning::scratch::ScratchFile file(path);
    if (!file.open() || file.write(encoded) != encoded.size()
        || !file.commit()) {
        return fail(tr("The image edit could not be saved."));
    }
    if (exceedsUploadLimit(QFileInfo(path).size())) {
        removeOwnedRendition(path);
        return fail(uploadLimitMessage());
    }
    removeOwnedRendition(entry.editPreviewPath);
    entry.editPreviewPath = path;
    return {{QStringLiteral("url"), QUrl::fromLocalFile(path)},
            {QStringLiteral("width"), output.width()},
            {QStringLiteral("height"), output.height()},
            {QStringLiteral("animated"), wasAnimated}};
}

bool AttachmentQueueModel::commitImageEdit(int row)
{
    if (row < 0 || row >= m_entries.size())
        return false;
    Entry &entry = m_entries[row];
    if (entry.editPreviewPath.isEmpty() || !QFileInfo::exists(entry.editPreviewPath)
        || entry.state == QLatin1String("dispatching"))
        return false;
    if (entry.converted)
        removeOwnedRendition(entry.localPath);
    if (!entry.stagedToken.isEmpty() && m_stagedImages)
        m_stagedImages->remove(entry.stagedToken);
    entry.stagedToken.clear();
    entry.data.clear();
    if (entry.sourcePath.isEmpty())
        entry.sourcePath = entry.localPath;
    entry.localPath = entry.editPreviewPath;
    entry.editPreviewPath.clear();
    entry.converted = true;
    entry.animated = false;
    entry.mime = QStringLiteral("image/png");
    entry.fileName = QFileInfo(entry.fileName).completeBaseName()
                     + QStringLiteral(".png");
    entry.sizeBytes = QFileInfo(entry.localPath).size();
    QImageReader reader(entry.localPath);
    const QSize size = reader.size();
    entry.width = size.width();
    entry.height = size.height();
    entry.state = QStringLiteral("queued");
    entry.error.clear();
    entry.retryable = true;
    entry.sendRequested = false;
    updateEntry(row);
    return true;
}

void AttachmentQueueModel::cancelImageEdit(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    Entry &entry = m_entries[row];
    removeOwnedRendition(entry.editPreviewPath);
    entry.editPreviewPath.clear();
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

QList<AttachmentQueueModel::Entry> AttachmentQueueModel::takeCommitted()
{
    QList<Entry> taken;
    for (int row = m_entries.size() - 1; row >= 0; --row) {
        if (!m_entries.at(row).committed)
            continue;
        beginRemoveRows({}, row, row);
        taken.prepend(m_entries.takeAt(row));
        endRemoveRows();
    }
    if (!taken.isEmpty())
        Q_EMIT countChanged();
    return taken;
}

void AttachmentQueueModel::restoreEntries(const QList<Entry> &entries)
{
    if (entries.isEmpty())
        return;
    const int first = m_entries.size();
    beginInsertRows({}, first, first + entries.size() - 1);
    m_entries.append(entries);
    endInsertRows();
    Q_EMIT countChanged();
}

void AttachmentQueueModel::discardEntries(const QList<Entry> &entries)
{
    for (const Entry &entry : entries)
        releaseStaged(entry);
}

void AttachmentQueueModel::retryAt(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    Entry &entry = m_entries[row];
    if (entry.state != QLatin1String("failed"))
        return;
    entry.committed = false;
    if (entry.isSvg) {
        // Refused or unreadable conversion: convert again, the file may have
        // changed. It dispatches once it resolves, if the user already sent.
        entry.state = QStringLiteral("queued");
        entry.error.clear();
        entry.retryable = true;
        entry.posterPending = true;
        entry.posterTag = QStringLiteral("send:%1").arg(m_nextPosterTag++);
        updateEntry(row);
        startSvgConversion(row);
        return;
    }
    entry.state = QStringLiteral("queued");
    entry.error.clear();
    entry.retryable = true;
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
    case RetryableRole: return e.retryable;
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
        { RetryableRole, "retryable" },
    };
}
