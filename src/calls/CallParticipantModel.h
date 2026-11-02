#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>
#include <QtQml/qqmlregistration.h>

struct CallParticipantRow {
    QString identity;
    QString sid;
    QString userId;
    QString deviceId;
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
        DeviceIdRole,
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

    void applyParticipants(const QVector<CallParticipantRow> &desired);

    void applySpeakers(const QHash<QString, bool> &activeBySid,
                       const QHash<QString, qreal> &levelBySid);

    void applyConnectionQuality(const QHash<QString, QString> &qualityBySid);

    /// Raise state for one participant, set only from an attributed event
    /// (element-call's `m.reaction` on the raiser's membership).
    void setHandRaised(const QString &identity, bool raised);

    bool setReaction(const QString &identity, const QString &emoji,
                     qint64 nowMs, int ttlMs);

    void expireReactions(qint64 nowMs);

    void setVolumePercent(const QString &identity, int percent);

    void clear();

    QVariantList toVariantList() const;

    Q_INVOKABLE int indexOfIdentity(const QString &identity) const;
    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void countChanged();

private:
    struct Entry {
        CallParticipantRow row;
        bool speaking = false;
        qreal speakingLevel = 0.0;
        bool handRaised = false;
        QString reactionEmoji;
        qint64 reactionExpiresAtMs = 0;
        int volumePercent = 100;
        QString connectionQuality;
        qint64 joinedAtMs = 0;
    };

    int indexOf(const QString &identity) const;
    void rearmReactionTimer(qint64 nowMs);
    static QList<int> mergeRow(Entry &entry, const CallParticipantRow &row);

    QVector<Entry> m_rows;
    QTimer m_reactionTimer;
};
