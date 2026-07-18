#pragma once

#include "gif/GifTransport.h"

class MatrixClient;

// v0.6.1: the real GIF transport — routes requests through the hardened Rust
// safe-get via MatrixClient::gifGet and forwards gifResponse() as finished().
// The URL (which carries the provider key) is passed straight through and is
// never logged on either side.
class MatrixGifTransport : public GifTransport
{
    Q_OBJECT

public:
    explicit MatrixGifTransport(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    bool available() const override;
    quint64 get(const QString &url) override;

private:
    MatrixClient *m_client = nullptr;
};
