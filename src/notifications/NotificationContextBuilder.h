// The notification context for one timeline event, built from plain inputs.
//
// Factored out of AppController::notificationContextFor() so the same rules
// serve the foreground account today and a background account's host later
// (live accounts, Phase 1), which has no AppController state to read. Pure:
// no I/O, no signals, no settings or client access.
//
// Includes NotificationManager.h ONLY. It must never include AppController,
// SettingsManager or MatrixClient: the caller gathers the inputs, which is
// what lets a host supply its own (a source scan enforces this in Phase 1).
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
    // RoomInfo::name; may be empty, and the room id stands in for it then.
    QString roomInfoName;
    bool roomIsDirect = false;
    // The room list row's "avatarUrl" / "identityColorKey".
    QString avatarMxc;
    QString avatarColorKey;
    int themeId = 0;
    // SettingsManager's stored value, 0..3, cast as today.
    int roomMode = 0;
    // The server does not have this device's choice; the local mode decides.
    bool localModeUnsynced = false;
    // noticePreviewFor().
    int previewMode = NotificationManager::SenderOnly;
    bool notificationsEnabled = true;
    // The event: its id and thread root, and whether it arrived through a
    // thread timeline.
    QString eventId;
    QString eventThreadRootId;
    bool fromThread = false;
    // What is on screen.
    QString currentRoomId;
    bool activeRoomAtLatest = false;
    bool activeRoomHydrating = false;
    bool threadPanelPresent = false;
    QString threadPanelRootId;
    bool threadPanelAtLatest = false;
    bool initialSyncComplete = true;
    int soundMode = NotificationManager::SoundMentionsAndDirect;
    bool senderIsIgnored = false;
};

struct NotificationContextBuilder {
    static NotificationManager::Context build(const NotificationContextInputs &in);
};

} // namespace lightning::notifications
