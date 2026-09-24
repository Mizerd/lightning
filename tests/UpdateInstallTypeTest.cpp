// Lightning secure update system: install-type identity.
//
// The install type decides whether Lightning may install anything at all,
// so every detection branch is exercised through the injectable
// environment — no setenv races, and no branch that only the packaging
// pipeline could reach in practice goes untested.

#include "update/InstallType.h"

// Header-only: modeFromString() and isSelfInstallable() are inline, so this
// test links no updater source. It is here to prove that the application's
// install-type policy and the helper's own mode policy cannot drift apart.
#include "updater/UpdaterArgs.h"

#include <QHash>
#include <QSet>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using lightning::update::canInstallAutomatically;
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
using lightning::update::sameInstallDirectory;

namespace {

const QList<InstallType> kAllTypes{
    InstallType::WindowsMsi,    InstallType::WindowsSetup,  InstallType::WindowsPortable,
    InstallType::LinuxAppImage, InstallType::LinuxDeb,      InstallType::LinuxRpm,
    InstallType::LinuxFlatpak,  InstallType::LinuxSnap,     InstallType::MacosDmg,
    InstallType::Development,   InstallType::Unknown,
};

// A fully synthetic environment: nothing is read from the real process.
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
    // In this fixture every existing path is taken to carry the AppImage
    // magic; appImageClaimNeedsTheAppImageMagic overrides the hook to say no.
    environment.looksLikeAppImage = [existingPaths](const QString &path) {
        return existingPaths.contains(path);
    };
    environment.compileTimeId = compileTimeId;
    return environment;
}

// The same, plus an install marker and the platform switch that decides
// whether the marker is consulted at all. Injecting the platform is what lets
// one host exercise both the Windows branch and the non-Windows branch.
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

} // namespace

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
    void packageManagedIsFlatpakAndSnapOnly();
    void compileTimeValueIsUsedForConcretePackages_data();
    void compileTimeValueIsUsedForConcretePackages();
    void unsetCompileTimeValueIsDevelopment();
    void unrecognisedCompileTimeValueIsUnknown();
    void flatpakEnvironmentIsDetected();
    void flatpakInfoFileIsDetected();
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

// ── The portable strategy must be PROVEN, never reached by fallback ─────
//
// windows-portable is the compiled-in value for all three Windows packages,
// because they are built from one tree; only the installer that placed the
// files corrects it, by writing `.lightning-install-type`. The NSIS script
// writes that file without checking the write succeeded, so an INSTALLED copy
// whose marker is missing used to fall through to portable — the one strategy
// that swaps the whole directory. It would move `.lightning-install-root` into
// the backup, leaving an ARP entry that can never uninstall, and relocate the
// user's data root so they appear signed out.
//
// A portable copy always ships `portable.marker`; the installed packages never
// do. So the fallback now needs that positive evidence, and without it reports
// Unknown: the update is still offered, it is simply not APPLIED with a
// strategy that was never confirmed.
void UpdateInstallTypeTest::portableIsRefusedWithoutItsOwnMarker()
{
    // A real portable copy: no install-type marker, but portable.marker is
    // there. Unchanged behaviour.
    const InstallDetection genuine = detectInstall(makeMarkerEnvironment(
        QString(), QStringLiteral("windows-portable"), true, true));
    QCOMPARE(genuine.type, InstallType::WindowsPortable);
    QVERIFY(genuine.automaticInstallAllowed);

    // An installed copy whose marker write failed: same compiled-in value,
    // but no portable.marker. It must NOT be treated as portable.
    const InstallDetection installed = detectInstall(makeMarkerEnvironment(
        QString(), QStringLiteral("windows-portable"), true, false));
    QVERIFY2(installed.type != InstallType::WindowsPortable,
             "an installation with no portable marker was treated as portable");
    QCOMPARE(installed.type, InstallType::Unknown);
    QVERIFY2(!installed.automaticInstallAllowed,
             "an unproven install type must not apply an update");

    // An explicit marker still wins outright: that is the installer speaking.
    const InstallDetection declared = detectInstall(makeMarkerEnvironment(
        QStringLiteral("windows-setup"), QStringLiteral("windows-portable"),
        true, false));
    QCOMPARE(declared.type, InstallType::WindowsSetup);

    // And nothing here touches other platforms.
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
        QStringLiteral("macos-dmg"),      QStringLiteral("development"),
        QStringLiteral("unknown"),
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
    // The updater helper has no macOS strategy, so Lightning must not offer
    // one either: planForMode(macos-dmg) returns UnsupportedPlatform and
    // isSelfInstallable(MacosDmg) is false.
    QTest::newRow("macos-dmg") << int(InstallType::MacosDmg) << false;
    QTest::newRow("linux-flatpak") << int(InstallType::LinuxFlatpak) << false;
    QTest::newRow("linux-snap") << int(InstallType::LinuxSnap) << false;
    QTest::newRow("development") << int(InstallType::Development) << false;
    QTest::newRow("unknown") << int(InstallType::Unknown) << false;
}

void UpdateInstallTypeTest::automaticInstallPolicyIsExplicit()
{
    QFETCH(int, type);
    QFETCH(bool, automatic);
    QCOMPARE(canInstallAutomatically(static_cast<InstallType>(type)), automatic);
}

void UpdateInstallTypeTest::packageManagedIsFlatpakAndSnapOnly()
{
    for (const InstallType type : kAllTypes) {
        const bool expected =
            type == InstallType::LinuxFlatpak || type == InstallType::LinuxSnap;
        QCOMPARE(isPackageManaged(type), expected);
    }
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
    // Never assumed to be a development build, and never installable.
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
    // The very same .deb-built binary really is running inside Flatpak.
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
    // linux-deb is normally installable; the override is not an authority.
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

    // An override naming something this build does not know is ignored.
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
    // Checking is permitted; installing still is not.
    QVERIFY(!on.automaticInstallAllowed);

    const InstallDetection wrongValue = detectInstall(makeEnvironment(
        { { QStringLiteral("LIGHTNING_ALLOW_DEV_UPDATE_CHECK"), QStringLiteral("yes") } }, {},
        QString()));
    QVERIFY(!wrongValue.developmentCheckAllowed);
}

void UpdateInstallTypeTest::installMarkerSelectsAmongTheWindowsPackages()
{
    // The whole reason the marker exists: one Windows build ships as an MSI,
    // a setup EXE and a portable ZIP, and only the installer knows which.
    const InstallDetection portable = detectInstall(makeMarkerEnvironment(
        QStringLiteral("windows-portable"), QStringLiteral("windows-msi"),
        /*windowsPlatform=*/true));
    QCOMPARE(portable.type, InstallType::WindowsPortable);
    QVERIFY(!portable.diagnosticOverride);
    QVERIFY(portable.automaticInstallAllowed);

    const InstallDetection setup = detectInstall(makeMarkerEnvironment(
        QStringLiteral("windows-setup"), QStringLiteral("windows-msi"), true));
    QCOMPARE(setup.type, InstallType::WindowsSetup);

    // Runtime ecosystem evidence still outranks it.
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

    // The defect: a stray marker file beside an AppImage flipped a genuine
    // linux-appimage install to linux-deb, which then asked for a PolicyKit
    // password and handed apt-get a package that was never installed here.
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
                                            /*windowsPlatform=*/false));
    QCOMPARE(detection.type, static_cast<InstallType>(expected));
}

void UpdateInstallTypeTest::installMarkerNeverNamesANonWindowsType()
{
    // Even ON Windows the marker only selects among the three Windows
    // packages; it is not a general-purpose install-type override.
    const InstallDetection detection = detectInstall(makeMarkerEnvironment(
        QStringLiteral("linux-deb"), QStringLiteral("windows-msi"),
        /*windowsPlatform=*/true));
    QCOMPARE(detection.type, InstallType::WindowsMsi);

    const InstallDetection nonsense = detectInstall(makeMarkerEnvironment(
        QStringLiteral("not-a-type"), QStringLiteral("windows-setup"), true));
    QCOMPARE(nonsense.type, InstallType::WindowsSetup);

    const InstallDetection empty = detectInstall(
        makeMarkerEnvironment(QString(), QStringLiteral("windows-setup"), true));
    QCOMPARE(empty.type, InstallType::WindowsSetup);
}

// ISSUE #14. A per-machine MSI or setup installation must be upgraded in the
// per-machine context, elevated; anything else leaves a second copy beside it.
// The scope marker is the only thing that knows, so every way it could be
// misread is pinned here: only the two installer types, only on Windows, and
// only the exact word "machine" -- a missing marker (every installation made
// by 0.9.9 or older) must stay per-user, or those users would be handed a UAC
// prompt for an update that never needed one.
void UpdateInstallTypeTest::scopeMarkerMakesAWindowsInstallerCopyPerMachine_data()
{
    // The COMPILE-TIME id is the type marker too, so every row lands on the
    // type it names on EVERY platform. That matters for the off-Windows rows:
    // off Windows the type marker is ignored, and with a compiled-in
    // windows-portable those rows used to land on portable -- where the scope
    // branch never runs, guard or no guard, so they could not see the Windows
    // guard at all (mutation m3 survived them). Now they reach an MSI / setup
    // type whose "machine" marker WOULD be honoured if the guard were missing.
    //
    // `registered` is what HKLM names as a per-machine directory. "machine" is
    // believed only when it names THIS directory: the marker alone is
    // user-writable in a per-user installation.
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
    // The spoof: a per-user install whose own marker says "machine". Nothing
    // in HKLM names it, so it stays per-user and no UAC prompt is raised.
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
    // Portable has no installer to own a context: the word means nothing there.
    QTest::newRow("portable-ignores-it")
        << "windows-portable" << "machine" << here << true << true << portable << user;
    // Off Windows the file is a stray, exactly like the type marker -- even
    // beside a build whose compile-time type IS an MSI or setup, and even with
    // a matching registration.
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
    // The row reached the type it is about; otherwise the scope assertion
    // below would be testing a branch that never runs.
    QCOMPARE(int(detection.type), expectedType);
    QCOMPARE(int(detection.scope), expectedScope);
    // The scope never changes WHAT is installed, only in which context.
    QVERIFY(detection.automaticInstallAllowed);
    if (!windowsPlatform) {
        // Off Windows the marker is not even consulted.
        QCOMPARE(reads, 0);
    } else if (typeMarker != QLatin1String("windows-portable")) {
        // And where it can decide, the hook really was the thing deciding.
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
    // The application writes installScopeId() into --install-scope and the
    // helper maps it back with its own copy of the two words.
    QCOMPARE(installScopeId(InstallScope::User),
             updater::installScopeToString(updater::InstallScope::User));
    QCOMPARE(installScopeId(InstallScope::Machine),
             updater::installScopeToString(updater::InstallScope::Machine));
    QCOMPARE(installScopeId(InstallScope::Machine), QStringLiteral("machine"));
    // A default-constructed detection is per-user: nothing asks for elevation
    // unless a marker said so.
    QCOMPARE(int(InstallDetection().scope), int(InstallScope::User));
}

void UpdateInstallTypeTest::automaticInstallAgreesWithTheUpdaterHelper()
{
    // Two independent policies, one promise. Lightning must never offer an
    // automatic install for a mode the helper refuses at argument parsing
    // (ModeNotSelfInstallable), and must never withhold one the helper would
    // happily perform. macos-dmg is the case that used to disagree.
    for (const InstallType type : kAllTypes) {
        const QString id = installTypeId(type);
        const updater::UpdaterMode mode = updater::modeFromString(id);
        // Every canonical install-type id is also a canonical helper mode.
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
    // $APPIMAGE is an environment variable and it becomes the updater's
    // --target: the file that is chmod +x'd and replaced. An existing path
    // that is NOT an AppImage does not make this an AppImage install.
    const QString path = QStringLiteral("/home/user/notes.txt");
    InstallEnvironment environment =
        makeEnvironment({ { QStringLiteral("APPIMAGE"), path } }, { path }, QString());
    environment.looksLikeAppImage = [](const QString &) { return false; };
    const InstallDetection refused = detectInstall(environment);
    QCOMPARE(refused.type, InstallType::Development);
    QVERIFY(!refused.automaticInstallAllowed);

    // A hand-built environment that never set the hook accepts nothing.
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
