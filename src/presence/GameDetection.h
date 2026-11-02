#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace gamedetect {

struct ProcessInfo {
    qint64 pid = 0;
    QString exePath;
    QStringList args;
    QString steamAppIdEnv;
};

struct SteamApp {
    QString appId;
    QString name;
    QString installDir;
};

QString vdfFirstValue(const QByteArray &text, const QString &key);
SteamApp parseAppManifest(const QByteArray &text);
QStringList parseLibraryFolders(const QByteArray &text);

class SteamCatalog
{
public:
    explicit SteamCatalog(QStringList steamRoots = {});
    void setRoots(const QStringList &steamRoots) { m_roots = steamRoots; m_stamp.clear(); }
    QString nameForAppId(const QString &appId);
    QString nameForPath(const QString &path);
    static QStringList defaultRoots();
    int appCount() { refresh(); return m_nameById.size(); }
    void beginBatch() { m_batch = true; m_checkedInBatch = false; }
    void endBatch() { m_batch = false; m_checkedInBatch = false; }
    int checksForTest() const { return m_checks; }

private:
    void refresh();
    QStringList steamappsDirs() const;
    QStringList m_roots;
    bool m_batch = false;
    bool m_checkedInBatch = false;
    int m_checks = 0;
    QString m_stamp;
    QHash<QString, QString> m_nameById;
    QHash<QString, QString> m_nameByDir;
};

struct Catalog {
    QHash<QString, QString> exeToName;
    static Catalog builtIn();
    void addUserEntries(const QStringList &entries);
};

QList<ProcessInfo> scanProcRoot(const QString &procRoot, int maxProcesses = 8192);

#ifdef Q_OS_WIN
QList<ProcessInfo> scanProcessesWindows();
QString foregroundFullscreenExePath();
#endif

QStringList detectGames(const QList<ProcessInfo> &procs, SteamCatalog &steam,
                        const Catalog &catalog);

QString knownGameForExePath(const QString &exePath, SteamCatalog &steam,
                            const Catalog &catalog);

// Windows: a fullscreen foreground window is only a CONFIRMATION. It can move
// an already-detected game to the front of the list when several run, and it
// can never add a name: a fullscreen browser, office suite or video player is
// not a game and nothing is derived from its file name.
QStringList preferForeground(QStringList names, const QString &foregroundGame);

QString statusTextFor(const QString &gameName);

bool isHidden(const QString &gameName, const QStringList &hideList);

// False where this build reads no process table at all (macOS): the feature
// can be switched on there and will never find a game.
bool detectionSupported();

bool detectionLimitedBySandbox();

}
