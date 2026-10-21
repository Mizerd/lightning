#include "presence/MusicActivity.h"

#include <QCoreApplication>
#include <QEvent>

#include "app/SettingsManager.h"
#include "presence/PresenceManager.h"

using musicdetect::NowPlaying;

MusicActivity::MusicActivity(QObject *parent)
    : QObject(parent)
    , m_supported(musicdetect::detectionSupported())
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(kSettleMs);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        if (active() && m_pending.showable())
            adopt(m_pending, false);
        m_pending = {};
    });
    m_grace.setSingleShot(true);
    m_grace.setInterval(kPauseGraceMs);
    connect(&m_grace, &QTimer::timeout, this, [this] { adopt({}, false); });
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(this);
}

bool MusicActivity::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::LanguageChange
        && watched == QCoreApplication::instance() && listening() && m_presence) {
        // Rides the next keep-alive like any other change of the line.
        m_presence->setMusicActivity(musicdetect::statusTextFor(m_current));
        Q_EMIT nowPlayingChanged();
        refreshShown();
    }
    return QObject::eventFilter(watched, event);
}

MusicActivity::~MusicActivity()
{
    if (m_source)
        m_source->stop();
}

void MusicActivity::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = settings;
    if (m_settings) {
        connect(m_settings, &SettingsManager::musicActivityChanged, this,
                &MusicActivity::reconfigure);
        connect(m_settings, &SettingsManager::sharePresenceChanged, this,
                &MusicActivity::reconfigure);
    }
    reconfigure();
}

void MusicActivity::setPresence(PresenceManager *presence)
{
    if (m_presence)
        disconnect(m_presence, nullptr, this, nullptr);
    m_presence = presence;
    if (m_presence) {
        connect(m_presence, &PresenceManager::sessionLiveChanged, this,
                &MusicActivity::reconfigure);
        // No presence backend, or a server that refuses presence: nobody can
        // read the line, so nothing is watched.
        connect(m_presence, &PresenceManager::activeChanged, this,
                &MusicActivity::reconfigure);
        connect(m_presence, &PresenceManager::supportedChanged, this,
                &MusicActivity::reconfigure);
        // A game or the user's own status can take the line, or give it back.
        connect(m_presence, &PresenceManager::ownStatusChanged, this,
                &MusicActivity::refreshShown);
        m_presence->setMusicActivity(musicdetect::statusTextFor(m_current));
    }
    reconfigure();
}

void MusicActivity::setSourceFactoryForTest(SourceFactory factory, bool supported)
{
    m_factory = std::move(factory);
    m_supported = supported;
    reconfigure();
}

bool MusicActivity::active() const
{
    return !m_shutdown && m_supported && m_settings && m_presence
        && m_presence->active() && m_presence->sessionLive()
        && m_settings->musicActivityEnabled()
        && m_settings->sharePresence();
}

void MusicActivity::shutdown()
{
    m_shutdown = true;
    reconfigure();
}

QString MusicActivity::statusLine() const
{
    return m_presence ? m_presence->musicActivity() : musicdetect::statusTextFor(m_current);
}

bool MusicActivity::shownAsStatus() const
{
    // Compared with PresenceManager's own copy: it neutralises markup-like
    // text, so the composed line and the published one can differ.
    return listening() && m_presence && !m_presence->musicActivity().isEmpty()
        && m_presence->effectiveStatusText() == m_presence->musicActivity();
}

void MusicActivity::reconfigure()
{
    if (!active()) {
        // Off, sharing off, signed out or quitting: no source, no timers, and
        // the line is withdrawn now, not after a grace period.
        m_settle.stop();
        m_grace.stop();
        m_pending = {};
        if (m_source) {
            disconnect(m_source.get(), nullptr, this, nullptr);
            m_source->stop();
            m_source.reset();
        }
        adopt({}, true);
        return;
    }
    if (m_source)
        return;
    m_source = m_factory ? m_factory() : createPlatformMusicSource();
    if (!m_source)
        return;
    ++m_sourcesCreated;
    connect(m_source.get(), &MusicSource::nowPlayingChanged, this,
            &MusicActivity::onSourceChanged);
    m_source->start();
    onSourceChanged();   // a source may already know
}

void MusicActivity::onSourceChanged()
{
    if (!active() || !m_source)
        return;
    const NowPlaying np = m_source->nowPlaying();
    if (np.showable()) {
        m_grace.stop();
        if (musicdetect::sameTrack(np, m_current)) {
            // Resumed, or a repeat report: nothing waits any more.
            m_settle.stop();
            m_pending = {};
            return;
        }
        if (m_pending.showable() && musicdetect::sameTrack(np, m_pending))
            return;   // already settling on this one
        // A new track restarts the settle, so skipping through songs adopts
        // only the one the user stays on.
        m_pending = np;
        m_settle.start();
        return;
    }
    // Paused, stopped, gone, or an advertisement.
    m_settle.stop();
    m_pending = {};
    if (listening() && !m_grace.isActive())
        m_grace.start();
}

void MusicActivity::adopt(const NowPlaying &np, bool publishNow)
{
    const bool changed = !(np == m_current);
    m_current = np;
    if (m_presence)
        m_presence->setMusicActivity(musicdetect::statusTextFor(m_current), publishNow);
    if (changed)
        Q_EMIT nowPlayingChanged();
    refreshShown();
}

void MusicActivity::refreshShown()
{
    const bool shown = shownAsStatus();
    if (shown == m_shown)
        return;
    m_shown = shown;
    Q_EMIT shownAsStatusChanged();
}
