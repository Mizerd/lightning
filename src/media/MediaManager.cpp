#include "media/MediaManager.h"

#include "matrix/MatrixClient.h"

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
    if (url.isValid())
        QDesktopServices::openUrl(url);
}
