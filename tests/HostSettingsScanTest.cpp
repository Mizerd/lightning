// Live accounts Phase 1, I8 (R1-H7): background hosts never read or write the
// ACTIVE account's state.
//
// A source contract over every src/accounts/*.cpp, NotificationContextBuilder
// and AppController's host wiring (the region between the markers
// "live-accounts host wiring: begin" and "...: end"): none of them calls the
// active-account accessors (activeAccountUserId, userId(), accessToken(),
// refreshToken(), the room-mode readers without "For"), the foreground client
// (m_client), the MediaBridge, or the current room; and only KeyringWorker.cpp
// touches the keyring (its worker thread, or SettingsManager by a queued GUI
// call, contract §5.6). SessionTokenCustody.cpp is exempt from the keyring
// rule alone: its functions are pure over an injected TokenStore and run on
// whatever thread their caller is on (the worker, or the foreground client's
// own path). Comments and string literals are ignored, so a log
// line or a comment naming a forbidden call is not a violation and a real call
// cannot hide in one.
//
// Also pinned here: the keyring item names KeyringWorker.cpp writes are
// SettingsManager.cpp's own (file-local there), so the two can never drift.
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtTest>

namespace {

QString stripCommentsAndStrings(const QString &src)
{
    QString out = src;
    const qsizetype n = src.size();
    qsizetype i = 0;
    const auto blank = [&out](qsizetype from, qsizetype to) {
        for (qsizetype k = from; k < to; ++k) {
            if (out.at(k) != QLatin1Char('\n'))
                out[k] = QLatin1Char(' ');
        }
    };
    while (i < n) {
        const QChar c = src.at(i);
        const QChar next = i + 1 < n ? src.at(i + 1) : QChar();
        if (c == QLatin1Char('/') && next == QLatin1Char('/')) {
            const qsizetype end = src.indexOf(QLatin1Char('\n'), i);
            const qsizetype stop = end < 0 ? n : end;
            blank(i, stop);
            i = stop;
            continue;
        }
        if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
            const qsizetype end = src.indexOf(QLatin1String("*/"), i + 2);
            const qsizetype stop = end < 0 ? n : end + 2;
            blank(i, stop);
            i = stop;
            continue;
        }
        if (c == QLatin1Char('R') && next == QLatin1Char('"')
            && (i == 0 || !(src.at(i - 1).isLetterOrNumber() || src.at(i - 1) == QLatin1Char('_')))) {
            const qsizetype open = src.indexOf(QLatin1Char('('), i + 2);
            if (open > 0) {
                const QString delim = src.mid(i + 2, open - (i + 2));
                const QString close = QLatin1Char(')') + delim + QLatin1Char('"');
                const qsizetype end = src.indexOf(close, open + 1);
                const qsizetype stop = end < 0 ? n : end + close.size();
                blank(i, stop);
                i = stop;
                continue;
            }
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            qsizetype j = i + 1;
            while (j < n && src.at(j) != c) {
                if (src.at(j) == QLatin1Char('\\'))
                    ++j;
                ++j;
            }
            blank(i, std::min(n, j + 1));
            i = j + 1;
            continue;
        }
        ++i;
    }
    return out;
}

// Forbidden in every scanned file.
const QList<QRegularExpression> &activeStatePatterns()
{
    static const QList<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral("\\bactiveAccountUserId\\b")),
        QRegularExpression(QStringLiteral("\\bactiveAccountSlug\\w*\\b")),
        // The active-account accessors take no argument; the *For(uid) ones do.
        QRegularExpression(QStringLiteral("\\buserId\\s*\\(\\s*\\)")),
        QRegularExpression(QStringLiteral("\\baccessToken\\s*\\(\\s*\\)")),
        QRegularExpression(QStringLiteral("\\brefreshToken\\s*\\(\\s*\\)")),
        QRegularExpression(QStringLiteral(
            "\\b(roomNotificationMode|roomNotificationModeUnsynced|"
            "roomNotificationModeUnsyncedBase|roomNotificationModeDeviceOnly|"
            "hasStoredRoomNotificationMode|roomUnreadMode|roomNotificationModeDecidesLocally)"
            "\\s*\\(")),
        QRegularExpression(QStringLiteral("\\bm_client\\b")),
        QRegularExpression(QStringLiteral("\\bMediaBridge\\b")),
        QRegularExpression(QStringLiteral("\\bcurrentRoomId\\s*\\(")),
        QRegularExpression(QStringLiteral("\\bm_currentRoomId\\b")),
    };
    return patterns;
}

// Forbidden outside KeyringWorker.cpp: any keyring call.
const QRegularExpression &keyringPattern()
{
    static const QRegularExpression re(QStringLiteral(
        "\\b(readSecret|storeSecret|deleteSecret|clearAccountSecrets|readLegacySecret|"
        "accessTokenFor|refreshTokenFor|oauthClientIdFor|updateSessionTokens|secretStore)"
        "\\s*\\("));
    return re;
}

QStringList violations(const QString &label, const QString &text, bool keyringAllowed)
{
    const QString stripped = stripCommentsAndStrings(text);
    QStringList out;
    const auto lineOf = [&stripped](qsizetype pos) {
        return int(stripped.left(pos).count(QLatin1Char('\n'))) + 1;
    };
    for (const QRegularExpression &re : activeStatePatterns()) {
        auto it = re.globalMatch(stripped);
        while (it.hasNext()) {
            const auto m = it.next();
            out << QStringLiteral("%1:%2: %3").arg(label).arg(lineOf(m.capturedStart()))
                       .arg(m.captured(0));
        }
    }
    if (!keyringAllowed) {
        auto it = keyringPattern().globalMatch(stripped);
        while (it.hasNext()) {
            const auto m = it.next();
            out << QStringLiteral("%1:%2: %3 (keyring outside KeyringWorker)")
                       .arg(label).arg(lineOf(m.capturedStart())).arg(m.captured(0));
        }
    }
    return out;
}

QString sourceRoot()
{
#ifdef LIGHTNING_SOURCE_DIR
    return QStringLiteral(LIGHTNING_SOURCE_DIR);
#else
    return {};
#endif
}

QString readSource(const QString &relative)
{
    QFile file(sourceRoot() + QLatin1Char('/') + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString literalOf(const QString &text, const QString &constant)
{
    const QRegularExpression re(QRegularExpression::escape(constant)
                                + QStringLiteral("\\s*=\\s*\"([^\"]+)\""));
    return re.match(text).captured(1);
}

} // namespace

class HostSettingsScanTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theScannerCatchesActiveStateAndKeyringCalls();
    void accountsSourcesNeverReachTheActiveAccount();
    void theContextBuilderStaysPure();
    void appControllersHostWiringNeverReachesTheActiveAccount();
    void keyringItemNamesMatchSettingsManager();
};

// The scanner must be able to fail, and must not fail on the uid-explicit
// readers, on a field assignment, or on a comment or a log string.
void HostSettingsScanTest::theScannerCatchesActiveStateAndKeyringCalls()
{
    const QString bad = QStringLiteral(
        "void f()\n{\n"
        "    const QString uid = m_settings->activeAccountUserId();\n"
        "    const int mode = m_settings->roomNotificationMode(room);\n"
        "    const QString who = settings->userId();\n"
        "    m_client->roomInfo(room);\n"
        "    const QString token = settings->accessTokenFor(uid);\n"
        "}\n");
    const QStringList found = violations(QStringLiteral("bad"), bad, false);
    QCOMPARE(found.size(), 5);

    const QString good = QStringLiteral(
        "void g()\n{\n"
        "    // activeAccountUserId() is never read here; m_client neither\n"
        "    qCInfo(lc) << \"no m_client, no roomNotificationMode(\";\n"
        "    const int mode = settings->roomNotificationModeFor(uid, room);\n"
        "    in.currentRoomId = QString();\n"
        "    const bool local = settings->roomNotificationModeDecidesLocallyFor(uid, room);\n"
        "    identity.userId = uid;\n"
        "}\n");
    const QStringList none = violations(QStringLiteral("good"), good, false);
    QVERIFY2(none.isEmpty(), qPrintable(none.join(QLatin1Char('\n'))));
    // The keyring calls are KeyringWorker's own.
    QCOMPARE(violations(QStringLiteral("kw"),
                        QStringLiteral("void k()\n{\n    s->readSecret(uid, key);\n}\n"), true)
                 .size(),
             0);
}

void HostSettingsScanTest::accountsSourcesNeverReachTheActiveAccount()
{
    if (sourceRoot().isEmpty())
        QFAIL("LIGHTNING_SOURCE_DIR not defined for this target: the scan cannot run");
    const QDir dir(sourceRoot() + QStringLiteral("/src/accounts"));
    const QStringList files = dir.entryList({QStringLiteral("*.cpp")}, QDir::Files, QDir::Name);
    // The Phase 1 host machinery must all be here, or the scan proves nothing.
    for (const char *expected : {"SessionPool.cpp", "BackgroundSessionHost.cpp",
                                 "BackgroundDrainer.cpp", "SessionScheduler.cpp",
                                 "RetirementLatches.cpp", "HostRetirementPool.cpp",
                                 "KeyringWorker.cpp"}) {
        QVERIFY2(files.contains(QLatin1String(expected)), expected);
    }
    QStringList all;
    int scanned = 0;
    for (const QString &file : files) {
        const QString text = readSource(QStringLiteral("src/accounts/") + file);
        QVERIFY2(!text.isEmpty(), qPrintable(file));
        ++scanned;
        all << violations(file, text,
                          file == QLatin1String("KeyringWorker.cpp")
                              || file == QLatin1String("SessionTokenCustody.cpp"));
    }
    QVERIFY(scanned >= 7);
    QVERIFY2(all.isEmpty(), qPrintable(all.join(QLatin1Char('\n'))));
}

void HostSettingsScanTest::theContextBuilderStaysPure()
{
    if (sourceRoot().isEmpty())
        QFAIL("LIGHTNING_SOURCE_DIR not defined for this target: the scan cannot run");
    for (const QString &file : {QStringLiteral("src/notifications/NotificationContextBuilder.cpp"),
                               QStringLiteral("src/notifications/NotificationContextBuilder.h")}) {
        const QString text = readSource(file);
        QVERIFY2(!text.isEmpty(), qPrintable(file));
        const QStringList found = violations(file, text, false);
        QVERIFY2(found.isEmpty(), qPrintable(found.join(QLatin1Char('\n'))));
        // It takes its inputs; it never includes what it could read them from.
        for (const char *header : {"AppController.h", "SettingsManager.h", "MatrixClient.h",
                                   "MediaBridge.h"}) {
            QVERIFY2(!text.contains(QLatin1String(header)),
                     qPrintable(file + QStringLiteral(" includes ") + QLatin1String(header)));
        }
    }
}

void HostSettingsScanTest::appControllersHostWiringNeverReachesTheActiveAccount()
{
    if (sourceRoot().isEmpty())
        QFAIL("LIGHTNING_SOURCE_DIR not defined for this target: the scan cannot run");
    const QString text = readSource(QStringLiteral("src/app/AppController.cpp"));
    QVERIFY2(!text.isEmpty(), "AppController.cpp not readable");
    const QString begin = QStringLiteral("live-accounts host wiring: begin");
    const QString end = QStringLiteral("live-accounts host wiring: end");
    int regions = 0;
    QStringList all;
    qsizetype from = 0;
    while (true) {
        const qsizetype b = text.indexOf(begin, from);
        if (b < 0)
            break;
        const qsizetype e = text.indexOf(end, b);
        QVERIFY2(e > b, "a host wiring region is never closed");
        ++regions;
        all << violations(QStringLiteral("AppController.cpp (host wiring %1)").arg(regions),
                          text.mid(b, e - b), false);
        from = e;
    }
    // Exactly the two marked regions (the models and pool construction, the
    // pool's signal wiring): a third is a new place that must be reviewed
    // and counted here, and fewer means one lost its markers (§18 round 1,
    // L11).
    QCOMPARE(regions, 2);
    QVERIFY2(all.isEmpty(), qPrintable(all.join(QLatin1Char('\n'))));
}

void HostSettingsScanTest::keyringItemNamesMatchSettingsManager()
{
    if (sourceRoot().isEmpty())
        QFAIL("LIGHTNING_SOURCE_DIR not defined for this target: the scan cannot run");
    const QString settings = readSource(QStringLiteral("src/app/SettingsManager.cpp"));
    const QString worker = readSource(QStringLiteral("src/accounts/KeyringWorker.cpp"));
    QVERIFY(!settings.isEmpty() && !worker.isEmpty());
    for (const QString &name : {QStringLiteral("kSecretAccessToken"),
                               QStringLiteral("kSecretRefreshToken"),
                               QStringLiteral("kSecretOAuthClientId")}) {
        const QString expected = literalOf(settings, name);
        QVERIFY2(!expected.isEmpty(), qPrintable(name + QStringLiteral(" not in SettingsManager.cpp")));
        QCOMPARE(literalOf(worker, name), expected);
    }
}

QTEST_GUILESS_MAIN(HostSettingsScanTest)
#include "HostSettingsScanTest.moc"
