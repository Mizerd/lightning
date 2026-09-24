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

// Bounded HTTPS fetching for updates, in two shapes with the same transport
// policy:
//   * UpdateDocumentFetcher: small in-memory documents (manifest, .sig) with a
//     hard byte ceiling, never written to disk.
//   * UpdateDownloader: the artifact, streamed to a caller-owned file and
//     hashed incrementally.
//
// Policy (spec §9), on the first request and every redirect:
//   - https only;
//   - the host must be allowed for this transfer's role (isPermittedUrl):
//     documents use the canonical host, or the mirror hosts for the fallback
//     pair only; artifacts also accept the bandwidth mirrors;
//   - bounded redirects and a hard size ceiling (aborts mid-stream);
//   - no cookies or cache, no headers beyond the "Lightning/<version>" user
//     agent, no user-derived query parameters, no Matrix data.
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
    // Test seam: the same policy, chunking, ceiling and completion path with
    // the network removed. `bytes` is the response body; std::nullopt is a
    // transport failure. isPermittedUrl() and the ceiling still apply.
    void deliverOfflineForTest(const QUrl &url, qint64 maxBytes,
                               const std::optional<QByteArray> &bytes);
    void fail(TransferError error, const QString &detail = QString());
    void succeed();
    void abortReply();

    // Host policy for this transfer's role; documents and artifacts differ.
    virtual bool isPermittedUrl(const QUrl &url) const = 0;
    virtual void onChunk(const QByteArray &chunk) = 0;
    virtual bool onCompleted(QString *message) = 0;
    virtual void onRestart() = 0; // a redirect restarts the body

    qint64 receivedBytes() const { return m_received; }

    // Clears the terminal flags so the object can run another transfer (the
    // mirror fallback reuses one downloader). Otherwise fail() and cancel() are
    // no-ops and a refused second attempt would never emit finished(). Call
    // before any check that can fail.
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

// Small bounded in-memory GET.
class UpdateDocumentFetcher final : public UpdateTransferBase
{
    Q_OBJECT
public:
    explicit UpdateDocumentFetcher(QNetworkAccessManager *network, QObject *parent = nullptr);

    void start(const QUrl &url, qint64 maxBytes, const QByteArray &userAgent);
    QByteArray document() const { return m_buffer; }
    // What a transfer started at `start` may hop to. For tests.
    bool permitsForTest(const QUrl &start, const QUrl &hop);

protected:
    // Fixed by the starting URL. Canonical metadata never leaves the canonical
    // host. The fallback pair (a fixed GitHub release slot) is served through
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

    // Test seam: installs start()'s streaming state without a request, to drive
    // the redirect-restart path offline. Size and SHA-256 are still enforced.
    void prepareForTest(QFile *target, qint64 expectedSize,
                        const QString &expectedSha256Hex);
    // Test seam: runs the redirect-restart hook as handleFinished() would.
    void restartForTest() { onRestart(); }

    // Test seam: streams `bytes` (std::nullopt = unreachable) as if from `url`.
    // Host policy, the target file, size and SHA-256 checks all apply, so a
    // mirror serving modified bytes fails exactly as in production.
    void deliverForTest(const QUrl &url, qint64 expectedSize,
                        const QString &expectedSha256Hex, QFile *target,
                        const std::optional<QByteArray> &bytes);

protected:
    // The canonical host or a compiled-in mirror, on every hop.
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

} // namespace lightning::update
