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
