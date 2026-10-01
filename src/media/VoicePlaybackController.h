#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <memory>

class MediaBridge;
class MediaPlaybackController;
class QAudioOutput;
class QMediaPlayer;
class SettingsManager;

// The one app-owned player for voice messages and audio files.
//
// Timeline rows are views: they are destroyed and rebuilt on every room switch,
// so a player inside a row cannot survive one. This owns the QMediaPlayer, the
// decrypted playable file's pin and the in-flight fetch, and rows (and the
// floating mini-player shown while the clip's room is not on screen,
// qml/VoiceMiniPlayer.qml) bind to it by event id.
//
// A room switch leaves it alone. Sign-out, account switch, shutdown and a
// redaction of the playing event stop it (AppController). A call going live
// pauses it. The end of a clip clears it, so the bar goes away and the row
// returns to idle.
//
// Media still arrives only through MediaBridge's validated playable path: the
// source is the session-scoped temp file it materialised, never a homeserver
// URL (CLAUDE.md §6). Logs carry no event ids, room ids or media keys.
class VoicePlaybackController : public QObject
{
    Q_OBJECT
    // Forward-declared here so AppController.h does not pull in Qt Multimedia.
    Q_MOC_INCLUDE(<QAudioOutput>)
    Q_MOC_INCLUDE(<QMediaPlayer>)
    // The playing (or paused, or fetching) clip's identity. Empty when idle.
    Q_PROPERTY(QString eventId READ eventId NOTIFY currentChanged)
    Q_PROPERTY(QString roomId READ roomId NOTIFY currentChanged)
    Q_PROPERTY(QString roomName READ roomName NOTIFY currentChanged)
    // The thread the clip was played from, or "" for the main timeline.
    Q_PROPERTY(QString threadRootId READ threadRootId NOTIFY currentChanged)
    Q_PROPERTY(QString senderName READ senderName NOTIFY currentChanged)
    // For the mini-player's avatar: the sender's user id (colour key) and
    // avatar mxc, fetched through the media bridge like every avatar.
    Q_PROPERTY(QString senderId READ senderId NOTIFY currentChanged)
    Q_PROPERTY(QString senderAvatarMxc READ senderAvatarMxc
                   NOTIFY currentChanged)
    Q_PROPERTY(QString mediaKey READ mediaKey NOTIFY currentChanged)
    Q_PROPERTY(QString filename READ filename NOTIFY currentChanged)
    Q_PROPERTY(bool isVoice READ isVoice NOTIFY currentChanged)
    // A clip is loaded: playing, paused, fetching or failed. The bar shows
    // exactly while this is true.
    Q_PROPERTY(bool active READ active NOTIFY currentChanged)
    // The player holds the clip's file (the fetch is done).
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    // "idle", "fetching" or "failed".
    Q_PROPERTY(QString fetchState READ fetchState NOTIFY fetchStateChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    // The decoder's duration once known, else the event's declared one.
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY seekableChanged)
    // The key this player holds audibility under in app.playback.
    Q_PROPERTY(QString ownerKey READ ownerKey NOTIFY currentChanged)
    // For metadata (cover art) and the shared volume control. Created on the
    // first play, so a session that never plays pays no multimedia start-up.
    Q_PROPERTY(QMediaPlayer *player READ player NOTIFY playerChanged)
    Q_PROPERTY(QAudioOutput *audioOutput READ audioOutput NOTIFY playerChanged)

public:
    using SourceResolver = std::function<QString(const QString &mediaKey)>;
    using RoomNameResolver = std::function<QString(const QString &roomId)>;

    explicit VoicePlaybackController(QObject *parent = nullptr);
    ~VoicePlaybackController() override;

    void setMediaBridge(MediaBridge *bridge);
    void setAudibility(MediaPlaybackController *playback);
    void setSettings(SettingsManager *settings);
    void setRoomNameResolver(RoomNameResolver resolver);
    // Tests: answer playable sources without a homeserver. Production asks
    // MediaBridge::playableSource().
    void setSourceResolverForTest(SourceResolver resolver);

    QString eventId() const { return m_eventId; }
    QString roomId() const { return m_roomId; }
    QString roomName() const { return m_roomName; }
    QString threadRootId() const { return m_threadRootId; }
    QString senderName() const { return m_senderName; }
    QString senderId() const { return m_senderId; }
    QString senderAvatarMxc() const { return m_senderAvatarMxc; }
    QString mediaKey() const { return m_mediaKey; }
    QString filename() const { return m_filename; }
    bool isVoice() const { return m_isVoice; }
    bool active() const { return !m_eventId.isEmpty(); }
    bool loaded() const { return m_loaded; }
    bool playing() const;
    QString fetchState() const { return m_fetchState; }
    qint64 position() const;
    qint64 duration() const;
    bool seekable() const;
    QString ownerKey() const;
    QMediaPlayer *player() const { return m_player.get(); }
    QAudioOutput *audioOutput() const { return m_output.get(); }

    // True when `eventId` is the loaded clip.
    Q_INVOKABLE bool isCurrent(const QString &eventId) const;
    // Start `eventId`. Another clip stops first. Playing the loaded clip again
    // resumes it (or retries a failed fetch). `meta` keys: senderName,
    // senderId, senderAvatarMxc, filename, isVoice, durationMs, threadRootId,
    // roomName.
    Q_INVOKABLE void play(const QString &eventId, const QString &roomId,
                          const QString &mediaKey, const QVariantMap &meta);
    // The row's Play/Pause button: pause the loaded clip if it is playing,
    // otherwise play (or resume) it.
    Q_INVOKABLE void toggle(const QString &eventId, const QString &roomId,
                            const QString &mediaKey, const QVariantMap &meta);
    // The bar's Play/Pause button.
    Q_INVOKABLE void togglePlayPause();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void seek(qint64 positionMs);
    // Unload the clip, release its file and fetch, clear the bar.
    Q_INVOKABLE void stop();

    // After app.playback.stopAll() on a room switch, hold audibility again so
    // the media key (Space) still reaches a clip that kept playing.
    void reclaimAudibility();
    // Stops when `eventId` in `roomId` is the loaded clip.
    void handleRedaction(const QString &roomId, const QString &eventId);

Q_SIGNALS:
    void currentChanged();
    void loadedChanged();
    void playingChanged();
    void fetchStateChanged();
    void positionChanged();
    void durationChanged();
    void seekableChanged();
    void playerChanged();

private:
    void ensurePlayer();
    void startOrFetch();
    void startSource(const QString &url);
    void setFetchState(const QString &state);
    void setLoaded(bool loaded);
    void cancelFetch();
    void unpin();
    void applySettings();
    void onAudibleOwnerChanged();
    void onToggleRequested(const QString &ownerKey);
    void onPlayableReady(const QString &cacheKey);
    void onFetchFailed(const QString &cacheKey);
    void onMediaStatus(int status);
    QString cacheKey() const;

    QPointer<MediaBridge> m_bridge;
    QPointer<MediaPlaybackController> m_audibility;
    QPointer<SettingsManager> m_settings;
    RoomNameResolver m_roomNameResolver;
    SourceResolver m_testSourceResolver;

    // A VoiceAudioOutput (VoicePlaybackController.cpp).
    std::unique_ptr<QAudioOutput> m_output;
    std::unique_ptr<QMediaPlayer> m_player;

    QString m_eventId;
    QString m_roomId;
    QString m_roomName;
    QString m_threadRootId;
    QString m_senderName;
    QString m_senderId;
    QString m_senderAvatarMxc;
    QString m_mediaKey;
    QString m_filename;
    bool m_isVoice = false;
    qint64 m_declaredDurationMs = 0;
    bool m_loaded = false;
    QString m_fetchState = QStringLiteral("idle");
    // The media key with an outstanding fetch, and the one pinned against LRU
    // eviction while the player holds its file open.
    QString m_fetchingKey;
    QString m_pinnedKey;
};
