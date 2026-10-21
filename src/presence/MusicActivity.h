#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

#include "presence/MusicDetection.h"
#include "presence/MusicSource.h"

class PresenceManager;
class SettingsManager;

// "Listening to Spotify" (Labs, off by default): Discord's Spotify activity,
// reduced to the one line Matrix presence carries. A platform MusicSource
// reports what Spotify plays on THIS computer; this class decides what, and
// when, goes to PresenceManager, which owns the only presence publisher.
//
// Rules (docs: the vault note Tasks/2026-10-20-musicact.md):
// - Active only while Labs is on AND presence sharing is on AND a session is
//   live AND not quitting. Inactive = no source exists (no D-Bus match, no
//   timer) and the line is withdrawn at once.
// - A new track is adopted after it has been playing kSettleMs unchanged, so
//   skipping through songs publishes none of the ones skipped.
// - Pause, stop, Spotify quitting or an advertisement withdraw the line after
//   kPauseGraceMs, so a quick pause/resume changes nothing.
// - A track change never forces a presence PUT: it rides the next keep-alive
//   (CLAUDE.md §16: rc_presence is per user, burst 1). Withdrawals the user
//   asked for (turning it off, quitting) publish at once.
// - Only the title and the artist leave this machine. The album is local,
//   for the Settings preview.
class MusicActivity : public QObject
{
    Q_OBJECT
    // False where this build cannot see Spotify (macOS, Snap, no QtDBus).
    Q_PROPERTY(bool supported READ supported CONSTANT)
    // A track is adopted (what the line says, when the line is shown).
    Q_PROPERTY(bool listening READ listening NOTIFY nowPlayingChanged)
    Q_PROPERTY(QString title READ title NOTIFY nowPlayingChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY nowPlayingChanged)
    Q_PROPERTY(QString album READ album NOTIFY nowPlayingChanged)
    // The line other people read while it is shown ("" none), exactly as
    // PresenceManager holds it (markup-like text neutralised).
    Q_PROPERTY(QString statusLine READ statusLine NOTIFY nowPlayingChanged)
    // True when the line is what presence actually carries now; false while a
    // game or the user's own status takes the one line (Discord's order).
    Q_PROPERTY(bool shownAsStatus READ shownAsStatus NOTIFY shownAsStatusChanged)

public:
    explicit MusicActivity(QObject *parent = nullptr);
    ~MusicActivity() override;

    void setSettings(SettingsManager *settings);
    void setPresence(PresenceManager *presence);

    bool supported() const { return m_supported; }
    bool listening() const { return !m_current.title.isEmpty(); }
    QString title() const { return m_current.title; }
    QString artist() const { return musicdetect::artistLine(m_current); }
    QString album() const { return m_current.album; }
    // As published: PresenceManager's copy, after its markup neutralising.
    QString statusLine() const;
    bool shownAsStatus() const;

    // The single gate for source, timers and publishing. Includes the
    // presence backend working at all (supported, not refused by the server).
    bool active() const;
    // Quit: withdraw the line while the client can still send, and never
    // watch again. Called before the sync loop stops.
    void shutdown();
    bool sourceRunning() const { return m_source != nullptr; }
    int sourcesCreated() const { return m_sourcesCreated; }

    // Test seams.
    using SourceFactory = std::function<std::unique_ptr<MusicSource>()>;
    void setSourceFactoryForTest(SourceFactory factory, bool supported = true);
    void setSettleForTest(int ms) { m_settle.setInterval(ms); }
    void setPauseGraceForTest(int ms) { m_grace.setInterval(ms); }

    static constexpr int kSettleMs = 3 * 1000;
    static constexpr int kPauseGraceMs = 10 * 1000;

Q_SIGNALS:
    void nowPlayingChanged();
    void shownAsStatusChanged();

protected:
    // The published line is in the app's language: recomposed when it changes
    // (Qt sends LanguageChange to the application object only).
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reconfigure();
    void onSourceChanged();
    void adopt(const musicdetect::NowPlaying &np, bool publishNow);
    void refreshShown();

    QPointer<SettingsManager> m_settings;
    QPointer<PresenceManager> m_presence;
    std::unique_ptr<MusicSource> m_source;
    SourceFactory m_factory;
    bool m_supported = false;
    bool m_shutdown = false;
    int m_sourcesCreated = 0;
    QTimer m_settle;
    QTimer m_grace;
    musicdetect::NowPlaying m_current;   // adopted ("" title = none)
    musicdetect::NowPlaying m_pending;   // waiting out kSettleMs
    bool m_shown = false;
};
