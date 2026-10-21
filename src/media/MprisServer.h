#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class VoicePlaybackController;
class QDBusConnection;

// The desktop's media keys and "now playing" widgets for Lightning's own
// voice/audio player: an MPRIS 2 server (org.mpris.MediaPlayer2.lightning) on
// the session bus, Linux and other D-Bus desktops only.
//
// It exists on the bus ONLY while a clip is loaded (VoicePlaybackController
// ::active), so a desktop's media keys go to some other player the rest of the
// time, and nothing appears in a "now playing" widget while Lightning plays
// nothing. Play, Pause, PlayPause, Stop, Next (the next voice message of the
// run), Previous (back to the start), Seek and SetPosition drive the app's
// player. What the metadata says follows the user's NOTIFICATION PREVIEW
// privacy for the clip's room (a desktop "now playing" widget and the lock
// screen are as public as a notification): full shows the title ("Voice
// message", or an audio file's own title, else its file name) and the artist
// (an audio file's own artist, else the sender); sender-only shows a generic
// title and the sender; private shows a generic title and no artist. No room
// name, no event id and no media URL ever leaves the process: the track id is
// a hash salted per process, and the cover art is not offered (it lives only
// in RAM behind an in-process image provider, CLAUDE.md §6).
//
// LIGHTNING_DISABLE_MPRIS=1 keeps it off the bus entirely (the test suites
// set it, so a ctest run never shows up in the developer's media widget).
//
// Not to be confused with the musicact feature, which READS other players'
// MPRIS state; that one must skip this service's name.
//
// A build without QtDBus (Windows, macOS) compiles this to an inert object.
// Windows' System Media Transport Controls would need the WinRT projection,
// which the MinGW toolchain does not ship: not provided.
class MprisServer : public QObject
{
    Q_OBJECT

public:
    explicit MprisServer(VoicePlaybackController *player,
                         QObject *parent = nullptr);
    ~MprisServer() override;

    // "org.mpris.MediaPlayer2.lightning".
    static QString baseServiceName();
    // The object path every MPRIS player is reached at.
    static QString objectPath();

    // Use `busName` (a QDBusConnection name) instead of the session bus.
    // Tests pass a connection to a private bus. Call before the first clip.
    void setBusConnectionName(const QString &busName);

    // The notification preview level (SettingsManager PreviewMode: 0 sender
    // and content, 1 sender only, 2 private) for a room; AppController passes
    // its per-room resolver. Without one, every clip is treated as private.
    using PreviewPolicy = std::function<int(const QString &roomId)>;
    void setPreviewPolicy(PreviewPolicy policy);
    // The policy's answer may have changed (the user changed the setting):
    // re-read the metadata and signal what changed.
    void refreshMetadata();
    int previewLevelFor(const QString &roomId) const;
    QByteArray trackSalt() const { return m_salt; }
    // The service name this server holds right now, or "" while it is not on
    // the bus (no clip loaded, no bus, or the name could not be taken).
    QString registeredServiceName() const { return m_serviceName; }

Q_SIGNALS:
    // The policy or the setting behind it changed (adaptor listens).
    void metadataPolicyChanged();
    // org.mpris.MediaPlayer2.Raise: bring the main window forward.
    void raiseRequested();
    // The service appeared on or left the bus.
    void registrationChanged();

private:
    void sync();
    void registerOnBus();
    void requestName(const QString &name, bool fallbackToInstance);
    void unregisterFromBus();

    QPointer<VoicePlaybackController> m_player;
    PreviewPolicy m_policy;
    // Per-process salt for the track id, so one clip's id does not
    // correlate across runs or with another client's.
    QByteArray m_salt;
    QString m_busName;
    QString m_serviceName;
    // The object exported at objectPath(), carrying both adaptors.
    QObject *m_exported = nullptr;
};
