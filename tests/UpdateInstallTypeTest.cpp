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
using lightning::update::InstallType;
using lightning::update::installTypeFromId;
using lightning::update::installTypeId;
using lightning::update::installTypeLabel;
using lightning::update::isPackageManaged;

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
                                         bool windowsPlatform)
{
    InstallEnvironment environment = makeEnvironment({}, {}, compileTimeId);
    environment.readInstallMarker = [markerContents]() { return markerContents; };
    environment.windowsPlatform = windowsPlatform;
    return environment;
}

} // namespace

class UpdateInstallTypeTest : public QObject
{
    Q_OBJECT

private slots:
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
    void automaticInstallAgreesWithTheUpdaterHelper();
    void appImageClaimNeedsTheAppImageMagic();
    void appImageMagicIsReadFromTheFile();
};

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
