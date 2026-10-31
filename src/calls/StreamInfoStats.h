#pragma once

#include <atomic>
#include <QString>
#include <QVariantMap>

namespace lightning::calls {
// One track's lifetime, shared by its bin and probes. Never holds an engine,
// device identifier, credentials or key material. Bytes include RTP headers;
// frames are counted after send encryption, or at the decoded receive sink.
struct StreamInfoStats {
    std::atomic<quint64> bytes{0}, packets{0}, frames{0}, decrypted{0}, dropped{0};
    std::atomic<int> width{0}, height{0};
    QString source, encoder, scaling;
    /// What the track carries, not what SDP once offered: a hardware share is
    /// H.264 and a software fallback is not.
    QString codec = QStringLiteral("VP8");
    bool outgoing = false;
    bool screen = false;
    qint64 lastMs = -1;
    quint64 lastBytes = 0, lastFrames = 0;
    double lastFps = -1, lastKbps = -1;
    /// Two surfaces can poll one track in the same tick (the stage and its
    /// full-screen window). A second sample that soon would divide one frame
    /// by two milliseconds, or nothing by nothing, and restart the window.
    static constexpr qint64 kMinSampleMs = 200;

    static double rate(quint64 current, quint64 previous, qint64 elapsedMs,
                       double multiplier)
    {
        return elapsedMs > 0 && current >= previous
            ? double(current - previous) * multiplier / double(elapsedMs) : -1;
    }

    QVariantMap sample(qint64 now)
    {
        const auto b = bytes.load();
        const auto f = frames.load();
        const auto elapsed = lastMs < 0 ? 0 : now - lastMs;
        const bool tooSoon = lastMs >= 0 && elapsed < kMinSampleMs;
        const double fps = tooSoon ? lastFps : rate(f, lastFrames, elapsed, 1000);
        const double kbps = tooSoon ? lastKbps : rate(b, lastBytes, elapsed, 8);
        QVariantMap row{
            {QStringLiteral("outgoing"), outgoing},
            {QStringLiteral("screen"), screen},
            {QStringLiteral("codec"), codec},
            {QStringLiteral("encoder"), encoder},
            {QStringLiteral("source"), source},
            {QStringLiteral("scaling"), scaling},
            {QStringLiteral("width"), width.load()},
            {QStringLiteral("height"), height.load()},
            {QStringLiteral("fps"), fps},
            {QStringLiteral("kbps"), kbps},
            {QStringLiteral("packets"), QVariant::fromValue(packets.load())},
            {QStringLiteral("decrypted"), QVariant::fromValue(decrypted.load())},
            {QStringLiteral("dropped"), QVariant::fromValue(dropped.load())}
        };
        if (tooSoon)
            return row;
        lastMs = now;
        lastBytes = b;
        lastFrames = f;
        lastFps = fps;
        lastKbps = kbps;
        return row;
    }
};
}
