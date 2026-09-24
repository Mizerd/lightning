#pragma once

#include <QString>

// One SHA-256 over a file, as 64 lowercase hex characters.
//
// The only integrity primitive the helper and the application share; they are
// connected by nothing but a filesystem path. After the streaming check, the
// file sits at a predictable path (possibly for hours before an
// install-on-quit) until `pkexec dpkg -i <path>` reads it as root, so anything
// running as the user could swap it. The digest is therefore re-taken at every
// hand-over: by the application before launching the helper, and by the
// helper before acting.
namespace updater {

// Returns the lowercase hex digest, or an empty string when the file cannot
// be read in full. Streams in bounded chunks; never loads the file at once.
QString sha256HexOfFile(const QString &path);

// Exactly 64 lowercase hex characters. Upper case is refused on purpose: the
// manifest side is normalised to lowercase, and accepting both would leave
// two spellings of one digest.
bool isSha256Hex(const QString &value);

} // namespace updater
