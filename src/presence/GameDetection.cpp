#include "presence/GameDetection.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace gamedetect {

namespace {

constexpr qint64 kMaxFileBytes = 256 * 1024;

QString unescapeVdf(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('\\') && i + 1 < s.size()) {
            ++i;
            out.append(s[i] == QLatin1Char('n') ? QLatin1Char('\n') : s[i]);
        } else {
            out.append(s[i]);
        }
    }
    return out;
}

const QRegularExpression &kvLine()
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*\"([^\"]+)\"\\s+\"((?:[^\"\\\\]|\\\\.)*)\""));
    return re;
}

QByteArray readBounded(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.read(kMaxFileBytes);
}

QString baseName(const QString &path)
{
    const int cut = qMax(path.lastIndexOf(QLatin1Char('/')),
                         path.lastIndexOf(QLatin1Char('\\')));
    return path.mid(cut + 1);
}

QString normalizedPath(QString p)
{
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return p;
}

// Steam's own tools are installed as apps under steamapps/common beside the
// games, and a process of theirs (Proton's launcher, the Steam Linux Runtime's
// container) runs for as long as any game does: they are not games, and a
// non-Steam title run through Proton must not surface as "Proton Experimental".
// Matched by id for the ones with a stable id and by name/installdir for the
// versioned family (every Proton release is a new app id).
bool isSteamTool(const QString &appId, const QString &name = {},
                 const QString &installDir = {})
{
    static const QStringList ids = {
        QStringLiteral("0"),
        QStringLiteral("228980"),    // Steamworks Common Redistributables
        QStringLiteral("1493710"),   // Proton Experimental
        QStringLiteral("2180100"),   // Proton Hotfix
        QStringLiteral("1070560"),   // Steam Linux Runtime 1.0 (scout)
        QStringLiteral("1391110"),   // Steam Linux Runtime 2.0 (soldier)
        QStringLiteral("1628350"),   // Steam Linux Runtime 3.0 (sniper)
        QStringLiteral("1826330"),   // Proton EasyAntiCheat Runtime
        QStringLiteral("1161040"),   // Proton BattlEye Runtime
    };
    if (ids.contains(appId))
        return true;
    static const QRegularExpression protonName(
        QStringLiteral("^Proton(\\s*-\\s*|\\s+|_)(\\d|Experimental|Hotfix|EasyAntiCheat|BattlEye)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression runtimeName(
        QStringLiteral("^(Steam\\s*Linux\\s*Runtime|Steamworks)"),
        QRegularExpression::CaseInsensitiveOption);
    return protonName.match(name).hasMatch() || runtimeName.match(name).hasMatch()
        || protonName.match(installDir).hasMatch()
        || runtimeName.match(installDir).hasMatch();
}

}   // namespace

QString vdfFirstValue(const QByteArray &text, const QString &key)
{
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const auto m = kvLine().match(line);
        if (m.hasMatch() && m.captured(1).compare(key, Qt::CaseInsensitive) == 0)
            return unescapeVdf(m.captured(2));
    }
    return {};
}

SteamApp parseAppManifest(const QByteArray &text)
{
    SteamApp app;
    app.appId = vdfFirstValue(text, QStringLiteral("appid")).trimmed();
    app.name = vdfFirstValue(text, QStringLiteral("name")).trimmed();
    app.installDir = vdfFirstValue(text, QStringLiteral("installdir")).trimmed();
    if (app.appId.isEmpty() || app.name.isEmpty())
        return {};
    return app;
}

QStringList parseLibraryFolders(const QByteArray &text)
{
    // New format: "0" { "path" "/lib" ... }. Old format (before 2021): a bare
    // "1" "/lib" pair, key all digits and value an absolute path. "ContentStatsID"
    // and friends are digits-valued and never look like a path.
    static const QRegularExpression digits(QStringLiteral("^\\d+$"));
    static const QRegularExpression absolute(
        QStringLiteral("^(/|[A-Za-z]:[\\\\/])"));
    QStringList out;
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const auto m = kvLine().match(line);
        if (!m.hasMatch())
            continue;
        const bool isPath = m.captured(1).compare(QLatin1String("path"), Qt::CaseInsensitive) == 0;
        const QString p = unescapeVdf(m.captured(2));
        const bool isOldEntry = digits.match(m.captured(1)).hasMatch()
            && absolute.match(p).hasMatch();
        if ((isPath || isOldEntry) && !p.isEmpty() && !out.contains(p))
            out.append(p);
    }
    return out;
}

// ---------------------------------------------------------------------------

SteamCatalog::SteamCatalog(QStringList steamRoots) : m_roots(std::move(steamRoots)) {}

QStringList SteamCatalog::defaultRoots()
{
    const QString home = QDir::homePath();
    QStringList roots = {
        home + QStringLiteral("/.local/share/Steam"),
        home + QStringLiteral("/.steam/steam"),
        home + QStringLiteral("/.var/app/com.valvesoftware.Steam/.local/share/Steam"),
    };
#ifdef Q_OS_WIN
    roots << QStringLiteral("C:/Program Files (x86)/Steam")
          << QStringLiteral("C:/Program Files/Steam");
#endif
    return roots;
}

QStringList SteamCatalog::steamappsDirs() const
{
    QStringList dirs;
    auto add = [&dirs](const QString &d) {
        const QString canon = QFileInfo(d).canonicalFilePath();
        if (!canon.isEmpty() && !dirs.contains(canon))
            dirs.append(canon);
    };
    for (const QString &root : m_roots) {
        add(root + QStringLiteral("/steamapps"));
        const QByteArray vdf =
            readBounded(root + QStringLiteral("/steamapps/libraryfolders.vdf"));
        for (const QString &lib : parseLibraryFolders(vdf))
            add(lib + QStringLiteral("/steamapps"));
    }
    return dirs;
}

void SteamCatalog::refresh()
{
    // Inside a batch the catalog is checked once, not once per matched
    // process: each check re-reads libraryfolders.vdf.
    if (m_batch && m_checkedInBatch)
        return;
    m_checkedInBatch = true;
    ++m_checks;
    const QStringList dirs = steamappsDirs();
    QString stamp;
    for (const QString &d : dirs)
        stamp += d + QLatin1Char('@')
            + QString::number(QFileInfo(d).lastModified().toMSecsSinceEpoch())
            + QLatin1Char(';');
    if (stamp == m_stamp && !m_stamp.isEmpty())
        return;
    m_stamp = stamp.isEmpty() ? QStringLiteral("none") : stamp;
    m_nameById.clear();
    m_nameByDir.clear();
    for (const QString &d : dirs) {
        const QStringList manifests = QDir(d).entryList(
            { QStringLiteral("appmanifest_*.acf") }, QDir::Files);
        for (const QString &file : manifests) {
            const SteamApp app = parseAppManifest(readBounded(d + QLatin1Char('/') + file));
            if (app.appId.isEmpty() || isSteamTool(app.appId, app.name, app.installDir))
                continue;
            m_nameById.insert(app.appId, app.name);
            if (!app.installDir.isEmpty())
                m_nameByDir.insert(app.installDir.toLower(), app.name);
        }
    }
}

QString SteamCatalog::nameForAppId(const QString &appId)
{
    if (appId.isEmpty() || isSteamTool(appId))
        return {};
    refresh();
    return m_nameById.value(appId);
}

QString SteamCatalog::nameForPath(const QString &path)
{
    static const QString marker = QStringLiteral("/steamapps/common/");
    const QString p = normalizedPath(path);
    const int at = p.indexOf(marker, 0, Qt::CaseInsensitive);
    if (at < 0)
        return {};
    const int start = at + marker.size();
    const int end = p.indexOf(QLatin1Char('/'), start);
    const QString dir = p.mid(start, end < 0 ? -1 : end - start).toLower();
    if (dir.isEmpty())
        return {};
    refresh();
    return m_nameByDir.value(dir);
}

// ---------------------------------------------------------------------------

Catalog Catalog::builtIn()
{
    // Well-known executables whose Steam id is not always in reach (a game
    // started outside Steam, or by Lutris/Heroic). Kept short on purpose:
    // the user's own entries are the long tail.
    Catalog c;
    const QList<QPair<QString, QString>> list = {
        { QStringLiteral("factorio"), QStringLiteral("Factorio") },
        { QStringLiteral("cs2"), QStringLiteral("Counter-Strike 2") },
        { QStringLiteral("cs2.exe"), QStringLiteral("Counter-Strike 2") },
        { QStringLiteral("dota2"), QStringLiteral("Dota 2") },
        { QStringLiteral("dota2.exe"), QStringLiteral("Dota 2") },
        { QStringLiteral("valheim.x86_64"), QStringLiteral("Valheim") },
        { QStringLiteral("valheim.exe"), QStringLiteral("Valheim") },
        { QStringLiteral("hades2.exe"), QStringLiteral("Hades II") },
        { QStringLiteral("hades.exe"), QStringLiteral("Hades") },
        { QStringLiteral("eldenring.exe"), QStringLiteral("Elden Ring") },
        { QStringLiteral("rocketleague.exe"), QStringLiteral("Rocket League") },
        { QStringLiteral("cyberpunk2077.exe"), QStringLiteral("Cyberpunk 2077") },
        { QStringLiteral("witcher3.exe"), QStringLiteral("The Witcher 3") },
        { QStringLiteral("overwatch.exe"), QStringLiteral("Overwatch 2") },
        { QStringLiteral("valorant-win64-shipping.exe"), QStringLiteral("VALORANT") },
        { QStringLiteral("league of legends.exe"), QStringLiteral("League of Legends") },
        { QStringLiteral("fortniteclient-win64-shipping.exe"), QStringLiteral("Fortnite") },
        { QStringLiteral("genshinimpact.exe"), QStringLiteral("Genshin Impact") },
        { QStringLiteral("terraria.exe"), QStringLiteral("Terraria") },
        { QStringLiteral("minecraftlauncher.exe"), QStringLiteral("Minecraft") },
        { QStringLiteral("stardewvalley"), QStringLiteral("Stardew Valley") },
        { QStringLiteral("stardew valley.exe"), QStringLiteral("Stardew Valley") },
    };
    for (const auto &e : list)
        c.exeToName.insert(e.first, e.second);
    return c;
}

void Catalog::addUserEntries(const QStringList &entries)
{
    for (const QString &raw : entries) {
        const int eq = raw.indexOf(QLatin1Char('='));
        const QString exe = (eq < 0 ? raw : raw.left(eq)).trimmed().toLower();
        QString name = (eq < 0 ? raw : raw.mid(eq + 1)).trimmed();
        if (exe.isEmpty() || name.isEmpty())
            continue;
        exeToName.insert(baseName(exe), name);
    }
}

// ---------------------------------------------------------------------------

#ifndef Q_OS_WIN
QList<ProcessInfo> scanProcRoot(const QString &procRoot, int maxProcesses)
{
    QList<ProcessInfo> out;
    const QStringList entries = QDir(procRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    static const QRegularExpression digits(QStringLiteral("^\\d+$"));
    // entryList is lexical ("10" < "9"): order by pid BEFORE the cap, so the
    // cap drops the newest processes and not an arbitrary set.
    QList<QPair<qint64, QString>> numbered;
    for (const QString &n : entries)
        if (digits.match(n).hasMatch())
            numbered.append({ n.toLongLong(), n });
    std::sort(numbered.begin(), numbered.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[pidNumber, n] : std::as_const(numbered)) {
        Q_UNUSED(pidNumber)
        if (out.size() >= maxProcesses)
            break;
        const QString dir = procRoot + QLatin1Char('/') + n;
        ProcessInfo p;
        p.pid = n.toLongLong();
        const QByteArray cmd = readBounded(dir + QStringLiteral("/cmdline"));
        if (!cmd.isEmpty()) {
            for (const QByteArray &part : cmd.split('\0'))
                if (!part.isEmpty())
                    p.args.append(QString::fromUtf8(part));
        }
        p.exePath = QFileInfo(dir + QStringLiteral("/exe")).symLinkTarget();
        // The environment is the only place a game launched by a wrapper
        // names its Steam id, but reading every process's is needless:
        // only a process that lives in a Steam library or runs a Windows
        // executable can be a Steam game's own.
        const QString joined = normalizedPath(p.exePath + QLatin1Char(' ') + p.args.join(QLatin1Char(' ')));
        if (joined.contains(QLatin1String("/steamapps/common/"), Qt::CaseInsensitive)
            || joined.contains(QLatin1String(".exe"), Qt::CaseInsensitive)) {
            const QList<QByteArray> env = readBounded(dir + QStringLiteral("/environ")).split('\0');
            for (const QByteArray &kv : env) {
                if (kv.startsWith("SteamAppId=")) {
                    p.steamAppIdEnv = QString::fromUtf8(kv.mid(11)).trimmed();
                    break;
                }
            }
        }
        if (p.args.isEmpty() && p.exePath.isEmpty())
            continue;   // kernel thread or unreadable: nothing to match
        out.append(p);
    }
    std::sort(out.begin(), out.end(),
              [](const ProcessInfo &a, const ProcessInfo &b) { return a.pid < b.pid; });
    return out;
}
#else
QList<ProcessInfo> scanProcRoot(const QString &, int) { return scanProcessesWindows(); }

QList<ProcessInfo> scanProcessesWindows()
{
    QList<ProcessInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    PROCESSENTRY32W e;
    e.dwSize = sizeof(e);
    if (Process32FirstW(snap, &e)) {
        do {
            ProcessInfo p;
            p.pid = e.th32ProcessID;
            p.exePath = QString::fromWCharArray(e.szExeFile);
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
            if (h) {
                wchar_t buf[MAX_PATH * 2];
                DWORD len = DWORD(sizeof(buf) / sizeof(buf[0]));
                if (QueryFullProcessImageNameW(h, 0, buf, &len))
                    p.exePath = QString::fromWCharArray(buf, int(len));
                CloseHandle(h);
            }
            out.append(p);
        } while (out.size() < 8192 && Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return out;
}

QString foregroundFullscreenExePath()
{
    HWND w = GetForegroundWindow();
    if (!w)
        return {};
    RECT r;
    if (!GetWindowRect(w, &r))
        return {};
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return {};
    if (r.left > mi.rcMonitor.left || r.top > mi.rcMonitor.top
        || r.right < mi.rcMonitor.right || r.bottom < mi.rcMonitor.bottom)
        return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD len = DWORD(sizeof(buf) / sizeof(buf[0]));
    QString path;
    if (QueryFullProcessImageNameW(h, 0, buf, &len))
        path = QString::fromWCharArray(buf, int(len));
    CloseHandle(h);
    return path;
}
#endif

// ---------------------------------------------------------------------------

QStringList detectGames(const QList<ProcessInfo> &procs, SteamCatalog &steam,
                        const Catalog &catalog)
{
    steam.beginBatch();
    struct BatchEnd {
        SteamCatalog &c;
        ~BatchEnd() { c.endBatch(); }
    } batchEnd{ steam };
    QStringList names;
    auto add = [&names](const QString &n) {
        if (!n.isEmpty() && !names.contains(n))
            names.append(n);
    };
    for (const ProcessInfo &p : procs) {
        // Steam's per-game launcher: "reaper SteamLaunch AppId=<id> -- ...".
        // It outlives nothing: it exits with the game.
        if (p.args.contains(QLatin1String("SteamLaunch"))) {
            for (const QString &a : p.args) {
                if (a.startsWith(QLatin1String("AppId="))) {
                    add(steam.nameForAppId(a.mid(6).trimmed()));
                    break;
                }
            }
            continue;
        }
        if (!p.steamAppIdEnv.isEmpty()) {
            const QString n = steam.nameForAppId(p.steamAppIdEnv);
            if (!n.isEmpty()) {
                add(n);
                continue;
            }
        }
        // A binary under steamapps/common/<installdir>/ (Windows, or a
        // native game started without the reaper).
        const QString byPath = steam.nameForPath(p.exePath);
        if (!byPath.isEmpty()) {
            add(byPath);
            continue;
        }
        // Built-in and user entries, by executable name: argv[0], the exe
        // symlink, and any Windows executable a wrapper (wine/proton) runs.
        QStringList candidates;
        candidates << baseName(p.exePath);
        if (!p.args.isEmpty())
            candidates << baseName(p.args.first());
        for (const QString &a : p.args)
            if (a.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive))
                candidates << baseName(a);
        for (const QString &c : candidates) {
            const QString n = catalog.exeToName.value(c.toLower());
            if (!n.isEmpty()) {
                add(n);
                break;
            }
        }
    }
    return names;
}

QString knownGameForExePath(const QString &exePath, SteamCatalog &steam,
                           const Catalog &catalog)
{
    if (exePath.isEmpty())
        return {};
    steam.beginBatch();
    const QString byPath = steam.nameForPath(exePath);
    steam.endBatch();
    if (!byPath.isEmpty())
        return byPath;
    return catalog.exeToName.value(baseName(exePath).toLower());
}

QStringList preferForeground(QStringList names, const QString &foregroundGame)
{
    const int at = foregroundGame.isEmpty() ? -1 : names.indexOf(foregroundGame);
    if (at > 0)
        names.move(at, 0);
    return names;
}

QString statusTextFor(const QString &gameName)
{
    QString clean;
    for (const QChar c : gameName)
        if (c.category() != QChar::Other_Control)
            clean.append(c);
    const QString n = clean.simplified().left(120);
    return n.isEmpty() ? QString()
                       : QCoreApplication::translate("GameActivity", "Playing %1").arg(n);
}

bool isHidden(const QString &gameName, const QStringList &hideList)
{
    const QString n = gameName.trimmed();
    for (const QString &h : hideList)
        if (h.trimmed().compare(n, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

bool detectionSupported()
{
#ifdef Q_OS_MACOS
    return false;
#else
    return true;
#endif
}

bool detectionLimitedBySandbox()
{
#ifdef Q_OS_LINUX
    return QFileInfo::exists(QStringLiteral("/.flatpak-info"))
        || !qEnvironmentVariableIsEmpty("SNAP");
#else
    return false;
#endif
}

}   // namespace gamedetect
