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
    Q_MOC_INCLUDE(<QAudioOutput>)
    Q_MOC_INCLUDE(<QMediaPlayer>)
    Q_PROPERTY(QString eventId READ eventId NOTIFY currentChanged)
    Q_PROPERTY(QString roomId READ roomId NOTIFY currentChanged)
    Q_PROPERTY(QString roomName READ roomName NOTIFY currentChanged)
    Q_PROPERTY(QString threadRootId READ threadRootId NOTIFY currentChanged)
    Q_PROPERTY(QString senderName READ senderName NOTIFY currentChanged)
    Q_PROPERTY(QString senderId READ senderId NOTIFY currentChanged)
    Q_PROPERTY(QString senderAvatarMxc READ senderAvatarMxc
                   NOTIFY currentChanged)
    Q_PROPERTY(QString mediaKey READ mediaKey NOTIFY currentChanged)
    Q_PROPERTY(QString filename READ filename NOTIFY currentChanged)
    Q_PROPERTY(bool isVoice READ isVoice NOTIFY currentChanged)
    Q_PROPERTY(QVariantList waveform READ waveform NOTIFY currentChanged)
    Q_PROPERTY(bool active READ active NOTIFY currentChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(QString fetchState READ fetchState NOTIFY fetchStateChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY seekableChanged)
    Q_PROPERTY(QString ownerKey READ ownerKey NOTIFY currentChanged)
    Q_PROPERTY(QMediaPlayer *player READ player NOTIFY playerChanged)
    Q_PROPERTY(QAudioOutput *audioOutput READ audioOutput NOTIFY playerChanged)
    Q_PROPERTY(qreal rate READ rate NOTIFY rateChanged)
    Q_PROPERTY(QVariantList upNext READ upNext NOTIFY upNextChanged)
    Q_PROPERTY(bool hasNext READ hasNext NOTIFY upNextChanged)
    Q_PROPERTY(bool pausedForCall READ pausedForCall
                   NOTIFY pausedForCallChanged)
    Q_PROPERTY(QString trackTitle READ trackTitle NOTIFY metadataChanged)
    Q_PROPERTY(QString trackArtist READ trackArtist NOTIFY metadataChanged)
    Q_PROPERTY(QString artworkSource READ artworkSource
                   NOTIFY metadataChanged)
    Q_PROPERTY(int clipInfoRevision READ clipInfoRevision
                   NOTIFY metadataChanged)
    Q_PROPERTY(bool rowOnScreen READ rowOnScreen NOTIFY rowOnScreenChanged)
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
    void setSourceResolverForTest(SourceResolver resolver);
    // Tests: the audio gate to ask before play() (QtAudioGate.h). Production
    // uses the process's installed gate.
    // Out of line: QPointer needs the complete type to assign.
    void setAudioGateForTest(lightning::media::QtAudioGate *gate);
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

    Q_INVOKABLE bool isCurrent(const QString &eventId) const;
    Q_INVOKABLE void play(const QString &eventId, const QString &roomId,
                          const QString &mediaKey, const QVariantMap &meta);
    Q_INVOKABLE void toggle(const QString &eventId, const QString &roomId,
                            const QString &mediaKey, const QVariantMap &meta);
    Q_INVOKABLE void togglePlayPause();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void seek(qint64 positionMs);
    Q_INVOKABLE void stop();
    void clearSession();
    Q_INVOKABLE bool playNext();
    Q_INVOKABLE void restart();
    Q_INVOKABLE void setRate(qreal rate);
    qreal volume() const;
    Q_INVOKABLE void setVolume(qreal volume);
    Q_INVOKABLE QVariantMap clipInfo(const QString &mediaKey) const;
    Q_INVOKABLE void rememberEventCover(const QString &mediaKey,
                                        const QString &source);
    Q_INVOKABLE void reportRowVisible(QObject *row, const QString &eventId,
                                      bool visible);

    void reclaimAudibility();
    void setUpNext(const QVariantList &queue);
    void pauseForCall();
    void resumeAfterCall();
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
    bool m_gateWaiting = false;

    QVariantList m_upNext;
    qreal m_clipRate = 1.0;
    bool m_pausedForCall = false;
    bool m_callPausing = false;
    qint64 m_pendingSeekMs = -1;
    QSet<QObject *> m_visibleRows;
    QHash<QString, QVariantMap> m_clipInfo;
    QStringList m_clipInfoOrder;
    int m_clipInfoRevision = 0;
};
