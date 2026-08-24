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

// Pure C++ backend interface for Matrix operations. UI/models depend only on
// this — never on a concrete backend. Concrete implementations live in
// MockMatrixClient (v0.1 shell / --mock), CppHttpMatrixClient (v0.2+ default),
// and a future RustSdkMatrixClient (v0.4) that will wrap the Matrix Rust SDK
// via FFI for E2EE and sliding sync.
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
    // v0.7 account switching: end the LOCAL session only — stop sync, tear
    // down subscriptions, drop in-memory state, and emit loggedOut() so every
    // account-scoped model clears — but do NOT log out on the server and do
    // NOT delete the account's persisted store, tokens, or metadata. After a
    // detach, restoreSession() activates whichever account the settings now
    // select. Returns false when the backend cannot detach.
    virtual bool detachSession() { return false; }

    // --- OAuth 2.0 / OIDC browser sign-in -----------------------------------
    // Additive, with safe defaults: a backend that does not implement browser
    // authentication (Mock, the experimental C++ HTTP client) keeps working
    // unchanged and simply reports that it cannot do it. Only
    // RustSdkMatrixClient overrides these.
    //
    // Whether this backend can perform a browser sign-in AT ALL. Independent
    // of whether a particular homeserver offers one — that is discovery.
    virtual bool supportsOAuthLogin() const { return false; }
    // Ask the homeserver which authentication methods it really offers.
    // Answers asynchronously through authMethodsDiscovered(). Never
    // hard-codes behaviour for any particular server.
    virtual void discoverAuthMethods(const QString &homeserver) { Q_UNUSED(homeserver); }
    // Start a browser sign-in. Emits oauthBrowserUrlReady() with the URL to
    // open, then either loginSucceeded()/loginFailed() through the normal
    // session path. The Matrix user id is NOT known until this completes, so
    // no account store is opened before it does.
    virtual void beginOAuthLogin(const QString &homeserver) { Q_UNUSED(homeserver); }
    // User cancelled, or the wait timed out. Safe to call when nothing is in
    // flight. Must leave the UI in a resolved state, never in "Signing in".
    virtual void cancelOAuthLogin() {}

    // --- Legacy Matrix SSO (m.login.sso) --------------------------------
    // A DIFFERENT flow from OAuth, kept distinct on purpose: the homeserver
    // redirects back with a single-use `loginToken` which is exchanged through
    // /login. It shares only the loopback listener.
    virtual bool supportsSsoLogin() const { return false; }
    // Ask which identity providers the server advertises for SSO. Answers on
    // ssoProvidersReceived(). An SSO server with NO providers is normal and
    // means one unnamed flow.
    virtual void requestSsoProviders(const QString &homeserver) { Q_UNUSED(homeserver); }
    // Start an SSO sign-in. `idpId` empty selects the server's default flow.
    // Emits ssoBrowserUrlReady(), then the normal session path. As with OAuth
    // the user id is unknown until this completes, so no store is opened
    // before it does.
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

    // v0.4.6: true once at least one /sync response has been processed for
    // the current session. Backends that synthesise state immediately (Mock)
    // return true by default; only backends that talk to a real homeserver
    // need to override and toggle this. QML consumes it to distinguish
    // "still loading rooms" from "sync loop is live but there are no rooms".
    virtual bool initialSyncDone() const { return true; }
    virtual QString syncMode() const { return QStringLiteral("classic_fallback"); }

    // Room + timeline queries
    virtual QList<RoomInfo> rooms() const = 0;
    // Targeted single-room lookup. The default derives from rooms(), which
    // deep-copies the whole list — backends with a native index override it
    // (the per-appended-event notification context used to pay that full
    // copy for every event of a sync burst). Returns a default-constructed
    // RoomInfo when the room is unknown.
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

    // v0.7 outgoing @-mentions. `mentionUserIds` are full MXIDs to place in
    // m.mentions (deduped, order-insensitive). The default impls forward to
    // the zero-mention versions so Mock/HTTP backends stay correct; the Rust
    // backend overrides these to attach m.mentions through the SDK. The body
    // already carries the matrix.to markdown links for those users.
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

    // v0.4.1: reply into a thread rooted at `threadRootEventId`. Default
    // falls back to sendReply — the HTTP backend still delivers the message
    // and it's marked as an in-reply-to on the server. Concrete backends
    // (Mock; later CppHttp v0.5) may override to attach an `m.thread`
    // relation so ThreadManager sees a proper thread grouping.
    virtual void sendThreadReply(const QString &roomId,
                                 const QString &threadRootEventId,
                                 const QString &body)
    {
        sendReply(roomId, threadRootEventId, body);
    }
    // ---- v0.6.0: SDK-backed thread timelines.
    //
    // A thread timeline is addressed by a composite timeline id so it flows
    // through the SAME diff signal pipeline (timelineReset, eventInsertedAt,
    // paginationStateChanged, ...) and model code as a room timeline —
    // TimelineModel simply sets its roomId to the composite id. The id
    // embeds a unit separator, which can never appear in Matrix room/event
    // ids, so it can never collide with a real room id.
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

    // True when the backend can open live thread timelines. Backends
    // without support keep the false default and the thread UI stays
    // hidden.
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
    // v0.6.0 checkpoint 8: manual decryption retry for a room's visible
    // unable-to-decrypt events. The Rust backend re-runs SDK decryption
    // against key material that has arrived since; other backends have no
    // crypto machine and keep the no-op default. Never resets any store.
    virtual void retryDecryption(const QString &roomId) { Q_UNUSED(roomId); }

    // ---- v0.6.0 checkpoint 5: thread list, follow state, threaded read.
    //
    // The Threads view lists a room's threads (server /threads pagination,
    // kept live by the backend); follow state is MSC4306 server-side thread
    // subscription where the homeserver supports it; markThreadRead sends a
    // THREADED read receipt for the open thread panel only — never a
    // room-wide receipt.
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

    // v0.7 outgoing @-mentions inside a thread. `inReplyToEventId` empty is a
    // plain thread reply; non-empty is a rich reply within the thread. Default
    // forwards to the zero-mention path.
    virtual void sendThreadReplyTo(const QString &roomId,
                                   const QString &threadRootEventId,
                                   const QString &inReplyToEventId,
                                   const QString &body,
                                   const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        sendThreadReplyTo(roomId, threadRootEventId, inReplyToEventId, body);
    }

    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody) = 0;

    // v0.7 outgoing @-mentions on an edit. Default forwards to the zero-mention
    // edit path.
    virtual void editMessage(const QString &roomId,
                             const QString &targetEventId,
                             const QString &newBody,
                             const QStringList &mentionUserIds)
    {
        Q_UNUSED(mentionUserIds);
        editMessage(roomId, targetEventId, newBody);
    }
    virtual void redactEvent(const QString &roomId,
                             const QString &eventId,
                             const QString &reason = QString()) = 0;
    virtual void toggleReaction(const QString &roomId,
                                const QString &targetEventId,
                                const QString &key) = 0;

    // 2026-08-18: "Remove edits" — redact the OWN m.replace events attached
    // to a message so it returns to its original text. Matrix has no unedit
    // primitive, and a backend that cannot reach the relations honestly
    // reports it as unsupported rather than pretending the edits are gone.
    virtual bool supportsRemovingEdits() const { return false; }
    virtual void removeMessageEdits(const QString &roomId,
                                    const QString &eventId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(eventId);
    }

    // v0.7: MSC3381 polls (Rust backend only; mock/HTTP keep the honest
    // false default and the poll actions stay hidden/disabled in the UI).
    // threadRootId empty targets the room's live timeline, otherwise the
    // thread whose root it names. answerIds empty retracts the vote.
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
    virtual void setRoomMarkedUnread(const QString &roomId, bool unread)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(unread);
    }
    // Element-parity favourites, stored as the Matrix `m.favourite` room
    // tag so the flag is the ACCOUNT's, shared with every other client.
    // Backends without tag support report false and the affordance is not
    // offered — a device-local "favourite" would silently disagree with
    // Element, which is worse than not having one.
    //
    // Never optimistic: the row's flag comes from the backend's own room
    // payload after the write lands, so a refused write leaves the list
    // exactly as it was.
    virtual bool supportsRoomFavourites() const { return false; }
    virtual void setRoomFavourite(const QString &roomId, bool favourite)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(favourite);
    }
    // Mark a room read without opening it. Distinct from sendReadReceipt,
    // which can only ever point at an event in the LOADED timeline — empty
    // for a room that is not open, which made marking a closed room read a
    // silent no-op. Backends that cannot resolve a closed room's latest
    // event leave this inert rather than pretending it worked.
    virtual bool supportsMarkRoomRead() const { return false; }
    virtual void markRoomRead(const QString &roomId) { Q_UNUSED(roomId); }
    // Server-synchronized per-room notification mode (account push rules
    // managed entirely by the Matrix SDK). Modes match SettingsManager /
    // NotificationManager::RoomMode: 0 = all messages, 1 = mentions &
    // keywords, 2 = mute. Backends without push-rule support keep the
    // false default and the per-room mode stays a device-local setting.
    // setRoomNotificationMode is label-faithful: mode 0 sets an explicit
    // AllMessages rule. Mode 3 (follow the account default) is NOT sent
    // here — it is the ABSENCE of a room override, so it goes through
    // clearRoomNotificationMode(), which deletes the user-defined rules.
    // requestRoomNotificationMode usually answers asynchronously via
    // roomNotificationModeChanged with the user-defined rule when one
    // exists, else the account default resolved for the room's shape —
    // but it is deliberately SKIPPED while a write for the room is queued
    // or in flight (the write's own report is authoritative and imminent).
    virtual bool supportsServerNotificationModes() const { return false; }
    virtual void setRoomNotificationMode(const QString &roomId, int mode)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(mode);
    }
    // v0.7: ask for a thread's real participants (facepile). Answers
    // asynchronously via threadParticipantsReceived. Cache-first on the
    // Rust side, so a repeat request for a known thread costs no network.
    // Backends without thread support simply never answer, and the card
    // renders without a facepile.
    virtual void requestThreadParticipants(const QString &roomId,
                                           const QString &rootEventId)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId);
    }
    // v0.7.x Matrix presence. Sliding Sync delivers no presence events, so
    // presence is a bounded polling loop: PresenceManager watches exactly
    // the users that are on screen and requests one batch per round.
    // Answers asynchronously via presenceReceived. Backends without
    // presence keep the false default and never answer — indicators simply
    // stay absent, exactly like the thread facepile on a non-Rust backend.
    virtual bool supportsPresence() const { return false; }
    virtual void requestPresence(const QStringList &userIds, quint64 opId)
    {
        Q_UNUSED(userIds); Q_UNUSED(opId);
    }
    // Publish the local user's own presence (0 online, 1 unavailable,
    // 2 offline). Fire-and-forget: the UI claims nothing about publication.
    virtual void publishPresence(int state) { Q_UNUSED(state); }
    // Profile banners (MSC4427 over MSC4133). False on a backend that cannot
    // read extended profile fields; the banner is then simply absent, exactly
    // like presence or the thread facepile on such a backend.
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
    // Room / Space banners. Lightning's own state event — Matrix specifies
    // no room banner at all — so a backend that cannot send an arbitrary
    // state event simply has none, and the surface is not offered.
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
    // v0.7 "follow account default": drop this room's user-defined push
    // rules so the account's rules decide again. Success reports on the
    // dedicated roomNotificationModeCleared signal — NOT on
    // roomNotificationModeChanged, which carries a rule's value, whereas
    // this outcome is the absence of a rule. Backends without push-rule
    // support do nothing, and the mode stays a device-local setting.
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

    // v0.5.7: true when the last backward pagination for this room failed
    // and can be retried. Backends without failure tracking return false.
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

    // v0.5.7: retry a failed outgoing message identified by its send-queue
    // transaction id. Only the Rust backend (SDK local echoes) implements
    // this; the default is a no-op so HTTP/Mock behavior is unchanged.
    virtual void retryFailedSend(const QString &roomId,
                                 const QString &transactionId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(transactionId);
    }

    // Discard a queued outgoing message — including an in-flight media
    // upload — identified by its send-queue transaction id. Backed by the
    // SDK's SendHandle::abort, which is also the only thing that can lose
    // the race honestly: an event already on the server is NOT aborted and
    // the row stays. Backends without a send queue report false and the
    // affordance is not offered, because there is nothing there to cancel.
    virtual bool supportsCancelSend() const { return false; }
    virtual void cancelSend(const QString &roomId,
                            const QString &transactionId)
    {
        Q_UNUSED(roomId);
        Q_UNUSED(transactionId);
    }

    // ---- v0.5.9: conversation creation, membership, room editing, media.
    //
    // Command methods return an operation id (> 0) echoed on the matching
    // *Finished signal, or 0 when the backend does not support the
    // operation. Defaults are inert so Mock/HTTP builds keep working; the
    // UI hides or disables unsupported actions via supportsRoomManagement /
    // supportsAttachmentSend.
    virtual bool supportsRoomManagement() const { return false; }
    virtual bool supportsAttachmentSend() const { return false; }
    virtual bool supportsMediaBridge() const { return false; }

    virtual quint64 searchUsers(const QString &query, int limit)
    { Q_UNUSED(query); Q_UNUSED(limit); return 0; }
    // v0.5.11: exact profile lookup for one full Matrix user id. Confirms
    // (or refutes) a bare-localpart candidate the directory may not list.
    virtual quint64 fetchUserProfile(const QString &userId)
    { Q_UNUSED(userId); return 0; }
    // v0.7.4: the signed-in account's OWN display name. Backends that
    // cannot write a profile keep the false default and the UI never
    // offers the affordance — a void call with no answer would otherwise
    // leave the editor spinning forever.
    virtual bool supportsOwnProfileEditing() const { return false; }
    // Set — or, with an EMPTY `name`, CLEAR — the own display name. The op
    // id is the CALLER's (the presence precedent: a void command whose
    // answer is matched by id), so the caller can record it before any
    // answer can arrive. Answers exactly once on ownDisplayNameChanged,
    // including for a synchronous refusal, so the caller never hangs.
    virtual void setOwnDisplayName(const QString &name, quint64 opId)
    { Q_UNUSED(name); Q_UNUSED(opId); }
    // v0.5.12: client-side URL preview (Rust validates and fetches the target;
    // the client never does). Backends without support return 0.
    virtual bool supportsUrlPreview() const { return false; }
    virtual quint64 fetchUrlPreview(const QString &url)
    { Q_UNUSED(url); return 0; }
    // v0.6.1: bounded, redirect-validated HTTPS GET for an external GIF
    // provider. `url` is built by the GIF provider layer and carries the
    // provider API key — callers must treat it as secret and never log it.
    // Backends without support return 0. Result arrives via gifResponse().
    virtual bool supportsGifProvider() const { return false; }
    virtual quint64 gifGet(const QString &url)
    { Q_UNUSED(url); return 0; }
    // v0.6.1: download + validate a provider GIF (result via
    // gifDownloadFinished, bytes included on success). The URL must be a
    // provider-CDN https .gif; Rust re-validates the host + GIF magic bytes.
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
    // v0.7.x room administration. Set ONE member's power level; the SDK
    // preserves every other user's level, including arbitrary custom
    // numbers. Answers on powerLevelChangeFinished. The SERVER enforces
    // permission — the client only avoids offering an action that must fail.
    virtual quint64 setMemberPowerLevel(const QString &roomId,
                                        const QString &userId,
                                        qlonglong level)
    { Q_UNUSED(roomId); Q_UNUSED(userId); Q_UNUSED(level); return 0; }
    // "invite" | "public" | "knock". Rules that carry an allow-rule list
    // (restricted) are deliberately not settable. Answers on
    // roomEditFinished with field "join_rule".
    virtual quint64 setRoomJoinRule(const QString &roomId, const QString &rule)
    { Q_UNUSED(roomId); Q_UNUSED(rule); return 0; }
    // An empty alias clears the canonical alias. Answers on
    // roomEditFinished with field "canonical_alias".
    virtual quint64 setRoomCanonicalAlias(const QString &roomId,
                                          const QString &alias)
    { Q_UNUSED(roomId); Q_UNUSED(alias); return 0; }
    // v0.7.x pinned messages (m.room.pinned_events). Backends without pin
    // support keep the false default; the UI then offers no pin actions and
    // shows no pinned surface, exactly like the thread facepile.
    virtual bool supportsPinnedMessages() const { return false; }
    // Read the room's pinned list and resolve each id into a displayable
    // row. `allowRemote` permits the /state fallback taken only when the
    // room carries no pinned-events state at all. Answers on pinnedReceived.
    virtual quint64 requestPinnedMessages(const QString &roomId,
                                          bool allowRemote)
    { Q_UNUSED(roomId); Q_UNUSED(allowRemote); return 0; }
    virtual quint64 setEventPinned(const QString &roomId,
                                   const QString &eventId, bool pin)
    { Q_UNUSED(roomId); Q_UNUSED(eventId); Q_UNUSED(pin); return 0; }
    // v0.7.x room discovery / join / knock. Backends without support keep
    // the inert defaults; the UI then offers no Discover surface at all.
    virtual bool supportsRoomDiscovery() const { return false; }
    // Resolve user input (#alias, !roomid, matrix: URI, matrix.to
    // permalink) into a normalized join target and preview it where the
    // server allows. Answers on roomTargetResolved. A refused preview is
    // NOT a failed resolution — the target still crosses so Join can be
    // offered.
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
    // v0.7.x personal moderation. Ignore state is the Matrix
    // m.ignored_user_list account data (SDK read-modify-write, never a
    // Lightning-local database); reporting is the stable /v3 event report.
    virtual bool supportsIgnoredUsers() const { return false; }
    virtual quint64 setUserIgnored(const QString &userId, bool ignored)
    { Q_UNUSED(userId); Q_UNUSED(ignored); return 0; }
    virtual quint64 requestIgnoredUsers() { return 0; }
    virtual bool supportsEventReporting() const { return false; }
    virtual quint64 reportMessage(const QString &roomId,
                                  const QString &eventId,
                                  const QString &reason)
    { Q_UNUSED(roomId); Q_UNUSED(eventId); Q_UNUSED(reason); return 0; }
    // 2026-08-18 voice-call signaling pipes (MSC2746 m.call.* v1 + the
    // m.rtc.notification/decline lane). Signaling only: SDP strings are
    // opaque required inputs supplied by a (future) media backend, never
    // logged and never echoed; there is no media transport in the tree.
    // Non-capable backends return 0 (the mock and HTTP backends stay
    // buildable per architecture rule 5 without claiming parity).
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
    // Media-capable mode (2026-08-18 round 2): ONLY when a media backend
    // is registered does the backend carry remote SDP into a bounded
    // C++-memory-only store — production today never enables it, so no
    // SDP crosses the FFI at all. The store is single-shot: take removes.
    // SDP must never reach QML, logs, or persistence.
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

    // MatrixRTC (MSC4143) — modern group/room calling. OBSERVATION and
    // DISCOVERY only in this round: there is no publish/join pipe here at
    // all, because advertising a joinable session without a media transport
    // tells every other client in the room to attempt an SFU connection
    // that cannot complete. That is a lie on the wire, not a stub, and it
    // is the same reason the legacy lane refuses to invite without an
    // engine.
    virtual bool supportsMatrixRtc() const { return false; }
    // Read one room's session. Answers on rtcSessionReceived.
    virtual quint64 rtcSession(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    // Discover usable transports for this account; `roomId` may be empty
    // and, when given, adds the focus the room's participants advertise.
    // Answers on rtcTransportsReceived.
    virtual quint64 rtcTransports(const QString &roomId)
    { Q_UNUSED(roomId); return 0; }
    // Send an org.matrix.msc4075.rtc.notification. Answers on
    // rtcSendFinished.
    // Publish/refresh our own membership; answers rtcMembershipPublished.
    virtual quint64 rtcPublishMembership(const QString &roomId,
                                         const QString &focusUrl,
                                         const QString &intent)
    {
        Q_UNUSED(roomId); Q_UNUSED(focusUrl); Q_UNUSED(intent); return 0;
    }
    /// Restart the server-side delayed retraction so it keeps not firing.
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
    /// LiveKit the frames carry E2EE (Encryption::GCM), which is how a
    /// receiving client decides to decrypt; encrypting the bytes while
    /// declaring NONE renders as garbage at the far end.
    /// `width`/`height` are the video track's declared size, 0 for audio.
    /// Not cosmetic: a video track with no size and no layer leaves the SFU
    /// to infer the track's shape, and it infers three-layer simulcast while
    /// we publish one untagged stream.
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
    // v0.7.x device sign-out through reusable UIA. The flow: deleteDevices
    // → (server may answer with a challenge → uiaRequired) →
    // uiaSubmitPassword / uiaCancel → deviceDeleteFinished. Credentials
    // pass through transiently and are scrubbed; they are never stored,
    // logged, or echoed back.
    virtual bool supportsDeviceDeletion() const { return false; }
    virtual quint64 deleteDevices(const QStringList &deviceIds)
    { Q_UNUSED(deviceIds); return 0; }
    virtual bool uiaSubmitPassword(quint64 uiaId, const QString &password)
    { Q_UNUSED(uiaId); Q_UNUSED(password); return false; }
    virtual void uiaCancel(quint64 uiaId) { Q_UNUSED(uiaId); }
    // MAS/OAuth accounts manage sessions in the account web console
    // instead of password UIA. deviceId "" = sessions list, else that
    // device's delete page. Answers on oauthManagementUrlReceived.
    virtual quint64 requestOAuthManagementUrl(const QString &deviceId)
    { Q_UNUSED(deviceId); return 0; }
    // v0.7.x server-side message search. Covers UNENCRYPTED rooms only —
    // the server cannot search ciphertext, and every UI surface must say
    // so. `roomId` empty = all rooms; `nextBatch` pages. Answers on
    // messageSearchFinished.
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
    // v0.7: MSC1772 child removal (empty-via m.space.child). Never leaves
    // or deletes the child room itself.
    virtual quint64 removeRoomFromSpace(const QString &spaceId,
                                        const QString &roomId)
    { Q_UNUSED(spaceId); Q_UNUSED(roomId); return 0; }
    // 2026-08-19: toggles the MSC1772 `suggested` flag on an EXISTING
    // child (via list and order key preserved; a non-child is refused,
    // never promoted to a child as a side effect).
    virtual quint64 setSpaceChildSuggested(const QString &spaceId,
                                           const QString &roomId,
                                           bool suggested)
    { Q_UNUSED(spaceId); Q_UNUSED(roomId); Q_UNUSED(suggested); return 0; }

    // Attachment sending (Rust: SDK send queue with local echo). `mime` is
    // detected by the caller from file content, not just the extension.
    virtual quint64 sendAttachment(const QString &roomId,
                                   const QString &localPath,
                                   const QString &mime,
                                   const QString &caption,
                                   int width, int height, bool animated)
    {
        Q_UNUSED(roomId); Q_UNUSED(localPath); Q_UNUSED(mime);
        Q_UNUSED(caption); Q_UNUSED(width); Q_UNUSED(height);
        Q_UNUSED(animated);
        return 0;
    }
    // v0.7: video send WITH a poster thumbnail Lightning extracted from the
    // outgoing file. `thumbnail` may be empty (extraction failed or is
    // unsupported) — that is not an error, the video simply goes out
    // without a poster. The SDK uploads and, in encrypted rooms, encrypts
    // the poster itself; nothing here builds thumbnail content by hand.
    // The default degrades to the plain attachment send, so a backend with
    // no thumbnail support keeps sending videos exactly as it did.
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
    // Same payload, but for a room whose live timeline is NOT open. The
    // variant above routes through the open SDK timeline and refuses every
    // other room — right for the composer, fatal for forwarding, whose
    // target is by definition a room the user is not looking at. The SDK
    // still encrypts for the target room when it is encrypted.
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
    // v0.7: MSC3245 voice message. The SDK marks the event as a voice
    // message and carries duration + waveform (0..=100 amplitudes, may be
    // empty) through the normal encrypting attachment path. Result echoes
    // on attachmentQueueFinished by op id.
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
    // v0.7 thread parity: the thread twin of sendVoiceMessage. Carries the
    // SAME MSC3245 metadata, routed through the SDK's thread-focused
    // timeline so the event is a real m.thread reply. Returning 0 (the
    // default, for backends without thread voice support) is a refusal, NOT
    // a licence to fall back to a room send — a thread voice message must
    // never land in the main timeline.
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

    // v0.6.1: attachment sending INTO a thread. Routed through the SDK's
    // thread-focused timeline so the m.thread relation and (in encrypted
    // rooms) encryption are handled by the SDK — never an ordinary room
    // send. Result echoes on attachmentQueueFinished by op id, exactly like
    // the room path. Backends without thread attachment support return 0.
    virtual quint64 sendThreadAttachment(const QString &roomId,
                                         const QString &rootEventId,
                                         const QString &localPath,
                                         const QString &mime,
                                         const QString &caption,
                                         int width, int height, bool animated)
    {
        Q_UNUSED(roomId); Q_UNUSED(rootEventId); Q_UNUSED(localPath);
        Q_UNUSED(mime); Q_UNUSED(caption); Q_UNUSED(width);
        Q_UNUSED(height); Q_UNUSED(animated);
        return 0;
    }
    // v0.7: the thread twin of sendVideo. Same degradation rule — a backend
    // without poster support falls back to the plain thread attachment.
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

    // Media bridge: fetch (and decrypt, inside the SDK) media bytes for a
    // timeline item media key. kind: 0 = full, 1 = thumbnail.
    // timeoutClass (v0.7): 0 = standard, 1 = playable materialization,
    // 2 = explicit Save As — the backend bounds the fetch accordingly.
    virtual quint64 fetchMedia(const QString &mediaKey, int kind,
                               int timeoutClass = 0)
    { Q_UNUSED(mediaKey); Q_UNUSED(kind); Q_UNUSED(timeoutClass); return 0; }
    // Server-side thumbnail of a plain mxc URI (avatars).
    virtual quint64 fetchMxcThumbnail(const QString &mxc, int width, int height)
    { Q_UNUSED(mxc); Q_UNUSED(width); Q_UNUSED(height); return 0; }
    // Cancel an in-flight media fetch. Best-effort and idempotent: backends
    // without cancellation ignore it; no mediaReady/mediaFailed is emitted
    // for a cancelled op (the caller already dropped its bookkeeping).
    virtual void cancelMediaFetch(quint64 opId) { Q_UNUSED(opId); }

    // Server upload limit in bytes; 0 while unknown.
    virtual qint64 maxUploadSize() const { return 0; }

Q_SIGNALS:
    void loginSucceeded(const QString &userId);
    void loginFailed(const QString &reason);
    void loggedOut();
    // Which authentication methods this homeserver actually offers, from the
    // server's own answer. `sso` reports legacy Matrix SSO for honest UI copy
    // only — Lightning cannot perform it (the SDK helper needs the
    // sso-login/local-server features, whose axum dependency is not vendored),
    // so it must never be presented as a usable option.
    void authMethodsDiscovered(const QString &homeserver,
                               bool password,
                               bool oauth,
                               bool sso);
    // The authorization URL to open in the system browser. Contains no
    // credentials — it is the authorization endpoint plus this attempt's
    // public parameters — but it is single-use, so it is not logged.
    void oauthBrowserUrlReady(const QString &url);
    // The homeserver's SSO redirect URL, to open in the system browser.
    // Carries no credential — the login token comes back on the callback —
    // but it is single-use, so it is not logged.
    void ssoBrowserUrlReady(const QString &url);
    // What the server advertises for m.login.sso. `providers` is a list of
    // {id, name, icon} maps; EMPTY with sso true means one unnamed flow, which
    // is the common case. `icon` is an mxc: URI or empty — never an http URL,
    // so the login screen cannot be made to fetch from a server-chosen host.
    void ssoProvidersReceived(const QString &homeserver, bool sso,
                              const QVariantList &providers);

    void connectionStateChanged(ConnectionState state);
    void initialSyncDoneChanged();
    void syncModeChanged();
    void roomsChanged();
    void roomUpdated(const QString &roomId);
    void timelineReset(const QString &roomId);
    // v0.6.0: opening a thread timeline failed (unknown root, build failure,
    // network). Success is signalled by timelineReset(threadTimelineId(...)).
    void threadTimelineFailed(const QString &roomId,
                              const QString &rootEventId,
                              const QString &category);
    // v0.6.0 checkpoint 5. Each thread entry map: rootEventId, rootSender,
    // rootSenderName, rootPreview, rootTimestamp, replyCount, latestSender,
    // latestSenderName, latestPreview, latestTimestamp. Bounded to the pages
    // fetched so far.
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

    // v0.3.
    void eventReplaced(const QString &roomId,
                       const QString &oldEventId,
                       const TimelineEvent &newEvent);

    // v0.5.7: index-based diff signals for backends whose timeline is a
    // mirrored SDK vector (RustSdkMatrixClient). Indices refer to the
    // backend's timeline(roomId) list AFTER the operation was applied to
    // it; TimelineModel validates them again defensively before mutating
    // its copy.
    void eventInsertedAt(const QString &roomId, int index,
                         const TimelineEvent &event);
    // A contiguous run of SDK inserts applied as one model transaction. The
    // Rust SDK commonly emits backward pagination as many one-item `Insert`
    // diffs (immediately after its timeline-start sentinel); exposing the
    // final contiguous range prevents the UI from laying out once per item.
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
    // A sync m.room.member event was seen for this room (join/leave/kick/
    // ban/invite AND every display-name or avatar change). Distinct from
    // membersChanged on purpose (review H1): membersChanged means "a
    // member SNAPSHOT landed" and drives presentation refreshes
    // (TimelineModel dirties every loaded row on it); this poke can fire
    // per member event in a busy bridged room and must only reach the
    // roster REFETCH consumers, whose pending-op guards bound the work.
    void roomMemberEventSeen(const QString &roomId);

    // Server-reported per-room notification mode (0/1/2 as above).
    // userDefined is true for an explicit per-room rule, false when the
    // report is the account default resolved for this room. Carries mode
    // integers and the room id only — never push-rule JSON.
    void roomNotificationModeChanged(const QString &roomId, int mode,
                                     bool userDefined);
    // A server push-rule write for this room failed: the device-local mode
    // is kept and the UI must say so instead of claiming the mode was
    // saved to the account. Room id only — no error text, no rule JSON.
    void roomNotificationModeWriteFailed(const QString &roomId);
    // v0.7: the room's user-defined push rules were successfully REMOVED —
    // it now follows the account default. Deliberately separate from
    // roomNotificationModeChanged: that signal carries a rule's value, and
    // this outcome is the absence of a rule. It is the acknowledgement that
    // retires a failed "follow account default" write.
    void roomNotificationModeCleared(const QString &roomId);

    void errorOccurred(const QString &message);

    // ---- v0.5.9 async command results. Every payload is non-secret:
    // categories are coarse ("network", "forbidden", "rate_limited", ...)
    // and no message body, token, key material, or local path is carried.
    void userSearchFinished(quint64 opId, bool ok,
                            const QVariantList &results, bool limited,
                            const QString &category);
    // v0.5.11: exact profile lookup result. ok=false with category
    // "not_found" means the homeserver does not know the user; other
    // categories are transient ("network", "rate_limited", ...).
    void userProfileFinished(quint64 opId, bool ok, const QString &userId,
                             const QString &displayName,
                             const QString &avatarUrl,
                             const QString &category);
    // v0.7.4: terminal answer for setOwnDisplayName(), matched by the
    // caller's op id. `error` is the SERVER's own sanitized sentence when
    // it sent one, and EMPTY when it did not (a timeout, a transport
    // failure, or a synchronous refusal) — the presentation layer supplies
    // the wording for that case rather than inventing a server message.
    // The name itself is never carried back: the caller already holds it.
    void ownDisplayNameChanged(quint64 opId, bool ok, const QString &error);
    // v0.5.11: URL-preview result. `fields` carries only whitelisted
    // OpenGraph values (title, description, siteName, imageMxc, imageMime,
    // imageWidth, imageHeight, imageSize) — never the requested URL.
    // v0.5.14: httpStatus/redirectCount are sanitized failure diagnostics
    // (0 when not applicable, e.g. a DNS/timeout failure with no response
    // at all) — enough to tell a code regression from live remote policy
    // without ever logging the URL, query string, or response body.
    void urlPreviewFinished(quint64 opId, bool ok, const QVariantMap &fields,
                            const QString &category, int httpStatus = 0,
                            int redirectCount = 0);
    // v0.6.1: one external GIF-provider response. `body` is the bounded JSON
    // text (parsed by the GIF controller into safe structs — never surfaced to
    // QML); empty on failure. `category` is a coarse safe state
    // (ok/rate_limited/provider_error/timeout/network/too_large/blocked). The
    // request URL (which carries the provider key) is never emitted or logged.
    void gifResponse(quint64 opId, bool ok, int httpStatus,
                     const QByteArray &body, const QString &category);
    // v0.6.1: a validated GIF download. `bytes` is the real GIF on success
    // (empty on failure); `category` is a coarse safe reason on failure
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
    // v0.7: real thread participants for the summary-card facepile.
    // `participants` is an ordered, user-id-deduplicated QVariantList of
    // maps (userId, displayName, avatarUrl) — root sender first, then
    // first-appearance order. `distinct` is the number of DISTINCT senders
    // found (never the reply count); `truncated` is true when more exist
    // than were sent. An unsuccessful lookup arrives with an empty list and
    // distinct 0, and must be treated as "unknown", never as "nobody".
    void threadParticipantsReceived(const QString &roomId,
                                    const QString &rootEventId,
                                    const QVariantList &participants,
                                    int distinct, bool truncated);
    // 2026-08-18 "Remove edits" result. Counts only: `removed` edits were
    // redacted, `failed` were refused by the server, `truncated` means the
    // chain was longer than one pass removes. ok == "nothing failed", which
    // is not the same as "something was removed" — a message with no edits
    // reports ok with removed 0, and the UI must not claim otherwise.
    void messageEditsRemoved(const QString &roomId, const QString &eventId,
                             bool ok, int removed, int failed,
                             bool truncated);
    // v0.7.x Matrix presence: one polling round's answers. `entries` is a
    // QVariantList of maps — userId, ok, state ("online" / "unavailable" /
    // "offline" / "unknown"), currentlyActive, lastActiveAgoMs (qlonglong,
    // -1 when the server sent none), category (coarse, on ok=false only).
    // An entry with ok=false means UNKNOWN for that user, never offline.
    void presenceReceived(quint64 opId, const QVariantList &entries);
    // A user's profile banner. `supported` false means the HOMESERVER does
    // not do extended profile fields — a different fact from "this user has
    // no banner", and one that must render as nothing rather than as an
    // absence the client is sure about.
    void profileBannerReceived(quint64 opId, const QString &userId,
                               const QString &mxc, bool supported);
    void profileBannerSet(quint64 opId, bool ok, const QString &mxc,
                          const QString &category);
    // A room's banner, and whether THIS account may change it — the room's
    // own required power level for the event, asked of the SDK.
    void roomBannerReceived(quint64 opId, const QString &roomId,
                            const QString &mxc, bool canSet);
    void roomBannerSet(quint64 opId, const QString &roomId, bool ok,
                       const QString &mxc, const QString &category);
    // Publishing the local user's own presence failed (coarse category).
    // Informational: PresenceManager uses it only for bounded diagnostics.
    void presencePublishFailed(const QString &category);
    void roomEditFinished(quint64 opId, const QString &roomId,
                          const QString &field, bool ok,
                          const QString &category);
    void roomLeaveFinished(quint64 opId, const QString &roomId, bool ok,
                           const QString &category);
    // op is "kick" or "ban"; category is a sanitized error class on failure.
    void moderationFinished(quint64 opId, const QString &roomId,
                            const QString &userId, const QString &op,
                            bool ok, const QString &category);
    // v0.7.x room administration: one member's power-level write completed.
    // `level` echoes what was REQUESTED, never what the room now holds —
    // the authoritative value comes from the roster refresh that follows.
    void powerLevelChangeFinished(quint64 opId, const QString &roomId,
                                  const QString &userId, qlonglong level,
                                  bool ok, const QString &category);
    // v0.7.x pinned messages: one resolved snapshot. `snapshot` carries
    // ok, canPin, total, truncated and entries (a QVariantList of maps —
    // eventId, available, and when available sender, senderDisplayName,
    // senderAvatarUrl, timestampMs, kind, preview). Entry previews are
    // decrypted message text in an encrypted room: memory only, never
    // CacheStore.
    void pinnedReceived(quint64 opId, const QString &roomId,
                        const QVariantMap &snapshot);
    // A pin/unpin write completed. `changed` is false for a no-op (the
    // event was already in the requested state) — reported as a no-op
    // rather than as a success that did something.
    void pinChangeFinished(quint64 opId, const QString &roomId,
                           const QString &eventId, bool pin, bool ok,
                           bool changed, const QString &category);
    // Sync saw m.room.pinned_events change in this room (another client,
    // or another of this user's devices). Carries no payload: consumers
    // re-read the authoritative list through requestPinnedMessages.
    void pinnedEventsChanged(const QString &roomId);
    // v0.7.x discovery. `result` carries ok / category and, when ok:
    // target, via (QStringList), eventId, previewOk and — when previewed —
    // roomId, alias, name, topic, avatarUrl, members, joinRule, membership,
    // isSpace (else previewCategory).
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
    // v0.7.x personal moderation results. `ignored` echoes the requested
    // direction; ignoredUsersChanged is the sync push (local AND remote
    // changes — both converge on a re-read by the consumer).
    void ignoreUserFinished(quint64 opId, const QString &userId, bool ignored,
                            bool ok, const QString &category);
    void ignoredUsersReceived(quint64 opId, bool ok, const QStringList &users);
    void ignoredUsersChanged(const QStringList &users);
    void reportMessageFinished(quint64 opId, const QString &roomId,
                               const QString &eventId, bool ok,
                               const QString &category);
    // 2026-08-18 voice-call signaling: one inbound observation (SDP-free —
    // see CallSignal.h) and one terminal send result per dispatched op.
    void callSignalReceived(const CallSignal &signal);
    void callSendFinished(quint64 opId, bool ok, const QString &category,
                          const QString &callId, const QString &eventId);
    // Remote trickled ICE (media-capable mode only; entries as above).
    // Pure transport data for the engine: never logged, never rendered.
    void callCandidatesReceived(const QString &roomId, const QString &callId,
                                const QString &partyId, bool own,
                                const QVariantList &candidates);
    // Short-lived TURN credentials: engine-only, never logged.
    void callTurnServersReceived(quint64 opId, bool ok,
                                 const QString &username,
                                 const QString &password,
                                 const QStringList &uris, qint64 ttlSeconds,
                                 const QString &category);

    // MatrixRTC observation. `rtcSessionChanged` is a payload-free poke: a
    // membership in that room changed and the session should be re-read, so
    // remote and local changes converge on ONE parse path (the
    // roomPinnedChanged precedent). `rtcSessionReceived` is the answer to a
    // read, whether we asked or a poke prompted it.
    void rtcSessionReceived(quint64 opId, const RtcSessionData &session);
    void rtcSessionChanged(const QString &roomId);
    // Discovery. `serverAnswered` false with a category distinguishes "this
    // homeserver has no MatrixRTC" from "the request failed", which the UI
    // must not conflate. URLs are opaque and must not be rendered raw.
    void rtcTransportsReceived(quint64 opId, bool serverAnswered,
                               const QString &category,
                               const QStringList &serverServiceUrls,
                               const QString &participantFocusUrl);
    void rtcSendFinished(quint64 opId, bool ok, const QString &category,
                         const QString &eventId);
    /// Our membership was published. `delayId` empty means the server has no
    /// MSC4140, so cleanup falls back to the membership's own `expires`.
    void rtcMembershipPublished(quint64 opId, bool ok, const QString &category,
                                const QString &eventId,
                                const QString &delayId);
    void rtcMembershipRetracted(quint64 opId, bool ok,
                                const QString &category);
    void rtcMediaKeySent(quint64 opId, bool ok, const QString &category,
                         int delivered, int keyIndex);
    /// A media key from another device, already Olm-decrypted. `sender` is
    /// what the SDK vouches for; `claimedDeviceId` is a CLAIM. The key is
    /// base64 raw bytes: C++ memory only, never QML, never logged.
    void rtcMediaKeyReceived(const QString &roomId, const QString &sender,
                             const QString &claimedDeviceId, int keyIndex,
                             const QString &keyBase64);

    // ── SFU signalling ──
    /// Closed-set lifecycle: authorized / signalling / ended / closed /
    /// failed. `category` explains a failure and is safe to log.
    void sfuStateChanged(const QString &state, const QString &category);
    void sfuJoined(const QString &identity, const QVariantList &participants,
                   const QVariantList &iceServers);
    void sfuParticipantsChanged(const QVariantList &participants);
    void sfuTrackPublished(const QString &cid, const QString &sid);
    void sfuSpeakersChanged(const QVariantList &speakers);
    void sfuConnectionQuality(const QVariantList &updates);
    /// Media transport only; never logged, never exposed to QML.
    void sfuRemoteDescription(const QString &kind, const QString &target,
                              const QString &sdp);
    void sfuRemoteCandidate(const QString &target,
                            const QString &candidateInit);
    // v0.7.x UIA: the server requires interactive auth before the pending
    // privileged operation completes. `stages` carries the flow stage
    // names for the honest "unsupported stage" surface; only the password
    // stage is renderable today. wrongPassword = a previous answer was
    // rejected (offer retry).
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
    // Queue acceptance only — delivery state flows through the timeline
    // item's send state like any other local echo.
    void attachmentQueueFinished(quint64 opId, const QString &roomId,
                                 bool ok, const QString &category);
    void mediaReady(quint64 opId, const QString &mediaKey, int kind,
                    const QByteArray &bytes, const QString &mimetype,
                    const QString &filename);
    void mediaFailed(quint64 opId, const QString &mediaKey, int kind,
                     const QString &category);
    void maxUploadSizeChanged();
};
