#pragma once

#include <QString>
#include <QStringList>

// User-invoked support bundle for bug reports.
//
// The design rule is that this file NEVER sees a secret in the first place:
// AppController fills a fixed struct of already-safe fields, and the report is
// assembled only from those. `redactSensitive()` is the second line of defence
// for the handful of fields that carry backend-authored free text (status
// lines, error categories), where an SDK message could in principle embed
// material we did not choose to include.
//
// Deliberately excluded, and there is no field to carry them: access and
// refresh tokens, passwords, recovery keys, private cross-signing or
// secret-storage material, decrypted events, message bodies, room names, room
// ids, media, database contents, and filesystem paths (a store path contains
// the account slug, which is the Matrix localpart).
namespace matrix::app_diagnostics {

// Short pseudonym for an account, for correlating lines WITHIN one report.
//
// `salt` is mandatory and must be random per report. A Matrix ID is drawn
// from a small space, so an UNSALTED hash of one is trivially reversible by
// hashing candidate IDs — and this bundle exists to be pasted in public. A
// fresh per-report salt keeps the only property the report actually needs
// (two lines referring to one account match each other) while making the
// value useless to anyone trying to recover the ID or link two reports.
//
// Empty input yields an empty string rather than the hash of "", so an
// absent value never looks like a present one.
QString hashIdentifier(const QString &value, const QByteArray &salt);

// A fresh random salt. Never persisted: cross-report linkability is not a
// property this bundle should have.
QByteArray newReportSalt();

// Scrubs token-like material from backend-authored free text. Conservative by
// construction: it would rather blank a harmless long identifier than let a
// credential through.
QString redactSensitive(const QString &input);

// Everything the report may contain. Every member is already safe by
// selection; nothing here is a credential, an identifier in the clear, or a
// path.
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

// Renders `report` as plain text suitable for pasting into a bug report. The
// whole rendered document is passed through redactSensitive() before it is
// returned, so a future field addition cannot quietly bypass the filter.
QString renderReport(const Report &report);

} // namespace matrix::app_diagnostics
