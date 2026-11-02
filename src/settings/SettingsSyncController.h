#pragma once

#include "settings/SettingsPortable.h"

#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

class CustomThemeStore;
class MatrixClient;
class SettingsManager;
class ShortcutRegistry;

class SettingsSyncController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool supported READ supported NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(qint64 lastSyncedMs READ lastSyncedMs NOTIFY changed)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY changed)
    Q_PROPERTY(int choiceDiffering READ choiceDiffering NOTIFY changed)
    Q_PROPERTY(QStringList conflicts READ conflicts NOTIFY changed)

public:
    explicit SettingsSyncController(QObject *parent = nullptr);

    void setSettings(SettingsManager *settings);
    void setShortcuts(ShortcutRegistry *shortcuts);
    void setCustomThemes(CustomThemeStore *themes);
    void setClient(MatrixClient *client);

    bool supported() const;
    bool enabled() const { return m_enabled; }
    QString state() const;
    QString error() const { return m_error; }
    qint64 lastSyncedMs() const { return m_lastSyncedMs; }
    int pendingCount() const { return int(m_state.pending.size()); }
    int choiceDiffering() const { return int(m_choiceDiffering); }
    QStringList conflicts() const { return m_conflicts; }

    Q_INVOKABLE void setSyncEnabled(bool on);
    Q_INVOKABLE void chooseAdoptServer();
    Q_INVOKABLE void chooseUploadMine();
    Q_INVOKABLE void removeFromServer();
    Q_INVOKABLE void acknowledgeConflicts();

    Q_INVOKABLE QString exportSettings(const QUrl &file);
    Q_INVOKABLE QVariantMap previewImport(const QUrl &file);
    Q_INVOKABLE QVariantMap applyImport();
    Q_INVOKABLE void cancelImport();
    Q_INVOKABLE QString labelFor(const QString &name) const;

    void checkLocalNow();
    qint64 nowMs() const;
    void setClockForTesting(qint64 fixedMs) { m_fixedNow = fixedMs; }
    const settingsport::State &stateForTesting() const { return m_state; }
    void setDebounceForTesting(int ms);

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void startSync();
    void onLocalTouched();
    void onSyncedChanged();
    void onRead(quint64 opId, bool ok, const QString &category,
                const QString &blobState, const QString &blobJson,
                int switchState);
    void onWritten(quint64 opId, int kind, bool ok, const QString &category);
    void retry();

private:
    enum class Force { None, Adopt, Upload };
    enum class AfterSwitch { None, Enable, Clear };

    void connectSettings();
    void requestRead(Force force = Force::None);
    void applyMerge(const settingsport::MergeResult &r, const settingsport::ParsedBlob &server);
    void sendSwitch(bool on, AfterSwitch after);
    void failWith(const QString &category);
    void resetForAccount();
    void loadState();
    void persistState();
    QString stateKey() const;
    QVariantMap localView() const;
    bool active() const { return m_enabled && m_hasBaseline && !m_choiceNeeded; }

    QPointer<SettingsManager> m_settings;
    QPointer<ShortcutRegistry> m_shortcuts;
    QPointer<CustomThemeStore> m_themes;
    QPointer<MatrixClient> m_client;

    QSettings m_store;
    settingsport::State m_state;
    bool m_hasBaseline = false;
    bool m_enabled = false;
    bool m_switchKnown = false;
    bool m_choiceNeeded = false;
    int m_choiceDiffering = 0;
    bool m_readOnly = false;
    QString m_error;
    qint64 m_lastSyncedMs = 0;
    QStringList m_conflicts;
    QString m_userKey;

    // In-flight work. A generation bump (account change, sign-out, switch
    // off) makes every outstanding answer stale.
    quint64 m_gen = 1;
    quint64 m_nextOp = 0x53455400;
    quint64 m_readOp = 0;
    quint64 m_writeOp = 0;
    quint64 m_switchOp = 0;
    quint64 m_readGen = 0;
    quint64 m_writeGen = 0;
    quint64 m_switchGen = 0;
    Force m_force = Force::None;
    AfterSwitch m_afterSwitch = AfterSwitch::None;
    bool m_switchTarget = false;
    bool m_rereadWanted = false;
    settingsport::State m_afterWrite;
    bool m_afterWriteBaseline = false;

    QTimer m_localTimer;
    QTimer m_changedTimer;
    QTimer m_retryTimer;
    bool m_applying = false;
    bool m_started = false;
    bool m_absorbLocal = false;
    qint64 m_fixedNow = 0;

    bool m_haveImport = false;
    settingsport::ParseResult m_import;
};
