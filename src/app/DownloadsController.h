#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <functional>

class FileChooser;
class FileLauncher;
class MediaBridge;
class SettingsManager;

class DownloadsController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(bool asksWhereToSave READ asksWhereToSave
                   NOTIFY configurationChanged)
    Q_PROPERTY(bool sandboxRequiresAsking READ sandboxRequiresAsking CONSTANT)
    Q_PROPERTY(QString folderDisplay READ folderDisplay
                   NOTIFY configurationChanged)
    Q_PROPERTY(bool customFolder READ customFolder NOTIFY configurationChanged)

public:
    struct Sandbox
    {
        bool flatpak = false;
        QString flatpakFilesystems;
        bool snap = false;
        QString appId;
        QString home;
        QHash<QString, QString> xdgDirs;
    };
    /// Whether a sandbox keeps this process from writing to ~/Downloads:
    /// a Flatpak without `home`, `host` or `xdg-download` (read-write), or a
    /// Snap (which has no `home` plug, and whose $HOME is its own).
    static bool sandboxBlocksDownloads(const Sandbox &sandbox);
    /// Whether a file written at `path` lands on the HOST's disk, where the
    /// user can find it after the app quits. Outside a sandbox: always. In a
    /// Flatpak: a document-portal file, the app's own ~/.var/app/<id>, or a
    /// path under a read-write filesystems= grant; anything else is the
    /// sandbox's private tmpfs and is gone at exit. A Snap's writes either
    /// reach the host or are refused by AppArmor, so they count as reaching.
    static bool pathReachesHost(const Sandbox &sandbox, const QString &path);
    static Sandbox hostSandbox();
    static Sandbox hostSandbox(const QString &flatpakInfoPath);

    DownloadsController(MediaBridge *bridge, FileChooser *chooser,
                        FileLauncher *launcher, QObject *parent = nullptr);
    DownloadsController(MediaBridge *bridge, FileChooser *chooser,
                        FileLauncher *launcher, const Sandbox &sandbox,
                        QObject *parent = nullptr);

    void setSettings(SettingsManager *settings);
    void setDefaultFolderProvider(std::function<QString()> provider);

    QVariantList items() const;
    bool asksWhereToSave() const;
    bool sandboxRequiresAsking() const { return m_sandboxBlocks; }
    QString folderDisplay() const;
    bool customFolder() const;
    QString downloadFolder() const;

    Q_INVOKABLE void download(const QString &mediaKey, const QString &rawName,
                              const QString &mime);
    Q_INVOKABLE void saveAs(const QString &mediaKey, const QString &rawName,
                            const QString &mime);
    Q_INVOKABLE bool open(int id);
    Q_INVOKABLE bool showInFolder(int id);
    Q_INVOKABLE void cancel(int id);
    Q_INVOKABLE void retry(int id);
    Q_INVOKABLE void dismiss(int id);
    /// Account switch / sign-out: the previous account's items (finished,
    /// failed and in-flight alike) must not be shown to the next one.
    /// MediaBridge::clear() drops the saves themselves.
    void clear();
    Q_INVOKABLE void chooseFolder();
    Q_INVOKABLE void resetFolder();

    QString pathForTest(int id) const;

Q_SIGNALS:
    void itemsChanged();
    void configurationChanged();

private:
    enum class State { Saving, Done, Failed };
    struct Item
    {
        int id = 0;
        QString mediaKey;
        QString rawName;
        QString mime;
        bool asked = false;
        QString fileName;
        QString path;
        QString hostPath;
        State state = State::Saving;
        QString message;
        bool revealFailed = false;
    };

    void startDirect(const QString &mediaKey, const QString &rawName,
                     const QString &mime);
    void startAsking(const QString &mediaKey, const QString &rawName,
                     const QString &mime);
    int addItem(const QString &mediaKey, const QString &rawName,
                const QString &mime, bool asked, const QString &fileName);
    void refuseSandboxOnlySave(const QString &mediaKey, const QString &rawName,
                               const QString &mime, bool asked,
                               const QString &fileName);
    static QString sandboxOnlyMessage();
    Item *itemById(int id);
    const Item *itemById(int id) const;
    Item *oldestSaving(const QString &mediaKey);
    void onSaveCompleted(const QString &mediaKey, const QString &path);
    void onSaveFinished(bool ok, const QString &message,
                        const QString &mediaKey);
    void onSaveCancelled(const QString &mediaKey);
    void trim();
    bool canOpen(const Item &item) const;

    QPointer<MediaBridge> m_bridge;
    QPointer<FileChooser> m_chooser;
    QPointer<FileLauncher> m_launcher;
    QPointer<SettingsManager> m_settings;
    std::function<QString()> m_defaultFolder;
    Sandbox m_sandbox;
    bool m_sandboxBlocks = false;
    QList<Item> m_items;
    int m_nextId = 1;
};
