#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class MatrixClient;
class MediaBridge;

// Forwards a message by sending its content as a new, unrelated event; Matrix
// has no forward primitive.
//
// - Media is re-uploaded, never mxc-copied: the target room's members may not
//   be entitled to the source mxc, and an encrypted source's `file` block
//   carries keys that must not reach another room. The normal attachment path
//   re-encrypts for the target.
// - Text carries the plain body only; formatted_body can hold pills and
//   permalinks into the source room.
// - The source's relation (reply, m.thread) is never carried over.
// - forwarded() fires only once the send is dispatched. Failures before
//   dispatch set `error`; a media send refused afterwards is reported through
//   forwardFailed(), since the picker has already closed.
// - Redacted, local-echo and undecryptable content is refused in begin().
//
// begin() takes an immutable snapshot of the activated row instead of
// re-resolving it later, because the model can change before a target is
// picked. Media bytes are still fetched fresh through MediaBridge.
class ForwardController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(QString sourceRoomId READ sourceRoomId NOTIFY changed)
    Q_PROPERTY(QString sourceEventId READ sourceEventId NOTIFY changed)
    Q_PROPERTY(QString previewText READ previewText NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool selectionActive READ selectionActive NOTIFY changed)
    Q_PROPERTY(int progressDone READ progressDone NOTIFY changed)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY changed)
    Q_PROPERTY(int failureCount READ failureCount NOTIFY changed)
    Q_PROPERTY(QVariantList failures READ failures NOTIFY changed)
    Q_PROPERTY(QString forwardMode READ forwardMode NOTIFY changed)
    Q_PROPERTY(bool selecting READ selecting NOTIFY changed)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY changed)

public:
    explicit ForwardController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setMediaBridge(MediaBridge *bridge);

    bool active() const { return m_active; }
    QString sourceRoomId() const { return m_sourceRoomId; }
    QString sourceEventId() const { return m_sourceEventId; }
    QString previewText() const { return m_previewText; }
    bool busy() const { return m_busy; }
    QString error() const { return m_error; }

    Q_INVOKABLE void begin(const QString &sourceRoomId,
                          const QString &sourceEventId,
                          const QVariantMap &snapshot);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void forwardTo(const QString &targetRoomId);

    Q_INVOKABLE void beginSelecting(const QString &sourceRoomId);
    Q_INVOKABLE void cancelSelecting();
    Q_INVOKABLE void toggleSelected(const QString &eventId,
                                    const QVariantMap &snapshot);
    Q_INVOKABLE bool isSelected(const QString &eventId) const;

    Q_INVOKABLE void beginSelection(const QString &sourceRoomId,
                                    const QVariantList &snapshots);
    Q_INVOKABLE void setForwardMode(const QString &mode);
    Q_INVOKABLE void sendSelection(const QVariantList &targets);
    Q_INVOKABLE void retryFailures();

    bool selectionActive() const { return m_selectionActive; }
    int progressDone() const { return m_progressDone; }
    int progressTotal() const { return m_progressTotal; }
    int failureCount() const { return int(m_failures.size()); }
    QVariantList failures() const { return m_failures; }
    QString forwardMode() const { return m_mode; }
    bool selecting() const { return m_selecting; }
    int selectedCount() const { return int(m_selectionSnapshots.size()); }

public:
    static QString sanitizedForwardFilename(const QString &raw);

Q_SIGNALS:
    void changed();
    void forwarded(const QString &targetRoomId);
    void forwardFailed(const QString &targetRoomId, const QString &message);

private Q_SLOTS:
    void onAttachmentQueueFinished(quint64 opId, const QString &roomId,
                                   bool ok, const QString &category);
    void onMediaBytesForStar(const QString &mediaKey, bool ok,
                             const QByteArray &bytes, const QString &category);

private:
    void resetToIdle();
    void setError(const QString &message);
    static bool snapshotIsMedia(const QVariantMap &snapshot);
    static QString buildPreview(const QVariantMap &snapshot);

    MatrixClient *m_client = nullptr;
    MediaBridge *m_mediaBridge = nullptr;

    bool m_active = false;
    bool m_busy = false;
    QString m_error;

    QString m_sourceRoomId;
    QString m_sourceEventId;
    QString m_previewText;

    // Frozen at begin() — never re-read from a live model.
    QVariantMap m_snapshot;

    struct Pending {
        QVariantMap snapshot;
        QString eventId;
        QString targetRoomId;
        QString threadRootId;
    };
    void pumpQueue();
    void notePairResult(const Pending &pair, bool ok, const QString &message);
    QString contextPrefixFor(const QVariantMap &snapshot) const;

    bool m_selectionActive = false;
    bool m_selecting = false;
    QStringList m_selectedIds;
    QString m_mode = QStringLiteral("content");
    QVariantList m_selectionSnapshots;
    QList<Pending> m_queue;
    QList<Pending> m_failedPairs;
    QVariantList m_failures;
    int m_progressDone = 0;
    int m_progressTotal = 0;
    bool m_pumpBusy = false;

    quint64 m_generation = 0;
    quint64 m_pendingGeneration = 0;
    QString m_pendingMediaKey;
    QHash<quint64, QString> m_dispatchedSends;
    QString m_pendingTargetRoomId;
};
