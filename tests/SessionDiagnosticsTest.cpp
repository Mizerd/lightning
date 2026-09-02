// Support-diagnostics redaction. The export exists so a user can paste
// Lightning's state into a bug report; the whole point is that doing so is
// safe. These tests deliberately INJECT credential-shaped values into every
// free-text field the report carries and prove none of them survive, and
// separately prove the report still contains the metadata that makes it worth
// exporting at all — a filter that redacted everything would pass a
// leak-only test.
#include "app/SessionDiagnostics.h"

#include <QCryptographicHash>

#include <QtTest>

using namespace matrix::app_diagnostics;

class SessionDiagnosticsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void hashIsStableNonReversibleAndShort();
    void hashIsNotReversibleByDictionaryAttack();
    void hashOfEmptyIsEmpty();
    void redactsAccessAndRefreshTokens();
    void redactsPasswordsAndPassphrases();
    void redactsRecoveryKeys();
    void redactsBareMatrixUserIds();
    void redactsLongOpaqueKeyMaterial();
    void keepsLongSnakeCaseReasonCodes();
    void reportNeverLeaksInjectedSecrets();
    void reportKeepsUsefulMetadata();
    void reportContainsNoRawUserId();
    void redactsFilesystemPaths();
};

void SessionDiagnosticsTest::hashIsStableNonReversibleAndShort()
{
    const QByteArray salt = newReportSalt();
    const QString a = hashIdentifier(QStringLiteral("@alice:example.org"), salt);
    const QString b = hashIdentifier(QStringLiteral("@alice:example.org"), salt);
    const QString c = hashIdentifier(QStringLiteral("@bob:example.org"), salt);

    QCOMPARE(a, b);                       // stable within one report
    QVERIFY(a != c);                      // distinguishes accounts
    QCOMPARE(a.size(), 16);               // short enough to survive redaction
    QVERIFY(!a.contains(QLatin1String("alice")));
    // Whitespace is normalized so the same account never hashes two ways.
    QCOMPARE(hashIdentifier(QStringLiteral("  @alice:example.org  "), salt), a);
}

void SessionDiagnosticsTest::hashIsNotReversibleByDictionaryAttack()
{
    // The property that matters for a bundle designed to be pasted in public:
    // a Matrix ID is drawn from a small space, so an UNSALTED hash could be
    // reversed by hashing candidate IDs. With a per-report salt, an attacker
    // holding the report cannot reproduce the value without it, and two
    // reports about the same account do not link.
    const QString id = QStringLiteral("@alice:example.org");
    const QByteArray saltA = newReportSalt();
    const QByteArray saltB = newReportSalt();

    QVERIFY(saltA != saltB);                       // fresh per report
    QCOMPARE(saltA.size(), 32);
    QVERIFY(hashIdentifier(id, saltA) != hashIdentifier(id, saltB));

    // The naive construction an attacker would try — hashing the candidate id
    // with no salt — must not match what the report contains.
    const QByteArray unsalted =
        QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256);
    QVERIFY(hashIdentifier(id, saltA)
            != QString::fromLatin1(unsalted.toHex().left(16)));
}

void SessionDiagnosticsTest::hashOfEmptyIsEmpty()
{
    // An absent value must not render as a plausible-looking hash, or a
    // reader cannot tell "no active account" from "some active account".
    const QByteArray salt = newReportSalt();
    QVERIFY(hashIdentifier(QString{}, salt).isEmpty());
    QVERIFY(hashIdentifier(QStringLiteral("   "), salt).isEmpty());
}

void SessionDiagnosticsTest::redactsAccessAndRefreshTokens()
{
    const QString out = redactSensitive(QStringLiteral(
        "sync failed: access_token=syt_bWl6ZXJk_AbCdEfGhIjKlMnOp_1a2b3c "
        "refresh_token: mda_QQQQwwwwEEEErrrrTTTTyyyy"));
    QVERIFY(!out.contains(QLatin1String("syt_")));
    QVERIFY(!out.contains(QLatin1String("mda_")));
    QVERIFY(!out.contains(QLatin1String("bWl6ZXJk")));
    QVERIFY(out.contains(QLatin1String("<redacted>")));
    // The KEY survives so a reader still learns which credential appeared.
    QVERIFY(out.contains(QLatin1String("access_token")));
}

void SessionDiagnosticsTest::redactsPasswordsAndPassphrases()
{
    const QString out = redactSensitive(QStringLiteral(
        "login rejected password=hunter2correcthorse passphrase: \"s3cr3t-phrase\""));
    QVERIFY(!out.contains(QLatin1String("hunter2correcthorse")));
    QVERIFY(!out.contains(QLatin1String("s3cr3t-phrase")));
}

void SessionDiagnosticsTest::redactsRecoveryKeys()
{
    // Matrix recovery keys are conventionally rendered as space-separated
    // groups of four base58 characters.
    const QString out = redactSensitive(QStringLiteral(
        "recovery: EsTc 5rPk 9uWq 2mFa 7vNz 3xLb 8jHd 4kGy 6tRs 1pQw 5nMv 9cZx"));
    QVERIFY(!out.contains(QLatin1String("EsTc")));
    QVERIFY(!out.contains(QLatin1String("9cZx")));
    QVERIFY(out.contains(QLatin1String("<redacted>")));
}

void SessionDiagnosticsTest::redactsBareMatrixUserIds()
{
    const QString out = redactSensitive(
        QStringLiteral("restore failed for @mizerd:matrix.example.net"));
    QVERIFY(!out.contains(QLatin1String("mizerd")));
    QVERIFY(!out.contains(QLatin1String("matrix.example.net")));
}

void SessionDiagnosticsTest::redactsLongOpaqueKeyMaterial()
{
    // The catch-all: base64/hex runs that none of the named rules matched.
    const QString megolm = QStringLiteral(
        "AgAAAAxvbGl2ZXIrbWVnb2xtK3Nlc3Npb24ra2V5K21hdGVyaWFs");
    const QString out = redactSensitive(
        QStringLiteral("imported session ") + megolm);
    QVERIFY(!out.contains(megolm));
    QVERIFY(out.contains(QLatin1String("<redacted>")));
}

void SessionDiagnosticsTest::keepsLongSnakeCaseReasonCodes()
{
    // A filter that blanks the failure classification defeats the purpose of
    // the export. These reason codes exceed the opaque-run threshold
    // ("saved_session_without_store" is 27 characters) and must survive.
    for (const auto &code : {QStringLiteral("saved_session_without_store"),
                             QStringLiteral("store_without_session_metadata"),
                             QStringLiteral("existing_store_requires_restore"),
                             QStringLiteral("sdk_store_ownership_mismatch"),
                             QStringLiteral("invalid_saved_account_identity")}) {
        QVERIFY2(redactSensitive(code).contains(code),
                 qPrintable(QStringLiteral("redacted reason code: %1").arg(code)));
    }
    // But a same-length run that is NOT a plain identifier is still removed.
    const QString mixed = QStringLiteral("AbCd3fGh1jKlMn0pQrSt2vWxYz");
    QVERIFY(!redactSensitive(mixed).contains(mixed));
}

void SessionDiagnosticsTest::reportNeverLeaksInjectedSecrets()
{
    Report r;
    // Every field that carries backend-authored free text gets a credential
    // pushed into it. None of these are values the app would legitimately
    // place here — that is the point: the filter is the backstop for a field
    // we misjudged, or one added later.
    r.appVersion = QStringLiteral("0.6.4");
    r.backendName = QStringLiteral("rust");
    r.rustSdkVersion =
        QStringLiteral("matrix-sdk 0.18 token=syt_leakedtokenvalue_abcdef");
    r.connectionStatus =
        QStringLiteral("error: access_token=syt_anotherleak_zzz rejected");
    r.syncMode = QStringLiteral("sliding");
    r.loginStage = QStringLiteral("authenticating");
    r.cryptoStatusSummary = QStringLiteral(
        "recovery_key: EsTc 5rPk 9uWq 2mFa 7vNz 3xLb 8jHd 4kGy 6tRs 1pQw 5nMv 9cZx");
    r.sessionTrustState = QStringLiteral("Verified for @mizerd:example.net");
    r.verificationState = QStringLiteral("done");
    r.osProduct = QStringLiteral("NixOS 25.11");
    r.desktopSession = QStringLiteral("KDE");

    const QString out = renderReport(r);

    QVERIFY(!out.contains(QLatin1String("syt_")));
    QVERIFY(!out.contains(QLatin1String("leakedtokenvalue")));
    QVERIFY(!out.contains(QLatin1String("anotherleak")));
    QVERIFY(!out.contains(QLatin1String("EsTc")));
    QVERIFY(!out.contains(QLatin1String("mizerd")));
    QVERIFY(!out.contains(QLatin1String("example.net")));
}

void SessionDiagnosticsTest::reportKeepsUsefulMetadata()
{
    Report r;
    r.appVersion = QStringLiteral("0.6.4");
    r.buildType = QStringLiteral("release");
    r.backendName = QStringLiteral("rust");
    r.qtVersion = QStringLiteral("6.11.1");
    r.osProduct = QStringLiteral("NixOS 25.11");
    r.accountCount = 2;
    const QByteArray salt = newReportSalt();
    r.activeAccountHash = hashIdentifier(QStringLiteral("@a:example.org"), salt);
    r.accountHashes = {r.activeAccountHash,
                       hashIdentifier(QStringLiteral("@b:example.org"), salt)};
    r.storeLayoutVersion = QStringLiteral("per-account-root/v1");
    r.secretStoreBackend = QStringLiteral("libsecret");
    r.secretStoreSecure = true;
    r.loginStage = QStringLiteral("authenticating");
    r.localSessionFailureReason = QStringLiteral("saved_session_without_store");
    r.keyBackupUsable = true;

    const QString out = renderReport(r);

    // A report that redacted everything would be useless; these are exactly
    // the fields that make a bug report actionable.
    QVERIFY(out.contains(QLatin1String("0.6.4")));
    QVERIFY(out.contains(QLatin1String("rust")));
    QVERIFY(out.contains(QLatin1String("6.11.1")));
    QVERIFY(out.contains(QLatin1String("NixOS 25.11")));
    QVERIFY(out.contains(QLatin1String("count: 2")));
    QVERIFY(out.contains(QLatin1String("per-account-root/v1")));
    QVERIFY(out.contains(QLatin1String("libsecret")));
    QVERIFY(out.contains(QLatin1String("authenticating")));
    QVERIFY(out.contains(QLatin1String("saved_session_without_store")));
    QVERIFY(out.contains(QLatin1String("key_backup_usable: yes")));
    // The hashes are short enough that the long-opaque-run rule leaves them.
    QVERIFY(out.contains(r.activeAccountHash));
}

void SessionDiagnosticsTest::reportContainsNoRawUserId()
{
    Report r;
    r.accountCount = 1;
    r.activeAccountHash =
        hashIdentifier(QStringLiteral("@mizerd:example.net"), newReportSalt());
    r.accountHashes = {r.activeAccountHash};
    r.localSessionFailureAccountHash = r.activeAccountHash;

    const QString out = renderReport(r);
    QVERIFY(!out.contains(QLatin1String("@mizerd")));
    QVERIFY(!out.contains(QLatin1String("mizerd")));
    QVERIFY(out.contains(r.activeAccountHash));
}


void SessionDiagnosticsTest::redactsFilesystemPaths()
{
    // The header promises no paths, and a store path carries the Matrix
    // localpart. Four free-text fields could carry one, and the long-opaque
    // rule cannot see a path because its class excludes '.' and '/'.
    const QString posix = redactSensitive(QStringLiteral(
        "store unusable: /home/mizerd/.local/share/Lightning/mizerd_matrix.org/store"));
    QVERIFY2(!posix.contains(QLatin1String("mizerd")), qPrintable(posix));
    QVERIFY2(!posix.contains(QLatin1String("/home/")), qPrintable(posix));
    QVERIFY(posix.contains(QLatin1String("store unusable:")));

    const QString windows = redactSensitive(QStringLiteral(
        "cannot open C:\\Users\\mizerd\\AppData\\Local\\Lightning\\store"));
    QVERIFY2(!windows.contains(QLatin1String("mizerd")), qPrintable(windows));

    // Reason codes and version strings are not paths.
    const QString kept = redactSensitive(QStringLiteral("state=connected/synced v6.11.1"));
    QCOMPARE(kept, QStringLiteral("state=connected/synced v6.11.1"));

    // A URL keeps its HOST and loses its path (which can carry ids/tokens).
    const QString url = redactSensitive(QStringLiteral(
        "connect failed: https://matrix.example.org/_matrix/client/versions"));
    QCOMPARE(url, QStringLiteral("connect failed: https://matrix.example.org/<path>"));
    const QString ws = redactSensitive(QStringLiteral("focus wss://sfu.example.net/rtc refused"));
    QCOMPARE(ws, QStringLiteral("focus wss://sfu.example.net/<path> refused"));
    const QString bare = redactSensitive(QStringLiteral("server https://matrix.example.org down"));
    QCOMPARE(bare, QStringLiteral("server https://matrix.example.org down"));
    // An upper-case scheme is the same URL.
    QCOMPARE(redactSensitive(QStringLiteral("HTTPS://matrix.example.org/_matrix/x")),
             QStringLiteral("HTTPS://matrix.example.org/<path>"));
    // ...but a file URL's path is a path.
    const QString fileUrl = redactSensitive(QStringLiteral("opened file:///home/mizerd/store"));
    QVERIFY2(!fileUrl.contains(QLatin1String("mizerd")), qPrintable(fileUrl));
}

QTEST_MAIN(SessionDiagnosticsTest)
#include "SessionDiagnosticsTest.moc"
