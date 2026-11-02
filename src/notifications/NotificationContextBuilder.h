#pragma once

#include "notifications/NotificationManager.h"

#include <QString>

namespace lightning::notifications {

struct NotificationContextInputs {
    // The account the event belongs to and its title label. Phase 0 (the
    // foreground account): both empty, which is today's stamping.
    QString accountUserId;
    QString accountLabel;
    QString selfUserId;
    QString roomId;
    QString roomInfoName;
    bool roomIsDirect = false;
    QString avatarMxc;
    QString avatarColorKey;
    int themeId = 0;
    int roomMode = 0;
    bool localModeUnsynced = false;
    int previewMode = NotificationManager::SenderOnly;
    bool notificationsEnabled = true;
    QString eventId;
    QString eventThreadRootId;
    bool fromThread = false;
    QString currentRoomId;
    bool activeRoomAtLatest = false;
    bool activeRoomHydrating = false;
    bool threadPanelPresent = false;
    QString threadPanelRootId;
    bool threadPanelAtLatest = false;
    bool initialSyncComplete = true;
    int soundMode = NotificationManager::SoundMentionsAndDirect;
    bool senderIsIgnored = false;
    QString senderMemberName;
    QString senderProfileName;
};

struct NotificationContextBuilder {
    static NotificationManager::Context build(const NotificationContextInputs &in);
};

}
