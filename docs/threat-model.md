# Threat model (v0.5.0-prep)

Scoped to what the current v0.5.0-prep foundation needs a reader to
know. Full model is v1.0 material.

## In scope

- The desktop process runs with the user's privileges. Same trust boundary
  as any other desktop application the user chooses to install.
- The client speaks Matrix over TLS to a homeserver of the user's choice.
- The client stores an access token via the SecretStore (libsecret when
  available; QSettings fallback with a visible warning otherwise).

## Out of scope for this pass

- Multi-user machines. If two humans share the same OS user, they can read
  each other's cached non-E2E messages. The OS user remains the identity
  boundary; libsecret does not protect against another process running as
  the same user.
- Malicious homeservers. The client trusts the homeserver it is pointed at.
- Compromised operating system, keyboard loggers, physical attackers.
- Compromised update channel (packaging story is v1.0).

## Sensitive assets

| Asset | v0.4 storage | Future plan |
|---|---|---|
| Access token | `SecretStore` — libsecret (Secret Service / KWallet with libsecret) when reachable; plaintext QSettings fallback with a visible warning otherwise. Migrated on first read from any legacy `session/accessToken` plaintext key. Not in the SQLite cache. | Same store; hardware-backed keychains as follow-up. |
| Device id, user id, homeserver URL | QSettings (non-secret in themselves) | unchanged |
| Sync token | QSettings (restart-recoverable state, not a credential) | unchanged |
| Device keys | Rust SDK store path exists for the Rust backend: `${XDG_DATA_HOME}/matrix-client/<safeUserId>/matrix-rust-sdk-store/`. E2EE is not enabled/claimed yet, so any SDK crypto material there is treated as SDK-owned state, separate from `cache.sqlite`, and never contains an access token. `--reset-crypto-store` removes it. | Rust SDK store, considered destroyed by `--reset-crypto-store`; key backup/verification later. |
| Message content in transit | TLS to homeserver via `QNetworkAccessManager` (HTTP backend) or the Matrix Rust SDK transport (Rust backend). | Rust SDK encrypted transports once E2EE is verified |
| Encrypted message plaintext | HTTP backend does not decrypt. Rust backend only displays text events that the Matrix SDK emits as decrypted events, but Lightning does not claim E2EE support yet. | Held in-process; do not write decrypted E2EE bodies to the C++ cache until that policy is explicitly designed |
| Local cache | **SQLite** at `${XDG_DATA_HOME}/matrix-client/<safeUserId>/cache.sqlite`. Contains rooms, last 200 events per room, and members. **No** access tokens. Non-E2E timeline bodies are stored plaintext by design. | Same schema; E2E rooms cached decrypted only when the Rust SDK opens the store with the user's session key |
| Media in transit / at rest | Fetched over HTTPS from `/_matrix/media/v3/download` (unauthenticated legacy endpoint). Rendered images sit in Qt's Image cache (in-memory). No custom media cache on disk yet. | Switch to authenticated `/_matrix/client/v1/media/*`; on-disk media cache with expiry |

## SecretStore backends

At startup `SecretStore::createDefault()` picks the first working backend:

1. `LibSecretStore` if `HAVE_LIBSECRET` was defined at build time AND
   `secret_service_get_sync` returns a live handle at runtime. On Linux
   this covers gnome-keyring and KWallet-with-libsecret-bridge. Values are
   stored keyed by `(schema, user_id, key)`.
2. `InsecureFallbackSecretStore` — a QSettings-backed fallback. Loudly
   reports insecure status. The Settings screen renders a red warning
   whenever this backend is active.

Headless CI, `--platform offscreen` runs, and containers without a session
bus will fall back to the insecure store. This is expected and warned about.

## Guarantees this pass does *not* make

- Does not authenticate media downloads. Legacy `/_matrix/media/v3/*` is
  used by design in v0.3/v0.4.
- Does not claim E2EE support. Encrypted sends remain blocked; encrypted
  reads are not considered supported unless the Matrix SDK emits a
  decrypted text event and the UI receives it through the normal timeline
  path.
- Does not protect the SQLite cache. Anyone with read access to the user's
  home directory can read cached non-E2E message bodies.
- Does not treat the Rust backend as feature-complete. `--backend=rust`
  now wires login/restore/sync/plain text send, but rich timeline
  features, media, pagination, and E2EE are still missing or partial.

## Guarantees this pass does make

- No telemetry. Only outbound traffic is to the user-configured homeserver
  via the Matrix Client-Server API over TLS.
- No embedded browser rendering chat. QML draws every message directly.
- No custom cryptography. E2EE must land through the Matrix Rust SDK.
- SQLite cache never contains access tokens or raw crypto key material.
- Rust SDK state is stored separately from the C++ cache at
  `${XDG_DATA_HOME}/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
- `--reset-crypto-store` deletes only Rust SDK store directories and
  never touches `cache.sqlite`, QSettings session metadata, or
  SecretStore access tokens.
- Raw access tokens are never logged.
- `clearSession()` (logout, `/whoami` 401, `/sync` 401) wipes QSettings
  session keys, deletes the SecretStore entry for that user, and clears
  the SQLite cache for that user.
- On first run after upgrading from v0.2/v0.3, any legacy
  `session/accessToken` value in QSettings is migrated into the
  SecretStore and the plaintext key is removed. The migration runs
  synchronously in `SettingsManager::setSecretStore()`.

## Warnings shown to the user (Settings screen)

- Secret backend name (e.g. `libsecret (Secret Service)` or
  `insecure fallback (QSettings)`).
- When the fallback is active: **red** warning explaining the risk and how
  to install a Secret Service provider.
- When libsecret is active: green line confirming secure storage.
- Crypto backend description — text depends on the active backend
  (mock / http / rust) and is the single source of truth for the UI. No
  other layer may claim E2EE support.
- E2EE status line ("E2EE active" / "Rust backend active (E2EE not yet
  verified)" / "E2EE not available on this backend").
