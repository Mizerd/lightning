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

class MusicActivity : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool listening READ listening NOTIFY nowPlayingChanged)
    Q_PROPERTY(QString title READ title NOTIFY nowPlayingChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY nowPlayingChanged)
    Q_PROPERTY(QString album READ album NOTIFY nowPlayingChanged)
    Q_PROPERTY(QString statusLine READ statusLine NOTIFY nowPlayingChanged)
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
    QString statusLine() const;
    bool shownAsStatus() const;

    bool active() const;
    void shutdown();
    bool sourceRunning() const { return m_source != nullptr; }
    int sourcesCreated() const { return m_sourcesCreated; }

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
    musicdetect::NowPlaying m_current;
    musicdetect::NowPlaying m_pending;
    bool m_shown = false;
};
