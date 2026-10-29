#include "calls/MacSystemAudioSrc.h"
#include "calls/ShareAudioSources.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#include <gst/base/gstpushsrc.h>
#include <gst/app/gstappsink.h>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <cstdlib>

namespace {
struct AudioQueue {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<GstBuffer *> buffers;
    bool flushing = false;
    bool failed = false;
    ~AudioQueue() { for (auto *buffer : buffers) gst_buffer_unref(buffer); }
};
}

@interface LightningAudioOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::shared_ptr<AudioQueue> audioQueue;
}
@end

@implementation LightningAudioOutput
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    NSLog(@"Lightning system audio stopped: %@", error.localizedDescription);
    std::lock_guard lock(audioQueue->mutex);
    audioQueue->failed = true;
    audioQueue->ready.notify_all();
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample
       ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeAudio || !CMSampleBufferIsValid(sample)) return;
    const auto *format = CMAudioFormatDescriptionGetStreamBasicDescription(
        CMSampleBufferGetFormatDescription(sample));
    if (!format || format->mFormatID != kAudioFormatLinearPCM
        || !(format->mFormatFlags & kAudioFormatFlagIsFloat)
        || format->mBitsPerChannel != 32 || format->mChannelsPerFrame != 2
        || format->mSampleRate != 48000) {
        std::lock_guard lock(audioQueue->mutex);
        audioQueue->failed = true;
        audioQueue->ready.notify_all();
        return;
    }
    // ScreenCaptureKit supplies planar float PCM; accept interleaved PCM too.
    size_t size = 0;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sample, &size, nullptr, 0, nullptr, nullptr, 0, nullptr);
    auto list = std::unique_ptr<AudioBufferList, decltype(&free)>(
        static_cast<AudioBufferList *>(malloc(size)), free);
    CMBlockBufferRef block = nullptr;
    if (!list || CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sample, nullptr, list.get(), size, nullptr, nullptr,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block) != noErr)
        return;
    const size_t frames = CMSampleBufferGetNumSamples(sample);
    const bool planar = format->mFormatFlags & kAudioFormatFlagIsNonInterleaved;
    const bool valid = frames > 0 && (planar
        ? list->mNumberBuffers == 2
          && list->mBuffers[0].mDataByteSize >= frames * sizeof(float)
          && list->mBuffers[1].mDataByteSize >= frames * sizeof(float)
        : list->mNumberBuffers == 1
          && list->mBuffers[0].mDataByteSize >= frames * 2 * sizeof(float));
    if (!valid) { if (block) CFRelease(block); return; }
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frames * 2 * sizeof(float), nullptr);
    GstMapInfo map;
    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        if (buffer) gst_buffer_unref(buffer);
        if (block) CFRelease(block);
        return;
    }
    auto *out = reinterpret_cast<float *>(map.data);
    const auto *left = static_cast<const float *>(list->mBuffers[0].mData);
    if (planar) {
        const auto *right = static_cast<const float *>(list->mBuffers[1].mData);
        for (size_t i = 0; i < frames; ++i) { out[2*i] = left[i]; out[2*i+1] = right[i]; }
    } else {
        memcpy(out, left, map.size);
    }
    gst_buffer_unmap(buffer, &map);
    if (block) CFRelease(block);
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(frames, GST_SECOND, 48000);
    // GstBaseSrc stamps running time when the buffer leaves this bounded queue.
    std::lock_guard lock(audioQueue->mutex);
    if (audioQueue->flushing) { gst_buffer_unref(buffer); return; }
    while (audioQueue->buffers.size() >= 12) {
        gst_buffer_unref(audioQueue->buffers.front());
        audioQueue->buffers.pop_front();
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
    }
    audioQueue->buffers.push_back(buffer);
    audioQueue->ready.notify_one();
}
@end

namespace {
struct Session {
    std::shared_ptr<AudioQueue> queue = std::make_shared<AudioQueue>();
    SCStream *__strong stream = nil;
    LightningAudioOutput *__strong output = nil;
};
typedef struct { GstPushSrc parent; Session *session; } LightningMacAudio;
typedef struct { GstPushSrcClass parent; } LightningMacAudioClass;
G_DEFINE_TYPE(LightningMacAudio, lightning_mac_audio, GST_TYPE_PUSH_SRC)

gboolean start(GstBaseSrc *base)
{
    @autoreleasepool {
        auto *self = reinterpret_cast<LightningMacAudio *>(base);
        delete self->session;
        self->session = new Session;
        auto *session = self->session;
        __block SCShareableContent *content = nil;
        __block NSError *failure = nil;
        dispatch_semaphore_t fetched = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentExcludingDesktopWindows:YES
            onScreenWindowsOnly:YES completionHandler:^(SCShareableContent *value, NSError *error) {
                content = value; failure = error; dispatch_semaphore_signal(fetched);
            }];
        if (dispatch_semaphore_wait(fetched, dispatch_time(DISPATCH_TIME_NOW, 10*NSEC_PER_SEC))
            || failure || content.displays.count == 0) {
            GST_ELEMENT_ERROR(base, RESOURCE, OPEN_READ,
                ("Allow Lightning in System Settings > Privacy & Security > Screen & System Audio Recording"),
                ("ScreenCaptureKit: %s", failure ? failure.localizedDescription.UTF8String : "no display or timed out"));
            return FALSE;
        }
        SCContentFilter *filter = [[SCContentFilter alloc]
            initWithDisplay:content.displays.firstObject excludingWindows:@[]];
        SCStreamConfiguration *config = [SCStreamConfiguration new];
        config.capturesAudio = YES;
        // The remote participants' playback must never be echoed back to them.
        config.excludesCurrentProcessAudio = YES;
        config.sampleRate = 48000;
        config.channelCount = 2;
        // Only attach an audio output. No screen images leave this stream.
        config.width = 2; config.height = 2;
        config.minimumFrameInterval = CMTimeMake(1, 1);
        session->output = [LightningAudioOutput new];
        session->output->audioQueue = session->queue;
        session->stream = [[SCStream alloc] initWithFilter:filter
            configuration:config delegate:session->output];
        NSError *addError = nil;
        if (![session->stream addStreamOutput:session->output type:SCStreamOutputTypeAudio
            sampleHandlerQueue:dispatch_queue_create("net.smetonis.lightning.share-audio", DISPATCH_QUEUE_SERIAL)
            error:&addError]) {
            GST_ELEMENT_ERROR(base, RESOURCE, OPEN_READ, ("Cannot attach system audio capture"),
                ("%s", addError.localizedDescription.UTF8String));
            return FALSE;
        }
        dispatch_semaphore_t started = dispatch_semaphore_create(0);
        __block NSError *startError = nil;
        [session->stream startCaptureWithCompletionHandler:^(NSError *error) {
            startError = error; dispatch_semaphore_signal(started);
        }];
        if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, 10*NSEC_PER_SEC)) || startError) {
            [session->stream stopCaptureWithCompletionHandler:^(NSError *) {}];
            GST_ELEMENT_ERROR(base, RESOURCE, OPEN_READ, ("Cannot start system audio capture"),
                ("%s", startError ? startError.localizedDescription.UTF8String : "timed out"));
            return FALSE;
        }
        GstCaps *caps = gst_caps_from_string("audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2");
        gboolean ok = gst_base_src_set_caps(base, caps);
        gst_caps_unref(caps);
        return ok;
    }
}
gboolean unlock(GstBaseSrc *base)
{
    auto *session = reinterpret_cast<LightningMacAudio *>(base)->session;
    if (session) {
        std::lock_guard lock(session->queue->mutex);
        session->queue->flushing = true;
        session->queue->ready.notify_all();
    }
    return TRUE;
}
gboolean unlockStop(GstBaseSrc *base)
{
    auto *session = reinterpret_cast<LightningMacAudio *>(base)->session;
    if (session) { std::lock_guard lock(session->queue->mutex); session->queue->flushing = false; }
    return TRUE;
}
gboolean stop(GstBaseSrc *base)
{
    auto *session = reinterpret_cast<LightningMacAudio *>(base)->session;
    unlock(base);
    if (session && session->stream) {
        [session->stream stopCaptureWithCompletionHandler:^(NSError *) {}];
        session->stream = nil;
        session->output = nil;
    }
    return TRUE;
}
GstFlowReturn create(GstPushSrc *src, GstBuffer **buffer)
{
    auto queue = reinterpret_cast<LightningMacAudio *>(src)->session->queue;
    std::unique_lock lock(queue->mutex);
    queue->ready.wait(lock, [&] { return queue->flushing || queue->failed || !queue->buffers.empty(); });
    if (queue->flushing) return GST_FLOW_FLUSHING;
    if (queue->failed) {
        GST_ELEMENT_ERROR(src, RESOURCE, READ, ("System audio capture stopped"), (nullptr));
        return GST_FLOW_ERROR;
    }
    *buffer = queue->buffers.front(); queue->buffers.pop_front();
    return GST_FLOW_OK;
}
void finalize(GObject *object)
{
    auto *self = reinterpret_cast<LightningMacAudio *>(object);
    stop(GST_BASE_SRC(self));
    delete self->session;
    G_OBJECT_CLASS(lightning_mac_audio_parent_class)->finalize(object);
}
void lightning_mac_audio_class_init(LightningMacAudioClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = finalize;
    auto *base = GST_BASE_SRC_CLASS(klass);
    base->start = start; base->stop = stop;
    base->unlock = unlock; base->unlock_stop = unlockStop;
    GST_PUSH_SRC_CLASS(klass)->create = create;
    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
        "macOS system audio", "Source/Audio", "ScreenCaptureKit audio excluding Lightning", "Lightning contributors");
    GstCaps *caps = gst_caps_from_string("audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2");
    gst_element_class_add_pad_template(GST_ELEMENT_CLASS(klass),
        gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
}
void lightning_mac_audio_init(LightningMacAudio *self)
{
    self->session = nullptr;
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), TRUE);
}
}

namespace lightning::macaudio {
void registerSource()
{
    static std::once_flag once;
    std::call_once(once, [] { gst_element_register(nullptr, "lightningmacsystemaudiosrc",
        GST_RANK_NONE, lightning_mac_audio_get_type()); });
}
int captureDiagnostic()
{
    GError *error = nullptr;
    // Exercise the actual share track's Opus/RTP chain, then decode locally.
    const QString description = lightning::shareaudio::encodedTrackDescription(
        QStringLiteral("lightningmacsystemaudiosrc name=sharesrc"), 12345)
        + QStringLiteral(" ! rtpopusdepay ! opusdec ! audioconvert "
                         "! audio/x-raw,format=F32LE,layout=interleaved,channels=2,rate=48000 "
                         "! appsink name=captured sync=false max-buffers=8 drop=true");
    GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (!pipeline || error) { if (error) g_error_free(error); if (pipeline) gst_object_unref(pipeline); return 1; }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "captured");
    const bool started = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    // Optional same-process playback verifies the exclusion used by real calls.
    GstElement *playback = nullptr;
    bool playbackStarted = false;
    const bool selfTest = g_getenv("LIGHTNING_TEST_MAC_PLAYBACK") != nullptr;
    if (started && selfTest) {
        playback = gst_parse_launch("audiotestsrc is-live=true freq=997 volume=0.1 ! audioconvert ! osxaudiosink", nullptr);
        if (playback) {
            gst_element_set_state(playback, GST_STATE_PLAYING);
            GstState state = GST_STATE_NULL;
            playbackStarted = gst_element_get_state(playback, &state, nullptr, GST_SECOND)
                != GST_STATE_CHANGE_FAILURE && state == GST_STATE_PLAYING;
        }
    }
    size_t frames = 0; double peak = 0;
    double toneReal = 0, toneImaginary = 0, tonePeak = 0;
    size_t toneFrames = 0;
    const GstClockTime deadline = gst_util_get_timestamp() + 7*GST_SECOND;
    if (started) while (gst_util_get_timestamp() < deadline) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100*GST_MSECOND);
        if (!sample) continue;
        GstMapInfo map;
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            const float *values = reinterpret_cast<const float *>(map.data);
            for (size_t j = 0; j < map.size/sizeof(float); ++j) peak = std::max(peak, double(std::abs(values[j])));
            for (size_t j = 0; j < map.size/(2*sizeof(float)); ++j) {
                const double value = (values[2*j] + values[2*j+1]) * 0.5;
                const double phase = 2 * 3.141592653589793 * 997 * toneFrames / 48000;
                toneReal += value * std::cos(phase);
                toneImaginary += value * std::sin(phase);
                if (++toneFrames == 4800) {
                    tonePeak = std::max(tonePeak, 2 * std::hypot(toneReal, toneImaginary) / toneFrames);
                    toneReal = toneImaginary = 0;
                    toneFrames = 0;
                }
            }
            frames += map.size/(2*sizeof(float));
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
    }
    if (playback) { gst_element_set_state(playback, GST_STATE_NULL); gst_object_unref(playback); }
    GstBus *bus = gst_element_get_bus(pipeline);
    if (GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
        GError *captureError = nullptr;
        gchar *details = nullptr;
        gst_message_parse_error(message, &captureError, &details);
        fprintf(stderr, "macOS capture failed: %s\n", captureError->message);
        g_error_free(captureError); g_free(details); gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink); gst_object_unref(pipeline);
    printf("macOS system audio RTP roundtrip: frames=%zu peak=%.6f tone-997Hz=%.6f excludes-current-process=yes\n", frames, peak, tonePeak);
    return frames > 0 && (selfTest ? playbackStarted && tonePeak < 0.001 : peak > 0.0001) ? 0 : 1;
}
}
