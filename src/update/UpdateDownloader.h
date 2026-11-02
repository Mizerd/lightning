#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <optional>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace lightning::update {

enum class TransferError {
    None,
    Cancelled,
    Network,
    InsecureUrl,
    ForeignHost,
    TooManyRedirects,
    SizeExceeded,
    SizeMismatch,
    HashMismatch,
    FileError,
    Stalled,
};

QString transferErrorText(TransferError error);

bool hashesEqual(const QString &expectedHex, const QString &actualHex);

class UpdateTransferBase : public QObject
{
    Q_OBJECT
public:
    ~UpdateTransferBase() override;

    void cancel();
    bool isRunning() const { return m_reply != nullptr; }

Q_SIGNALS:
    void progress(qint64 receivedBytes, qint64 totalBytes);
    void finished(bool ok, lightning::update::TransferError error, const QString &message);

protected:
    explicit UpdateTransferBase(QNetworkAccessManager *network, QObject *parent = nullptr);

    void beginTransfer(const QUrl &url, const QByteArray &userAgent, qint64 maxBytes);
    void deliverOfflineForTest(const QUrl &url, qint64 maxBytes,
                               const std::optional<QByteArray> &bytes);
    void fail(TransferError error, const QString &detail = QString());
    void succeed();
    void abortReply();

    virtual bool isPermittedUrl(const QUrl &url) const = 0;
    virtual void onChunk(const QByteArray &chunk) = 0;
    virtual bool onCompleted(QString *message) = 0;
    virtual void onRestart() = 0;

    qint64 receivedBytes() const { return m_received; }

    void resetTerminalStateForReuse();

private:
    void issueRequest(const QUrl &url);
    void handleReadyRead();
    void handleFinished();

    QPointer<QNetworkAccessManager> m_network;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_stallTimer = nullptr;
    QUrl m_url;
    QByteArray m_userAgent;
    qint64 m_maxBytes = 0;
    qint64 m_received = 0;
    qint64 m_declaredTotal = -1;
    int m_redirects = 0;
    bool m_cancelled = false;
    bool m_done = false;

    static constexpr int kMaxRedirects = 4;
    static constexpr int kStallTimeoutMs = 60000;
};

class UpdateDocumentFetcher final : public UpdateTransferBase
{
    Q_OBJECT
public:
    explicit UpdateDocumentFetcher(QNetworkAccessManager *network, QObject *parent = nullptr);

    void start(const QUrl &url, qint64 maxBytes, const QByteArray &userAgent);
    QByteArray document() const { return m_buffer; }
    bool permitsForTest(const QUrl &start, const QUrl &hop);

protected:
    // Fixed by the starting URL. Canonical metadata never leaves the canonical
    // host. The mirror pair (the fixed GitHub release slot, now the primary) is served through
    // GitHub's asset redirects, so that transfer may hop within the mirror host
    // list only.
    bool isPermittedUrl(const QUrl &url) const override;
    void onChunk(const QByteArray &chunk) override;
    bool onCompleted(QString *message) override;
    void onRestart() override;

private:
    QByteArray m_buffer;
    bool m_fallbackRole = false;
};

class UpdateDownloader final : public UpdateTransferBase
{
    Q_OBJECT
public:
    explicit UpdateDownloader(QNetworkAccessManager *network, QObject *parent = nullptr);

    void start(const QUrl &url, qint64 expectedSize, const QString &expectedSha256Hex,
               QFile *target, const QByteArray &userAgent);

    void prepareForTest(QFile *target, qint64 expectedSize,
                        const QString &expectedSha256Hex);
    void restartForTest() { onRestart(); }

    void deliverForTest(const QUrl &url, qint64 expectedSize,
                        const QString &expectedSha256Hex, QFile *target,
                        const std::optional<QByteArray> &bytes);

protected:
    bool isPermittedUrl(const QUrl &url) const override;
    void onChunk(const QByteArray &chunk) override;
    bool onCompleted(QString *message) override;
    void onRestart() override;

private:
    QFile *m_target = nullptr;
    qint64 m_expectedSize = 0;
    QString m_expectedHash;
    QCryptographicHash m_hash{ QCryptographicHash::Sha256 };
    bool m_writeFailed = false;
};

}
