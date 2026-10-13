// Settings -> About -> Export logs. Reads the rolling log, sanitizes it off
// the GUI thread (lightning::logexport) and writes a .zip the user chose.
// The "prepare" step builds exactly what would be saved, so the size and
// counts the user is shown describe the file, not an estimate of it.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <functional>

class LogExportController : public QObject
{
    Q_OBJECT
    // { ready, preparing, zipBytes, lines, removedLines, idsReplaced,
    //   sessionFiles, rawBytes, available }
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)
    // What the exported file contains, in the words shown beside the button.
    Q_PROPERTY(QString promise READ promise CONSTANT)
public:
    explicit LogExportController(QObject *parent = nullptr);

    // Supplied by the application: header text built from safe fields, and
    // the strings that identify the user (OS user, home, account names).
    void setDiagnosticsProvider(std::function<QString()> provider);
    void setLiteralsProvider(std::function<QStringList()> provider);
    // Where the rolling files live (defaults to the process-wide directory).
    void setDirectory(const QString &directory);

    QVariantMap preview() const { return m_preview; }
    QString promise() const;

    // Builds the sanitized export in the background; emits previewChanged.
    Q_INVOKABLE void prepare();
    // Builds a fresh export and writes it to `file` (a local file URL).
    // Emits exportFinished.
    Q_INVOKABLE void exportTo(const QUrl &file);
    // The last `count` lines of the prepared, sanitized log.
    Q_INVOKABLE void copyRecentLines(int count = 200);
    Q_INVOKABLE QString suggestedFileName() const;

    // Synchronous, for tests: the same pipeline without the thread hop.
    QByteArray buildZipNow(QString *recentLinesOut = nullptr);

Q_SIGNALS:
    void previewChanged();
    void exportFinished(bool ok, const QString &message);
    void copied(int lines);

private:
    struct Job;
    struct Built;
    std::shared_ptr<Job> makeJob() const;
    static Built run(const Job &job);

    std::function<QString()> m_diagnostics;
    std::function<QStringList()> m_literals;
    QString m_directory;
    QVariantMap m_preview;
    QString m_recent;
    quint64 m_generation = 0;
};
