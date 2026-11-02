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

struct Violation {
    QString call;
    int line = 0;
};

QList<Violation> unclaimedCalls(const QString &stripped, const QRegularExpression &call,
                                const QString &claim, int *found)
{
    QList<Violation> out;
    *found = 0;
    auto it = call.globalMatch(stripped);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const qsizetype pos = m.capturedStart();
        ++*found;
        const qsizetype open = stripped.lastIndexOf(QLatin1String("\n{"), pos);
        const qsizetype close = stripped.indexOf(QLatin1String("\n}"), pos);
        const bool insideOneFunction = open >= 0 && (close < 0 || close > pos)
            && stripped.indexOf(QLatin1String("\n}"), open) >= pos;
        const QString before = insideOneFunction ? stripped.mid(open, pos - open) : QString();
        if (!before.contains(claim)) {
            Violation v;
            v.call = m.captured(1);
            v.line = int(stripped.left(pos).count(QLatin1Char('\n'))) + 1;
            out.append(v);
        }
    }
    return out;
}

QString readSource(const QString &relative)
{
#ifdef LIGHTNING_SOURCE_DIR
    QFile file(QStringLiteral(LIGHTNING_SOURCE_DIR "/") + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
#else
    Q_UNUSED(relative);
    return {};
#endif
}

const QRegularExpression &openCall()
{
    static const QRegularExpression re(QStringLiteral("\\b(mx_rust_create)\\s*\\("));
    return re;
}

const QRegularExpression &deletionCall()
{
    static const QRegularExpression re(QStringLiteral(
        "\\b(quarantineRustStore|quarantineAccountRustState|removeAccountRustState|"
        "removeAttemptRustStore)\\s*\\("));
    return re;
}

}

class ClaimCallerScanTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theScannerCatchesAnUnclaimedCall();
    void everyRustClientStoreEntryPointClaimsFirst();
    void everyAppControllerStoreDeletionClaimsFirst();
    void theClientsLabsBranchesAreGated();
};

void ClaimCallerScanTest::theScannerCatchesAnUnclaimedCall()
{
    const QString source = QStringLiteral(
        "void a()\n{\n    removeAccountRustState(identity);\n}\n"
        "void b()\n{\n    registry.claimForDeletion(path);\n}\n"
        "void c()\n{\n    quarantineRustStore(identity);\n    claimForDeletion(path);\n}\n"
        "void d()\n{\n    // claimForDeletion(path)\n    const char *s = \"claimForDeletion(\";\n"
        "    quarantineAccountRustState(identity);\n}\n"
        "void e()\n{\n    if (registry.claimForDeletion(path) == Granted)\n"
        "        removeAccountRustState(identity);\n}\n"
        "void f()\n{\n    m_rustHandle = mx_rust_create(path);\n}\n"
        "void g()\n{\n    claimOpen(path);\n    mx_rust_create_dm(x);\n"
        "    h = mx_rust_create(path);\n}\n"
        "void h()\n{\n    removeAttemptRustStore(store);\n}\n");
    const QString stripped = stripCommentsAndStrings(source);
    int found = 0;
    const QList<Violation> deletions =
        unclaimedCalls(stripped, deletionCall(), QStringLiteral("claimForDeletion("), &found);
    QCOMPARE(found, 5);
    QCOMPARE(deletions.size(), 4);
    QCOMPARE(deletions.at(0).call, QStringLiteral("removeAccountRustState"));
    QCOMPARE(deletions.at(1).call, QStringLiteral("quarantineRustStore"));
    QCOMPARE(deletions.at(2).call, QStringLiteral("quarantineAccountRustState"));
    QCOMPARE(deletions.at(3).call, QStringLiteral("removeAttemptRustStore"));
    const QList<Violation> opens =
        unclaimedCalls(stripped, openCall(), QStringLiteral("claimOpen("), &found);
    QCOMPARE(found, 2);
    QCOMPARE(opens.size(), 1);
}

void ClaimCallerScanTest::everyRustClientStoreEntryPointClaimsFirst()
{
#ifndef LIGHTNING_SOURCE_DIR
    QFAIL("LIGHTNING_SOURCE_DIR not defined for this target: the scan cannot run");
#endif
    const QString source = readSource(QStringLiteral("src/matrix/RustSdkMatrixClient.cpp"));
    QVERIFY2(!source.isEmpty(), "RustSdkMatrixClient.cpp not readable");
    const QString stripped = stripCommentsAndStrings(source);

    int opens = 0;
    const QList<Violation> unclaimedOpens =
        unclaimedCalls(stripped, openCall(), QStringLiteral("claimOpen("), &opens);
    for (const Violation &v : unclaimedOpens)
        qWarning("RustSdkMatrixClient.cpp:%d: %s without claimOpen( before it", v.line,
                 qPrintable(v.call));
    QVERIFY(unclaimedOpens.isEmpty());
    QCOMPARE(opens, 1);

    int deletions = 0;
    const QList<Violation> unclaimed = unclaimedCalls(
        stripped, deletionCall(), QStringLiteral("claimForDeletion("), &deletions);
    for (const Violation &v : unclaimed)
        qWarning("RustSdkMatrixClient.cpp:%d: %s without claimForDeletion( before it", v.line,
                 qPrintable(v.call));
    QVERIFY(unclaimed.isEmpty());
    QCOMPARE(deletions, 7);
}

void ClaimCallerScanTest::everyAppControllerStoreDeletionClaimsFirst()
{
#ifndef LIGHTNING_SOURCE_DIR
    QFAIL("LIGHTNING_SOURCE_DIR not defined for this target: the scan cannot run");
#endif
    const QString source = readSource(QStringLiteral("src/app/AppController.cpp"));
    QVERIFY2(!source.isEmpty(), "AppController.cpp not readable");
    const QString stripped = stripCommentsAndStrings(source);
    int opens = 0;
    QVERIFY(unclaimedCalls(stripped, openCall(), QStringLiteral("claimOpen("), &opens).isEmpty());
    QCOMPARE(opens, 0);
    int deletions = 0;
    const QList<Violation> unclaimed = unclaimedCalls(
        stripped, deletionCall(), QStringLiteral("claimForDeletion("), &deletions);
    for (const Violation &v : unclaimed)
        qWarning("AppController.cpp:%d: %s without claimForDeletion( before it", v.line,
                 qPrintable(v.call));
    QVERIFY(unclaimed.isEmpty());
    QVERIFY2(deletions >= 1, "the scan found no store deletion in AppController.cpp");
}

namespace {
QString memberBody(const QString &source, const QString &signature)
{
    const qsizetype start = source.indexOf(signature);
    if (start < 0)
        return {};
    const qsizetype end = source.indexOf(QLatin1String("\n}"), start);
    return source.mid(start, (end < 0 ? source.size() : end) - start);
}
}

void ClaimCallerScanTest::theClientsLabsBranchesAreGated()
{
#ifndef LIGHTNING_SOURCE_DIR
    QFAIL("LIGHTNING_SOURCE_DIR not defined for this target: the scan cannot run");
#endif
    const QString source = stripCommentsAndStrings(
        readSource(QStringLiteral("src/matrix/RustSdkMatrixClient.cpp")));
    QVERIFY(!source.isEmpty());
    for (const char *helper : {"bool RustSdkMatrixClient::labsHardProofAllowsReplace(",
                               "bool RustSdkMatrixClient::labsSoftProofAllowsResume("}) {
        const QString body = memberBody(source, QLatin1String(helper));
        QVERIFY2(!body.isEmpty(), helper);
        const qsizetype guard = body.indexOf(QLatin1String("if (!m_liveAccountsLabs"));
        const qsizetype read = body.indexOf(QLatin1String("m_revocationProofs->"));
        QVERIFY2(guard > 0 && read > guard, helper);
    }
    const QRegularExpression proofRead(QStringLiteral("m_revocationProofs->(hardFor|softFor)\\("));
    QCOMPARE(int(source.count(proofRead)), 2);
    QList<QPair<qsizetype, qsizetype>> guarded;
    {
        const QLatin1String guardText("if (m_liveAccountsLabs &&");
        for (qsizetype at = source.indexOf(guardText); at >= 0;
             at = source.indexOf(guardText, at + 1)) {
            qsizetype i = at + 3;
            int depth = 0;
            for (; i < source.size(); ++i) {
                if (source.at(i) == QLatin1Char('('))
                    ++depth;
                else if (source.at(i) == QLatin1Char(')') && --depth == 0)
                    break;
            }
            qsizetype j = i + 1;
            while (j < source.size() && source.at(j).isSpace())
                ++j;
            qsizetype end = source.indexOf(QLatin1Char(';'), j);
            if (j < source.size() && source.at(j) == QLatin1Char('{')) {
                int braces = 0;
                for (end = j; end < source.size(); ++end) {
                    if (source.at(end) == QLatin1Char('{'))
                        ++braces;
                    else if (source.at(end) == QLatin1Char('}') && --braces == 0)
                        break;
                }
            }
            guarded.append({ at, end });
        }
    }
    const QRegularExpression hostCheck(QStringLiteral("HostSignInRetirements::process\\(\\)"));
    auto it = hostCheck.globalMatch(source);
    int hostChecks = 0;
    while (it.hasNext()) {
        const auto m = it.next();
        ++hostChecks;
        bool inside = false;
        for (const auto &span : std::as_const(guarded))
            inside = inside || (m.capturedStart() > span.first && m.capturedStart() < span.second);
        QVERIFY2(inside, "a host-retirement check outside the Labs switch");
    }
    QCOMPARE(hostChecks, 3);
    const QString open = memberBody(
        source, QStringLiteral("bool RustSdkMatrixClient::ensureRustHandleForStorePath("));
    const qsizetype enforcedClaim = open.indexOf(QLatin1String("if (claimsEnforced) {"));
    const qsizetype create = open.indexOf(QLatin1String("mx_rust_create("));
    const qsizetype phase0Record = open.indexOf(QLatin1String("if (!claimsEnforced)"));
    QVERIFY(enforcedClaim > 0 && create > enforcedClaim && phase0Record > create);
    QCOMPARE(int(source.count(QLatin1String("setEnforced("))), 1);
    const QString labs = memberBody(source, QStringLiteral("void RustSdkMatrixClient::setLiveAccountsLabs("));
    QVERIFY(labs.contains(QLatin1String("setEnforced(true)")));
}

QTEST_GUILESS_MAIN(ClaimCallerScanTest)
#include "ClaimCallerScanTest.moc"
