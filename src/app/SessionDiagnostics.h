#pragma once

#include <QString>
#include <QStringList>

namespace matrix::app_diagnostics {

// Short pseudonym for correlating an account within one report. `salt` must
// be random per report: an unsalted hash of a Matrix ID is reversible by
// hashing candidates. Empty input yields an empty string.
QString hashIdentifier(const QString &value, const QByteArray &salt);

QByteArray newReportSalt();

QString redactSensitive(const QString &input);

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

QString renderReport(const Report &report);

}
