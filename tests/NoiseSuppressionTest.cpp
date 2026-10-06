// Microphone noise suppression (GitHub #20): the processing layer.
//
// Everything here runs through the REAL `lightningdenoise` element, driven
// pad-to-pad on the test thread (no pipeline threads, so every byte is
// deterministic), with FAKE backends whose effect is exact in float: a gain
// of 0.5 (standing in for RNNoise) or 0.25 (DeepFilterNet). The fakes count
// their instances, their frames and every sample they were handed, which is
// what "only the selected backend ran" and "nothing leaked" are asserted on.
//
// The webrtcdsp live swap and the shared chain descriptions run in real
// pipelines; the real backends run when this build carries them.

#include "calls/GstBootstrap.h"
#include "calls/noise/DenoiseElement.h"
#include "calls/noise/MicProcessing.h"
#include "calls/noise/NoiseSuppressor.h"

#include <QElapsedTimer>
#include <QThread>
#include <QtTest/QtTest>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gst/gst.h>

using calls::noise::Mode;

namespace {

// ── Fake backends ───────────────────────────────────────────────────────

struct Ledger {
    std::atomic<int> live{0};
    std::array<std::atomic<int>, 4> created{};
    std::array<std::atomic<long long>, 4> frames{};
    std::atomic<int> resets{0};
    std::atomic<bool> sawBadSample{false};

    void clear()
    {
        // `live` is deliberately not cleared: a leak from an earlier case
        // must stay visible.
        for (auto &c : created)
            c.store(0);
        for (auto &f : frames)
            f.store(0);
        resets.store(0);
        sawBadSample.store(false);
    }
};
Ledger g_ledger;

class FakeSuppressor final : public calls::noise::NoiseSuppressor
{
public:
    FakeSuppressor(Mode mode, float gain, bool ok = true,
                   int frame = calls::noise::kFrameSamples, int latency = 0,
                   bool realtimeReset = false)
        : m_mode(mode), m_gain(gain), m_ok(ok), m_frame(frame),
          m_latency(latency), m_realtimeReset(realtimeReset)
    {
        ++g_ledger.live;
        ++g_ledger.created[size_t(mode)];
    }
    ~FakeSuppressor() override { --g_ledger.live; }

    Mode mode() const override { return m_mode; }
    int frameSize() const override { return m_frame; }
    bool ok() const override { return m_ok; }
    void process(float *frame) noexcept override
    {
        ++g_ledger.frames[size_t(m_mode)];
        for (int i = 0; i < m_frame; ++i) {
            if (!std::isfinite(frame[i]) || std::fabs(frame[i]) > 4.0f)
                g_ledger.sawBadSample.store(true);
            frame[i] *= m_gain;
        }
    }
    int latencySamples() const override { return m_latency; }
    void reset() noexcept override { ++g_ledger.resets; }
    bool resetIsRealtimeSafe() const override { return m_realtimeReset; }

private:
    Mode m_mode;
    float m_gain;
    bool m_ok;
    int m_frame;
    int m_latency;
    bool m_realtimeReset;
};

/// A backend that stops processing for good after `frames` frames, as
/// DeepFilterNet does after a caught panic: from then on it passes audio
/// through and says failed().
class FailingSuppressor final : public calls::noise::NoiseSuppressor
{
public:
    FailingSuppressor(Mode mode, int frames) : m_mode(mode), m_left(frames)
    {
        ++g_ledger.live;
        ++g_ledger.created[size_t(mode)];
    }
    ~FailingSuppressor() override { --g_ledger.live; }

    Mode mode() const override { return m_mode; }
    int frameSize() const override { return calls::noise::kFrameSamples; }
    bool ok() const override { return true; }
    void process(float *frame) noexcept override
    {
        if (m_failed.load())
            return;
        ++g_ledger.frames[size_t(m_mode)];
        for (int i = 0; i < calls::noise::kFrameSamples; ++i)
            frame[i] *= 0.5f;
        if (--m_left <= 0)
            m_failed.store(true);
    }
    int latencySamples() const override { return 0; }
    void reset() noexcept override {}
    bool failed() const noexcept override { return m_failed.load(); }

private:
    Mode m_mode;
    int m_left;
    std::atomic<bool> m_failed{false};
};

constexpr float kRnnoiseGain = 0.5f;
constexpr float kDfnGain = 0.25f;
constexpr int kFakeLatency = 96;

std::unique_ptr<calls::noise::NoiseSuppressor> fakeFactory(Mode mode)
{
    if (mode == Mode::RNNoise)
        // Like the real ones: RNNoise declares a real-time-safe reset,
        // DeepFilterNet (until its reset stops allocating) does not.
        return std::make_unique<FakeSuppressor>(mode, kRnnoiseGain, true,
                                                calls::noise::kFrameSamples,
                                                kFakeLatency,
                                                /*realtimeReset=*/true);
    if (mode == Mode::DeepFilterNet)
        return std::make_unique<FakeSuppressor>(mode, kDfnGain);
    return nullptr;
}

/// Non-zero, exact in float, and well inside the sanitising clamp.
float ramp(size_t i) { return 1.0f + float(i % 1000) / 1024.0f; }

std::vector<float> rampSamples(size_t from, size_t count)
{
    std::vector<float> out(count);
    for (size_t i = 0; i < count; ++i)
        out[i] = ramp(from + i);
    return out;
}

std::atomic<int> g_elementsAlive{0};
void elementFinalized(gpointer, GObject *) { --g_elementsAlive; }

// ── A pad-to-pad harness around one element ─────────────────────────────

GstStaticPadTemplate kAnySrc = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
GstStaticPadTemplate kAnySink = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

const char *const kFloatCaps =
    "audio/x-raw,format=F32LE,rate=48000,channels=1,layout=interleaved";

class Harness
{
public:
    explicit Harness(const char *caps = kFloatCaps)
    {
        element = gst_element_factory_make(
            calls::noise::denoiseElementName(), nullptr);
        if (!element)
            return;
        gst_object_ref_sink(element);
        ++g_elementsAlive;
        g_object_weak_ref(G_OBJECT(element), elementFinalized, nullptr);
        bus = gst_bus_new();
        gst_element_set_bus(element, bus);

        src = gst_pad_new_from_static_template(&kAnySrc, "testsrc");
        sink = gst_pad_new_from_static_template(&kAnySink, "testsink");
        g_object_set_data(G_OBJECT(sink), "harness", this);
        gst_pad_set_chain_function(sink, chain);
        gst_pad_set_event_function(sink, acceptEvent);
        gst_pad_set_query_function(src, upstreamQuery);
        GstPad *elementSink = gst_element_get_static_pad(element, "sink");
        GstPad *elementSrc = gst_element_get_static_pad(element, "src");
        gst_pad_link(src, elementSink);
        gst_pad_link(elementSrc, sink);
        gst_object_unref(elementSink);
        gst_object_unref(elementSrc);
        gst_pad_set_active(src, TRUE);
        gst_pad_set_active(sink, TRUE);
        gst_element_set_state(element, GST_STATE_PLAYING);

        gst_pad_push_event(src, gst_event_new_stream_start("noise-test"));
        GstCaps *parsed = gst_caps_from_string(caps);
        // Not push_event's return value: a sticky event the peer refuses is
        // still stored on our pad, and that counts as TRUE. What matters is
        // whether the ELEMENT took the caps.
        gst_pad_push_event(src, gst_event_new_caps(parsed));
        gst_caps_unref(parsed);
        {
            GstPad *taken = gst_element_get_static_pad(element, "sink");
            capsAccepted = gst_pad_has_current_caps(taken);
            gst_object_unref(taken);
        }
        GstSegment segment;
        gst_segment_init(&segment, GST_FORMAT_TIME);
        gst_pad_push_event(src, gst_event_new_segment(&segment));
    }

    ~Harness()
    {
        if (!element)
            return;
        gst_element_set_state(element, GST_STATE_NULL);
        gst_pad_set_active(src, FALSE);
        gst_pad_set_active(sink, FALSE);
        gst_object_unref(src);
        gst_object_unref(sink);
        gst_element_set_bus(element, nullptr);
        gst_object_unref(bus);
        gst_object_unref(element);
    }

    Harness(const Harness &) = delete;
    Harness &operator=(const Harness &) = delete;

    GstFlowReturn pushBytes(const void *data, gsize bytes, bool discont = false)
    {
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
        if (bytes > 0)
            gst_buffer_fill(buffer, 0, data, bytes);
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale_int(
            m_pushedSamples, GST_SECOND, calls::noise::kSampleRate);
        m_pushedSamples += bytes / sizeof(float);
        if (discont)
            GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
        inputPts.push_back(GST_BUFFER_PTS(buffer));
        return gst_pad_push(src, buffer);
    }

    GstFlowReturn push(const std::vector<float> &samples, bool discont = false)
    {
        return pushBytes(samples.data(), samples.size() * sizeof(float),
                         discont);
    }

    void clearOutput()
    {
        output.clear();
        outputPts.clear();
        inputPts.clear();
    }

    std::vector<float> outputSamples() const
    {
        std::vector<float> out(size_t(output.size()) / sizeof(float));
        if (!out.empty())
            std::memcpy(out.data(), output.constData(),
                        out.size() * sizeof(float));
        return out;
    }

    void setMode(Mode mode) { calls::noise::setDenoiseMode(element, mode); }

    calls::noise::DenoiseStatus status() const
    {
        return calls::noise::denoiseStatus(element);
    }

    /// Feeds silence until the element reports `requested` taking effect,
    /// and returns that report's `ok`. The silence is cleared from output.
    /// Like waitForStatus(), for a report with this exact outcome.
    bool waitForStatusWith(Mode requested, bool wantOk, int timeoutMs = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            bool ok = !wantOk;
            if (waitForStatus(requested, &ok, int(timeoutMs - timer.elapsed()))
                && ok == wantOk) {
                return true;
            }
        }
        return false;
    }

    bool waitForStatus(Mode requested, bool *ok, int timeoutMs = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        const std::vector<float> silence(48, 0.0f);
        while (timer.elapsed() < timeoutMs) {
            push(silence);
            while (GstMessage *message =
                       gst_bus_pop_filtered(bus, GST_MESSAGE_ELEMENT)) {
                const GstStructure *fields = gst_message_get_structure(message);
                bool matched = false;
                if (fields
                    && gst_structure_has_name(fields,
                                              calls::noise::kStatusMessageName)
                    && g_strcmp0(gst_structure_get_string(fields, "requested"),
                                 calls::noise::modeKey(requested))
                        == 0) {
                    gboolean reported = FALSE;
                    gst_structure_get_boolean(fields, "ok", &reported);
                    if (ok)
                        *ok = reported;
                    matched = true;
                }
                gst_message_unref(message);
                if (matched) {
                    clearOutput();
                    return true;
                }
            }
            QThread::msleep(2);
        }
        return false;
    }

    GstElement *element = nullptr;
    GstBus *bus = nullptr;
    GstPad *src = nullptr;
    GstPad *sink = nullptr;
    bool capsAccepted = false;
    QByteArray output;
    std::vector<GstClockTime> inputPts;
    std::vector<GstClockTime> outputPts;

private:
    static GstFlowReturn chain(GstPad *pad, GstObject *, GstBuffer *buffer)
    {
        auto *self =
            static_cast<Harness *>(g_object_get_data(G_OBJECT(pad), "harness"));
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            self->output.append(reinterpret_cast<const char *>(map.data),
                                qsizetype(map.size));
            gst_buffer_unmap(buffer, &map);
        }
        self->outputPts.push_back(GST_BUFFER_PTS(buffer));
        gst_buffer_unref(buffer);
        return GST_FLOW_OK;
    }

    static gboolean acceptEvent(GstPad *, GstObject *, GstEvent *event)
    {
        gst_event_unref(event);
        return TRUE;
    }

    static gboolean upstreamQuery(GstPad *pad, GstObject *parent,
                                  GstQuery *query)
    {
        if (GST_QUERY_TYPE(query) == GST_QUERY_LATENCY) {
            gst_query_set_latency(query, TRUE, 0, GST_CLOCK_TIME_NONE);
            return TRUE;
        }
        return gst_pad_query_default(pad, parent, query);
    }

    guint64 m_pushedSamples = 0;
};

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QThread::msleep(5);
    }
    return true;
}

bool elementExists(const char *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

} // namespace

class NoiseSuppressionTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QString whyNot;
        if (!lightning::gst::ensureInitialised(&whyNot))
            QSKIP(qPrintable(QStringLiteral("no GStreamer: ") + whyNot));
        calls::noise::registerDenoiseElement();
        QVERIFY(elementExists(calls::noise::denoiseElementName()));
    }

    void init()
    {
        g_ledger.clear();
        calls::noise::setSuppressorFactoryForTest(fakeFactory);
    }

    void cleanup() { calls::noise::setSuppressorFactoryForTest({}); }

    // The setting's spelling round-trips, and anything else falls back rather
    // than landing on the "nearest" mode.
    void modeKeysRoundTripAndUnknownKeysFallBack()
    {
        for (Mode mode : {Mode::Off, Mode::WebRtc, Mode::RNNoise,
                          Mode::DeepFilterNet}) {
            QCOMPARE(int(calls::noise::modeFromKey(calls::noise::modeKey(mode),
                                                   Mode::Off)),
                     int(mode));
            QVERIFY(calls::noise::isModeKey(calls::noise::modeKey(mode)));
        }
        // "3" is DeepFilterNet's enum value: an index must not be honoured.
        QVERIFY(calls::noise::modeFromKey("3", Mode::WebRtc) == Mode::WebRtc);
        QVERIFY(calls::noise::modeFromKey("RNNoise", Mode::WebRtc)
                == Mode::WebRtc);
        QVERIFY(calls::noise::modeFromKey("", Mode::WebRtc) == Mode::WebRtc);
        QVERIFY(!calls::noise::isModeKey("deepfilternet "));
        QVERIFY(calls::noise::kDefaultMode == Mode::WebRtc);
    }

    // Off (and WebRTC, whose suppressor lives in webrtcdsp) leave every byte
    // alone, whatever the bytes are, and build no backend at all.
    void offAndWebrtcLeaveTheStreamBitExact()
    {
        for (Mode mode : {Mode::Off, Mode::WebRtc}) {
            Harness h;
            QVERIFY(h.element && h.capsAccepted);
            h.setMode(mode);
            // Arbitrary bit patterns, NaNs and infinities included: nothing
            // may touch them, not even the sanitiser.
            QByteArray sent;
            quint32 seed = 0x1234567u;
            for (const gsize words : {gsize(1), gsize(480), gsize(479),
                                      gsize(4096), gsize(13), gsize(960)}) {
                QByteArray chunk(qsizetype(words * 4), Qt::Uninitialized);
                for (gsize i = 0; i < words; ++i) {
                    seed = seed * 1664525u + 1013904223u;
                    std::memcpy(chunk.data() + i * 4, &seed, 4);
                }
                QCOMPARE(int(h.pushBytes(chunk.constData(), gsize(chunk.size()))), int(GST_FLOW_OK));
                sent += chunk;
            }
            QVERIFY2(h.output == sent,
                     qPrintable(QStringLiteral("mode %1 altered the stream")
                                    .arg(calls::noise::modeKey(mode))));
            QVERIFY(h.outputPts == h.inputPts);
            const auto status = h.status();
            QVERIFY(status.active == Mode::Off);
            QCOMPARE(status.latencySamples, 0);
            QCOMPARE(status.frames, 0ull);
        }
        QCOMPARE(g_ledger.created[size_t(Mode::RNNoise)].load(), 0);
        QCOMPARE(g_ledger.created[size_t(Mode::DeepFilterNet)].load(), 0);
    }

    // The selected backend runs, and only it: switching hands the stream to
    // the other one and the first stops seeing frames.
    void onlyTheSelectedBackendProcesses()
    {
        Harness h;
        QVERIFY(h.element);
        bool ok = false;
        h.setMode(Mode::RNNoise);
        QVERIFY(h.waitForStatus(Mode::RNNoise, &ok));
        QVERIFY(ok);
        QVERIFY(h.status().active == Mode::RNNoise);
        QVERIFY(h.push(rampSamples(0, 4800)) == GST_FLOW_OK);
        QVERIFY(g_ledger.frames[size_t(Mode::RNNoise)].load() >= 10);
        QCOMPARE(g_ledger.created[size_t(Mode::DeepFilterNet)].load(), 0);
        QCOMPARE(g_ledger.frames[size_t(Mode::DeepFilterNet)].load(), 0ll);

        h.setMode(Mode::DeepFilterNet);
        QVERIFY(h.waitForStatus(Mode::DeepFilterNet, &ok));
        QVERIFY(ok);
        const long long rnnoiseFrames =
            g_ledger.frames[size_t(Mode::RNNoise)].load();
        QVERIFY(h.push(rampSamples(0, 4800)) == GST_FLOW_OK);
        QCOMPARE(g_ledger.frames[size_t(Mode::RNNoise)].load(), rnnoiseFrames);
        QVERIFY(g_ledger.frames[size_t(Mode::DeepFilterNet)].load() >= 10);
        QCOMPARE(g_ledger.created[size_t(Mode::RNNoise)].load(), 1);
        QCOMPARE(g_ledger.created[size_t(Mode::DeepFilterNet)].load(), 1);
        // The retired RNNoise backend is destroyed off the streaming thread.
        QVERIFY(waitUntil([] { return g_ledger.live.load() == 1; }));

        // Back to Off: pass-through again, every backend gone.
        h.setMode(Mode::Off);
        QVERIFY(h.waitForStatus(Mode::Off, &ok));
        const long long dfnFrames =
            g_ledger.frames[size_t(Mode::DeepFilterNet)].load();
        const std::vector<float> after = rampSamples(7, 1000);
        h.push(after);
        QCOMPARE(g_ledger.frames[size_t(Mode::DeepFilterNet)].load(), dfnFrames);
        QVERIFY(h.outputSamples() == after);
        QVERIFY(waitUntil([] { return g_ledger.live.load() == 0; }));
    }

    // Any buffer size: the output is the input delayed by exactly one frame
    // and processed, sample for sample, with the same sample count and the
    // same timestamps; the delay is what the element reports.
    void framingHoldsAcrossOddBufferSizes()
    {
        Harness h;
        QVERIFY(h.element);
        bool ok = false;
        h.setMode(Mode::RNNoise);
        QVERIFY(h.waitForStatus(Mode::RNNoise, &ok));
        QVERIFY(ok);

        const std::vector<gsize> sizes = {1,   7,   479, 480, 481, 1000, 13,
                                          2048, 96, 1,   959, 961, 3,    5000};
        std::vector<float> sent;
        bool first = true;
        for (const gsize n : sizes) {
            const std::vector<float> chunk = rampSamples(sent.size(), n);
            // DISCONT on the first: the delay line restarts from a known
            // point, independent of the silence fed while waiting.
            QCOMPARE(int(h.push(chunk, first)), int(GST_FLOW_OK));
            first = false;
            sent.insert(sent.end(), chunk.begin(), chunk.end());
        }
        const std::vector<float> got = h.outputSamples();
        QCOMPARE(got.size(), sent.size());
        QVERIFY(h.outputPts == h.inputPts);
        const size_t delay = calls::noise::kFrameSamples;
        for (size_t k = 0; k < got.size(); ++k) {
            const float want = k < delay ? 0.0f : kRnnoiseGain * sent[k - delay];
            if (got[k] != want) {
                QFAIL(qPrintable(QStringLiteral("sample %1: got %2, want %3")
                                     .arg(k)
                                     .arg(double(got[k]))
                                     .arg(double(want))));
            }
        }
        QCOMPARE(h.status().latencySamples,
                 calls::noise::kFrameSamples + kFakeLatency);

        // ... and the LATENCY query carries it downstream.
        GstQuery *query = gst_query_new_latency();
        GstPad *out = gst_element_get_static_pad(h.element, "src");
        QVERIFY(gst_pad_query(out, query));
        gboolean live = FALSE;
        GstClockTime minimum = 0;
        GstClockTime maximum = 0;
        gst_query_parse_latency(query, &live, &minimum, &maximum);
        QCOMPARE(minimum,
                 gst_util_uint64_scale_int(calls::noise::kFrameSamples
                                               + kFakeLatency,
                                           GST_SECOND,
                                           calls::noise::kSampleRate));
        gst_object_unref(out);
        gst_query_unref(query);
    }

    // A switch between two backends lands between frames: no frame is ever
    // part one backend, part the other, and the framing carries on.
    void aBackendSwitchNeverSplitsAFrame()
    {
        Harness h;
        QVERIFY(h.element);
        bool ok = false;
        h.setMode(Mode::RNNoise);
        QVERIFY(h.waitForStatus(Mode::RNNoise, &ok));

        std::vector<float> sent;
        auto feed = [&](gsize n, bool discont) {
            const std::vector<float> chunk = rampSamples(sent.size(), n);
            h.push(chunk, discont);
            sent.insert(sent.end(), chunk.begin(), chunk.end());
        };
        feed(1000, true);
        h.setMode(Mode::DeepFilterNet);
        // Odd chunks while the new backend is being built, then plenty more.
        QElapsedTimer timer;
        timer.start();
        while (h.status().active != Mode::DeepFilterNet
               && timer.elapsed() < 5000) {
            feed(173, false);
            QThread::msleep(1);
        }
        QVERIFY(h.status().active == Mode::DeepFilterNet);
        feed(4000, false);

        const std::vector<float> got = h.outputSamples();
        const size_t delay = calls::noise::kFrameSamples;
        QVERIFY(got.size() == sent.size());
        int rnnoiseFrames = 0;
        int dfnFrames = 0;
        bool switched = false;
        for (size_t start = delay;
             start + calls::noise::kFrameSamples <= got.size();
             start += calls::noise::kFrameSamples) {
            const float gain = got[start] / sent[start - delay];
            QVERIFY2(gain == kRnnoiseGain || gain == kDfnGain,
                     qPrintable(QString::number(double(gain))));
            for (size_t k = start; k < start + calls::noise::kFrameSamples;
                 ++k) {
                QVERIFY2(got[k] == gain * sent[k - delay],
                         qPrintable(QStringLiteral("frame at %1 is mixed")
                                        .arg(start)));
            }
            if (gain == kDfnGain) {
                switched = true;
                ++dfnFrames;
            } else {
                QVERIFY2(!switched, "went back to the retired backend");
                ++rnnoiseFrames;
            }
        }
        QVERIFY(rnnoiseFrames > 0);
        QVERIFY(dfnFrames > 0);
    }

    // Malformed input never crashes and never reaches a backend as garbage.
    void malformedInputIsSurvived()
    {
        {
            Harness h;
            QVERIFY(h.element);
            bool ok = false;
            h.setMode(Mode::RNNoise);
            QVERIFY(h.waitForStatus(Mode::RNNoise, &ok));
            // A buffer that is not a whole number of samples, and an empty one.
            const char odd[7] = {1, 2, 3, 4, 5, 6, 7};
            QCOMPARE(int(h.pushBytes(odd, sizeof odd)), int(GST_FLOW_OK));
            QCOMPARE(int(h.pushBytes(nullptr, 0)), int(GST_FLOW_OK));
            QByteArray ragged(4 * 700 + 3, '\x01');
            QCOMPARE(int(h.pushBytes(ragged.constData(), gsize(ragged.size()))), int(GST_FLOW_OK));
            QCOMPARE(h.output.size(), qsizetype(7 + 0 + ragged.size()));
            // Non-finite and absurd samples.
            std::vector<float> nasty(2000, 0.1f);
            nasty[3] = std::numeric_limits<float>::quiet_NaN();
            nasty[500] = std::numeric_limits<float>::infinity();
            nasty[501] = -std::numeric_limits<float>::infinity();
            nasty[900] = 1e30f;
            nasty[1500] = -1e30f;
            h.clearOutput();
            QCOMPARE(int(h.push(nasty)), int(GST_FLOW_OK));
            QCOMPARE(int(h.push(std::vector<float>(2000, 0.1f))), int(GST_FLOW_OK));
            QVERIFY(!g_ledger.sawBadSample.load());
            for (const float v : h.outputSamples())
                QVERIFY(std::isfinite(v));
        }
        // Caps it cannot take are refused at negotiation, not crashed on.
        for (const char *caps :
             {"audio/x-raw,format=S16LE,rate=48000,channels=1,"
              "layout=interleaved",
              "audio/x-raw,format=F32LE,rate=44100,channels=1,"
              "layout=interleaved",
              "audio/x-raw,format=F32LE,rate=48000,channels=2,"
              "layout=interleaved"}) {
            Harness h(caps);
            QVERIFY(h.element);
            QVERIFY2(!h.capsAccepted, caps);
            const std::vector<float> samples(480, 0.1f);
            QVERIFY(h.push(samples) != GST_FLOW_OK);
        }
    }

    // A backend that fails to start (says !ok, has the wrong frame size,
    // returns nothing, or throws) leaves the microphone passing through,
    // unaltered, and says so.
    void aFailedBackendPassesThroughAndIsReported_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("not ok") << 0;
        QTest::newRow("wrong frame size") << 1;
        QTest::newRow("null") << 2;
        QTest::newRow("throws") << 3;
    }
    void aFailedBackendPassesThroughAndIsReported()
    {
        QFETCH(int, failure);
        calls::noise::setSuppressorFactoryForTest(
            [failure](Mode mode) -> std::unique_ptr<calls::noise::NoiseSuppressor> {
                switch (failure) {
                case 0:
                    return std::make_unique<FakeSuppressor>(mode, 0.5f, false);
                case 1:
                    return std::make_unique<FakeSuppressor>(mode, 0.5f, true,
                                                            256);
                case 2:
                    return nullptr;
                default:
                    throw std::runtime_error("backend construction failed");
                }
            });
        Harness h;
        QVERIFY(h.element);
        bool ok = true;
        h.setMode(Mode::DeepFilterNet);
        QVERIFY(h.waitForStatus(Mode::DeepFilterNet, &ok));
        QVERIFY(!ok);
        const auto status = h.status();
        QVERIFY(status.requested == Mode::DeepFilterNet);
        QVERIFY(status.active == Mode::Off);
        QVERIFY(!status.ok);
        const std::vector<float> samples = rampSamples(0, 2000);
        h.push(samples);
        QVERIFY(h.outputSamples() == samples);
        QCOMPARE(g_ledger.frames[size_t(Mode::DeepFilterNet)].load(), 0ll);
        // The refused instance is destroyed, not kept.
        QVERIFY(waitUntil([] { return g_ledger.live.load() == 0; }));
    }

    // Calls start and stop, modes change mid-build, elements die with a
    // build in flight: every backend and every element is released.
    void repeatedStartStopLeaksNothing()
    {
        for (int round = 0; round < 25; ++round) {
            Harness h;
            QVERIFY(h.element);
            const Mode first = round % 2 ? Mode::RNNoise : Mode::DeepFilterNet;
            h.setMode(first);
            if (round % 3 == 0)
                continue; // destroyed with the build still in flight
            bool ok = false;
            QVERIFY(h.waitForStatus(first, &ok));
            h.push(rampSamples(0, 1500));
            h.setMode(first == Mode::RNNoise ? Mode::DeepFilterNet
                                             : Mode::RNNoise);
            if (round % 3 == 1)
                continue; // second build in flight
            h.setMode(Mode::Off);
            h.setMode(first); // quick toggles: only the last may survive
            QVERIFY(h.waitForStatus(first, &ok));
            h.push(rampSamples(0, 1500));
        }
        QVERIFY2(waitUntil([] { return g_ledger.live.load() == 0; }),
                 qPrintable(QStringLiteral("%1 backends still alive")
                                .arg(g_ledger.live.load())));
        // Every harness of the whole run is gone by now; their elements must
        // be too once the pool has finished with them.
        QVERIFY2(waitUntil([] { return g_elementsAlive.load() == 0; }),
                 qPrintable(QStringLiteral("%1 elements outlived their harness")
                                .arg(g_elementsAlive.load())));
    }

    // The fragment the call and the mic test share: in every mode exactly one
    // suppressor is configured, and the result parses and runs.
    void theChainRunsExactlyOneSuppressor_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("dsp");
        QTest::addColumn<bool>("denoise");
        for (Mode mode : {Mode::Off, Mode::WebRtc, Mode::RNNoise,
                          Mode::DeepFilterNet}) {
            for (int shape = 0; shape < 4; ++shape) {
                QTest::newRow(qPrintable(QStringLiteral("%1 dsp=%2 denoise=%3")
                                             .arg(calls::noise::modeKey(mode))
                                             .arg(shape & 1)
                                             .arg((shape >> 1) & 1)))
                    << int(mode) << bool(shape & 1) << bool(shape & 2);
            }
        }
    }
    void theChainRunsExactlyOneSuppressor()
    {
        QFETCH(int, mode);
        QFETCH(bool, dsp);
        QFETCH(bool, denoise);
        const Mode m = Mode(mode);
        const QString fragment =
            calls::noise::voiceProcessingDescription(m, dsp, denoise);
        QVERIFY(fragment.isEmpty() || fragment.startsWith(QStringLiteral("! ")));
        QCOMPARE(fragment.contains(QStringLiteral("webrtcdsp")), dsp);
        QCOMPARE(fragment.contains(QLatin1String(
                     calls::noise::denoiseElementName())),
                 denoise);
        // webrtcdsp suppresses in WebRTC mode only; its gain control and
        // high-pass stay in every mode.
        QCOMPARE(fragment.contains(QStringLiteral("noise-suppression=true")),
                 dsp && m == Mode::WebRtc);
        if (dsp) {
            QVERIFY(fragment.contains(QStringLiteral("gain-control=true")));
            QVERIFY(fragment.contains(QStringLiteral("echo-cancel=false")));
            QVERIFY(!fragment.contains(QStringLiteral("high-pass-filter=false")));
        }
        if (denoise) {
            QVERIFY(fragment.contains(
                QStringLiteral("mode=%1").arg(calls::noise::modeKey(m))));
            // The float round trip must not add dither to the stream.
            QVERIFY(fragment.contains(QStringLiteral("dithering=none")));
        }
        // GStreamer accepts and runs it.
        if (dsp && !elementExists("webrtcdsp"))
            QSKIP("webrtcdsp is not in this GStreamer");
        const QString description =
            QStringLiteral("audiotestsrc num-buffers=30 samplesperbuffer=441 "
                           "! audio/x-raw,rate=44100,channels=2 "
                           "! audioconvert ! audioresample "
                           "! audio/x-raw,channels=1 %1"
                           "! volume ! fakesink sync=false")
                .arg(fragment);
        GError *error = nullptr;
        GstElement *pipeline =
            gst_parse_launch(description.toUtf8().constData(), &error);
        QVERIFY2(!error, error ? error->message : "");
        QVERIFY(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 10 * GST_SECOND,
            GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        QVERIFY(message);
        const bool eos = GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
        gst_message_unref(message);
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        QVERIFY2(eos, "the chain errored instead of reaching EOS");
    }

    // webrtcdsp reads its configuration only when it negotiates, so a live
    // WebRTC on/off REPLACES the element; the audio keeps flowing through
    // the new one.
    void aLiveWebrtcSwitchReplacesTheDspAndAudioKeepsFlowing()
    {
        if (!elementExists("webrtcdsp"))
            QSKIP("webrtcdsp is not in this GStreamer");
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            "audiotestsrc is-live=true wave=white-noise volume=0.1 "
            "samplesperbuffer=480 "
            "! audio/x-raw,format=S16LE,rate=48000,channels=1 "
            "! webrtcdsp name=micdsp echo-cancel=false gain-control=true "
            "noise-suppression=true "
            "! audioconvert ! fakesink name=out sync=false",
            &error);
        QVERIFY2(!error, error ? error->message : "");
        std::atomic<int> buffers{0};
        GstElement *out = gst_bin_get_by_name(GST_BIN(pipeline), "out");
        GstPad *outPad = gst_element_get_static_pad(out, "sink");
        gst_pad_add_probe(
            outPad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                ++*static_cast<std::atomic<int> *>(data);
                return GST_PAD_PROBE_OK;
            },
            &buffers, nullptr);
        gst_object_unref(outPad);
        gst_object_unref(out);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        QVERIFY(waitUntil([&] { return buffers.load() > 20; }));

        auto currentDsp = [pipeline] {
            return gst_bin_get_by_name(GST_BIN(pipeline), "micdsp");
        };
        auto suppressing = [&currentDsp]() -> int {
            GstElement *dsp = currentDsp();
            if (!dsp)
                return -1;
            gboolean on = FALSE;
            g_object_get(dsp, "noise-suppression", &on, nullptr);
            gst_object_unref(dsp);
            return on ? 1 : 0;
        };
        GstElement *before = currentDsp();
        calls::noise::applyMode(pipeline, Mode::RNNoise);
        QVERIFY(waitUntil([&] { return suppressing() == 0; }));
        GstElement *after = currentDsp();
        QVERIFY2(after != before, "the dsp was not replaced");
        const int atSwap = buffers.load();
        QVERIFY(waitUntil([&] { return buffers.load() > atSwap + 20; }));

        calls::noise::applyMode(pipeline, Mode::WebRtc);
        QVERIFY(waitUntil([&] { return suppressing() == 1; }));
        const int atSecondSwap = buffers.load();
        QVERIFY(waitUntil([&] { return buffers.load() > atSecondSwap + 20; }));

        // No error anywhere in the pipeline.
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *errorMessage =
            gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        QVERIFY(!errorMessage);
        gst_object_unref(bus);

        gst_object_unref(before);
        gst_object_unref(after);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }

    // Before it negotiates, webrtcdsp just takes the property: no swap.
    // Live finding 2026-10-06: after unmute the far end first heard 60-80 ms
    // of the moment of MUTING with RNNoise and DeepFilterNet. The delay line
    // was cleared on the valve's discontinuity, but the backend's own history
    // was not. A backend whose reset is real-time safe is reset there; one
    // that has not declared it is left alone (its reset may allocate).
    // Review LOW 4: DeepFilterNet after a caught panic passes audio through
    // while the element went on reporting it active and ok (40 ms latency
    // included), so nothing fell back and nothing was logged. The element
    // now retires such a backend and reports the mode as failed, once.
    void aBackendThatFailsMidCallIsRetiredAndReported()
    {
        calls::noise::setSuppressorFactoryForTest(
            [](Mode mode) -> std::unique_ptr<calls::noise::NoiseSuppressor> {
                return std::make_unique<FailingSuppressor>(mode, 5);
            });
        Harness h;
        QVERIFY(h.element);
        h.setMode(Mode::DeepFilterNet);
        QVERIFY(h.waitForStatusWith(Mode::DeepFilterNet, true));
        QVERIFY(h.status().active == Mode::DeepFilterNet);
        // Past its fifth frame it fails.
        h.push(rampSamples(0, 4000));
        QVERIFY(h.waitForStatusWith(Mode::DeepFilterNet, false));
        const auto status = h.status();
        QVERIFY(status.active == Mode::Off);
        QVERIFY(!status.ok);
        QCOMPARE(status.latencySamples, 0);
        // Pass-through from here, bit-exact and undelayed, and no second
        // report for the same failure.
        h.clearOutput();
        const std::vector<float> after = rampSamples(3, 2000);
        h.push(after);
        QVERIFY(h.outputSamples() == after);
        bool again = true;
        QVERIFY(!h.waitForStatus(Mode::DeepFilterNet, &again, 300));
        // The dead backend is destroyed, off the streaming thread.
        QVERIFY(waitUntil([] { return g_ledger.live.load() == 0; }));
    }

    // Review LOW 6: after a backend failed, selecting the same mode again
    // did nothing (the element saw no change), so retrying needed a detour
    // through another mode. A re-select of a FAILED mode builds it again; a
    // re-select of a running one still does nothing.
    void reselectingAFailedModeRetriesIt()
    {
        static std::atomic<int> attempts{0};
        attempts.store(0);
        calls::noise::setSuppressorFactoryForTest(
            [](Mode mode) -> std::unique_ptr<calls::noise::NoiseSuppressor> {
                // The first build fails, the next ones work.
                if (attempts.fetch_add(1) == 0)
                    return nullptr;
                return fakeFactory(mode);
            });
        Harness h;
        QVERIFY(h.element);
        h.setMode(Mode::RNNoise);
        QVERIFY(h.waitForStatusWith(Mode::RNNoise, false));
        QCOMPARE(attempts.load(), 1);
        h.setMode(Mode::RNNoise);
        QVERIFY(h.waitForStatusWith(Mode::RNNoise, true));
        QVERIFY(h.status().active == Mode::RNNoise);
        QCOMPARE(attempts.load(), 2);
        // Running: the same mode again builds nothing.
        h.setMode(Mode::RNNoise);
        bool ok = false;
        QVERIFY(!h.waitForStatus(Mode::RNNoise, &ok, 300));
        QCOMPARE(attempts.load(), 2);
    }

    void aDiscontinuityResetsOnlyARealtimeSafeBackend()
    {
        Harness h;
        QVERIFY(h.element);
        bool ok = false;
        h.setMode(Mode::RNNoise); // the fake declares a real-time-safe reset
        QVERIFY(h.waitForStatus(Mode::RNNoise, &ok));
        QVERIFY(ok);
        h.push(rampSamples(0, 1000));
        const int before = g_ledger.resets.load();
        h.push(rampSamples(0, 1000)); // no gap: no reset
        QCOMPARE(g_ledger.resets.load(), before);
        h.push(rampSamples(0, 700), /*discont=*/true);
        QCOMPARE(g_ledger.resets.load(), before + 1);

        h.setMode(Mode::DeepFilterNet); // the fake does not declare it
        QVERIFY(h.waitForStatus(Mode::DeepFilterNet, &ok));
        QVERIFY(ok);
        const int dfnBefore = g_ledger.resets.load();
        h.push(rampSamples(0, 700), /*discont=*/true);
        QCOMPARE(g_ledger.resets.load(), dfnBefore);
        // Either way the delay line restarts: the first frame after the gap
        // is silence, never audio from before it.
        h.clearOutput();
        h.push(rampSamples(0, 480), /*discont=*/true);
        for (const float v : h.outputSamples())
            QCOMPARE(v, 0.0f);
    }

    // Live finding 2026-10-06: a WebRTC -> DeepFilterNet switch left about a
    // second (the model's build) with NO suppressor, because webrtcdsp was
    // replaced at once. Waiting for the denoiser keeps WebRTC suppressing
    // until the caller is told the new backend runs.
    void aSwitchToANeuralModeKeepsWebrtcSuppressingUntilTheDenoiserReports()
    {
        if (!elementExists("webrtcdsp"))
            QSKIP("webrtcdsp is not in this GStreamer");
        static std::atomic<bool> release{false};
        release.store(false);
        calls::noise::setSuppressorFactoryForTest(
            [](Mode mode) -> std::unique_ptr<calls::noise::NoiseSuppressor> {
                // A slow build, like DeepFilterNet unpacking its model.
                QElapsedTimer waited;
                waited.start();
                while (!release.load() && waited.elapsed() < 10000)
                    QThread::msleep(5);
                return fakeFactory(mode);
            });
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            "audiotestsrc is-live=true wave=white-noise volume=0.1 "
            "samplesperbuffer=480 "
            "! audio/x-raw,format=F32LE,rate=48000,channels=1,"
            "layout=interleaved "
            "! lightningdenoise name=micdenoise mode=webrtc "
            "! audioconvert ! audio/x-raw,format=S16LE,rate=48000 "
            "! webrtcdsp name=micdsp echo-cancel=false gain-control=true "
            "noise-suppression=true "
            "! audioconvert ! fakesink sync=false",
            &error);
        QVERIFY2(!error, error ? error->message : "");
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        auto suppressing = [pipeline]() -> int {
            GstElement *dsp = gst_bin_get_by_name(GST_BIN(pipeline), "micdsp");
            if (!dsp)
                return -1;
            gboolean on = FALSE;
            g_object_get(dsp, "noise-suppression", &on, nullptr);
            gst_object_unref(dsp);
            return on ? 1 : 0;
        };
        GstElement *denoise = gst_bin_get_by_name(GST_BIN(pipeline), "micdenoise");
        QVERIFY(denoise);
        // Negotiated and flowing, so a dsp change would be a real swap.
        QVERIFY(waitUntil([denoise] {
            GstPad *pad = gst_element_get_static_pad(denoise, "src");
            const bool negotiated = gst_pad_has_current_caps(pad);
            gst_object_unref(pad);
            return negotiated;
        }));

        const QString did = calls::noise::applyMode(
            pipeline, Mode::RNNoise, calls::noise::DspSwitch::WaitForDenoiser);
        QVERIFY2(did.contains(QStringLiteral("waits")), qPrintable(did));
        // The backend is still building: WebRTC must still be suppressing.
        QTest::qWait(400);
        QCOMPARE(suppressing(), 1);
        QVERIFY(calls::noise::denoiseStatus(denoise).active == Mode::Off);

        release.store(true);
        QVERIFY(waitUntil([denoise] {
            return calls::noise::denoiseStatus(denoise).active == Mode::RNNoise;
        }));
        // The caller follows the report.
        calls::noise::applyDspMode(pipeline, Mode::RNNoise);
        QVERIFY(waitUntil([&] { return suppressing() == 0; }));

        // Into WebRTC (or Off) nothing is built: the switch is at once even
        // when asked to wait.
        calls::noise::applyMode(pipeline, Mode::WebRtc,
                                calls::noise::DspSwitch::WaitForDenoiser);
        QVERIFY(waitUntil([&] { return suppressing() == 1; }));

        gst_object_unref(denoise);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        release.store(true);
    }

    void aDspThatHasNotNegotiatedIsConfiguredInPlace()
    {
        if (!elementExists("webrtcdsp"))
            QSKIP("webrtcdsp is not in this GStreamer");
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            "audiotestsrc ! audio/x-raw,format=S16LE,rate=48000,channels=1 "
            "! webrtcdsp name=micdsp noise-suppression=true ! fakesink",
            &error);
        QVERIFY2(!error, error ? error->message : "");
        GstElement *before = gst_bin_get_by_name(GST_BIN(pipeline), "micdsp");
        calls::noise::applyMode(pipeline, Mode::Off);
        GstElement *after = gst_bin_get_by_name(GST_BIN(pipeline), "micdsp");
        QCOMPARE(after, before);
        gboolean on = TRUE;
        g_object_get(after, "noise-suppression", &on, nullptr);
        QVERIFY(!on);
        gst_object_unref(before);
        gst_object_unref(after);
        gst_object_unref(pipeline);
    }

    // The real backends, when this build carries them, really suppress:
    // white noise through the element comes out much quieter. (The speech
    // quality comparison is a separate, longer test.)
    void theRealBackendsSuppressNoiseWhenBuiltIn_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("rnnoise") << int(Mode::RNNoise);
        QTest::newRow("deepfilternet") << int(Mode::DeepFilterNet);
    }
    void theRealBackendsSuppressNoiseWhenBuiltIn()
    {
        QFETCH(int, mode);
        const Mode m = Mode(mode);
        if (!calls::noise::backendCompiledIn(m))
            QSKIP("this build does not carry the backend");
        calls::noise::setSuppressorFactoryForTest({});
        Harness h;
        QVERIFY(h.element);
        bool ok = false;
        h.setMode(m);
        QVERIFY(h.waitForStatus(m, &ok, 20000));
        QVERIFY2(ok, "the real backend failed to start");
        QVERIFY(h.status().active == m);
        // Two seconds of deterministic white noise at about -23 dBFS RMS.
        std::vector<float> noise(2 * calls::noise::kSampleRate);
        quint32 seed = 0xC0FFEEu;
        for (float &v : noise) {
            seed = seed * 1664525u + 1013904223u;
            v = (float(seed >> 8) / float(1u << 24) - 0.5f) * 0.25f;
        }
        for (size_t i = 0; i < noise.size(); i += 960)
            h.push(std::vector<float>(noise.begin() + long(i),
                                      noise.begin() + long(i + 960)));
        const std::vector<float> got = h.outputSamples();
        QCOMPARE(got.size(), noise.size());
        auto rms = [](const float *p, size_t n) {
            double sum = 0;
            for (size_t i = 0; i < n; ++i)
                sum += double(p[i]) * p[i];
            return std::sqrt(sum / double(n));
        };
        // The second half: past start-up and the adaptation of either model.
        const size_t half = noise.size() / 2;
        const double in = rms(noise.data() + half, half);
        const double outRms = rms(got.data() + half, half);
        QVERIFY2(outRms < 0.5 * in,
                 qPrintable(QStringLiteral("in %1 out %2").arg(in).arg(outRms)));
        QVERIFY(h.status().frames > 150);
    }
};

QTEST_GUILESS_MAIN(NoiseSuppressionTest)
#include "NoiseSuppressionTest.moc"
