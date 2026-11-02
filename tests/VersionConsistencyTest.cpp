
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

QString firstCapture(const QString &text, const QString &pattern)
{
    const QRegularExpression re(pattern);
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1) : QString();
}

const QString kTriple = QStringLiteral("([0-9]+\\.[0-9]+\\.[0-9]+)");

QString labelPattern()
{
    return QStringLiteral("set\\(APP_VERSION_LABEL\\s+\"") + kTriple
           + QStringLiteral("\"");
}

}

class VersionConsistencyTest : public QObject
{
    Q_OBJECT

private slots:
    void everyVersionInTheTreeAgreesWithCMake()
    {
        const QString cmake = readFile(QStringLiteral("CMakeLists.txt"));
        QVERIFY2(!cmake.isEmpty(), "CMakeLists.txt is unreadable");

        const QString projectVersion = firstCapture(
            cmake,
            QStringLiteral("project\\(lightning\\s*\\n\\s*VERSION\\s+") + kTriple);
        QVERIFY2(!projectVersion.isEmpty(),
                 "could not read VERSION from the project() call");
        const QString label = firstCapture(cmake, labelPattern());
        QVERIFY2(!label.isEmpty(), "could not read APP_VERSION_LABEL");
        QCOMPARE(label, projectVersion);

        const QString cargoToml = readFile(QStringLiteral("rust/Cargo.toml"));
        QVERIFY2(!cargoToml.isEmpty(), "rust/Cargo.toml is unreadable");
        const QString cargoVersion = firstCapture(
            cargoToml,
            QStringLiteral("(?m)^version = \"") + kTriple + QStringLiteral("\""));
        QCOMPARE(cargoVersion, projectVersion);

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

    void theReadmeDoesNotAdvertiseAnOlderRelease()
    {
        const QString cmake = readFile(QStringLiteral("CMakeLists.txt"));
        QVERIFY2(!cmake.isEmpty(), "CMakeLists.txt is unreadable");
        const QString version = firstCapture(cmake, labelPattern());
        QVERIFY2(!version.isEmpty(), "could not read APP_VERSION_LABEL");

        const QStringList files = {QStringLiteral("README.md"),
                                   QStringLiteral("docs/install.md")};

        const QRegularExpression triple(QStringLiteral("\\b") + kTriple
                                        + QStringLiteral("\\b"));
        QStringList wrong;
        int ours = 0;
        for (const QString &file : files) {
            const QString text = readFile(file);
            QVERIFY2(!text.isEmpty(),
                     qPrintable(file + QStringLiteral(" is unreadable")));
            const QStringList lines = text.split(QLatin1Char('\n'));
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
                        continue;
                    ++ours;
                    const QString where =
                        QStringLiteral("%1 in %2").arg(found, file);
                    if (found != version && !wrong.contains(where))
                        wrong.append(where);
                }
            }
        }

        QVERIFY2(ours >= 5,
                 qPrintable(QStringLiteral(
                     "only %1 Lightning version(s) found in README.md and "
                     "docs/install.md -- the install commands have been "
                     "reworded or moved and this check no longer reaches "
                     "them").arg(ours)));

        QVERIFY2(wrong.isEmpty(),
                 qPrintable(QStringLiteral(
                     "found version(s) %1 but this tree is %2. Every install "
                     "command there then names a file the download page no "
                     "longer serves. Bump them together -- that is what this "
                     "case exists to force.")
                                .arg(wrong.join(QStringLiteral(", ")), version)));
    }
};

QTEST_APPLESS_MAIN(VersionConsistencyTest)
#include "VersionConsistencyTest.moc"
