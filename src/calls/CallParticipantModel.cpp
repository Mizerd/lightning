#include "calls/CallParticipantModel.h"

#include <QDateTime>
#include <QVariantMap>
#include <QtMath>

#include <utility>

namespace {
/// Below this, a level change is invisible on a ring a few pixels wide, and
/// emitting for it turns every SpeakersChanged round into a signal storm
/// across every row. The bool flipping always emits regardless of the band.
constexpr qreal kLevelDeadband = 0.02;

/// 0..200, not 0..100. Above 100 is real amplification, which is what was
/// asked for ("make it overclockable so i can do 200% volume like in
/// discord") and what the GStreamer `volume` element does with a linear
/// factor above 1.0. Clamping at 100 here silently discarded the entire
/// upper half of every slider — the store keeps 0..200 and the engine accepts
/// 0..200, so this was the one layer that would have thrown it away.
int clampVolume(int percent)
{
    return percent < 0 ? 0 : (percent > 200 ? 200 : percent);
}
} // namespace

CallParticipantModel::CallParticipantModel(QObject *parent)
    : QAbstractListModel(parent)
{
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
    // HAND RAISE IS LOCAL-ONLY AND INVISIBLE TO PEERS.
    //
    // `SfuCallController::setHandRaised` writes a member and emits
    // `mediaStateChanged`; nothing reaches the SFU, the MatrixRTC membership
    // or a to-device message, and `grep hand` finds nothing in
    // SfuMediaEngine or rust/src/calls.rs. So this role can only ever be
    // true for the LOCAL row. It is kept — honestly, and documented — rather
    // than deleted, because the local badge is genuine feedback that the
    // user's own toggle took effect; a remote hand would need a wire
    // representation checked against a real element-call client, which is a
    // protocol decision and not something to invent here.
    case HandRaisedRole:
        return entry.handRaised;
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
    // `sid` carries no role: it is the SFU's routing key for the level and
    // quality feeds, not something a tile draws. It still has to be kept
    // current, and a change in it alone must not produce a dataChanged.
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

    // 1. REMOVALS, back to front so the indices stay valid as we go.
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

    // 2. INSERTS AND MOVES, front to back, so after step i the first i+1 rows
    //    are exactly the first i+1 desired rows.
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
            // Local observation, NOT a Matrix fact: when THIS client first
            // saw the participant in the call. It exists so a view can order
            // stably; it must never be presented as "joined the call at".
            entry.joinedAtMs = QDateTime::currentMSecsSinceEpoch();
            beginInsertRows(QModelIndex(), i, i);
            m_rows.insert(i, entry);
            endInsertRows();
            continue;
        }
        if (at != i) {
            // A real move, so a view can ANIMATE it and the delegate holding
            // a live VideoOutput survives. beginMoveRows' destination is the
            // index the row lands at when moving up, which is exactly `i`.
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
        // Absence from the round IS the stop signal: LiveKit sends the set
        // of speakers, so a sid that is not in it is not speaking. Reading
        // "absent" as "unchanged" is how a ring gets stuck on.
        qreal level = sid.isEmpty() ? 0.0 : levelBySid.value(sid, 0.0);
        if (level < 0.0)
            level = 0.0;
        if (level > 1.0)
            level = 1.0;
        const bool active = !sid.isEmpty() && activeBySid.value(sid, false);
        // The UNION, deliberately. An SFU that reports only `active`
        // degrades to a binary ring (level stays 0.0 and the view draws its
        // minimum), and one that reports only a level still lights up. What
        // is refused is the third option: manufacturing a level from a
        // boolean, which would draw a confident amplitude nobody measured.
        const bool speaking = active || level > 0.0;

        QList<int> changed;
        if (entry.speaking != speaking) {
            entry.speaking = speaking;
            changed.append(SpeakingRole);
        }
        // A level that stopped speaking must fall to 0 exactly, or the ring
        // keeps its last width forever; inside the band, only real motion
        // signals.
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
            continue; // a delta: unmentioned keeps its last known value
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
