#include "notifications/NotificationContextBuilder.h"

namespace lightning::notifications {

NotificationManager::Context
NotificationContextBuilder::build(const NotificationContextInputs &in)
{
    NotificationManager::Context context;
    context.selfUserId = in.selfUserId;
    context.roomName = in.roomInfoName.isEmpty() ? in.roomId : in.roomInfoName;
    context.roomIsDirect = in.roomIsDirect;
    context.avatarMxc = in.avatarMxc;
    // Initials colour and theme from the same row the room list uses, so the
    // two never disagree about an identity.
    context.avatarColorKey = in.avatarColorKey;
    context.themeId = in.themeId;
    context.roomMode = static_cast<NotificationManager::RoomMode>(in.roomMode);
    // The server does not have this device's choice (a failed write, or a
    // device-only legacy mode); the local mode then decides alone.
    context.localModeUnsynced = in.localModeUnsynced;
    // Encrypted rooms may withhold more; a room not listed yet resolves like
    // unknown encryption (the stricter level).
    context.previewMode =
        static_cast<NotificationManager::PreviewMode>(in.previewMode);
    context.notificationsEnabled = in.notificationsEnabled;
    // "On screen" is thread-aware. The room timeline hides threaded events,
    // so a thread reply is on screen only while the panel shows its thread
    // and the panel itself follows the latest reply in a focused window; the
    // main timeline's own scroll position says nothing about it.
    const bool threaded = in.fromThread
        || (!in.eventThreadRootId.isEmpty()
            && in.eventThreadRootId != in.eventId);
    if (threaded) {
        context.roomVisibleAtLatest = in.roomId == in.currentRoomId
            && in.threadPanelPresent
            && in.threadPanelRootId == in.eventThreadRootId
            && in.threadPanelAtLatest;
    } else {
        context.roomVisibleAtLatest =
            in.roomId == in.currentRoomId && in.activeRoomAtLatest;
    }
    // The open room's backlog arrives as live appends while its view is still
    // hydrating.
    context.roomHydrating =
        in.roomId == in.currentRoomId && in.activeRoomHydrating;
    // Initial-sync backlog is history and must not re-notify each launch.
    context.initialSyncComplete = in.initialSyncComplete;
    context.soundMode =
        static_cast<NotificationManager::SoundMode>(in.soundMode);
    // Events from an ignored user already in flight must not notify.
    context.senderIsIgnored = in.senderIsIgnored;
    // Empty (the foreground account) keeps today's stamping.
    context.accountUserId = in.accountUserId;
    context.accountLabel = in.accountLabel;
    return context;
}

} // namespace lightning::notifications
