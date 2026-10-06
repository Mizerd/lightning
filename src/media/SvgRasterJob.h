#pragma once

// Runs an SVG -> PNG conversion (SvgRaster.h) in a HELPER PROCESS: the app's
// own binary started with the hidden `--rasterize-svg IN OUT MIN MAX` flag.
// QtSvg cannot be interrupted, so a pathological file (nested `<use>`
// multiplication, huge filters) would otherwise hold a thread for ever; a
// process can be killed, and a crash in the renderer takes down the helper, not
// the app.
//
// Protocol: the parent writes the SVG to IN, the helper writes the PNG to OUT
// and exits 0, or writes a short reason code to OUT and exits 3. Files rather
// than pipes: a Windows GUI-subsystem binary has no usable stdout.
//
// Everything here runs on the GUI thread (QProcess is event driven); the
// callback is delivered on it, and only while `context` is alive. Up to
// kMaxParallel helpers run at once, the rest queue.

#include "media/SvgRaster.h"

#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <deque>
#include <functional>
#include <memory>

namespace lightning::svgraster {

inline constexpr int kMaxParallel = 3;
inline constexpr int kHelperRefusedExit = 3;

using Callback = std::function<void(const Result &)>;

/// Test and wiring seams. All GUI-thread only.
struct Hooks {
    /// Where the job's scratch directory is created. Empty: the system temp
    /// directory. The app points it at the portable media scratch root.
    std::function<QString()> scratchRoot;
    /// Mark a scratch directory live / release it, so another instance's
    /// startup sweep cannot delete files an upload is still reading. Optional.
    std::function<void(const QString &)> holdScratchDir;
    std::function<void(const QString &)> releaseScratchDir;
    /// A replacement helper. `args` may use %IN %OUT %MIN %MAX; an empty
    /// program means the app binary with the real flag.
    QString program;
    QStringList args;
    /// Convert in this process instead of spawning (model-level tests).
    bool inProcess = false;
    /// 0 = kDefaultTimeoutMs.
    int timeoutMs = 0;
};

inline Hooks &hooks()
{
    static Hooks h;
    return h;
}

namespace detail {

struct Job {
    QByteArray svg;
    Policy policy;
    QPointer<QObject> context;
    Callback done;
    bool noText = false; // retry without a GUI app (text refused)
};

inline std::deque<Job> &pending()
{
    static std::deque<Job> q;
    return q;
}

inline int &running()
{
    static int n = 0;
    return n;
}

inline void pump();

inline void deliver(const Job &job, const Result &result)
{
    if (job.context && job.done)
        job.done(result);
}

inline Result refusal(const QString &code)
{
    Result r;
    r.refusal = code;
    return r;
}

inline void startProcess(Job job)
{
    // Cheap Qt Core pre-screen: a plainly bad file never costs a process.
    const QString early = screen(job.svg, true);
    if (!early.isEmpty()) {
        deliver(job, refusal(early));
        --running();
        pump();
        return;
    }
    if (!available()) {
        deliver(job, refusal(QStringLiteral("unavailable")));
        --running();
        pump();
        return;
    }
    if (hooks().inProcess) {
        // Still asynchronous, so callers see one ordering in every mode.
        QTimer::singleShot(0, QCoreApplication::instance(), [job] {
            const Result r = rasterize(job.svg, job.policy, true);
            deliver(job, r);
            --running();
            pump();
        });
        return;
    }

    const QString root = hooks().scratchRoot ? hooks().scratchRoot() : QString();
    auto dir = std::make_shared<QTemporaryDir>(
        (root.isEmpty() ? QDir::tempPath() : root)
        + QStringLiteral("/lightning-svg-XXXXXX"));
    const QString in = dir->filePath(QStringLiteral("in.svg"));
    const QString out = dir->filePath(QStringLiteral("out.bin"));
    QFile source(in);
    if (!dir->isValid() || !source.open(QIODevice::WriteOnly)
        || source.write(job.svg) != job.svg.size()) {
        deliver(job, refusal(QStringLiteral("helper_failed")));
        --running();
        pump();
        return;
    }
    source.close();

    QString program = hooks().program;
    QStringList args;
    if (program.isEmpty()) {
        program = QCoreApplication::applicationFilePath();
        args = { QStringLiteral("--rasterize-svg"), in, out,
                 QString::number(job.policy.minEdge),
                 QString::number(job.policy.maxEdge) };
    } else {
        for (QString a : hooks().args) {
            a.replace(QStringLiteral("%IN"), in);
            a.replace(QStringLiteral("%OUT"), out);
            a.replace(QStringLiteral("%MIN"), QString::number(job.policy.minEdge));
            a.replace(QStringLiteral("%MAX"), QString::number(job.policy.maxEdge));
            args << a;
        }
    }
    if (job.noText)
        args << QStringLiteral("--no-text");

    auto *process = new QProcess(QCoreApplication::instance());
    process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    process->setStandardInputFile(QProcess::nullDevice());
    process->setStandardOutputFile(QProcess::nullDevice());
    auto timedOut = std::make_shared<bool>(false);
    auto finished = std::make_shared<bool>(false);

    const auto finish = [job, process, dir, out, timedOut, finished](
                            const QString &failure, int exitCode,
                            QProcess::ExitStatus status) {
        if (*finished)
            return;
        *finished = true;
        Result result;
        if (!failure.isEmpty()) {
            result.refusal = failure;
        } else if (*timedOut) {
            result.refusal = QStringLiteral("timeout");
        } else if (status != QProcess::NormalExit) {
            result.refusal = QStringLiteral("crashed");
        } else {
            QFile file(out);
            if (exitCode == 0 && file.open(QIODevice::ReadOnly)
                && file.size() <= kMaxPngBytes) {
                const QByteArray png = file.readAll();
                QBuffer buffer;
                buffer.setData(png);
                buffer.open(QIODevice::ReadOnly);
                // Header-only: the helper's output is not trusted either.
                QImageReader reader(&buffer, QByteArrayLiteral("png"));
                reader.setAutoDetectImageFormat(false);
                reader.setDecideFormatFromContent(false);
                const QSize size = reader.size();
                if (size.isValid() && !size.isEmpty()
                    && size.width() <= kAbsoluteMaxEdge
                    && size.height() <= kAbsoluteMaxEdge) {
                    result.png = png;
                    result.size = size;
                } else {
                    result.refusal = QStringLiteral("helper_failed");
                }
            } else if (exitCode == kHelperRefusedExit
                       && file.open(QIODevice::ReadOnly)) {
                const QString reason =
                    QString::fromLatin1(file.read(64)).trimmed();
                result.refusal = reason.isEmpty()
                    ? QStringLiteral("helper_failed") : reason;
            } else {
                result.refusal = QStringLiteral("helper_failed");
            }
        }
        // The helper's GUI app could not start (no usable platform plugin):
        // once, convert again without text rather than fail.
        if (result.refusal == QLatin1String("crashed") && !job.noText) {
            Job again = job;
            again.noText = true;
            pending().push_front(std::move(again));
        } else {
            deliver(job, result);
        }
        process->deleteLater();
        --running();
        pump();
    };

    QObject::connect(process, &QProcess::finished, process,
                     [finish](int code, QProcess::ExitStatus st) {
                         finish(QString(), code, st);
                     });
    QObject::connect(process, &QProcess::errorOccurred, process,
                     [finish, process](QProcess::ProcessError e) {
                         if (e == QProcess::FailedToStart)
                             finish(QStringLiteral("helper_failed"), -1,
                                    QProcess::CrashExit);
                         // Crashed/Timedout: finished() follows.
                         Q_UNUSED(process)
                     });
    const int timeout =
        hooks().timeoutMs > 0 ? hooks().timeoutMs : kDefaultTimeoutMs;
    QTimer::singleShot(timeout, process, [process, timedOut, finished] {
        if (*finished || process->state() == QProcess::NotRunning)
            return;
        *timedOut = true;
        process->kill();
    });
    process->start(program, args);
}

inline void pump()
{
    while (running() < kMaxParallel && !pending().empty()) {
        Job job = std::move(pending().front());
        pending().pop_front();
        if (!job.context)
            continue; // the requester is gone; nothing to deliver
        ++running();
        startProcess(std::move(job));
    }
}

} // namespace detail

/// Convert `svg` to a PNG under `policy`. `done` runs on the GUI thread, later
/// than this call, only while `context` is alive. A refusal or failure comes
/// back as `Result::refusal` (a code `userMessage()` understands).
inline void rasterizeAsync(const QByteArray &svg, Policy policy,
                           QObject *context, Callback done)
{
    detail::pending().push_back({ svg, policy, QPointer<QObject>(context),
                                  std::move(done) });
    // Deferred so the first job never completes inside the caller.
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [] { detail::pump(); });
}

/// Same, reading `path` first (bounded: a file over kMaxSourceBytes is refused
/// as "too_large" without being read whole).
inline void rasterizeFileAsync(const QString &path, Policy policy,
                               QObject *context, Callback done)
{
    QByteArray bytes;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly))
        bytes = file.read(kMaxSourceBytes + 1);
    // An unreadable file reaches the screen as empty: "not a valid SVG".
    rasterizeAsync(bytes, policy, context, std::move(done));
}

} // namespace lightning::svgraster
