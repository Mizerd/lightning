#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class VoicePlaybackController;
class QDBusConnection;

class MprisServer : public QObject
{
    Q_OBJECT

public:
    explicit MprisServer(VoicePlaybackController *player,
                         QObject *parent = nullptr);
    ~MprisServer() override;

    // "org.mpris.MediaPlayer2.lightning", or under Flatpak (FLATPAK_ID set)
    // "org.mpris.MediaPlayer2.<FLATPAK_ID>", the one name the sandbox grants.
    // The per-instance fallback keeps whichever prefix this returns.
    static QString baseServiceName();
    static QString objectPath();

    void setBusConnectionName(const QString &busName);

    using PreviewPolicy = std::function<int(const QString &roomId)>;
    void setPreviewPolicy(PreviewPolicy policy);
    void refreshMetadata();
    int previewLevelFor(const QString &roomId) const;
    QByteArray trackSalt() const { return m_salt; }
    QString registeredServiceName() const { return m_serviceName; }

Q_SIGNALS:
    void metadataPolicyChanged();
    void raiseRequested();
    void registrationChanged();

private:
    void sync();
    void registerOnBus();
    void requestName(const QString &name, bool fallbackToInstance);
    void unregisterFromBus();

    QPointer<VoicePlaybackController> m_player;
    PreviewPolicy m_policy;
    QByteArray m_salt;
    QString m_busName;
    QString m_serviceName;
    QObject *m_exported = nullptr;
};
