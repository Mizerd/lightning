#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// v0.5.9: state for the Room Information panel — member snapshot,
// SDK-derived permissions, room profile editing (name/topic/avatar) and
// Leave room.
//
// Permission flags come exclusively from the Rust member snapshot
// (RoomMember::can_invite / can_send_state); nothing here guesses from role
// labels. Member data lives only in memory — it is never written into
// CacheStore. All async completions are matched by operation id and
// invalidated on room switch and sign-out.
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
    // @room: the room's OWN required level for a whole-room
    // notification (notifications.room, default 50), from the SDK. The
    // mention is only OFFERED where the account may actually trigger
    // one — sending it anyway would notify nobody and say nothing.
    // Whether this account may trigger a whole-room notification (@room).
    //
    // READ THE HAZARD BEFORE USING THIS. It is false in three different
    // situations that this one boolean cannot tell apart: the account really
    // is below the room's notifications.room level; the roster has not
    // arrived yet (clearSnapshot() sets it false on every re-point); and the
    // backend never sent the key at all, because QVariantMap::value() on a
    // missing key returns a default QVariant whose toBool() is false.
    //
    // Gating a control on it therefore hides that control most of the time.
    // The composer's @room suggestion did exactly that and was suppressed
    // everywhere, because the room-info panel is part of the default layout
    // so the "is this controller describing my room" test was usually true
    // and this answer was usually false. It now reads the permission from the
    // roster snapshot MentionSuggestionModel already receives for its own
    // room, where absent is UNKNOWN rather than "no".
    Q_PROPERTY(bool canNotifyRoom READ canNotifyRoom NOTIFY membersChanged)
    Q_PROPERTY(bool canUnban READ canUnban NOTIFY membersChanged)
    Q_PROPERTY(qlonglong ownPowerLevel READ ownPowerLevel NOTIFY membersChanged)
    // v0.7.x room administration. Every flag is the SDK's own power-level
    // check against the room's REAL required level for that state event —
    // a room may require any level for any of them, so nothing here assumes
    // "administrator only".
    Q_PROPERTY(bool canChangePowerLevels READ canChangePowerLevels
                   NOTIFY membersChanged)
    Q_PROPERTY(bool canPinMessages READ canPinMessages NOTIFY membersChanged)
    Q_PROPERTY(bool canChangeJoinRule READ canChangeJoinRule
                   NOTIFY membersChanged)
    Q_PROPERTY(bool canChangeAlias READ canChangeAlias NOTIFY membersChanged)
    // 2026-08-19: m.space.child management (add/remove/suggest) — the
    // SDK's can_send_state against the room's REAL required level.
    Q_PROPERTY(bool canManageSpaceChildren READ canManageSpaceChildren
               NOTIFY membersChanged)
    // The room's default user level. Needed to render "Member" honestly:
    // a room may set it to anything, and update_power_levels treats a
    // set-to-default as removal from the users map.
    Q_PROPERTY(qlonglong usersDefaultPowerLevel READ usersDefaultPowerLevel
                   NOTIFY membersChanged)
    // "invite" | "public" | "knock" | "private" | "restricted" |
    // "knock_restricted", or "" when not known. Only the first three are
    // settable (see setJoinRule).
    Q_PROPERTY(QString joinRule READ joinRule NOTIFY membersChanged)
    Q_PROPERTY(QString canonicalAlias READ canonicalAlias NOTIFY membersChanged)
    // 2026-08-26 Space settings — the room's REAL m.room.power_levels
    // thresholds, one integer per FIXED key (see powerLevelKeys()). Before
    // this, only the derived `can*` booleans existed: the UI could say
    // whether YOU may do a thing and never what the room REQUIRES for it, so
    // a permissions matrix had no source of truth at all.
    //
    // Empty until the roster arrives. An ABSENT key is UNKNOWN, never 0 —
    // a level of 0 is a real, common configuration, so a missing entry must
    // render as nothing rather than as "anyone may".
    Q_PROPERTY(QVariantMap powerLevels READ powerLevels NOTIFY membersChanged)
    // The room version as the SDK reports it ("6", "10", "org.matrix.msc…"),
    // or empty when the room state has not settled. Display only: Lightning
    // implements no upgrade, so this never gates a control.
    Q_PROPERTY(QString roomVersion READ roomVersion NOTIFY membersChanged)
    // Whether this account may send m.room.tombstone, i.e. whether an
    // upgrade would be permitted. Reported honestly next to the version;
    // it enables nothing, because no upgrade path exists here.
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
    Q_INVOKABLE void leaveRoom();
    // v0.6.5 (SPEC 1d): room-list context-menu adapter. Acts on an EXPLICIT
    // room id instead of the controller's own roomId property — the Room
    // Information panel may be showing a different room (or none), and this
    // must never mutate that binding. Tracked independently of the panel's
    // own leavePending/leaveError state (see m_adhocLeaveOps) so the two
    // paths can never corrupt each other.
    Q_INVOKABLE void leaveRoom(const QString &roomId);
    // Case-insensitive member filter over the loaded snapshot; returns the
    // same map shape as `members`.
    Q_INVOKABLE QVariantList filterMembers(const QString &needle) const;
    // ONE member of the loaded snapshot, by exact user id, in the same map
    // shape as `members`. EMPTY when this user is not in the room's roster —
    // which is the ordinary answer for a mention of somebody who has left,
    // and for every room whose roster has not been fetched.
    //
    // It exists because a caller may hold nothing but a user id. A mention
    // link in a message body is exactly that: clicking it used to open a
    // profile card with no picture and an MXID where the display name should
    // be, while clicking the same person's avatar one line above worked,
    // because THAT call site happened to pass the two fields. Resolving in
    // the surface rather than at each of the five call sites is what stops
    // the sixth getting it wrong too.
    //
    // A miss must NEVER be filled in with a guess: no fabricated name (the
    // localpart fallback every surface already shares is the honest answer)
    // and no avatar at all, because a wrong face is worse than none.
    /// Rooms the signed-in account and `userId` are BOTH joined to, for the
    /// profile card's overflow menu.
    ///
    /// Reads only what the store already holds — it issues NO request per
    /// room, which is the same rule that governs the room-list call glyph.
    /// The honest cost is that a room whose members were never synced is not
    /// listed; under-reporting beats a card that costs a /state per room
    /// every time it opens.
    Q_INVOKABLE void requestMutualRooms(const QString &userId);
    /// The last answer, for the user it was asked about. Empty until one
    /// arrives, and cleared when a different user is asked about, so a card
    /// can never show the previous person's rooms.
    Q_PROPERTY(QVariantList mutualRooms READ mutualRooms
               NOTIFY mutualRoomsChanged)
    QVariantList mutualRooms() const { return m_mutualRooms; }

    Q_INVOKABLE QVariantMap memberFor(const QString &userId) const;
    // The same lookup against an EXPLICIT room, because the caller that
    // needs this most is not the Room Information panel: `roomId` here
    // follows whatever surface last pointed this controller somewhere, and
    // that may be a Space's settings rather than the room the reader is
    // looking at. A SEPARATE overload rather than a defaulted argument, for
    // the reason filterMembers gives above — a defaulted parameter QML must
    // pass fails SILENTLY.
    Q_INVOKABLE QVariantMap memberFor(const QString &userId,
                                      const QString &roomId) const;
    // 2026-08-26 Space settings: the same filter plus a membership facet and
    // an A-to-Z option. A SEPARATE overload rather than defaulted arguments,
    // because a defaulted parameter QML must pass fails SILENTLY (the
    // 2026-08-21 setMentionStyle lesson) — two arities cannot be confused.
    //
    // `membership` is "" (all), "joined", "invited" or "banned"; anything
    // else matches nothing rather than quietly meaning "all".
    //
    // HONEST LIMIT, and it is a real one: the Rust snapshot sorts joined →
    // invited → banned, then power level DESCENDING, and only THEN caps at
    // MEMBER_SNAPSHOT_CAP. So an alphabetical re-sort here re-orders the
    // rows that survived a power-level truncation — on a space past the cap
    // the A-to-Z list is missing names from the MIDDLE of the alphabet, not
    // from its tail. `truncated` is what says so, and the dialog renders it.
    Q_INVOKABLE QVariantList filterMembers(const QString &needle,
                                           const QString &membership,
                                           bool alphabetical) const;
    // The same rows, bucketed by EXACT power level, highest first, each
    // group labelled with roleLabelForLevel. A room using 42 therefore gets
    // its own "Custom (42)" group — never folded into Moderator, which would
    // misdescribe the room's own configuration in the one place a person
    // consults to understand it.
    //
    // Shape: [{ "level": qlonglong, "label": QString, "members": [row…] }].
    Q_INVOKABLE QVariantList memberRoleGroups(const QString &needle,
                                              const QString &membership,
                                              bool alphabetical) const;
    // The same buckets FLATTENED into one list a ListView can virtualise:
    // a `{ kind: "header", label, level, count }` row followed by its
    // `{ kind: "member", … }` rows, in the same order.
    //
    // Why flattened rather than a Repeater over memberRoleGroups(): a nested
    // Repeater instantiates EVERY member row of EVERY group at once, and this
    // list is the side panel of a room that may have thousands of members —
    // where the dialog's version is a bounded page inside a modal. A ListView
    // needs one flat model, and building the flattening in QML would put the
    // grouping rule in two places.
    //
    // Each member row carries `roleLabel` and `powerLevel` so a delegate never
    // has to ask again per row: `roleLabelForLevel` is Q_INVOKABLE, so a call
    // from a binding creates no dependency and a recycled delegate would keep
    // the previous member's label.
    Q_INVOKABLE QVariantList memberRoleRows(const QString &needle,
                                            const QString &membership,
                                            bool alphabetical) const;
    // Moderation (kick / ban / unban) against the panel's room. `reason`
    // may be empty. One in-flight action at a time. The offer/dispatch
    // policy lives HERE, not in QML (architecture §5): canModerate(op)
    // with op "kick" | "ban" | "unban" requires the SDK-derived
    // permission flag (unban has its OWN flag — its required level is
    // max(ban, kick) per ruma's PowerLevelAction::Unban, asked of the
    // SDK, never derived from the ban flag), a loaded snapshot row
    // for the target (an unknown target FAILS CLOSED — note a row's
    // power level may legitimately be negative, e.g. Element's
    // "Restricted" -1, so absence of the row, never a sentinel value, is
    // the unknown state), a non-self target, membership that matches the
    // action (unban ONLY for banned members; kick/ban never for them),
    // and the target sitting STRICTLY below the viewer's own power level
    // (Element semantics; the server enforces regardless — this only
    // avoids offering an action that must fail).
    Q_INVOKABLE bool canModerate(const QString &userId,
                                 const QString &op) const;
    Q_INVOKABLE void kickMember(const QString &userId, const QString &reason);
    Q_INVOKABLE void banMember(const QString &userId, const QString &reason);
    // `inviteBack`: after a SUCCESSFUL unban, send a normal invite so the
    // user can rejoin without a second manual step (maintainer request,
    // 2026-08-14). The invite result surfaces through
    // moderationActionFinished with op "invite_back"; the server remains
    // the authority on both steps.
    Q_INVOKABLE void unbanMember(const QString &userId,
                                 const QString &reason,
                                 bool inviteBack = false);

    // ---- v0.7.x room administration ----
    //
    // Whether changing `userId`'s level to `level` should be OFFERED. The
    // Matrix rules the server will apply, checked here so the UI never
    // dispatches a state event that is known to be unauthorized:
    //   * the viewer must be permitted to send m.room.power_levels at all;
    //   * the new level may not exceed the viewer's own level (you cannot
    //     hand out authority you do not have);
    //   * the target's CURRENT level must be strictly below the viewer's,
    //     with the one exception that a user may always demote themselves —
    //     you may not overrule a peer at your own level;
    //   * the target must exist in the loaded snapshot. An unknown target
    //     FAILS CLOSED: levels may legitimately be negative, so absence of
    //     the row — never a sentinel value — is the unknown state.
    // The server remains the authority in every case.
    Q_INVOKABLE bool canSetPowerLevel(const QString &userId,
                                      qlonglong level) const;
    // The target's current level, or the room's users_default when the row
    // exists without one. Returns usersDefaultPowerLevel for an unknown
    // user — callers should gate on canSetPowerLevel, not on this.
    Q_INVOKABLE qlonglong powerLevelFor(const QString &userId) const;
    // Friendly label for a numeric level, WITHOUT flattening it: a level
    // that is not one of the conventional presets renders as its number.
    Q_INVOKABLE QString roleLabelForLevel(qlonglong level) const;
    Q_INVOKABLE void setMemberPowerLevel(const QString &userId,
                                         qlonglong level);
    // "invite" | "public" | "knock". Anything else is refused: the
    // restricted rules carry an allow-rule list that needs a space picker,
    // and sending one with an empty list would silently lock the room to
    // invite-only while claiming otherwise.
    Q_INVOKABLE void setJoinRule(const QString &rule);
    // An empty alias clears the canonical alias. A bare localpart is
    // completed with the account's own server so the user does not have to
    // type "#name:server" by hand.
    Q_INVOKABLE void setCanonicalAlias(const QString &alias);

    // ---- 2026-08-26: the m.room.power_levels matrix ----
    //
    // The keys this surface may read and write, in one place so the QML
    // cannot invent a key the Rust edge would refuse. Seven scalar
    // thresholds plus four state-event types; `m.call.member` is
    // deliberately absent (see rooms.rs — the identifier Lightning sends is
    // the MSC3401 unstable one and ruma aliases the stable name onto it, so
    // no honest row exists yet).
    // Not Q_INVOKABLE: QML never enumerates the keys — the dialog names its
    // own rows — and this is here so the contract test can prove that every
    // row it names is a key the backend actually accepts.
    static QStringList powerLevelKeys();
    // The room's current requirement for `key`, or -1 when it is not known
    // (roster not loaded, or an unrecognised key). Callers must treat the
    // UNKNOWN case as "render nothing": a real level of 0 is common, so no
    // in-band number can mean "unknown", which is why this returns a
    // sentinel AND `powerLevelKnown` exists to ask the question directly.
    Q_INVOKABLE qlonglong powerLevelForKey(const QString &key) const;
    Q_INVOKABLE bool powerLevelKnown(const QString &key) const;
    // Whether changing `key`'s threshold to `level` should be OFFERED.
    //
    //   * the viewer must be permitted to send m.room.power_levels at all;
    //   * the new level may not exceed the viewer's OWN level. This is the
    //     one-way door: a person who raises `m.room.power_levels` above
    //     themselves can never lower it again and no server will undo it,
    //     and the same clause stops `users_default` being raised above the
    //     person setting it;
    //   * the room's CURRENT value for the key must be known — an unknown
    //     threshold FAILS CLOSED, exactly as an unknown member does;
    //   * a no-op is not offered;
    //   * the level must be inside kMinSettableLevel..kMaxSettableLevel.
    //     DISPLAY is unbounded (a room using 4000 renders as 4000 and is
    //     never saved rounded); only what this client will WRITE is bounded,
    //     which also keeps the MSC4289 room-creator sentinel — a level no
    //     real room configures — from ever being sent as a threshold.
    // The server remains the authority in every case.
    Q_INVOKABLE bool canSetPowerLevelKey(const QString &key,
                                         qlonglong level) const;
    Q_INVOKABLE void setPowerLevelKey(const QString &key, qlonglong level);

    // The bounds on what this client will WRITE as a threshold. Element's
    // "Restricted" is -1, so negatives are legal and must stay reachable.
    static constexpr qlonglong kMinSettableLevel = -100;
    static constexpr qlonglong kMaxSettableLevel = 100;

Q_SIGNALS:
    void mutualRoomsChanged();
    void roomIdChanged();
    void membersChanged();
    void editStateChanged();
    void leaveStateChanged();
    // The active room was left; AppController closes the timeline. Fired for
    // both the panel's own Leave button and the room-list adapter above.
    void roomLeft(const QString &roomId);
    // v0.6.5: the room-list adapter's own honest failure surface — the
    // panel's leaveError/leaveStateChanged stay reserved for the panel's own
    // pending room and are not touched by this path.
    void roomLeaveFailed(const QString &roomId, const QString &message);
    void moderationStateChanged();
    // op is "kick", "ban" or "unban"; message is a sanitized failure
    // text, empty on success. A successful action triggers a
    // client-initiated roster refresh (the backend never emits a members
    // snapshot from sync).
    void moderationActionFinished(const QString &roomId, const QString &userId,
                                  const QString &op, bool ok,
                                  const QString &message);
    void powerLevelStateChanged();
    // A member's power-level write finished. `message` is empty on success.
    // The authoritative level arrives with the roster refresh that follows,
    // never from this signal — the UI must not treat `level` as applied.
    void powerLevelActionFinished(const QString &roomId, const QString &userId,
                                  qlonglong level, bool ok,
                                  const QString &message);
    void powerMatrixStateChanged();
    // A threshold write finished. `message` is empty on success. The
    // authoritative value arrives with the roster refresh that follows,
    // never from this signal — nothing is applied optimistically, so a
    // rejection snaps the control back to what the room actually holds.
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
    // Shared by filterMembers() and memberRoleGroups() so the two can never
    // disagree about what the visible roster is.
    QVariantList visibleMembers(const QString &needle,
                                const QString &membership,
                                bool alphabetical) const;

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    quint64 m_membersOp = 0;
    quint64 m_editOp = 0;
    quint64 m_leaveOp = 0;
    // v0.6.5: in-flight room-list adapter leave() calls, keyed by opId —
    // deliberately separate from m_leaveOp (the panel's own pending leave).
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
    QVariantMap m_powerLevels;
    QString m_roomVersion;
    bool m_canUpgradeRoom = false;
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
