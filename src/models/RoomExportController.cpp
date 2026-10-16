#include "models/RoomExportController.h"

#include "app/FileLauncher.h"
#include "matrix/MatrixClient.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QTimeZone>

#include <algorithm>

Q_LOGGING_CATEGORY(lcRoomExport, "lightning.roomexport")

namespace {
const char *formatName(roomexport::Format format)
{
    switch (format) {
    case roomexport::Format::Json: return "json";
    case roomexport::Format::Html: return "html";
    case roomexport::Format::PlainText: break;
    }
    return "text";
}
} // namespace

RoomExportController::RoomExportController(QObject *parent)
    : QObject(parent)
{
}

RoomExportController::~RoomExportController()
{
    // A destroyed controller must not leave a walk running or handles held.
    if (m_client) {
        if (m_handleOp != 0)
            m_client->cancelRoomExport(m_handleOp);
        if (m_fetchOp != 0)
            m_client->cancelMediaFetch(m_fetchOp);
    }
}

void RoomExportController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client) {
        if (m_busy)
            cancel();
        disconnect(m_client, nullptr, this, nullptr);
    }
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::roomExportPage,
                this, &RoomExportController::onPage);
        connect(m_client, &MatrixClient::roomExportFailed,
                this, &RoomExportController::onWalkFailed);
        // Only this export's own op ids are acted on; every other media
        // answer belongs to someone else.
        connect(m_client, &MatrixClient::mediaReady, this,
                [this](quint64 opId, const QString &, int,
                       const QByteArray &bytes, const QString &,
                       const QString &) { onMediaReady(opId, bytes); });
        connect(m_client, &MatrixClient::mediaFailed, this,
                [this](quint64 opId, const QString &, int, const QString &) {
                    onMediaFailed(opId);
                });
        // An export belongs to the account that started it.
        connect(m_client, &MatrixClient::loggedOut, this, [this] {
            if (m_busy)
                cancel();
            clearResult();
            Q_EMIT availabilityChanged();
        });
        connect(m_client, &MatrixClient::loginSucceeded, this,
                [this] { Q_EMIT availabilityChanged(); });
    }
    Q_EMIT availabilityChanged();
}

bool RoomExportController::historyAvailable() const
{
    return m_client && m_client->supportsRoomExportWalk();
}

TimelineEvent RoomExportController::eventFromWalkRow(const QVariantMap &row,
                                                     const QString &roomId)
{
    TimelineEvent e;
    e.eventId = row.value(QStringLiteral("eventId")).toString();
    e.roomId = roomId;
    e.sender = row.value(QStringLiteral("sender")).toString();
    e.senderDisplayName = row.value(QStringLiteral("senderName")).toString();
    e.timestamp = QDateTime::fromMSecsSinceEpoch(
        row.value(QStringLiteral("timestampMs")).toLongLong(), QTimeZone::UTC);
    e.body = row.value(QStringLiteral("body")).toString();
    // Untrusted HTML: only the HTML writer reads it, through the sanitizer.
    e.formattedBody = row.value(QStringLiteral("formattedBody")).toString();
    e.threadRootId = row.value(QStringLiteral("threadRoot")).toString();
    e.replyToEventId = row.value(QStringLiteral("replyTo")).toString();
    e.edited = row.value(QStringLiteral("edited")).toBool();
    e.status = TimelineEvent::Sent;

    const QString kind = row.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("notice")) {
        e.type = TimelineEvent::Notice;
    } else if (kind == QLatin1String("emote")) {
        e.type = TimelineEvent::Emote;
    } else if (kind == QLatin1String("image")) {
        e.type = TimelineEvent::Image;
    } else if (kind == QLatin1String("video")) {
        e.type = TimelineEvent::Video;
    } else if (kind == QLatin1String("audio")
               || kind == QLatin1String("voice")) {
        e.type = TimelineEvent::Audio;
        e.mediaIsVoice = kind == QLatin1String("voice");
    } else if (kind == QLatin1String("file")) {
        e.type = TimelineEvent::File;
    } else if (kind == QLatin1String("sticker")) {
        e.type = TimelineEvent::Sticker;
    } else if (kind == QLatin1String("poll")) {
        e.type = TimelineEvent::Poll;
    } else if (kind == QLatin1String("redacted")) {
        e.type = TimelineEvent::TextMessage;
        e.redacted = true;
        e.body.clear();
        e.formattedBody.clear();
    } else if (kind == QLatin1String("undecryptable")) {
        e.type = TimelineEvent::Unknown;
        e.undecryptable = true;
        e.isEncrypted = true;
    } else {
        // text, location, and any msgtype without a renderer of its own: the
        // body is the spec's fallback text.
        e.type = TimelineEvent::TextMessage;
    }
    if (roomexport::isAttachment(e)) {
        e.mediaFilename = row.value(QStringLiteral("filename")).toString();
        e.mediaMimetype = row.value(QStringLiteral("mimetype")).toString();
        e.mediaSize = row.value(QStringLiteral("size")).toLongLong();
        // An opaque handle for fetchMedia, never a URL.
        e.mediaKey = row.value(QStringLiteral("mediaKey")).toString();
        e.mediaSourceAvailable = !e.mediaKey.isEmpty();
    }
    return e;
}

QString RoomExportController::rangeDescription(const Job &job,
                                               const QString &stopReason)
{
    // Written into the file, which (like the existing text and JSON exports)
    // is in English.
    QString out;
    switch (job.range) {
    case Range::Loaded:
        return QString();
    case Range::Latest:
        out = QStringLiteral("the latest %1 messages").arg(job.limit);
        break;
    case Range::All:
        out = QStringLiteral("every message from the start of the room");
        break;
    case Range::Between:
        if (!job.fromText.isEmpty() && !job.toText.isEmpty())
            out = QStringLiteral("the messages from %1 to %2")
                      .arg(job.fromText, job.toText);
        else if (!job.fromText.isEmpty())
            out = QStringLiteral("the messages from %1 on").arg(job.fromText);
        else
            out = QStringLiteral("the messages up to %1").arg(job.toText);
        break;
    }
    if (stopReason == QLatin1String("cap"))
        out += QStringLiteral(" (stopped at the 100,000-message limit)");
    return out;
}

QString RoomExportController::start(const Job &job)
{
    if (m_busy)
        return tr("An export is already running.");
    if (job.roomId.isEmpty())
        return tr("No room is open.");
    if (job.filePath.isEmpty())
        return tr("Choose a file on this computer.");

    reset();
    m_job = job;
    m_resultMessage.clear();
    m_resultPath.clear();
    m_resultOk = false;
    m_busy = true;

    if (job.range == Range::Loaded) {
        m_events = job.loadedEvents;
        int undecryptable = 0;
        for (const TimelineEvent &e : std::as_const(m_events)) {
            if (roomexport::isExportable(e) && e.undecryptable)
                ++undecryptable;
        }
        m_job.options.undecryptableCount = undecryptable;
        Q_EMIT stateChanged();
        beginWriting();
        return QString();
    }

    if (!historyAvailable()) {
        reset();
        Q_EMIT stateChanged();
        return tr("This account cannot read room history for an export.");
    }
    m_walkOp = m_client->startRoomExport(job.roomId, int(job.range),
                                         job.limit, job.fromMs, job.toMs);
    if (m_walkOp == 0) {
        reset();
        Q_EMIT stateChanged();
        return tr("Could not start reading the room's history.");
    }
    m_handleOp = m_walkOp;
    setPhase(QStringLiteral("walking"));
    setProgress(tr("Reading the room's history…"));
    Q_EMIT stateChanged();
    return QString();
}

void RoomExportController::onPage(quint64 opId, const QString &roomId,
                                  const QVariantList &rows, qint64 rowsTotal,
                                  qint64 scannedTotal, qint64 undecryptable,
                                  bool done, const QString &stopReason,
                                  bool encryptedRoom)
{
    Q_UNUSED(scannedTotal);
    Q_UNUSED(encryptedRoom);
    // Another export, a cancelled one or another account's: not ours.
    if (!m_busy || opId == 0 || opId != m_walkOp || roomId != m_job.roomId)
        return;
    for (const QVariant &value : rows)
        m_newestFirst.append(eventFromWalkRow(value.toMap(), m_job.roomId));
    if (m_job.range == Range::Latest) {
        setProgress(tr("Fetched %1 of %2 messages…")
                        .arg(rowsTotal).arg(m_job.limit));
    } else {
        setProgress(tr("Fetched %1 messages…").arg(rowsTotal));
    }
    if (!done)
        return;

    m_walkOp = 0;
    m_stopReason = stopReason;
    // The walk is newest-first; a file reads oldest-first.
    m_events.reserve(m_newestFirst.size());
    for (auto it = m_newestFirst.crbegin(); it != m_newestFirst.crend(); ++it)
        m_events.append(*it);
    m_newestFirst.clear();
    m_job.options.undecryptableCount = int(undecryptable);
    m_job.options.reachesStartOfRoom = stopReason == QLatin1String("start");
    beginWriting();
}

void RoomExportController::onWalkFailed(quint64 opId, const QString &roomId,
                                        const QString &message)
{
    if (!m_busy || opId == 0 || opId != m_walkOp || roomId != m_job.roomId)
        return;
    // Not the reason text: a transport error can quote the request URL, which
    // names the room.
    Q_UNUSED(message);
    qCWarning(lcRoomExport) << "history walk failed";
    m_walkOp = 0;
    fail(tr("Could not read the room's history. Try again later."));
}

void RoomExportController::beginWriting()
{
    roomexport::Options &options = m_job.options;
    switch (m_job.range) {
    case Range::Loaded:
        options.scope = QStringLiteral("loaded-timeline");
        break;
    case Range::Latest:
        options.scope = QStringLiteral("latest");
        break;
    case Range::All:
        options.scope = QStringLiteral("from-beginning");
        break;
    case Range::Between:
        options.scope = QStringLiteral("date-range");
        break;
    }
    options.rangeDescription = rangeDescription(m_job, m_stopReason);
    const bool textAllowed =
        !options.encrypted || options.allowEncryptedPlaintext;
    // Attachments are an HTML feature, and decrypted media is private content
    // that the plaintext opt-in governs as much as the text.
    options.includeAttachments = options.includeAttachments
        && m_job.format == roomexport::Format::Html && textAllowed;

    if (m_job.format != roomexport::Format::Html) {
        writeTextOrJson();
        return;
    }

    m_file = std::make_unique<QSaveFile>(m_job.filePath);
    if (!m_file->open(QIODevice::WriteOnly)) {
        fail(tr("Could not write that file."));
        return;
    }
    m_writer = std::make_unique<roomexport::HtmlWriter>(m_file.get(), options);
    QHash<QString, QString> names;
    for (const TimelineEvent &e : std::as_const(m_events)) {
        if (!e.senderDisplayName.trimmed().isEmpty())
            names.insert(e.sender, e.senderDisplayName.trimmed());
    }
    m_writer->setDisplayNames(names);
    m_writer->writeHeader(roomexport::exportableCount(m_events));

    m_budgetLeft = options.attachmentBudgetBytes;
    m_attachmentsTotal = 0;
    for (const TimelineEvent &e : std::as_const(m_events)) {
        if (roomexport::isExportable(e) && wantsFetch(e))
            ++m_attachmentsTotal;
    }
    m_index = 0;
    setPhase(m_attachmentsTotal > 0 ? QStringLiteral("attachments")
                                    : QStringLiteral("writing"));
    setProgress(tr("Writing the file…"));
    scheduleStep();
}

void RoomExportController::writeTextOrJson()
{
    setPhase(QStringLiteral("writing"));
    setProgress(tr("Writing the file…"));
    const QByteArray bytes =
        roomexport::render(m_events, m_job.options, m_job.format).toUtf8();
    m_file = std::make_unique<QSaveFile>(m_job.filePath);
    if (!m_file->open(QIODevice::WriteOnly)) {
        fail(tr("Could not write that file."));
        return;
    }
    if (m_file->write(bytes) != bytes.size()) {
        fail(tr("Could not finish writing that file."));
        return;
    }
    m_written = roomexport::exportableCount(m_events);
    if (!m_file->commit()) {
        fail(tr("Could not finish writing that file."));
        return;
    }
    succeed();
}

bool RoomExportController::wantsFetch(const TimelineEvent &event) const
{
    return m_job.options.includeAttachments && roomexport::isAttachment(event)
        && !event.redacted && !event.mediaKey.isEmpty()
        && !roomexport::declaredSvg(event);
}

roomexport::Attachment
RoomExportController::unfetchedAttachment(const TimelineEvent &event) const
{
    roomexport::Attachment a;
    if (!m_job.options.includeAttachments || !roomexport::isAttachment(event))
        return a; // NotIncluded
    if (roomexport::declaredSvg(event))
        a.state = roomexport::AttachmentState::Svg;
    else if (event.mediaKey.isEmpty())
        a.state = roomexport::AttachmentState::Unavailable;
    return a;
}

void RoomExportController::scheduleStep()
{
    const quint64 generation = m_generation;
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation == m_generation && m_busy)
            step();
    }, Qt::QueuedConnection);
}

void RoomExportController::step()
{
    if (!m_writer || m_fetchOp != 0 || m_writingPayload)
        return;
    for (int n = 0; n < kRowsPerTick; ++n) {
        if (m_index >= m_events.size()) {
            finishWriting();
            return;
        }
        const TimelineEvent &event = m_events.at(m_index);
        if (!roomexport::isExportable(event)) {
            ++m_index;
            continue;
        }
        if (wantsFetch(event)) {
            const qint64 declared = event.mediaSize;
            if (declared > m_budgetLeft) {
                // Known not to fit: no download at all.
                ++m_attachmentsDone;
                roomexport::Attachment over;
                over.state = roomexport::AttachmentState::OverLimit;
                m_writer->writeEvent(event, over);
                ++m_written;
                ++m_index;
                continue;
            }
            setProgress(tr("Downloading attachments %1 of %2…")
                            .arg(m_attachmentsDone + 1)
                            .arg(m_attachmentsTotal));
            // Full payload, Save As timeout class: the backend bounds it.
            m_fetchOp = m_client ? m_client->fetchMedia(event.mediaKey, 0, 2)
                                 : 0;
            if (m_fetchOp == 0) {
                ++m_attachmentsDone;
                roomexport::Attachment failed;
                failed.state = roomexport::AttachmentState::Failed;
                m_writer->writeEvent(event, failed);
                ++m_written;
                ++m_index;
                continue;
            }
            return; // resumed by onMediaReady / onMediaFailed
        }
        m_writer->writeEvent(event, unfetchedAttachment(event));
        ++m_written;
        ++m_index;
        if (!m_writer->ok()) {
            fail(tr("Could not write that file."));
            return;
        }
    }
    scheduleStep();
}

void RoomExportController::onMediaReady(quint64 opId, const QByteArray &bytes)
{
    if (!m_busy || opId == 0 || opId != m_fetchOp)
        return;
    m_fetchOp = 0;
    ++m_attachmentsDone;
    roomexport::Attachment attachment;
    const TimelineEvent &event = m_events.at(m_index);
    if (bytes.isEmpty()) {
        attachment.state = roomexport::AttachmentState::Failed;
    } else if (roomexport::refusedAsSvg(event, bytes)) {
        // Never embedded, so never charged to the size limit.
        attachment.state = roomexport::AttachmentState::Svg;
    } else if (bytes.size() > m_budgetLeft) {
        // The declared size was absent or wrong; the bytes decide.
        attachment.state = roomexport::AttachmentState::OverLimit;
    } else {
        attachment.state = roomexport::AttachmentState::Embedded;
        attachment.bytes = bytes;
        m_budgetLeft -= bytes.size();
        ++m_attachmentsEmbedded;
    }
    writeRowChunked(event, attachment);
}

void RoomExportController::onMediaFailed(quint64 opId)
{
    if (!m_busy || opId == 0 || opId != m_fetchOp)
        return;
    m_fetchOp = 0;
    ++m_attachmentsDone;
    roomexport::Attachment failed;
    failed.state = roomexport::AttachmentState::Failed;
    writeRowChunked(m_events.at(m_index), failed);
}

void RoomExportController::writeRowChunked(
    const TimelineEvent &event, const roomexport::Attachment &attachment)
{
    if (m_writer->beginEvent(event, attachment) == 0) {
        ++m_written;
        ++m_index;
        if (!m_writer->ok()) {
            fail(tr("Could not write that file."));
            return;
        }
        scheduleStep();
        return;
    }
    m_writingPayload = true;
    writePayloadStep();
}

void RoomExportController::writePayloadStep()
{
    if (!m_writer)
        return;
    if (m_writer->writePayloadChunk(kPayloadChunkBytes)) {
        const quint64 generation = m_generation;
        QMetaObject::invokeMethod(this, [this, generation] {
            if (generation == m_generation && m_busy)
                writePayloadStep();
        }, Qt::QueuedConnection);
        return;
    }
    m_writer->endEvent();
    m_writingPayload = false;
    ++m_written;
    ++m_index;
    if (!m_writer->ok()) {
        fail(tr("Could not write that file."));
        return;
    }
    scheduleStep();
}

void RoomExportController::finishWriting()
{
    m_writer->writeFooter();
    if (!m_writer->ok()) {
        fail(tr("Could not write that file."));
        return;
    }
    if (!m_file->commit()) {
        fail(tr("Could not finish writing that file."));
        return;
    }
    succeed();
}

void RoomExportController::setPhase(const QString &phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    Q_EMIT stateChanged();
}

void RoomExportController::setProgress(const QString &text)
{
    if (m_progressText == text)
        return;
    m_progressText = text;
    Q_EMIT progressChanged();
}

void RoomExportController::succeed()
{
    // Counts and flags only: never the path, the room or a body.
    qCInfo(lcRoomExport) << "room exported messages=" << m_written
                         << "format=" << formatName(m_job.format)
                         << "range=" << int(m_job.range)
                         << "encrypted=" << m_job.options.encrypted
                         << "text_included="
                         << (!m_job.options.encrypted
                             || m_job.options.allowEncryptedPlaintext)
                         << "attachments_embedded=" << m_attachmentsEmbedded
                         << "of" << m_attachmentsTotal;
    const int written = m_written;
    const QString path = m_job.filePath;
    reset();
    m_resultOk = true;
    m_resultPath = path;
    // "events", not "messages": the file also carries the room's state
    // events (joins, topic changes), and the dialog counts them the same way.
    const QString name = QFileInfo(path).fileName();
    m_resultMessage = written == 1
        ? tr("Exported 1 event to %1.").arg(name)
        : tr("Exported %1 events to %2.").arg(written).arg(name);
    Q_EMIT stateChanged();
    Q_EMIT finished(true, m_resultMessage);
}

void RoomExportController::fail(const QString &message)
{
    qCWarning(lcRoomExport) << "room export failed phase=" << m_phase;
    reset();
    m_resultOk = false;
    m_resultMessage = message;
    Q_EMIT stateChanged();
    Q_EMIT finished(false, m_resultMessage);
}

void RoomExportController::cancel()
{
    if (!m_busy)
        return;
    qCInfo(lcRoomExport) << "room export cancelled phase=" << m_phase;
    reset();
    m_resultOk = false;
    m_resultMessage = tr("Export cancelled. No file was written.");
    Q_EMIT stateChanged();
    Q_EMIT finished(false, m_resultMessage);
}

void RoomExportController::clearResult()
{
    if (m_resultMessage.isEmpty() && !m_resultOk)
        return;
    m_resultMessage.clear();
    m_resultPath.clear();
    m_resultOk = false;
    Q_EMIT stateChanged();
}

bool RoomExportController::showInFolder()
{
    if (!m_resultOk || m_resultPath.isEmpty())
        return false;
    if (!m_launcher)
        m_launcher = new FileLauncher(this);
    return m_launcher->showInFolder(m_resultPath);
}

void RoomExportController::reset()
{
    ++m_generation;
    if (m_client) {
        // Stops a walk still running and forgets every attachment handle.
        if (m_handleOp != 0)
            m_client->cancelRoomExport(m_handleOp);
        if (m_fetchOp != 0)
            m_client->cancelMediaFetch(m_fetchOp);
    }
    m_walkOp = 0;
    m_handleOp = 0;
    m_fetchOp = 0;
    m_writer.reset();
    if (m_file) {
        // Never committed: the destination is left as it was.
        m_file->cancelWriting();
        m_file.reset();
    }
    m_newestFirst.clear();
    m_events.clear();
    m_stopReason.clear();
    m_index = 0;
    m_written = 0;
    m_writingPayload = false;
    m_budgetLeft = 0;
    m_attachmentsTotal = 0;
    m_attachmentsDone = 0;
    m_attachmentsEmbedded = 0;
    m_busy = false;
    m_phase.clear();
    if (!m_progressText.isEmpty()) {
        m_progressText.clear();
        Q_EMIT progressChanged();
    }
}
