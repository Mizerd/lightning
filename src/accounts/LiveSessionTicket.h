#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace lightning::accounts {

struct LiveSessionTicket {
    void *handle = nullptr;
    QString userId;
    QString deviceId;
    QString homeserver;
    QString storePath;
    QString slug;
    quint64 storeSerial = 0;
    QByteArray persistedDigest;
    bool oauth = false;
    bool valid() const { return handle != nullptr && !userId.isEmpty(); }
};

}
