#include "update/UpdateDownloader.h"

#include "update/UpdateEndpoints.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QVariant>

namespace lightning::update {
namespace {

bool isRedirectStatus(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

} // namespace

QString transferErrorText(TransferError error)
{
    switch (error) {
    case TransferError::None:
        return QStringLiteral("No error");
    case TransferError::Cancelled:
        return QStringLiteral("The transfer was cancelled");
    case TransferError::Network:
        return QStringLiteral("The download could not be completed");
    case TransferError::InsecureUrl:
        return QStringLiteral("The download address is not a secure HTTPS address");
    case TransferError::ForeignHost:
        return QStringLiteral("The download address points at an untrusted host");
    case TransferError::TooManyRedirects:
        return QStringLiteral("The download was redirected too many times");
    case TransferError::SizeExceeded:
        return QStringLiteral("The download is larger than the update information declared");
    case TransferError::SizeMismatch:
        return QStringLiteral("The download size does not match the update information");
    case TransferError::HashMismatch:
        return QStringLiteral("The downloaded file failed its integrity check");
    case TransferError::FileError:
        return QStringLiteral("The downloaded file could not be written");
    case TransferError::Stalled:
        return QStringLiteral("The download stopped responding");
    }
    return QStringLiteral("Unknown download error");
}

bool hashesEqual(const QString &expectedHex, const QString &actualHex)
{
    // Length is public information; the byte comparison itself does not
    // short-circuit.
    if (expectedHex.size() != actualHex.size() || expectedHex.isEmpty())
        return false;
    const QByteArray expected = expectedHex.toLower().toLatin1();
    const QByteArray actual = actualHex.toLower().toLatin1();
    if (expected.size() != actual.size())
        return false;
    unsigned char diff = 0;
    for (qsizetype i = 0; i < expected.size(); ++i)
        diff |= static_cast<unsigned char>(expected.at(i) ^ actual.at(i));
    return diff == 0;
}

UpdateTransferBase::UpdateTransferBase(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent)
    , m_network(network)
{
}

UpdateTransferBase::~UpdateTransferBase()
{
    abortReply();
}

void UpdateTransferBase::resetTerminalStateForReuse()
{
    m_done = false;
    m_cancelled = false;
}

void UpdateTransferBase::abortReply()
{
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void UpdateTransferBase::cancel()
{
    if (m_done)
        return;
    m_cancelled = true;
    abortReply();
    fail(TransferError::Cancelled);
}

void UpdateTransferBase::beginTransfer(const QUrl &url, const QByteArray &userAgent,
                                       qint64 maxBytes)
{
    m_userAgent = userAgent;
    m_maxBytes = maxBytes;
    m_received = 0;
    m_declaredTotal = -1;
    m_redirects = 0;
    m_cancelled = false;
    m_done = false;
    m_url = url;

    if (!m_network) {
        fail(TransferError::Network);
        return;
    }
    if (!isPermittedUrl(url)) {
        fail(url.scheme() == QLatin1String("https") ? TransferError::ForeignHost
                                                    : TransferError::InsecureUrl);
        return;
    }
    issueRequest(url);
}

void UpdateTransferBase::deliverOfflineForTest(const QUrl &url, qint64 maxBytes,
                                               const std::optional<QByteArray> &bytes)
{
    m_userAgent.clear();
    m_maxBytes = maxBytes;
    m_received = 0;
    m_declaredTotal = -1;
    m_redirects = 0;
    m_cancelled = false;
    m_done = false;

    // The same host policy the network path applies, in the same place.
    if (!isPermittedUrl(url)) {
        fail(url.scheme() == QLatin1String("https") ? TransferError::ForeignHost
                                                    : TransferError::InsecureUrl);
        return;
    }
    m_url = url;
    if (!bytes) {
        // DNS failure, TLS failure, 404, 500 — indistinguishable here and
        // treated exactly as the network path treats them.
        fail(TransferError::Network);
        return;
    }

    const QByteArray &body = *bytes;
    constexpr qsizetype kChunk = 64 * 1024;
    for (qsizetype offset = 0; offset < body.size(); offset += kChunk) {
        const QByteArray chunk = body.mid(offset, kChunk);
        m_received += chunk.size();
        if (m_maxBytes > 0 && m_received > m_maxBytes) {
            fail(TransferError::SizeExceeded);
            return;
        }
        onChunk(chunk);
        if (m_done)
            return; // onChunk reported its own failure
    }
    Q_EMIT progress(m_received, m_maxBytes > 0 ? m_maxBytes : m_received);

    QString message;
    if (!onCompleted(&message))
        return; // onCompleted already reported the specific failure
    succeed();
}

void UpdateTransferBase::issueRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    // The only header Lightning adds, and it is exactly
    // "Lightning/<version>" -- no platform, no build id, no locale, nothing
    // account-derived. The same string the rest of Lightning already sends.
    request.setRawHeader(QByteArrayLiteral("User-Agent"), m_userAgent);
    // Redirects are followed manually so every hop is re-validated. The
    // enum values convert to int, which is what QNetworkRequest stores.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         int(QNetworkRequest::ManualRedirectPolicy));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         int(QNetworkRequest::AlwaysNetwork));
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    // No cookies are sent or stored for an update request.
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                         int(QNetworkRequest::Manual));
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                         int(QNetworkRequest::Manual));
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                         int(QNetworkRequest::Manual));

    m_reply = m_network->get(request);
    if (!m_reply) {
        fail(TransferError::Network);
        return;
    }
    connect(m_reply, &QNetworkReply::readyRead, this, &UpdateTransferBase::handleReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateTransferBase::handleFinished);

    if (!m_stallTimer) {
        m_stallTimer = new QTimer(this);
        m_stallTimer->setSingleShot(true);
        connect(m_stallTimer, &QTimer::timeout, this, [this]() {
            if (m_done)
                return;
            abortReply();
            fail(TransferError::Stalled);
        });
    }
    m_stallTimer->start(kStallTimeoutMs);
}

void UpdateTransferBase::handleReadyRead()
{
    if (m_done || !m_reply)
        return;
    if (m_stallTimer)
        m_stallTimer->start(kStallTimeoutMs);

    // A redirect response body is discarded, not streamed to the target.
    const int status =
        m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (isRedirectStatus(status)) {
        m_reply->readAll();
        return;
    }

    while (m_reply && m_reply->bytesAvailable() > 0) {
        const QByteArray chunk = m_reply->read(64 * 1024);
        if (chunk.isEmpty())
            break;
        m_received += chunk.size();
        if (m_maxBytes > 0 && m_received > m_maxBytes) {
            abortReply();
            fail(TransferError::SizeExceeded);
            return;
        }
        onChunk(chunk);
        if (m_done)
            return;
    }

    if (m_reply) {
        const qint64 declared =
            m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (declared > 0)
            m_declaredTotal = declared;
    }
    Q_EMIT progress(m_received, m_maxBytes > 0 ? m_maxBytes : m_declaredTotal);
}

void UpdateTransferBase::handleFinished()
{
    if (m_done)
        return;
    if (!m_reply)
        return;

    QNetworkReply *reply = m_reply;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();

    if (isRedirectStatus(status)) {
        const QUrl location =
            reply->header(QNetworkRequest::LocationHeader).toUrl();
        const QUrl resolved = location.isRelative() ? reply->url().resolved(location) : location;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->deleteLater();

        if (++m_redirects > kMaxRedirects) {
            fail(TransferError::TooManyRedirects);
            return;
        }
        if (resolved.scheme() != QLatin1String("https")) {
            fail(TransferError::InsecureUrl);
            return;
        }
        // Same role, same policy: a mirror artifact download may hop to the
        // mirror's object host, a manifest fetch may not hop anywhere but the
        // canonical host.
        if (!isPermittedUrl(resolved)) {
            fail(TransferError::ForeignHost);
            return;
        }
        // Restart the body from scratch: partial bytes from before the
        // redirect must never be mixed into the hash.
        m_received = 0;
        onRestart();
        // onRestart may itself be terminal (it truncates the target file, and
        // that can fail). It reports its own failure; without this guard the
        // transfer would issue a fresh request after having already finished.
        if (m_done)
            return;
        issueRequest(resolved);
        return;
    }

    m_reply = nullptr;
    reply->disconnect(this);
    reply->deleteLater();
    if (m_stallTimer)
        m_stallTimer->stop();

    if (m_cancelled) {
        fail(TransferError::Cancelled);
        return;
    }
    if (networkError != QNetworkReply::NoError) {
        fail(TransferError::Network);
        return;
    }
    if (status != 0 && (status < 200 || status >= 300)) {
        fail(TransferError::Network);
        return;
    }

    QString message;
    if (!onCompleted(&message))
        return; // onCompleted already reported the specific failure
    succeed();
}

void UpdateTransferBase::fail(TransferError error, const QString &detail)
{
    if (m_done)
        return;
    m_done = true;
    if (m_stallTimer)
        m_stallTimer->stop();
    const QString message = detail.isEmpty()
        ? transferErrorText(error)
        : transferErrorText(error) + QStringLiteral(": ") + detail;
    Q_EMIT finished(false, error, message);
}

void UpdateTransferBase::succeed()
{
    if (m_done)
        return;
    m_done = true;
    if (m_stallTimer)
        m_stallTimer->stop();
    Q_EMIT finished(true, TransferError::None, QString());
}

UpdateDocumentFetcher::UpdateDocumentFetcher(QNetworkAccessManager *network, QObject *parent)
    : UpdateTransferBase(network, parent)
{
}

// ONE definition of the role, used by production and by the test that pins
// it: canonical wins any overlap (fail-safe), and only an address on the
// mirror list that is NOT canonical is the fallback.
static bool fallbackRoleFor(const QUrl &start)
{
    return !isAllowedManifestUrl(start) && isAllowedFallbackManifestUrl(start);
}

void UpdateDocumentFetcher::start(const QUrl &url, qint64 maxBytes, const QByteArray &userAgent)
{
    m_buffer.clear();
    // The ROLE is decided once, from the address the caller chose, and holds
    // for every hop: a canonical fetch cannot be redirected onto a mirror,
    // and a fallback fetch cannot be redirected back to some third host.
    m_fallbackRole = fallbackRoleFor(url);
    beginTransfer(url, userAgent, maxBytes);
}

bool UpdateDocumentFetcher::permitsForTest(const QUrl &start, const QUrl &hop)
{
    m_fallbackRole = fallbackRoleFor(start);
    return isPermittedUrl(hop);
}

bool UpdateDocumentFetcher::isPermittedUrl(const QUrl &url) const
{
    // Metadata is canonical-only, except for the one fallback pair, which
    // lives on the mirror and hops only within the mirror's hosts. Either
    // way the bytes are trusted for nothing until the compiled-in key has
    // verified the signature.
    return m_fallbackRole ? isAllowedFallbackManifestUrl(url) : isAllowedManifestUrl(url);
}

void UpdateDocumentFetcher::onChunk(const QByteArray &chunk)
{
    m_buffer.append(chunk);
}

bool UpdateDocumentFetcher::onCompleted(QString *)
{
    return true;
}

void UpdateDocumentFetcher::onRestart()
{
    m_buffer.clear();
}

UpdateDownloader::UpdateDownloader(QNetworkAccessManager *network, QObject *parent)
    : UpdateTransferBase(network, parent)
{
}

void UpdateDownloader::start(const QUrl &url, qint64 expectedSize,
                             const QString &expectedSha256Hex, QFile *target,
                             const QByteArray &userAgent)
{
    m_target = target;
    m_expectedSize = expectedSize;
    m_expectedHash = expectedSha256Hex.toLower();
    m_hash.reset();
    m_writeFailed = false;

    resetTerminalStateForReuse();

    if (!target || !target->isOpen() || !target->isWritable()) {
        fail(TransferError::FileError);
        return;
    }
    if (expectedSize <= 0 || m_expectedHash.size() != 64) {
        // Without a declared size and hash there is nothing to verify
        // against, and an unverifiable download is never started.
        fail(TransferError::SizeMismatch);
        return;
    }
    beginTransfer(url, userAgent, expectedSize);
}

void UpdateDownloader::prepareForTest(QFile *target, qint64 expectedSize,
                                      const QString &expectedSha256Hex)
{
    m_target = target;
    m_expectedSize = expectedSize;
    m_expectedHash = expectedSha256Hex.toLower();
    m_hash.reset();
    m_writeFailed = false;
}

void UpdateDownloader::deliverForTest(const QUrl &url, qint64 expectedSize,
                                      const QString &expectedSha256Hex, QFile *target,
                                      const std::optional<QByteArray> &bytes)
{
    // Byte for byte the preparation start() performs, including both refusals
    // below: an unverifiable download is never started here either.
    prepareForTest(target, expectedSize, expectedSha256Hex);
    resetTerminalStateForReuse();
    if (!target || !target->isOpen() || !target->isWritable()) {
        fail(TransferError::FileError);
        return;
    }
    if (expectedSize <= 0 || m_expectedHash.size() != 64) {
        fail(TransferError::SizeMismatch);
        return;
    }
    deliverOfflineForTest(url, expectedSize, bytes);
}

bool UpdateDownloader::isPermittedUrl(const QUrl &url) const
{
    // Artifact bytes: canonical host or a compiled-in bandwidth mirror.
    // Whatever serves them, they are installable only after the manifest's
    // size and SHA-256 have both matched.
    return isAllowedArtifactUrl(url);
}

void UpdateDownloader::onChunk(const QByteArray &chunk)
{
    if (m_writeFailed)
        return;
    if (!m_target || m_target->write(chunk) != chunk.size()) {
        m_writeFailed = true;
        abortReply();
        fail(TransferError::FileError);
        return;
    }
    m_hash.addData(chunk);
}

bool UpdateDownloader::onCompleted(QString *message)
{
    Q_UNUSED(message)
    if (m_writeFailed)
        return false;
    if (!m_target) {
        fail(TransferError::FileError);
        return false;
    }
    if (!m_target->flush()) {
        fail(TransferError::FileError);
        return false;
    }
    if (receivedBytes() != m_expectedSize) {
        fail(TransferError::SizeMismatch);
        return false;
    }
    const QString actual = QString::fromLatin1(m_hash.result().toHex());
    if (!hashesEqual(m_expectedHash, actual)) {
        fail(TransferError::HashMismatch);
        return false;
    }
    return true;
}

void UpdateDownloader::onRestart()
{
    // A redirect means the body starts over; so must the file and the hash.
    m_hash.reset();
    if (!m_target || !m_target->isOpen()) {
        // There is nothing left to stream into. Report it here rather than
        // letting the transfer continue into a target it cannot write.
        m_writeFailed = true;
        abortReply();
        fail(TransferError::FileError);
        return;
    }
    if (!m_target->resize(0) || !m_target->seek(0)) {
        // Reporting the failure HERE is the point. Setting the flag and
        // returning quietly left onCompleted() to return false without ever
        // calling fail(), so finished() was never emitted, the stall timer was
        // already stopped, and UpdateManager sat in Downloading forever
        // holding the single-instance lock and the temp file.
        m_writeFailed = true;
        abortReply();
        fail(TransferError::FileError);
    }
}

} // namespace lightning::update
