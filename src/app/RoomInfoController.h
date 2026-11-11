#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

class RoomInfoController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY roomIdChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY membersChanged)
    Q_PROPERTY(QVariantList members READ members NOTIFY membersChanged)
    Q_PROPERTY(QVariantList joinedMembers READ joinedMembers NOTIFY membersChanged)
    Q_PROPERTY(int joinedCount READ joinedCount NOTIFY membersChanged)
    Q_PROPERTY(int invitedCount READ invitedCount NOTIFY membersChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY membersChanged)
    Q_PROPERTY(bool canInvite READ canInvite NOTIFY membersChanged)
    Q_PROPERTY(bool canEditName READ canEditName NOTIFY membersChanged)
    Q_PROPERTY(bool canEditTopic READ canEditTopic NOTIFY membersChanged)
    Q_PROPERTY(bool canEditAvatar READ canEditAvatar NOTIFY membersChanged)
    Q_PROPERTY(bool canKick READ canKick NOTIFY membersChanged)
    Q_PROPERTY(bool canBan READ canBan NOTIFY membersChanged)
    // Whether this account may trigger @room (the room's notifications.room
    // level, default 50).
    //
    // Hazard: false means "no", "roster not loaded yet" (clearSnapshot()) or
    // "backend sent no key", indistinguishably. Do not gate a control on it;
    // the composer reads the permission from MentionSuggestionModel's roster,
    // where absent means unknown.
    Q_PROPERTY(bool canNotifyRoom READ canNotifyRoom NOTIFY membersChanged)
    Q_PROPERTY(bool canUnban READ canUnban NOTIFY membersChanged)
    Q_PROPERTY(bool canSendMessages READ canSendMessages NOTIFY membersChanged)
    Q_PROPERTY(QString sendBlockedReason READ sendBlockedReason
                   NOTIFY membersChanged)
    // Whether this account may redact other people's events (the SDK's
    // RedactOther check). False until the snapshot says so: it offers a
    // moderation action, so unknown is "no".
    Q_PROPERTY(bool canRedactOthers READ canRedactOthers NOTIFY membersChanged)
    Q_PROPERTY(QString ownMembership READ ownMembership NOTIFY membersChanged)
    Q_PROPERTY(qlonglong ownPowerLevel READ ownPowerLevel NOTIFY membersChanged)
    Q_PROPERTY(bool canChangePowerLevels READ canChangePowerLevels
                   NOTIFY membersChanged)
    Q_PROPERTY(bool canPinMessages READ canPinMessages NOTIFY membersChanged)
    Q_PROPERTY(bool canChangeJoinRule READ canChangeJoinRule
                   NOTIFY membersChanged)
    Q_PROPERTY(bool canChangeAlias READ canChangeAlias NOTIFY membersChanged)
    Q_PROPERTY(bool canManageSpaceChildren READ canManageSpaceChildren
               NOTIFY membersChanged)
    Q_PROPERTY(qlonglong usersDefaultPowerLevel READ usersDefaultPowerLevel
                   NOTIFY membersChanged)
    Q_PROPERTY(QString joinRule READ joinRule NOTIFY membersChanged)
    Q_PROPERTY(QString canonicalAlias READ canonicalAlias NOTIFY membersChanged)
    Q_PROPERTY(QVariantMap canonicalSpaceParent READ canonicalSpaceParent
                   NOTIFY membersChanged)
    Q_PROPERTY(QString historyVisibility READ historyVisibility
                   NOTIFY membersChanged)
    Q_PROPERTY(QString guestAccess READ guestAccess NOTIFY membersChanged)
    Q_PROPERTY(QStringList altAliases READ altAliases NOTIFY membersChanged)
    Q_PROPERTY(QStringList restrictedAllowedRooms READ restrictedAllowedRooms
                   NOTIFY membersChanged)
    Q_PROPERTY(bool restrictedHasUnknownRules READ restrictedHasUnknownRules
                   NOTIFY membersChanged)
    Q_PROPERTY(bool canChangeHistoryVisibility READ canChangeHistoryVisibility
                   NOTIFY membersChanged)
    Q_PROPERTY(bool canChangeGuestAccess READ canChangeGuestAccess
                   NOTIFY membersChanged)
    Q_PROPERTY(int directoryPublished READ directoryPublished
                   NOTIFY directoryVisibilityChanged)
    Q_PROPERTY(QVariantList joinedSpaces READ joinedSpaces NOTIFY membersChanged)
    // The room's m.room.power_levels thresholds, one integer per key in
    // powerLevelKeys(). Empty until the roster arrives. An absent key is
    // unknown, never 0, since 0 is a common real level.
    Q_PROPERTY(QVariantMap powerLevels READ powerLevels NOTIFY membersChanged)
    Q_PROPERTY(QString roomVersion READ roomVersion NOTIFY membersChanged)
    Q_PROPERTY(bool canUpgradeRoom READ canUpgradeRoom NOTIFY membersChanged)
    Q_PROPERTY(bool powerMatrixPending READ powerMatrixPending
                   NOTIFY powerMatrixStateChanged)
    Q_PROPERTY(QString powerMatrixError READ powerMatrixError
                   NOTIFY powerMatrixStateChanged)
    Q_PROPERTY(bool powerLevelPending READ powerLevelPending
                   NOTIFY powerLevelStateChanged)
    Q_PROPERTY(bool moderationPending READ moderationPending
                   NOTIFY moderationStateChanged)
    Q_PROPERTY(bool editPending READ editPending NOTIFY editStateChanged)
    Q_PROPERTY(bool roomProfilePending READ roomProfilePending
                   NOTIFY roomProfileChanged)
    Q_PROPERTY(QString roomProfileError READ roomProfileError
                   NOTIFY roomProfileChanged)
    Q_PROPERTY(bool roomProfilesSupported READ roomProfilesSupported
                   NOTIFY roomProfileChanged)
    Q_PROPERTY(QString editError READ editError NOTIFY editStateChanged)
    Q_PROPERTY(bool leavePending READ leavePending NOTIFY leaveStateChanged)
    Q_PROPERTY(QString leaveError READ leaveError NOTIFY leaveStateChanged)

public:
    explicit RoomInfoController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);

    bool supported() const;
    bool loading() const { return m_membersOp != 0; }
    QVariantList members() const { return m_members; }
    QVariantList joinedMembers() const;
    int joinedCount() const { return m_joinedCount; }
    int invitedCount() const { return m_invitedCount; }
    bool truncated() const { return m_truncated; }
    bool canInvite() const { return m_canInvite; }
    bool canEditName() const { return m_canEditName; }
    bool canEditTopic() const { return m_canEditTopic; }
    bool canEditAvatar() const { return m_canEditAvatar; }
    bool canKick() const { return m_canKick; }
    bool canBan() const { return m_canBan; }
    bool canNotifyRoom() const { return m_canNotifyRoom; }
    bool canUnban() const { return m_canUnban; }
    bool canSendMessages() const;
    QString sendBlockedReason() const;
    bool canRedactOthers() const { return m_canRedactOthers; }
    QString ownMembership() const { return m_ownMembership; }
    qlonglong ownPowerLevel() const { return m_ownPowerLevel; }
    bool canChangePowerLevels() const { return m_canChangePowerLevels; }
    bool canPinMessages() const { return m_canPinMessages; }
    bool canChangeJoinRule() const { return m_canChangeJoinRule; }
    bool canChangeAlias() const { return m_canChangeAlias; }
    bool canManageSpaceChildren() const { return m_canManageSpaceChildren; }
    qlonglong usersDefaultPowerLevel() const { return m_usersDefaultPowerLevel; }
    QString joinRule() const { return m_joinRule; }
    QString canonicalAlias() const { return m_canonicalAlias; }
    QVariantMap canonicalSpaceParent() const { return m_canonicalSpaceParent; }
    QVariantMap powerLevels() const { return m_powerLevels; }
    QString roomVersion() const { return m_roomVersion; }
    bool canUpgradeRoom() const { return m_canUpgradeRoom; }
    bool canPublishCallMembership() const
    { return m_canPublishCallMembership; }
    bool powerMatrixPending() const { return m_powerMatrixOp != 0; }
    QString powerMatrixError() const { return m_powerMatrixError; }
    bool powerLevelPending() const { return m_powerLevelOp != 0; }
    bool moderationPending() const { return m_moderationOp != 0; }
    bool editPending() const { return m_editOp != 0; }
    QString editError() const { return m_editError; }
    bool leavePending() const { return m_leaveOp != 0; }
    QString leaveError() const { return m_leaveError; }

    Q_INVOKABLE void refreshMembers();
    Q_INVOKABLE void setRoomName(const QString &name);
    Q_INVOKABLE void setRoomTopic(const QString &topic);
    Q_INVOKABLE void setRoomAvatar(const QUrl &fileUrl);
    Q_INVOKABLE void removeRoomAvatar();

    Q_INVOKABLE void setMyRoomDisplayName(const QString &name);
    Q_INVOKABLE void setMyRoomAvatar(const QUrl &fileUrl);
    Q_INVOKABLE void clearMyRoomAvatar();
    bool roomProfilePending() const { return m_profileOps > 0; }
    QString roomProfileError() const { return m_profileError; }
    bool roomProfilesSupported() const;
    Q_INVOKABLE void leaveRoom();
    Q_INVOKABLE void leaveRoom(const QString &roomId);
    Q_INVOKABLE QVariantList filterMembers(const QString &needle) const;
    Q_INVOKABLE void requestMutualRooms(const QString &userId);
    Q_PROPERTY(QVariantList mutualRooms READ mutualRooms
               NOTIFY mutualRoomsChanged)
    QVariantList mutualRooms() const { return m_mutualRooms; }
    // One line on how many joined rooms the local store shows `userId` in, or
    // "" until the first answer; asking starts that local scan (no request).
    Q_INVOKABLE QString mutualRoomHint(const QString &userId);
    Q_PROPERTY(int mutualSummaryRevision READ mutualSummaryRevision
               NOTIFY mutualSummariesChanged)
    int mutualSummaryRevision() const { return m_mutualSummaryRevision; }
    // How long a hint counts as still shown, for rescans and eviction.
    void setMutualHintWindowMs(qint64 ms) { m_mutualHintWindowMs = ms; }

    Q_INVOKABLE QVariantMap memberFor(const QString &userId) const;
    Q_INVOKABLE QVariantMap memberFor(const QString &userId,
                                      const QString &roomId) const;
    Q_INVOKABLE QVariantList filterMembers(const QString &needle,
                                           const QString &membership,
                                           bool alphabetical) const;
    Q_INVOKABLE QVariantList memberRoleGroups(const QString &needle,
                                              const QString &membership,
                                              bool alphabetical) const;
    Q_INVOKABLE QVariantList memberRoleRows(const QString &needle,
                                            const QString &membership,
                                            bool alphabetical) const;
    Q_INVOKABLE bool canModerate(const QString &userId,
                                 const QString &op) const;
    Q_INVOKABLE void kickMember(const QString &userId, const QString &reason);
    Q_INVOKABLE void banMember(const QString &userId, const QString &reason);
    Q_INVOKABLE void unbanMember(const QString &userId,
                                 const QString &reason,
                                 bool inviteBack = false);

    Q_INVOKABLE bool canSetPowerLevel(const QString &userId,
                                      qlonglong level) const;
    Q_INVOKABLE qlonglong powerLevelFor(const QString &userId) const;
    Q_INVOKABLE QString roleLabelForLevel(qlonglong level) const;
    Q_INVOKABLE void setMemberPowerLevel(const QString &userId,
                                         qlonglong level);
    Q_INVOKABLE void setJoinRule(const QString &rule);
    Q_INVOKABLE void setRestrictedJoinRule(const QString &rule,
                                           const QStringList &allowedRoomIds);
    Q_INVOKABLE void setHistoryVisibility(const QString &visibility);
    Q_INVOKABLE void setGuestAccess(const QString &access);
    Q_INVOKABLE void setDirectoryPublished(bool published);
    Q_INVOKABLE void setAltAliases(const QStringList &aliases);
    Q_INVOKABLE void requestDirectoryVisibility();
    QString historyVisibility() const { return m_historyVisibility; }
    QString guestAccess() const { return m_guestAccess; }
    QStringList altAliases() const { return m_altAliases; }
    QStringList restrictedAllowedRooms() const { return m_restrictedAllowedRooms; }
    bool restrictedHasUnknownRules() const { return m_restrictedHasUnknownRules; }
    bool canChangeHistoryVisibility() const { return m_canChangeHistoryVisibility; }
    bool canChangeGuestAccess() const { return m_canChangeGuestAccess; }
    int directoryPublished() const { return m_directoryPublished; }
    QVariantList joinedSpaces() const;
    Q_INVOKABLE void setCanonicalAlias(const QString &alias);
    Q_INVOKABLE QString aliasProblem(const QString &alias) const;

    static QStringList powerLevelKeys();
    Q_INVOKABLE qlonglong powerLevelForKey(const QString &key) const;
    Q_INVOKABLE bool powerLevelKnown(const QString &key) const;
    Q_INVOKABLE bool canSetPowerLevelKey(const QString &key,
                                         qlonglong level) const;
    Q_INVOKABLE void setPowerLevelKey(const QString &key, qlonglong level);

    // Bounds on what this client writes as a threshold. Element's
    // "Restricted" is -1, so negatives must stay reachable.
    static constexpr qlonglong kMinSettableLevel = -100;
    static constexpr qlonglong kMaxSettableLevel = 100;

Q_SIGNALS:
    void mutualRoomsChanged();
    void mutualSummariesChanged();
    void roomIdChanged();
    void membersChanged();
    void directoryVisibilityChanged();
    void editStateChanged();
    void roomProfileChanged();
    void leaveStateChanged();
    void roomLeft(const QString &roomId);
    void roomLeaveFailed(const QString &roomId, const QString &message);
    void moderationStateChanged();
    void moderationActionFinished(const QString &roomId, const QString &userId,
                                  const QString &op, bool ok,
                                  const QString &message);
    void powerLevelStateChanged();
    void powerLevelActionFinished(const QString &roomId, const QString &userId,
                                  qlonglong level, bool ok,
                                  const QString &message);
    void powerMatrixStateChanged();
    void powerMatrixActionFinished(const QString &roomId, const QString &key,
                                   qlonglong level, bool ok,
                                   const QString &message);

private Q_SLOTS:
    void onRoomMembersReceived(quint64 opId, const QString &roomId,
                               const QVariantMap &snapshot);
    void onPowerLevelChangeFinished(quint64 opId, const QString &roomId,
                                    const QString &userId, qlonglong level,
                                    bool ok, const QString &category);
    void onPowerMatrixFinished(quint64 opId, const QString &roomId,
                               const QString &key, qlonglong level, bool ok,
                               const QString &category);
    void onRoomEditFinished(quint64 opId, const QString &roomId,
                            const QString &field, bool ok,
                            const QString &category);
    void onRoomLeaveFinished(quint64 opId, const QString &roomId, bool ok,
                             const QString &category);
    void onModerationFinished(quint64 opId, const QString &roomId,
                              const QString &userId, const QString &op,
                              bool ok, const QString &category);
    void onInviteUserFinished(quint64 opId, const QString &roomId,
                              const QString &userId, bool ok,
                              const QString &category);
    void onMembersChanged(const QString &roomId);
    void onLoggedOut();

private:
    QVariantList m_mutualRooms;
    QString m_mutualRoomsUser;
    quint64 m_mutualRoomsOp = 0;
    QHash<QString, QVariantMap> m_mutualSummaries;
    QHash<quint64, QString> m_mutualSummaryOps;
    int m_mutualSummaryRevision = 0;
    QTimer m_mutualSummaryRefresh;
    QHash<QString, qint64> m_mutualSummaryAsked;
    QElapsedTimer m_mutualSummaryClock;
    qint64 m_mutualHintWindowMs = 60000;
    QString m_mutualSummaryStarting;
    bool m_mutualSummaryStartAnswered = false;
    void startMutualSummaryScan(const QString &userId);
    void storeMutualSummary(const QString &userId, const QVariantList &rooms,
                            bool complete);
    void clearMutualSummaries();
    void clearSnapshot();
    void clearPermissions();
    void moderate(const QString &userId, const QString &reason,
                  const QString &op);
    QVariantList visibleMembers(const QString &needle,
                                const QString &membership,
                                bool alphabetical) const;

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    quint64 m_membersOp = 0;
    quint64 m_editOp = 0;
    int m_profileOps = 0;
    QString m_profileError;
    quint64 m_leaveOp = 0;
    QSet<quint64> m_adhocLeaveOps;
    QVariantList m_members;
    int m_joinedCount = 0;
    int m_invitedCount = 0;
    bool m_truncated = false;
    bool m_canInvite = false;
    bool m_canEditName = false;
    bool m_canEditTopic = false;
    bool m_canEditAvatar = false;
    bool m_canKick = false;
    bool m_canBan = false;
    bool m_canNotifyRoom = false;
    bool m_canUnban = false;
    bool m_canSendMessages = true;
    bool m_canRedactOthers = false;
    QString m_ownMembership;
    bool m_membersRefetchOwed = false;
    qlonglong m_ownPowerLevel = 0;
    bool m_canChangePowerLevels = false;
    bool m_canPinMessages = false;
    bool m_canChangeJoinRule = false;
    bool m_canChangeAlias = false;
    bool m_canManageSpaceChildren = false;
    qlonglong m_usersDefaultPowerLevel = 0;
    QString m_joinRule;
    QString m_canonicalAlias;
    QVariantMap m_canonicalSpaceParent;
    QString m_historyVisibility;
    QString m_guestAccess;
    QStringList m_altAliases;
    QStringList m_restrictedAllowedRooms;
    bool m_restrictedHasUnknownRules = false;
    bool m_canChangeHistoryVisibility = false;
    bool m_canChangeGuestAccess = false;
    int m_directoryPublished = -1;
    QString completeAlias(const QString &alias, QString *error) const;
    void dispatchEdit(quint64 opId);
    QVariantMap m_powerLevels;
    QString m_roomVersion;
    bool m_canUpgradeRoom = false;
    bool m_canPublishCallMembership = true;
    quint64 m_powerMatrixOp = 0;
    QString m_powerMatrixError;
    quint64 m_powerLevelOp = 0;
    QString m_powerLevelUserId;
    quint64 m_moderationOp = 0;
    QString m_inviteBackUserId;
    quint64 m_inviteBackOp = 0;
    QString m_editError;
    QString m_leaveError;
};
