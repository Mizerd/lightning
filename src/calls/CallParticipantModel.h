// The call's people, as a REAL model.
//
// WHAT THIS REPLACES, AND WHY IT HAD TO GO.
//
// Until this file existed the call stage read its participants from
// `SfuCallController::participants()` — a `Q_INVOKABLE QVariantList` that QML
// re-invoked whenever a hand-bumped `refreshTick` changed. QML cannot observe
// a function call, so the whole list was rebuilt and reassigned as a plain JS
// array, and a JS array bound to a view is a MODEL RESET: no insert, no move,
// no dataChanged. Every delegate is destroyed and rebuilt.
//
// That alone would be a performance smell. What made it a defect is WHEN it
// fired: `onSfuSpeakers` emitted `participantsChanged()` on every LiveKit
// SpeakersChanged, i.e. continuously while anybody talks. So every syllable
// destroyed every tile, and with each tile its `VideoOutput` and the
// attach()/detach() pair that routes a video track into it. A speaking ring
// that reacts to amplitude is impossible on top of that, because the thing
// that would animate does not survive the update that drives it.
//
// This is the same lesson the Spaces rail learned on 2026-08-25 ("a JS array
// bound to a ListView is a model RESET on every change"), in a place where
// the cost is a live video surface rather than a drag gesture.
//
// THE RULES THIS CLASS EXISTS TO KEEP:
//
//   * The key is the SFU `identity` — stable for one participant for one
//     call, in BOTH identity formats (the legacy `@user:server:DEVICE` and
//     the sticky sha256 form). Never a row index, never a parsed user id.
//   * Membership changes are begin{Insert,Remove,Move}Rows.
//   * Value changes are per-row `dataChanged` naming ONLY the roles that
//     actually changed.
//   * There is NO path that resets the model for a value update. A
//     QSignalSpy on `modelAboutToBeReset` must stay at zero across any
//     number of speaker updates; the test pins exactly that.
//
// FIELD OWNERSHIP. Two feeds write here and they must not clobber each other:
// `applyParticipants()` carries what the SFU states (identity, profile, track
// state, track keys) and is the only writer of those; the speaking level,
// connection quality, raised hand and local playback volume arrive on their
// own signals and are preserved verbatim across a participant update.
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QtQml/qqmlregistration.h>

/// One participant, as the SFU states them. Deliberately NOT the whole row:
/// the level/quality/hand/volume fields are owned by other feeds and live
/// only inside the model, so a participant update cannot reset them.
struct CallParticipantRow {
    QString identity;
    /// The LiveKit participant sid. Speaker levels and connection quality are
    /// keyed on it, not on the identity, so the model has to hold it.
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
        VolumePercentRole,
        ConnectionQualityRole,
        JoinedAtMsRole,
    };
    Q_ENUM(Roles)

    explicit CallParticipantModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// Reconcile the model against the SFU's current participant list.
    ///
    /// Diffed, never assigned: rows that disappeared are removed, rows that
    /// appeared are inserted at their target position, rows that moved are
    /// MOVED, and surviving rows get a `dataChanged` naming only the fields
    /// that differ. Locally-owned fields (level, quality, hand, volume,
    /// joinedAtMs) are carried across untouched.
    void applyParticipants(const QVector<CallParticipantRow> &desired);

    /// Apply one LiveKit SpeakersChanged round, keyed by participant sid.
    ///
    /// A sid ABSENT from the round is not speaking — LiveKit sends the active
    /// set, so absence is the stop signal. `speaking` is the union of the
    /// SFU's own `active` flag and a non-zero level, so an SFU that publishes
    /// only `active` still lights a (binary) ring and one that publishes only
    /// a level still lights an (amplitude) ring. Nothing is fabricated in
    /// either direction: an unknown amplitude stays 0.0, and the view draws
    /// its minimum ring rather than a made-up size.
    ///
    /// Levels move continuously, so a deadband suppresses `dataChanged` for
    /// changes too small to see. Without it this is a per-round signal storm
    /// across every row.
    void applySpeakers(const QHash<QString, bool> &activeBySid,
                       const QHash<QString, qreal> &levelBySid);

    /// Merge a LiveKit ConnectionQuality round, keyed by participant sid.
    /// A sid the round does not mention keeps its last known value — a
    /// quality report is a delta, and "unmentioned" is not "unknown".
    void applyConnectionQuality(const QHash<QString, QString> &qualityBySid);

    /// Raise state for ONE participant. Hand raise has no wire
    /// representation in MatrixRTC as Lightning speaks it, so in practice the
    /// only row this is ever true for is the local one. See the note on
    /// `HandRaisedRole` in the .cpp.
    void setHandRaised(const QString &identity, bool raised);

    /// Local playback volume, 0..200 (100 is unity; above it is real
    /// amplification, as Discord allows). Local-only: it reaches the audio
    /// pipeline and nothing else. Held here so the control that sets it can
    /// also READ it back — `SfuCallController::setParticipantVolume` was
    /// write-only, so a volume slider had nothing to bind to.
    void setVolumePercent(const QString &identity, int percent);

    /// Everyone leaves. A membership change to zero, so it is
    /// begin/endRemoveRows — still not a reset.
    void clear();

    /// The legacy `participants()` shape, read straight out of the rows.
    /// ONE derivation: the invokable list and the model can no longer
    /// disagree, because the list IS the model.
    QVariantList toVariantList() const;

    /// Row lookup for QML, so a control can address a participant by
    /// identity without walking the view.
    Q_INVOKABLE int indexOfIdentity(const QString &identity) const;
    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void countChanged();

private:
    struct Entry {
        CallParticipantRow row;
        // Locally owned, preserved across every applyParticipants().
        bool speaking = false;
        qreal speakingLevel = 0.0;
        bool handRaised = false;
        int volumePercent = 100;
        QString connectionQuality; // "" = unknown; never rendered as a lie
        qint64 joinedAtMs = 0;
    };

    int indexOf(const QString &identity) const;
    /// Copy the SFU-owned fields onto an existing entry, returning the roles
    /// that actually changed. An empty result means no signal is emitted at
    /// all, which is what keeps a steady call quiet.
    static QList<int> mergeRow(Entry &entry, const CallParticipantRow &row);

    QVector<Entry> m_rows;
};
