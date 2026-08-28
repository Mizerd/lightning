#include "media/MediaManager.h"

#include "matrix/MatrixClient.h"
#include "models/LinkPreview.h"

#include "app/UrlLauncher.h"

#include <QDesktopServices>

MediaManager::MediaManager(QObject *parent)
    : QObject(parent)
{
}

void MediaManager::setClient(MatrixClient *client)
{
    m_client = client;
}

QString MediaManager::localPathFromUrl(const QUrl &url)
{
    if (url.isLocalFile())
        return url.toLocalFile();
    return url.toString();
}

void MediaManager::sendPickedImage(const QString &roomId, const QUrl &fileUrl)
{
    if (!m_client) return;
    const QString path = localPathFromUrl(fileUrl);
    if (path.isEmpty()) return;
    m_client->sendImage(roomId, path);
}

void MediaManager::sendPickedFile(const QString &roomId, const QUrl &fileUrl)
{
    if (!m_client) return;
    const QString path = localPathFromUrl(fileUrl);
    if (path.isEmpty()) return;
    m_client->sendFile(roomId, path);
}

void MediaManager::openExternal(const QUrl &url)
{
    // Not QDesktopServices directly: inside an AppImage the spawned browser
    // would inherit the bundle's LD_LIBRARY_PATH and fail to start. See
    // lightning::urls::openExternally.
    if (url.isValid())
        lightning::urls::openExternally(url);
}

void MediaManager::openWebUrl(const QUrl &url)
{
    if (matrix::link_preview::isSafeExternalUrl(url))
        lightning::urls::openExternally(url);
}
