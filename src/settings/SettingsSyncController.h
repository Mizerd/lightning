#pragma once

// Settings export / import and "Sync settings across your devices".
//
// Both features move the SAME allow-listed preference set (SettingsPortable.h)
// and share its codec. This class is the glue: it reads and writes
// SettingsManager through its properties, the shortcut registry and the custom
// theme store through their public API, and talks to the homeserver only
// through MatrixClient's three account-data calls.
//
// Sync, in one paragraph. An ACCOUNT-WIDE switch (its own account-data type,
// absent = off) turns it on. While on, every read asks the SERVER, merges per
// key (last writer wins, with the later one reported when it replaces an
// unsynced local edit), applies what changed locally and writes the blob back
// when this device owes it a change. Local edits are noticed through the
// properties' own change signals and pushed after a 2 s debounce. Nothing
// untouched is uploaded (an untouched setting is not a choice), so improved
// defaults still reach a device that never chose. The first enable on a device
// with its own non-default settings asks: adopt the account's, or upload mine.

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
    // The account-wide switch as the server last said it.
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    // "unsupported" | "off" | "checking" | "choice" | "idle" | "busy" |
    // "error" | "readOnly"
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(qint64 lastSyncedMs READ lastSyncedMs NOTIFY changed)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY changed)
    // First enable with settings on both sides that disagree.
    Q_PROPERTY(int choiceDiffering READ choiceDiffering NOTIFY changed)
    // Settings a later change on another device replaced here. Cleared by
    // acknowledgeConflicts().
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

    // ---- sync ----
    Q_INVOKABLE void setSyncEnabled(bool on);
    Q_INVOKABLE void chooseAdoptServer();
    Q_INVOKABLE void chooseUploadMine();
    // Turns the switch off and empties the server copy. Local settings stay.
    Q_INVOKABLE void removeFromServer();
    Q_INVOKABLE void acknowledgeConflicts();

    // ---- export / import ----
    // "" on success, else a user-facing reason.
    Q_INVOKABLE QString exportSettings(const QUrl &file);
    // { ok, error, version, newerVersion, changes: [{name, label, group,
    //   from, to}], rejected: [{label, reason}], unknownCount,
    //   shortcutChanges, themesNew }
    Q_INVOKABLE QVariantMap previewImport(const QUrl &file);
    // { ok, error, applied, shortcutsSkipped, themesAdded }
    Q_INVOKABLE QVariantMap applyImport();
    Q_INVOKABLE void cancelImport();
    Q_INVOKABLE QString labelFor(const QString &name) const;

    // For tests: run the debounced local-change check now.
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
    quint64 m_nextOp = 0x53455400; // "SET\0": apart from other modules' ids
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
    // The first merge after loading a persisted baseline absorbs differences
    // nobody edited for this account (see settingsport::absorbLocal).
    bool m_absorbLocal = false;
    qint64 m_fixedNow = 0;

    // The parsed import awaiting the user's decision.
    bool m_haveImport = false;
    settingsport::ParseResult m_import;
};
