#pragma once

#ifdef ENABLE_RUST_SDK_BACKEND

// Headless verification harness for the Matrix Rust SDK backend.
//
// Reads credentials from the following environment variables so no
// password / token / body ever ends up on a command line, in a log,
// or in a script checked into the repo:
//
//   LIGHTNING_TEST_HOMESERVER   (required)  full https URL
//   LIGHTNING_TEST_USER         (required)  MXID or localpart
//   LIGHTNING_TEST_PASSWORD     (required)  never printed
//   LIGHTNING_TEST_PERSISTENT_STORE
//                               (optional)  "1" to reuse the account's
//                                           app-data Rust SDK store and a
//                                           smoke-only MatrixSession sidecar
//   LIGHTNING_TEST_SEND         (optional)  "1" to send one probe message
//   LIGHTNING_TEST_ROOM_ID      (optional)  override which room to send in
//   LIGHTNING_TEST_EXPECT_TEXT  (optional)  watch decrypted bodies for marker;
//                                           marker itself is never printed
//   LIGHTNING_TEST_REQUIRE_EXPECT
//                               (optional)  "1" to exit 14 if marker unseen
//   LIGHTNING_TEST_SEND_ENCRYPTED
//                               (optional)  "1" to run smoke-only encrypted
//                                           send probe
//
// Exit codes:
//   0   login + initial sync + >=1 joined room, plus send=ok when SEND=1
//   2   missing / invalid environment
//   10  login failed
//   11  initial sync did not complete in time
//   12  zero joined rooms observed
//   13  probe send failed (only when LIGHTNING_TEST_SEND=1)
//   14  expected marker not seen when REQUIRE_EXPECT=1
//   15  encrypted send probe failed / timed out
//
// Only counts / statuses go to stdout; the harness deliberately never
// prints tokens, passwords, message bodies, or crypto material.
int runRustSdkSmokeTest(int argc, char *argv[]);

#endif  // ENABLE_RUST_SDK_BACKEND
