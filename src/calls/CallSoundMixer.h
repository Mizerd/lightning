#pragma once

#include "calls/CallSoundPcm.h"

#include <QAudioFormat>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>
#include <optional>

class CueOutput : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void start(QIODevice *feed) = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual bool suspended() const = 0;
    virtual void stop() = 0;

Q_SIGNALS:
    void failed();
};

class CueEngine : public QObject
{
    Q_OBJECT

public:
    struct Hooks {
        std::function<std::optional<QByteArray>(const QString &wanted)>
            resolveOutput;
        std::function<bool(const QByteArray &id, const QAudioFormat &format)>
            supports;
        std::function<CueOutput *(const QByteArray &id,
                                  const QAudioFormat &format, QObject *parent)>
            open;
        std::function<std::optional<QByteArray>(const QString &sound)> wav;
        std::function<void(const QString &sound, bool ready)> setReady;
        /// Whether a new stream may be opened now as far as the sound server
        /// goes (the app: PulseAudio's socket accepts and Qt is connected to
        /// it); set where a new stream must wait for that (Qt's PulseAudio
        /// backend, see the top of this file), unset where it need not.
        std::function<bool()> serverReachable;
    };

    static constexpr int kMaxOutputs = 3;
    /// How long an output may take no audio while some waits for it before
    /// it counts as dead. A healthy one takes some every ring buffer
    /// (120 ms, AudioSinkOutput) unless the GUI thread is held up, and a
    /// check that comes late (the thread was held up) judges nothing.
    static constexpr int kStallMs = 2500;
    static constexpr int kProbeMs = 500;
    static constexpr int kStallRetryMs = 30000;
    static constexpr int kReopenSettleMs = 5000;
    static constexpr int kServerLiveMs = 4000;
    static constexpr int kGatePollMinMs = 250;
    static constexpr int kGatePollMaxMs = 5000;

    CueEngine(QStringList sounds, Hooks hooks, QObject *parent = nullptr);
    ~CueEngine() override;

    void preload();
    void play(const QString &sound, float gain, const QString &wanted);
    void loop(const QString &sound, float gain, const QString &wanted);
    int reloadUnusable();
    void stopAll();

    void setIdleSuspendMsForTest(int ms) { m_idleSuspendMs = ms; }
    void setStallMsForTest(int stallMs, int retryMs, int settleMs)
    {
        m_stallMs = stallMs;
        m_stallRetryMs = retryMs;
        m_reopenSettleMs = settleMs;
    }
    void setServerLiveMsForTest(int ms) { m_serverLiveMs = ms; }
    void setGatePollMsForTest(int minMs, int maxMs)
    {
        m_gatePollMinMs = m_gatePollMs = minMs;
        m_gatePollMaxMs = maxMs;
    }
    int outputsReopenedForTest() const { return m_outputsReopened; }
    bool stallTimerActiveForTest() const { return m_stallTimer.isActive(); }
    int outputsOpenedForTest() const { return m_outputsOpened; }
    CueOutput *outputForTest(const QByteArray &id = {}) const;
    QList<QByteArray> outputIdsForTest() const;
    int voicesForTest(const QByteArray &id = {}) const;
    bool failedForTest(const QByteArray &id) const { return m_failed.contains(id); }

private:
    struct Lane {
        QByteArray id;
        QPointer<CueOutput> output;
        callsound::MixerFeed *feed = nullptr;
        std::unique_ptr<callsound::CueMixer> mixer;
        std::unique_ptr<QTimer> idleTimer;
        quint64 lastUsed = 0;
        quint64 takenAtCheck = 0;
        qint64 checkedAtMs = -1;
        bool unproven = false;
        bool dead = false;
        /// Offered the silence of the last output change and has taken
        /// nothing since: a stream that died before that change, which Qt
        /// could only report over a live connection.
        bool probed = false;
    };

    Lane *laneFor(const QString &wanted);
    Lane *openLane(const QByteArray &id,
                   std::unique_ptr<callsound::CueMixer> carried = {});
    void closeLane(const QByteArray &id);
    void onOutputFailed(CueOutput *output);
    void wake(Lane *lane);
    void onIdle(const QByteArray &id);
    void watch(Lane *lane);
    void onStallTick();
    void reopenStalled(const QByteArray &id);
    bool serverSettled(qint64 now, bool waiting, bool late);
    void giveUpStalled(const QByteArray &id);
    void resumeLoop();
    bool decodable(const QString &sound);
    std::optional<callsound::Pcm> decoded(const QString &sound);
    std::optional<callsound::Pcm> pcm(const QString &sound, int channels);
    void startLoop(Lane *lane);
    void stopLoopVoice();
    int refreshReadiness();

    QStringList m_sounds;
    Hooks m_hooks;
    std::map<QByteArray, std::unique_ptr<Lane>> m_lanes;
    QSet<QString> m_decodable;
    QHash<int, QHash<QString, callsound::Pcm>> m_converted;
    QSet<QString> m_undecodable;
    QSet<QByteArray> m_failed;
    int m_outputsOpened = 0;
    quint64 m_useCounter = 0;
    QString m_loopSound;
    float m_loopGain = 1.0f;
    QString m_loopDevice;
    QByteArray m_loopLane;
    int m_loopVoice = 0;
    int m_idleSuspendMs = 5000;
    QTimer m_stallTimer{this};
    QElapsedTimer m_clock;
    qint64 m_lastStallTickMs = -1;
    int m_stallMs = kStallMs;
    int m_stallRetryMs = kStallRetryMs;
    int m_reopenSettleMs = kReopenSettleMs;
    QSet<QByteArray> m_stallFailed;
    bool m_retryDue = false;
    quint64 m_retryGeneration = 0;
    qint64 m_serverLiveSinceMs = -1;
    int m_serverLiveMs = kServerLiveMs;
    int m_gatePollMinMs = kGatePollMinMs;
    int m_gatePollMaxMs = kGatePollMaxMs;
    int m_gatePollMs = kGatePollMinMs;
    qint64 m_nextGatePollMs = -1;
    int m_outputsReopened = 0;
};
