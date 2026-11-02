#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <functional>

namespace lightning::switchtrace {

enum class Step : quint8 { S1 = 1, S2, S3, S4, S5, S6, S7, S8, S9, S10, S11 };
enum class Path : quint8 { Cold, Adopt, Warm, HostThenAdopt, RetireThenCold, Startup };
enum class Outcome : quint8 { Ok, Partial, Refused, Failed, Superseded };

constexpr int kStepCount = 11;
constexpr qint64 kDeadlineMs = 30000;

bool enabled();
bool active();
void begin(const QString &fromUserId, const QString &toUserId, Path path = Path::Cold);
void noteIntent(const QString &toUserId);
constexpr qint64 kIntentMaxAgeMs = 120000;
void stepBegin(Step);
void stepEnd(Step);

class StepScope {
public:
    explicit StepScope(Step s);
    ~StepScope();
    StepScope(const StepScope &) = delete;
    StepScope &operator=(const StepScope &) = delete;

private:
    Step m_step;
    bool m_on;
};

void noteRustRestore(const QJsonObject &rustTrace, qint64 receivedWallMs);
void noteFirstReset(int rooms, qint64 parseMs, qint64 modelMs,
                    const QJsonObject &rustTrace, qint64 receivedWallMs);
void markFirstFrame();
void markRoomInteractive();
void abandonRoomInteractive();
void noteLoggedIn(bool loggedIn);
void markLoginSucceeded();
bool awaitingLogin();
void finish(Outcome);

bool enabledFromEnvValue(const QByteArray &raw);
struct Record {
    quint64 id = 0;
    Path path = Path::Cold;
    Outcome outcome = Outcome::Ok;
    QString fromSlug;
    QString toSlug;
    qint64 steps[kStepCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int s6Runs = 0;
    qint64 rustDiscoveryMs = -1;
    qint64 rustStoreOpenMs = -1;
    qint64 rustStoreOpens = -1;
    qint64 rustFallbackBuildMs = -1;
    int rustOfflineFallback = -1;
    qint64 rustRestoreMs = -1;
    qint64 rustResumeUnsentMs = -1;
    qint64 rustRestoreTotalMs = -1;
    qint64 restorePollMs = -1;
    qint64 resetRooms = -1;
    qint64 rustSyncBuildMs = -1;
    qint64 rustSinceStartSyncMs = -1;
    qint64 rustResetSerializeMs = -1;
    qint64 resetPollMs = -1;
    qint64 resetParseMs = -1;
    qint64 modelMs = -1;
    qint64 firstFrameMs = -1;
    qint64 roomInteractiveMs = -1;
    int loggedInFlips = 0;
    qint64 intentLeadMs = -1;

    qint64 step(Step s) const { return steps[int(s) - 1]; }
};

QString formatLine(const Record &);
Record currentRecordForTest();
void setEnabledForTest(bool);
void setClockForTest(std::function<qint64()> monotonicMs);
void resetForTest();

}
