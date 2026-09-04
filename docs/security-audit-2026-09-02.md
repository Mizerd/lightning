# Security audit, 2026-09-02 — lessons and refuted hypotheses

Moved out of `CLAUDE.md` §16 on 2026-09-03 because that file had crossed the
150,000-character line past which agents silently lose its tail. **Read this
before touching the updater, the calls lane, the QML plain-text rule, or the
packaging pipeline's secret handling.** Everything here is binding in the
same way §16 is. The round itself — every finding, who fixed it, and the
validation — is in the Obsidian vault (`Lightning/Archive/Rounds/2026-09-02
Security audit round.md`) and in the two commit messages.

## A COMMENT SAYING "VOUCHED FOR BY OLM" IS NOT AN EXTRACTOR

The
MatrixRTC media-key handler read `ev.sender` and the code and
`docs/matrixrtc.md` both said the SDK vouched for it after Olm decryption.
The handler took no `EncryptionInfo`, and matrix-sdk 0.18 dispatches
PLAINTEXT to-device events to the same handler
(`event_handler/mod.rs:379-390`: `ProcessedToDeviceEvent::Decrypted` gives
`Some`, everything else `None`). So any Matrix user on any server could PUT an
unencrypted `io.element.call.encryption_keys` naming the victim's room, and a
homeserver could forge `sender` on it. Fixed 2026-09-02: the handler takes
`Option<EncryptionInfo>`, refuses `None`, keys the ring on the Olm-vouched
sender AND `sender_device`, and refuses a `claimed_device_id` that disagrees.
GENERALISE: when a to-device handler must trust the sender, the extractor is
the guarantee; a comment is a claim. Same round, same file: `membershipID`
was read from CONTENT as the SFU identity (the JWT service always assigns
`{user}:{device}`, so it is now DERIVED), and `created_ts`/`expires` were
unclamped content — `created_ts: 0` + `expires: u64::MAX` made an immortal,
always-oldest membership, and "oldest" chooses the SFU every later joiner
POSTs its OpenID token to. Both are now bounded against the ENVELOPE: `created_ts` within [event − 24 h, event + 5 min], deadline at most event + 24 h — never against `created_ts`, which would have expired every refresh of a call older than a day.

## THE UPDATER VERIFIED THE BYTES ONCE AND THEN TRUSTED A PATH

SHA-256 was
checked while streaming; the file then sat under the manifest's PREDICTABLE
name for as long as the user kept the app open on install-on-quit, and
`pkexec dpkg -i <path>` read it as ROOT after a PolicyKit prompt the user was
expecting anyway. The helper now takes `--sha256` and re-hashes right before
acting; the app re-hashes before every launch (`src/updater/ArtifactDigest.*`),
and `updater-helper-args` runs the REAL helper binary end to end. Also fixed:
no manifest freshness (a captured `latest` pair verified forever — every
manifest now carries a signed `expires`, generator default 120 days; **on
2026-09-03, at the maintainer's direction, a past or missing expiry became
INFORMATIONAL rather than a failure**, so clients keep updating from the
GitHub mirror if his servers die — see `docs/updates.md`), and the prerelease guard read the
manifest's OWN `channel` (a document calling itself "beta" walked past it —
the build decides now). A guard the guarded document can switch off is not a
guard.

## LEGACY 1:1 CALL SIGNALS MATCHED ON `(room, call_id)` ONLY

, and `call_id`
is a plaintext field of the invite every member can read: any member could
hang up a ringing call (a "missed call" from a non-caller), suppress it with
`select_answer`, or ANSWER an outbound call first and win the media session.
`isDirect` is not "1:1" either (a DM a third person was invited into is still
direct). Now: the session binds the remote user (invite sender / invitee /
first answer), candidates gate on the locked party id, and the legacy lane
needs exactly ONE `m.direct` target, named as `invitee`. matrix-js-sdk makes
the same check (`getSender() !== getOpponentMember()`).

## THE QML PLAINTEXT SWEEP HAD THREE BLIND SPOTS AND ~60 SINKS SURVIVED IT

—
one property hop (`text: root._label`), case-sensitive nouns, and a
brace-walker that dropped every `text: {` block binding uncounted. A sweep
that asks a `MenuItem` for `textFormat` produces a LOAD-TIME error that takes
every parent component down (CallStage went with it); `AppMenuItem`'s one
shared label carries the format for every menu row instead. And the
pipeline's own `test-release-notes-policy.py` ASSERTED that a prose mention of
"Code signing policy" suppressed the unsigned-Windows disclosure — a test can
codify the defect. Read what a passing assertion protects.

## THE SIGNING KEY WAS IN EVERY BUILD JOB'S ENVIRONMENT

A project-level
GitLab variable with no environment scope is injected into every job of the
pipeline, including six that run project-6 CMake and every `build.rs` in the
matrix-sdk graph; one hostile build script and the attacker IS the signer,
which no downstream hash check can contain. `sign-update-manifest` now
declares environment `signing`, the mirror job `mirror`, and `resolve-source`
checks the PUBLIC half only. **RESOLVED 2026-09-04, operator-side:** the
GitLab variable/environment scoping for `UPDATE_SIGNING_KEY_B64` /
`UPDATE_SIGNING_KEY_ID` (environment `signing`) and `GITHUB_MIRROR_TOKEN`
(`mirror`) was applied in project 7's settings. It lives in GitLab's own
configuration, not in any repository, so there is nothing to verify in tree
and nothing to change — do not reopen this. Same round:
`sh.rustup.rs` was the ONE unverified executable in the pipeline (now the
version-addressed `rustup-init` with a pinned SHA-256), every `image:` was a
mutable tag (now digests), the token-bearing API client followed redirects
over plaintext (curl re-sends custom headers cross-host; now `--max-redirs 0`),
and `attach-existing` on an old release re-pointed `latest` backwards (a
FREEZE; refused unless `UPDATE_ALLOW_LATEST_ROLLBACK=true`).

## TWO SESSIONS, ONE WORKING TREE

Every hard build failure in the audit
round was a race (the other session's ninja reading a half-saved file) or a
scripted edit that truncated a file (`s[:i] + new` with no `+ s[i:]`) — never
a logic error. Exclusive file ownership, one build lock, `git diff --stat`
after every scripted edit.

## The mock backend still violates §8, and cannot be fixed with a filter

Its room list is that backend's STORAGE and its thread view is DERIVED from
it, so a true thread reply cannot be filtered on the way in (the thread
empties) or on the way out (roots vanish, because `TimelineModel` derives
thread roots by COUNTING the replies in the list it was given — the same
shape that broke a first attempt at filtering the model). Enforcing §8 on the
mock needs it to emulate the SDK's thread SUMMARY fields, a fixture redesign.
The real backend enforces the invariant where a reply ENTERS the raw-sync
mirror (`RustSdkMatrixClient::handleTimelineEvent`), and the active room is
fed by SDK diffs with `hide_threaded_events`. A mock-driven QML test of "§8
in the live path" is measuring the limitation, not the product.

## A store passphrase would have bricked every install — refuted, do not re-propose

`ClientBuilder::sqlite_store(path, passphrase)` builds ONE `SqliteStoreConfig`
for the state, event-cache, media AND crypto stores, and matrix-sdk-sqlite
treats an absent `cipher` row as "mint a new one": an existing plaintext
store opens without complaint and then fails to decode its own account
pickle. The media cache is its own database in 0.18
(`matrix-sdk-media.sqlite3`) and `StoreConfig::media_store()` accepts an
independently opened store with its own key, composed through
`ClientBuilder::store_config` — the route for a later round (a per-account
key in the keyring, and `cross_process_store_config` reproduced by hand).
What shipped instead: encrypted-room media is not admitted to the SDK media
cache (`use_cache=false` for `MediaSource::Encrypted`), and the store
directory and files are 0700/0600.

## NOT TESTED from this round (both sessions)

- An ANSWERED legacy 1:1 call after the sender binding.
- A MatrixRTC call against Element after the key handler began requiring
  Olm-encrypted delivery (Element sends them encrypted per matrix-js-sdk's
  `encryptAndSendToDevice` — a reading, not a capture).
- A real update through the new `--sha256` helper contract, on each format.
- The SSO `/callback/<nonce>` redirect shape against a real homeserver's
  redirect validation; one live SSO sign-in settles it.
- A LAN-only or split-horizon SFU now being refused by policy
  (`focus_unroutable`).
- The scratch-directory lock protocol with two Lightning instances running
  side by side.
