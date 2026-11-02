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
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)
    Q_PROPERTY(QString promise READ promise CONSTANT)
    Q_PROPERTY(bool keepLog READ keepLog WRITE setKeepLog NOTIFY keepLogChanged)
    // LIGHTNING_NO_ROLLING_LOG is set: the switch cannot turn it on.
    Q_PROPERTY(bool keepLogForcedOff READ keepLogForcedOff CONSTANT)
public:
    explicit LogExportController(QObject *parent = nullptr);

    void setDiagnosticsProvider(std::function<QString()> provider);
    void setLiteralsProvider(std::function<QStringList()> provider);
    void setDirectory(const QString &directory);

    QVariantMap preview() const { return m_preview; }
    QString promise() const;

    bool keepLog() const;
    void setKeepLog(bool on);
    bool keepLogForcedOff() const;
    Q_INVOKABLE int deleteSavedLogs();

    Q_INVOKABLE void prepare();
    Q_INVOKABLE void exportTo(const QUrl &file);
    Q_INVOKABLE void copyRecentLines(int count = 200);
    Q_INVOKABLE QString suggestedFileName() const;

    QByteArray buildZipNow(QString *recentLinesOut = nullptr);

Q_SIGNALS:
    void previewChanged();
    void keepLogChanged();
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
