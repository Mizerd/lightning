# Threat model (v0.4)

Scoped to what v0.4 needs a reader to know. Full model is v1.0 material.

## In scope

- The desktop process runs with the user's privileges. Same trust boundary
  as any other desktop application the user chooses to install.
- The client speaks Matrix over TLS to a homeserver of the user's choice.
- The client stores an access token via the SecretStore (libsecret when
  available; QSettings fallback with a visible warning otherwise).

## Out of scope for v0.4

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
| Device keys | Not present. When the Rust backend graduates, keys will be owned by the Matrix Rust SDK's own store, separate from the app SQLite cache. | Rust SDK store, encrypted at rest by the SDK |
| Message content in transit | TLS to homeserver via `QNetworkAccessManager` (HTTP backend). Rust backend will use the SDK's own transport. | Rust SDK encrypted transports |
| Encrypted message plaintext | Not decrypted by HTTP backend or v0.4 Rust scaffold (placeholder text only) | Held in-process; not written to disk in plaintext once E2EE lands |
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

## Guarantees v0.4 does *not* make

- Does not authenticate media downloads. Legacy `/_matrix/media/v3/*` is
  used by design in v0.3/v0.4.
- Does not decrypt anything. `m.room.encrypted` renders as a placeholder;
  encrypted media renders as `[encrypted media - E2EE not implemented yet]`.
- Does not send anything into encrypted rooms.
- Does not protect the SQLite cache. Anyone with read access to the user's
  home directory can read cached non-E2E message bodies.
- Does not treat the Rust backend as feature-complete. `--backend=rust`
  runs but refuses login, sends, and pagination — see
  `docs/architecture.md` for the exact seam.

## Guarantees v0.4 does make

- No telemetry. Only outbound traffic is to the user-configured homeserver
  via the Matrix Client-Server API over TLS.
- No embedded browser rendering chat. QML draws every message directly.
- No custom cryptography. E2EE lands through the Matrix Rust SDK.
- SQLite cache never contains access tokens or key material.
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
- E2EE status line ("E2EE active" / "Rust backend scaffold (E2EE not yet
  wired)" / "E2EE not available on this backend").
