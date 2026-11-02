#include "calls/AudioDeviceTester.h"

#include "app/SettingsManager.h"
#include "calls/CallDeviceController.h"

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMetaObject>

#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/GstBootstrap.h"
#include "calls/SfuMediaEngine.h"
#include "calls/noise/MicProcessing.h"

#include <gst/gst.h>
#endif

namespace {
Q_LOGGING_CATEGORY(lcDeviceTest, "lightning.calls.devicetest")

qint64 monotonicMs()
{
    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    return clock.elapsed();
}
}

struct AudioDeviceTester::Session {
    std::mutex mutex;
    bool cancelled = false;
#ifdef HAVE_LIGHTNING_WEBRTC
    GstElement *pipeline = nullptr;
#endif
};

struct AudioDeviceTester::Relay {
    std::mutex mutex;
    AudioDeviceTester *target = nullptr;

    template <typename Fn>
    void post(Fn &&fn)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (target)
            QMetaObject::invokeMethod(target, std::forward<Fn>(fn),
                                      Qt::QueuedConnection);
    }

    void deliverDescription(quint64 generation, const QString &description)
    {
        post([this, generation, description] {
            if (target)
                target->onDescription(generation, description);
        });
    }
    void deliverStarted(quint64 generation)
    {
        post([this, generation] {
            if (target)
                target->onStarted(generation);
        });
    }
    void deliverFailed(quint64 generation, const QString &category)
    {
        post([this, generation, category] {
            if (target)
                target->onFailed(generation, category);
        });
    }
    void deliverFinished(quint64 generation)
    {
        post([this, generation] {
            if (target)
                target->onFinished(generation);
        });
    }
    void deliverLevel(quint64 generation, double peakDb)
    {
        post([this, generation, peakDb] {
            if (target)
                target->onLevel(generation, peakDb, monotonicMs());
        });
    }

#ifdef HAVE_LIGHTNING_WEBRTC
    struct Plan {
        Mode mode = Mode::Idle;
        bool testSources = false;
        int testChannels = 0;
        QString testMicSource;
        SfuMediaEngine::DeviceChoice microphone;
        SfuMediaEngine::DeviceChoice speaker;
        int gainPercent = 100;
        calls::noise::Mode noiseMode = calls::noise::kDefaultMode;
        bool autoGain = true;
    };

    struct BusContext {
        std::shared_ptr<Relay> relay;
        quint64 generation = 0;
        Mode mode = Mode::Idle;
    };

    static QString compose(Mode mode,
                           const SfuMediaEngine::MicrophoneCapture &capture,
                           const QString &sink, int gainPercent,
                           calls::noise::Mode noiseMode,
                           bool autoGain = true);
    static void run(std::shared_ptr<Relay> relay,
                    std::shared_ptr<Session> session, Plan plan,
                    quint64 generation);
    static void teardown(GstElement *pipeline);
    static GstBusSyncReply onBus(GstBus *bus, GstMessage *message,
                                 gpointer data);
#endif
};

#ifdef HAVE_LIGHTNING_WEBRTC

QString AudioDeviceTester::Relay::compose(
    Mode mode, const SfuMediaEngine::MicrophoneCapture &capture,
    const QString &sink, int gainPercent, calls::noise::Mode noiseMode,
    bool autoGain)
{
    if (mode == Mode::Tone) {
        return QStringLiteral("audiotestsrc name=tonesrc wave=sine freq=660 "
                              "volume=0.2 samplesperbuffer=4800 num-buffers=%1 "
                              "! audio/x-raw,rate=48000,channels=1 "
                              "! audioconvert ! audioresample ! %2")
            .arg(QString::number(kToneBuffers), sink);
    }
    const QString front = SfuMediaEngine::microphoneFrontDescription(capture);
    const QString processing =
        SfuMediaEngine::voiceProcessingDescription(noiseMode, false, autoGain);
    const QString gain = QString::number(
        SfuMediaEngine::audioFactorPercent(gainPercent) / 100.0, 'f', 3);
    const QString meter =
        QStringLiteral("! volume name=testvol volume=%1 "
                       "! level name=testlevel post-messages=true interval=%2 ")
            .arg(gain, QString::number(SfuMediaEngine::kMicLevelIntervalNs));
    // An element between the front's mono caps and the processing stage's
    // own caps: gst_parse reads a second caps string straight after a first
    // as an element name ("no element \"audio\""). The call has its mute
    // valve here; so does this, never closed.
    const QString valve = QStringLiteral(" ! valve name=testvalve drop=false ");
    const QString playbackSink = sink.contains(QLatin1String("sync="))
        ? sink
        : sink + QStringLiteral(" sync=false");
    const QString tail = mode == Mode::Loopback
        ? QStringLiteral("! queue max-size-buffers=0 max-size-bytes=0 "
                         "max-size-time=200000000 leaky=downstream "
                         "! audioconvert ! audioresample ! %1")
              .arg(playbackSink)
        : QStringLiteral("! fakesink sync=false async=false");
    return front + valve + processing + meter + tail;
}

void AudioDeviceTester::Relay::teardown(GstElement *pipeline)
{
    if (!pipeline)
        return;
    // NULL first: once it returns no streaming thread is left to run the
    // sync handler whose context is about to be freed.
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (GstBus *bus = gst_element_get_bus(pipeline)) {
        gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
        gst_object_unref(bus);
    }
    gst_object_unref(pipeline);
}

GstBusSyncReply AudioDeviceTester::Relay::onBus(GstBus *, GstMessage *message,
                                                gpointer data)
{
    auto *context = static_cast<BusContext *>(data);
    const gchar *source = GST_MESSAGE_SRC_NAME(message);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ELEMENT: {
        const GstStructure *fields = gst_message_get_structure(message);
        double peak = -350.0;
        if (fields && source && g_strcmp0(source, "testlevel") == 0
            && gst_structure_has_name(fields, "level")
            && SfuMediaEngine::readLevelPeak(fields, &peak)) {
            context->relay->deliverLevel(context->generation, peak);
        }
        break;
    }
    case GST_MESSAGE_ERROR: {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        const QString element = QString::fromUtf8(source ? source : "");
        QString category = QStringLiteral("pipeline");
        if (element.startsWith(QLatin1String("micsrc")))
            category = QStringLiteral("microphone");
        else if (element.startsWith(QLatin1String("outsink"))
                 || element.startsWith(QLatin1String("tonesrc")))
            category = QStringLiteral("output");
        // The element name and GStreamer's static message only; `debug`
        // carries file paths.
        qCWarning(lcDeviceTest)
            << "audio device test failed element=" << element << "category="
            << category << "reason="
            << (error && error->message ? error->message : "?");
        if (error)
            g_error_free(error);
        g_free(debug);
        context->relay->deliverFailed(context->generation, category);
        break;
    }
    case GST_MESSAGE_EOS:
        context->relay->deliverFinished(context->generation);
        break;
    default:
        break;
    }
    gst_message_unref(message);
    return GST_BUS_DROP;
}

void AudioDeviceTester::Relay::run(std::shared_ptr<Relay> relay,
                                   std::shared_ptr<Session> session, Plan plan,
                                   quint64 generation)
{
    SfuMediaEngine::MicrophoneCapture capture;
    SfuMediaEngine::OutputSink output;
    if (plan.mode != Mode::Tone) {
        if (plan.testSources) {
            capture.source = plan.testMicSource.isEmpty()
                ? QStringLiteral(
                      "audiotestsrc is-live=true wave=sine freq=440 volume=0.5 "
                      "name=micsrc")
                : QStringLiteral("%1 name=micsrc").arg(plan.testMicSource);
        } else {
            capture = SfuMediaEngine::resolveMicrophoneCapture(plan.microphone);
        }
        if (plan.testChannels > 0)
            capture.channels = plan.testChannels;
    }
    if (plan.mode != Mode::Meter) {
        if (plan.testSources)
            output.sink = QStringLiteral("fakesink name=outsink sync=false");
        else
            output = SfuMediaEngine::resolveSpeakerSink(plan.speaker);
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->cancelled)
            return;
    }

    const QString description =
        compose(plan.mode, capture, output.sink, plan.gainPercent,
                plan.noiseMode, plan.autoGain);
    relay->deliverDescription(generation, description);

    GError *error = nullptr;
    GstElement *pipeline =
        gst_parse_launch(description.toUtf8().constData(), &error);
    if (error || !pipeline) {
        qCWarning(lcDeviceTest)
            << "audio device test: pipeline parse failed:"
            << (error && error->message ? error->message : "no pipeline");
        if (error)
            g_error_free(error);
        if (pipeline)
            gst_object_unref(pipeline);
        relay->deliverFailed(generation, QStringLiteral("pipeline"));
        return;
    }
    if (!capture.binding.isEmpty())
        SfuMediaEngine::applyCaptureBinding(pipeline, "micsrc", capture.binding);
    if (!output.binding.isEmpty())
        SfuMediaEngine::applyCaptureBinding(pipeline, "outsink", output.binding);

    if (GstBus *bus = gst_element_get_bus(pipeline)) {
        auto *context = new BusContext{relay, generation, plan.mode};
        gst_bus_set_sync_handler(
            bus, &Relay::onBus, context,
            [](gpointer data) { delete static_cast<BusContext *>(data); });
        gst_object_unref(bus);
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE) {
        teardown(pipeline);
        relay->deliverFailed(generation, plan.mode == Mode::Tone
                                             ? QStringLiteral("output")
                                             : QStringLiteral("microphone"));
        return;
    }

    bool keep = false;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        keep = !session->cancelled;
        if (keep)
            session->pipeline = pipeline;
    }
    if (!keep) {
        teardown(pipeline);
        return;
    }
    relay->deliverStarted(generation);
}

#endif

AudioDeviceTester::AudioDeviceTester(QObject *parent)
    : QObject(parent)
    , m_relay(std::make_shared<Relay>())
{
    m_relay->target = this;
    m_limitTimer.setSingleShot(true);
    connect(&m_limitTimer, &QTimer::timeout, this, [this] {
        qCInfo(lcDeviceTest) << "audio device test: time limit reached mode="
                             << modeName();
        stop();
    });
    m_countdown.setInterval(1000);
    connect(&m_countdown, &QTimer::timeout, this, [this] {
        const int left = m_limitTimer.isActive()
            ? (m_limitTimer.remainingTime() + 999) / 1000
            : 0;
        if (left == m_secondsLeft)
            return;
        m_secondsLeft = left;
        Q_EMIT secondsLeftChanged();
    });
}

AudioDeviceTester::~AudioDeviceTester()
{
    {
        std::lock_guard<std::mutex> lock(m_relay->mutex);
        m_relay->target = nullptr;
    }
    m_limitTimer.stop();
    m_countdown.stop();
    std::shared_ptr<Session> session = std::move(m_session);
    if (!session)
        return;
#ifdef HAVE_LIGHTNING_WEBRTC
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    std::thread([session, done] {
        GstElement *pipeline = nullptr;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->cancelled = true;
            pipeline = session->pipeline;
            session->pipeline = nullptr;
        }
        Relay::teardown(pipeline);
        done->set_value();
    }).detach();
    finished.wait_for(std::chrono::milliseconds(2000));
#else
    std::lock_guard<std::mutex> lock(session->mutex);
    session->cancelled = true;
#endif
}

void AudioDeviceTester::setDeviceController(CallDeviceController *devices)
{
    if (m_devices == devices)
        return;
    if (m_devices)
        disconnect(m_devices, nullptr, this, nullptr);
    m_devices = devices;
    if (!m_devices)
        return;
    connect(m_devices, &CallDeviceController::selectionChanged, this, [this] {
        if (microphoneTestRunning())
            start(m_mode);
    });
}

void AudioDeviceTester::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = settings;
    if (m_settings) {
        connect(m_settings, &SettingsManager::microphoneGainChanged, this,
                &AudioDeviceTester::applyGain);
        connect(m_settings, &SettingsManager::noiseSuppressionModeChanged,
                this, &AudioDeviceTester::applyNoiseMode);
    }
}

void AudioDeviceTester::setRuntimeAvailable(bool runtimeAvailable)
{
    if (m_runtimeAvailable == runtimeAvailable)
        return;
    m_runtimeAvailable = runtimeAvailable;
    if (!runtimeAvailable)
        stop();
    Q_EMIT availableChanged();
}

void AudioDeviceTester::setCallActive(bool active)
{
    if (m_callActive == active)
        return;
    m_callActive = active;
    Q_EMIT blockedByCallChanged();
    if (active && m_mode != Mode::Idle) {
        qCInfo(lcDeviceTest)
            << "audio device test: stopped because a call is starting";
        stop();
    }
}

bool AudioDeviceTester::built() const
{
#ifdef HAVE_LIGHTNING_WEBRTC
    return true;
#else
    return false;
#endif
}

QString AudioDeviceTester::modeName() const
{
    switch (m_mode) {
    case Mode::Meter:
        return QStringLiteral("meter");
    case Mode::Loopback:
        return QStringLiteral("loopback");
    case Mode::Tone:
        return QStringLiteral("tone");
    case Mode::Idle:
        break;
    }
    return QStringLiteral("idle");
}

bool AudioDeviceTester::startMicrophoneTest(bool playBack)
{
    return start(playBack ? Mode::Loopback : Mode::Meter);
}

bool AudioDeviceTester::playTestSound()
{
    return start(Mode::Tone);
}

void AudioDeviceTester::stop()
{
    endSession();
    m_throttle.reset();
    setLevel(0.0);
    if (m_secondsLeft != 0) {
        m_secondsLeft = 0;
        Q_EMIT secondsLeftChanged();
    }
    setMode(Mode::Idle);
}

bool AudioDeviceTester::start(Mode mode)
{
    if (mode == Mode::Idle) {
        stop();
        return true;
    }
    if (m_callActive) {
        setError(QStringLiteral("in_call"));
        return false;
    }
    if (!available()) {
        setError(QStringLiteral("unavailable"));
        return false;
    }
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!lightning::gst::ensureInitialised()) {
        setError(QStringLiteral("unavailable"));
        return false;
    }
    if (mode != Mode::Tone && !SfuMediaEngine::levelElementAvailable()) {
        setError(QStringLiteral("no_meter"));
        return false;
    }

    endSession();
    m_throttle.reset();
    setLevel(0.0);
    setError(QString());

    Relay::Plan plan;
    plan.mode = mode;
    plan.testSources = m_testSources;
    plan.testChannels = m_testChannels;
    plan.testMicSource = m_testMicSource;
    if (!m_testSources && m_devices) {
        const CallDeviceController::Selection mic =
            m_devices->microphoneSelection();
        const CallDeviceController::Selection speaker =
            m_devices->speakerSelection();
        plan.microphone = {mic.id, mic.description};
        plan.speaker = {speaker.id, speaker.description};
    }
    plan.gainPercent = m_settings ? m_settings->microphoneGain() : 100;
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_settings) {
        plan.noiseMode = calls::noise::modeFromKey(
            m_settings->noiseSuppressionMode().toStdString(),
            calls::noise::kDefaultMode);
        plan.autoGain = m_settings->autoGainControl();
    }
#endif

    const quint64 generation = ++m_generation;
    m_session = std::make_shared<Session>();
    setMode(mode);
    m_limitTimer.start(limitMs(mode));
    if (mode != Mode::Tone) {
        m_secondsLeft = (limitMs(mode) + 999) / 1000;
        Q_EMIT secondsLeftChanged();
        m_countdown.start();
    }
    qCInfo(lcDeviceTest) << "audio device test: starting mode=" << modeName()
                         << "chosen-microphone=" << !plan.microphone.id.isEmpty()
                         << "chosen-output=" << !plan.speaker.id.isEmpty();
    std::thread([relay = m_relay, session = m_session, plan, generation] {
        Relay::run(relay, session, plan, generation);
    }).detach();
    return true;
#else
    return false;
#endif
}

void AudioDeviceTester::endSession()
{
    ++m_generation;
    m_limitTimer.stop();
    m_countdown.stop();
    std::shared_ptr<Session> session = std::move(m_session);
    if (!session)
        return;
#ifdef HAVE_LIGHTNING_WEBRTC
    std::thread([session] {
        GstElement *pipeline = nullptr;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->cancelled = true;
            pipeline = session->pipeline;
            session->pipeline = nullptr;
        }
        Relay::teardown(pipeline);
    }).detach();
#else
    std::lock_guard<std::mutex> lock(session->mutex);
    session->cancelled = true;
#endif
}

int AudioDeviceTester::limitMs(Mode mode) const
{
    if (m_limitOverrideMs > 0)
        return m_limitOverrideMs;
    switch (mode) {
    case Mode::Meter:
        return kMeterLimitMs;
    case Mode::Loopback:
        return kLoopbackLimitMs;
    case Mode::Tone:
        return kToneBuffers * 100 + 4000;
    case Mode::Idle:
        break;
    }
    return 0;
}

void AudioDeviceTester::applyGain()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_session || !m_settings)
        return;
    const double factor =
        SfuMediaEngine::audioFactorPercent(m_settings->microphoneGain()) / 100.0;
    std::lock_guard<std::mutex> lock(m_session->mutex);
    if (!m_session->pipeline)
        return;
    if (GstElement *volume =
            gst_bin_get_by_name(GST_BIN(m_session->pipeline), "testvol")) {
        g_object_set(volume, "volume", factor, nullptr);
        gst_object_unref(volume);
    }
#endif
}

void AudioDeviceTester::applyNoiseMode()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_session || !m_settings)
        return;
    const calls::noise::Mode mode = calls::noise::modeFromKey(
        m_settings->noiseSuppressionMode().toStdString(),
        calls::noise::kDefaultMode);
    std::lock_guard<std::mutex> lock(m_session->mutex);
    if (!m_session->pipeline)
        return;
    const QString did = calls::noise::applyMode(
        m_session->pipeline, mode, calls::noise::DspSwitch::Now);
    qCInfo(lcDeviceTest) << "audio device test: noise suppression mode="
                         << calls::noise::modeKey(mode) << did;
#endif
}

bool AudioDeviceTester::pipelineRunningForTest() const
{
    if (!m_session)
        return false;
#ifdef HAVE_LIGHTNING_WEBRTC
    std::lock_guard<std::mutex> lock(m_session->mutex);
    return m_session->pipeline != nullptr;
#else
    return false;
#endif
}

QString AudioDeviceTester::describeForTest(Mode mode, const QString &micSource,
                                           int channels, const QString &sink,
                                           int gainPercent,
                                           calls::noise::Mode noiseMode,
                                           bool autoGain)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    SfuMediaEngine::MicrophoneCapture capture;
    capture.source = micSource;
    capture.channels = channels;
    return Relay::compose(mode, capture, sink, gainPercent, noiseMode,
                          autoGain);
#else
    Q_UNUSED(mode);
    Q_UNUSED(micSource);
    Q_UNUSED(channels);
    Q_UNUSED(sink);
    Q_UNUSED(gainPercent);
    Q_UNUSED(noiseMode);
    Q_UNUSED(autoGain);
    return {};
#endif
}

void AudioDeviceTester::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    Q_EMIT modeChanged();
}

void AudioDeviceTester::setError(const QString &category)
{
    if (m_error == category)
        return;
    m_error = category;
    Q_EMIT errorChanged();
}

void AudioDeviceTester::setLevel(double level)
{
    if (qFuzzyCompare(1.0 + level, 1.0 + m_level))
        return;
    m_level = level;
    Q_EMIT levelChanged();
}

void AudioDeviceTester::onDescription(quint64 generation,
                                      const QString &description)
{
    if (generation != m_generation)
        return;
    m_lastDescription = description;
}

void AudioDeviceTester::onStarted(quint64 generation)
{
    if (generation != m_generation)
        return;
    ++m_startsReported;
    qCInfo(lcDeviceTest) << "audio device test: running mode=" << modeName();
}

void AudioDeviceTester::onFailed(quint64 generation, const QString &category)
{
    if (generation != m_generation)
        return;
    stop();
    setError(category);
}

void AudioDeviceTester::onFinished(quint64 generation)
{
    if (generation != m_generation)
        return;
    const bool microphoneEnded = microphoneTestRunning();
    stop();
    if (microphoneEnded)
        setError(QStringLiteral("microphone"));
}

void AudioDeviceTester::onLevel(quint64 generation, double peakDb,
                                qint64 nowMs)
{
    if (generation != m_generation || !microphoneTestRunning())
        return;
    double shown = 0.0;
    if (!m_throttle.offer(peakDb, nowMs, &shown))
        return;
    setLevel(lightning::calls::meterFraction(shown));
}
