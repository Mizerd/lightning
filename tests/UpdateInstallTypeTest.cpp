
#include "update/InstallType.h"

#include "updater/UpdaterArgs.h"

#include <QHash>
#include <QSet>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using lightning::update::canInstallAutomatically;
using lightning::update::defaultInstallEnvironment;
using lightning::update::detectInstall;
using lightning::update::fileLooksLikeAppImage;
using lightning::update::InstallDetection;
using lightning::update::InstallEnvironment;
using lightning::update::InstallScope;
using lightning::update::installScopeId;
using lightning::update::InstallType;
using lightning::update::installTypeFromId;
using lightning::update::installTypeId;
using lightning::update::installTypeLabel;
using lightning::update::isPackageManaged;
using lightning::update::packageManagerName;
using lightning::update::repositoryRpmManager;
using lightning::update::sameInstallDirectory;

namespace {

const QList<InstallType> kAllTypes{
    InstallType::WindowsMsi,    InstallType::WindowsSetup,  InstallType::WindowsPortable,
    InstallType::LinuxAppImage, InstallType::LinuxDeb,      InstallType::LinuxRpm,
    InstallType::LinuxFlatpak,  InstallType::LinuxSnap,     InstallType::LinuxRpmRepo,
    InstallType::MacosDmg,      InstallType::Development,   InstallType::Unknown,
};

InstallEnvironment makeEnvironment(const QHash<QString, QString> &variables,
                                   const QSet<QString> &existingPaths,
                                   const QString &compileTimeId)
{
    InstallEnvironment environment;
    environment.readEnv = [variables](const char *name) -> QString {
        return variables.value(QString::fromLatin1(name));
    };
    environment.pathExists = [existingPaths](const QString &path) {
        return existingPaths.contains(path);
    };
    environment.looksLikeAppImage = [existingPaths](const QString &path) {
        return existingPaths.contains(path);
    };
    environment.compileTimeId = compileTimeId;
    return environment;
}

InstallEnvironment makeMarkerEnvironment(const QString &markerContents,
                                         const QString &compileTimeId,
                                         bool windowsPlatform,
                                         bool portableMarkerPresent = true)
{
    InstallEnvironment environment = makeEnvironment({}, {}, compileTimeId);
    environment.readInstallMarker = [markerContents]() { return markerContents; };
    environment.windowsPlatform = windowsPlatform;
    environment.portableMarkerPresent = [portableMarkerPresent] {
        return portableMarkerPresent;
    };
    return environment;
}

}

class UpdateInstallTypeTest : public QObject
{
    Q_OBJECT

private slots:
    void portableIsRefusedWithoutItsOwnMarker();
    void idsRoundTripForEveryType();
    void unknownIdIsReportedNotGuessed();
    void labelsAreDistinctAndNonEmpty();
    void automaticInstallPolicyIsExplicit_data();
    void automaticInstallPolicyIsExplicit();
    void packageManagedIsFlatpakSnapAndTheDnfRepositoryOnly();
    void aRepositoryRpmOnOpenSuseIsZyppers();
    void aRepositoryRpmIsUpdatedByDnfAndADownloadedOneIsNot();
    void compileTimeValueIsUsedForConcretePackages_data();
    void compileTimeValueIsUsedForConcretePackages();
    void unsetCompileTimeValueIsDevelopment();
    void unrecognisedCompileTimeValueIsUnknown();
    void flatpakEnvironmentIsDetected();
    void flatpakInfoFileIsDetected();
    void flatpakContainerVariableIsDetected();
    void hostPathCheckSeesAnUnlinkedBindMountedFile();
    void snapNeedsBothVariables();
    void appImageNeedsAnExistingPath();
    void runtimeEvidenceOverridesCompileTimeValue();
    void diagnosticOverrideNeverAuthorisesInstallation();
    void diagnosticOverrideDoesNotBeatConcreteBuildMetadata();
    void developmentChecksAreOptIn();
    void installMarkerSelectsAmongTheWindowsPackages();
    void installMarkerIsIgnoredOffWindows_data();
    void installMarkerIsIgnoredOffWindows();
    void installMarkerNeverNamesANonWindowsType();
    void scopeMarkerMakesAWindowsInstallerCopyPerMachine_data();
    void scopeMarkerMakesAWindowsInstallerCopyPerMachine();
    void scopeIdsMatchTheHelperOption();
    void registeredDirectoryComparisonIgnoresOnlyTheInsignificant_data();
    void registeredDirectoryComparisonIgnoresOnlyTheInsignificant();
    void automaticInstallAgreesWithTheUpdaterHelper();
    void appImageClaimNeedsTheAppImageMagic();
    void appImageMagicIsReadFromTheFile();
};

void UpdateInstallTypeTest::portableIsRefusedWithoutItsOwnMarker()
{
    const InstallDetection genuine = detectInstall(makeMarkerEnvironment(
        QString(), QStringLiteral("windows-portable"), true, true));
    QCOMPARE(genuine.type, InstallType::WindowsPortable);
    QVERIFY(genuine.automaticInstallAllowed);

    const InstallDetection installed = detectInstall(makeMarkerEnvironment(
        QString(), QStringLiteral("windows-portable"), true, false));
    QVERIFY2(installed.type != InstallType::WindowsPortable,
             "an installation with no portable marker was treated as portable");
    QCOMPARE(installed.type, InstallType::Unknown);
    QVERIFY2(!installed.automaticInstallAllowed,
             "an unproven install type must not apply an update");

    const InstallDetection declared = detectInstall(makeMarkerEnvironment(
        QStringLiteral("windows-setup"), QStringLiteral("windows-portable"),
        true, false));
    QCOMPARE(declared.type, InstallType::WindowsSetup);

    const InstallDetection linux = detectInstall(makeMarkerEnvironment(
        QString(), QStringLiteral("linux-appimage"), false, false));
    QCOMPARE(linux.type, InstallType::LinuxAppImage);
}

void UpdateInstallTypeTest::idsRoundTripForEveryType()
{
    const QStringList expected{
        QStringLiteral("windows-msi"),    QStringLiteral("windows-setup"),
        QStringLiteral("windows-portable"), QStringLiteral("linux-appimage"),
        QStringLiteral("linux-deb"),      QStringLiteral("linux-rpm"),
        QStringLiteral("linux-flatpak"),  QStringLiteral("linux-snap"),
        QStringLiteral("linux-rpm-repo"), QStringLiteral("macos-dmg"),
        QStringLiteral("development"),    QStringLiteral("unknown"),
    };
    QCOMPARE(kAllTypes.size(), expected.size());
    for (int i = 0; i < kAllTypes.size(); ++i) {
        const QString id = installTypeId(kAllTypes.at(i));
        QCOMPARE(id, expected.at(i));
        const std::optional<InstallType> parsed = installTypeFromId(id);
        QVERIFY(parsed.has_value());
        QCOMPARE(*parsed, kAllTypes.at(i));
    }
}

void UpdateInstallTypeTest::unknownIdIsReportedNotGuessed()
{
    QVERIFY(!installTypeFromId(QStringLiteral("linux-fooix")).has_value());
    QVERIFY(!installTypeFromId(QStringLiteral("")).has_value());
    QVERIFY(!installTypeFromId(QStringLiteral("LINUX-DEB")).has_value());
    QVERIFY(!installTypeFromId(QStringLiteral(" linux-deb")).has_value());
}

void UpdateInstallTypeTest::labelsAreDistinctAndNonEmpty()
{
    QSet<QString> labels;
    for (const InstallType type : kAllTypes) {
        const QString label = installTypeLabel(type);
        QVERIFY(!label.isEmpty());
        labels.insert(label);
    }
    QCOMPARE(labels.size(), kAllTypes.size());
}

void UpdateInstallTypeTest::automaticInstallPolicyIsExplicit_data()
{
    QTest::addColumn<int>("type");
    QTest::addColumn<bool>("automatic");

    QTest::newRow("windows-msi") << int(InstallType::WindowsMsi) << true;
    QTest::newRow("windows-setup") << int(InstallType::WindowsSetup) << true;
    QTest::newRow("windows-portable") << int(InstallType::WindowsPortable) << true;
    QTest::newRow("linux-appimage") << int(InstallType::LinuxAppImage) << true;
    QTest::newRow("linux-deb") << int(InstallType::LinuxDeb) << true;
    QTest::newRow("linux-rpm") << int(InstallType::LinuxRpm) << true;
    QTest::newRow("macos-dmg") << int(InstallType::MacosDmg) << false;
    QTest::newRow("linux-flatpak") << int(InstallType::LinuxFlatpak) << false;
    QTest::newRow("linux-snap") << int(InstallType::LinuxSnap) << false;
    QTest::newRow("linux-rpm-repo") << int(InstallType::LinuxRpmRepo) << false;
    QTest::newRow("development") << int(InstallType::Development) << false;
    QTest::newRow("unknown") << int(InstallType::Unknown) << false;
}

void UpdateInstallTypeTest::automaticInstallPolicyIsExplicit()
{
    QFETCH(int, type);
    QFETCH(bool, automatic);
    QCOMPARE(canInstallAutomatically(static_cast<InstallType>(type)), automatic);
}

void UpdateInstallTypeTest::packageManagedIsFlatpakSnapAndTheDnfRepositoryOnly()
{
    int managed = 0;
    for (const InstallType type : kAllTypes) {
        const bool expected = type == InstallType::LinuxFlatpak
            || type == InstallType::LinuxSnap || type == InstallType::LinuxRpmRepo;
        QCOMPARE(isPackageManaged(type), expected);
        QCOMPARE(!packageManagerName(type).isEmpty(), expected);
        managed += expected ? 1 : 0;
    }
    QCOMPARE(managed, 3);
    QCOMPARE(packageManagerName(InstallType::LinuxFlatpak), QStringLiteral("Flatpak"));
    QCOMPARE(packageManagerName(InstallType::LinuxSnap), QStringLiteral("Snap"));
    QCOMPARE(packageManagerName(InstallType::LinuxRpmRepo), repositoryRpmManager());
}

void UpdateInstallTypeTest::aRepositoryRpmOnOpenSuseIsZyppers()
{
    const auto only = [](const QStringList &present) {
        return [present](const QString &path) { return present.contains(path); };
    };
    const QString zypper = QStringLiteral("/usr/bin/zypper");
    const QString dnf = QStringLiteral("/usr/bin/dnf");
    const QString dnf5 = QStringLiteral("/usr/bin/dnf5");
    QCOMPARE(repositoryRpmManager(only({zypper})), QStringLiteral("zypper"));
    QCOMPARE(repositoryRpmManager(only({dnf})), QStringLiteral("dnf"));
    QCOMPARE(repositoryRpmManager(only({dnf5})), QStringLiteral("dnf"));
    QCOMPARE(repositoryRpmManager(only({zypper, dnf})), QStringLiteral("dnf"));
    QCOMPARE(repositoryRpmManager(only({zypper, dnf5})), QStringLiteral("dnf"));
    QCOMPARE(repositoryRpmManager(only({})), QStringLiteral("dnf"));
}

// The COPR spec compiles in linux-rpm-repo; the GitLab .rpm compiles in
// linux-rpm. Nothing at runtime tells them apart, so the compile-time value
// alone decides who updates the installation.
void UpdateInstallTypeTest::aRepositoryRpmIsUpdatedByDnfAndADownloadedOneIsNot()
{
    const InstallDetection repository =
        detectInstall(makeEnvironment({}, {}, QStringLiteral("linux-rpm-repo")));
    QCOMPARE(repository.type, InstallType::LinuxRpmRepo);
    QVERIFY(!repository.diagnosticOverride);
    QVERIFY2(!repository.automaticInstallAllowed,
             "a COPR install must never download and install a GitLab .rpm");
    QVERIFY(isPackageManaged(repository.type));
    QCOMPARE(installTypeId(repository.type), QStringLiteral("linux-rpm-repo"));

    const InstallDetection downloaded =
        detectInstall(makeEnvironment({}, {}, QStringLiteral("linux-rpm")));
    QCOMPARE(downloaded.type, InstallType::LinuxRpm);
    QVERIFY2(downloaded.automaticInstallAllowed,
             "the GitLab .rpm keeps its automatic install");
    QVERIFY(!isPackageManaged(downloaded.type));

    const InstallDetection overridden = detectInstall(makeEnvironment(
        { { QStringLiteral("LIGHTNING_INSTALL_TYPE_OVERRIDE"), QStringLiteral("linux-rpm-repo") } },
        {}, QStringLiteral("linux-rpm")));
    QCOMPARE(overridden.type, InstallType::LinuxRpm);
}

void UpdateInstallTypeTest::compileTimeValueIsUsedForConcretePackages_data()
{
    QTest::addColumn<QString>("compileTimeId");
    QTest::addColumn<int>("expected");

    QTest::newRow("windows-msi") << "windows-msi" << int(InstallType::WindowsMsi);
    QTest::newRow("windows-setup") << "windows-setup" << int(InstallType::WindowsSetup);
    QTest::newRow("windows-portable") << "windows-portable" << int(InstallType::WindowsPortable);
    QTest::newRow("linux-appimage") << "linux-appimage" << int(InstallType::LinuxAppImage);
    QTest::newRow("linux-deb") << "linux-deb" << int(InstallType::LinuxDeb);
    QTest::newRow("linux-rpm") << "linux-rpm" << int(InstallType::LinuxRpm);
    QTest::newRow("linux-flatpak") << "linux-flatpak" << int(InstallType::LinuxFlatpak);
    QTest::newRow("linux-snap") << "linux-snap" << int(InstallType::LinuxSnap);
    QTest::newRow("linux-rpm-repo") << "linux-rpm-repo" << int(InstallType::LinuxRpmRepo);
    QTest::newRow("macos-dmg") << "macos-dmg" << int(InstallType::MacosDmg);
}

void UpdateInstallTypeTest::compileTimeValueIsUsedForConcretePackages()
{
    QFETCH(QString, compileTimeId);
    QFETCH(int, expected);

    const InstallDetection detection = detectInstall(makeEnvironment({}, {}, compileTimeId));
    QCOMPARE(detection.type, static_cast<InstallType>(expected));
    QVERIFY(!detection.diagnosticOverride);
    QCOMPARE(detection.automaticInstallAllowed,
             canInstallAutomatically(static_cast<InstallType>(expected)));
}

void UpdateInstallTypeTest::unsetCompileTimeValueIsDevelopment()
{
    const InstallDetection detection = detectInstall(makeEnvironment({}, {}, QString()));
    QCOMPARE(detection.type, InstallType::Development);
    QVERIFY(!detection.automaticInstallAllowed);
}

void UpdateInstallTypeTest::unrecognisedCompileTimeValueIsUnknown()
{
    const InstallDetection detection =
        detectInstall(makeEnvironment({}, {}, QStringLiteral("linux-fooix")));
    QCOMPARE(detection.type, InstallType::Unknown);
    QVERIFY(!detection.automaticInstallAllowed);
}

void UpdateInstallTypeTest::flatpakEnvironmentIsDetected()
{
    const InstallDetection detection = detectInstall(
        makeEnvironment({ { QStringLiteral("FLATPAK_ID"), QStringLiteral("net.example.App") } },
                        {}, QStringLiteral("development")));
    QCOMPARE(detection.type, InstallType::LinuxFlatpak);
    QVERIFY(!detection.automaticInstallAllowed);
    QVERIFY(isPackageManaged(detection.type));
}

void UpdateInstallTypeTest::flatpakInfoFileIsDetected()
{
    const InstallDetection detection =
        detectInstall(makeEnvironment({}, { QStringLiteral("/.flatpak-info") },
                                      QStringLiteral("linux-deb")));
    QCOMPARE(detection.type, InstallType::LinuxFlatpak);
}

void UpdateInstallTypeTest::flatpakContainerVariableIsDetected()
{
    const InstallDetection detection = detectInstall(makeEnvironment(
        { { QStringLiteral("container"), QStringLiteral("flatpak") } }, {},
        QStringLiteral("linux-deb")));
    QCOMPARE(detection.type, InstallType::LinuxFlatpak);
}

void UpdateInstallTypeTest::hostPathCheckSeesAnUnlinkedBindMountedFile()
{
#if defined(Q_OS_LINUX)
    // /.flatpak-info is bind-mounted from a file bwrap already unlinked
    // (st_nlink == 0), and Qt 6 reports such a file as absent. An open fd to
    // an unlinked file, reached through /proc/self/fd, is the same inode
    // shape. The host check must still see it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString name = dir.filePath(QStringLiteral("flatpak-info"));
    {
        QFile out(name);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write("[Application]\nname=net.example.App\n");
    }
    QFile held(name);
    QVERIFY(held.open(QIODevice::ReadOnly));
    QVERIFY(QFile::remove(name));
    const QString unlinked =
        QStringLiteral("/proc/self/fd/%1").arg(held.handle());
    qInfo() << "QFileInfo::exists() on an unlinked file:"
            << QFileInfo::exists(unlinked);
    QVERIFY(defaultInstallEnvironment().pathExists(unlinked));
#else
    QSKIP("Linux only");
#endif
}

void UpdateInstallTypeTest::snapNeedsBothVariables()
{
    const InstallDetection partial = detectInstall(makeEnvironment(
        { { QStringLiteral("SNAP"), QStringLiteral("/snap/x/1") } }, {}, QString()));
    QCOMPARE(partial.type, InstallType::Development);

    const InstallDetection full = detectInstall(
        makeEnvironment({ { QStringLiteral("SNAP"), QStringLiteral("/snap/x/1") },
                          { QStringLiteral("SNAP_NAME"), QStringLiteral("lightning") } },
                        {}, QString()));
    QCOMPARE(full.type, InstallType::LinuxSnap);
    QVERIFY(!full.automaticInstallAllowed);
    QVERIFY(isPackageManaged(full.type));
}

void UpdateInstallTypeTest::appImageNeedsAnExistingPath()
{
    const QString path = QStringLiteral("/home/user/Lightning.AppImage");

    const InstallDetection missing = detectInstall(
        makeEnvironment({ { QStringLiteral("APPIMAGE"), path } }, {}, QString()));
    QCOMPARE(missing.type, InstallType::Development);

    const InstallDetection present = detectInstall(
        makeEnvironment({ { QStringLiteral("APPIMAGE"), path } }, { path }, QString()));
    QCOMPARE(present.type, InstallType::LinuxAppImage);
    QVERIFY(present.automaticInstallAllowed);
}

void UpdateInstallTypeTest::runtimeEvidenceOverridesCompileTimeValue()
{
    const InstallDetection detection = detectInstall(
        makeEnvironment({ { QStringLiteral("FLATPAK_ID"), QStringLiteral("net.example.App") } },
                        {}, QStringLiteral("linux-deb")));
    QCOMPARE(detection.type, InstallType::LinuxFlatpak);
    QVERIFY(!detection.automaticInstallAllowed);
}

void UpdateInstallTypeTest::diagnosticOverrideNeverAuthorisesInstallation()
{
    const InstallDetection detection = detectInstall(makeEnvironment(
        { { QStringLiteral("LIGHTNING_INSTALL_TYPE_OVERRIDE"), QStringLiteral("linux-deb") } },
        {}, QStringLiteral("development")));
    QCOMPARE(detection.type, InstallType::LinuxDeb);
    QVERIFY(detection.diagnosticOverride);
    QVERIFY(canInstallAutomatically(detection.type));
    QVERIFY(!detection.automaticInstallAllowed);
}

void UpdateInstallTypeTest::diagnosticOverrideDoesNotBeatConcreteBuildMetadata()
{
    const InstallDetection detection = detectInstall(makeEnvironment(
        { { QStringLiteral("LIGHTNING_INSTALL_TYPE_OVERRIDE"), QStringLiteral("linux-rpm") } },
        {}, QStringLiteral("linux-deb")));
    QCOMPARE(detection.type, InstallType::LinuxDeb);
    QVERIFY(!detection.diagnosticOverride);

    const InstallDetection nonsense = detectInstall(makeEnvironment(
        { { QStringLiteral("LIGHTNING_INSTALL_TYPE_OVERRIDE"), QStringLiteral("nonsense") } },
        {}, QString()));
    QCOMPARE(nonsense.type, InstallType::Development);
    QVERIFY(!nonsense.diagnosticOverride);
}

void UpdateInstallTypeTest::developmentChecksAreOptIn()
{
    const InstallDetection off = detectInstall(makeEnvironment({}, {}, QString()));
    QVERIFY(!off.developmentCheckAllowed);

    const InstallDetection on = detectInstall(makeEnvironment(
        { { QStringLiteral("LIGHTNING_ALLOW_DEV_UPDATE_CHECK"), QStringLiteral("1") } }, {},
        QString()));
    QVERIFY(on.developmentCheckAllowed);
    QVERIFY(!on.automaticInstallAllowed);

    const InstallDetection wrongValue = detectInstall(makeEnvironment(
        { { QStringLiteral("LIGHTNING_ALLOW_DEV_UPDATE_CHECK"), QStringLiteral("yes") } }, {},
        QString()));
    QVERIFY(!wrongValue.developmentCheckAllowed);
}

void UpdateInstallTypeTest::installMarkerSelectsAmongTheWindowsPackages()
{
    const InstallDetection portable = detectInstall(makeMarkerEnvironment(
        QStringLiteral("windows-portable"), QStringLiteral("windows-msi"),
true));
    QCOMPARE(portable.type, InstallType::WindowsPortable);
    QVERIFY(!portable.diagnosticOverride);
    QVERIFY(portable.automaticInstallAllowed);

    const InstallDetection setup = detectInstall(makeMarkerEnvironment(
        QStringLiteral("windows-setup"), QStringLiteral("windows-msi"), true));
    QCOMPARE(setup.type, InstallType::WindowsSetup);

    InstallEnvironment flatpak = makeMarkerEnvironment(QStringLiteral("windows-msi"),
                                                       QStringLiteral("windows-msi"), true);
    flatpak.readEnv = [](const char *name) -> QString {
        return QString::fromLatin1(name) == QLatin1String("FLATPAK_ID")
            ? QStringLiteral("net.example.App")
            : QString();
    };
    QCOMPARE(detectInstall(flatpak).type, InstallType::LinuxFlatpak);
}

void UpdateInstallTypeTest::installMarkerIsIgnoredOffWindows_data()
{
    QTest::addColumn<QString>("marker");
    QTest::addColumn<QString>("compileTimeId");
    QTest::addColumn<int>("expected");

    QTest::newRow("deb-marker-beside-appimage")
        << "linux-deb" << "linux-appimage" << int(InstallType::LinuxAppImage);
    QTest::newRow("rpm-marker-beside-deb")
        << "linux-rpm" << "linux-deb" << int(InstallType::LinuxDeb);
    QTest::newRow("appimage-marker-beside-rpm")
        << "linux-appimage" << "linux-rpm" << int(InstallType::LinuxRpm);
    QTest::newRow("windows-marker-off-windows")
        << "windows-msi" << "linux-deb" << int(InstallType::LinuxDeb);
    QTest::newRow("marker-cannot-invent-a-package-type")
        << "linux-deb" << "" << int(InstallType::Development);
}

void UpdateInstallTypeTest::installMarkerIsIgnoredOffWindows()
{
    QFETCH(QString, marker);
    QFETCH(QString, compileTimeId);
    QFETCH(int, expected);

    const InstallDetection detection =
        detectInstall(makeMarkerEnvironment(marker, compileTimeId,
false));
    QCOMPARE(detection.type, static_cast<InstallType>(expected));
}

void UpdateInstallTypeTest::installMarkerNeverNamesANonWindowsType()
{
    const InstallDetection detection = detectInstall(makeMarkerEnvironment(
        QStringLiteral("linux-deb"), QStringLiteral("windows-msi"),
true));
    QCOMPARE(detection.type, InstallType::WindowsMsi);

    const InstallDetection nonsense = detectInstall(makeMarkerEnvironment(
        QStringLiteral("not-a-type"), QStringLiteral("windows-setup"), true));
    QCOMPARE(nonsense.type, InstallType::WindowsSetup);

    const InstallDetection empty = detectInstall(
        makeMarkerEnvironment(QString(), QStringLiteral("windows-setup"), true));
    QCOMPARE(empty.type, InstallType::WindowsSetup);
}

void UpdateInstallTypeTest::scopeMarkerMakesAWindowsInstallerCopyPerMachine_data()
{
    QTest::addColumn<QString>("typeMarker");
    QTest::addColumn<QString>("scopeMarker");
    QTest::addColumn<QString>("registered");
    QTest::addColumn<bool>("windowsPlatform");
    QTest::addColumn<bool>("portableMarker");
    QTest::addColumn<int>("expectedType");
    QTest::addColumn<int>("expectedScope");

    const int user = int(InstallScope::User);
    const int machine = int(InstallScope::Machine);
    const int msi = int(InstallType::WindowsMsi);
    const int setup = int(InstallType::WindowsSetup);
    const int portable = int(InstallType::WindowsPortable);
    const QString here = QStringLiteral("C:\\Program Files\\Lightning");
    const QString msiHere = QStringLiteral("C:\\Program Files\\Lightning\\");
    QTest::newRow("msi-machine")
        << "windows-msi" << "machine" << msiHere << true << false << msi << machine;
    QTest::newRow("setup-machine")
        << "windows-setup" << "machine" << here << true << false << setup << machine;
    QTest::newRow("setup-machine-crlf-trimmed")
        << "windows-setup" << "machine\r\n" << here << true << false << setup << machine;
    QTest::newRow("machine-marker-without-hklm-registration-is-user")
        << "windows-setup" << "machine" << QString() << true << false << setup << user;
    QTest::newRow("machine-marker-registered-elsewhere-is-user")
        << "windows-msi" << "machine" << QStringLiteral("D:\\Apps\\Lightning") << true
        << false << msi << user;
    QTest::newRow("msi-user") << "windows-msi" << "user" << here << true << false << msi << user;
    QTest::newRow("setup-no-marker-is-an-old-per-user-install")
        << "windows-setup" << QString() << here << true << false << setup << user;
    QTest::newRow("setup-garbage-is-user")
        << "windows-setup" << "MACHINE" << here << true << false << setup << user;
    QTest::newRow("setup-prefix-is-not-machine")
        << "windows-setup" << "machines" << here << true << false << setup << user;
    QTest::newRow("portable-ignores-it")
        << "windows-portable" << "machine" << here << true << true << portable << user;
    QTest::newRow("off-windows-setup-ignores-it")
        << "windows-setup" << "machine" << here << false << false << setup << user;
    QTest::newRow("off-windows-msi-ignores-it")
        << "windows-msi" << "machine" << msiHere << false << false << msi << user;
}

void UpdateInstallTypeTest::scopeMarkerMakesAWindowsInstallerCopyPerMachine()
{
    QFETCH(QString, typeMarker);
    QFETCH(QString, scopeMarker);
    QFETCH(QString, registered);
    QFETCH(bool, windowsPlatform);
    QFETCH(bool, portableMarker);
    QFETCH(int, expectedType);
    QFETCH(int, expectedScope);

    InstallEnvironment environment = makeMarkerEnvironment(
        typeMarker, typeMarker, windowsPlatform, portableMarker);
    int reads = 0;
    environment.readInstallScopeMarker = [scopeMarker, &reads]() {
        ++reads;
        return scopeMarker;
    };
    environment.applicationDir = QStringLiteral("C:/Program Files/Lightning");
    environment.readMachineInstallDirs = [registered]() {
        return registered.isEmpty() ? QStringList() : QStringList{registered};
    };
    const InstallDetection detection = detectInstall(environment);
    QCOMPARE(int(detection.type), expectedType);
    QCOMPARE(int(detection.scope), expectedScope);
    QVERIFY(detection.automaticInstallAllowed);
    if (!windowsPlatform) {
        QCOMPARE(reads, 0);
    } else if (typeMarker != QLatin1String("windows-portable")) {
        QVERIFY(reads > 0);
    }
}

void UpdateInstallTypeTest::registeredDirectoryComparisonIgnoresOnlyTheInsignificant_data()
{
    QTest::addColumn<QString>("registered");
    QTest::addColumn<QString>("applicationDir");
    QTest::addColumn<bool>("same");
    const QString app = QStringLiteral("C:/Program Files/Lightning");
    QTest::newRow("nsis-form") << "C:\\Program Files\\Lightning" << app << true;
    QTest::newRow("msi-trailing-backslash") << "C:\\Program Files\\Lightning\\" << app << true;
    QTest::newRow("case") << "c:\\PROGRAM FILES\\lightning" << app << true;
    QTest::newRow("empty-never-matches") << "" << app << false;
    QTest::newRow("whitespace-never-matches") << "  " << app << false;
    QTest::newRow("prefix-is-not-the-same") << "C:\\Program Files\\Light" << app << false;
    QTest::newRow("longer-is-not-the-same") << "C:\\Program Files\\Lightning2" << app << false;
    QTest::newRow("per-user-dir")
        << "C:\\Users\\x\\AppData\\Local\\Programs\\Lightning" << app << false;
    QTest::newRow("no-application-dir") << "C:\\Program Files\\Lightning" << "" << false;
}

void UpdateInstallTypeTest::registeredDirectoryComparisonIgnoresOnlyTheInsignificant()
{
    QFETCH(QString, registered);
    QFETCH(QString, applicationDir);
    QFETCH(bool, same);
    QCOMPARE(sameInstallDirectory(registered, applicationDir), same);
}

void UpdateInstallTypeTest::scopeIdsMatchTheHelperOption()
{
    QCOMPARE(installScopeId(InstallScope::User),
             updater::installScopeToString(updater::InstallScope::User));
    QCOMPARE(installScopeId(InstallScope::Machine),
             updater::installScopeToString(updater::InstallScope::Machine));
    QCOMPARE(installScopeId(InstallScope::Machine), QStringLiteral("machine"));
    QCOMPARE(int(InstallDetection().scope), int(InstallScope::User));
}

void UpdateInstallTypeTest::automaticInstallAgreesWithTheUpdaterHelper()
{
    for (const InstallType type : kAllTypes) {
        const QString id = installTypeId(type);
        const updater::UpdaterMode mode = updater::modeFromString(id);
        QVERIFY2(mode != updater::UpdaterMode::Invalid, qPrintable(id));
        const bool offered = canInstallAutomatically(type);
        const bool accepted = updater::isSelfInstallable(mode);
        QVERIFY2(offered == accepted,
                 qPrintable(QStringLiteral("%1: Lightning offers=%2, helper accepts=%3")
                                .arg(id,
                                     offered ? QStringLiteral("true") : QStringLiteral("false"),
                                     accepted ? QStringLiteral("true")
                                              : QStringLiteral("false"))));
    }
}

void UpdateInstallTypeTest::appImageClaimNeedsTheAppImageMagic()
{
    const QString path = QStringLiteral("/home/user/notes.txt");
    InstallEnvironment environment =
        makeEnvironment({ { QStringLiteral("APPIMAGE"), path } }, { path }, QString());
    environment.looksLikeAppImage = [](const QString &) { return false; };
    const InstallDetection refused = detectInstall(environment);
    QCOMPARE(refused.type, InstallType::Development);
    QVERIFY(!refused.automaticInstallAllowed);

    environment.looksLikeAppImage = nullptr;
    QCOMPARE(detectInstall(environment).type, InstallType::Development);
}

void UpdateInstallTypeTest::appImageMagicIsReadFromTheFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto write = [&](const char *name, const QByteArray &bytes) {
        const QString path = QDir(dir.path()).absoluteFilePath(QString::fromLatin1(name));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return QString();
        file.write(bytes);
        return path;
    };
    QByteArray elf = QByteArrayLiteral("\x7f" "ELF");
    elf += QByteArray(4, '\x01');
    const QByteArray appImage2 = elf + QByteArrayLiteral("AI\x02") + QByteArray(32, '\0');
    const QByteArray appImage1 = elf + QByteArrayLiteral("AI\x01") + QByteArray(32, '\0');
    const QByteArray plainElf = elf + QByteArray(35, '\0');
    QVERIFY(fileLooksLikeAppImage(write("type2.AppImage", appImage2)));
    QVERIFY(fileLooksLikeAppImage(write("type1.AppImage", appImage1)));
    QVERIFY(!fileLooksLikeAppImage(write("plain.elf", plainElf)));
    QVERIFY(!fileLooksLikeAppImage(write("text.txt", QByteArrayLiteral("hello"))));
    QVERIFY(!fileLooksLikeAppImage(write("short", QByteArrayLiteral("\x7f" "ELF"))));
    QVERIFY(!fileLooksLikeAppImage(QDir(dir.path()).absoluteFilePath(QStringLiteral("absent"))));
}

QTEST_APPLESS_MAIN(UpdateInstallTypeTest)
#include "UpdateInstallTypeTest.moc"
