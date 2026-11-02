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

class GameActivity : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentGame READ currentGame NOTIFY currentGameChanged)
    // Detection is partial: the Flatpak and Snap sandboxes cannot see host
    // processes.
    Q_PROPERTY(bool limitedDetection READ limitedDetection CONSTANT)
    Q_PROPERTY(bool detectionSupported READ detectionSupported CONSTANT)

public:
    explicit GameActivity(QObject *parent = nullptr);
    ~GameActivity() override;

    void setSettings(SettingsManager *settings);
    void setPresence(PresenceManager *presence);

    QString currentGame() const { return m_current; }
    bool limitedDetection() const { return gamedetect::detectionLimitedBySandbox(); }
    bool detectionSupported() const { return gamedetect::detectionSupported(); }
    bool active() const;
    void shutdown();
    bool polling() const { return m_timer.isActive(); }
    int scansStarted() const { return m_scansStarted; }

    Q_INVOKABLE void rescanNow();

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
    std::shared_ptr<gamedetect::SteamCatalog> m_steam;
};
