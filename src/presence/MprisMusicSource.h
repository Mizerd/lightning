#pragma once

// Linux: what Spotify plays, from its MPRIS interface on the session bus.
// Only built with QtDBus (HAVE_QT_DBUS); src/presence/MusicSource.cpp picks it.
#ifdef HAVE_QT_DBUS

#include <QDBusConnection>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include "presence/MusicSource.h"

class QDBusServiceWatcher;

// Watches exactly ONE bus name (Spotify's) and reads only the Player
// interface's PlaybackStatus and Metadata. Every call to the PLAYER is
// asynchronous, so a slow or hung player cannot block the GUI thread. start()
// does make blocking round trips to the BUS DAEMON (the service watcher's and
// QDBusConnection::connect()'s AddMatch/GetNameOwner), as every QtDBus
// subscription does; the daemon answers those itself. Signal driven
// (PropertiesChanged, owner changes), with a slow safety poll because players
// do not all emit reliably; the poll is skipped while the name has no owner.
//
// In the Flatpak this needs `--talk-name=org.mpris.MediaPlayer2.spotify`, which
// the manifest does NOT carry (Flathub review): a user may grant it themselves;
// without it the proxy hides the name and the source simply sees nothing.
class MprisMusicSource : public MusicSource
{
    Q_OBJECT
public:
    // The user's session bus and Spotify's name.
    explicit MprisMusicSource(QObject *parent = nullptr);
    // Test seam: a private bus and a fake player's name.
    MprisMusicSource(const QDBusConnection &bus, const QString &service,
                     QObject *parent = nullptr);
    ~MprisMusicSource() override;

    void start() override;
    void stop() override;
    musicdetect::NowPlaying nowPlaying() const override { return m_now; }

    bool running() const { return m_running; }
    void setPollIntervalForTest(int ms) { m_poll.setInterval(ms); }
    // GetAll calls sent since construction (a test proves "stopped = silent").
    int requestsSent() const { return m_requests; }

private Q_SLOTS:
    void onPropertiesChanged(const QString &interfaceName,
                             const QVariantMap &changed,
                             const QStringList &invalidated);

private:
    void refresh();
    void applyProperties(const QVariantMap &props, bool replace);
    void recompute();

    static constexpr int kPollIntervalMs = 15 * 1000;
    static constexpr int kCallTimeoutMs = 2000;

    QDBusConnection m_bus;
    QString m_service;
    QDBusServiceWatcher *m_watcher = nullptr;
    QTimer m_poll;
    bool m_running = false;
    bool m_subscribed = false;
    // False after the bus said the name has no owner; true again when an
    // owner appears or the player signals. Unknown at start reads as true.
    bool m_hasOwner = true;
    int m_generation = 0;
    int m_requests = 0;
    QString m_status;
    QVariantMap m_metadata;
    musicdetect::NowPlaying m_now;
};

#endif   // HAVE_QT_DBUS
