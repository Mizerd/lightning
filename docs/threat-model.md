# Threat model (early draft)

Scoped to what v0.3 needs a reader to know. Full model is v1.0 material.

## In scope

- The desktop process runs with the user's privileges. Same trust boundary
  as any other desktop application the user chooses to install.
- The client speaks Matrix over TLS to a homeserver of the user's choice.
- As of v0.2, real network traffic exists: the client speaks the Matrix
  Client-Server API over HTTPS to the configured homeserver.

## Out of scope for v0.1

- Multi-user machines. If two humans share the same OS user, they can read
  each other's messages via the local cache. Treat the OS user as the
  identity boundary until v0.4 adds keychain storage.
- Malicious homeservers. The client trusts the homeserver it is pointed at.
- Compromised operating system, keyboard loggers, physical attackers.
- Compromised update channel (packaging story is v1.0).

## Sensitive assets

| Asset | v0.3 storage | v0.4+ plan |
|---|---|---|
| Access token | **QSettings (plaintext)** as `session/accessToken`, warning shown in Settings; NOT in the SQLite cache | OS keychain (Secret Service / KWallet, Keychain, DPAPI) |
| Device id, user id, sync token | QSettings (non-secret in themselves) | still QSettings; only the token moves to the keychain |
| Device keys | not present | Rust SDK-managed, keychain-backed |
| Message content in transit | TLS to homeserver via `QNetworkAccessManager` | same; plus Rust SDK encrypted transports |
| Encrypted message plaintext | not decrypted (placeholder text only) | held in-process; not written to disk in plaintext once E2EE lands |
| Local cache | **SQLite** at `${XDG_DATA_HOME}/matrix-client/<safeUserId>/cache.sqlite`. Contains rooms, last 200 events per room, and members. **No** access tokens. Non-E2E timeline bodies are stored plaintext by design. | Same schema; E2E rooms cached decrypted only when the Rust SDK opens the store with the user's session key. |
| Media in transit / at rest | Fetched over HTTPS from `/_matrix/media/v3/download` (unauthenticated legacy endpoint). Rendered images sit in Qt's Image cache (in-memory). No custom media cache on disk yet. | Switch to authenticated `/_matrix/client/v1/media/*`; on-disk media cache with expiry. |

## Guarantees v0.3 does *not* make

- Does not claim to protect tokens at rest. `session/accessToken` is plaintext in QSettings on disk.
- Does not claim any Matrix cryptographic properties. `CryptoManager` is still a no-op interface.
- Does not send anything into encrypted rooms (text, media, reactions, edits, redactions). Sends are blocked with an explicit error.
- Does not decrypt anything. `m.room.encrypted` renders as a placeholder; encrypted media renders as `[encrypted media - E2EE not implemented yet]`.
- Does not authenticate media downloads. The legacy unauthenticated `/_matrix/media/v3/download` endpoint is used by design in v0.3.
- Does not protect the SQLite cache. Anyone with read access to the user's home directory can read cached non-E2E message bodies.

## Guarantees v0.3 does make

- No telemetry. The only outbound traffic is to the user-configured homeserver, using the Matrix Client-Server API over TLS.
- No `--mock` traffic in default mode; no HTTP traffic when `--mock` is passed.
- No embedded browser rendering chat. QML draws every message directly.
- No custom cryptography. E2EE will land through a maintained upstream (Matrix Rust SDK), not an in-house implementation.
- The SQLite cache never contains access tokens. `saveSession` writes to QSettings only.
- Raw access tokens are never logged. Log lines about auth include only user ids and HTTP status codes.
- `clearSession()` (called on logout, whoami-401, and sync-401) wipes both QSettings session keys and the local SQLite cache.

## Warnings shown to the user

- Settings screen: "Access tokens are stored in QSettings (plaintext) in
  v0.1. Secure storage arrives in v0.4."
- Settings screen: "End-to-end encryption is not available in v0.1 and will
  be delivered via the Matrix Rust SDK in v0.4. This client does not roll
  its own crypto."
