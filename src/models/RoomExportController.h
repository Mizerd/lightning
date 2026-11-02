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

class RoomExportController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString phase READ phase NOTIFY stateChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
    Q_PROPERTY(bool historyAvailable READ historyAvailable
                   NOTIFY availabilityChanged)
    Q_PROPERTY(QString resultMessage READ resultMessage NOTIFY stateChanged)
    Q_PROPERTY(bool resultOk READ resultOk NOTIFY stateChanged)

public:
    static constexpr int kRowsPerTick = 200;
    static constexpr qsizetype kPayloadChunkBytes = 3 * 256 * 1024;

    enum class Range {
        Loaded = -1,
        Latest = 0,
        All = 1,
        Between = 2,
    };

    struct Job {
        QString roomId;
        QString filePath;
        roomexport::Format format = roomexport::Format::Html;
        roomexport::Options options;
        Range range = Range::Loaded;
        qint64 limit = 0;
        qint64 fromMs = 0;
        qint64 toMs = 0;
        QString fromText;
        QString toText;
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

    QString start(const Job &job);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearResult();
    /// Reveals the file the last successful export wrote in the file manager.
    /// The path stays in C++ (it names the user's folders); QML only asks.
    /// False when there is nothing to reveal or the file is gone.
    Q_INVOKABLE bool showInFolder();

    static TimelineEvent eventFromWalkRow(const QVariantMap &row,
                                          const QString &roomId);
    static QString rangeDescription(const Job &job, const QString &stopReason);

Q_SIGNALS:
    void stateChanged();
    void progressChanged();
    void availabilityChanged();
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
    QString m_resultPath;
    FileLauncher *m_launcher = nullptr;
    quint64 m_generation = 0;

    quint64 m_walkOp = 0;
    quint64 m_handleOp = 0;
    QList<TimelineEvent> m_newestFirst;
    QString m_stopReason;

    QList<TimelineEvent> m_events;
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
