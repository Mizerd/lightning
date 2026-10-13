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

// ---- sanitizer ------------------------------------------------------------

struct SanitizeOptions {
    // Random per export. Ids are replaced by sigil + 8 hex of
    // SHA-256(salt + id) (the shape matrix::e2ee::redactId gives), so equal
    // ids correlate inside one export and nowhere else; an unsalted hash of a
    // Matrix id is reversible by hashing candidates. Empty: plain redactId().
    QByteArray salt;
    // Literal strings that identify the user and must not survive anywhere
    // (the OS user name, home directory, account localparts and slugs). Each
    // is matched case-insensitively; anything shorter than 3 is ignored.
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

// Log categories whose lines are never exported.
bool isSensitiveCategory(QStringView category);

// "!room:server" -> "!1a2b3c4d" (the sigil is kept).
QString maskMatrixId(const QString &id, const QByteArray &salt);

// One log line, sanitized; nullopt when the whole line is dropped.
std::optional<QString> sanitizeLine(const QString &line,
                                    const SanitizeOptions &options,
                                    SanitizeStats *stats = nullptr);
// A whole log. Lines the sanitizer drops are removed, and one line says how
// many, so a reader can tell "nothing logged" from "removed".
QString sanitizeLog(const QString &raw, const SanitizeOptions &options,
                    SanitizeStats *stats = nullptr);
// Free text with no per-line dropping (the diagnostics header).
QString sanitizeText(const QString &text, const SanitizeOptions &options);

// The last `count` lines of `text`.
QString lastLines(const QString &text, int count);

// ---- rolling log ----------------------------------------------------------

struct RollingLimits {
    // A file is closed at the first batch that takes it past this.
    qint64 maxFileBytes = 1 * 1024 * 1024;
    // Oldest files go first when the set passes either bound. The file being
    // written is never removed.
    qint64 maxTotalBytes = 8 * 1024 * 1024;
    int maxFiles = 16;
};

// Write-only device for AsyncLogSink. One file per rotation, named
// `lightning-<UTC date>-<UTC time>-<seq>.log` so a plain sort is chronological
// and a new session never overwrites an older one. Files are owner-only.
// Every write() is one write(2); there is never an fsync: a log is advisory
// and a power cut losing the last second of it is the right trade.
class RollingLogDevice : public QIODevice
{
public:
    RollingLogDevice(const QString &directory, RollingLimits limits,
                     QObject *parent = nullptr);
    ~RollingLogDevice() override;

    // Creates the directory and prunes it; false when it cannot be used.
    bool open(OpenMode mode) override;
    void close() override;
    bool isSequential() const override { return true; }

    // Log files in `directory`, oldest first (absolute paths).
    static QStringList logFiles(const QString &directory);
    // Deletes the oldest files until `limits` holds, never `keep`.
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

// The newest `maxBytes` of the files' text, oldest first, cut at a line
// start. `fileCount` receives how many files contributed.
QByteArray readRecentLog(const QString &directory, qint64 maxBytes,
                         int *fileCount = nullptr);

// The process-wide rolling sink, or null (disabled, or not started yet).
lightning::logging::AsyncLogSink *rollingLogSink();
void setRollingLogSink(lightning::logging::AsyncLogSink *sink);
QString rollingLogDirectory();
void setRollingLogDirectory(const QString &directory);

// ---- zip ------------------------------------------------------------------

struct ZipEntry {
    QString name;
    QByteArray data;
};
QByteArray writeZip(const QList<ZipEntry> &entries, const QDateTime &when);
// Reads what writeZip wrote (stored or deflated); empty and *ok=false on any
// inconsistency, a bad CRC included.
QList<ZipEntry> readZip(const QByteArray &zip, bool *ok = nullptr);

// ---- the export -----------------------------------------------------------

struct ExportInputs {
    QByteArray rawLog;       // the rolling files' text
    QString diagnostics;     // header text, built by the caller from safe fields
    SanitizeOptions options;
    int sessionFiles = 0;
    QDateTime when;          // UTC
};

struct ExportResult {
    QByteArray zip;
    QString sanitizedLog;
    QString sanitizedDiagnostics;
    SanitizeStats stats;
    qint64 rawBytes = 0;
    int sessionFiles = 0;
};

// Entry names inside the zip.
inline constexpr char kReadmeName[] = "README.txt";
inline constexpr char kDiagnosticsName[] = "lightning-diagnostics.txt";
inline constexpr char kLogName[] = "lightning-log.txt";

ExportResult buildExport(const ExportInputs &inputs);

// What the user is told the file contains, before saving it.
QString exportPromise();

} // namespace lightning::logexport

namespace lightning::logexport {
// Non-secret build facts (`key: value` lines, as --build-info prints), set
// once by main(): the compile definitions live on that target alone.
void setBuildInfo(const QString &text);
QString buildInfo();
} // namespace lightning::logexport
