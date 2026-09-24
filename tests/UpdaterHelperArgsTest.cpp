// lightning-updater: argv contract and install-strategy planning.
//
// The helper accepts no command strings: every option is enumerated, every
// value checked, injection-shaped values are refused on their own merits, and
// every strategy produces a program plus an argument vector in which an
// awkward path stays one element.

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
    void perMachineMsiPlanAddsAllUsersAndElevates();
    void perMachineSetupPlanPassesAllUsersAndElevates();
    void installScopeIsOptionalAndDefaultsToUser();
    void installScopeParsesForTheTwoWindowsInstallers();
    void installScopeIsRefusedForEveryOtherMode_data();
    void installScopeIsRefusedForEveryOtherMode();
    void installScopeRefusesAnythingButUserOrMachine_data();
    void installScopeRefusesAnythingButUserOrMachine();
    void windowsCommandLineRoundTripsThroughCommandLineToArgv_data();
    void windowsCommandLineRoundTripsThroughCommandLineToArgv();
    void windowsCommandLineRefusesQuotesAndControlCharacters();
    void finalPathBecomesALaunchablePath_data();
    void finalPathBecomesALaunchablePath();
    void elevatedPlansAreRetargetedToTheLockedFile();
    void retargetRefusesAPlanThatDoesNotNameTheLockedFile();
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
    void theRelaunchedApplicationDoesNotInheritADeadTemporaryDirectory();

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
    // Without --relaunch (a plain installUpdate()) the update applies when the
    // user quits and the helper must not start the application again.
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
    // The package path must reach msiexec with backslashes: msiexec reads '/'
    // as a switch, returns 1619 and installs nothing.
    QCOMPARE(result.plan.arguments.at(1),
             QStringLiteral("C:\\Users\\x\\AppData\\Local\\updates\\Lightning-0.8.0.msi"));
    QVERIFY(!result.plan.arguments.at(1).contains(QLatin1Char('/')));
    QCOMPARE(result.plan.arguments.at(2), QStringLiteral("/qb"));
    QCOMPARE(result.plan.arguments.at(3), QStringLiteral("REINSTALLMODE=vomus"));
    // A per-user MSI (the default) keeps exactly this vector and never
    // elevates.
    QVERIFY(!result.plan.elevates);
    QVERIFY(!result.plan.elevateOnWindows);
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
    // "/S" first, then the scope -- explicit, so the installer never has to
    // guess which of two copies a person with both is upgrading.
    QCOMPARE(result.plan.arguments,
             QStringList(kNsisSilentSwitches) << QStringLiteral("/CURRENTUSER"));
    // NSIS parses its switches positionally: /S must be first.
    QCOMPARE(result.plan.arguments.first(), QStringLiteral("/S"));
    // Per-user installer: no elevation of either kind.
    QVERIFY(!result.plan.elevates);
    QVERIFY(!result.plan.elevateOnWindows);
    QVERIFY(result.plan.lockedArtifact.isEmpty());
    // We deliberately do not force an install directory.
    for (const QString &argument : result.plan.arguments)
        QVERIFY(!argument.startsWith(QStringLiteral("/D")));
}

// A per-machine MSI must be upgraded in the per-machine context: without
// ALLUSERS=1 Windows Installer looks only in the per-user context and
// installs a second copy in %LOCALAPPDATA%.
void UpdaterHelperArgsTest::perMachineMsiPlanAddsAllUsersAndElevates()
{
    UpdaterArguments args;
    args.mode = UpdaterMode::WindowsMsi;
    args.installScope = updater::InstallScope::Machine;
    args.artifactPath = QStringLiteral("C:/Users/x/AppData/Local/updates/Lightning.msi");

    const StrategyResult result = planWindowsMsi(args);
    QVERIFY(result.ok());
    QCOMPARE(result.plan.arguments,
             (QStringList{QStringLiteral("/i"),
                          QStringLiteral("C:\\Users\\x\\AppData\\Local\\updates\\Lightning.msi"),
                          QStringLiteral("/qb"), QStringLiteral("REINSTALLMODE=vomus"),
                          QStringLiteral("ALLUSERS=1")}));
    // Started through ShellExecuteEx "runas": one UAC prompt for an update
    // the person asked for. Never pkexec, which is the Linux meaning.
    QVERIFY(result.plan.elevateOnWindows);
    QVERIFY(!result.plan.elevates);
    // And the package msiexec will read is held open, read-shared only, and
    // re-hashed through that handle for as long as the prompt and the install
    // take -- it sits in a user-writable directory.
    QCOMPARE(result.plan.lockedArtifact,
             QStringLiteral("C:\\Users\\x\\AppData\\Local\\updates\\Lightning.msi"));
}

void UpdaterHelperArgsTest::perMachineSetupPlanPassesAllUsersAndElevates()
{
    UpdaterArguments args;
    args.mode = UpdaterMode::WindowsSetup;
    args.installScope = updater::InstallScope::Machine;
    args.artifactPath = QStringLiteral("/staging/Lightning-setup.exe");

    const StrategyResult result = planWindowsSetup(args);
    QVERIFY(result.ok());
    QCOMPARE(result.plan.arguments,
             (QStringList{QStringLiteral("/S"), QStringLiteral("/ALLUSERS")}));
    QVERIFY(result.plan.elevateOnWindows);
    QVERIFY(!result.plan.elevates);
    // The setup IS the artifact: the locked file is the one that runs elevated.
    QCOMPARE(result.plan.lockedArtifact, result.plan.program);
    QVERIFY(!result.plan.lockedArtifact.isEmpty());
    for (const QString &argument : result.plan.arguments)
        QVERIFY(!argument.startsWith(QStringLiteral("/D")));
}

void UpdaterHelperArgsTest::installScopeIsOptionalAndDefaultsToUser()
{
    // A caller that omits the option gets per-user and no elevation.
    for (const char *mode : {"windows-msi", "windows-setup"}) {
        const ArgsParseResult parsed =
            parseUpdaterArgs(baseArgs(QString::fromLatin1(mode), m_targetFile));
        QVERIFY2(parsed.ok(), mode);
        QCOMPARE(int(parsed.args.installScope), int(updater::InstallScope::User));
        QVERIFY(!planForMode(parsed.args).plan.elevateOnWindows);
    }
}

void UpdaterHelperArgsTest::installScopeParsesForTheTwoWindowsInstallers()
{
    for (const char *mode : {"windows-msi", "windows-setup"}) {
        for (const char *scope : {"user", "machine"}) {
            QStringList arguments = baseArgs(QString::fromLatin1(mode), m_targetFile);
            arguments << QStringLiteral("--install-scope") << QString::fromLatin1(scope);
            const ArgsParseResult parsed = parseUpdaterArgs(arguments);
            QVERIFY2(parsed.ok(), qPrintable(QStringLiteral("%1 %2: %3")
                                                 .arg(QLatin1String(mode), QLatin1String(scope),
                                                      parseErrorName(parsed.error))));
            const bool machine = QLatin1String(scope) == QLatin1String("machine");
            QCOMPARE(parsed.args.installScope,
                     machine ? updater::InstallScope::Machine : updater::InstallScope::User);
            QCOMPARE(updater::installScopeToString(parsed.args.installScope),
                     QString::fromLatin1(scope));
            // The parsed value decides elevation end to end, and the artifact
            // lock always travels with it.
            const InstallPlan plan = planForMode(parsed.args).plan;
            QCOMPARE(plan.elevateOnWindows, machine);
            QCOMPARE(!plan.lockedArtifact.isEmpty(), machine);
        }
    }
    // Still at most once, like every option.
    QStringList twice = baseArgs(QStringLiteral("windows-msi"), m_targetFile);
    twice << QStringLiteral("--install-scope") << QStringLiteral("user")
          << QStringLiteral("--install-scope") << QStringLiteral("machine");
    QCOMPARE(parseUpdaterArgs(twice).error, ArgsError::DuplicateOption);
}

void UpdaterHelperArgsTest::installScopeIsRefusedForEveryOtherMode_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("directoryTarget");
    QTest::newRow("portable") << "windows-portable" << true;
    QTest::newRow("appimage") << "linux-appimage" << false;
    QTest::newRow("deb") << "linux-deb" << false;
    QTest::newRow("rpm") << "linux-rpm" << false;
}

void UpdaterHelperArgsTest::installScopeIsRefusedForEveryOtherMode()
{
    // Refused, not ignored: a mistaken "machine" must never slide silently
    // into a path that cannot honour it.
    QFETCH(QString, mode);
    QFETCH(bool, directoryTarget);
    for (const char *scope : {"user", "machine"}) {
        QStringList arguments = baseArgs(mode, directoryTarget ? m_targetDir : m_targetFile);
        arguments << QStringLiteral("--install-scope") << QString::fromLatin1(scope);
        const ArgsParseResult parsed = parseUpdaterArgs(arguments);
        QCOMPARE(parsed.error, ArgsError::InvalidInstallScope);
        QCOMPARE(parsed.offendingOption, QStringLiteral("--install-scope"));
        QCOMPARE(parseErrorName(parsed.error), QStringLiteral("invalid-install-scope"));
    }
}

void UpdaterHelperArgsTest::installScopeRefusesAnythingButUserOrMachine_data()
{
    QTest::addColumn<QString>("value");
    QTest::newRow("capitalised") << "Machine";
    QTest::newRow("allusers-spelling") << "allusers";
    QTest::newRow("trailing-space") << "machine ";
    QTest::newRow("prefix") << "machines";
    QTest::newRow("numeric") << "1";
}

void UpdaterHelperArgsTest::installScopeRefusesAnythingButUserOrMachine()
{
    QFETCH(QString, value);
    QStringList arguments = baseArgs(QStringLiteral("windows-setup"), m_targetFile);
    arguments << QStringLiteral("--install-scope") << value;
    QCOMPARE(parseUpdaterArgs(arguments).error, ArgsError::InvalidInstallScope);
}

namespace {

// CommandLineToArgvW's documented rules, the convention the elevated path's
// quoting follows. msiexec and NSIS have parsers of their own; what the plans
// produce (switches, properties, double-quoted paths) reads the same in all
// three, and the native Windows test is what proves it for msiexec. Whitespace
// separates outside quotes; 2n backslashes + quote -> n backslashes and a
// quote toggle; 2n+1 backslashes + quote -> n backslashes and a literal
// quote; backslashes not followed by a quote are literal.
QStringList commandLineToArgv(const QString &line)
{
    QStringList out;
    QString current;
    bool inQuotes = false;
    bool haveToken = false;
    int i = 0;
    while (i < line.size()) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('\\')) {
            int run = 0;
            while (i < line.size() && line.at(i) == QLatin1Char('\\')) {
                ++run;
                ++i;
            }
            if (i < line.size() && line.at(i) == QLatin1Char('"')) {
                current += QString(run / 2, QLatin1Char('\\'));
                if (run % 2) {
                    current += QLatin1Char('"');
                    ++i;
                }
            } else {
                current += QString(run, QLatin1Char('\\'));
            }
            haveToken = true;
            continue;
        }
        if (c == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            haveToken = true;
            ++i;
            continue;
        }
        if (!inQuotes && (c == QLatin1Char(' ') || c == QLatin1Char('\t'))) {
            if (haveToken)
                out << current;
            current.clear();
            haveToken = false;
            ++i;
            continue;
        }
        current += c;
        haveToken = true;
        ++i;
    }
    if (haveToken)
        out << current;
    return out;
}

} // namespace

void UpdaterHelperArgsTest::windowsCommandLineRoundTripsThroughCommandLineToArgv_data()
{
    QTest::addColumn<QStringList>("arguments");
    QTest::newRow("msi-machine-plain") << QStringList{
        QStringLiteral("/i"), QStringLiteral("C:\\Users\\x\\Lightning.msi"),
        QStringLiteral("/qb"), QStringLiteral("REINSTALLMODE=vomus"),
        QStringLiteral("ALLUSERS=1")};
    // The staging directory lives under the profile, and profiles have spaces.
    QTest::newRow("path-with-spaces") << QStringList{
        QStringLiteral("/i"),
        QStringLiteral("C:\\Users\\Jane Doe\\AppData\\Local\\MatrixClient\\updates\\Lightning 1.msi"),
        QStringLiteral("/qb")};
    QTest::newRow("trailing-backslash-inside-quotes") << QStringList{
        QStringLiteral("C:\\dir with space\\"), QStringLiteral("x")};
    QTest::newRow("trailing-backslash-unquoted") << QStringList{
        QStringLiteral("C:\\plain\\"), QStringLiteral("y")};
    QTest::newRow("empty-element") << QStringList{QStringLiteral("/S"), QString(),
                                                  QStringLiteral("/ALLUSERS")};
    QTest::newRow("unicode-and-space") << QStringList{
        QStringLiteral("C:\\Users\\Ūla Ž\\setup.exe"), QStringLiteral("/S")};
    QTest::newRow("setup-machine") << QStringList{QStringLiteral("/S"),
                                                  QStringLiteral("/ALLUSERS")};
}

void UpdaterHelperArgsTest::windowsCommandLineRoundTripsThroughCommandLineToArgv()
{
    QFETCH(QStringList, arguments);
    bool ok = false;
    const QString line = updater::windowsCommandLine(arguments, &ok);
    QVERIFY(ok);
    QCOMPARE(commandLineToArgv(line), arguments);
}

void UpdaterHelperArgsTest::windowsCommandLineRefusesQuotesAndControlCharacters()
{
    bool ok = true;
    QVERIFY(updater::windowsCommandLine({QStringLiteral("a\"b")}, &ok).isEmpty());
    QVERIFY(!ok);
    ok = true;
    QVERIFY(updater::windowsCommandLine({QStringLiteral("line\nbreak")}, &ok).isEmpty());
    QVERIFY(!ok);
    // No Windows file name can hold a control character, tab included.
    ok = true;
    QVERIFY(updater::windowsCommandLine({QStringLiteral("tab\there")}, &ok).isEmpty());
    QVERIFY(!ok);
    ok = false;
    updater::windowsCommandLine({QStringLiteral("/S")}, &ok);
    QVERIFY(ok);
}

// A junction in a parent directory can be re-pointed after the helper locked
// the artifact, so the plan is rewritten to the locked handle's final path
// before launch. These two functions are the portable half of that.
void UpdaterHelperArgsTest::finalPathBecomesALaunchablePath_data()
{
    QTest::addColumn<QString>("finalPath");
    QTest::addColumn<QString>("launchable");
    QTest::newRow("local") << "\\\\?\\C:\\Users\\x\\u\\L.msi" << "C:\\Users\\x\\u\\L.msi";
    QTest::newRow("unc") << "\\\\?\\UNC\\srv\\share\\u\\L.msi" << "\\\\srv\\share\\u\\L.msi";
    QTest::newRow("unc-prefix-any-case") << "\\\\?\\unc\\srv\\share\\L.msi"
                                         << "\\\\srv\\share\\L.msi";
    QTest::newRow("already-plain") << "D:\\u\\L.msi" << "D:\\u\\L.msi";
    QTest::newRow("spaces-and-parentheses")
        << "\\\\?\\C:\\Users\\Jane Doe\\setup (1).exe" << "C:\\Users\\Jane Doe\\setup (1).exe";
    // Refused: nothing msiexec or the loader could be handed safely.
    QTest::newRow("volume-guid") << "\\\\?\\Volume{0a1b2c3d-0000-0000-0000-000000000000}\\L.msi" << "";
    QTest::newRow("device") << "\\\\.\\C:\\L.msi" << "";
    QTest::newRow("unc-without-share") << "\\\\?\\UNC\\srv" << "";
    QTest::newRow("unc-empty-server") << "\\\\?\\UNC\\\\share\\L.msi" << "";
    QTest::newRow("empty") << "" << "";
    QTest::newRow("relative") << "u\\L.msi" << "";
    QTest::newRow("drive-relative") << "C:L.msi" << "";
    QTest::newRow("forward-slashes") << "\\\\?\\C:\\u/L.msi" << "";
}

void UpdaterHelperArgsTest::finalPathBecomesALaunchablePath()
{
    QFETCH(QString, finalPath);
    QFETCH(QString, launchable);
    QCOMPARE(launchablePathFromFinal(finalPath), launchable);
}

void UpdaterHelperArgsTest::elevatedPlansAreRetargetedToTheLockedFile()
{
    const QString real = QStringLiteral("D:\\A\\Lightning.msi");

    UpdaterArguments msiArgs;
    msiArgs.mode = UpdaterMode::WindowsMsi;
    msiArgs.installScope = updater::InstallScope::Machine;
    msiArgs.artifactPath = QStringLiteral("C:/Users/x/cache/updates/Lightning.msi");
    InstallPlan msi = planWindowsMsi(msiArgs).plan;
    QVERIFY(retargetPlanToLockedFile(msi, real));
    // msiexec reads the /i argument: it is the locked object now, and nothing
    // in the plan still names the path the junction could re-point.
    QCOMPARE(msi.arguments.at(1), real);
    QCOMPARE(msi.lockedArtifact, real);
    QVERIFY(!msi.arguments.join(QLatin1Char(' ')).contains(QStringLiteral("updates")));
    QVERIFY(msi.program.endsWith(QStringLiteral("msiexec.exe")));

    UpdaterArguments setupArgs;
    setupArgs.mode = UpdaterMode::WindowsSetup;
    setupArgs.installScope = updater::InstallScope::Machine;
    setupArgs.artifactPath = QStringLiteral("/cache/updates/Lightning-setup.exe");
    InstallPlan setup = planWindowsSetup(setupArgs).plan;
    const QString realSetup = QStringLiteral("D:\\A\\Lightning-setup.exe");
    QVERIFY(retargetPlanToLockedFile(setup, realSetup));
    // The loader maps lpFile: the program IS the locked file, and it starts in
    // the locked file's own directory.
    QCOMPARE(setup.program, realSetup);
    QCOMPARE(setup.workingDirectory, QStringLiteral("D:\\A"));
    QCOMPARE(setup.arguments, (QStringList{QStringLiteral("/S"), QStringLiteral("/ALLUSERS")}));

    // A file in a drive's root keeps a directory for a working directory.
    InstallPlan root = planWindowsSetup(setupArgs).plan;
    QVERIFY(retargetPlanToLockedFile(root, QStringLiteral("E:\\setup.exe")));
    QCOMPARE(root.workingDirectory, QStringLiteral("E:\\"));
}

void UpdaterHelperArgsTest::retargetRefusesAPlanThatDoesNotNameTheLockedFile()
{
    // A per-user plan locks nothing, so there is nothing to retarget.
    UpdaterArguments userArgs;
    userArgs.mode = UpdaterMode::WindowsMsi;
    userArgs.artifactPath = QStringLiteral("C:/u/L.msi");
    InstallPlan user = planWindowsMsi(userArgs).plan;
    const InstallPlan before = user;
    QVERIFY(!retargetPlanToLockedFile(user, QStringLiteral("D:\\A\\L.msi")));
    QCOMPARE(user.arguments, before.arguments);

    // An unresolvable final path (empty) is refused, never launched as "".
    UpdaterArguments machineArgs = userArgs;
    machineArgs.installScope = updater::InstallScope::Machine;
    InstallPlan machine = planWindowsMsi(machineArgs).plan;
    const InstallPlan untouched = machine;
    QVERIFY(!retargetPlanToLockedFile(machine, QString()));
    QCOMPARE(machine.arguments, untouched.arguments);
    QCOMPARE(machine.lockedArtifact, untouched.lockedArtifact);

    // A plan whose locked name appears nowhere is refused and left alone.
    machine.lockedArtifact = QStringLiteral("C:\\elsewhere\\L.msi");
    QVERIFY(!retargetPlanToLockedFile(machine, QStringLiteral("D:\\A\\L.msi")));
    QCOMPARE(machine.arguments, untouched.arguments);
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

// The relaunch's environment, through the real helper binary: a recording
// script reads back what the relaunched process was handed. The input is a
// TMPDIR that no longer exists and an APPDIR from appimage-run.
void UpdaterHelperArgsTest::theRelaunchedApplicationDoesNotInheritADeadTemporaryDirectory()
{
#ifdef Q_OS_WIN
    QSKIP("the end-to-end helper run uses a POSIX shell script as the relaunch target");
#else
    const QString helper = QStringLiteral(LIGHTNING_UPDATER_HELPER_PATH);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir root(dir.path());
    const QString record = root.absoluteFilePath(QStringLiteral("relaunch-environment.txt"));
    const QByteArray script =
        QByteArrayLiteral("#!/bin/sh\nprintf 'tmpdir=[%s] extract=[%s]' "
                          "\"${TMPDIR-unset}\" \"${APPIMAGE_EXTRACT_AND_RUN-unset}\" > '")
        + record.toLocal8Bit() + QByteArrayLiteral("'\n");
    const QString artifact = writeFile(root.absoluteFilePath(QStringLiteral("new.AppImage")),
                                       script);
    const QString target = writeFile(root.absoluteFilePath(QStringLiteral("Lightning.AppImage")),
                                     QByteArray("installed bytes"));
    const QString status = root.absoluteFilePath(QStringLiteral("status.json"));
    const qint64 pid = exitedPid();
    QVERIFY(pid > 1);

    // A temporary directory that existed at startup and is gone now, as when
    // the owning shell exits along with Lightning.
    const QString deadTemp = root.absoluteFilePath(QStringLiteral("shell-private-tmp"));
    QVERIFY(QDir().mkpath(deadTemp));
    QVERIFY(QDir(deadTemp).removeRecursively());
    QVERIFY(!QFileInfo::exists(deadTemp));

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TMPDIR"), deadTemp);
    environment.insert(QStringLiteral("APPDIR"),
                       QStringLiteral("/home/someone/.cache/appimage-run/e77b1a96fe0a"));
    environment.remove(QStringLiteral("APPIMAGE_EXTRACT_AND_RUN"));

    QProcess run;
    run.setProcessEnvironment(environment);
    run.start(helper, {QStringLiteral("--mode"), QStringLiteral("linux-appimage"),
                       QStringLiteral("--artifact"), artifact,
                       QStringLiteral("--pid"), QString::number(pid),
                       QStringLiteral("--target"), target,
                       QStringLiteral("--status"), status,
                       QStringLiteral("--sha256"), sha256HexOfFile(artifact),
                       QStringLiteral("--relaunch"), target});
    QVERIFY(run.waitForStarted(5000));
    QVERIFY(run.waitForFinished(30000));
    QCOMPARE(run.exitCode(), 0);

    // The relaunch is detached, so it lands a moment after the helper exits.
    for (int waited = 0; waited < 10000 && !QFileInfo::exists(record); waited += 50)
        QTest::qWait(50);
    QVERIFY2(QFileInfo::exists(record), "the relaunched application never ran");
    QFile file(record);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString seen = QString::fromLocal8Bit(file.readAll());

    QVERIFY2(!seen.contains(deadTemp),
             qPrintable(QStringLiteral("the relaunched process inherited a TMPDIR that no "
                                       "longer exists, which is 'create mount dir error': %1")
                            .arg(seen)));
    QVERIFY2(seen.contains(QStringLiteral("extract=[1]")),
             qPrintable(QStringLiteral("an AppImage that was unpacked by appimage-run was "
                                       "relaunched expecting to mount itself, which cannot "
                                       "work inside that sandbox: %1")
                            .arg(seen)));
#endif
}

QTEST_GUILESS_MAIN(UpdaterHelperArgsTest)
#include "UpdaterHelperArgsTest.moc"
