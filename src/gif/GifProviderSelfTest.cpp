#include "gif/GifProviderSelfTest.h"

#include "gif/GifBuildKeys.h"
#include "gif/GifKeyConfig.h"
#include "gif/GifProvider.h"
#include "gif/GifResponseParser.h"

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

namespace gif {
namespace {

// Bounded caps for the diagnostic request. Trending JSON is small.
constexpr qint64 kMaxBytes = 2 * 1024 * 1024;
constexpr int kTimeoutMs = 15000;

ResolvedKey resolvedKey(const QString &providerId)
{
    return resolveProviderKeyDetailed(providerId);
}

// Perform one bounded trending request. On success sets *count and returns
// true. `detail` receives a presentation-safe reason on failure — never a key
// or URL.
bool trendingProbe(const QString &providerId, const QString &key, int *count,
                   QString *detail)
{
    auto provider = makeGifProvider(providerId);
    if (!provider) {
        *detail = QStringLiteral("unknown provider");
        return false;
    }
    // The URL embeds the key: build it, use it, never log it.
    const QString url =
        provider->trendingUrl(Rating::G, /*page=*/0, /*perPage=*/5, key);
    if (url.isEmpty()) {
        *detail = QStringLiteral("no request URL");
        return false;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    // https only; keep any redirect on the same origin so a key is never
    // replayed to another host, matching the app's provider-host policy.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::SameOriginRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Lightning-GIF-selftest"));

    QNetworkReply *reply = nam.get(req);
    QByteArray body;
    bool tooLarge = false;
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&] {
        body.append(reply->read(kMaxBytes - body.size() + 1));
        if (body.size() > kMaxBytes) {
            tooLarge = true;
            reply->abort();
        }
    });

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, reply, [&] {
        timedOut = true;
        reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(kTimeoutMs);
    loop.exec();
    timer.stop();

    const int http =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netErr = reply->error();
    reply->deleteLater();

    if (timedOut) { *detail = QStringLiteral("timeout"); return false; }
    if (tooLarge) { *detail = QStringLiteral("response too large"); return false; }
    if (http != 200) {
        // Status only — never the body (may echo the key) or URL.
        *detail = QStringLiteral("status %1").arg(http);
        return false;
    }
    if (netErr != QNetworkReply::NoError && body.isEmpty()) {
        *detail = QStringLiteral("network error");
        return false;
    }

    const ParseOutcome parsed =
        provider->parse(body, Rating::G, /*requestPage=*/0, /*perPage=*/5);
    if (!parsed.ok || parsed.results.isEmpty()) {
        *detail = QStringLiteral("no structured results");
        return false;
    }
    *count = static_cast<int>(parsed.results.size());
    return true;
}

} // namespace

int printProviderStatus()
{
    // Booleans first (the packaging validation greps these exact lines),
    // then the sanitized source class — never a key, prefix, or suffix.
    QTextStream out(stdout);
    for (const QString &id : {QStringLiteral("giphy"), QStringLiteral("klipy")}) {
        const ResolvedKey resolved = resolvedKey(id);
        out << id.toUpper() << " configured: "
            << (resolved.configured() ? "yes" : "no") << '\n';
        out << id.toUpper() << " source: " << keySourceName(resolved.source)
            << '\n';
    }
    return 0;
}

int runProviderSelfTest()
{
    QTextStream out(stdout);
    bool allOk = true;
    for (const QString &id : {QStringLiteral("giphy"), QStringLiteral("klipy")}) {
        const ResolvedKey resolved = resolvedKey(id);
        const QString key = resolved.key;
        const bool configured = resolved.configured();
        out << id.toUpper() << " configured: " << (configured ? "yes" : "no")
            << '\n';
        out << id.toUpper() << " source: " << keySourceName(resolved.source)
            << '\n';
        if (!configured) {
            out << id.toUpper() << " request: skipped (not configured)\n";
            allOk = false;
            continue;
        }
        int count = 0;
        QString detail;
        if (trendingProbe(id, key, &count, &detail)) {
            out << id.toUpper() << " request: ok (" << count << " results)\n";
        } else {
            out << id.toUpper() << " request: failed (" << detail << ")\n";
            allOk = false;
        }
    }
    out.flush();
    return allOk ? 0 : 1;
}

} // namespace gif
