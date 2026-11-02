#pragma once

#include <QString>

namespace matrix::user_lookup {

QString serverNameFromUserId(const QString &userId);

// Strict Matrix localpart grammar. Candidates are only built from it, so no
// malformed id is fabricated (historical ids still work when typed in full).
bool isValidLocalpart(const QString &localpart);

bool isValidServerName(const QString &serverName);

QString exactCandidate(const QString &rawQuery, const QString &ownServerName);

bool queryNamesServer(const QString &rawQuery);

QString localpartOrUserId(const QString &userId);

}
