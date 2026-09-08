#include "updater/RelaunchEnvironment.h"

#include <QtTest/QtTest>

#include <QHash>

using updater::RelaunchEnvironmentChange;
using updater::relaunchEnvironmentChanges;

// The environment corrections a relaunch needs, CALLED rather than performed.
//
// Every case here is a real observation from 2026-09-08, reproduced with a
// throwaway AppImage built from the shipped runtime and run exactly the way
// the maintainer runs Lightning
// (`nix-shell -p appimage-run --run "appimage-run …"`):
//
//   inherited TMPDIR (deleted with the shell) -> create mount dir error
//   TMPDIR dropped, mount attempted           -> no suitable fusermount, 127
//   TMPDIR dropped + extraction asked for     -> "Lightning 0.9.3", exit 0
//
// The first two FAIL on the code that shipped in 0.9.3, which relaunched with
// whatever environment it happened to inherit.
namespace {

struct Env {
    QHash<QString, QString> variables;
    QSet<QString> directories;

    std::function<QString(const QString &)> lookup() const
    {
        return [this](const QString &name) { return variables.value(name); };
    }
    std::function<bool(const QString &)> exists() const
    {
        return [this](const QString &path) { return directories.contains(path); };
    }
};

bool removes(const QList<RelaunchEnvironmentChange> &changes, const QString &name)
{
    for (const auto &change : changes) {
        if (change.name == name)
            return change.remove;
    }
    return false;
}

bool mentions(const QList<RelaunchEnvironmentChange> &changes, const QString &name)
{
    for (const auto &change : changes) {
        if (change.name == name)
            return true;
    }
    return false;
}

QString valueOf(const QList<RelaunchEnvironmentChange> &changes, const QString &name)
{
    for (const auto &change : changes) {
        if (change.name == name && !change.remove)
            return change.value;
    }
    return {};
}

} // namespace

class RelaunchEnvironmentTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // THE REPORTED FAILURE. nix-shell deletes its private temporary directory
    // when it exits, and it exits when Lightning does — so the AppImage
    // runtime's mount point cannot be created and the relaunched process dies
    // before it runs anything of its own.
    void aTemporaryDirectoryThatDiedWithItsShellIsDropped()
    {
        Env env;
        env.variables[QStringLiteral("TMPDIR")] =
            QStringLiteral("/tmp/nix-shell-2895236-3208647408");
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QVERIFY2(removes(changes, QStringLiteral("TMPDIR")),
                 "a TMPDIR naming a directory that no longer exists was passed to the "
                 "relaunched process, which is 'create mount dir error: No such file "
                 "or directory'");
    }

    // Half a fix is not a fix: Qt reads TEMP and TMP on Windows, the AppImage
    // runtime reads TMPDIR, and a shell that sets one usually sets all of
    // them at the same dead path.
    void everySpellingOfTheTemporaryDirectoryIsDropped()
    {
        Env env;
        for (const auto &name : {"TMPDIR", "TMP", "TEMP", "TEMPDIR"})
            env.variables[QString::fromLatin1(name)] = QStringLiteral("/tmp/gone-with-the-shell");
        const auto changes = relaunchEnvironmentChanges(false, env.lookup(), env.exists());
        for (const auto &name : {"TMPDIR", "TMP", "TEMP", "TEMPDIR"}) {
            QVERIFY2(removes(changes, QString::fromLatin1(name)),
                     qPrintable(QStringLiteral("%1 survived").arg(QLatin1String(name))));
        }
    }

    // NOT AN APPIMAGE PROBLEM. A deb or rpm relaunched with a dead TMPDIR
    // fails on its first QTemporaryFile instead, so the correction is not
    // conditional on the install mode.
    void theTemporaryDirectoryIsCorrectedForEveryInstallMode()
    {
        Env env;
        env.variables[QStringLiteral("TMPDIR")] = QStringLiteral("/tmp/gone");
        QVERIFY(removes(relaunchEnvironmentChanges(false, env.lookup(), env.exists()),
                        QStringLiteral("TMPDIR")));
    }

    void aTemporaryDirectoryThatStillExistsIsLeftAlone()
    {
        Env env;
        env.variables[QStringLiteral("TMPDIR")] = QStringLiteral("/tmp/still-here");
        env.directories.insert(QStringLiteral("/tmp/still-here"));
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QVERIFY2(!mentions(changes, QStringLiteral("TMPDIR")),
                 "a perfectly good TMPDIR was thrown away");
    }

    void anUnsetTemporaryDirectoryIsNotInvented()
    {
        Env env;
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QVERIFY(!mentions(changes, QStringLiteral("TMPDIR")));
    }

    // THE SECOND MEASURED FAILURE. Started through appimage-run, the process
    // — and the helper, and therefore the relaunch — lives inside a
    // bubblewrap sandbox with no_new_privs set, where the setuid fusermount
    // cannot gain privilege. The runtime answers "No suitable fusermount
    // binary found on the $PATH" and exits 127. Extraction is the only way
    // back up from in there.
    void anExtractedAppImageAsksForExtractionWhenItRestarts()
    {
        Env env;
        env.variables[QStringLiteral("APPDIR")] =
            QStringLiteral("/home/roksme/.cache/appimage-run/e77b1a96fe0a51b9");
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QCOMPARE(valueOf(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")),
                 QStringLiteral("1"));
    }

    // A previous --appimage-extract-and-run is the same situation: whatever
    // unpacked us, we are not mounted, so we cannot assume a mount will work.
    void anAlreadyExtractedRunAsksForExtractionToo()
    {
        Env env;
        env.variables[QStringLiteral("APPDIR")] =
            QStringLiteral("/tmp/appimage_extracted_5d0c6a452b329277");
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QCOMPARE(valueOf(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")),
                 QStringLiteral("1"));
    }

    // THE CHEAP PATH STAYS CHEAP. A mounted AppImage proves fuse works here,
    // and extraction would copy the whole payload for nothing on every single
    // update.
    void aMountedAppImageIsNotMadeToUnpackItself()
    {
        Env env;
        env.variables[QStringLiteral("APPDIR")] = QStringLiteral("/tmp/.mount_LightnLMAJmM");
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QVERIFY2(!mentions(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")),
                 "a normally mounted AppImage was told to unpack itself");
        // A trailing slash is not a different directory.
        env.variables[QStringLiteral("APPDIR")] = QStringLiteral("/tmp/.mount_LightnLMAJmM/");
        QVERIFY(!mentions(relaunchEnvironmentChanges(true, env.lookup(), env.exists()),
                          QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")));
    }

    void nothingIsAskedOfANonAppImageInstall()
    {
        Env env;
        env.variables[QStringLiteral("APPDIR")] = QStringLiteral("/opt/somewhere");
        const auto changes = relaunchEnvironmentChanges(false, env.lookup(), env.exists());
        QVERIFY(!mentions(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")));
    }

    // NO APPDIR IS NO EVIDENCE. Keep the behaviour that has always shipped
    // rather than imposing an extraction on a guess.
    void anAppImageWithoutAnAppDirKeepsTheOldBehaviour()
    {
        Env env;
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QVERIFY(!mentions(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")));
    }

    // A deliberate choice by whoever launched us wins, in both directions.
    void anExistingExtractionChoiceIsNeverOverridden()
    {
        Env env;
        env.variables[QStringLiteral("APPDIR")] =
            QStringLiteral("/home/roksme/.cache/appimage-run/e77b1a96fe0a51b9");
        env.variables[QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")] = QStringLiteral("0");
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QVERIFY2(!mentions(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")),
                 "someone set it to 0 on purpose and it was overwritten");
    }

    // Both corrections at once is the maintainer's actual case, and they are
    // independent: the mount-point failure comes first and hides the fuse
    // failure behind it.
    void theMaintainersCaseGetsBothCorrections()
    {
        Env env;
        env.variables[QStringLiteral("TMPDIR")] =
            QStringLiteral("/tmp/nix-shell-2895236-3208647408");
        env.variables[QStringLiteral("APPDIR")] =
            QStringLiteral("/home/roksme/.cache/appimage-run/e77b1a96fe0a51b9");
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QVERIFY(removes(changes, QStringLiteral("TMPDIR")));
        QCOMPARE(valueOf(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")),
                 QStringLiteral("1"));
        // Every change carries a sentence a support log can quote.
        for (const auto &change : changes)
            QVERIFY(!change.reason.isEmpty());
    }
};

QTEST_MAIN(RelaunchEnvironmentTest)
#include "RelaunchEnvironmentTest.moc"
