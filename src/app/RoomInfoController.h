#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// State for the Room Information panel: member snapshot, SDK-derived
// permissions, room profile editing (name/topic/avatar) and Leave room.
//
// Permission flags come only from the Rust member snapshot
// (RoomMember::can_invite / can_send_state), never from role labels. Member
// data lives only in memory, never in CacheStore. Async completions are
// matched by operation id and invalidated on room switch and sign-out.
class RoomInfoController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY roomIdChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY membersChanged)
    Q_PROPERTY(QVariantList members READ members NOTIFY membersChanged)
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
    Q_PROPERTY(qlonglong ownPowerLevel READ ownPowerLevel NOTIFY membersChanged)
    // Room administration flags: each is the SDK's power-level check against
    // the room's actual required level, never an "administrator only"
    // assumption.
    Q_PROPERTY(bool canChangePowerLevels READ canChangePowerLevels
                   NOTIFY membersChanged)
    Q_PROPERTY(bool canPinMessages READ canPinMessages NOTIFY membersChanged)
    Q_PROPERTY(bool canChangeJoinRule READ canChangeJoinRule
                   NOTIFY membersChanged)
    Q_PROPERTY(bool canChangeAlias READ canChangeAlias NOTIFY membersChanged)
    // m.space.child management (add/remove/suggest).
    Q_PROPERTY(bool canManageSpaceChildren READ canManageSpaceChildren
               NOTIFY membersChanged)
    // The room's default user level. update_power_levels treats setting a
    // user to it as removal from the users map.
    Q_PROPERTY(qlonglong usersDefaultPowerLevel READ usersDefaultPowerLevel
                   NOTIFY membersChanged)
    // "invite" | "public" | "knock" | "private" | "restricted" |
    // "knock_restricted", or "" when unknown. The first three are set via
    // setJoinRule, the restricted pair via setRestrictedJoinRule.
    Q_PROPERTY(QString joinRule READ joinRule NOTIFY membersChanged)
    Q_PROPERTY(QString canonicalAlias READ canonicalAlias NOTIFY membersChanged)
    // Room access, from the member snapshot; "" = unknown.
    // historyVisibility: invited | joined | shared | world_readable.
    // guestAccess: can_join | forbidden. restrictedAllowedRooms: room ids whose
    // members may join a restricted room; restrictedHasUnknownRules is true
    // when the list also has allow rules this client cannot render (they are
    // preserved on save). directoryPublished: -1 unknown, 0 private,
    // 1 published.
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
    // The account's joined Spaces as {roomId, name}, for the restricted-access
    // picker. Read from the client's room list, never fetched.
    Q_PROPERTY(QVariantList joinedSpaces READ joinedSpaces NOTIFY membersChanged)
    // The room's m.room.power_levels thresholds, one integer per key in
    // powerLevelKeys(). Empty until the roster arrives. An absent key is
    // unknown, never 0, since 0 is a common real level.
    Q_PROPERTY(QVariantMap powerLevels READ powerLevels NOTIFY membersChanged)
    // The room version as the SDK reports it ("6", "10", "org.matrix.msc…"),
    // or empty when the room state has not settled.
    Q_PROPERTY(QString roomVersion READ roomVersion NOTIFY membersChanged)
    // Whether this account may send m.room.tombstone; gates the Upgrade
    // control (flow in RoomUpgradeController).
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
    qlonglong ownPowerLevel() const { return m_ownPowerLevel; }
    bool canChangePowerLevels() const { return m_canChangePowerLevels; }
    bool canPinMessages() const { return m_canPinMessages; }
    bool canChangeJoinRule() const { return m_canChangeJoinRule; }
    bool canChangeAlias() const { return m_canChangeAlias; }
    bool canManageSpaceChildren() const { return m_canManageSpaceChildren; }
    qlonglong usersDefaultPowerLevel() const { return m_usersDefaultPowerLevel; }
    QString joinRule() const { return m_joinRule; }
    QString canonicalAlias() const { return m_canonicalAlias; }
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

    // ── This account's profile in this room ──────────────────────────────
    //
    // Per-room member profile. Empty clears the override. The two fields are
    // separate requests and can fail independently.
    Q_INVOKABLE void setMyRoomDisplayName(const QString &name);
    Q_INVOKABLE void setMyRoomAvatar(const QUrl &fileUrl);
    Q_INVOKABLE void clearMyRoomAvatar();
    bool roomProfilePending() const { return m_profileOps > 0; }
    QString roomProfileError() const { return m_profileError; }
    bool roomProfilesSupported() const;
    Q_INVOKABLE void leaveRoom();
    // Room-list context-menu adapter. Acts on an explicit room id rather than
    // the panel's roomId, which it must not mutate, and tracks its ops
    // separately from leavePending/leaveError (m_adhocLeaveOps).
    Q_INVOKABLE void leaveRoom(const QString &roomId);
    // Case-insensitive member filter over the loaded snapshot; returns the
    // same map shape as `members`.
    Q_INVOKABLE QVariantList filterMembers(const QString &needle) const;
    /// Rooms the signed-in account and `userId` are both joined to, for the
    /// profile card's overflow menu. Reads only the store and issues no
    /// request per room, so rooms whose members were never synced are missed.
    Q_INVOKABLE void requestMutualRooms(const QString &userId);
    /// The last answer, for the user it was asked about. Cleared when a
    /// different user is asked about, so a card never shows the previous
    /// person's rooms.
    Q_PROPERTY(QVariantList mutualRooms READ mutualRooms
               NOTIFY mutualRoomsChanged)
    QVariantList mutualRooms() const { return m_mutualRooms; }

    // One member of the loaded snapshot by exact user id, in the `members`
    // shape, for callers that hold only a user id (e.g. a mention link).
    // Empty when the user is not in the roster; never filled with a guess.
    Q_INVOKABLE QVariantMap memberFor(const QString &userId) const;
    // The same lookup against an explicit room: `roomId` follows whichever
    // surface last pointed this controller, which may be a Space's settings.
    // An overload rather than a default argument, since QML fails silently on
    // a defaulted parameter it must pass.
    Q_INVOKABLE QVariantMap memberFor(const QString &userId,
                                      const QString &roomId) const;
    // The same filter plus a membership facet and A-to-Z option. An overload
    // for the same reason as memberFor().
    //
    // `membership` is "" (all), "joined", "invited" or "banned"; anything
    // else matches nothing.
    //
    // Limit: the Rust snapshot sorts by membership then power level and only
    // then caps at MEMBER_SNAPSHOT_CAP, so past the cap an A-to-Z list misses
    // names from the middle of the alphabet. `truncated` reports it.
    Q_INVOKABLE QVariantList filterMembers(const QString &needle,
                                           const QString &membership,
                                           bool alphabetical) const;
    // The same rows bucketed by exact power level, highest first, labelled
    // with roleLabelForLevel. A custom level (e.g. 42) gets its own group
    // rather than being folded into Moderator.
    //
    // Shape: [{ "level": qlonglong, "label": QString, "members": [row…] }].
    Q_INVOKABLE QVariantList memberRoleGroups(const QString &needle,
                                              const QString &membership,
                                              bool alphabetical) const;
    // The same buckets flattened for a ListView: a
    // `{ kind: "header", label, level, count }` row followed by its
    // `{ kind: "member", … }` rows. A nested Repeater would instantiate every
    // row of a room with thousands of members.
    //
    // Member rows carry `roleLabel` and `powerLevel`, because calling
    // roleLabelForLevel from a binding creates no dependency and a recycled
    // delegate would keep a stale label.
    Q_INVOKABLE QVariantList memberRoleRows(const QString &needle,
                                            const QString &membership,
                                            bool alphabetical) const;
    // Moderation (kick / ban / unban) against the panel's room. `reason` may
    // be empty; one action in flight at a time. canModerate(op) requires:
    //   * the SDK permission flag (unban has its own: max(ban, kick) per
    //     ruma's PowerLevelAction::Unban);
    //   * a loaded snapshot row for the target (unknown fails closed; levels
    //     may be negative, so no sentinel means unknown);
    //   * a non-self target whose membership matches the action (unban only
    //     for banned members, kick/ban never for them);
    //   * the target strictly below the viewer's own level.
    // The server enforces regardless; this avoids offering doomed actions.
    Q_INVOKABLE bool canModerate(const QString &userId,
                                 const QString &op) const;
    Q_INVOKABLE void kickMember(const QString &userId, const QString &reason);
    Q_INVOKABLE void banMember(const QString &userId, const QString &reason);
    // `inviteBack`: after a successful unban, invite the user so they can
    // rejoin. The invite result arrives via moderationActionFinished with op
    // "invite_back".
    Q_INVOKABLE void unbanMember(const QString &userId,
                                 const QString &reason,
                                 bool inviteBack = false);

    // ---- room administration ----
    //
    // Whether changing `userId`'s level to `level` should be offered:
    //   * the viewer may send m.room.power_levels;
    //   * the new level does not exceed the viewer's own;
    //   * the target's current level is strictly below the viewer's, except
    //     that users may always demote themselves;
    //   * the target is in the loaded snapshot (unknown fails closed).
    // The server remains the authority.
    Q_INVOKABLE bool canSetPowerLevel(const QString &userId,
                                      qlonglong level) const;
    // The target's current level, or users_default when the row has none or
    // the user is unknown. Gate on canSetPowerLevel, not on this.
    Q_INVOKABLE qlonglong powerLevelFor(const QString &userId) const;
    // Label for a level; a non-preset level renders as its number.
    Q_INVOKABLE QString roleLabelForLevel(qlonglong level) const;
    Q_INVOKABLE void setMemberPowerLevel(const QString &userId,
                                         qlonglong level);
    // "invite" | "public" | "knock". Anything else is refused: restricted
    // rules need an allow list, and an empty one would silently make the
    // room invite-only.
    Q_INVOKABLE void setJoinRule(const QString &rule);
    // restricted / knock_restricted with the allowed room (Space) ids.
    // Refused with an empty list, for the same reason.
    Q_INVOKABLE void setRestrictedJoinRule(const QString &rule,
                                           const QStringList &allowedRoomIds);
    Q_INVOKABLE void setHistoryVisibility(const QString &visibility);
    Q_INVOKABLE void setGuestAccess(const QString &access);
    Q_INVOKABLE void setDirectoryPublished(bool published);
    Q_INVOKABLE void setAltAliases(const QStringList &aliases);
    // Asks the server whether this room is in its public directory; the
    // answer lands on directoryPublished.
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
    // An empty alias clears the canonical alias. A bare localpart is
    // completed with the account's own server.
    Q_INVOKABLE void setCanonicalAlias(const QString &alias);

    // ---- the m.room.power_levels matrix ----
    //
    // The keys this surface may read and write: seven scalar thresholds plus
    // four state-event types. `m.call.member` is absent on purpose (see
    // rooms.rs: Lightning sends the MSC3401 unstable identifier). Not
    // Q_INVOKABLE; it exists so the contract test can check every row the
    // dialog names against what the backend accepts.
    static QStringList powerLevelKeys();
    // The room's current requirement for `key`, or -1 when unknown (roster
    // not loaded, or unrecognised key). 0 is a common real level, so use
    // `powerLevelKnown` to ask directly and render nothing when unknown.
    Q_INVOKABLE qlonglong powerLevelForKey(const QString &key) const;
    Q_INVOKABLE bool powerLevelKnown(const QString &key) const;
    // Whether changing `key`'s threshold to `level` should be offered:
    //   * the viewer may send m.room.power_levels;
    //   * the new level does not exceed the viewer's own (raising
    //     `m.room.power_levels` above yourself cannot be undone);
    //   * the current value is known (unknown fails closed);
    //   * it is not a no-op;
    //   * the level is within kMinSettableLevel..kMaxSettableLevel. Display
    //     is unbounded; only writes are, which also keeps the MSC4289
    //     room-creator sentinel from being sent.
    // The server remains the authority.
    Q_INVOKABLE bool canSetPowerLevelKey(const QString &key,
                                         qlonglong level) const;
    Q_INVOKABLE void setPowerLevelKey(const QString &key, qlonglong level);

    // Bounds on what this client writes as a threshold. Element's
    // "Restricted" is -1, so negatives must stay reachable.
    static constexpr qlonglong kMinSettableLevel = -100;
    static constexpr qlonglong kMaxSettableLevel = 100;

Q_SIGNALS:
    void mutualRoomsChanged();
    void roomIdChanged();
    void membersChanged();
    // The on-demand directory-visibility tri-state.
    void directoryVisibilityChanged();
    void editStateChanged();
    void roomProfileChanged();
    void leaveStateChanged();
    // The room was left; AppController closes the timeline. Fired for both
    // the panel's Leave button and the room-list adapter.
    void roomLeft(const QString &roomId);
    // Failure surface for the room-list adapter; the panel's own
    // leaveError/leaveStateChanged are not touched by that path.
    void roomLeaveFailed(const QString &roomId, const QString &message);
    void moderationStateChanged();
    // op is "kick", "ban" or "unban"; message is a sanitized failure text,
    // empty on success. Success triggers a roster refresh (the backend never
    // emits a members snapshot from sync).
    void moderationActionFinished(const QString &roomId, const QString &userId,
                                  const QString &op, bool ok,
                                  const QString &message);
    void powerLevelStateChanged();
    // A member's power-level write finished; `message` is empty on success.
    // The applied level arrives with the following roster refresh, not here.
    void powerLevelActionFinished(const QString &roomId, const QString &userId,
                                  qlonglong level, bool ok,
                                  const QString &message);
    void powerMatrixStateChanged();
    // A threshold write finished; `message` is empty on success. Nothing is
    // applied optimistically: the value arrives with the roster refresh, so
    // a rejection snaps the control back.
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
    void clearSnapshot();
    void moderate(const QString &userId, const QString &reason,
                  const QString &op);
    // Shared by filterMembers() and memberRoleGroups() so they agree on the
    // visible roster.
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
    // In-flight room-list adapter leaves, separate from m_leaveOp.
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
    qlonglong m_ownPowerLevel = 0;
    bool m_canChangePowerLevels = false;
    bool m_canPinMessages = false;
    bool m_canChangeJoinRule = false;
    bool m_canChangeAlias = false;
    bool m_canManageSpaceChildren = false;
    qlonglong m_usersDefaultPowerLevel = 0;
    QString m_joinRule;
    QString m_canonicalAlias;
    QString m_historyVisibility;
    QString m_guestAccess;
    QStringList m_altAliases;
    QStringList m_restrictedAllowedRooms;
    bool m_restrictedHasUnknownRules = false;
    bool m_canChangeHistoryVisibility = false;
    bool m_canChangeGuestAccess = false;
    int m_directoryPublished = -1;
    // Alias validation shared by the canonical and alternative paths.
    QString completeAlias(const QString &alias, QString *error) const;
    // One dispatcher for every single-field write: claims the edit op or
    // reports the refusal.
    void dispatchEdit(quint64 opId);
    QVariantMap m_powerLevels;
    QString m_roomVersion;
    bool m_canUpgradeRoom = false;
    /// Whether this account may write the room's call membership; gates the
    /// Join button.
    bool m_canPublishCallMembership = true;
    quint64 m_powerMatrixOp = 0;
    QString m_powerMatrixError;
    quint64 m_powerLevelOp = 0;
    QString m_powerLevelUserId;
    quint64 m_moderationOp = 0;
    // Armed while an unban that should be followed by an invite is in
    // flight; the invite itself is tracked by its own op id.
    QString m_inviteBackUserId;
    quint64 m_inviteBackOp = 0;
    QString m_editError;
    QString m_leaveError;
};
