// The four places a release version lives, and the one that drifted.
//
// §14 says a version bump updates "those same synchronized locations", and
// names CMake, Rust and the user agent. The README is not on that list, and
// on 2026-09-13 the maintainer found it still advertising **0.9.3** while the
// tree, the tag and the published packages were all on 0.9.4 -- a whole
// release behind, in the one file a new user reads first. Every install
// command in it (the apt path, the AppImage glob, the Flatpak and Snap lines,
// the Nix flake pin) named a version the download page no longer offers.
//
// Nothing could have caught it, because nothing compared them. This does.
//
// IT IS DELIBERATELY A CONSISTENCY CHECK, NOT A VALUE CHECK. It never says
// what the version should BE -- CMakeLists.txt is authoritative for that
// (§14: "read it from CMakeLists.txt rather than from here"). It says only
// that everything which quotes a version quotes the SAME one, so the next
// bump cannot half-land.

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

// Written with escapes rather than raw strings on purpose: several of these
// patterns must match a literal double quote, and a raw string whose body ends
// in `)"` closes itself early -- which is exactly how the first version of
// this file failed to compile.
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
        // `--version`, and 0.8.0 is on record shipping them out of step.
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

        // The lock file's entry for OUR crate specifically. Matched through
        // the package NAME rather than by value: `rand` was coincidentally
        // also 0.9.4, so a value-shaped search here would pass on a lock file
        // that had never been bumped at all.
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

        // OURS ONLY. The README also quotes DEPENDENCY versions -- Qt 6.8.2,
        // GStreamer 1.26.2, the Ubuntu and Fedora Qt levels -- and those must
        // NOT move with a release. A first version of this case matched every
        // triple in the file and flagged all four of them, which would have
        // trained the next person to edit the test instead of the README.
        //
        // Two rules, because the real occurrences fall into exactly two
        // shapes and the dependency ones fall into neither:
        //   * the line mentions Lightning -- every install command does
        //     (`lightning_X_amd64.deb`, `Lightning-X-x86_64.AppImage`, the
        //     flake ref, the release badge URL);
        //   * or the version is in BACKTICKS -- which is how the one bare
        //     prose mention is written ("Replace `0.9.3` below").
        // The dependency versions are plain text on lines about Qt and
        // GStreamer, so neither rule reaches them.
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

        // A scan that matches NOTHING passes vacuously, which is the shape
        // §16 records as "mutation-check every new sweep". The README has
        // carried ten of these for several releases; five is a floor that a
        // reorganisation can cross without a rewrite going unnoticed.
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
