// Warm account adoption (master prompt Part 4 §4): what moves from a running
// background host to the foreground client when the user switches to an
// account that is already live. Plain data, Qt Core only.
//
// Ownership: the ticket CARRIES the Rust handle. Exactly one party owns it at a
// time: SessionPool::takeForAdoption() gives it up (the host is gone and the
// store record already names the foreground), MatrixClient::adoptLiveSession()
// takes it, and on a refused promotion retires it through its own normal
// release (tokens handed back). Nothing else may destroy or reuse `handle`.
//
// SENSITIVE: persistedDigest is a digest of the access token the keyring holds,
// never the token; still never logged.
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
    QString slug;              // safeUserSlug(userId), for logs
    quint64 storeSerial = 0;   // the StoreOwnershipRegistry record, now Foreground
    QByteArray persistedDigest;
    bool oauth = false;
    bool valid() const { return handle != nullptr && !userId.isEmpty(); }
};

} // namespace lightning::accounts
