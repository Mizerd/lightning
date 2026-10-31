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
    bool outgoing = false;
    bool screen = false;
    qint64 lastMs = -1;
    quint64 lastBytes = 0, lastFrames = 0;

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
        QVariantMap row{
            {QStringLiteral("outgoing"), outgoing},
            {QStringLiteral("screen"), screen},
            {QStringLiteral("codec"), QStringLiteral("VP8")},
            {QStringLiteral("encoder"), encoder},
            {QStringLiteral("source"), source},
            {QStringLiteral("scaling"), scaling},
            {QStringLiteral("width"), width.load()},
            {QStringLiteral("height"), height.load()},
            {QStringLiteral("fps"), rate(f, lastFrames, elapsed, 1000)},
            {QStringLiteral("kbps"), rate(b, lastBytes, elapsed, 8)},
            {QStringLiteral("packets"), QVariant::fromValue(packets.load())},
            {QStringLiteral("decrypted"), QVariant::fromValue(decrypted.load())},
            {QStringLiteral("dropped"), QVariant::fromValue(dropped.load())}
        };
        lastMs = now;
        lastBytes = b;
        lastFrames = f;
        return row;
    }
};
}
