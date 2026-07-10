# Threat model (v0.5.8 room state, live timeline, verification, and key import)

Scoped to what the current v0.5.0-prep foundation needs a reader to
know. Full model is v1.0 material.

## Room-list and sync boundary (v0.5.8)

- Rust is the only layer that probes server support, runs room-list sync,
  interprets `m.direct`, consumes account/ephemeral data, builds Space filters,
  and sends typing/receipt/membership actions. QML sees bounded metadata and
  never receives raw sync/account-data/state events.
- Modern mode uses matrix-sdk-ui's unified supervisor. Its permit makes the
  room-list and encryption Sliding Sync pair mutually owned; classic `/sync`
  is not started concurrently. Compatibility fallback requires a positive
  unsupported capability/endpoint result, never a transient or authentication
  failure.
- Routine logs contain only mode/state/action/category/count information. They
  exclude room names, previews, mappings, membership lists, composer content,
  raw receipts/events/responses, credentials and cryptographic material.
- Typing is ephemeral and never persisted. `m.direct` stays in the SDK store
  and is not copied into Lightning's custom SQLite database. Receipt/fully-read
  state is sent through official SDK methods and is not replayed after the
  owning lifecycle ends.
- Encrypted sidebar plaintext is memory-only. `CacheStore::saveRoom` blanks
  encrypted previews, and the 0.5.8 marker regression scans raw database bytes.
- Room actions have owned join handles. Sign-out invalidates C++ callbacks,
  stops UI producers, joins room actions/import, then stops and joins the one
  authoritative sync source before client/store release.

## Live SDK timeline and decryption retry (v0.5.7)

- The Rust bridge owns persistent `matrix-sdk-ui` Timeline objects; item
  payloads that cross the FFI carry only UI-safe metadata (stable item
  id, event/transaction id, sender, timestamp, body plaintext the SDK
  already produced for display, send state, coarse UTD reason
  categories). Ciphertext, Megolm/Olm session keys, sender keys, device
  keys, and raw event JSON never cross the FFI.
- Post-import decryption retry keeps the imported Megolm **session
  identifiers** inside Rust (`RoomKeyImportResult.keys`, flattened to
  room → session-id lists; sender keys dropped). Only counts and
  already-public room IDs are surfaced to C++/QML. Session IDs are not
  logged; log lines carry counts and coarse categories only.
- Decrypted encrypted-room plaintext remains memory-only. The Rust
  backend does not use `CacheStore` at all, and `CacheStore` refuses
  `isEncrypted` rows on every write path — including retry-decrypted
  events, local echoes, edits, reply previews, and local-echo
  replacements. `tests/CacheStoreSecurityTest.cpp` scans the raw
  `cache.sqlite` bytes for a unique marker to lock this in.
- Stale-callback rejection is two-layered: Rust stamps every timeline
  event with a room generation and a lifecycle generation and stops
  forwarding when either advances; C++ additionally tracks the adopted
  generation per requested room and rejects mismatches. A diff from a
  previous room, a previous open of the same room, or a signed-out
  session cannot mutate visible state or repopulate the Login screen.
- Sign-out performs a deterministic managed-task shutdown
  (`mx_rust_shutdown_tasks`): the timeline subscription is aborted and
  joined, an in-flight room-key import is **joined** (bounded 15 s
  timeout only as a last-resort error boundary), sync stops, and only
  then is the client released and the store deleted. The store can no
  longer be deleted while a task still owns it; this replaces the
  v0.5.6 ~5 s `import_active` poll.
- Send failures surface as coarse categories ("network", "rejected");
  raw SDK/server error strings from the send path are not forwarded to
  the UI or logs. Failed-send retry uses the SDK send queue's unwedge on
  the same queued item, so a retry cannot duplicate a message.

## Session verification (v0.5.6)

- SAS verification is initiated with `VerificationMethod::SasV1` as the
  only advertised method, so the SDK never sends an `m.qr_code.*` request
  Lightning cannot follow through. Inbound and outbound flows share the
  same active-request / active-SAS slots, so exactly one verification is
  in flight at any moment.
- SAS emojis are safe to display and log by SAS design; Lightning still
  clears them from `AppController` on completion, cancellation, sign-out,
  and next-flow start so the QML cache never holds stale material.
- The label promoted to **Verified** comes solely from
  `Device::is_cross_signed_by_owner()`, queried through
  `mx_rust_query_own_device_status`. Local "they match" clicks do not
  set the label; the SDK snapshot does.
- Sign-out invalidates the active verification's lifecycle generation.
  A late `verification_done` / `verification_cancelled` callback from a
  previous session cannot mark the next login verified because the
  generation check in `pollRustEvents` rejects it, and
  `MatrixClient::loggedOut` clears the verification cache in
  `AppController`.
- Non-SAS verification (QR, in-room verification requests, arbitrary
  other-user verification, device deletion, cross-signing bootstrap /
  reset, Secure Secret Storage setup) is intentionally out of scope for
  0.5.6.

## Encrypted room-key import (v0.5.6)

- Decryption and import happen entirely inside
  `matrix-sdk::Encryption::import_room_keys`, which internally uses
  `matrix_sdk_base::crypto::decrypt_room_key_export` and wraps the
  passphrase in `zeroize::Zeroizing`. Lightning never implements PBKDF /
  AES / HMAC / base64 framing / MAC verification itself.
- The passphrase lifetime is one dispatch. QML clears its `TextField`
  after `app.importRoomKeys(...)`. C++ forwards a stack `QByteArray` and
  zeroes it before return. Neither QSettings, SecretStore, SQLite, nor
  a log ever sees it.
- Decrypted key material stays inside Rust. The FFI reply is only
  `{imported, total, affected_rooms, room_ids}`. `room_ids` are already
  public Matrix room identifiers, not room keys.
- No plaintext temporary file is created. Lightning does not write a
  decrypted copy to `/tmp` or into the app cache.
- The source export file is opened read-only by matrix-sdk. Lightning
  never modifies, renames, truncates, or deletes it.
- Only the active Rust crypto store is written. `CacheStore` still
  refuses encrypted `TimelineEvent` rows and the timeline reprocess path
  never inserts decrypted encrypted-room plaintext into the SQLite
  cache.
- Only one import runs at a time per Rust client. A second request while
  one is active is rejected via the atomic `import_active` flag and
  surfaces as an `already_running` failure, not a parallel decrypt.
- Sign-out joins an active import deterministically (v0.5.7
  `mx_rust_shutdown_tasks`; a bounded 15 s timeout remains only as a
  last-resort error boundary), so a stuck import can never permanently
  block sign-out and the store is never deleted mid-write. A late
  import completion for a released client is ignored by the generation
  check.
- Room-key import never modifies verification / cross-signing state.
  The UI shows "Not verified" side-by-side with "Import complete" as a
  deliberate signal that these are separate operations.

## Rust sign-out and local reset

- Explicit Rust sign-out captures one canonical account identity before
  clearing memory. It invalidates the old callback generation, cancels and
  joins sync, attempts Matrix logout, releases the Rust client, clears only
  that MXID's saved Lightning session, then removes only the account's
  `matrix-rust-sdk-store/` and existing smoke-session sidecar/temporary file.
- An `M_UNKNOWN_TOKEN` returned by logout or an invalidated sync during this
  intentional shutdown is expected and cannot become a footer error. The same
  authentication error from the active generation remains a real error.
- Login-screen reset derives the account in C++ from homeserver plus full MXID
  or localpart. `AppDataPaths` rejects malformed identities, traversal, empty
  slugs, broad parent targets, and symlinked account roots. QML never deletes
  files or constructs a path.
- Reset is idempotent and preserves `cache.sqlite`, unrelated account-local
  files, every other Lightning account, Element data, and server messages.
  A partial SecretStore/filesystem failure is reported and leaves reset
  available for retry.
- A subsequent password login may create a new Matrix device. The user may
  need Secure Backup recovery and/or SAS verification again. A recovery key
  unlocks backed-up Megolm room keys; it cannot recover messages whose room
  keys were never backed up or shared.

## Verification harness (v0.5.0-prep+4)

`--rust-sdk-smoke-test` (Rust-enabled build only) reads credentials
from environment variables and drives a live login/sync/probe-send
sequence against a real homeserver. Threat surface it deliberately
minimises:

- Credentials come from `LIGHTNING_TEST_HOMESERVER`,
  `LIGHTNING_TEST_USER`, `LIGHTNING_TEST_PASSWORD` — never CLI
  arguments — so `ps` / shell history / process listing never see a
  password.
- Output is `smoke:` lines with counts and statuses. Message
  bodies, access tokens, and Rust SDK crypto material are never
  printed. Matrix SDK error strings are trimmed to 240 characters
  and one line before being surfaced.
- A null `SettingsManager` is injected into `RustSdkMatrixClient`
  so a successful login response cannot overwrite the interactive
  user's `SecretStore` token, syncToken, or homeserver.
- v0.5.0-prep+5: the harness now writes the Rust SDK store to a
  fresh `QTemporaryDir` under `/tmp/lightning-rust-sdk-smoke-XXXXXX/`,
  not into the interactive user's persistent store. The temp
  directory is removed on process exit. The interactive Rust
  backend still uses the persistent per-account layout at
  `matrix::app_data::primaryRoot()/<safeUserId>/matrix-rust-sdk-store/`.
  `--reset-crypto-store` scans that root plus the pre-fix legacy
  root and never touches `/tmp/`.
- Persistent smoke mode is opt-in via
  `LIGHTNING_TEST_PERSISTENT_STORE=1`. It uses the same
  account-specific Rust SDK store as the interactive Rust backend and
  a smoke-only MatrixSession sidecar next to that account directory.
  The sidecar contains an access token, is written 0600 on Unix, is
  never printed, and is not QSettings/SecretStore state. It exists
  only to restore the same SDK device for encrypted receive testing.
- On the known Matrix SDK account/device mismatch, persistent smoke
  mode deletes only that account's Rust SDK store plus the smoke
  sidecar and retries once. It never deletes `cache.sqlite`,
  QSettings, or SecretStore entries.
- The harness only sends into non-encrypted, non-Space rooms when
  `LIGHTNING_TEST_SEND=1` is explicitly set. The smoke-only encrypted
  send probe requires `LIGHTNING_TEST_SEND_ENCRYPTED=1`, refuses
  non-encrypted rooms, and never prints the body. Interactive
  encrypted send remains gated by `supportsE2ee() == false`.

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
| Smoke MatrixSession sidecar | Only when `LIGHTNING_TEST_PERSISTENT_STORE=1`: `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-smoke-session.json`. Contains an access token, is 0600 on Unix, and is never printed. | Replace with a dedicated secure smoke credential store if this becomes more than a test harness. |
| Device id, user id, homeserver URL | QSettings (non-secret in themselves) | unchanged |
| Sync token | QSettings (restart-recoverable state, not a credential) | unchanged |
| Device keys | Rust SDK-owned state at `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/`, separate from `cache.sqlite`. Explicit Rust sign-out and the one-account Login reset remove it after releasing the SDK client. The CLI bulk repair can remove stores without touching session metadata. | New devices may require key-backup recovery and/or SAS verification. |
| Message content in transit | TLS to homeserver via `QNetworkAccessManager` (HTTP backend) or the Matrix Rust SDK transport (Rust backend). | Rust SDK encrypted transports once E2EE is verified |
| Encrypted message plaintext | HTTP backend does not decrypt. Rust displays plaintext only after matrix-sdk decrypts it. `CacheStore` skips every encrypted `TimelineEvent`, including successfully decrypted events, so encrypted-room plaintext is not written to `cache.sqlite`. | Held in-process until an explicit encrypted-cache design exists |
| Local cache | **SQLite** at `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/cache.sqlite`. Contains rooms, last 200 non-encrypted events per room, and members. **No** access tokens and no encrypted-room plaintext. Non-E2E timeline bodies are stored plaintext by design. | Same schema unless an encrypted-cache design is added |
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
- Does not claim cryptographic independence from matrix-sdk. All Rust-backend
  E2EE, recovery, and verification operations remain SDK-owned.
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
  `${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/matrix-rust-sdk-store/`.
- Explicit Rust sign-out and the Login reset delete only one validated
  account's Rust SDK store/session state. They never touch `cache.sqlite`,
  other accounts, Element data, or server messages. The legacy CLI reset is
  broader across Lightning account slugs but still deletes Rust stores only.
- `CacheStore` refuses to persist encrypted `TimelineEvent` rows, so
  decrypted encrypted-room message bodies remain memory-only for now.
- Raw access tokens are never logged.
- `clearSessionForAccount()` removes only the matching MXID's QSettings
  session metadata and SecretStore entry. The HTTP backend retains its
  existing separate cache-clear behavior; Rust reset preserves cache data.
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
