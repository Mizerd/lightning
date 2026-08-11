#pragma once

#include <QString>
#include <atomic>

// Bounds the log volume of Qt Multimedia's per-frame VAAPI texture-export
// failures. On hardware where vaExportSurfaceHandle cannot export a decoded
// surface to a GL/Vulkan texture, Qt's FFmpeg backend retries — and warns —
// for EVERY frame: a 60 fps video emits thousands of identical lines
// ("vaExportSurfaceHandle failed", "failed to get textures for frame"),
// competing with the UI thread for stderr/journal I/O. Playback itself
// continues through Qt's CPU fallback; the spam is the app-fixable part.
//
// The gate passes the first kPassThrough occurrences through untouched (the
// diagnosis must stay visible), then drops the rest, emitting a one-line
// summary every kSummaryEvery-th occurrence so the ongoing condition is
// still observable. Non-matching messages always print. Thread-safe: Qt
// invokes message handlers from any thread, including render threads.
class VaapiLogGate
{
public:
    enum class Action { Print, Drop, Summary };

    static bool matches(const QString &message)
    {
        return message.contains(QLatin1String("vaExportSurfaceHandle failed"))
            || message.contains(
                QLatin1String("failed to get textures for frame"));
    }

    Action classify(const QString &message)
    {
        if (!matches(message))
            return Action::Print;
        const qint64 n = ++m_seen;
        if (n <= kPassThrough)
            return Action::Print;
        if (n % kSummaryEvery == 0)
            return Action::Summary;
        return Action::Drop;
    }

    qint64 seen() const { return m_seen.load(); }

    QString summaryLine() const
    {
        return QStringLiteral(
                   "VAAPI texture export keeps failing (%1 occurrences "
                   "suppressed) — video decodes through the CPU fallback; "
                   "set QT_DISABLE_HW_TEXTURES_CONVERSION=1 to silence the "
                   "export attempts entirely")
            .arg(m_seen.load());
    }

    static constexpr qint64 kPassThrough = 20;
    static constexpr qint64 kSummaryEvery = 500;

private:
    std::atomic<qint64> m_seen{0};
};
