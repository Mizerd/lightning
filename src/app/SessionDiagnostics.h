#pragma once

#include <QString>
#include <QStringList>

// User-invoked support bundle for bug reports.
//
// The report is built only from a fixed struct of already-safe fields;
// `redactSensitive()` is a second line of defence for backend free text.
// There is no field for tokens, passwords, keys, secret-storage material,
// message content, room names or ids, media, or filesystem paths (a store path
// contains the Matrix localpart).
namespace matrix::app_diagnostics {

// Short pseudonym for correlating an account within one report. `salt` must
// be random per report: an unsalted hash of a Matrix ID is reversible by
// hashing candidates. Empty input yields an empty string.
QString hashIdentifier(const QString &value, const QByteArray &salt);

// Never persisted, so reports cannot be linked.
QByteArray newReportSalt();

// Scrubs token-like material from backend free text, erring towards blanking.
QString redactSensitive(const QString &input);

// Everything the report may contain; every member is safe by selection.
struct Report {
    QString appVersion;
    QString qtVersion;
    QString backendName;
    QString rustSdkVersion;
    QString buildType;

    QString osProduct;
    QString kernelVersion;
    QString desktopSession;
    QString sessionType;

    QString secretStoreBackend;
    bool secretStoreSecure = false;

    int accountCount = 0;
    QStringList accountHashes;
    QString activeAccountHash;
    QString storeLayoutVersion;

    QString connectionStatus;
    QString syncMode;
    QString loginStage;
    bool initialSyncDone = false;

    QString localSessionFailureReason;
    QString localSessionFailureAccountHash;

    QString sessionTrustState;
    QString verificationState;
    QString cryptoStatusSummary;
    bool crossSigningAvailable = false;
    bool keyBackupUsable = false;
};

// Renders plain text for a bug report. The whole document passes through
// redactSensitive(), so a new field cannot bypass the filter.
QString renderReport(const Report &report);

} // namespace matrix::app_diagnostics
