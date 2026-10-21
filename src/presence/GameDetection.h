#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// Pure, GUI-free game detection for the "Playing X" Labs feature: read the
// process table, resolve a Steam app id to a name from the LOCAL library
// (no network), match a small built-in list plus user entries. Everything
// takes its roots as arguments so a fixture directory can stand in for /proc
// and for ~/.local/share/Steam. Only the resulting game NAME ever leaves
// this module; the process list never does.
namespace gamedetect {

struct ProcessInfo {
    qint64 pid = 0;
    // exe symlink target (Linux) or full image path (Windows); may be empty.
    QString exePath;
    QStringList args;   // argv; empty on Windows
    // SteamAppId from the environment, read only for Steam-looking children.
    QString steamAppIdEnv;
};

struct SteamApp {
    QString appId;
    QString name;
    QString installDir;
};

// "key" "value" lines of a Valve KeyValues text file; first occurrence wins.
QString vdfFirstValue(const QByteArray &text, const QString &key);
// appmanifest_<id>.acf -> {appid, name, installdir}; invalid when no name.
SteamApp parseAppManifest(const QByteArray &text);
// libraryfolders.vdf -> library root paths (each "path" entry).
QStringList parseLibraryFolders(const QByteArray &text);

// Local Steam library: appid -> name and installdir -> name. Reloads when a
// steamapps directory's mtime moves, so a game installed mid-session shows.
class SteamCatalog
{
public:
    explicit SteamCatalog(QStringList steamRoots = {});
    void setRoots(const QStringList &steamRoots) { m_roots = steamRoots; m_stamp.clear(); }
    QString nameForAppId(const QString &appId);
    // `path` is any path; matches ".../steamapps/common/<dir>/...".
    QString nameForPath(const QString &path);
    // The default roots for this user (native + flatpak Steam).
    static QStringList defaultRoots();
    int appCount() { refresh(); return m_nameById.size(); }
    // Within a batch (one detectGames pass) the library is checked once.
    void beginBatch() { m_batch = true; m_checkedInBatch = false; }
    void endBatch() { m_batch = false; m_checkedInBatch = false; }
    // How many times the library directories were actually examined.
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
    QHash<QString, QString> m_nameByDir;   // installdir, lower-cased
};

// One Steam app id, a built-in exe, or a user entry: "exe=Display Name"
// (or a bare "exe", shown under that name).
struct Catalog {
    QHash<QString, QString> exeToName;   // lower-cased basename -> name
    static Catalog builtIn();
    void addUserEntries(const QStringList &entries);
};

// Linux /proc (or a fixture root). Bounded: numeric dirs only, capped count,
// capped bytes per file.
QList<ProcessInfo> scanProcRoot(const QString &procRoot, int maxProcesses = 8192);

#ifdef Q_OS_WIN
QList<ProcessInfo> scanProcessesWindows();
// Full image path of the foreground window's process when the window covers
// its monitor, else empty. It is only ever fed to knownGameForExePath().
QString foregroundFullscreenExePath();
#endif

// Detected game names, earliest process first, de-duplicated, never empty
// strings. Steam ids that map to no local name are dropped, not shown.
QStringList detectGames(const QList<ProcessInfo> &procs, SteamCatalog &steam,
                        const Catalog &catalog);

// The game a process image path is, by the SAME rules detectGames() uses
// (Steam appmanifest under steamapps/common, then the built-in and user exe
// list), or "" when it is not a known game. An arbitrary executable is never a
// game here, however it is displayed.
QString knownGameForExePath(const QString &exePath, SteamCatalog &steam,
                            const Catalog &catalog);

// Windows: a fullscreen foreground window is only a CONFIRMATION. It can move
// an already-detected game to the front of the list when several run, and it
// can never add a name: a fullscreen browser, office suite or video player is
// not a game and nothing is derived from its file name.
QStringList preferForeground(QStringList names, const QString &foregroundGame);

// The text published as status_msg, in the SENDER's app language: a status
// line is the user's own words, and Matrix cannot localise it per reader.
QString statusTextFor(const QString &gameName);

// True for a game the user hid (case-insensitive, whitespace-trimmed).
bool isHidden(const QString &gameName, const QStringList &hideList);

// False where this build reads no process table at all (macOS): the feature
// can be switched on there and will never find a game.
bool detectionSupported();

// Sandboxed builds cannot see host processes.
bool detectionLimitedBySandbox();

}   // namespace gamedetect
