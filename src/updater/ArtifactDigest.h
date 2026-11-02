#pragma once

#include <QString>

namespace updater {

QString sha256HexOfFile(const QString &path);

// Exactly 64 lowercase hex characters. Upper case is refused on purpose: the
// manifest side is normalised to lowercase, and accepting both would leave
// two spellings of one digest.
bool isSha256Hex(const QString &value);

}
