#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Lightning secure update system — bounded HTTPS fetching.
//
// Two shapes, both with identical transport policy:
//   * UpdateDocumentFetcher — small in-memory documents (manifest, .sig),
//     hard byte ceiling, never written to disk.
//   * UpdateDownloader — the artifact, STREAMED to a caller-owned file and
//     hashed incrementally. A 200 MB package is never held in RAM.
//
// Policy (spec §9), enforced on the initial request AND on every redirect:
//   - https only; any other scheme aborts the transfer;
//   - the host must be on the compiled-in allowlist;
//   - bounded redirect count;
//   - a hard size ceiling; exceeding it aborts mid-stream;
//   - no cookies, no cache, no custom headers beyond the Lightning user
//     agent ("Lightning/<version>" — version only, no platform token and
//     nothing else), no query parameters derived from the user, no Matrix
//     data.
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

// Case-insensitive, length-checked hex comparison in constant time with
// respect to the compared bytes.
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
    void fail(TransferError error, const QString &detail = QString());
    void succeed();
    void abortReply();

    virtual void onChunk(const QByteArray &chunk) = 0;
    virtual bool onCompleted(QString *message) = 0;
    virtual void onRestart() = 0; // a redirect restarts the body

    qint64 receivedBytes() const { return m_received; }

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

// Small bounded in-memory GET.
class UpdateDocumentFetcher final : public UpdateTransferBase
{
    Q_OBJECT
public:
    explicit UpdateDocumentFetcher(QNetworkAccessManager *network, QObject *parent = nullptr);

    void start(const QUrl &url, qint64 maxBytes, const QByteArray &userAgent);
    QByteArray document() const { return m_buffer; }

protected:
    void onChunk(const QByteArray &chunk) override;
    bool onCompleted(QString *message) override;
    void onRestart() override;

private:
    QByteArray m_buffer;
};

// Streaming artifact download with an incremental SHA-256.
class UpdateDownloader final : public UpdateTransferBase
{
    Q_OBJECT
public:
    explicit UpdateDownloader(QNetworkAccessManager *network, QObject *parent = nullptr);

    // `target` stays owned by the caller and must be open for writing.
    // The transfer fails unless the streamed bytes are exactly
    // `expectedSize` and hash to `expectedSha256Hex`.
    void start(const QUrl &url, qint64 expectedSize, const QString &expectedSha256Hex,
               QFile *target, const QByteArray &userAgent);

    // Test seam. Installs the streaming state that start() would install,
    // WITHOUT issuing any request, so the redirect-restart path can be driven
    // deterministically and offline. It relaxes nothing: onCompleted() still
    // enforces the declared size and the SHA-256, and there is no way from
    // here to reach succeed() without both.
    void prepareForTest(QFile *target, qint64 expectedSize,
                        const QString &expectedSha256Hex);
    // Test seam: runs the redirect-restart hook exactly as handleFinished()
    // would, so its failure path is observable without a live redirect.
    void restartForTest() { onRestart(); }

protected:
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

} // namespace lightning::update
