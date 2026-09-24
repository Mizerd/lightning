#pragma once

#include "matrix/CallSignal.h"
#include "matrix/RtcSession.h"
#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

// Backend interface for Matrix operations. UI and models depend only on this,
// never on a concrete backend: MockMatrixClient (--mock), CppHttpMatrixClient
// (experimental HTTP) and RustSdkMatrixClient (Matrix Rust SDK over FFI).
class MatrixClient : public QObject
{
    Q_OBJECT
public:
    enum ConnectionState {
        Disconnected,
        Connecting,
        Syncing,
        Error,
        Offline,
    };
    Q_ENUM(ConnectionState)

    explicit MatrixClient(QObject *parent = nullptr) : QObject(parent) {}
    ~MatrixClient() override = default;

    // Session lifecycle
    virtual void login(const QString &homeserver,
                       const QString &user,
                       const QString &password) = 0;
    virtual void logout() = 0;
    virtual bool restoreSession() = 0;
    // Account switching: end the local session only (stop sync, drop in-memory
    // state, emit loggedOut()) without a server logout and without deleting the
    // account's store, tokens or metadata. restoreSession() then activates the
    // account the settings select. Returns false when unsupported.
    virtual bool detachSession() { return false; }

    // --- OAuth 2.0 / OIDC browser sign-in -----------------------------------
    // Only RustSdkMatrixClient implements these; other backends report that
    // they cannot.
    //
    // Whether this backend can perform a browser sign-in at all, independent of
    // whether a given homeserver offers one (that is discovery).
    virtual bool supportsOAuthLogin() const { return false; }
    // Ask the homeserver which authentication methods it offers. Answers
    // asynchronously through authMethodsDiscovered().
    virtual void discoverAuthMethods(const QString &homeserver) { Q_UNUSED(homeserver); }
    // Start a browser sign-in. Emits oauthBrowserUrlReady(), then
    // loginSucceeded()/loginFailed(). The user id is unknown until this
    // completes, so no account store is opened before then.
    virtual void beginOAuthLogin(const QString &homeserver) { Q_UNUSED(homeserver); }
    // User cancelled or the wait timed out. Safe when nothing is in flight;
    // must leave the UI resolved, never in "Signing in".
    virtual void cancelOAuthLogin() {}

    // --- Legacy Matrix SSO (m.login.sso) --------------------------------
    // Distinct from OAuth: the homeserver redirects back with a single-use
    // `loginToken` exchanged through /login. Only the loopback listener is
    // shared.
    virtual bool supportsSsoLogin() const { return false; }
    // Identity providers the server advertises for SSO, answered on
    // ssoProvidersReceived(). No providers is normal and means one unnamed
    // flow.
    virtual void requestSsoProviders(const QString &homeserver) { Q_UNUSED(homeserver); }
    // Start an SSO sign-in; an empty `idpId` selects the default flow. Emits
    // ssoBrowserUrlReady(), then the normal session path. No store is opened
    // until the user id is known.
    virtual void beginSsoLogin(const QString &homeserver, const QString &idpId)
    {
        Q_UNUSED(homeserver);
        Q_UNUSED(idpId);
    }
    // Safe when nothing is in flight. Must leave the UI resolved.
    virtual void cancelSsoLogin() {}

    virtual bool isLoggedIn() const = 0;
    virtual QString currentUserId() const = 0;
    virtual QString homeserverUrl() const = 0;

    // Sync
    virtual void startSync() = 0;
    virtual void stopSync() = 0;
    virtual ConnectionState connectionState() const = 0;

    // True once at least one /sync response has been processed for the current
    // session. Backends that synthesise state immediately (Mock) return true.
    // Lets QML tell "still loading rooms" from "no rooms".
    virtual bool initialSyncDone() const { return true; }
    virtual QString syncMode() const { return QStringLiteral("classic_fallback"); }

    // Room + timeline queries
    virtual QList<RoomInfo> rooms() const = 0;
    // Single-room lookup. The default deep-copies rooms(); backends with a
    // native index should override. Returns a default RoomInfo for an unknown
    // room.
    virtual RoomInfo roomInfo(const QString &roomId) const
    {
        const auto all = rooms();
        for (const auto &room : all) {
            if (room.id == roomId)
                return room;
        }
        return {};
    }
    virtual QList<TimelineEvent> timeline(const QString &roomId) const = 0;

    // Member lookup: display name and avatar mxc. Fallback = MXID / empty.
    virtual QString displayNameFor(const QString &roomId, const QString &userId) const = 0;
    virtual QString avatarMxcFor(const QString &roomId, const QString &userId) const = 0;
    virtual QStringList typingUsersFor(const QString &roomId) const = 0;

    // Media URL helpers. These return authenticated HTTP URLs suitable for
    // <img src="..."> or QDesktopServices::openUrl.
    virtual QUrl mediaDownloadUrl(const QString &mxcUrl) const = 0;
    virtual QUrl mediaThumbnailUrl(const QString &mxcUrl,
                                   int width, int height,
                                   bool crop = false) const = 0;

    // Sending
    virtual void sendTextMessage(const QString &roomId, const QString &body) = 0;
    virtual void sendReply(const QString &roomId,
                           const QString &replyToEventId,
                           const QString &body) = 0;

    // Outgoing @-mentions. `mentionUserIds` are full MXIDs for m.mentions; the
    // body already carries the matrix.to links. The defaults drop the mentions
    // and forward to the plain overloads; the Rust backend attaches m.mentions.
    virtual void sendTextMessage(const QString &roomId, const QString &body,
                                 const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        sendTextMessage(roomId, body);
    }
    virtual void sendReply(const QString &roomId,
                           const QString &replyToEventId,
                           const QString &body,
                           const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        sendReply(roomId, replyToEventId, body);
    }

    // ---- Formatted sends.
    //
    // `bodySpec` selects how the body is interpreted:
    //   {"format": "markdown"|"plain"|"html", "html": "…",
    //    "msgtype": "text"|"emote"}
    // An empty map means markdown. "plain" sends the body verbatim (/shrug);
    // "html" carries a Matrix-subset formatted body generated from the same
    // document as the plain body; msgtype "emote" is /me. The defaults drop the
    // spec; the Rust backend refuses rather than silently degrading a spec it
    // cannot honour.
    virtual void sendTextMessage(const QString &roomId, const QString &body,
                                 const QStringList &mentionUserIds,
                                 const QVariantMap &bodySpec)
    {
        Q_UNUSED(bodySpec);
        sendTextMessage(roomId, body, mentionUserIds);
    }
    virtual void sendReply(const QString &roomId,
                           const QString &replyToEventId,
                           const QString &body,
                           const QStringList &mentionUserIds,
                           const QVariantMap &bodySpec)
    {
        Q_UNUSED(bodySpec);
        sendReply(roomId, replyToEventId, body, mentionUserIds);
    }

    // Reply into a thread rooted at `threadRootEventId`. The default falls back
    // to sendReply; backends may override to attach an `m.thread` relation.
    virtual void sendThreadReply(const QString &roomId,
                                 const QString &threadRootEventId,
                                 const QString &body)
    {
        sendReply(roomId, threadRootEventId, body);
    }
    // ---- SDK-backed thread timelines.
    //
    // A thread timeline is addressed by a composite id so it flows through the
    // same diff signals and model code as a room timeline. The id embeds a unit
    // separator, which never appears in Matrix ids, so it cannot collide with a
    // real room id.
    static QString threadTimelineId(const QString &roomId,
                                    const QString &rootEventId)
    {
        return roomId + QStringLiteral("\x1f" "thread" "\x1f") + rootEventId;
    }
    static bool isThreadTimelineId(const QString &timelineId)
    {
        return timelineId.contains(QStringLiteral("\x1f" "thread" "\x1f"));
    }
    static QString threadTimelineRoomId(const QString &timelineId)
    {
        const int sep = timelineId.indexOf(QChar(0x1f));
        return sep < 0 ? timelineId : timelineId.left(sep);
    }
    static QString threadTimelineRootId(const QString &timelineId)
    {
        const int sep = timelineId.lastIndexOf(QChar(0x1f));
        return sep < 0 ? QString{} : timelineId.mid(sep + 1);
    }

    // True when the backend can open live thread timelines; otherwise the
    // thread UI stays hidden.
    virtual bool supportsThreadTimelines() const { return false; }
    // Open (or replace) the single live thread timeline. The backend
    // responds with timelineReset(threadTimelineId(...)) on success or
    // threadTimelineFailed(...) on failure. A room switch closes it.
    virtual void openThread(const QString &roomId, const QString &rootEventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(rootEventId);
    }
    virtual void closeThread() {}
    // Manual decryption retry for a room's unable-to-decrypt events against key
    // material that has arrived since. No-op without a crypto machine. Never
    // resets any store.
    virtual void retryDecryption(const QString &roomId) { Q_UNUSED(roomId); }

    // ---- Thread list, follow state, threaded read.
    //
    // The Threads view lists a room's threads (server /threads pagination, kept
    // live by the backend). Follow state is MSC4306 thread subscription where
    // supported. markThreadRead sends a threaded receipt for the open thread
    // panel only, never a room-wide one.
    virtual bool supportsThreadList() const { return false; }
    virtual void openThreadList(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void closeThreadList() {}
    virtual void paginateThreadList(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void markThreadRead(const QString &roomId,
                                const QString &rootEventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(rootEventId);
    }
    virtual void queryThreadSubscription(const QString &roomId,
                                         const QString &rootEventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(rootEventId);
    }
    virtual void setThreadSubscribed(const QString &roomId,
                                     const QString &rootEventId,
                                     bool subscribed)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(rootEventId);
        Q_UNUSED(subscribed);
    }

    // Rich reply to a specific event WITHIN the thread. Backends without a
    // dedicated path fall back to a plain thread reply (the message still
    // lands in the correct thread).
    virtual void sendThreadReplyTo(const QString &roomId,
                                   const QString &threadRootEventId,
                                   const QString &inReplyToEventId,
                                   const QString &body)
    {
        Q_UNUSED(inReplyToEventId);
        sendThreadReply(roomId, threadRootEventId, body);
    }

    // Outgoing @-mentions inside a thread. An empty `inReplyToEventId` is a
    // plain thread reply, otherwise a rich reply within the thread.
    virtual void sendThreadReplyTo(const QString &roomId,
                                   const QString &threadRootEventId,
                                   const QString &inReplyToEventId,
                                   const QString &body,
                                   const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        sendThreadReplyTo(roomId, threadRootEventId, inReplyToEventId, body);
    }

    // Formatted thread sends; see the room-level bodySpec overloads.
    virtual void sendThreadReplyTo(const QString &roomId,
                                   const QString &threadRootEventId,
                                   const QString &inReplyToEventId,
                                   const QString &body,
                                   const QStringList &mentionUserIds,
                                   const QVariantMap &bodySpec)
    {
        Q_UNUSED(bodySpec);
        sendThreadReplyTo(roomId, threadRootEventId, inReplyToEventId, body,
                          mentionUserIds);
    }

    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody) = 0;

    // Outgoing @-mentions on an edit.
    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody,
                             const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        editMessage(roomId, targetEventId, newBody);
    }

    // Formatted edits; see the bodySpec send overloads.
    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody,
                             const QStringList &mentionUserIds,
                             const QVariantMap &bodySpec)
    {
        Q_UNUSED(bodySpec);
        editMessage(roomId, targetEventId, newBody, mentionUserIds);
    }
    virtual void redactEvent(const QString &roomId,
                             const QString &eventId,
                             const QString &reason = QString()) = 0;
    virtual void toggleReaction(const QString &roomId,
                                const QString &targetEventId,
                                const QString &key) = 0;

    // "Remove edits": redact the user's own m.replace events on a message so it
    // returns to its original text. Matrix has no unedit primitive; a backend
    // that cannot reach the relations reports it as unsupported.
    virtual bool supportsRemovingEdits() const { return false; }
    virtual void removeMessageEdits(const QString &roomId,
                                    const QString &eventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(eventId);
    }

    // MSC3381 polls (Rust backend only; elsewhere the actions stay hidden). An
    // empty threadRootId targets the room timeline, otherwise that thread.
    // Empty answerIds retracts the vote.
    virtual bool supportsPolls() const { return false; }
    virtual void sendPollResponse(const QString &roomId,
                                  const QString &threadRootId,
                                  const QString &pollStartEventId,
                                  const QStringList &answerIds)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(threadRootId);
        Q_UNUSED(pollStartEventId);
        Q_UNUSED(answerIds);
    }
    virtual void endPoll(const QString &roomId,
                         const QString &threadRootId,
                         const QString &pollStartEventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(threadRootId);
        Q_UNUSED(pollStartEventId);
    }
    virtual void createPoll(const QString &roomId,
                            const QString &threadRootId,
                            const QString &question,
                            const QStringList &answers,
                            bool undisclosed,
                            int maxSelections)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(threadRootId);
        Q_UNUSED(question);
        Q_UNUSED(answers);
        Q_UNUSED(undisclosed);
        Q_UNUSED(maxSelections);
    }
    virtual void sendTyping(const QString &roomId,
                            bool isTyping,
                            int timeoutMs = 20000) = 0;
    virtual void sendReadReceipt(const QString &roomId,
                                 const QString &eventId) = 0;
    /// Who learns that this account read a message: 0 public, 1 private
    /// (`m.read.private`), 2 nobody. Applies to later receipts only; a sent
    /// receipt cannot be retracted.
    virtual void setReadReceiptPrivacy(int mode) { Q_UNUSED(mode); }
    virtual void setRoomMarkedUnread(const QString &roomId, bool unread)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(unread);
    }
    // Favourites use the Matrix `m.favourite` room tag so they are shared with
    // other clients. Backends without tag support report false and the action
    // is not offered. Never optimistic: the flag comes from the backend's room
    // payload after the write lands.
    virtual bool supportsRoomFavourites() const { return false; }
    virtual void setRoomFavourite(const QString &roomId, bool favourite)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(favourite);
    }
    // Mark a room read without opening it. sendReadReceipt can only target an
    // event in the loaded timeline, so it cannot do this for a closed room.
    // Backends that cannot resolve a closed room's latest event leave this
    // inert.
    virtual bool supportsMarkRoomRead() const { return false; }
    virtual void markRoomRead(const QString &roomId) { Q_UNUSED(roomId); }
    // Server-synchronized per-room notification mode (account push rules owned
    // by the SDK). Modes match NotificationManager::RoomMode: 0 all messages, 1
    // mentions & keywords, 2 mute. Without support the mode stays device-local.
    // Mode 3 (account default) is the absence of an override and goes through
    // clearRoomNotificationMode(). requestRoomNotificationMode answers via
    // roomNotificationModeChanged, and is skipped while a write for the room is
    // queued or in flight (that write's own report is authoritative).
    virtual bool supportsServerNotificationModes() const { return false; }
    virtual void setRoomNotificationMode(const QString &roomId, int mode)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(mode);
    }
    // A thread's participants (facepile), answered via
    // threadParticipantsReceived. Cache-first on the Rust side. Backends
    // without thread support never answer.
    virtual void requestThreadParticipants(const QString &roomId,
                                           const QString &rootEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId);
    }
    // Matrix presence. Sliding Sync delivers no presence events, so
    // PresenceManager polls in bounded batches for the users on screen. Answers
    // via presenceReceived; backends without presence never answer.
    virtual bool supportsPresence() const { return false; }
    // Whether this backend keeps a room's read state (unreadCount,
    // highlightCount, hasUnreadMessages, markedUnread) current across devices.
    // The mock and HTTP backends never write hasUnreadMessages or markedUnread,
    // so "every unread signal is clear" is vacuously true there, and
    // notification_count alone is not a read signal. Ask this before acting on
    // "this room has been read"; see
    // ActivityModel::reconcileRoomsAgainstTheirReadState.
    virtual bool tracksRoomReadState() const { return false; }
    virtual void requestPresence(const QStringList &userIds, quint64 opId)
    {
        Q_UNUSED(userIds); Q_UNUSED(opId);
    }
    // Publish the user's own presence (0 online, 1 unavailable, 2 offline).
    // Fire-and-forget.
    virtual void publishPresence(int state) { Q_UNUSED(state); }
    // Publish with `m.presence` status_msg; empty clears it.
    virtual void publishPresence(int state, const QString &statusMsg)
    { Q_UNUSED(statusMsg); publishPresence(state); }
    // Profile banners (MSC4427 over MSC4133). False when extended profile
    // fields are unreadable; the banner is then absent.
    virtual bool supportsProfileBanners() const { return false; }
    virtual void fetchProfileBanner(const QString &userId, quint64 opId)
    {
        Q_UNUSED(userId); Q_UNUSED(opId);
    }
    // An EMPTY path clears the banner. Reports on profileBannerSet.
    virtual void setProfileBanner(const QString &localPath, quint64 opId)
    {
        Q_UNUSED(localPath); Q_UNUSED(opId);
    }
    // User-chosen display-name colour in their profile
    // (org.lightning.name_color, MSC4133). Same rules as banners.
    virtual bool supportsNameColors() const { return false; }
    virtual void fetchNameColor(const QString &userId, quint64 opId)
    {
        Q_UNUSED(userId); Q_UNUSED(opId);
    }
    // An EMPTY value clears it. Reports on nameColorSet.
    virtual void setNameColor(const QString &value, quint64 opId)
    {
        Q_UNUSED(value); Q_UNUSED(opId);
    }
    // Profile bios (MSC4440 over MSC4133), gated like banners.
    virtual bool supportsProfileBios() const { return false; }
    virtual void fetchProfileBio(const QString &userId, quint64 opId)
    {
        Q_UNUSED(userId); Q_UNUSED(opId);
    }
    // Empty or whitespace-only text clears the bio. Reports on profileBioSet.
    // Plain text, never HTML; see rust/src/bio.rs.
    virtual void setProfileBio(const QString &text, quint64 opId)
    {
        Q_UNUSED(text); Q_UNUSED(opId);
    }
    // Room / Space banners use Lightning's own state event (Matrix has none);
    // backends that cannot send arbitrary state do not offer them.
    virtual bool supportsRoomBanners() const { return false; }
    virtual void fetchRoomBanner(const QString &roomId, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(opId);
    }
    // An EMPTY path clears it. Reports on roomBannerSet.
    virtual void setRoomBanner(const QString &roomId, const QString &localPath,
                               quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(localPath); Q_UNUSED(opId);
    }

    // ---- Stickers and custom emoji: MSC2545 image packs ----------------
    //
    // Needs global account data and arbitrary room state; without them the
    // sticker button is not offered rather than showing an empty picker.
    virtual bool supportsStickerPacks() const { return false; }
    // Read every pack available to this account. With an empty `roomId`, the
    // active room's packs are excluded. Answers on stickerPacksReceived.
    virtual void fetchStickerPacks(const QString &roomId, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(opId);
    }
    // Send one m.sticker. An empty rootId targets the room timeline; otherwise
    // it is an m.thread reply built by the SDK. `url` must be a plain mxc://.
    virtual void sendSticker(const QString &roomId, const QString &rootId,
                             const QString &url, const QString &body,
                             const QString &mimetype, quint64 width,
                             quint64 height, quint64 size)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootId); Q_UNUSED(url); Q_UNUSED(body);
        Q_UNUSED(mimetype); Q_UNUSED(width); Q_UNUSED(height); Q_UNUSED(size);
    }
    // Add one image to a room's im.ponies.room_emotes pack. Room state, so
    // gated in Rust on the room's power level (category "forbidden"). Reports
    // on stickerPackAddFinished.
    virtual void addStickerToRoomPack(const QString &roomId,
                                      const QString &stateKey,
                                      const QString &shortcode,
                                      const QString &url, const QString &body,
                                      const QString &mimetype, quint64 width,
                                      quint64 height, quint64 size,
                                      quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(stateKey); Q_UNUSED(shortcode);
        Q_UNUSED(url); Q_UNUSED(body); Q_UNUSED(mimetype); Q_UNUSED(width);
        Q_UNUSED(height); Q_UNUSED(size); Q_UNUSED(opId);
    }
    // Enable a room pack everywhere via im.ponies.emote_rooms. Account data, so
    // no power level is involved. Reports on stickerPackRoomsSet.
    virtual void setStickerRoomPackEnabled(const QString &roomId,
                                           const QString &stateKey,
                                           bool enabled, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(stateKey); Q_UNUSED(enabled);
        Q_UNUSED(opId);
    }
    // Upload a local image into this account's own pack; the only way to create
    // a pack from nothing. Reports on stickerPackAddFinished.
    virtual void uploadStickerToUserPack(const QString &shortcode,
                                         const QString &body,
                                         const QString &localPath,
                                         quint64 opId)
    {
        Q_UNUSED(shortcode); Q_UNUSED(body); Q_UNUSED(localPath);
        Q_UNUSED(opId);
    }
    // Add one image to this account's own im.ponies.user_emotes pack.
    // Reports on stickerPackAddFinished.
    virtual void addStickerToUserPack(const QString &shortcode,
                                      const QString &url, const QString &body,
                                      const QString &mimetype, quint64 width,
                                      quint64 height, quint64 size,
                                      quint64 opId)
    {
        Q_UNUSED(shortcode); Q_UNUSED(url); Q_UNUSED(body);
        Q_UNUSED(mimetype); Q_UNUSED(width); Q_UNUSED(height);
        Q_UNUSED(size); Q_UNUSED(opId);
    }
    // ── Policy lists, Mjolnir-style ─────────────────────────────────────
    //
    // `m.policy.rule.*` room state. Reading needs a raw /state fetch because
    // the SDK store is empty for uncommon types; writing is power-level gated;
    // the subscription list is account data.
    //
    // Nothing here acts on a match: hiding people on someone else's list,
    // invisibly, is a different feature from showing that a list covers them.
    virtual bool supportsPolicyLists() const { return false; }
    virtual void fetchPolicyRules(const QString &roomId, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(opId);
    }
    /// `recommendation` empty removes the rule. `stateKey` empty derives
    /// `rule:<entity>`, which is right for a new rule. A removal must pass the
    /// rule's own key as read back: a rule written by another tool may use a
    /// different key, and removing by the derived key would report success
    /// while the rule stays.
    virtual void writePolicyRule(const QString &roomId, const QString &kind,
                                 const QString &entity,
                                 const QString &stateKey,
                                 const QString &recommendation,
                                 const QString &reason, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(kind); Q_UNUSED(entity);
        Q_UNUSED(stateKey); Q_UNUSED(recommendation); Q_UNUSED(reason);
        Q_UNUSED(opId);
    }
    virtual void setPolicySubscribed(const QString &roomId, bool subscribed,
                                     quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(subscribed); Q_UNUSED(opId);
    }
    virtual void fetchPolicySubscriptions(quint64 opId) { Q_UNUSED(opId); }
    virtual void checkPolicyEntity(const QString &kind, const QString &entity,
                                   quint64 opId)
    {
        Q_UNUSED(kind); Q_UNUSED(entity); Q_UNUSED(opId);
    }

    // ── MSC4108: sign another device in from this one ──────────────────
    //
    // Only the already-signed-in side: show a QR for a new device to scan, or
    // take the text of one it shows. Signing this device in from a code needs
    // the OAuth device-code grant, which rust/src/oauth.rs does not request.
    //
    // `qrLoginGenerate`/`qrLoginScan` return the flow's generation, or 0 when
    // refused. Everything after that arrives on qrLoginProgress.
    virtual bool supportsQrLogin() const { return false; }
    virtual quint64 qrLoginGenerate() { return 0; }
    virtual quint64 qrLoginScan(const QString &payload)
    {
        Q_UNUSED(payload);
        return 0;
    }
    virtual void qrLoginSubmitCheckCode(quint64 generation, int code)
    {
        Q_UNUSED(generation); Q_UNUSED(code);
    }
    virtual void qrLoginCancel() {}

    // MSC2545 pack management: remove one image, rename its shortcode, rename
    // the pack, or empty it.
    //
    // An empty `roomId` means this account's own pack (account data); otherwise
    // the room pack under `stateKey`, power-level gated like adding. `action`
    // is "remove_image", "rename_image", "set_name" or "delete_pack"; argA/argB
    // are its operands. One verb because all four are the same
    // read-modify-write.
    //
    // Reports on stickerPackEditFinished. Nothing is applied optimistically;
    // the caller re-reads the authoritative pack.
    virtual void editStickerPack(const QString &roomId,
                                 const QString &stateKey,
                                 const QString &action, const QString &argA,
                                 const QString &argB, quint64 opId)
    {
        Q_UNUSED(roomId); Q_UNUSED(stateKey); Q_UNUSED(action);
        Q_UNUSED(argA); Q_UNUSED(argB); Q_UNUSED(opId);
    }
    // "Follow account default": drop the room's user-defined push rules.
    // Success reports on roomNotificationModeCleared, not
    // roomNotificationModeChanged, since the outcome is the absence of a rule.
    virtual void clearRoomNotificationMode(const QString &roomId)
    {
        Q_UNUSED(roomId);
    }
    virtual void requestRoomNotificationMode(const QString &roomId)
    {
        Q_UNUSED(roomId);
    }
    virtual void acceptInvite(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void rejectInvite(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void sendImage(const QString &roomId, const QString &localPath) = 0;
    virtual void sendFile(const QString &roomId, const QString &localPath) = 0;

    // Pagination
    virtual void loadOlderMessages(const QString &roomId) = 0;
    virtual bool canPaginate(const QString &roomId) const = 0;
    virtual bool paginationReady(const QString &roomId) const
    { return canPaginate(roomId) || paginating(roomId) || paginationFailed(roomId); }
    virtual bool paginating(const QString &roomId) const = 0;

    // True when the last backward pagination for this room failed and can be
    // retried.
    virtual bool paginationFailed(const QString &roomId) const
    {
        Q_UNUSED(roomId);
        return false;
    }
    // True only when the current pagination failure is safe to retry without
    // user intervention (for example a temporary network failure or a live
    // timeline readiness race). Permission and invalid-room failures must
    // remain false so the controller exposes manual Retry immediately.
    virtual bool paginationFailureTransient(const QString &roomId) const
    {
        Q_UNUSED(roomId);
        return false;
    }

    /// Did the last completed back-pagination produce no rows because the
    /// timeline filter dropped every event? The controller otherwise waits 250
    /// ms for rows still in flight over the bridge; when the filter ate the
    /// page nothing is coming. False is always safe; it only keeps the wait.
    virtual bool lastPaginationFullyFiltered(const QString &roomId) const
    {
        Q_UNUSED(roomId);
        return false;
    }

    // Retry a failed outgoing message by send-queue transaction id. Rust
    // backend only (SDK local echoes).
    virtual void retryFailedSend(const QString &roomId,
                                 const QString &transactionId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(transactionId);
    }

    // Discard a queued outgoing message, including an in-flight upload, by
    // send-queue transaction id (SDK SendHandle::abort). An event already on
    // the server is not aborted and its row stays. Backends without a send
    // queue report false and the action is not offered.
    virtual bool supportsCancelSend() const { return false; }
    virtual void cancelSend(const QString &roomId,
                            const QString &transactionId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(transactionId);
    }

    // ---- Conversation creation, membership, room editing, media.
    //
    // Commands return an operation id (> 0) echoed on the matching *Finished
    // signal, or 0 when unsupported. The UI hides unsupported actions via
    // supportsRoomManagement / supportsAttachmentSend.
    virtual bool supportsRoomManagement() const { return false; }
    virtual bool supportsAttachmentSend() const { return false; }
    virtual bool supportsMediaBridge() const { return false; }

    virtual quint64 searchUsers(const QString &query, int limit)
    { Q_UNUSED(query); Q_UNUSED(limit); return 0; }
    // Exact profile lookup for one full user id; confirms a bare-localpart
    // candidate the directory may not list.
    virtual quint64 fetchUserProfile(const QString &userId)
    { Q_UNUSED(userId); return 0; }
    // Editing the account's own display name. Unsupported backends never offer
    // it, since a call with no answer would leave the editor spinning.
    virtual bool supportsOwnProfileEditing() const { return false; }
    // Set, or with an empty `name` clear, the own display name. The op id is
    // the caller's, so it can be recorded before any answer. Answers exactly
    // once on ownDisplayNameChanged, including for a synchronous refusal.
    virtual void setOwnDisplayName(const QString &name, quint64 opId)
    { Q_UNUSED(name); Q_UNUSED(opId); }
    // Set the own avatar from a local file, or clear it. Same contract as
    // setOwnDisplayName; answers once on ownAvatarChanged. Rust sniffs the MIME
    // from the bytes.
    virtual void setOwnAvatar(const QString &localPath, quint64 opId)
    { Q_UNUSED(localPath); Q_UNUSED(opId); }
    virtual void clearOwnAvatar(quint64 opId) { Q_UNUSED(opId); }
    // Rooms this account and `userId` are both joined to, from cached
    // membership only (no request), so unsynced rooms are omitted. 0 when
    // unsupported.
    virtual quint64 fetchMutualRooms(const QString &userId)
    { Q_UNUSED(userId); return 0; }
    // Client-side URL preview; Rust validates and fetches the target. 0 when
    // unsupported.
    virtual bool supportsUrlPreview() const { return false; }
    virtual quint64 fetchUrlPreview(const QString &url)
    { Q_UNUSED(url); return 0; }
    // Bounded, redirect-validated HTTPS GET for a GIF provider. `url` carries
    // the provider API key: treat it as secret and never log it. Result arrives
    // via gifResponse(); 0 when unsupported.
    virtual bool supportsGifProvider() const { return false; }
    virtual quint64 gifGet(const QString &url)
    { Q_UNUSED(url); return 0; }
    // Download and validate a provider GIF (gifDownloadFinished carries the
    // bytes). The URL must be a provider-CDN https .gif; Rust re-validates the
    // host and GIF magic bytes.
    virtual quint64 gifDownload(const QString &url)
    { Q_UNUSED(url); return 0; }
    // Existing joined DM rooms for a user, from authoritative m.direct.
    // Each entry: {roomId, name}. Synchronous store lookup.
    virtual QVariantList existingDirectRooms(const QString &userId) const
    { Q_UNUSED(userId); return {}; }
    virtual quint64 createDirectChat(const QString &userId)
    { Q_UNUSED(userId); return 0; }
    // options: name, topic, public(bool), encrypted(bool), alias,
    // invites(QStringList), spaceId.
    virtual quint64 createRoom(const QVariantMap &options)
    { Q_UNUSED(options); return 0; }
    virtual quint64 inviteUsers(const QString &roomId, const QStringList &userIds)
    { Q_UNUSED(roomId); Q_UNUSED(userIds); return 0; }
    virtual quint64 requestRoomMembers(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual quint64 setRoomName(const QString &roomId, const QString &name)
    { Q_UNUSED(roomId); Q_UNUSED(name); return 0; }
    virtual quint64 setRoomTopic(const QString &roomId, const QString &topic)
    { Q_UNUSED(roomId); Q_UNUSED(topic); return 0; }
    virtual quint64 setRoomAvatar(const QString &roomId, const QString &localPath)
    { Q_UNUSED(roomId); Q_UNUSED(localPath); return 0; }
    virtual quint64 removeRoomAvatar(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    virtual quint64 leaveRoom(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    // Moderation: kick, ban or unban one user (SDK-owned power-level
    // semantics; the server enforces, the client only surfaces the
    // result). `reason` may be empty. 0 = unsupported on this backend.
    virtual quint64 kickUser(const QString &roomId, const QString &userId,
                             const QString &reason)
    { Q_UNUSED(roomId); Q_UNUSED(userId); Q_UNUSED(reason); return 0; }
    virtual quint64 banUser(const QString &roomId, const QString &userId,
                            const QString &reason)
    { Q_UNUSED(roomId); Q_UNUSED(userId); Q_UNUSED(reason); return 0; }
    virtual quint64 unbanUser(const QString &roomId, const QString &userId,
                              const QString &reason)
    { Q_UNUSED(roomId); Q_UNUSED(userId); Q_UNUSED(reason); return 0; }
    // Set one member's power level; the SDK preserves every other level.
    // Answers on powerLevelChangeFinished. The server enforces permission.
    virtual quint64 setMemberPowerLevel(const QString &roomId,
                                        const QString &userId,
                                        qlonglong level)
    { Q_UNUSED(roomId); Q_UNUSED(userId); Q_UNUSED(level); return 0; }
    // Set one threshold in m.room.power_levels. `key` is one of
    // RoomInfoController's powerLevelKeys(); Rust refuses anything else, so
    // this is not a generic event-level writer. Answers on
    // roomPowerMatrixFinished.
    virtual quint64 setRoomPowerLevelKey(const QString &roomId,
                                         const QString &key, qlonglong level)
    { Q_UNUSED(roomId); Q_UNUSED(key); Q_UNUSED(level); return 0; }

    // "invite" | "public" | "knock". Answers on roomEditFinished with field
    // "join_rule".
    virtual quint64 setRoomJoinRule(const QString &roomId, const QString &rule)
    { Q_UNUSED(roomId); Q_UNUSED(rule); return 0; }
    // Room access. "restricted" | "knock_restricted" with allowed room (Space)
    // ids; the default forwards to the plain setter, which refuses them.
    // historyVisibility: invited | joined | shared | world_readable (field
    // "history_visibility"); guestAccess: can_join | forbidden
    // ("guest_access"); directory visibility is read via
    // roomDirectoryVisibilityReceived and written as published/private
    // ("directory_visibility"); altAliases replaces the whole list
    // ("alt_aliases"). 0 = unsupported.
    virtual quint64 setRoomJoinRule(const QString &roomId, const QString &rule,
                                    const QStringList &allowedRoomIds)
    { Q_UNUSED(allowedRoomIds); return setRoomJoinRule(roomId, rule); }
    virtual quint64 setRoomHistoryVisibility(const QString &roomId,
                                             const QString &visibility)
    { Q_UNUSED(roomId); Q_UNUSED(visibility); return 0; }
    virtual quint64 setRoomGuestAccess(const QString &roomId,
                                       const QString &access)
    { Q_UNUSED(roomId); Q_UNUSED(access); return 0; }
    virtual void requestRoomDirectoryVisibility(const QString &roomId)
    { Q_UNUSED(roomId); }
    virtual quint64 setRoomDirectoryVisibility(const QString &roomId,
                                               bool published)
    { Q_UNUSED(roomId); Q_UNUSED(published); return 0; }
    virtual quint64 setRoomAltAliases(const QString &roomId,
                                      const QStringList &aliases)
    { Q_UNUSED(roomId); Q_UNUSED(aliases); return 0; }
    // Room upgrade: supported versions (roomVersionsReceived) and /upgrade,
    // answering on roomUpgradeFinished with the replacement room id.
    // 0 = unsupported.
    virtual void requestRoomVersions() {}
    virtual quint64 upgradeRoom(const QString &roomId, const QString &newVersion)
    { Q_UNUSED(roomId); Q_UNUSED(newVersion); return 0; }
    // Device and backup management. renameDevice answers on deviceRenamed;
    // backupAction ("enable" | "create_backup" | "reset_key" |
    // "disable_and_delete" | "disable_recovery") on backupActionFinished, whose
    // recoveryKey is set once for enable/reset_key and must be shown and then
    // dropped, never logged or persisted. requestBackupProgress answers on
    // backupProgress. 0 = unsupported.
    virtual quint64 renameDevice(const QString &deviceId, const QString &name)
    { Q_UNUSED(deviceId); Q_UNUSED(name); return 0; }
    virtual quint64 backupAction(const QString &action)
    { Q_UNUSED(action); return 0; }
    virtual void requestBackupProgress() {}
    // Edit history and event source, answered on editHistoryReceived /
    // eventSourceReceived. Display data held only while a dialog is open; never
    // cached.
    virtual void requestEditHistory(const QString &roomId, const QString &eventId)
    { Q_UNUSED(roomId); Q_UNUSED(eventId); }
    virtual void requestEventSource(const QString &roomId, const QString &eventId)
    { Q_UNUSED(roomId); Q_UNUSED(eventId); }
    // Jump to date (MSC3030). Searches forward from the stamp, so a day lands
    // on its first message. Answers on eventAtTimestampReceived. 0 means this
    // backend is unsupported; an unsupported server answers ok=false with
    // category "not_found".
    virtual quint64 eventAtTimestamp(const QString &roomId, qint64 timestampMs)
    { Q_UNUSED(roomId); Q_UNUSED(timestampMs); return 0; }

    // ── Local message search ─────────────────────────────────────────────
    //
    // Server search cannot read encrypted rooms; this searches the index the
    // client builds from plaintext it already holds. 0 means this backend has
    // no local index, which must stay distinguishable from "no results".
    virtual quint64 localSearch(const QString &query, const QString &roomId,
                                int limit, int offset)
    { Q_UNUSED(query); Q_UNUSED(roomId); Q_UNUSED(limit); Q_UNUSED(offset);
      return 0; }
    /// What the index holds, so a surface can say what search covers.
    virtual quint64 searchIndexStats() { return 0; }
    /// Sweep cached events into the index. Bounded per call.
    virtual quint64 sweepSearchIndex() { return 0; }
    /// Page one room backwards and index what arrives ("index this room's
    /// history").
    virtual quint64 deepenSearchIndex(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    /// A redacted message must not stay findable by its own text.
    virtual void forgetIndexedEvent(const QString &eventId) { Q_UNUSED(eventId); }
    virtual void forgetIndexedRoom(const QString &roomId) { Q_UNUSED(roomId); }
    virtual void clearSearchIndex() {}
    /// Whether this backend can search locally at all, so a surface can be
    /// absent rather than dead.
    virtual bool supportsLocalSearch() const { return false; }

    // ── Widgets ──────────────────────────────────────────────────────────
    //
    // Widgets are listed and opened in the user's browser, not embedded; see
    // docs/widgets.md for why. `theme` and `language` are template variables
    // the widget URL may carry.
    virtual quint64 roomWidgets(const QString &roomId, const QString &theme,
                                const QString &language)
    { Q_UNUSED(roomId); Q_UNUSED(theme); Q_UNUSED(language); return 0; }
    virtual bool supportsWidgets() const { return false; }
    /// Add or remove a widget by writing `contentJson` as the room's
    /// `im.vector.modular.widgets` state under `widgetId`; an empty object
    /// removes it. Power-level gated in Rust, which also refuses a non-https
    /// `url`. Answers on roomWidgetWritten; 0 when unsupported.
    virtual quint64 writeRoomWidget(const QString &roomId,
                                    const QString &widgetId,
                                    const QString &contentJson)
    { Q_UNUSED(roomId); Q_UNUSED(widgetId); Q_UNUSED(contentJson); return 0; }

    // ── Bridges (MSC2346) ────────────────────────────────────────────────
    //
    // Which network a room is bridged to, as the bridge says. The heuristic in
    // matrix::bridge::networkIdForRoom only works for DMs and portal aliases.
    //
    // `allowNetwork` permits the /state fallback: sliding sync does not carry
    // this state type, so the store-only read finds nothing. It is a budget
    // control: the room list must never trigger it; a surface the user opened
    // may.
    //
    // Answers on roomBridgesReceived; 0 when unsupported.
    virtual quint64 roomBridges(const QString &roomId, bool allowNetwork)
    { Q_UNUSED(roomId); Q_UNUSED(allowNetwork); return 0; }
    virtual bool supportsRoomBridges() const { return false; }

    // ── Room media history ───────────────────────────────────────────────
    //
    // Walked independently of the live timeline, so Room Information can browse
    // attachments to the start of history without moving the reader. One page
    // per call from where the walk left off; `restart` begins again at the live
    // edge. Answers on mediaHistoryPage / mediaHistoryFailed.
    //
    // Deliberately unfiltered: the server's EventsWithUrl filter would hide
    // encrypted events and messages that merely contain links. See
    // rust/src/mediahistory.rs.
    virtual quint64 requestMediaHistoryPage(const QString &roomId, int limit,
                                            bool restart)
    { Q_UNUSED(roomId); Q_UNUSED(limit); Q_UNUSED(restart); return 0; }
    virtual bool supportsMediaHistory() const { return false; }

    // ── Per-room profile ─────────────────────────────────────────────────
    //
    // Display name and avatar this account shows in one room, overriding the
    // global profile; empty clears. Both live in the user's m.room.member
    // event, and the avatar path edits the raw event so nothing else is lost
    // (see rust/src/profile.rs).
    virtual quint64 setRoomDisplayName(const QString &roomId,
                                       const QString &name)
    { Q_UNUSED(roomId); Q_UNUSED(name); return 0; }
    virtual quint64 setRoomMemberAvatar(const QString &roomId,
                                        const QString &mxc)
    { Q_UNUSED(roomId); Q_UNUSED(mxc); return 0; }
    virtual bool supportsRoomProfiles() const { return false; }
    // Scheduled send, server side (MSC4140 delayed events). probeDelayedEvents
    // answers on delayedEventsSupportReceived; scheduleMessage on
    // scheduledSendFinished (with the delayId; category "encrypted_unsupported"
    // for encrypted rooms, see rooms.rs); updateScheduled ("cancel" | "send" |
    // "restart") on scheduledUpdateFinished. 0 = unsupported.
    virtual void probeDelayedEvents() {}
    virtual quint64 scheduleMessage(const QString &roomId, const QString &body,
                                    const QVariantMap &bodySpec,
                                    const QStringList &mentionUserIds,
                                    qint64 delayMs)
    { Q_UNUSED(roomId); Q_UNUSED(body); Q_UNUSED(bodySpec);
      Q_UNUSED(mentionUserIds); Q_UNUSED(delayMs); return 0; }
    virtual quint64 updateScheduledMessage(const QString &delayId,
                                           const QString &action)
    { Q_UNUSED(delayId); Q_UNUSED(action); return 0; }
    // Room-level send for any joined room (the timeline sends need the live
    // timeline open), answering on roomSendFinished. Empty reply/thread ids
    // send a plain message. 0 = unsupported; the scheduler then uses the
    // timeline sends.
    virtual quint64 sendRoomMessage(const QString &roomId, const QString &body,
                                    const QVariantMap &bodySpec,
                                    const QStringList &mentionUserIds,
                                    const QString &replyToEventId,
                                    const QString &threadRootEventId)
    { Q_UNUSED(roomId); Q_UNUSED(body); Q_UNUSED(bodySpec);
      Q_UNUSED(mentionUserIds); Q_UNUSED(replyToEventId);
      Q_UNUSED(threadRootEventId); return 0; }
    // Activity Center seed: the server's highlight notifications
    // (GET /notifications?only=highlight), bounded to `limit`. Answers on
    // activitySeedReceived with [{eventId, roomId, senderId, timestampMs, read,
    // encrypted, preview, threadRootId}]; never ciphertext.
    virtual void requestActivitySeed(int limit) { Q_UNUSED(limit); }
    // An empty alias clears the canonical alias. Answers on
    // roomEditFinished with field "canonical_alias".
    virtual quint64 setRoomCanonicalAlias(const QString &roomId,
                                          const QString &alias)
    { Q_UNUSED(roomId); Q_UNUSED(alias); return 0; }
    // Pinned messages (m.room.pinned_events). Without support the UI offers no
    // pin actions or pinned surface.
    virtual bool supportsPinnedMessages() const { return false; }
    // Read the pinned list and resolve each id into a row. `allowRemote`
    // permits the /state fallback when the room has no pinned-events state at
    // all. Answers on pinnedReceived.
    virtual quint64 requestPinnedMessages(const QString &roomId,
                                          bool allowRemote)
    { Q_UNUSED(roomId); Q_UNUSED(allowRemote); return 0; }
    virtual quint64 setEventPinned(const QString &roomId,
                                   const QString &eventId, bool pin)
    { Q_UNUSED(roomId); Q_UNUSED(eventId); Q_UNUSED(pin); return 0; }
    // Room discovery / join / knock. Without support there is no Discover
    // surface.
    virtual bool supportsRoomDiscovery() const { return false; }
    // Resolve user input (#alias, !roomid, matrix: URI, matrix.to permalink)
    // into a join target and preview it where allowed. Answers on
    // roomTargetResolved. A refused preview still returns the target so Join
    // can be offered.
    virtual quint64 resolveRoomTarget(const QString &input)
    { Q_UNUSED(input); return 0; }
    // One page of the public room directory. `server` optionally targets
    // another homeserver's directory ("" = own); `since` is the pagination
    // token from the previous page. Answers on publicRoomsReceived.
    virtual quint64 searchPublicRooms(const QString &query,
                                      const QString &server,
                                      const QString &since, int limit)
    {
        Q_UNUSED(query); Q_UNUSED(server); Q_UNUSED(since); Q_UNUSED(limit);
        return 0;
    }
    // Join by room id or alias, optionally routed via the given servers.
    // Answers on roomJoinFinished.
    virtual quint64 joinRoomByIdOrAlias(const QString &target,
                                        const QStringList &via)
    { Q_UNUSED(target); Q_UNUSED(via); return 0; }
    // Knock (request access) with an optional reason. Only offered when
    // the join rule allows knocking; answers on roomKnockFinished.
    virtual quint64 knockRoom(const QString &target, const QStringList &via,
                              const QString &reason)
    { Q_UNUSED(target); Q_UNUSED(via); Q_UNUSED(reason); return 0; }
    // Withdraw a pending knock (leaves the Knocked room). Answers on
    // knockCancelFinished.
    virtual quint64 cancelKnock(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    // List a Space's children — joined AND unjoined — through the server's
    // /hierarchy. Bounded; answers on spaceChildrenReceived.
    virtual quint64 requestSpaceChildren(const QString &spaceId)
    { Q_UNUSED(spaceId); return 0; }
    // Personal moderation. Ignore state is m.ignored_user_list account data
    // (SDK read-modify-write); reporting is the /v3 event report.
    virtual bool supportsIgnoredUsers() const { return false; }
    virtual quint64 setUserIgnored(const QString &userId, bool ignored)
    { Q_UNUSED(userId); Q_UNUSED(ignored); return 0; }
    virtual quint64 requestIgnoredUsers() { return 0; }
    virtual bool supportsEventReporting() const { return false; }
    virtual quint64 reportMessage(const QString &roomId,
                                  const QString &eventId,
                                  const QString &reason)
    { Q_UNUSED(roomId); Q_UNUSED(eventId); Q_UNUSED(reason); return 0; }
    // Voice-call signaling (MSC2746 m.call.* v1 plus
    // m.rtc.notification/decline). SDP strings are opaque inputs from the media
    // backend, never logged or echoed. Non-capable backends return 0.
    virtual bool supportsCallSignaling() const { return false; }
    virtual quint64 callInvite(const QString &roomId, const QString &callId,
                               const QString &partyId,
                               const QString &offerType,
                               const QString &offerSdp, quint64 lifetimeMs,
                               const QString &invitee)
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(offerType); Q_UNUSED(offerSdp); Q_UNUSED(lifetimeMs);
        Q_UNUSED(invitee); return 0;
    }
    virtual quint64 callAnswer(const QString &roomId, const QString &callId,
                               const QString &partyId,
                               const QString &answerType,
                               const QString &answerSdp)
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(answerType); Q_UNUSED(answerSdp); return 0;
    }
    virtual quint64 callReject(const QString &roomId, const QString &callId,
                               const QString &partyId)
    { Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId); return 0; }
    virtual quint64 callHangup(const QString &roomId, const QString &callId,
                               const QString &partyId, const QString &reason)
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(reason); return 0;
    }
    virtual quint64 callSelectAnswer(const QString &roomId,
                                     const QString &callId,
                                     const QString &partyId,
                                     const QString &selectedPartyId)
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(selectedPartyId); return 0;
    }
    virtual quint64 callRtcDecline(const QString &roomId,
                                   const QString &notificationEventId)
    { Q_UNUSED(roomId); Q_UNUSED(notificationEventId); return 0; }
    // Only while a media backend is registered does the backend carry remote
    // SDP into a bounded, C++-memory-only, single-shot store (take removes).
    // SDP must never reach QML, logs or persistence.
    virtual void setCallMediaCapable(bool capable) { Q_UNUSED(capable); }
    virtual QString takeCallSessionDescription(const QString &eventId)
    { Q_UNUSED(eventId); return {}; }
    // Trickle our locally gathered ICE candidates (media engine present
    // only). `candidates` entries: {candidate, sdpMid, sdpMLineIndex}.
    virtual quint64 callCandidates(const QString &roomId,
                                   const QString &callId,
                                   const QString &partyId,
                                   const QVariantList &candidates)
    {
        Q_UNUSED(roomId); Q_UNUSED(callId); Q_UNUSED(partyId);
        Q_UNUSED(candidates); return 0;
    }
    // Homeserver TURN credentials for the engine's ICE config.
    virtual quint64 requestCallTurnServers() { return 0; }

    // MatrixRTC (MSC4143) group calling.
    virtual bool supportsMatrixRtc() const { return false; }
    // Read one room's session; answers on rtcSessionReceived.
    //
    // `preferServer` reads the room's state from the homeserver as well as the
    // local store. It costs a /state request, so it is only for a caller with
    // evidence the store is incomplete (e.g. an SFU participant no membership
    // accounts for).
    virtual quint64 rtcSession(const QString &roomId, bool preferServer = false)
    { Q_UNUSED(roomId); Q_UNUSED(preferServer); return 0; }
    // Discover usable transports for this account; `roomId` may be empty
    // and, when given, adds the focus the room's participants advertise.
    // Answers on rtcTransportsReceived.
    virtual quint64 rtcTransports(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    // Publish or refresh our own membership; answers rtcMembershipPublished.
    virtual quint64 rtcPublishMembership(const QString &roomId,
                                         const QString &focusUrl,
                                         const QString &intent)
    {
        Q_UNUSED(roomId); Q_UNUSED(focusUrl); Q_UNUSED(intent); return 0;
    }
    /// Restart the server-side delayed retraction so it does not fire.
    virtual quint64 rtcRestartDelayedLeave(const QString &delayId)
    { Q_UNUSED(delayId); return 0; }
    /// Retract our membership and cancel the pending delayed retraction.
    virtual quint64 rtcRetractMembership(const QString &roomId,
                                         const QString &delayId)
    { Q_UNUSED(roomId); Q_UNUSED(delayId); return 0; }
    /// Distribute a media key, Olm-encrypted per device. The key is raw
    /// bytes base64'd; it is never logged and never reaches QML.
    virtual quint64 rtcSendMediaKey(const QString &roomId,
                                    const QString &keyBase64, int keyIndex,
                                    const QString &targetsJson)
    {
        Q_UNUSED(roomId); Q_UNUSED(keyBase64); Q_UNUSED(keyIndex);
        Q_UNUSED(targetsJson); return 0;
    }

    // ── LiveKit SFU signalling ──
    virtual bool supportsSfu() const { return false; }
    virtual quint64 sfuConnect(const QString &serviceUrl,
                               const QString &roomId)
    { Q_UNUSED(serviceUrl); Q_UNUSED(roomId); return 0; }
    /// `target` is "publisher" (our tracks) or "subscriber" (everyone
    /// else's) — LiveKit runs two peer connections.
    virtual void sfuLocalDescription(const QString &kind,
                                     const QString &target,
                                     const QString &sdp)
    { Q_UNUSED(kind); Q_UNUSED(target); Q_UNUSED(sdp); }
    virtual void sfuLocalCandidate(const QString &target,
                                   const QString &candidateInit)
    { Q_UNUSED(target); Q_UNUSED(candidateInit); }
    /// Declare a track to the SFU before negotiating it. `encrypted` tells
    /// LiveKit the frames carry E2EE (Encryption::GCM), which is how receivers
    /// decide to decrypt. `width`/`height` are the video size (0 for audio);
    /// without them the SFU assumes three-layer simulcast.
    virtual void sfuAddTrack(const QString &cid, const QString &name,
                             int kind, int width, int height,
                             bool screenShare, bool encrypted)
    {
        Q_UNUSED(cid); Q_UNUSED(name); Q_UNUSED(kind);
        Q_UNUSED(width); Q_UNUSED(height);
        Q_UNUSED(screenShare); Q_UNUSED(encrypted);
    }
    virtual void sfuMuteTrack(const QString &sid, bool muted)
    { Q_UNUSED(sid); Q_UNUSED(muted); }
    virtual void sfuDisconnect() {}

    virtual quint64 rtcNotify(const QString &roomId,
                              const QString &notificationType,
                              const QString &intent, quint64 lifetimeMs,
                              const QString &membershipEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(notificationType); Q_UNUSED(intent);
        Q_UNUSED(lifetimeMs); Q_UNUSED(membershipEventId); return 0;
    }
    // Raised hands, in element-call's own wire format: raising sends an
    // `m.reaction` annotating the sender's own `m.call.member` state event
    // with the raised-hand emoji, lowering REDACTS that reaction.
    // `membershipEventId` is required to raise, `reactionEventId` to lower.
    // Answers on rtcHandResult; a successful raise carries the reaction id the
    // eventual lower must redact.
    virtual quint64 rtcSetHandRaised(const QString &roomId,
                                     const QString &membershipEventId,
                                     const QString &reactionEventId,
                                     bool raised)
    {
        Q_UNUSED(roomId); Q_UNUSED(membershipEventId);
        Q_UNUSED(reactionEventId); Q_UNUSED(raised); return 0;
    }
    // element-call's transient call reaction: `io.element.call.reaction`
    // referencing the sender's own call membership. Never redacted; it expires
    // on its own. `emoji` and `name` must be a pair element-call knows; the
    // bridge refuses unknown ones. Answers on rtcSendFinished.
    virtual quint64 rtcSendCallReaction(const QString &roomId,
                                        const QString &membershipEventId,
                                        const QString &emoji,
                                        const QString &name)
    {
        Q_UNUSED(roomId); Q_UNUSED(membershipEventId);
        Q_UNUSED(emoji); Q_UNUSED(name); return 0;
    }
    // Hands already raised when we joined; an earlier raise produces no sync
    // event for us. Answers on rtcHandsReceived.
    virtual quint64 rtcReadRaisedHands(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    // Device sign-out through reusable UIA: deleteDevices -> (challenge ->
    // uiaRequired) -> uiaSubmitPassword / uiaCancel -> deviceDeleteFinished.
    // Credentials pass through transiently and are scrubbed; never stored,
    // logged or echoed.
    virtual bool supportsDeviceDeletion() const { return false; }
    virtual quint64 deleteDevices(const QStringList &deviceIds)
    { Q_UNUSED(deviceIds); return 0; }
    virtual bool uiaSubmitPassword(quint64 uiaId, const QString &password)
    { Q_UNUSED(uiaId); Q_UNUSED(password); return false; }
    virtual void uiaCancel(quint64 uiaId) { Q_UNUSED(uiaId); }
    // MAS/OAuth accounts manage sessions in the account web console instead of
    // password UIA. An empty deviceId opens the sessions list, otherwise that
    // device's page. Answers on oauthManagementUrlReceived.
    virtual quint64 requestOAuthManagementUrl(const QString &deviceId)
    { Q_UNUSED(deviceId); return 0; }
    // Server-side message search. Unencrypted rooms only (the server cannot
    // search ciphertext), and every UI surface must say so. Empty `roomId`
    // means all rooms; `nextBatch` pages. Answers on messageSearchFinished.
    virtual bool supportsMessageSearch() const { return false; }
    virtual quint64 searchMessages(const QString &term, const QString &roomId,
                                   const QString &nextBatch, int limit,
                                   const QVariantMap &filters = {})
    {
        Q_UNUSED(term); Q_UNUSED(roomId); Q_UNUSED(nextBatch); Q_UNUSED(limit);
        Q_UNUSED(filters);
        return 0;
    }
    virtual quint64 addRoomToSpace(const QString &spaceId, const QString &roomId)
    { Q_UNUSED(spaceId); Q_UNUSED(roomId); return 0; }
    // MSC1772 child removal (empty-via m.space.child). Never leaves or deletes
    // the child room.
    virtual quint64 removeRoomFromSpace(const QString &spaceId,
                                        const QString &roomId)
    { Q_UNUSED(spaceId); Q_UNUSED(roomId); return 0; }
    // Toggle the MSC1772 `suggested` flag on an existing child, preserving via
    // and order. A non-child is refused, never promoted to a child.
    virtual quint64 setSpaceChildSuggested(const QString &spaceId,
                                           const QString &roomId,
                                           bool suggested)
    { Q_UNUSED(spaceId); Q_UNUSED(roomId); Q_UNUSED(suggested); return 0; }

    // Attachment sending (Rust: SDK send queue with local echo). The caller
    // detects `mime` from file content. `durationMs` is the clip length for
    // timed media, 0 when unknown; zero is sent as absent, never as a literal
    // zero.
    virtual quint64 sendAttachment(const QString &roomId,
                                   const QString &localPath,
                                   const QString &mime,
                                   const QString &caption,
                                   int width, int height, bool animated,
                                   qint64 durationMs = 0)
    {
        Q_UNUSED(roomId); Q_UNUSED(localPath); Q_UNUSED(mime);
        Q_UNUSED(caption); Q_UNUSED(width); Q_UNUSED(height);
        Q_UNUSED(animated); Q_UNUSED(durationMs);
        return 0;
    }
    // Video send with a caller-extracted poster. An empty `thumbnail` is not an
    // error; the video goes out without one. The SDK uploads (and encrypts) the
    // poster. The default degrades to the plain attachment send.
    virtual quint64 sendVideo(const QString &roomId,
                              const QString &localPath,
                              const QString &mime,
                              const QString &caption,
                              int width, int height, qint64 durationMs,
                              const QByteArray &thumbnail,
                              int thumbnailWidth, int thumbnailHeight)
    {
        Q_UNUSED(durationMs); Q_UNUSED(thumbnail);
        Q_UNUSED(thumbnailWidth); Q_UNUSED(thumbnailHeight);
        return sendAttachment(roomId, localPath, mime, caption, width, height,
                              false);
    }
    // Clipboard images: bytes transfer directly, no temporary file.
    virtual quint64 sendAttachmentBytes(const QString &roomId,
                                        const QByteArray &bytes,
                                        const QString &filename,
                                        const QString &mime,
                                        int width, int height)
    {
        Q_UNUSED(roomId); Q_UNUSED(bytes); Q_UNUSED(filename);
        Q_UNUSED(mime); Q_UNUSED(width); Q_UNUSED(height);
        return 0;
    }
    // Same payload for a room whose live timeline is not open (e.g.
    // forwarding). The variant above goes through the open timeline and refuses
    // other rooms. The SDK still encrypts for encrypted targets.
    virtual bool supportsRoomScopedAttachmentSend() const { return false; }
    virtual quint64 sendAttachmentBytesToRoom(const QString &roomId,
                                              const QByteArray &bytes,
                                              const QString &filename,
                                              const QString &mime,
                                              int width, int height)
    {
        Q_UNUSED(roomId); Q_UNUSED(bytes); Q_UNUSED(filename);
        Q_UNUSED(mime); Q_UNUSED(width); Q_UNUSED(height);
        return 0;
    }
    // MSC3245 voice message: duration and waveform (0..=100 amplitudes, may be
    // empty) through the normal encrypting attachment path. Answers on
    // attachmentQueueFinished.
    virtual quint64 sendVoiceMessage(const QString &roomId,
                                     const QString &localPath,
                                     const QString &mime,
                                     qint64 durationMs,
                                     const QList<int> &waveform)
    {
        Q_UNUSED(roomId); Q_UNUSED(localPath); Q_UNUSED(mime);
        Q_UNUSED(durationMs); Q_UNUSED(waveform);
        return 0;
    }
    // Thread twin of sendVoiceMessage, sent through the SDK's thread-focused
    // timeline as a real m.thread reply. 0 is a refusal, never a licence to
    // fall back to a room send.
    virtual quint64 sendThreadVoiceMessage(const QString &roomId,
                                           const QString &rootEventId,
                                           const QString &localPath,
                                           const QString &mime,
                                           qint64 durationMs,
                                           const QList<int> &waveform)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId); Q_UNUSED(localPath);
        Q_UNUSED(mime); Q_UNUSED(durationMs); Q_UNUSED(waveform);
        return 0;
    }

    // Attachment into a thread, through the SDK's thread-focused timeline so
    // the relation and encryption are the SDK's; never an ordinary room send.
    // Answers on attachmentQueueFinished; 0 when unsupported.
    virtual quint64 sendThreadAttachment(const QString &roomId,
                                         const QString &rootEventId,
                                         const QString &localPath,
                                         const QString &mime,
                                         const QString &caption,
                                         int width, int height, bool animated,
                                         qint64 durationMs = 0)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId); Q_UNUSED(localPath);
        Q_UNUSED(mime); Q_UNUSED(caption); Q_UNUSED(width);
        Q_UNUSED(height); Q_UNUSED(animated); Q_UNUSED(durationMs);
        return 0;
    }
    // Thread twin of sendVideo; without poster support it falls back to the
    // plain thread attachment.
    virtual quint64 sendThreadVideo(const QString &roomId,
                                    const QString &rootEventId,
                                    const QString &localPath,
                                    const QString &mime,
                                    const QString &caption,
                                    int width, int height, qint64 durationMs,
                                    const QByteArray &thumbnail,
                                    int thumbnailWidth, int thumbnailHeight)
    {
        Q_UNUSED(durationMs); Q_UNUSED(thumbnail);
        Q_UNUSED(thumbnailWidth); Q_UNUSED(thumbnailHeight);
        return sendThreadAttachment(roomId, rootEventId, localPath, mime,
                                    caption, width, height, false);
    }
    virtual quint64 sendThreadAttachmentBytes(const QString &roomId,
                                              const QString &rootEventId,
                                              const QByteArray &bytes,
                                              const QString &filename,
                                              const QString &mime,
                                              int width, int height)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId); Q_UNUSED(bytes);
        Q_UNUSED(filename); Q_UNUSED(mime); Q_UNUSED(width); Q_UNUSED(height);
        return 0;
    }

    // Fetch (and decrypt, inside the SDK) media bytes for a timeline media key.
    // kind: 0 full, 1 thumbnail. timeoutClass: 0 standard, 1 playable
    // materialization, 2 explicit Save As; the backend bounds the fetch
    // accordingly.
    virtual quint64 fetchMedia(const QString &mediaKey, int kind,
                               int timeoutClass = 0)
    { Q_UNUSED(mediaKey); Q_UNUSED(kind); Q_UNUSED(timeoutClass); return 0; }
    // Server-side thumbnail of a plain mxc URI (avatars).
    virtual quint64 fetchMxcThumbnail(const QString &mxc, int width, int height)
    { Q_UNUSED(mxc); Q_UNUSED(width); Q_UNUSED(height); return 0; }
    // Cancel an in-flight media fetch. Best-effort and idempotent; no
    // mediaReady/mediaFailed follows a cancelled op.
    virtual void cancelMediaFetch(quint64 opId) { Q_UNUSED(opId); }

    // Server upload limit in bytes; 0 while unknown.
    virtual qint64 maxUploadSize() const { return 0; }

Q_SIGNALS:
    void loginSucceeded(const QString &userId);
    void loginFailed(const QString &reason);
    void loggedOut();
    // Authentication methods this homeserver offers. `sso` reports legacy
    // Matrix SSO for UI copy only and must not be presented as usable here: the
    // SDK helper needs sso-login/local-server features whose axum dependency is
    // not vendored.
    void authMethodsDiscovered(const QString &homeserver,
                               bool password,
                               bool oauth,
                               bool sso);
    // Authorization URL for the system browser. No credentials, but single-use,
    // so not logged.
    void oauthBrowserUrlReady(const QString &url);
    // The homeserver's SSO redirect URL for the system browser. No credentials,
    // but single-use, so not logged.
    void ssoBrowserUrlReady(const QString &url);
    // The system browser could not be launched (OAuth or SSO). The flow stays
    // alive (timeout and Cancel still apply); this lets the UI say why nothing
    // appeared.
    void browserLaunchFailed();
    // m.login.sso providers as {id, name, icon} maps; empty with sso true means
    // one unnamed flow. `icon` is an mxc: URI or empty, never http, so the
    // login screen cannot be made to fetch from a server-chosen host.
    void ssoProvidersReceived(const QString &homeserver, bool sso,
                              const QVariantList &providers);

    void connectionStateChanged(ConnectionState state);
    void initialSyncDoneChanged();
    void syncModeChanged();
    void roomsChanged();
    void roomUpdated(const QString &roomId);
    void timelineReset(const QString &roomId);
    // Opening a thread timeline failed. Success is
    // timelineReset(threadTimelineId(...)).
    void threadTimelineFailed(const QString &roomId,
                              const QString &rootEventId,
                              const QString &category);
    // Each thread entry: rootEventId, rootSender, rootSenderName, rootPreview,
    // rootTimestamp, replyCount, latestSender, latestSenderName, latestPreview,
    // latestTimestamp. Bounded to the pages fetched so far.
    void threadListUpdated(const QString &roomId, const QVariantList &threads,
                           bool endReached, bool failed);
    void threadSubscriptionState(const QString &roomId,
                                 const QString &rootEventId, bool supported,
                                 bool subscribed, bool automatic);
    void threadSubscriptionResult(const QString &roomId,
                                  const QString &rootEventId, bool ok,
                                  bool subscribed);
    void eventAppended(const QString &roomId, const TimelineEvent &event);
    void eventStatusChanged(const QString &roomId,
                            const QString &eventId,
                            TimelineEvent::Status status);

    void eventReplaced(const QString &roomId,
                       const QString &oldEventId,
                       const TimelineEvent &newEvent);

    // Index-based diffs for backends whose timeline mirrors an SDK vector.
    // Indices refer to timeline(roomId) after the op was applied; TimelineModel
    // re-validates them.
    void eventInsertedAt(const QString &roomId, int index,
                         const TimelineEvent &event);
    // A contiguous run of SDK inserts applied as one model transaction, so
    // back-pagination (many one-item Inserts) is laid out once, not per item.
    void eventsInsertedAt(const QString &roomId, int index,
                          const QList<TimelineEvent> &events);
    void eventChangedAt(const QString &roomId, int index,
                        const TimelineEvent &event);
    void eventRemovedAt(const QString &roomId, int index);
    void eventsTruncatedTo(const QString &roomId, int length);
    void eventEdited(const QString &roomId, const QString &eventId);
    void eventRedacted(const QString &roomId, const QString &eventId);
    void reactionsChanged(const QString &roomId, const QString &eventId);
    void eventsPrepended(const QString &roomId, const QList<TimelineEvent> &events);
    void paginationStateChanged(const QString &roomId);
    void typingChanged(const QString &roomId);
    void membersChanged(const QString &roomId);
    // A sync m.room.member event was seen (membership, display name or avatar
    // change). Distinct from membersChanged, which drives presentation
    // refreshes of every loaded row; this can fire per event in busy rooms and
    // only reaches roster-refetch consumers.
    void roomMemberEventSeen(const QString &roomId);

    // Server-reported per-room mode (0/1/2 as above). userDefined is true for
    // an explicit room rule, false for the resolved account default. Mode and
    // room id only; never push-rule JSON.
    void roomNotificationModeChanged(const QString &roomId, int mode,
                                     bool userDefined);
    // A push-rule write failed: the device-local mode is kept and the UI must
    // not claim it was saved to the account. Room id only.
    void roomNotificationModeWriteFailed(const QString &roomId);
    // The room's user-defined push rules were removed; it follows the account
    // default. Separate from roomNotificationModeChanged because this is the
    // absence of a rule; it acknowledges a "follow account default" write.
    void roomNotificationModeCleared(const QString &roomId);

    void errorOccurred(const QString &message);

    // ---- Async command results. Payloads are non-secret: coarse categories
    // ("network", "forbidden", "rate_limited", ...), never bodies, tokens, keys
    // or local paths.
    void userSearchFinished(quint64 opId, bool ok,
                            const QVariantList &results, bool limited,
                            const QString &category);
    // Exact profile lookup. ok=false with "not_found" means the server does not
    // know the user; other categories are transient.
    void userProfileFinished(quint64 opId, bool ok, const QString &userId,
                             const QString &displayName,
                             const QString &avatarUrl,
                             const QString &category);
    // Terminal answer for setOwnDisplayName(), by op id. `error` is the
    // server's sanitized sentence, or empty when there was none (timeout,
    // transport failure, synchronous refusal) and the UI supplies the wording.
    // The name is not echoed.
    void ownDisplayNameChanged(quint64 opId, bool ok, const QString &error);
    // Terminal answer for setOwnAvatar()/clearOwnAvatar(), same conventions.
    // The path is never echoed: it contains the user's name.
    void ownAvatarChanged(quint64 opId, bool ok, const QString &error);
    // Answer to fetchMutualRooms. `rooms` carries maps of
    // roomId/name/avatarUrl/isDirect — presentation-safe values only.
    void mutualRoomsReceived(quint64 opId, const QString &userId,
                             const QVariantList &rooms);
    // URL preview. `fields` carries whitelisted OpenGraph values only (title,
    // description, siteName, imageMxc, imageMime, imageWidth, imageHeight,
    // imageSize), never the URL. httpStatus/redirectCount are sanitized failure
    // diagnostics (0 when not applicable).
    void urlPreviewFinished(quint64 opId, bool ok, const QVariantMap &fields,
                            const QString &category, int httpStatus = 0,
                            int redirectCount = 0);
    // One GIF-provider response. `body` is bounded JSON for the GIF controller
    // (never surfaced to QML); empty on failure. `category` is a coarse state
    // (ok/rate_limited/provider_error/timeout/network/too_large/blocked). The
    // request URL carries the provider key and is never emitted or logged.
    void gifResponse(quint64 opId, bool ok, int httpStatus,
                     const QByteArray &body, const QString &category);
    // A validated GIF download: the bytes on success, else a coarse category
    // (blocked/not_a_gif/too_large/invalid_media/timeout/network/provider_error).
    void gifDownloadFinished(quint64 opId, bool ok, const QByteArray &bytes,
                             const QString &mime, int width, int height,
                             qint64 size, const QString &category);
    void dmCreateFinished(quint64 opId, bool ok, const QString &roomId,
                          const QString &category);
    void roomCreateFinished(quint64 opId, bool ok, const QString &roomId,
                            const QString &category, const QString &warning);
    void inviteUserFinished(quint64 opId, const QString &roomId,
                            const QString &userId, bool ok,
                            const QString &category);
    void inviteBatchFinished(quint64 opId, const QString &roomId,
                             int okCount, int failCount);
    // snapshot: ok, truncated, joinedCount, invitedCount, canInvite,
    // canEditName, canEditTopic, canEditAvatar, members(QVariantList of
    // maps: userId, displayName, avatarUrl, membership, role, ambiguous,
    // isOwn).
    void roomMembersReceived(quint64 opId, const QString &roomId,
                             const QVariantMap &snapshot);
    // Thread participants for the facepile: user-id-deduplicated maps (userId,
    // displayName, avatarUrl), root sender first. `distinct` counts distinct
    // senders (not replies); `truncated` means more exist. A failed lookup has
    // an empty list and distinct 0 and means "unknown", never "nobody".
    void threadParticipantsReceived(const QString &roomId,
                                    const QString &rootEventId,
                                    const QVariantList &participants,
                                    int distinct, bool truncated);
    // "Remove edits" result, counts only: `removed` redacted, `failed` refused,
    // `truncated` when one pass could not remove the whole chain. ok means
    // nothing failed; a message with no edits reports ok with removed 0.
    void messageEditsRemoved(const QString &roomId, const QString &eventId,
                             bool ok, int removed, int failed,
                             bool truncated);
    // One presence polling round. Each entry: userId, ok, state ("online" /
    // "unavailable" / "offline" / "unknown"), currentlyActive, lastActiveAgoMs
    // (-1 when none), category (ok=false only). ok=false means unknown, never
    // offline.
    void presenceReceived(quint64 opId, const QVariantList &entries);
    // A user's profile banner. `supported` false means the homeserver lacks
    // extended profile fields, which renders as nothing rather than "no
    // banner".
    void profileBannerReceived(quint64 opId, const QString &userId,
                               const QString &mxc, bool supported);
    void profileBannerSet(quint64 opId, bool ok, const QString &mxc,
                          const QString &category);
    // Chosen display-name colour as `#rrggbb`, or empty. `supported` false
    // means the homeserver lacks extended profile fields.
    void nameColorReceived(quint64 opId, const QString &userId,
                           const QString &color, bool supported);
    void nameColorSet(quint64 opId, bool ok, const QString &color,
                      const QString &category);
    // A user's bio as plain text. `supported` false means the homeserver lacks
    // extended profile fields, so an absent bio is never an error.
    void profileBioReceived(quint64 opId, const QString &userId,
                            const QString &bio, bool supported);
    void profileBioSet(quint64 opId, bool ok, const QString &bio,
                       const QString &category);
    // A room's banner, and whether this account may change it (per the SDK's
    // power levels).
    void roomBannerReceived(quint64 opId, const QString &roomId,
                            const QString &mxc, bool canSet);
    void roomBannerSet(quint64 opId, const QString &roomId, bool ok,
                       const QString &mxc, const QString &category);
    // One MSC2545 snapshot of every usable pack, validated and bounded in Rust;
    // see StickerPackModel for the row shape. An empty list means "no packs".
    // `roomCanManage` is whether this account may write `im.ponies.room_emotes`
    // in `roomId`, reported per room so a first pack can be created. Unknown
    // permission is reported as false.
    void stickerPacksReceived(quint64 opId, const QString &roomId,
                              bool roomCanManage, const QVariantList &packs);
    /// One policy room's rules. `truncated` says the read hit its bound.
    void policyRulesReceived(quint64 opId, bool ok, const QString &roomId,
                             bool canWrite, bool truncated,
                             const QVariantList &rules);
    void policyRuleWritten(quint64 opId, bool ok, const QString &category);
    void policySubscriptionsReceived(quint64 opId, bool ok,
                                     const QString &category,
                                     const QStringList &rooms);
    /// Whether the subscribed lists cover an entity. `detail` carries
    /// roomId, ruleEntity, ruleKind and reason when matched.
    void policyCheckFinished(quint64 opId, const QString &entity, bool matched,
                             const QVariantMap &detail);
    /// One step of an MSC4108 sign-in-another-device flow.
    ///
    /// `step`: "starting", "qr_ready", "check_code_needed", "check_code_shown",
    /// "waiting_for_auth", "syncing_secrets", "done", "failed". `detail`
    /// carries that step's payload: qr_ready -> qrSize (int), qrBits (base64),
    /// qrText; check_code_shown -> checkCode (int); waiting_for_auth ->
    /// verificationUri; failed -> category.
    ///
    /// A step naming an old `generation` belongs to an abandoned flow and must
    /// be ignored.
    void qrLoginProgress(quint64 generation, const QString &step,
                         const QVariantMap &detail);
    /// Result of editStickerPack. `shortcode` is the code actually stored by a
    /// rename, which is sanitized and may differ from what was typed.
    void stickerPackEditFinished(quint64 opId, bool ok, const QString &category,
                                 const QString &shortcode);
    // Result of adding a sticker to a pack. `category` is "duplicate",
    // "pack_full", or a coarse room-error class.
    void stickerPackAddFinished(quint64 opId, bool ok, const QString &category,
                                const QString &shortcode);
    // The result of turning a room's pack on or off globally.
    void stickerPackRoomsSet(quint64 opId, bool ok, const QString &category,
                             const QString &roomId, const QString &stateKey,
                             bool enabled);
    // Publishing the user's own presence failed (coarse category); diagnostics
    // only.
    /// `retryAfterMs` is the server's M_LIMIT_EXCEEDED hint, or 0. The receiver
    /// bounds it before acting.
    void presencePublishFailed(const QString &category, qint64 retryAfterMs);
    void roomEditFinished(quint64 opId, const QString &roomId,
                          const QString &field, bool ok,
                          const QString &category);
    // Answer to requestRoomDirectoryVisibility; `published` is meaningful only
    // when ok.
    void roomDirectoryVisibilityReceived(const QString &roomId, bool ok,
                                         bool published);
    // Room upgrade. `available` is [{version, stable}] in display order;
    // `defaultVersion` is what the server creates rooms with.
    void roomVersionsReceived(bool ok, const QString &defaultVersion,
                              const QVariantList &available);
    void roomUpgradeFinished(quint64 opId, const QString &roomId, bool ok,
                             const QString &replacementRoomId,
                             const QString &category);
    // Device and backup management.
    void deviceRenamed(quint64 opId, bool ok, const QString &category);
    // SENSITIVE: recoveryKey is real secret material when non-empty.
    // Consumers display it once and clear it; nothing may log it.
    void backupActionFinished(quint64 opId, const QString &action, bool ok,
                              const QString &recoveryKey,
                              const QString &category);
    void backupProgress(const QString &backupState, const QString &uploadState,
                        qint64 backedUp, qint64 total);
    // `revisions` is [{eventId, sender, timestamp(QDateTime), body,
    // formattedBody, redacted, undecryptable, isOriginal, isLatest}] in
    // chronological order; `encryption` is {encrypted, senderKey, senderDevice,
    // algorithm, verification}. Both can carry decrypted text: memory-only,
    // never logged or cached.
    //
    // `partial` is true when this is not the whole history (the event cache
    // answered, or the server had more than one page). The missing rows are the
    // oldest, and the UI must say so.
    void editHistoryReceived(const QString &roomId, const QString &eventId,
                             bool ok, bool partial,
                             const QVariantList &revisions);
    void eventSourceReceived(const QString &roomId, const QString &eventId,
                             bool ok, const QString &json,
                             const QVariantMap &encryption);
    /// Jump to date. `eventId` is empty when not ok; `category` is a sanitized
    /// shape ("not_found" when the server lacks the endpoint), never server
    /// prose.
    void eventAtTimestampReceived(quint64 opId, const QString &roomId, bool ok,
                                  const QString &eventId, qint64 timestampMs,
                                  const QString &category);
    /// Local search results: maps of eventId, roomId, sender, senderName, body,
    /// msgtype, timestampMs. `category` "too_short" means the query cannot
    /// match the trigram tokenizer, which differs from "no results".
    ///
    /// PRIVACY: bodies may be decrypted plaintext. They live in the receiving
    /// model's memory only: never persisted, logged or sent anywhere.
    void localSearchFinished(quint64 opId, bool ok, const QString &category,
                             int minChars, const QVariantList &results);
    /// A room's widgets: id, creator, kind, name, url, refusal, discloses.
    /// `url` is resolved and validated; when empty, `refusal` says why it
    /// cannot open. `canManage` is whether this account may write the room's
    /// widget state, so Add and Remove are offered only when they can work.
    void roomWidgetsReceived(quint64 opId, const QString &roomId, bool ok,
                             bool canManage, const QVariantList &widgets);
    /// A widget write finished. `category` is empty on success and a coarse
    /// room-error class otherwise ("forbidden", "unknown_room", ...).
    void roomWidgetWritten(quint64 opId, const QString &roomId, bool ok,
                           const QString &category);
    /// A room's advertised bridges (MSC2346): protocol, protocolName, network,
    /// sanitised in Rust because any sufficiently powered member can write
    /// them. `bridgebot` and `creator` are omitted: admin-chosen mxids that
    /// would read as unverifiable provenance. An empty list is a fact; `ok` is
    /// false only when the read failed.
    void roomBridgesReceived(quint64 opId, const QString &roomId, bool ok,
                             const QVariantList &bridges);
    /// One page of a room's media-history walk. `scanned` counts events
    /// examined, not matched, so the panel never claims "all media"
    /// prematurely. `complete` is the start of accessible history.
    /// `undecryptable` counts history that is present but unreadable.
    void mediaHistoryPage(quint64 opId, const QString &roomId,
                          const QVariantList &entries, qint64 scanned,
                          qint64 scannedTotal, qint64 undecryptable,
                          bool complete, bool encryptedRoom);
    void mediaHistoryFailed(quint64 opId, const QString &roomId,
                            const QString &message);
    /// A per-room profile write finished. `field` is "displayname" or
    /// "avatar_url"; they are separate requests and can fail independently.
    void roomProfileResult(quint64 opId, const QString &roomId,
                           const QString &field, bool ok,
                           const QString &error);
    void searchIndexStatsReceived(quint64 opId, qint64 messages, qint64 rooms);
    void searchIndexSwept(quint64 opId, int rooms, int written,
                          qint64 messages, qint64 indexedRooms);
    void searchIndexDeepened(quint64 opId, bool ok, const QString &roomId,
                             int pages, bool reachedStart, int written,
                             qint64 messages, const QString &category);
    // Scheduled send.
    void delayedEventsSupportReceived(bool supported, bool advertised);
    void scheduledSendFinished(quint64 opId, const QString &roomId, bool ok,
                               const QString &delayId, const QString &category);
    void scheduledUpdateFinished(quint64 opId, const QString &delayId,
                                 const QString &action, bool ok,
                                 const QString &category);
    void roomSendFinished(quint64 opId, const QString &roomId, bool ok,
                          const QString &category);
    // A reaction seen over sync in any room. Only ids, sender and the bounded
    // key cross; the Activity Center decides whether it targets the user's own
    // message.
    void reactionEventReceived(const QString &roomId, const QString &reactionEventId,
                               const QString &targetEventId, const QString &senderId,
                               const QString &key, qint64 timestampMs);
    void activitySeedReceived(const QVariantList &entries);
    void roomLeaveFinished(quint64 opId, const QString &roomId, bool ok,
                           const QString &category);
    // op is "kick" or "ban"; category is a sanitized error class on failure.
    void moderationFinished(quint64 opId, const QString &roomId,
                            const QString &userId, const QString &op,
                            bool ok, const QString &category);
    // One member's power-level write completed. `level` echoes the request; the
    // authoritative value comes from the roster refresh that follows.
    void powerLevelChangeFinished(quint64 opId, const QString &roomId,
                                  const QString &userId, qlonglong level,
                                  bool ok, const QString &category);
    // One m.room.power_levels threshold write completed. `level` echoes the
    // request; the roster refresh that follows is authoritative.
    void roomPowerMatrixFinished(quint64 opId, const QString &roomId,
                                 const QString &key, qlonglong level,
                                 bool ok, const QString &category);
    // One resolved pinned snapshot: ok, canPin, total, truncated and entries
    // (eventId, available, and when available sender, senderDisplayName,
    // senderAvatarUrl, timestampMs, kind, preview). Previews may be decrypted
    // text: memory only, never CacheStore.
    void pinnedReceived(quint64 opId, const QString &roomId,
                        const QVariantMap &snapshot);
    // A pin/unpin write completed. `changed` is false for a no-op, which is
    // reported as such.
    void pinChangeFinished(quint64 opId, const QString &roomId,
                           const QString &eventId, bool pin, bool ok,
                           bool changed, const QString &category);
    // m.room.pinned_events changed in this room. No payload; consumers re-read
    // through requestPinnedMessages.
    void pinnedEventsChanged(const QString &roomId);
    // Discovery. `result` carries ok / category and, when ok: target, via
    // (QStringList), eventId, previewOk and, when previewed, roomId, alias,
    // name, topic, avatarUrl, members, joinRule, membership, isSpace (else
    // previewCategory).
    void roomTargetResolved(quint64 opId, const QVariantMap &result);
    // One directory page. Each row: roomId, name, alias, topic, avatarUrl,
    // members, joinRule, membership, worldReadable, guestCanJoin, isSpace.
    void publicRoomsReceived(quint64 opId, bool ok, const QVariantList &rooms,
                             const QString &nextBatch, quint64 totalEstimate,
                             const QString &category);
    void roomJoinFinished(quint64 opId, bool ok, const QString &roomId,
                          const QString &category);
    void roomKnockFinished(quint64 opId, bool ok, const QString &roomId,
                           const QString &category);
    void knockCancelFinished(quint64 opId, bool ok, const QString &roomId,
                             const QString &category);
    // Personal moderation results. `ignored` echoes the requested direction;
    // ignoredUsersChanged is the sync push for local and remote changes.
    void ignoreUserFinished(quint64 opId, const QString &userId, bool ignored,
                            bool ok, const QString &category);
    void ignoredUsersReceived(quint64 opId, bool ok, const QStringList &users);
    void ignoredUsersChanged(const QStringList &users);
    void reportMessageFinished(quint64 opId, const QString &roomId,
                               const QString &eventId, bool ok,
                               const QString &category);
    // Voice-call signaling: one inbound observation (SDP-free, see
    // CallSignal.h) and one terminal send result per dispatched op.
    void callSignalReceived(const CallSignal &signal);
    void callSendFinished(quint64 opId, bool ok, const QString &category,
                          const QString &callId, const QString &eventId);
    // Remote trickled ICE (media-capable mode only). Transport data for the
    // engine; never logged or rendered.
    void callCandidatesReceived(const QString &roomId, const QString &callId,
                                const QString &partyId, bool own,
                                const QVariantList &candidates);
    // Short-lived TURN credentials: engine-only, never logged.
    void callTurnServersReceived(quint64 opId, bool ok,
                                 const QString &username,
                                 const QString &password,
                                 const QStringList &uris, qint64 ttlSeconds,
                                 const QString &category);

    // MatrixRTC observation. `rtcSessionChanged` is a payload-free poke to
    // re-read the session, so remote and local changes share one parse path.
    // `rtcSessionReceived` answers a read.
    void rtcSessionReceived(quint64 opId, const RtcSessionData &session);
    void rtcSessionChanged(const QString &roomId);
    // Discovery. `serverAnswered` false with a category distinguishes "no
    // MatrixRTC here" from "the request failed". URLs are opaque; never render
    // them raw.
    void rtcTransportsReceived(quint64 opId, bool serverAnswered,
                               const QString &category,
                               const QStringList &serverServiceUrls,
                               const QString &participantFocusUrl);
    void rtcSendFinished(quint64 opId, bool ok, const QString &category,
                         const QString &eventId);
    /// A raise or lower completed. After a raise, `eventId` is the reaction a
    /// later lower must redact.
    void rtcHandResult(quint64 opId, bool ok, bool raised,
                       const QString &category, const QString &eventId);
    /// A hand went up or down (sync). A raise carries `membershipEventId`; a
    /// lower carries only the redacted reaction id, which the receiver matches
    /// against the ids it tracks.
    void rtcHandChanged(const QString &roomId, const QString &sender,
                        const QString &membershipEventId,
                        const QString &reactionEventId, bool raised);
    /// A transient call reaction (sync), attributed through the referenced
    /// membership so a sender who does not own it can be refused.
    void rtcCallReactionReceived(const QString &roomId, const QString &sender,
                                 const QString &membershipEventId,
                                 const QString &emoji);
    /// The join-time sweep. Each entry:
    /// {userId, deviceId, rtcIdentity, membershipEventId, reactionEventId}.
    void rtcHandsReceived(quint64 opId, const QString &roomId,
                          const QVariantList &hands);
    /// Our membership was published. An empty `delayId` means no delayed
    /// retraction was armed, so cleanup relies on the membership's `expires`.
    /// `delayedCategory` is sticky, not per-answer: it is also empty when no
    /// arm was attempted (rtc.rs skips it once the refusal is latched).
    /// `unrecognized`/`not_found`/`no_delay_id` mean the endpoint is absent
    /// (the latched cases); `rate_limited`/`forbidden`/`invalid`/`network` are
    /// transient or room-specific and the next publish retries.
    void rtcMembershipPublished(quint64 opId, bool ok, const QString &category,
                                const QString &eventId,
                                const QString &delayId,
                                const QString &delayedCategory);
    void rtcMembershipRetracted(quint64 opId, bool ok,
                                const QString &category);
    void rtcMediaKeySent(quint64 opId, bool ok, const QString &category,
                         int delivered, int keyIndex);
    /// A media key from another device, already Olm-decrypted. `sender` is
    /// vouched for by the SDK; `claimedDeviceId` is only a claim. The key is
    /// base64 raw bytes: C++ memory only, never QML or logs.
    void rtcMediaKeyReceived(const QString &roomId, const QString &sender,
                             const QString &claimedDeviceId, int keyIndex,
                             const QString &keyBase64);

    // ── SFU signalling ──
    /// Closed-set lifecycle: authorized / signalling / ended / closed /
    /// failed. `category` explains a failure and is safe to log.
    void sfuStateChanged(const QString &state, const QString &category);
    /// LiveKit's per-room server-injected-frame trailer
    /// (`JoinResponse.sif_trailer`): at most 64 raw bytes, empty when absent.
    /// It marks the unencrypted blank frames the SFU writes into encrypted
    /// tracks; see SfuMediaEngine::framesServerInjected().
    void sfuJoined(const QString &identity, const QVariantList &participants,
                   const QVariantList &iceServers,
                   const QByteArray &sifTrailer);
    void sfuParticipantsChanged(const QVariantList &participants);
    void sfuTrackPublished(const QString &cid, const QString &sid);
    void sfuSpeakersChanged(const QVariantList &speakers);
    void sfuConnectionQuality(const QVariantList &updates);
    /// Media transport only; never logged, never exposed to QML.
    void sfuRemoteDescription(const QString &kind, const QString &target,
                              const QString &sdp);
    void sfuRemoteCandidate(const QString &target,
                            const QString &candidateInit);
    // The server requires interactive auth to complete the pending operation.
    // `stages` lists flow stage names for the "unsupported stage" display; only
    // the password stage is renderable. wrongPassword means a previous answer
    // was rejected.
    void uiaRequired(quint64 uiaId, bool hasPasswordStage,
                     bool wrongPassword, const QStringList &stages);
    void deviceDeleteFinished(quint64 opId, bool ok, const QString &category);
    void oauthManagementUrlReceived(quint64 opId, bool ok, const QString &url);
    // One server-search page. Each row: roomId, eventId, sender,
    // senderDisplayName, senderAvatarUrl, timestampMs, msgtype, body.
    void messageSearchFinished(quint64 opId, bool ok,
                               const QVariantList &results,
                               const QString &nextBatch, quint64 count,
                               const QString &category);
    // Space children incl. unjoined rows. Each row: roomId, name, alias,
    // topic, avatarUrl, members, joinRule, membership, isSpace,
    // childrenCount, suggested, via (QStringList).
    void spaceChildrenReceived(quint64 opId, const QString &spaceId, bool ok,
                               const QVariantList &rooms, bool truncated,
                               const QString &category);
    void spaceChildFinished(quint64 opId, const QString &spaceId,
                            const QString &roomId, bool ok);
    void spaceChildRemoveFinished(quint64 opId, const QString &spaceId,
                                  const QString &roomId, bool ok);
    void spaceChildSuggestedFinished(quint64 opId, const QString &spaceId,
                                     const QString &roomId, bool suggested,
                                     bool ok);
    // Queue acceptance only; delivery state flows through the item's send
    // state.
    void attachmentQueueFinished(quint64 opId, const QString &roomId,
                                 bool ok, const QString &category);
    void mediaReady(quint64 opId, const QString &mediaKey, int kind,
                    const QByteArray &bytes, const QString &mimetype,
                    const QString &filename);
    void mediaFailed(quint64 opId, const QString &mediaKey, int kind,
                     const QString &category);
    void maxUploadSizeChanged();
};
