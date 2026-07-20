#pragma once

#include <QString>

// v0.5.11: pure helpers for the invite/user-search flow. No Qt models, no
// FFI, no I/O — fully unit-testable.
//
// The user directory on many homeservers does not list local users that
// have never shared a room with the searcher, so a plausible query like
// "admin" must additionally be resolved as "@admin:<own-server>" through an
// exact profile lookup. These helpers derive that candidate; they never
// hardcode a server name and never fabricate an id from an invalid
// localpart.
namespace matrix::user_lookup {

// Server name of a full Matrix user id ("@a:server[:port]" -> "server[:port]").
// Empty when the id has no server part.
QString serverNameFromUserId(const QString &userId);

// Matrix-spec localpart grammar (historical ids are accepted by the exact
// lookup anyway once typed fully; candidates are only constructed from the
// strict grammar so no malformed id is ever fabricated).
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

// v0.7: the shared visible-name fallback for a user with no resolved
// display name anywhere: the localpart of the Matrix id ("@matas:server"
// -> "matas"). The complete MXID is returned only when no localpart can be
// derived; it stays available separately for tooltips/disambiguation.
QString localpartOrUserId(const QString &userId);

} // namespace matrix::user_lookup
