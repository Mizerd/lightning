#include "app/LogExportController.h"

#include "app/AsyncLogSink.h"
#include "app/LogExport.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QThreadPool>

namespace lx = lightning::logexport;

namespace {
// Newest part of the rolling log an export reads. The files hold up to 8 MB;
// an issue is nearly always in the latest minutes, and the sanitizer's cost
// is linear in what it is given.
constexpr qint64 kMaxRawBytes = 3 * 1024 * 1024;
} // namespace

struct LogExportController::Job {
    QString directory;
    QString diagnostics;
    lx::SanitizeOptions options;
    QString destination; // empty: build only
};

struct LogExportController::Built {
    lx::ExportResult result;
    bool available = false;
    bool written = false;
    QString error;
};

LogExportController::LogExportController(QObject *parent) : QObject(parent) {}

void LogExportController::setDiagnosticsProvider(std::function<QString()> p)
{
    m_diagnostics = std::move(p);
}

void LogExportController::setLiteralsProvider(std::function<QStringList()> p)
{
    m_literals = std::move(p);
}

void LogExportController::setDirectory(const QString &directory)
{
    m_directory = directory;
}

QString LogExportController::promise() const
{
    return lx::exportPromise();
}

QString LogExportController::suggestedFileName() const
{
    return QStringLiteral("lightning-logs-%1.zip")
        .arg(QDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyyMMdd-HHmmss")));
}

std::shared_ptr<LogExportController::Job> LogExportController::makeJob() const
{
    auto job = std::make_shared<Job>();
    job->directory = m_directory.isEmpty() ? lx::rollingLogDirectory()
                                           : m_directory;
    job->diagnostics = m_diagnostics ? m_diagnostics() : QString();
    // Fresh per export: ids match inside one file and nowhere else.
    job->options.salt = QByteArray(16, Qt::Uninitialized);
    QRandomGenerator::system()->generate(job->options.salt.begin(),
                                         job->options.salt.end());
    if (m_literals)
        job->options.literals = m_literals();
    return job;
}

LogExportController::Built LogExportController::run(const Job &job)
{
    Built b;
    // What was logged a moment ago is still in the writer's queue.
    if (auto *sink = lx::rollingLogSink())
        sink->flush(500);
    int files = 0;
    const QByteArray raw = job.directory.isEmpty()
        ? QByteArray()
        : lx::readRecentLog(job.directory, kMaxRawBytes, &files);
    b.available = !raw.isEmpty();

    lx::ExportInputs in;
    in.rawLog = raw;
    in.diagnostics = job.diagnostics;
    in.options = job.options;
    in.sessionFiles = files;
    in.when = QDateTime::currentDateTimeUtc();
    b.result = lx::buildExport(in);

    if (!job.destination.isEmpty()) {
        QSaveFile out(job.destination);
        if (!out.open(QIODevice::WriteOnly)) {
            b.error = out.errorString();
            return b;
        }
        out.write(b.result.zip);
        if (!out.commit()) {
            b.error = out.errorString();
            return b;
        }
        b.written = true;
    }
    return b;
}

QByteArray LogExportController::buildZipNow(QString *recentLinesOut)
{
    const auto job = makeJob();
    const Built b = run(*job);
    if (recentLinesOut)
        *recentLinesOut = lx::lastLines(b.result.sanitizedLog, 200);
    return b.result.zip;
}

void LogExportController::prepare()
{
    const quint64 generation = ++m_generation;
    m_preview.insert(QStringLiteral("preparing"), true);
    Q_EMIT previewChanged();

    const std::shared_ptr<Job> job = makeJob();
    QPointer<LogExportController> self(this);
    QThreadPool::globalInstance()->start([self, job, generation] {
        Built built = run(*job);
        QMetaObject::invokeMethod(
            qApp, [self, generation, built = std::move(built)]() mutable {
                if (!self || generation != self->m_generation)
                    return; // a newer prepare() owns the answer
                const lx::ExportResult &r = built.result;
                QVariantMap m;
                m.insert(QStringLiteral("ready"), true);
                m.insert(QStringLiteral("preparing"), false);
                m.insert(QStringLiteral("available"), built.available);
                m.insert(QStringLiteral("zipBytes"), qint64(r.zip.size()));
                m.insert(QStringLiteral("lines"), r.stats.linesKept);
                m.insert(QStringLiteral("removedLines"),
                         r.stats.droppedSensitiveCategory
                             + r.stats.droppedMessageBody);
                m.insert(QStringLiteral("idsReplaced"), r.stats.idsReplaced);
                m.insert(QStringLiteral("sessionFiles"), r.sessionFiles);
                m.insert(QStringLiteral("rawBytes"), r.rawBytes);
                self->m_recent = lx::lastLines(r.sanitizedLog, 200);
                self->m_preview = m;
                Q_EMIT self->previewChanged();
            },
            Qt::QueuedConnection);
    });
}

void LogExportController::exportTo(const QUrl &file)
{
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    if (path.isEmpty()) {
        Q_EMIT exportFinished(false, tr("No file was chosen."));
        return;
    }
    const std::shared_ptr<Job> job = makeJob();
    job->destination = path;
    QPointer<LogExportController> self(this);
    QThreadPool::globalInstance()->start([self, job] {
        const Built built = run(*job);
        QMetaObject::invokeMethod(
            qApp, [self, ok = built.written, err = built.error] {
                if (!self)
                    return;
                Q_EMIT self->exportFinished(
                    ok, ok ? QString()
                           : (err.isEmpty() ? tr("Could not save the file.")
                                            : err));
            },
            Qt::QueuedConnection);
    });
}

void LogExportController::copyRecentLines(int count)
{
    // The prepared text was cut at 200 lines; a smaller request trims it.
    const QString text = lx::lastLines(m_recent, count);
    if (text.isEmpty())
        return;
    if (auto *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
    Q_EMIT copied(int(text.count(QLatin1Char('\n'))) + 1);
}
