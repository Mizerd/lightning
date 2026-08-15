// Lightning secure update system: semantic version parsing and ordering.
//
// Update decisions ride on these comparisons, so the suite pins the three
// behaviours that a lexicographic compare would get wrong (0.10 above 0.9,
// prerelease below release, build metadata irrelevant) and proves that
// malformed input is REPORTED rather than guessed.

#include "update/Version.h"

#include <QtTest/QtTest>

using lightning::update::Version;

namespace {

Version mustParse(const QString &text)
{
    const std::optional<Version> parsed = Version::parse(text);
    if (!parsed) {
        qWarning("version %s did not parse", qPrintable(text));
        return {};
    }
    return *parsed;
}

} // namespace

class UpdateVersionTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesReleaseVersions_data();
    void parsesReleaseVersions();
    void rejectsMalformedInput_data();
    void rejectsMalformedInput();
    void ordersNumericallyNotLexicographically();
    void prereleaseSortsBelowItsRelease();
    void prereleaseIdentifiersFollowSemver();
    void buildMetadataIsIgnoredForOrdering();
    void roundTripsThroughToString();
    void reportsPrereleaseState();
};

void UpdateVersionTest::parsesReleaseVersions_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("major");
    QTest::addColumn<int>("minor");
    QTest::addColumn<int>("patch");
    QTest::addColumn<bool>("prerelease");

    QTest::newRow("zero") << "0.0.0" << 0 << 0 << 0 << false;
    QTest::newRow("release") << "0.7.0" << 0 << 7 << 0 << false;
    QTest::newRow("two digits") << "0.10.12" << 0 << 10 << 12 << false;
    QTest::newRow("rc") << "0.8.0-rc1" << 0 << 8 << 0 << true;
    QTest::newRow("dotted prerelease") << "1.0.0-alpha.1" << 1 << 0 << 0 << true;
    QTest::newRow("build metadata") << "1.2.3+build.5" << 1 << 2 << 3 << false;
    QTest::newRow("both") << "1.2.3-rc.1+sha.abcdef" << 1 << 2 << 3 << true;
    QTest::newRow("hyphen in prerelease") << "1.2.3-x-y.2" << 1 << 2 << 3 << true;
}

void UpdateVersionTest::parsesReleaseVersions()
{
    QFETCH(QString, text);
    QFETCH(int, major);
    QFETCH(int, minor);
    QFETCH(int, patch);
    QFETCH(bool, prerelease);

    const std::optional<Version> parsed = Version::parse(text);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->majorVersion(), major);
    QCOMPARE(parsed->minorVersion(), minor);
    QCOMPARE(parsed->patchVersion(), patch);
    QCOMPARE(parsed->isPrerelease(), prerelease);
}

void UpdateVersionTest::rejectsMalformedInput_data()
{
    QTest::addColumn<QString>("text");

    QTest::newRow("empty") << "";
    QTest::newRow("one component") << "1";
    QTest::newRow("two components") << "1.2";
    QTest::newRow("four components") << "1.2.3.4";
    QTest::newRow("tag prefix") << "v1.2.3";
    QTest::newRow("leading zero") << "01.2.3";
    QTest::newRow("leading zero patch") << "1.2.03";
    QTest::newRow("negative") << "1.2.-3";
    QTest::newRow("letters") << "a.b.c";
    QTest::newRow("empty prerelease") << "1.2.3-";
    QTest::newRow("empty prerelease identifier") << "1.2.3-alpha..1";
    QTest::newRow("numeric prerelease leading zero") << "1.2.3-01";
    QTest::newRow("empty build") << "1.2.3+";
    QTest::newRow("space inside") << "1.2.3-al pha";
    QTest::newRow("leading space") << " 1.2.3";
    QTest::newRow("trailing space") << "1.2.3 ";
    QTest::newRow("newline") << "1.2.3\n";
    QTest::newRow("illegal character") << "1.2.3-alpha_1";
    QTest::newRow("huge component") << "1234567890.0.0";
}

void UpdateVersionTest::rejectsMalformedInput()
{
    QFETCH(QString, text);
    // nullopt, never a guessed value: the caller must fail the check.
    QVERIFY(!Version::parse(text).has_value());
}

void UpdateVersionTest::ordersNumericallyNotLexicographically()
{
    QVERIFY(mustParse("0.9.0") < mustParse("0.10.0"));
    QVERIFY(mustParse("0.9.9") < mustParse("0.10.0"));
    QVERIFY(mustParse("1.0.0") > mustParse("0.99.99"));
    QVERIFY(mustParse("0.7.0") < mustParse("0.7.1"));
    QVERIFY(mustParse("0.7.0") == mustParse("0.7.0"));
    QVERIFY(mustParse("0.7.0") <= mustParse("0.7.0"));
    QVERIFY(mustParse("0.7.0") >= mustParse("0.7.0"));
    QVERIFY(mustParse("0.7.1") != mustParse("0.7.0"));
}

void UpdateVersionTest::prereleaseSortsBelowItsRelease()
{
    QVERIFY(mustParse("0.8.0-rc1") < mustParse("0.8.0"));
    QVERIFY(mustParse("0.8.0") > mustParse("0.8.0-rc1"));
    // ...but still above the previous release.
    QVERIFY(mustParse("0.8.0-rc1") > mustParse("0.7.9"));
}

void UpdateVersionTest::prereleaseIdentifiersFollowSemver()
{
    const QStringList ascending{
        QStringLiteral("1.0.0-alpha"),  QStringLiteral("1.0.0-alpha.1"),
        QStringLiteral("1.0.0-alpha.beta"), QStringLiteral("1.0.0-beta"),
        QStringLiteral("1.0.0-beta.2"), QStringLiteral("1.0.0-beta.11"),
        QStringLiteral("1.0.0-rc.1"),   QStringLiteral("1.0.0"),
    };
    for (int i = 1; i < ascending.size(); ++i) {
        const Version lower = mustParse(ascending.at(i - 1));
        const Version higher = mustParse(ascending.at(i));
        QVERIFY2(lower < higher,
                 qPrintable(ascending.at(i - 1) + QStringLiteral(" < ") + ascending.at(i)));
        QVERIFY(higher > lower);
    }
    // Numeric identifiers sort below alphanumeric ones.
    QVERIFY(mustParse("1.0.0-1") < mustParse("1.0.0-alpha"));
}

void UpdateVersionTest::buildMetadataIsIgnoredForOrdering()
{
    QVERIFY(mustParse("1.2.3+aaa") == mustParse("1.2.3+zzz"));
    QVERIFY(mustParse("1.2.3+aaa") == mustParse("1.2.3"));
    QVERIFY(!(mustParse("1.2.3+aaa") < mustParse("1.2.3+zzz")));
    QVERIFY(mustParse("1.2.3+zzz") < mustParse("1.2.4"));
}

void UpdateVersionTest::roundTripsThroughToString()
{
    QCOMPARE(mustParse("1.2.3").toString(), QStringLiteral("1.2.3"));
    QCOMPARE(mustParse("0.8.0-rc.1").toString(), QStringLiteral("0.8.0-rc.1"));
    QCOMPARE(mustParse("0.8.0-rc.1+sha.7").toString(), QStringLiteral("0.8.0-rc.1+sha.7"));
    QCOMPARE(mustParse("0.8.0-rc.1+sha.7").toComparableString(), QStringLiteral("0.8.0-rc.1"));
}

void UpdateVersionTest::reportsPrereleaseState()
{
    QVERIFY(!mustParse("1.0.0").isPrerelease());
    QVERIFY(mustParse("1.0.0-rc1").isPrerelease());
    QVERIFY(!mustParse("1.0.0+rc1").isPrerelease()); // build metadata is not a prerelease
}

QTEST_APPLESS_MAIN(UpdateVersionTest)
#include "UpdateVersionTest.moc"
