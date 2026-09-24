#include "calls/CallParticipantModel.h"

#include <QDateTime>
#include <QVariantMap>
#include <QtMath>

#include <utility>

namespace {
/// Level changes below this are invisible on a thin ring, and signalling them
/// would storm every row each speakers round. The speaking flag always emits.
constexpr qreal kLevelDeadband = 0.02;

/// 0..200: above 100 is real amplification, which the store and the engine
/// both support.
int clampVolume(int percent)
{
    return percent < 0 ? 0 : (percent > 200 ? 200 : percent);
}
} // namespace

CallParticipantModel::CallParticipantModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // One single-shot timer for the model, armed to the earliest outstanding
    // deadline (not a repeating timer, not one per row).
    m_reactionTimer.setSingleShot(true);
    connect(&m_reactionTimer, &QTimer::timeout, this, [this] {
        expireReactions(QDateTime::currentMSecsSinceEpoch());
    });
}

int CallParticipantModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QHash<int, QByteArray> CallParticipantModel::roleNames() const
{
    return {
        { IdentityRole, "identity" },
        { UserIdRole, "userId" },
        { DisplayNameRole, "displayName" },
        { AvatarMxcRole, "avatarMxc" },
        { LocalRole, "local" },
        { MicKnownRole, "micKnown" },
        { MicMutedRole, "micMuted" },
        { CameraKnownRole, "cameraKnown" },
        { CameraOnRole, "cameraOn" },
        { CameraTrackKeyRole, "cameraTrackKey" },
        { ScreenSharingRole, "screenSharing" },
        { ScreenTrackKeyRole, "screenTrackKey" },
        { SpeakingRole, "speaking" },
        { SpeakingLevelRole, "speakingLevel" },
        { HandRaisedRole, "handRaised" },
        { ReactionEmojiRole, "reactionEmoji" },
        { VolumePercentRole, "volumePercent" },
        { ConnectionQualityRole, "connectionQuality" },
        { JoinedAtMsRole, "joinedAtMs" },
    };
}

QVariant CallParticipantModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Entry &entry = m_rows.at(index.row());
    switch (role) {
    case IdentityRole:
        return entry.row.identity;
    case UserIdRole:
        return entry.row.userId;
    case DisplayNameRole:
        return entry.row.displayName;
    case AvatarMxcRole:
        return entry.row.avatarMxc;
    case LocalRole:
        return entry.row.local;
    case MicKnownRole:
        return entry.row.micKnown;
    case MicMutedRole:
        return entry.row.micMuted;
    case CameraKnownRole:
        return entry.row.cameraKnown;
    case CameraOnRole:
        return entry.row.cameraOn;
    case CameraTrackKeyRole:
        return entry.row.cameraTrackKey;
    case ScreenSharingRole:
        return entry.row.screenSharing;
    case ScreenTrackKeyRole:
        return entry.row.screenTrackKey;
    case SpeakingRole:
        return entry.speaking;
    case SpeakingLevelRole:
        return entry.speakingLevel;
    // Both come from element-call's wire formats (a hand is an `m.reaction`
    // on the raiser's `m.call.member` event; a reaction is an
    // `io.element.call.reaction` referencing it) and are set only from
    // attributed events.
    case HandRaisedRole:
        return entry.handRaised;
    // Empty is the normal state and must render as nothing; see the role's
    // declaration.
    case ReactionEmojiRole:
        return entry.reactionEmoji;
    case VolumePercentRole:
        return entry.volumePercent;
    case ConnectionQualityRole:
        return entry.connectionQuality;
    case JoinedAtMsRole:
        return entry.joinedAtMs;
    default:
        return {};
    }
}

int CallParticipantModel::indexOf(const QString &identity) const
{
    if (identity.isEmpty())
        return -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).row.identity == identity)
            return i;
    }
    return -1;
}

int CallParticipantModel::indexOfIdentity(const QString &identity) const
{
    return indexOf(identity);
}

QVariantMap CallParticipantModel::get(int row) const
{
    QVariantMap out;
    if (row < 0 || row >= m_rows.size())
        return out;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        out.insert(QString::fromUtf8(it.value()), data(index(row), it.key()));
    return out;
}

QList<int> CallParticipantModel::mergeRow(Entry &entry,
                                          const CallParticipantRow &row)
{
    QList<int> changed;
    // `sid` has no role (it keys the level and quality feeds); keep it
    // current without emitting dataChanged.
    entry.row.sid = row.sid;
    const auto note = [&changed](int role) { changed.append(role); };
    if (entry.row.userId != row.userId) {
        entry.row.userId = row.userId;
        note(UserIdRole);
    }
    if (entry.row.displayName != row.displayName) {
        entry.row.displayName = row.displayName;
        note(DisplayNameRole);
    }
    if (entry.row.avatarMxc != row.avatarMxc) {
        entry.row.avatarMxc = row.avatarMxc;
        note(AvatarMxcRole);
    }
    if (entry.row.local != row.local) {
        entry.row.local = row.local;
        note(LocalRole);
    }
    if (entry.row.micKnown != row.micKnown) {
        entry.row.micKnown = row.micKnown;
        note(MicKnownRole);
    }
    if (entry.row.micMuted != row.micMuted) {
        entry.row.micMuted = row.micMuted;
        note(MicMutedRole);
    }
    if (entry.row.cameraKnown != row.cameraKnown) {
        entry.row.cameraKnown = row.cameraKnown;
        note(CameraKnownRole);
    }
    if (entry.row.cameraOn != row.cameraOn) {
        entry.row.cameraOn = row.cameraOn;
        note(CameraOnRole);
    }
    if (entry.row.cameraTrackKey != row.cameraTrackKey) {
        entry.row.cameraTrackKey = row.cameraTrackKey;
        note(CameraTrackKeyRole);
    }
    if (entry.row.screenSharing != row.screenSharing) {
        entry.row.screenSharing = row.screenSharing;
        note(ScreenSharingRole);
    }
    if (entry.row.screenTrackKey != row.screenTrackKey) {
        entry.row.screenTrackKey = row.screenTrackKey;
        note(ScreenTrackKeyRole);
    }
    return changed;
}

void CallParticipantModel::applyParticipants(
    const QVector<CallParticipantRow> &desired)
{
    const int before = m_rows.size();

    // 1. Removals, back to front so indices stay valid.
    {
        QHash<QString, int> wanted;
        wanted.reserve(desired.size());
        for (int i = 0; i < desired.size(); ++i)
            wanted.insert(desired.at(i).identity, i);
        for (int i = m_rows.size() - 1; i >= 0; --i) {
            if (wanted.contains(m_rows.at(i).row.identity))
                continue;
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.remove(i);
            endRemoveRows();
        }
    }

    // 2. Inserts and moves, front to back: after step i the first i+1 rows
    //    match the first i+1 desired rows.
    for (int i = 0; i < desired.size(); ++i) {
        const CallParticipantRow &row = desired.at(i);
        if (row.identity.isEmpty())
            continue;
        int at = -1;
        for (int j = i; j < m_rows.size(); ++j) {
            if (m_rows.at(j).row.identity == row.identity) {
                at = j;
                break;
            }
        }
        if (at < 0) {
            Entry entry;
            entry.row = row;
            // When this client first saw the participant, for stable
            // ordering only; never present it as a join time.
            entry.joinedAtMs = QDateTime::currentMSecsSinceEpoch();
            beginInsertRows(QModelIndex(), i, i);
            m_rows.insert(i, entry);
            endInsertRows();
            continue;
        }
        if (at != i) {
            // A real move, so views can animate it and a delegate holding a
            // live VideoOutput survives. Moving up, the destination is `i`.
            beginMoveRows(QModelIndex(), at, at, QModelIndex(), i);
            m_rows.move(at, i);
            endMoveRows();
        }
        const QList<int> changed = mergeRow(m_rows[i], row);
        if (!changed.isEmpty())
            Q_EMIT dataChanged(index(i), index(i), changed);
    }

    if (m_rows.size() != before)
        Q_EMIT countChanged();
}

void CallParticipantModel::applySpeakers(
    const QHash<QString, bool> &activeBySid,
    const QHash<QString, qreal> &levelBySid)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        Entry &entry = m_rows[i];
        const QString &sid = entry.row.sid;
        // Absence from the round means not speaking: LiveKit sends the active
        // set. Treating absence as "unchanged" leaves rings stuck on.
        qreal level = sid.isEmpty() ? 0.0 : levelBySid.value(sid, 0.0);
        if (level < 0.0)
            level = 0.0;
        if (level > 1.0)
            level = 1.0;
        const bool active = !sid.isEmpty() && activeBySid.value(sid, false);
        // The union: an SFU reporting only `active` gives a binary ring, one
        // reporting only a level still lights up. A level is never invented
        // from the flag.
        const bool speaking = active || level > 0.0;

        QList<int> changed;
        if (entry.speaking != speaking) {
            entry.speaking = speaking;
            changed.append(SpeakingRole);
        }
        // Snap to 0 on silence, or the ring keeps its last width; otherwise
        // only changes beyond the deadband signal.
        const bool crossedToSilence = !speaking && entry.speakingLevel != 0.0;
        if (crossedToSilence
            || qAbs(entry.speakingLevel - level) >= kLevelDeadband) {
            entry.speakingLevel = speaking ? level : 0.0;
            changed.append(SpeakingLevelRole);
        }
        if (!changed.isEmpty())
            Q_EMIT dataChanged(index(i), index(i), changed);
    }
}

void CallParticipantModel::applyConnectionQuality(
    const QHash<QString, QString> &qualityBySid)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        Entry &entry = m_rows[i];
        if (entry.row.sid.isEmpty())
            continue;
        const auto it = qualityBySid.constFind(entry.row.sid);
        if (it == qualityBySid.cend())
            continue; // a delta: unmentioned keeps its last value
        if (entry.connectionQuality == *it)
            continue;
        entry.connectionQuality = *it;
        Q_EMIT dataChanged(index(i), index(i), { ConnectionQualityRole });
    }
}

void CallParticipantModel::setHandRaised(const QString &identity, bool raised)
{
    const int at = indexOf(identity);
    if (at < 0 || m_rows.at(at).handRaised == raised)
        return;
    m_rows[at].handRaised = raised;
    Q_EMIT dataChanged(index(at), index(at), { HandRaisedRole });
}

bool CallParticipantModel::setReaction(const QString &identity,
                                       const QString &emoji, qint64 nowMs,
                                       int ttlMs)
{
    if (emoji.isEmpty() || ttlMs <= 0)
        return false;
    const int at = indexOf(identity);
    if (at < 0)
        return false;
    // One still showing: drop the new one and keep the original deadline.
    // Refreshing would let a sender hold a permanent badge (element-call
    // refuses the same way).
    if (!m_rows.at(at).reactionEmoji.isEmpty()
        && m_rows.at(at).reactionExpiresAtMs > nowMs) {
        return false;
    }
    m_rows[at].reactionEmoji = emoji;
    m_rows[at].reactionExpiresAtMs = nowMs + ttlMs;
    Q_EMIT dataChanged(index(at), index(at), { ReactionEmojiRole });
    rearmReactionTimer(nowMs);
    return true;
}

void CallParticipantModel::expireReactions(qint64 nowMs)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        Entry &entry = m_rows[i];
        if (entry.reactionEmoji.isEmpty()
            || entry.reactionExpiresAtMs > nowMs) {
            continue;
        }
        entry.reactionEmoji.clear();
        entry.reactionExpiresAtMs = 0;
        Q_EMIT dataChanged(index(i), index(i), { ReactionEmojiRole });
    }
    rearmReactionTimer(nowMs);
}

void CallParticipantModel::rearmReactionTimer(qint64 nowMs)
{
    qint64 earliest = 0;
    for (const Entry &entry : std::as_const(m_rows)) {
        if (entry.reactionEmoji.isEmpty())
            continue;
        if (earliest == 0 || entry.reactionExpiresAtMs < earliest)
            earliest = entry.reactionExpiresAtMs;
    }
    if (earliest == 0) {
        m_reactionTimer.stop();
        return;
    }
    // A past deadline gives 0, firing on the next event-loop pass; never
    // negative, which QTimer treats as stop.
    m_reactionTimer.start(static_cast<int>(qMax<qint64>(0, earliest - nowMs)));
}

void CallParticipantModel::setVolumePercent(const QString &identity,
                                            int percent)
{
    const int at = indexOf(identity);
    if (at < 0)
        return;
    const int value = clampVolume(percent);
    if (m_rows.at(at).volumePercent == value)
        return;
    m_rows[at].volumePercent = value;
    Q_EMIT dataChanged(index(at), index(at), { VolumePercentRole });
}

void CallParticipantModel::clear()
{
    // Stop the timer explicitly, or it fires into an empty model after the
    // call.
    m_reactionTimer.stop();
    if (m_rows.isEmpty())
        return;
    beginRemoveRows(QModelIndex(), 0, m_rows.size() - 1);
    m_rows.clear();
    endRemoveRows();
    Q_EMIT countChanged();
}

QVariantList CallParticipantModel::toVariantList() const
{
    QVariantList out;
    out.reserve(m_rows.size());
    for (int i = 0; i < m_rows.size(); ++i)
        out.append(get(i));
    return out;
}
