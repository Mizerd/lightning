// Every place that quotes the release version must quote the same one:
// CMake's project() and APP_VERSION_LABEL, rust/Cargo.toml, rust/Cargo.lock
// and the README.
//
// A consistency check, not a value check: CMakeLists.txt is authoritative for
// what the version is.

#include <QtTest/QtTest>

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace {

QString readFile(const QString &relative)
{
    QFile f(QStringLiteral(REPO_ROOT "/") + relative);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

/// First capture of `pattern` in `text`, or an empty string.
QString firstCapture(const QString &text, const QString &pattern)
{
    const QRegularExpression re(pattern);
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1) : QString();
}

// Written with escapes rather than raw strings: several patterns match a
// literal double quote, and a raw string whose body ends in `)"` closes
// early.
const QString kTriple = QStringLiteral("([0-9]+\\.[0-9]+\\.[0-9]+)");

QString labelPattern()
{
    return QStringLiteral("set\\(APP_VERSION_LABEL\\s+\"") + kTriple
           + QStringLiteral("\"");
}

} // namespace

class VersionConsistencyTest : public QObject
{
    Q_OBJECT

private slots:
    void everyVersionInTheTreeAgreesWithCMake()
    {
        const QString cmake = readFile(QStringLiteral("CMakeLists.txt"));
        QVERIFY2(!cmake.isEmpty(), "CMakeLists.txt is unreadable");

        // The authority. Both spellings must already agree with each other:
        // `project(VERSION ...)` feeds the build, APP_VERSION_LABEL feeds
        // `--version`.
        const QString projectVersion = firstCapture(
            cmake,
            QStringLiteral("project\\(lightning\\s*\\n\\s*VERSION\\s+") + kTriple);
        QVERIFY2(!projectVersion.isEmpty(),
                 "could not read VERSION from the project() call");
        const QString label = firstCapture(cmake, labelPattern());
        QVERIFY2(!label.isEmpty(), "could not read APP_VERSION_LABEL");
        QCOMPARE(label, projectVersion);

        // The Rust crate, whose version also derives the HTTP user agent.
        const QString cargoToml = readFile(QStringLiteral("rust/Cargo.toml"));
        QVERIFY2(!cargoToml.isEmpty(), "rust/Cargo.toml is unreadable");
        const QString cargoVersion = firstCapture(
            cargoToml,
            QStringLiteral("(?m)^version = \"") + kTriple + QStringLiteral("\""));
        QCOMPARE(cargoVersion, projectVersion);

        // The lock file's entry for our crate, matched by package name so an
        // unrelated crate with the same version cannot satisfy it.
        const QString cargoLock = readFile(QStringLiteral("rust/Cargo.lock"));
        QVERIFY2(!cargoLock.isEmpty(), "rust/Cargo.lock is unreadable");
        const QString lockVersion = firstCapture(
            cargoLock,
            QStringLiteral("name = \"matrix-client-rust\"\\s*\\nversion = \"")
                + kTriple + QStringLiteral("\""));
        QVERIFY2(!lockVersion.isEmpty(),
                 "could not find matrix-client-rust in rust/Cargo.lock");
        QCOMPARE(lockVersion, projectVersion);
    }

    // ---- and the file a new user actually reads ----

    void theReadmeDoesNotAdvertiseAnOlderRelease()
    {
        const QString cmake = readFile(QStringLiteral("CMakeLists.txt"));
        QVERIFY2(!cmake.isEmpty(), "CMakeLists.txt is unreadable");
        const QString version = firstCapture(cmake, labelPattern());
        QVERIFY2(!version.isEmpty(), "could not read APP_VERSION_LABEL");

        const QString readme = readFile(QStringLiteral("README.md"));
        QVERIFY2(!readme.isEmpty(), "README.md is unreadable");

        // Ours only: the README also quotes dependency versions (Qt,
        // GStreamer) that must not move with a release. A version counts if
        // its line mentions Lightning (every install command, flake ref and
        // badge URL does) or if it is in backticks.
        const QRegularExpression triple(QStringLiteral("\\b") + kTriple
                                        + QStringLiteral("\\b"));
        QStringList wrong;
        int ours = 0;
        const QStringList lines = readme.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const bool mentionsProduct =
                line.contains(QStringLiteral("lightning"), Qt::CaseInsensitive);
            auto it = triple.globalMatch(line);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                const QString found = m.captured(1);
                const int at = m.capturedStart(1);
                const bool backticked =
                    at > 0 && at + found.size() < line.size()
                    && line.at(at - 1) == QLatin1Char('`')
                    && line.at(at + found.size()) == QLatin1Char('`');
                if (!mentionsProduct && !backticked)
                    continue;   // a dependency's version, not ours
                ++ours;
                if (found != version && !wrong.contains(found))
                    wrong.append(found);
            }
        }

        // A scan that matches nothing passes vacuously, so require a floor.
        QVERIFY2(ours >= 5,
                 qPrintable(QStringLiteral(
                     "only %1 Lightning version(s) found in README.md -- the "
                     "install commands have been reworded and this check no "
                     "longer reaches them").arg(ours)));

        QVERIFY2(wrong.isEmpty(),
                 qPrintable(QStringLiteral(
                     "README.md quotes version(s) %1 but this tree is %2. "
                     "Every install command in the README then names a file "
                     "the download page no longer serves. Bump them together "
                     "-- that is what this case exists to force.")
                                .arg(wrong.join(QStringLiteral(", ")), version)));
    }
};

QTEST_APPLESS_MAIN(VersionConsistencyTest)
#include "VersionConsistencyTest.moc"
