#pragma once

#include <QString>

// One SHA-256 over a file on disk, as 64 lowercase hex characters.
//
// This is the ONLY integrity primitive the updater helper and the
// application share, and it exists because the two of them are connected by
// nothing but a filesystem path. UpdateDownloader verifies the manifest's
// hash while the bytes stream in; everything after that -- promotion to the
// manifest's predictable filename, an install-on-quit that may sit armed for
// hours, and finally `pkexec dpkg -i <path>` reading the file as ROOT -- used
// to trust the path alone. Anything running as the user could swap the file
// in that window and have the user's own PolicyKit consent install it.
//
// So the digest is re-taken at every hand-over: by the application right
// before it launches the helper, and by the helper right before it acts.
// The windows that remain are the microseconds between a re-hash and the
// read that follows it, not the hours between download and install.
namespace updater {

// Returns the lowercase hex digest, or an empty string when the file cannot
// be read in full. Streams in bounded chunks; never loads the file at once.
QString sha256HexOfFile(const QString &path);

// Exactly 64 lowercase hex characters. Upper case is refused on purpose: the
// manifest side is normalised to lowercase, and accepting both would leave
// two spellings of one digest.
bool isSha256Hex(const QString &value);

} // namespace updater
