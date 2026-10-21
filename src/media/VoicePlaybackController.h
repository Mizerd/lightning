#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

class MediaBridge;
namespace lightning::media {
class QtAudioGate;
}
class MediaPlaybackController;
class QAudioOutput;
class QMediaPlayer;
class SettingsManager;

// The one app-owned player for voice messages and audio files.
//
// Timeline rows are views: they are destroyed and rebuilt on every room switch,
// so a player inside a row cannot survive one. This owns the QMediaPlayer, the
// decrypted playable file's pin and the in-flight fetch, and rows (and the
// "now playing" strip or floating card, qml/VoicePlayerBar.qml and
// qml/VoiceMiniPlayer.qml, hosted by Main.qml and TimelinePane) bind to it by
// event id.
//
// A room switch leaves it alone. Sign-out, account switch, shutdown and a
// redaction of the playing event stop it (AppController). A call going live
// pauses it, and the end of that call resumes it if nothing else touched it
// (pausedForCall). The end of a clip clears it, so the bar goes away and the
// row returns to idle; for a voice message, AppController then starts the
// next one from `upNext`, the run of voice messages that followed it in its
// OWN timeline, snapshotted while that timeline was loaded, so the chain does
// not depend on which room is on screen (GitHub #36, design-media B3).
//
// Speed is per kind: voice messages play at the remembered
// SettingsManager::voicePlaybackRate; an audio file starts at 1x and its
// speed belongs to that clip (`rate`).
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
    // The clip's MSC3245 waveform (0..100 buckets) when its row had one, for
    // the larger popped-out card. Empty otherwise; never fabricated.
    Q_PROPERTY(QVariantList waveform READ waveform NOTIFY currentChanged)
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
    // The current clip's speed: the remembered voice speed for a voice
    // message, the clip's own (1x until changed) for an audio file.
    Q_PROPERTY(qreal rate READ rate NOTIFY rateChanged)
    // The voice messages that follow the current one in its own timeline,
    // oldest first, as maps of play()'s meta plus eventId and mediaKey. Empty
    // for an audio file and at the end of a run.
    Q_PROPERTY(QVariantList upNext READ upNext NOTIFY upNextChanged)
    Q_PROPERTY(bool hasNext READ hasNext NOTIFY upNextChanged)
    // A call going live paused the clip, and nothing has touched it since:
    // the bar says why, and the end of the call resumes it.
    Q_PROPERTY(bool pausedForCall READ pausedForCall
                   NOTIFY pausedForCallChanged)
    // An audio file's embedded tags (ID3 / Vorbis comments), "" until the
    // backend has read them and for a voice message. Plain text, bounded.
    Q_PROPERTY(QString trackTitle READ trackTitle NOTIFY metadataChanged)
    Q_PROPERTY(QString trackArtist READ trackArtist NOTIFY metadataChanged)
    // The embedded cover art, as MediaBridge's RAM-only image URL, or "".
    Q_PROPERTY(QString artworkSource READ artworkSource
                   NOTIFY metadataChanged)
    // Bumped whenever clipInfo() learns something, so a row's binding over
    // clipInfo(mediaKey) re-evaluates.
    Q_PROPERTY(int clipInfoRevision READ clipInfoRevision
                   NOTIFY metadataChanged)
    // A timeline row showing the current clip is on screen (it reported so
    // through reportRowVisible). The floating card hides only then.
    Q_PROPERTY(bool rowOnScreen READ rowOnScreen NOTIFY rowOnScreenChanged)
    // One of those rows (the window's "focus the player" shortcut lands on
    // its play button when no strip or card shows), or null.
    Q_PROPERTY(QObject *visibleRow READ visibleRow NOTIFY rowOnScreenChanged)

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
    // Tests: the audio gate to ask before play() (QtAudioGate.h). Production
    // uses the process's installed gate.
    // Out of line: QPointer needs the complete type to assign.
    void setAudioGateForTest(lightning::media::QtAudioGate *gate);
    // Tests: a play() reached the player (not one still held by the gate).
    int playerStartsForTest() const { return m_playerStarts; }

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
    QVariantList waveform() const { return m_waveform; }
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
    qreal rate() const;
    QVariantList upNext() const { return m_upNext; }
    bool hasNext() const { return !m_upNext.isEmpty(); }
    bool pausedForCall() const { return m_pausedForCall; }
    QString trackTitle() const;
    QString trackArtist() const;
    QString artworkSource() const;
    int clipInfoRevision() const { return m_clipInfoRevision; }
    bool rowOnScreen() const;
    QObject *visibleRow() const;

    // True when `eventId` is the loaded clip.
    Q_INVOKABLE bool isCurrent(const QString &eventId) const;
    // Start `eventId`. Another clip stops first. Playing the loaded clip again
    // resumes it (or retries a failed fetch). `meta` keys: senderName,
    // senderId, senderAvatarMxc, filename, isVoice, durationMs, threadRootId,
    // roomName, waveform, and startAtMs (start there rather than at 0: a seek on an
    // idle row). Starting the head of `upNext` keeps the rest of the run.
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
    // Sign-out and account switch: stop, and forget what this session learned
    // about its files (clipInfo: tags and cover art).
    void clearSession();
    // Skip to the next voice message of the run (the bar's and the media
    // keys' Next). False when there is none.
    Q_INVOKABLE bool playNext();
    // The media keys' Previous: back to the start of the clip.
    Q_INVOKABLE void restart();
    // The current clip's speed (see `rate`): a voice message writes the
    // remembered voice speed, an audio file only its own.
    Q_INVOKABLE void setRate(qreal rate);
    // The shared media volume (0..1) the player follows; writing it writes
    // the remembered setting, as the in-app volume control does.
    qreal volume() const;
    Q_INVOKABLE void setVolume(qreal volume);
    // What is known about an audio file's tags and cover art from an earlier
    // play this session (RAM only): {title, artist, artworkSource}, or an
    // empty map. Rows read it at rest, so a file's art and title do not
    // appear and vanish with playback.
    Q_INVOKABLE QVariantMap clipInfo(const QString &mediaKey) const;
    // A timeline row reports whether it is on screen. `row` is the reporting
    // card (a QML item); only reports for the current clip count, a row's
    // destruction counts as hidden, and a new clip forgets every report.
    Q_INVOKABLE void reportRowVisible(QObject *row, const QString &eventId,
                                      bool visible);

    // After app.playback.stopAll() on a room switch, hold audibility again so
    // the media key (Space) still reaches the clip: a playing one, and a
    // paused one too, so Space resumes a clip paused before the switch.
    void reclaimAudibility();
    // The run that follows the current clip in its own timeline (AppController
    // resolves it). Ignored while nothing is loaded.
    void setUpNext(const QVariantList &queue);
    // A call went live: pause, and remember that the call did it.
    void pauseForCall();
    // The call ended: resume a clip the call paused, if nothing (the reader,
    // another clip, a stop) touched it since.
    void resumeAfterCall();
    // Stops when `eventId` in `roomId` is the loaded clip.
    void handleRedaction(const QString &roomId, const QString &eventId);

Q_SIGNALS:
    /// A clip played to its end on its own (not stopped, paused, replaced or
    /// redacted). Emitted after the player was cleared, so a listener may
    /// start the next clip. AppController uses it for auto-play-next.
    /// `upNext` is the run that followed it, as it stood when it ended.
    void clipFinished(const QString &eventId, const QString &roomId,
                      const QString &threadRootId, bool isVoice,
                      const QVariantList &upNext = {});
    void currentChanged();
    void loadedChanged();
    void playingChanged();
    void fetchStateChanged();
    void positionChanged();
    void durationChanged();
    void seekableChanged();
    void playerChanged();
    void rateChanged();
    void upNextChanged();
    void pausedForCallChanged();
    void metadataChanged();
    void rowOnScreenChanged();

private:
    void ensurePlayer();
    void startOrFetch();
    void startSource(const QString &url);
    void playGated();
    // Drops a play() still waiting at the audio gate (pause, stop, another
    // card taking the speaker).
    void dropWaitingStart();
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
    void readMetadata();
    void applyPendingSeek();
    void setPausedForCall(bool paused);
    void onRowDestroyed(QObject *row);
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
    QVariantList m_waveform;
    qint64 m_declaredDurationMs = 0;
    bool m_loaded = false;
    QString m_fetchState = QStringLiteral("idle");
    // The media key with an outstanding fetch, and the one pinned against LRU
    // eviction while the player holds its file open.
    QString m_fetchingKey;
    QString m_pinnedKey;
    QPointer<lightning::media::QtAudioGate> m_audioGate;
    // Bumped by every play request, pause and stop: a start the gate released
    // late runs only if it is still the newest request.
    quint64 m_playTag = 0;
    int m_playerStarts = 0;
    // A start is held at the audio gate (playGated()); cleared when it runs,
    // gives up or is dropped.
    bool m_gateWaiting = false;

    // The run after the current clip (see setUpNext()).
    QVariantList m_upNext;
    // An audio file's own speed; a voice message uses the setting.
    qreal m_clipRate = 1.0;
    bool m_pausedForCall = false;
    // Set while pauseForCall() pauses, so the pause it causes is not taken
    // for the reader's.
    bool m_callPausing = false;
    // A position to start from once the file is open; -1 for none.
    qint64 m_pendingSeekMs = -1;
    // Rows that reported the current clip on screen.
    QSet<QObject *> m_visibleRows;
    // mediaKey -> {title, artist, artworkSource}, bounded, oldest evicted.
    QHash<QString, QVariantMap> m_clipInfo;
    QStringList m_clipInfoOrder;
    int m_clipInfoRevision = 0;
};
