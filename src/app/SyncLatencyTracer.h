#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace synctrace {

bool enabled();

int slowThresholdMs();

// Mint a correlation id and record the SDK stage. `roomId` is stored only as a
// hash. Returns 0 when disabled; every other entry point ignores id 0.
// `sdkEpochMs` is the Rust side's wall-clock enqueue stamp (0 = use now).
quint64 beginEvent(const QString &roomId, qint64 sdkEpochMs = 0);

void noteBridge(quint64 id);
void noteModel(quint64 id);
void noteUi(quint64 id);

void noteSyncResponse();
void noteSyncState(const char *state);

int completedJourneys();
int reportedStalls();
qint64 lastJourneyTotalMs();
void resetForTest(int thresholdMsOverride);

}
