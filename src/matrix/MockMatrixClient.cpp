#include "matrix/MockMatrixClient.h"

#include "matrix/MediaHelpers.h"

#include <QDateTime>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

MockMatrixClient::MockMatrixClient(QObject *parent)
    : MatrixClient(parent)
{
    seedMockData();
}

void MockMatrixClient::login(const QString &homeserver,
                             const QString &user,
                             const QString &password)
{
    setState(Connecting);
    m_homeserver = homeserver.isEmpty()
        ? QStringLiteral("https://mock.local") : homeserver;
    const QString localpart = user.isEmpty() ? QStringLiteral("alice") : user;
    QString host = QUrl(m_homeserver).host();
    if (host.isEmpty())
        host = QStringLiteral("mock.local");
    m_userId = QStringLiteral("@%1:%2").arg(localpart, host);
    Q_UNUSED(password);

    QTimer::singleShot(120, this, [this] {
        m_loggedIn = true;
        Q_EMIT loginSucceeded(m_userId);
        setState(Disconnected);
    });
}

void MockMatrixClient::logout()
{
    stopSync();
    m_loggedIn = false;
    m_userId.clear();
    Q_EMIT loggedOut();
}

bool MockMatrixClient::restoreSession()
{
    return false;
}

void MockMatrixClient::startSync()
{
    if (!m_loggedIn)
        return;
    setState(Syncing);
    Q_EMIT roomsChanged();
    for (const auto &r : m_rooms)
        Q_EMIT timelineReset(r.id);
}

void MockMatrixClient::stopSync()
{
    setState(Disconnected);
}

QList<RoomInfo> MockMatrixClient::rooms() const
{
    return m_rooms;
}

QList<TimelineEvent> MockMatrixClient::timeline(const QString &roomId) const
{
    return m_timelines.value(roomId);
}

QString MockMatrixClient::displayNameFor(const QString &roomId,
                                         const QString &userId) const
{
    for (const auto &r : m_rooms) {
        if (r.id != roomId) continue;
        const auto it = r.members.constFind(userId);
        if (it != r.members.constEnd() && !it->displayName.isEmpty())
            return it->displayName;
    }
    return userId;
}

QString MockMatrixClient::avatarMxcFor(const QString &roomId,
                                       const QString &userId) const
{
    for (const auto &r : m_rooms) {
        if (r.id != roomId) continue;
        const auto it = r.members.constFind(userId);
        if (it != r.members.constEnd())
            return it->avatarMxcUrl;
    }
    return {};
}

QStringList MockMatrixClient::typingUsersFor(const QString &roomId) const
{
    for (const auto &r : m_rooms)
        if (r.id == roomId)
            return r.typingUserIds;
    return {};
}

QUrl MockMatrixClient::mediaDownloadUrl(const QString &mxcUrl) const
{
    return matrix::media::downloadUrl(m_homeserver, mxcUrl);
}

QUrl MockMatrixClient::mediaThumbnailUrl(const QString &mxcUrl,
                                         int width, int height,
                                         bool crop) const
{
    return matrix::media::thumbnailUrl(m_homeserver, mxcUrl, width, height, crop);
}

TimelineEvent *MockMatrixClient::findEvent(const QString &roomId,
                                           const QString &eventId)
{
    auto it = m_timelines.find(roomId);
    if (it == m_timelines.end())
        return nullptr;
    for (auto &e : *it) {
        if (e.eventId == eventId)
            return &e;
    }
    return nullptr;
}

void MockMatrixClient::ackAfter(int ms, const QString &roomId,
                                const QString &eventId)
{
    QTimer::singleShot(ms, this, [this, roomId, eventId] {
        if (auto *ev = findEvent(roomId, eventId)) {
            ev->status = TimelineEvent::Sent;
            Q_EMIT eventStatusChanged(roomId, eventId, TimelineEvent::Sent);
        }
    });
}

void MockMatrixClient::sendTextMessage(const QString &roomId, const QString &body)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }

    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = body;
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::TextMessage;
    ev.status            = TimelineEvent::Sending;

    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(150, roomId, ev.eventId);

    for (auto &room : m_rooms) {
        if (room.id == roomId) {
            room.lastMessagePreview = matrix::media::previewSnippet(body);
            room.lastActivity = ev.timestamp;
            Q_EMIT roomUpdated(roomId);
            break;
        }
    }
}

void MockMatrixClient::sendReply(const QString &roomId,
                                  const QString &replyToEventId,
                                  const QString &body)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = body;
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::TextMessage;
    ev.status            = TimelineEvent::Sending;
    ev.replyToEventId    = replyToEventId;
    if (auto *target = findEvent(roomId, replyToEventId)) {
        ev.replyToSender  = target->senderDisplayName.isEmpty()
                              ? target->sender : target->senderDisplayName;
        ev.replyToPreview = matrix::media::previewSnippet(target->body);
    }
    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(150, roomId, ev.eventId);
}

void MockMatrixClient::sendThreadReply(const QString &roomId,
                                        const QString &threadRootEventId,
                                        const QString &body)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = body;
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::TextMessage;
    ev.status            = TimelineEvent::Sending;
    ev.threadRootId      = threadRootEventId;
    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(150, roomId, ev.eventId);
}

void MockMatrixClient::editMessage(const QString &roomId,
                                    const QString &targetEventId,
                                    const QString &newBody)
{
    auto *ev = findEvent(roomId, targetEventId);
    if (!ev) {
        Q_EMIT errorOccurred(tr("Cannot edit: original message not found."));
        return;
    }
    ev->body   = newBody;
    ev->edited = true;
    Q_EMIT eventEdited(roomId, targetEventId);

    for (auto &room : m_rooms) {
        if (room.id == roomId && !m_timelines[roomId].isEmpty() &&
            m_timelines[roomId].last().eventId == targetEventId) {
            room.lastMessagePreview = matrix::media::previewSnippet(newBody);
            Q_EMIT roomUpdated(roomId);
            break;
        }
    }
}

void MockMatrixClient::redactEvent(const QString &roomId,
                                    const QString &eventId,
                                    const QString &reason)
{
    Q_UNUSED(reason);
    auto *ev = findEvent(roomId, eventId);
    if (!ev) return;
    ev->redacted = true;
    ev->body.clear();
    // If it was a reaction, remove from parent.
    // Mock keeps reactions on their target event, so no removal needed by id.
    Q_EMIT eventRedacted(roomId, eventId);
}

void MockMatrixClient::toggleReaction(const QString &roomId,
                                       const QString &targetEventId,
                                       const QString &key)
{
    auto *ev = findEvent(roomId, targetEventId);
    if (!ev) return;
    bool matched = false;
    for (auto &r : ev->reactions) {
        if (r.key == key) {
            matched = true;
            if (r.byMe) {
                r.count = qMax(0, r.count - 1);
                r.byMe  = false;
                r.myEventId.clear();
            } else {
                r.count += 1;
                r.byMe   = true;
                r.myEventId = QStringLiteral("$mock-rx-%1").arg(nextTxnId());
            }
            break;
        }
    }
    if (!matched) {
        Reaction r;
        r.key = key;
        r.count = 1;
        r.byMe = true;
        r.myEventId = QStringLiteral("$mock-rx-%1").arg(nextTxnId());
        ev->reactions.append(r);
    }
    // Drop zero-count reactions.
    QList<Reaction> filtered;
    filtered.reserve(ev->reactions.size());
    for (const auto &r : ev->reactions)
        if (r.count > 0) filtered.append(r);
    ev->reactions = filtered;
    Q_EMIT reactionsChanged(roomId, targetEventId);
}

void MockMatrixClient::sendTyping(const QString &roomId, bool isTyping, int)
{
    Q_UNUSED(roomId);
    Q_UNUSED(isTyping);
    // Mock: swallow (no-op).
}

void MockMatrixClient::sendReadReceipt(const QString &roomId, const QString &eventId)
{
    Q_UNUSED(roomId);
    Q_UNUSED(eventId);
    // Mock: swallow.
}

void MockMatrixClient::sendImage(const QString &roomId, const QString &localPath)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    QFileInfo fi(localPath);
    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = fi.fileName().isEmpty() ? tr("Image") : fi.fileName();
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::Image;
    ev.status            = TimelineEvent::Sending;
    ev.mediaMxcUrl       = QStringLiteral("mxc://mock.local/mock-image-%1")
                               .arg(m_eventCounter);
    ev.mediaMimetype     = QStringLiteral("image/png");
    ev.mediaFilename     = ev.body;
    ev.mediaSize         = fi.size();
    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(200, roomId, ev.eventId);
}

void MockMatrixClient::sendFile(const QString &roomId, const QString &localPath)
{
    if (!m_timelines.contains(roomId)) {
        Q_EMIT errorOccurred(tr("Unknown room: %1").arg(roomId));
        return;
    }
    QFileInfo fi(localPath);
    TimelineEvent ev;
    ev.eventId           = nextEventId();
    ev.roomId            = roomId;
    ev.sender            = m_userId;
    ev.senderDisplayName = QStringLiteral("You");
    ev.body              = fi.fileName().isEmpty() ? tr("File") : fi.fileName();
    ev.timestamp         = QDateTime::currentDateTimeUtc();
    ev.type              = TimelineEvent::File;
    ev.status            = TimelineEvent::Sending;
    ev.mediaMxcUrl       = QStringLiteral("mxc://mock.local/mock-file-%1")
                               .arg(m_eventCounter);
    ev.mediaMimetype     = matrix::media::mimetypeForFile(localPath);
    ev.mediaFilename     = ev.body;
    ev.mediaSize         = fi.size();
    m_timelines[roomId].append(ev);
    Q_EMIT eventAppended(roomId, ev);
    ackAfter(200, roomId, ev.eventId);
}

void MockMatrixClient::loadOlderMessages(const QString &roomId)
{
    if (!m_paginationRemaining.contains(roomId)) return;
    if (m_paginating.contains(roomId)) return;
    m_paginationFailed.remove(roomId);
    if (m_failNextPagination) {
        m_failNextPagination = false;
        m_paginationFailed.insert(roomId);
        Q_EMIT paginationStateChanged(roomId);
        return;
    }
    if (m_paginationRemaining[roomId] <= 0) {
        for (auto &r : m_rooms)
            if (r.id == roomId) r.paginationExhausted = true;
        Q_EMIT paginationStateChanged(roomId);
        return;
    }
    m_paginating.insert(roomId);
    Q_EMIT paginationStateChanged(roomId);

    QTimer::singleShot(300, this, [this, roomId] {
        m_paginating.remove(roomId);
        int &remaining = m_paginationRemaining[roomId];
        if (remaining <= 0) {
            for (auto &r : m_rooms)
                if (r.id == roomId) r.paginationExhausted = true;
            Q_EMIT paginationStateChanged(roomId);
            return;
        }
        // Prepend a small chunk of synthetic older events.
        const auto &existing = m_timelines[roomId];
        QDateTime start = existing.isEmpty()
            ? QDateTime::currentDateTimeUtc().addSecs(-3600)
            : existing.first().timestamp.addSecs(-60);

        QList<TimelineEvent> chunk;
        for (int i = 0; i < 3; ++i) {
            TimelineEvent e;
            e.eventId           = nextEventId();
            e.roomId            = roomId;
            e.sender            = QStringLiteral("@history-bot:mock.local");
            e.senderDisplayName = QStringLiteral("History Bot");
            e.body              = tr("Older message #%1 (page %2)")
                                      .arg(3 - i).arg(remaining);
            e.timestamp         = start.addSecs(-i * 60);
            e.type              = TimelineEvent::TextMessage;
            e.status            = TimelineEvent::Sent;
            chunk.prepend(e);
        }
        auto &tl = m_timelines[roomId];
        for (int i = chunk.size() - 1; i >= 0; --i)
            tl.prepend(chunk.at(i));
        Q_EMIT eventsPrepended(roomId, chunk);
        remaining -= 1;
        if (remaining <= 0) {
            for (auto &r : m_rooms)
                if (r.id == roomId) r.paginationExhausted = true;
        }
        Q_EMIT paginationStateChanged(roomId);
    });
}

bool MockMatrixClient::canPaginate(const QString &roomId) const
{
    return m_paginationRemaining.value(roomId, 0) > 0;
}

bool MockMatrixClient::paginating(const QString &roomId) const
{
    return m_paginating.contains(roomId);
}

void MockMatrixClient::seedMockData()
{
    const auto now = QDateTime::currentDateTimeUtc();

    auto member = [](const QString &id, const QString &name) {
        MemberInfo m; m.userId = id; m.displayName = name; return m;
    };

    RoomInfo general;
    general.id                 = QStringLiteral("!general:mock.local");
    general.name               = QStringLiteral("General");
    general.topic              = QStringLiteral("Casual chat");
    general.lastMessagePreview = QStringLiteral("Welcome to the mock backend!");
    general.lastActivity       = now.addSecs(-60);
    general.unreadCount        = 2;
    general.members.insert(QStringLiteral("@alice:mock.local"),
                           member(QStringLiteral("@alice:mock.local"),
                                  QStringLiteral("Alice")));
    general.members.insert(QStringLiteral("@bob:mock.local"),
                           member(QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("Bob")));
    general.members.insert(QStringLiteral("@carol:mock.local"),
                           member(QStringLiteral("@carol:mock.local"),
                                  QStringLiteral("Carol")));
    general.typingUserIds << QStringLiteral("@bob:mock.local");

    RoomInfo devs;
    devs.id                 = QStringLiteral("!devs:mock.local");
    devs.name               = QStringLiteral("Developers");
    devs.topic              = QStringLiteral("Client development discussion");
    devs.lastMessagePreview = QStringLiteral("Timeline model wiring works.");
    devs.lastActivity       = now.addSecs(-300);
    devs.encrypted          = true;

    RoomInfo dm;
    dm.id                 = QStringLiteral("!dm-bob:mock.local");
    // Named explicitly (rather than left to compute from the other
    // member) so the mock backend also exercises "explicit room name must
    // not disable member-avatar derivation".
    dm.name               = QStringLiteral("Bob");
    dm.topic              = QStringLiteral("Direct message");
    dm.lastMessagePreview = QStringLiteral("See you tomorrow.");
    dm.lastActivity       = now.addSecs(-3600);
    dm.isDirect           = true;
    dm.directUserId       = QStringLiteral("@bob:mock.local");
    dm.directUserIds      = { dm.directUserId };
    dm.members.insert(QStringLiteral("@bob:mock.local"),
                      member(QStringLiteral("@bob:mock.local"),
                             QStringLiteral("Bob")));

    // v0.4.1: one Space grouping general + devs. `dm` stays outside any
    // Space so QML can also render an "Other rooms" row.
    RoomInfo team;
    team.id                 = QStringLiteral("!space-team:mock.local");
    team.name               = QStringLiteral("Team");
    team.topic              = QStringLiteral("Mock Space containing General and Developers");
    team.isSpace            = true;
    team.childRoomIds       = { general.id, devs.id };
    team.lastActivity       = now.addSecs(-60);
    general.spaceId         = team.id;
    devs.spaceId            = team.id;

    m_rooms = { team, general, devs, dm };
    m_paginationRemaining[general.id] = 2;
    m_paginationRemaining[devs.id]    = 1;
    m_paginationRemaining[dm.id]      = 0;

    auto makeEvent = [this, &now](const QString &roomId, const QString &sender,
                                   const QString &name, const QString &body,
                                   int secondsAgo) {
        TimelineEvent e;
        e.eventId           = nextEventId();
        e.roomId            = roomId;
        e.sender            = sender;
        e.senderDisplayName = name;
        e.body              = body;
        e.timestamp         = now.addSecs(-secondsAgo);
        e.type              = TimelineEvent::TextMessage;
        e.status            = TimelineEvent::Sent;
        return e;
    };

    auto ev1 = makeEvent(general.id, QStringLiteral("@alice:mock.local"), "Alice",
                         QStringLiteral("Welcome to the mock backend!"), 600);
    auto ev2 = makeEvent(general.id, QStringLiteral("@bob:mock.local"), "Bob",
                         QStringLiteral("Everything you see here is fake."), 540);
    auto ev3 = makeEvent(general.id, QStringLiteral("@carol:mock.local"), "Carol",
                         QStringLiteral("Real Matrix arrives in v0.2."), 480);
    // A reply
    auto ev4 = makeEvent(general.id, QStringLiteral("@bob:mock.local"), "Bob",
                         QStringLiteral("Nice — replies show up as threaded previews."),
                         420);
    ev4.replyToEventId = ev1.eventId;
    ev4.replyToSender  = QStringLiteral("Alice");
    ev4.replyToPreview = QStringLiteral("Welcome to the mock backend!");
    // An edited message
    auto ev5 = makeEvent(general.id, QStringLiteral("@alice:mock.local"), "Alice",
                         QStringLiteral("This message was edited (v0.3)."), 360);
    ev5.edited = true;
    // A redacted message
    auto ev6 = makeEvent(general.id, QStringLiteral("@carol:mock.local"), "Carol",
                         QStringLiteral(""), 300);
    ev6.redacted = true;
    // An image
    auto ev7 = makeEvent(general.id, QStringLiteral("@alice:mock.local"), "Alice",
                         QStringLiteral("cat.png"), 240);
    ev7.type          = TimelineEvent::Image;
    ev7.mediaMxcUrl   = QStringLiteral("mxc://mock.local/cat");
    ev7.mediaMimetype = QStringLiteral("image/png");
    ev7.mediaFilename = QStringLiteral("cat.png");
    ev7.mediaSize     = 45123;
    // A file
    auto ev8 = makeEvent(general.id, QStringLiteral("@bob:mock.local"), "Bob",
                         QStringLiteral("notes.pdf"), 180);
    ev8.type          = TimelineEvent::File;
    ev8.mediaMxcUrl   = QStringLiteral("mxc://mock.local/notes");
    ev8.mediaMimetype = QStringLiteral("application/pdf");
    ev8.mediaFilename = QStringLiteral("notes.pdf");
    ev8.mediaSize     = 82344;
    // Reactions on ev2
    Reaction rx1; rx1.key = QStringLiteral("👍"); rx1.count = 2;
    Reaction rx2; rx2.key = QStringLiteral("❤️"); rx2.count = 1;
    ev2.reactions = { rx1, rx2 };
    // A thread root + two mock replies inside the thread.
    auto evThreadRoot = makeEvent(general.id, QStringLiteral("@carol:mock.local"), "Carol",
                                  QStringLiteral("Anyone up for a mock deploy tomorrow?"), 220);
    auto evThreadReply1 = makeEvent(general.id, QStringLiteral("@alice:mock.local"), "Alice",
                                    QStringLiteral("Sure, morning works."), 190);
    evThreadReply1.threadRootId = evThreadRoot.eventId;
    auto evThreadReply2 = makeEvent(general.id, QStringLiteral("@bob:mock.local"), "Bob",
                                    QStringLiteral("I'll take the afternoon slot."), 160);
    evThreadReply2.threadRootId = evThreadRoot.eventId;
    // Latest message
    auto ev9 = makeEvent(general.id, QStringLiteral("@alice:mock.local"), "Alice",
                         QStringLiteral("Type something below and press Send."), 60);

    m_timelines[general.id] = { ev1, ev2, ev3, ev4, ev5, ev6, ev7, ev8,
                                evThreadRoot, evThreadReply1, evThreadReply2, ev9 };

    // Two consecutive room-activity (state-change) events after the last
    // message — matches what the Rust bridge produces for membership/
    // profile/room-settings changes, and lets the compact Expand/Collapse
    // UI be exercised (clicked, keyboard-activated) without a live server.
    auto stateEvent = [this, &now](const QString &roomId, const QString &stateKind,
                                    const QString &body, int secondsAgo) {
        TimelineEvent e;
        e.eventId    = nextEventId();
        e.roomId     = roomId;
        e.type       = TimelineEvent::StateChange;
        e.stateKind  = stateKind;
        e.body       = body;
        e.timestamp  = now.addSecs(-secondsAgo);
        return e;
    };

    auto decrypted = makeEvent(
        devs.id, QStringLiteral("@dave:mock.local"), "Dave",
        QStringLiteral("This fixture represents decrypted encrypted text."), 900);
    decrypted.isEncrypted = true;
    decrypted.isDecrypted = true;

    auto undecryptable = makeEvent(
        devs.id, QStringLiteral("@unknown:mock.local"), QString{},
        QStringLiteral("Unable to decrypt this message"), 700);
    undecryptable.isEncrypted = true;
    undecryptable.undecryptable = true;
    undecryptable.errorKind = QStringLiteral("session_missing");

    auto missingReply = makeEvent(
        devs.id, QStringLiteral("@eve:mock.local"), "Eve",
        QStringLiteral("Reply target is intentionally unavailable."), 600);
    missingReply.isEncrypted = true;
    missingReply.isDecrypted = true;
    missingReply.replyToEventId = QStringLiteral("$missing:mock.local");

    auto pendingMedia = makeEvent(
        devs.id, QStringLiteral("@frank:mock.local"), "Frank",
        QStringLiteral("encrypted-image.png"), 500);
    pendingMedia.type = TimelineEvent::Image;
    pendingMedia.isEncrypted = true;
    pendingMedia.isDecrypted = true;
    pendingMedia.mediaMimetype = QStringLiteral("image/png");
    pendingMedia.mediaFilename = pendingMedia.body;
    pendingMedia.mediaKey = QStringLiteral("mock-pending-encrypted-media");
    pendingMedia.mediaSourceAvailable = false;

    auto longDecrypted = makeEvent(
        devs.id, QStringLiteral("@dave:mock.local"), "Dave",
        QStringLiteral("Large encrypted timeline fixture line.\n").repeated(128),
        100);
    longDecrypted.isEncrypted = true;
    longDecrypted.isDecrypted = true;

    m_timelines[devs.id] = {
        decrypted,
        undecryptable,
        missingReply,
        pendingMedia,
        stateEvent(devs.id, QStringLiteral("membership"),
                   QStringLiteral("Grace joined the room."), 200),
        stateEvent(devs.id, QStringLiteral("member_profile"),
                   QStringLiteral("Grace changed their display name."), 190),
        longDecrypted,
    };

    m_timelines[dm.id] = {
        makeEvent(dm.id, QStringLiteral("@bob:mock.local"), "Bob",
                  QStringLiteral("Hey, are you around?"), 7200),
        makeEvent(dm.id, QStringLiteral("@alice:mock.local"), "Alice",
                  QStringLiteral("Just for a bit."), 6900),
        makeEvent(dm.id, QStringLiteral("@bob:mock.local"), "Bob",
                  QStringLiteral("See you tomorrow."), 3600),
    };
}

void MockMatrixClient::setState(ConnectionState s)
{
    if (m_state == s)
        return;
    m_state = s;
    Q_EMIT connectionStateChanged(m_state);
}

QString MockMatrixClient::nextEventId()
{
    return QStringLiteral("$mock-%1:mock.local").arg(++m_eventCounter);
}

QString MockMatrixClient::nextTxnId()
{
    return QStringLiteral("mock-txn-%1").arg(++m_txnCounter);
}
