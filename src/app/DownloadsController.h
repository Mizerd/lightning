#pragma once

// Downloading an attachment, the way Element does it (`app.downloads`).
//
//   download()  Element's "Download": straight into the downloads folder, no
//               dialog, never replacing a file ("name (1).ext"), unless the
//               "Always ask where to save files" setting is on or a sandbox
//               cannot write there, in which case it asks like saveAs().
//   saveAs()    Always asks: the native save dialog (FileChooser), prefilled
//               with the attachment's own name, its extension corrected from
//               the MIME type and the matching type filter selected. An
//               extension the user deletes is put back.
//
// Every download is listed in `items` until dismissed, which is what the
// downloads card renders: in flight (cancellable), then finished with "Open"
// and "Show in folder" (Element's "Download completed" toast). A file whose
// type can run code is never opened from here; it offers "Show in folder"
// only and says why (savenaming::isRiskyToOpen). Nothing is opened unless the
// user clicks.
//
// Paths stay in C++: QML gets ids, the file name and the folder's name.
//
// A Snap or Flatpak saving through the portal gets the file back as a path in
// the document portal's mount (/run/user/<uid>/doc/<id>/<name>), whose parent
// is a document id. That id is never shown as the folder: the real location
// is asked of the portal (FileLauncher::resolveHostPath) and named when it
// answers; until then, or when it cannot, the card says only "Saved".

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
    /// Oldest first: maps of {id, mediaKey, fileName, folderName, state
    /// ("saving" | "done" | "failed"), message, risky, canOpen,
    /// revealFailed}. folderName is empty when no folder the user knows can
    /// be named (a document-portal file whose real location the portal did
    /// not say); revealFailed is true after Show in folder found no way to
    /// show it.
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    /// True when "Download" will ask where to save: the setting is on, or the
    /// sandbox cannot write to the downloads folder.
    Q_PROPERTY(bool asksWhereToSave READ asksWhereToSave
                   NOTIFY configurationChanged)
    /// True inside a Flatpak or Snap that has no write access to the
    /// downloads folder; the setting is then moot and the UI says so.
    Q_PROPERTY(bool sandboxRequiresAsking READ sandboxRequiresAsking CONSTANT)
    /// The downloads folder, for display (native separators, home as "~").
    Q_PROPERTY(QString folderDisplay READ folderDisplay
                   NOTIFY configurationChanged)
    /// True when the folder is the user's choice, not the system default.
    Q_PROPERTY(bool customFolder READ customFolder NOTIFY configurationChanged)

public:
    struct Sandbox
    {
        bool flatpak = false;
        /// The `filesystems=` line of /.flatpak-info's [Context] group.
        QString flatpakFilesystems;
        bool snap = false;
    };
    /// Whether a sandbox keeps this process from writing to ~/Downloads:
    /// a Flatpak without `home`, `host` or `xdg-download` (read-write), or a
    /// Snap (which has no `home` plug, and whose $HOME is its own).
    static bool sandboxBlocksDownloads(const Sandbox &sandbox);
    static Sandbox hostSandbox();

    DownloadsController(MediaBridge *bridge, FileChooser *chooser,
                        FileLauncher *launcher, QObject *parent = nullptr);
    /// For tests: a given sandbox instead of the host's.
    DownloadsController(MediaBridge *bridge, FileChooser *chooser,
                        FileLauncher *launcher, const Sandbox &sandbox,
                        QObject *parent = nullptr);

    void setSettings(SettingsManager *settings);
    /// Where downloads go when the setting names no folder; defaults to
    /// QStandardPaths::DownloadLocation.
    void setDefaultFolderProvider(std::function<QString()> provider);

    QVariantList items() const;
    bool asksWhereToSave() const;
    bool sandboxRequiresAsking() const { return m_sandboxBlocks; }
    QString folderDisplay() const;
    bool customFolder() const;
    /// The absolute downloads folder (the setting, or the default).
    QString downloadFolder() const;

    /// Element's "Download". `rawName` is the event's filename or body,
    /// `mime` its info.mimetype; both sender-chosen.
    Q_INVOKABLE void download(const QString &mediaKey, const QString &rawName,
                              const QString &mime);
    /// "Save as…": always asks.
    Q_INVOKABLE void saveAs(const QString &mediaKey, const QString &rawName,
                            const QString &mime);
    /// The finished file, opened with the desktop's handler. Refused (false)
    /// for a risky type, an unfinished item, or a file that is gone.
    Q_INVOKABLE bool open(int id);
    /// Reveals the finished file in the file manager.
    Q_INVOKABLE bool showInFolder(int id);
    /// Abandons a download in flight.
    Q_INVOKABLE void cancel(int id);
    /// Starts a failed download again, the same way it was started.
    Q_INVOKABLE void retry(int id);
    /// Removes a finished item from the list.
    Q_INVOKABLE void dismiss(int id);
    /// Settings: pick another downloads folder (native folder chooser).
    Q_INVOKABLE void chooseFolder();
    /// Settings: back to the system's Downloads folder.
    Q_INVOKABLE void resetFolder();

    /// The finished file's path (tests; never exposed to QML).
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
        /// A document-portal file's real location, once the portal said.
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
    bool m_sandboxBlocks = false;
    QList<Item> m_items;
    int m_nextId = 1;
};
