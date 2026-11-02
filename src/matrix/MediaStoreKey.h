#pragma once

#include <QByteArray>
#include <QString>

class SecretStore;

namespace matrix::media_store_key {

inline constexpr char kSecretName[] = "mediaStoreKey";
inline constexpr int kKeyBytes = 32;
inline constexpr char kAccessTokenName[] = "accessToken";

enum class Found { Present, Absent, Unreadable };

struct Resolution {
    Found found = Found::Unreadable;
    bool created = false;
    QByteArray key;
};

Resolution read(const SecretStore *store, const QString &userId);

Resolution resolve(SecretStore *store, const QString &userId,
                   bool mayCreate = true);

// Whether resolve() may create a key, from what SettingsManager reports about
// the account's secret store. Pure. A backend that could not answer never
// gets one. A store whose misses are structurally inconclusive gets one only
// when it is the insecure fallback, whose own per-read verdict (read())
// already refuses a miss for an account it holds nothing for; a secure store
// could not say that, so it never does.
bool mayCreate(bool backendUnavailable, bool missesAreInconclusive,
               bool storeIsSecure);

bool admitsEncryptedRooms(const Resolution &resolution, bool storeIsSecure);

const char *describe(const Resolution &resolution);

}
