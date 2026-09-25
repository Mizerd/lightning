#include "gif/MatrixGifTransport.h"

#include "matrix/MatrixClient.h"

MatrixGifTransport::MatrixGifTransport(QObject *parent)
    : GifTransport(parent)
{
}

void MatrixGifTransport::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::gifResponse, this,
                &MatrixGifTransport::finished);
        // Every download result is relayed; consumers match their own op ids.
        connect(m_client, &MatrixClient::gifDownloadFinished, this,
                [this](quint64 opId, bool ok, const QByteArray &bytes,
                       const QString &, int, int, qint64,
                       const QString &category) {
                    Q_EMIT downloadFinished(opId, ok, bytes, category);
                });
        connect(m_client, &MatrixClient::loggedOut, this,
                &MatrixGifTransport::sessionEnded);
    }
}

bool MatrixGifTransport::available() const
{
    return m_client && m_client->supportsGifProvider();
}

quint64 MatrixGifTransport::get(const QString &url)
{
    return m_client ? m_client->gifGet(url) : 0;
}

quint64 MatrixGifTransport::download(const QString &url)
{
    return m_client ? m_client->gifDownload(url) : 0;
}
