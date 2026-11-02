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
    context.avatarColorKey = in.avatarColorKey;
    context.themeId = in.themeId;
    context.roomMode = static_cast<NotificationManager::RoomMode>(in.roomMode);
    context.localModeUnsynced = in.localModeUnsynced;
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
    context.roomHydrating =
        in.roomId == in.currentRoomId && in.activeRoomHydrating;
    context.initialSyncComplete = in.initialSyncComplete;
    context.soundMode =
        static_cast<NotificationManager::SoundMode>(in.soundMode);
    context.senderIsIgnored = in.senderIsIgnored;
    context.senderMemberName = in.senderMemberName;
    context.senderProfileName = in.senderProfileName;
    context.accountUserId = in.accountUserId;
    context.accountLabel = in.accountLabel;
    return context;
}

}
