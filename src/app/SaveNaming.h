#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace savenaming {

QString sanitizeLeaf(const QString &raw);

QString extensionForMime(const QString &mime);

bool looksLikeExtension(const QString &suffix);

QString extensionOf(const QString &leaf);

QString suggestedFileName(const QString &rawName, const QString &mime);

QString ensureExtension(const QString &chosenLeaf,
                        const QString &expectedExtension);

QString numberedName(const QString &leaf, int n);

QString uniqueFileName(const QString &directory, const QString &leaf);

bool isRiskyToOpen(const QString &fileName, const QString &mime = {});

QByteArray zoneIdentifierContent();

bool markAsDownloaded(const QString &path);

QStringList saveDialogFilters(const QString &leaf);

// ── Files the xdg document portal granted ──
//
// A Snap or a Flatpak that saves through the FileChooser portal is handed the
// file inside the document portal's FUSE mount: /run/user/<uid>/doc/<id>/<name>
// (a Snap, whose $XDG_RUNTIME_DIR is /run/user/<uid>/snap.<name>, still sees
// it there), or /run/flatpak/doc/<id>/<name> once a Flatpak's symlink is
// resolved, or, on the host, .../doc/by-app/<app>/<id>/<name>. <id> is a
// document id like "fd37b80a": a folder name the user has never seen, and one
// no file manager outside the sandbox can be pointed at usefully.

bool isDocumentPortalPath(const QString &path, const QString &runtimeDir);
bool isDocumentPortalPath(const QString &path);

QString documentPortalId(const QString &path, const QString &runtimeDir);
QString documentPortalId(const QString &path);

QString savedFolderName(const QString &path, const QString &hostPath,
                        const QString &runtimeDir);
QString savedFolderName(const QString &path, const QString &hostPath);

}
