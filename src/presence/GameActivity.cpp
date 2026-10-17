#include "presence/GameActivity.h"

#include <QMetaObject>
#include <QThreadPool>

#include "app/SettingsManager.h"
#include "presence/PresenceManager.h"

GameActivity::GameActivity(QObject *parent)
    : QObject(parent)
    , m_steam(std::make_shared<gamedetect::SteamCatalog>(
          gamedetect::SteamCatalog::defaultRoots()))
{
    m_timer.setInterval(kPollIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &GameActivity::startScan);
}

GameActivity::~GameActivity()
{
    // A queued result addresses this object through a QPointer guard; the
    // generation bump also makes any inline path a no-op.
    ++m_generation;
}

void GameActivity::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = settings;
    if (m_settings) {
        connect(m_settings, &SettingsManager::gameActivityChanged, this,
                &GameActivity::reconfigure);
        connect(m_settings, &SettingsManager::sharePresenceChanged, this,
                &GameActivity::reconfigure);
    }
    reconfigure();
}

void GameActivity::setPresence(PresenceManager *presence)
{
    if (m_presence)
        disconnect(m_presence, nullptr, this, nullptr);
    m_presence = presence;
    if (m_presence) {
        connect(m_presence, &PresenceManager::sessionLiveChanged, this,
                &GameActivity::reconfigure);
        m_presence->setGameActivity(m_current);
    }
    reconfigure();
}

void GameActivity::shutdown()
{
    m_shutdown = true;
    reconfigure();
}

bool GameActivity::active() const
{
    return !m_shutdown && m_settings && m_presence && m_presence->sessionLive()
        && m_settings->gameActivityEnabled() && m_settings->sharePresence();
}

void GameActivity::reconfigure()
{
    if (!active()) {
        // Off: stop everything and take the name back at once.
        ++m_generation;
        m_timer.stop();
        m_pending.clear();
        m_pendingSeen = 0;
        setCurrent({});
        return;
    }
    // Hiding the current game must act immediately, not at the next scan.
    const QStringList hidden = m_settings->gameActivityHidden();
    if (!m_current.isEmpty() && gamedetect::isHidden(m_current, hidden))
        setCurrent({});
    if (!m_timer.isActive()) {
        m_timer.start();
        startScan();
    }
    // A changed custom list is applied to the next scan.
}

void GameActivity::rescanNow()
{
    if (active())
        startScan();
}

void GameActivity::startScan()
{
    if (!active() || m_inFlight)
        return;
    m_inFlight = true;
    ++m_scansStarted;
    const int gen = m_generation;
    const QString procRoot = m_procRoot;
    gamedetect::Catalog catalog = gamedetect::Catalog::builtIn();
    catalog.addUserEntries(m_settings->gameActivityCustom());
    const std::shared_ptr<gamedetect::SteamCatalog> steam = m_steam;
    QPointer<GameActivity> self(this);

    auto work = [procRoot, catalog, steam]() -> QStringList {
        const auto procs = gamedetect::scanProcRoot(procRoot);
        QStringList names = gamedetect::detectGames(procs, *steam, catalog);
#ifdef Q_OS_WIN
        // Fullscreen only confirms which known game is in front.
        if (names.size() > 1)
            names = gamedetect::preferForeground(
                names,
                gamedetect::knownGameForExePath(
                    gamedetect::foregroundFullscreenExePath(), *steam, catalog));
#endif
        return names;
    };
    auto deliver = [self, gen](const QStringList &names) {
        if (!self)
            return;
        self->m_inFlight = false;
        if (gen != self->m_generation || !self->active())
            return;   // disabled or reconfigured while the scan ran
        self->applyDetected(names);
    };

    if (m_synchronous) {
        deliver(work());
        return;
    }
    QThreadPool::globalInstance()->start([work, deliver, self]() {
        const QStringList names = work();
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(), [deliver, names]() { deliver(names); },
            Qt::QueuedConnection);
    });
}

void GameActivity::applyDetected(const QStringList &names)
{
    if (!active())
        return;
    const QStringList hidden = m_settings->gameActivityHidden();
    QString candidate;
    for (const QString &n : names) {
        if (!gamedetect::isHidden(n, hidden)) {
            candidate = n;
            break;
        }
    }
    // A game that is still running keeps the status even when another
    // appears first in the list.
    if (!m_current.isEmpty() && names.contains(m_current)
        && !gamedetect::isHidden(m_current, hidden))
        candidate = m_current;
    if (candidate == m_current) {
        m_pending.clear();
        m_pendingSeen = 0;
        return;
    }
    if (candidate == m_pending) {
        ++m_pendingSeen;
    } else {
        m_pending = candidate;
        m_pendingSeen = 1;
    }
    if (m_pendingSeen >= m_confirmScans) {
        m_pending.clear();
        m_pendingSeen = 0;
        setCurrent(candidate);
    }
}

void GameActivity::setCurrent(const QString &game)
{
    if (m_current == game)
        return;
    m_current = game;
    if (m_presence)
        m_presence->setGameActivity(game);
    Q_EMIT currentGameChanged();
}
