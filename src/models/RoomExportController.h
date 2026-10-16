#pragma once

#include "matrix/TimelineEvent.h"
#include "models/RoomExport.h"
#include "models/RoomExportHtml.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

#include <memory>

class MatrixClient;
class FileLauncher;
class QSaveFile;

/// Runs one room export at a time: optionally walks the room's history
/// (rust/src/roomexport.rs, never the live timeline), then writes the file,
/// fetching attachments one by one through the controlled media path when an
/// HTML export embeds them.
///
/// The dialog stays responsive: rows are written a batch per event-loop turn,
/// an attachment's base64 a chunk per turn, and nothing waits synchronously.
/// cancel() really stops: it aborts the walk, cancels the download in flight
/// and discards the partly written file (QSaveFile is never committed).
///
/// Encrypted rooms keep the one rule the other formats follow: without
/// `allowEncryptedPlaintext` no body is written AND no attachment is fetched,
/// since decrypted media is private content too.
///
/// Late answers cannot cross exports or accounts: pages, media and failures
/// are matched on the op ids this export started, and sign-out cancels.
/// Logs carry counts and flags only, never a path, a name or a body.
class RoomExportController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    /// "", "walking" (reading history), "attachments", "writing".
    Q_PROPERTY(QString phase READ phase NOTIFY stateChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
    /// The backend can walk history beyond the loaded timeline.
    Q_PROPERTY(bool historyAvailable READ historyAvailable
                   NOTIFY availabilityChanged)
    Q_PROPERTY(QString resultMessage READ resultMessage NOTIFY stateChanged)
    Q_PROPERTY(bool resultOk READ resultOk NOTIFY stateChanged)

public:
    /// Rows written per event-loop turn when no attachment is in the way.
    static constexpr int kRowsPerTick = 200;
    /// Raw attachment bytes base64-encoded per event-loop turn.
    static constexpr qsizetype kPayloadChunkBytes = 3 * 256 * 1024;

    enum class Range {
        Loaded = -1,   ///< What the timeline has loaded (no walk).
        Latest = 0,    ///< The newest `limit` messages.
        All = 1,       ///< From the start of the room.
        Between = 2,   ///< [fromMs, toMs].
    };

    struct Job {
        QString roomId;
        /// Local path the save dialog chose.
        QString filePath;
        roomexport::Format format = roomexport::Format::Html;
        /// Room name, exporter, encryption flags; scope fields are filled in
        /// here.
        roomexport::Options options;
        Range range = Range::Loaded;
        qint64 limit = 0;
        qint64 fromMs = 0;
        qint64 toMs = 0;
        /// "YYYY-MM-DD" as typed, for the file's range sentence.
        QString fromText;
        QString toText;
        /// For Range::Loaded: the timeline's rows, oldest first.
        QList<TimelineEvent> loadedEvents;
    };

    explicit RoomExportController(QObject *parent = nullptr);
    ~RoomExportController() override;

    void setClient(MatrixClient *client);

    bool busy() const { return m_busy; }
    QString phase() const { return m_phase; }
    QString progressText() const { return m_progressText; }
    bool historyAvailable() const;
    QString resultMessage() const { return m_resultMessage; }
    bool resultOk() const { return m_resultOk; }

    /// Starts `job`. Returns "" when started, or a short reason when refused
    /// (nothing was started then). Completion arrives on finished().
    QString start(const Job &job);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearResult();
    /// Reveals the file the last successful export wrote in the file manager.
    /// The path stays in C++ (it names the user's folders); QML only asks.
    /// False when there is nothing to reveal or the file is gone.
    Q_INVOKABLE bool showInFolder();

    /// A walk row (as MatrixClient::roomExportPage carries it) as the event
    /// the writers take. Public for tests.
    static TimelineEvent eventFromWalkRow(const QVariantMap &row,
                                          const QString &roomId);
    /// The file's sentence for a range, e.g. "the latest 100 messages".
    static QString rangeDescription(const Job &job, const QString &stopReason);

Q_SIGNALS:
    void stateChanged();
    void progressChanged();
    void availabilityChanged();
    /// The export ended: written (ok), failed or cancelled.
    void finished(bool ok, const QString &message);

private:
    void onPage(quint64 opId, const QString &roomId, const QVariantList &rows,
                qint64 rowsTotal, qint64 scannedTotal, qint64 undecryptable,
                bool done, const QString &stopReason, bool encryptedRoom);
    void onWalkFailed(quint64 opId, const QString &roomId,
                      const QString &message);
    void onMediaReady(quint64 opId, const QByteArray &bytes);
    void onMediaFailed(quint64 opId);

    void beginWriting();
    void writeTextOrJson();
    void scheduleStep();
    void step();
    void writeRowChunked(const TimelineEvent &event,
                         const roomexport::Attachment &attachment);
    void writePayloadStep();
    void finishWriting();
    bool wantsFetch(const TimelineEvent &event) const;
    roomexport::Attachment unfetchedAttachment(const TimelineEvent &event) const;
    void setPhase(const QString &phase);
    void setProgress(const QString &text);
    void fail(const QString &message);
    void succeed();
    void reset();

    QPointer<MatrixClient> m_client;
    Job m_job;
    bool m_busy = false;
    QString m_phase;
    QString m_progressText;
    QString m_resultMessage;
    bool m_resultOk = false;
    /// The file the last successful export wrote; cleared with the result.
    QString m_resultPath;
    FileLauncher *m_launcher = nullptr;
    /// Bumped on every start/cancel/reset, so a queued step of a finished
    /// export does nothing.
    quint64 m_generation = 0;

    // Walk.
    quint64 m_walkOp = 0;
    /// The op whose attachment handles are registered in Rust, released when
    /// the export ends.
    quint64 m_handleOp = 0;
    QList<TimelineEvent> m_newestFirst;
    QString m_stopReason;

    // Writing.
    QList<TimelineEvent> m_events;  ///< Oldest first.
    std::unique_ptr<QSaveFile> m_file;
    std::unique_ptr<roomexport::HtmlWriter> m_writer;
    qsizetype m_index = 0;
    int m_written = 0;
    quint64 m_fetchOp = 0;
    bool m_writingPayload = false;
    qint64 m_budgetLeft = 0;
    int m_attachmentsTotal = 0;
    int m_attachmentsDone = 0;
    int m_attachmentsEmbedded = 0;
};
