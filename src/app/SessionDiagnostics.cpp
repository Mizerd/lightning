#include "app/SessionDiagnostics.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace matrix::app_diagnostics {
namespace {

constexpr QLatin1String kRedacted{"<redacted>"};

// `key = value` / `key: value` / `key"="value` for any key that names a
// credential. Case-insensitive, and the value runs to the end of the token so
// a quoted or bare form is caught identically.
const QRegularExpression &credentialAssignment()
{
    static const QRegularExpression re(
        QStringLiteral(
            R"((\baccess[_-]?token|\brefresh[_-]?token|\bauth[_-]?token|\bbearer)"
            R"(|\bpassword|\bpassphrase|\brecovery[_-]?key|\bsecret[_-]?storage)"
            R"(|\bsecret|\bsession[_-]?key|\broom[_-]?key|\bprivate[_-]?key)"
            R"(|\bapi[_-]?key)(\s*["']?\s*[:=]\s*["']?)([^\s"',;}]+))"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// Synapse token prefixes, so even short tokens are removed.
const QRegularExpression &tokenPrefix()
{
    static const QRegularExpression re(
        QStringLiteral(R"((?i)\b(?:syt|mda|mdt)_[A-Za-z0-9_.=+/-]+)"));
    return re;
}

// A Matrix recovery key is 48 base58 characters, conventionally shown in
// twelve space-separated groups of four.
const QRegularExpression &recoveryKeyGroups()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\b(?:[A-Za-z0-9]{4}\s+){7,}[A-Za-z0-9]{4}\b)"));
    return re;
}

// Bare Matrix user ids; the report carries only hashed identifiers.
const QRegularExpression &matrixUserId()
{
    static const QRegularExpression re(
        QStringLiteral(R"(@[^\s:@"']+:[A-Za-z0-9.-]+(?::\d+)?)"));
    return re;
}

// Catch-all for base64/hex key material. hashIdentifier()'s 16 characters stay
// below the threshold.
const QRegularExpression &longOpaqueRun()
{
    static const QRegularExpression re(
        QStringLiteral(R"([A-Za-z0-9+/=_-]{24,})"));
    return re;
}

// Snake_case reason codes ("saved_session_without_store") are exempt: encoded
// key material mixes case and digits, so it does not take this shape.
bool looksLikeIdentifier(const QString &run)
{
    for (const QChar ch : run) {
        if (!((ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
              || ch == QLatin1Char('_'))) {
            return false;
        }
    }
    return true;
}

// An explicit match walk rather than a lookaround, so the exemption is
// auditable.
QString redactLongOpaqueRuns(const QString &input)
{
    QString out;
    out.reserve(input.size());
    qsizetype cursor = 0;
    auto it = longOpaqueRun().globalMatch(input);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += input.mid(cursor, m.capturedStart() - cursor);
        out += looksLikeIdentifier(m.captured()) ? m.captured()
                                                 : QString(kRedacted);
        cursor = m.capturedEnd();
    }
    out += input.mid(cursor);
    return out;
}

QString line(const QString &key, const QString &value)
{
    return key + QLatin1String(": ")
        + (value.isEmpty() ? QStringLiteral("(none)") : value)
        + QLatin1Char('\n');
}

QString line(const QString &key, bool value)
{
    return line(key, value ? QStringLiteral("yes") : QStringLiteral("no"));
}

QString line(const QString &key, int value)
{
    return line(key, QString::number(value));
}

} // namespace

QByteArray newReportSalt()
{
    QByteArray salt(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(salt.begin(), salt.end());
    return salt;
}

QString hashIdentifier(const QString &value, const QByteArray &salt)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(salt);
    hash.addData(trimmed.toUtf8());
    // Short enough to survive the long-opaque-run rule.
    return QString::fromLatin1(hash.result().toHex().left(16));
}

// Filesystem paths (a store path contains the Matrix localpart). The
// long-opaque-run rule cannot see them, since it excludes '.' and '/'.
const QRegularExpression &filesystemPath()
{
    static const QRegularExpression re(QStringLiteral(
        // POSIX: two or more components. A URL's "scheme://" slashes cannot
        // start a match, so "https://host/a/b" keeps its host while
        // "file:///home/…" still loses its path.
        "(?:(?<![A-Za-z0-9:])(?<!:/)/(?:[^\\s/\"']+/)+[^\\s/\"']*)"
        // ... or a Windows drive path.
        "|(?:\\b[A-Za-z]:\\\\[^\\s\"']+)"));
    return re;
}

// A URL's path, which can carry ids or tokens. Applied first so the host
// survives rather than being eaten along with a long path.
const QRegularExpression &urlPath()
{
    static const QRegularExpression re(
        QStringLiteral("\\b([a-z][a-z0-9+.-]*://[^\\s/\"']+)/[^\\s\"']*"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

QString redactSensitive(const QString &input)
{
    QString out = input;
    out.replace(urlPath(), QStringLiteral("\\1/<path>"));
    out.replace(filesystemPath(), QStringLiteral("<path>"));
    // Keyed assignments first (they keep the key name), then specific shapes,
    // then the catch-all.
    out.replace(credentialAssignment(), QStringLiteral("\\1\\2") + kRedacted);
    out.replace(tokenPrefix(), kRedacted);
    out.replace(recoveryKeyGroups(), kRedacted);
    out.replace(matrixUserId(), QStringLiteral("@<redacted>"));
    return redactLongOpaqueRuns(out);
}

QString renderReport(const Report &report)
{
    QString out;
    out += QLatin1String("Lightning support diagnostics\n");
    out += QLatin1String("(sanitized: no tokens, keys, message content, room "
                         "identifiers, or file paths)\n\n");

    out += QLatin1String("[application]\n");
    out += line(QStringLiteral("lightning_version"), report.appVersion);
    out += line(QStringLiteral("build_type"), report.buildType);
    out += line(QStringLiteral("backend"), report.backendName);
    out += line(QStringLiteral("qt_version"), report.qtVersion);
    out += line(QStringLiteral("rust_sdk"), report.rustSdkVersion);

    out += QLatin1String("\n[environment]\n");
    out += line(QStringLiteral("os"), report.osProduct);
    out += line(QStringLiteral("kernel"), report.kernelVersion);
    out += line(QStringLiteral("desktop"), report.desktopSession);
    out += line(QStringLiteral("session_type"), report.sessionType);

    out += QLatin1String("\n[accounts]\n");
    out += line(QStringLiteral("count"), report.accountCount);
    out += line(QStringLiteral("active"), report.activeAccountHash);
    for (int i = 0; i < report.accountHashes.size(); ++i) {
        out += line(QStringLiteral("account_%1").arg(i),
                    report.accountHashes.at(i));
    }
    out += line(QStringLiteral("store_layout"), report.storeLayoutVersion);
    out += line(QStringLiteral("secret_store"), report.secretStoreBackend);
    out += line(QStringLiteral("secret_store_secure"),
                report.secretStoreSecure);

    out += QLatin1String("\n[session]\n");
    out += line(QStringLiteral("connection"), report.connectionStatus);
    out += line(QStringLiteral("sync_mode"), report.syncMode);
    out += line(QStringLiteral("login_stage"), report.loginStage);
    out += line(QStringLiteral("initial_sync_done"), report.initialSyncDone);
    out += line(QStringLiteral("local_session_failure"),
                report.localSessionFailureReason);
    out += line(QStringLiteral("local_session_failure_account"),
                report.localSessionFailureAccountHash);

    out += QLatin1String("\n[encryption]\n");
    out += line(QStringLiteral("session_trust"), report.sessionTrustState);
    out += line(QStringLiteral("verification_state"),
                report.verificationState);
    out += line(QStringLiteral("cross_signing_available"),
                report.crossSigningAvailable);
    out += line(QStringLiteral("key_backup_usable"), report.keyBackupUsable);
    out += line(QStringLiteral("crypto_status"), report.cryptoStatusSummary);

    // A field added in future cannot bypass the filter.
    return redactSensitive(out);
}

} // namespace matrix::app_diagnostics
