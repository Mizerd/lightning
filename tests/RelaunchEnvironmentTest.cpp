#include "updater/RelaunchEnvironment.h"

#include <QtTest/QtTest>

#include <QHash>

using updater::RelaunchEnvironmentChange;
using updater::relaunchEnvironmentChanges;

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

}

class RelaunchEnvironmentTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
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

    void anExtractedAppImageAsksForExtractionWhenItRestarts()
    {
        Env env;
        env.variables[QStringLiteral("APPDIR")] =
            QStringLiteral("/home/roksme/.cache/appimage-run/e77b1a96fe0a51b9");
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QCOMPARE(valueOf(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")),
                 QStringLiteral("1"));
    }

    void anAlreadyExtractedRunAsksForExtractionToo()
    {
        Env env;
        env.variables[QStringLiteral("APPDIR")] =
            QStringLiteral("/tmp/appimage_extracted_5d0c6a452b329277");
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QCOMPARE(valueOf(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")),
                 QStringLiteral("1"));
    }

    void aMountedAppImageIsNotMadeToUnpackItself()
    {
        Env env;
        env.variables[QStringLiteral("APPDIR")] = QStringLiteral("/tmp/.mount_LightnLMAJmM");
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QVERIFY2(!mentions(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")),
                 "a normally mounted AppImage was told to unpack itself");
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

    void anAppImageWithoutAnAppDirKeepsTheOldBehaviour()
    {
        Env env;
        const auto changes = relaunchEnvironmentChanges(true, env.lookup(), env.exists());
        QVERIFY(!mentions(changes, QStringLiteral("APPIMAGE_EXTRACT_AND_RUN")));
    }

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
        for (const auto &change : changes)
            QVERIFY(!change.reason.isEmpty());
    }
};

QTEST_MAIN(RelaunchEnvironmentTest)
#include "RelaunchEnvironmentTest.moc"
