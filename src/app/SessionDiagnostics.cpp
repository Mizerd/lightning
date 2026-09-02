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

// Homeserver-issued access tokens. Synapse uses `syt_`/`mda_` prefixes; the
// generic long-opaque-run rule below catches the rest, but naming these
// explicitly means a short test token is still removed.
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

// Bare Matrix user ids. The report carries hashed account identifiers by
// design, so any full MXID appearing in free text is unintended.
const QRegularExpression &matrixUserId()
{
    static const QRegularExpression re(
        QStringLiteral(R"(@[^\s:@"']+:[A-Za-z0-9.-]+(?::\d+)?)"));
    return re;
}

// Long opaque runs — the catch-all for base64/hex key and token material that
// none of the specific rules named. hashIdentifier() emits 16 characters, so
// the report's own hashes stay below the threshold.
const QRegularExpression &longOpaqueRun()
{
    static const QRegularExpression re(
        QStringLiteral(R"([A-Za-z0-9+/=_-]{24,})"));
    return re;
}

// Snake_case identifiers reach this length legitimately — the reason codes
// this report exists to carry are exactly that shape
// ("saved_session_without_store" is 27 characters), and blanking them would
// make the export useless for the failure it is meant to describe.
//
// Encoded credential material is not this shape: base64 mixes case and
// digits, and hex carries digits. An all-lowercase-and-underscore run of 24+
// characters being real key material would require every one of those
// characters to land in 26 of the 64 base64 symbols, and such a token would
// still be caught by the keyed-assignment and prefix rules above.
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

// Replaces long opaque runs, skipping snake_case identifiers. Done as an
// explicit match walk rather than a lookaround so the exemption is auditable.
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
    // Salt first, then the value. Without the salt this is a hash of a
    // dictionary-sized input and anyone holding the report could recover the
    // Matrix ID by hashing candidates; truncation does not help there.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(salt);
    hash.addData(trimmed.toUtf8());
    // 16 hex characters: enough to correlate lines within one report, and
    // short enough to survive the long-opaque-run rule below.
    return QString::fromLatin1(hash.result().toHex().left(16));
}

// A filesystem path. The report header promises "no file paths" and a store
// path contains the Matrix localpart, yet four free-text fields (connection
// status, login stage, local session failure, crypto summary) are filled
// from strings that COULD carry one, and the long-opaque-run rule cannot see
// a path because its character class excludes '.' and '/'.
const QRegularExpression &filesystemPath()
{
    static const QRegularExpression re(QStringLiteral(
        // POSIX: two or more slash-separated components. Neither slash of a
        // URL's "scheme://" may start a match (the first is preceded by ':',
        // the second by ':/'), so "https://host/a/b" keeps its host -- the
        // most useful thing in a connection-failure line -- while
        // "file:///home/…" still loses its path.
        "(?:(?<![A-Za-z0-9:])(?<!:/)/(?:[^\\s/\"']+/)+[^\\s/\"']*)"
        // ... or a Windows drive path.
        "|(?:\\b[A-Za-z]:\\\\[^\\s\"']+)"));
    return re;
}

// A URL's PATH. Applied before everything else so the host -- the single most
// useful thing in a connection-failure line -- survives, while the path
// (which can carry a room id, an event id, or a token) is gone before the
// long-opaque-run rule sees it; that rule used to eat the host's last label
// together with a long path ("matrix.example.org/_matrix/client/versions").
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
    // Order matters: keyed assignments first (they preserve the key name so a
    // reader can still see WHICH credential was present), then the specific
    // shapes, then the catch-all.
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

    // Belt and braces: a field added here in future cannot bypass the filter.
    return redactSensitive(out);
}

} // namespace matrix::app_diagnostics
