// Export of the application's recent log for a bug report.
//
// Three independent parts, all Qt Core only:
//
//  * a SANITIZER that makes a log fit to be shared (CLAUDE.md section 6: what
//    the user is invited to share is held to a stricter bar than the local
//    log). It runs at EXPORT time, never at write time, so the local file
//    stays a faithful debugging aid and a new rule applies to old logs too.
//  * a ROLLING LOG DEVICE: an append-only, size-bounded set of files in the
//    cache directory, written through lightning::logging::AsyncLogSink.
//  * a minimal ZIP writer (and reader, for tests), because Qt Core has none.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QIODevice>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace lightning::logging {
class AsyncLogSink;
}

namespace lightning::logexport {

struct SanitizeOptions {
    // Random per export. Ids are replaced by sigil + 8 hex of
    // SHA-256(salt + id) (the shape matrix::e2ee::redactId gives), so equal
    // ids correlate inside one export and nowhere else; an unsalted hash of a
    // Matrix id is reversible by hashing candidates. Empty: plain redactId().
    QByteArray salt;
    QStringList literals;
};

struct SanitizeStats {
    int linesIn = 0;
    int linesKept = 0;
    int droppedSensitiveCategory = 0;
    int droppedMessageBody = 0;
    int truncatedLines = 0;
    int idsReplaced = 0;
};

bool isSensitiveCategory(QStringView category);

QString maskMatrixId(const QString &id, const QByteArray &salt);

std::optional<QString> sanitizeLine(const QString &line,
                                    const SanitizeOptions &options,
                                    SanitizeStats *stats = nullptr);
QString sanitizeLog(const QString &raw, const SanitizeOptions &options,
                    SanitizeStats *stats = nullptr);
QString sanitizeText(const QString &text, const SanitizeOptions &options);

QString lastLines(const QString &text, int count);

struct RollingLimits {
    qint64 maxFileBytes = 1 * 1024 * 1024;
    qint64 maxTotalBytes = 8 * 1024 * 1024;
    int maxFiles = 16;
};

class RollingLogDevice : public QIODevice
{
public:
    RollingLogDevice(const QString &directory, RollingLimits limits,
                     QObject *parent = nullptr);
    ~RollingLogDevice() override;

    bool open(OpenMode mode) override;
    void close() override;
    bool isSequential() const override { return true; }

    static QStringList logFiles(const QString &directory);
    static void prune(const QString &directory, const RollingLimits &limits,
                      const QString &keep);

protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 len) override;

private:
    bool openNextFile();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

QByteArray readRecentLog(const QString &directory, qint64 maxBytes,
                         int *fileCount = nullptr);

lightning::logging::AsyncLogSink *rollingLogSink();
void setRollingLogSink(lightning::logging::AsyncLogSink *sink);
QString rollingLogDirectory();
void setRollingLogDirectory(const QString &directory);

// The rolling log is OFF unless the user turned it on ("Keep a diagnostic
// log", Settings -> About). The choice is device-local: one raw QSettings key
// that is in no settings-sync or export schema, so it is neither synced nor
// exported. LIGHTNING_NO_ROLLING_LOG is a hard off above the setting.
bool keepLogEnabled();
void setKeepLogEnabled(bool on);
bool rollingLogForbiddenByEnvironment();
bool startRollingLog(const QString &directory);
void stopRollingLog();
bool rollingLogRunning();
int deleteSavedLogs();

struct ZipEntry {
    QString name;
    QByteArray data;
};
QByteArray writeZip(const QList<ZipEntry> &entries, const QDateTime &when);
QList<ZipEntry> readZip(const QByteArray &zip, bool *ok = nullptr);

struct ExportInputs {
    QByteArray rawLog;
    QString diagnostics;
    SanitizeOptions options;
    int sessionFiles = 0;
    QDateTime when;
};

struct ExportResult {
    QByteArray zip;
    QString sanitizedLog;
    QString sanitizedDiagnostics;
    SanitizeStats stats;
    qint64 rawBytes = 0;
    int sessionFiles = 0;
};

inline constexpr char kReadmeName[] = "README.txt";
inline constexpr char kDiagnosticsName[] = "lightning-diagnostics.txt";
inline constexpr char kLogName[] = "lightning-log.txt";

ExportResult buildExport(const ExportInputs &inputs);

QString exportPromise();

}

namespace lightning::logexport {
void setBuildInfo(const QString &text);
QString buildInfo();
}
