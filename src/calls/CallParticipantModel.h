// The call's participants as a real list model.
//
// Replaces a Q_INVOKABLE QVariantList that QML re-read and reassigned on every
// change: a reassigned JS array is a model reset, and since speaker updates
// arrive continuously, every syllable destroyed every tile and its
// VideoOutput.
//
// Rules:
//
//   * The key is the SFU `identity`, stable for a participant within a call in
//     both identity formats (legacy `@user:server:DEVICE` and sticky sha256).
//     Never a row index or a parsed user id.
//   * Membership changes use begin{Insert,Remove,Move}Rows.
//   * Value changes are per-row dataChanged naming only the changed roles.
//   * Nothing ever resets the model for a value update (a test pins
//     `modelAboutToBeReset` at zero).
//
// Field ownership: applyParticipants() is the only writer of what the SFU
// states (identity, profile, track state, track keys). Speaking level,
// connection quality, raised hand, reaction and local volume arrive on their
// own feeds and are preserved across participant updates.
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>
#include <QtQml/qqmlregistration.h>

/// One participant as the SFU states them; excludes fields owned by other
/// feeds, so a participant update cannot reset those.
struct CallParticipantRow {
    QString identity;
    /// The LiveKit participant sid; speaker levels and connection quality are
    /// keyed on it.
    QString sid;
    QString userId;
    QString displayName;
    QString avatarMxc;
    bool local = false;
    bool micKnown = false;
    bool micMuted = false;
    bool cameraKnown = false;
    bool cameraOn = false;
    QString cameraTrackKey;
    bool screenSharing = false;
    QString screenTrackKey;
};

class CallParticipantModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("CallParticipantModel is exposed via "
                    "app.groupCall.participantModel")

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        IdentityRole = Qt::UserRole + 1,
        UserIdRole,
        DisplayNameRole,
        AvatarMxcRole,
        LocalRole,
        MicKnownRole,
        MicMutedRole,
        CameraKnownRole,
        CameraOnRole,
        CameraTrackKeyRole,
        ScreenSharingRole,
        ScreenTrackKeyRole,
        SpeakingRole,
        SpeakingLevelRole,
        HandRaisedRole,
        /// The emoji of a transient reaction, or "" when none is showing.
        /// Surfaces must draw it behind a Loader: a Label created empty keeps
        /// ItemObservesViewport forever.
        ReactionEmojiRole,
        VolumePercentRole,
        ConnectionQualityRole,
        JoinedAtMsRole,
    };
    Q_ENUM(Roles)

    explicit CallParticipantModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Reconcile against the SFU's current participant list by diffing:
    /// remove, insert, move, and per-role dataChanged for changed fields.
    /// Locally owned fields (level, quality, hand, volume, joinedAtMs) are
    /// preserved.
    void applyParticipants(const QVector<CallParticipantRow> &desired);

    /// Apply one LiveKit SpeakersChanged round, keyed by sid. A sid absent from
    /// the round is not speaking. `speaking` is the SFU's `active` flag or a
    /// non-zero level; an unknown amplitude stays 0.0. A deadband suppresses
    /// dataChanged for invisible level changes.
    void applySpeakers(const QHash<QString, bool> &activeBySid,
                       const QHash<QString, qreal> &levelBySid);

    /// Merge a LiveKit ConnectionQuality round, keyed by sid. It is a delta:
    /// an unmentioned sid keeps its last value.
    void applyConnectionQuality(const QHash<QString, QString> &qualityBySid);

    /// Raise state for one participant, set only from an attributed event
    /// (element-call's `m.reaction` on the raiser's membership).
    void setHandRaised(const QString &identity, bool raised);

    /// Show one transient reaction, expiring at `nowMs + ttlMs`. Returns false
    /// and changes nothing when the identity is unknown, the emoji is empty,
    /// or a reaction is still showing for that participant (element-call's
    /// "one is still playing" rule, which stops re-sending from pinning a
    /// permanent badge).
    bool setReaction(const QString &identity, const QString &emoji,
                     qint64 nowMs, int ttlMs);

    /// Clear every reaction whose deadline has passed at `nowMs`. Called by
    /// the model's timer, and directly by tests.
    void expireReactions(qint64 nowMs);

    /// Local playback volume, 0..200 (100 is unity). Local only; held here so
    /// the control can read it back.
    void setVolumePercent(const QString &identity, int percent);

    /// Remove every row (begin/endRemoveRows, not a reset).
    void clear();

    /// The `participants()` list shape, read from the rows, so the list and
    /// the model cannot disagree.
    QVariantList toVariantList() const;

    /// Row lookup by identity for QML.
    Q_INVOKABLE int indexOfIdentity(const QString &identity) const;
    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void countChanged();

private:
    struct Entry {
        CallParticipantRow row;
        // Locally owned; preserved across applyParticipants().
        bool speaking = false;
        qreal speakingLevel = 0.0;
        bool handRaised = false;
        /// The reaction showing and when it stops; always cleared together.
        QString reactionEmoji;
        qint64 reactionExpiresAtMs = 0;
        int volumePercent = 100;
        QString connectionQuality; // "" = unknown
        qint64 joinedAtMs = 0;
    };

    int indexOf(const QString &identity) const;
    /// Re-arm the single expiry timer to the earliest deadline, or stop it.
    void rearmReactionTimer(qint64 nowMs);
    /// Copy the SFU-owned fields onto an entry, returning the roles that
    /// changed; empty means no signal.
    static QList<int> mergeRow(Entry &entry, const CallParticipantRow &row);

    QVector<Entry> m_rows;
    QTimer m_reactionTimer;
};
