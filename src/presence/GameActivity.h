#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

#include "presence/GameDetection.h"

class PresenceManager;
class SettingsManager;

// "Playing X" game activity (Labs, off by default). A low-frequency timer
// scans the process table on a worker thread, maps a game to a name, filters
// the user's hide list and hands the NAME to PresenceManager, which owns the
// only presence publisher. Nothing runs while the feature is off or while
// presence sharing is off: no timer, no scan, no publish.
//
// A change is adopted only after it is seen by two consecutive scans, so a
// launcher that spawns and exits helper processes cannot flap the status;
// hiding the current game, turning the feature off or turning sharing off
// clear it at once.
class GameActivity : public QObject
{
    Q_OBJECT
    // The game currently published ("" none), after the hide list.
    Q_PROPERTY(QString currentGame READ currentGame NOTIFY currentGameChanged)
    // Detection is partial: the Flatpak and Snap sandboxes cannot see host
    // processes.
    Q_PROPERTY(bool limitedDetection READ limitedDetection CONSTANT)
    // False on platforms with no process-table access in this build (macOS).
    Q_PROPERTY(bool detectionSupported READ detectionSupported CONSTANT)

public:
    explicit GameActivity(QObject *parent = nullptr);
    ~GameActivity() override;

    void setSettings(SettingsManager *settings);
    void setPresence(PresenceManager *presence);

    QString currentGame() const { return m_current; }
    bool limitedDetection() const { return gamedetect::detectionLimitedBySandbox(); }
    bool detectionSupported() const { return gamedetect::detectionSupported(); }
    // Enabled AND sharing presence AND a live session AND not quitting: the
    // single predicate gating timer, scan and publish.
    bool active() const;
    // Quit: take the game back and never poll again. Called before the sync
    // loop stops so the user's own status is the last thing published.
    void shutdown();
    // True while the poll timer is running.
    bool polling() const { return m_timer.isActive(); }
    // Scans started since construction (a test reads it to prove "off = no
    // scan").
    int scansStarted() const { return m_scansStarted; }

    Q_INVOKABLE void rescanNow();

    // Test seams: fixture roots, fast cadence, inline scan, one-scan debounce.
    void setProcRootForTest(const QString &root) { m_procRoot = root; }
    void setSteamRootsForTest(const QStringList &roots)
    {
        m_steam->setRoots(roots);
    }
    void setPollIntervalForTest(int ms) { m_timer.setInterval(ms); }
    void setSynchronousForTest(bool on) { m_synchronous = on; }
    void setConfirmScansForTest(int n) { m_confirmScans = n; }

Q_SIGNALS:
    void currentGameChanged();

private:
    void reconfigure();
    void startScan();
    void applyDetected(const QStringList &names);
    void setCurrent(const QString &game);

    static constexpr int kPollIntervalMs = 15 * 1000;

    QPointer<SettingsManager> m_settings;
    QPointer<PresenceManager> m_presence;
    QTimer m_timer;
    QString m_current;
    QString m_pending;
    int m_pendingSeen = 0;
    int m_confirmScans = 2;
    bool m_shutdown = false;
    bool m_inFlight = false;
    int m_generation = 0;
    int m_scansStarted = 0;
    bool m_synchronous = false;
    QString m_procRoot = QStringLiteral("/proc");
    // Touched only by the one in-flight scan (serialised by m_inFlight);
    // shared so a scan outliving this object never dangles.
    std::shared_ptr<gamedetect::SteamCatalog> m_steam;
};
