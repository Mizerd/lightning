#pragma once

#include <QString>

// Pure helpers for the invite/user-search flow; no models, FFI or I/O.
//
// Many homeservers' user directories omit local users who share no room with
// the searcher, so a query like "admin" is also resolved as
// "@admin:<own-server>" by exact profile lookup. These helpers derive that
// candidate without hardcoding a server or fabricating ids from invalid
// localparts.
namespace matrix::user_lookup {

// Server name of a full Matrix user id ("@a:server[:port]" -> "server[:port]").
// Empty when the id has no server part.
QString serverNameFromUserId(const QString &userId);

// Strict Matrix localpart grammar. Candidates are only built from it, so no
// malformed id is fabricated (historical ids still work when typed in full).
bool isValidLocalpart(const QString &localpart);

// dns-name[:port] (no IPv6 literal support — consistent with
// UserSearchModel::looksLikeMxid).
bool isValidServerName(const QString &serverName);

// Derive the exact-lookup candidate for a raw query, using the
// authenticated account's server for bare localparts:
//   "admin"                     -> "@admin:<ownServer>"
//   "@admin"                    -> "@admin:<ownServer>"
//   "admin:server.example"      -> "@admin:server.example"
//   "@admin:server.example"     -> "@admin:server.example"
// Returns an empty string when no well-formed candidate exists.
QString exactCandidate(const QString &rawQuery, const QString &ownServerName);

// True when the query names an explicit server (with or without the
// leading "@") — i.e. the candidate is an "exact Matrix ID" rather than a
// bare-localpart guess against the user's own homeserver.
bool queryNamesServer(const QString &rawQuery);

// Visible-name fallback when no display name is known: the localpart
// ("@matas:server" -> "matas"), or the full id if none can be derived.
QString localpartOrUserId(const QString &userId);

} // namespace matrix::user_lookup
