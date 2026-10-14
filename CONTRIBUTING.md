# Contributing to Lightning

Issues, focused patches, testing and bug reports are all welcome. Lightning is a
native Qt 6/QML Matrix client built on the official Rust Matrix SDK; the
repository's operating conventions live in [`CLAUDE.md`](CLAUDE.md), which is
worth skimming before a larger change.

The public home of the project is
<https://github.com/Mizerd/lightning>: clone it, file issues there, and download
releases from it. Development and releases are driven from the maintainer's own
GitLab, which is not open to the public and which GitHub is synchronised from
(force-pushed): anything committed directly to GitHub's own branches is
overwritten by the next push.

Anyone can clone the repository. There are two ways to send a change and
neither needs an account anywhere but GitHub — a pull request is the simpler of
the two.

## Reporting a bug

Open an issue on [GitHub](https://github.com/Mizerd/lightning/issues)
or email the maintainer. Say which version you are on (Settings → About, or
`lightning-matrix --version`), your platform, and what you expected against what
happened. For calling or media problems include the output of
`lightning-matrix --call-media-status`, and for a crash or freeze a log captured with
`lightning-matrix --console --log-file <path>`. Never paste tokens, recovery keys or
private message contents.

## Sending a change

### 1. A pull request on GitHub — the easiest way

Fork [Mizerd/lightning](https://github.com/Mizerd/lightning) on GitHub and open
a PR against `main`. You need nothing but a GitHub account, and you get a web UI
to review the diff in.

One thing to know so the outcome is not a surprise: the PR is read as a
**proposal**, and it is not merged on GitHub. Accepted commits are applied on
the maintainer's repository and reach GitHub through the synchronisation, so the
PR ends up **closed rather than merged** even when your change ships. Your authorship is preserved either
way — the commits keep your name and email, and you appear in the repository's
contributor list.

Do not push directly to the repository's own branches. GitHub is force-updated
from the maintainer's repository and such a commit is destroyed without warning.

### 2. A patch by email

If you would rather not use GitHub:

```sh
git clone https://github.com/Mizerd/lightning.git
cd lightning
# commit your work on top of main, in scoped commits
git format-patch origin/main --stdout > lightning-<topic>.patch
```

Send the `.patch` file to <antrasrokas@gmail.com> (`git send-email` works too).
It applies with `git am`, so your name and email stay on the commit as its
author.

If you expect to contribute more than once, ask the maintainer about direct
write access, which can be arranged on request.

## Build and test

Full instructions are in [`docs/build-and-test.md`](docs/build-and-test.md). The
short version, on Linux with the repository's Nix flake:

```sh
# the real client: Rust SDK backend, real Matrix, E2EE, threads, calls
nix develop -c cmake -S . -B build-rust -G Ninja -DENABLE_RUST_SDK_BACKEND=ON
nix develop -c cmake --build build-rust
nix develop -c ctest --test-dir build-rust --output-on-failure

# lighter tree with the mock backend, for UI work without a homeserver
nix develop -c cmake -S . -B build -G Ninja
nix develop -c cmake --build build
nix develop -c ctest --test-dir build --output-on-failure

# Rust-side tests
nix develop -c cargo test --manifest-path rust/Cargo.toml
```

Run the suites that cover what you changed; you are not expected to run
everything for a focused fix. Three QML/CTest suites (`timeline-pane-qml`,
`timeline-hydration-qml`, `media-bridge`) are load-sensitive and flake under high
parallelism — re-run a failure alone before treating it as a regression.

If your change touches anything behind the WebRTC guard in `src/calls/`, please
also configure with `-DLIGHTNING_ENABLE_WEBRTC=OFF` and build every target. No
package job builds that way any more, but it is the only build that compiles
the other half of those guards, and that configuration has broken releases
twice.

## What makes a change easy to accept

- **Keep it scoped.** One coherent change per commit; no unrelated cleanup,
  formatting sweeps or dependency bumps mixed in. Stage explicit files rather
  than `git add .`.
- **Add focused tests** where they give real regression coverage, and make sure
  the test fails against the unfixed code — a test that passes either way is
  decoration.
- **Report validation honestly.** State exactly what you ran and what you did
  not: **PASS**, **FAIL** or **NOT TESTED**. Compiling is not a GUI pass,
  launching is not feature validation, and automated tests are not live Matrix
  interoperability. An honest "not tested" costs nothing; an unsupported claim
  costs a release.
- **Explain the root cause**, not just the symptom, and mark anything that is
  still a hypothesis as one.
- Do not update `rust/Cargo.lock` or dependency pins incidentally.
- Do not run project-wide formatters or code generators.

## Security-sensitive areas

These are hard rules, and a change that crosses one will be sent back:

- All E2EE goes through the official Matrix SDK. Never implement custom Matrix
  cryptography, Olm/Megolm, SAS generation or key transfer, and never promote a
  local UI confirmation to SDK trust.
- Never expose access or refresh tokens to QML, and never log decrypted message
  bodies, recovery keys, room or session keys, passwords or tokens.
- Never commit credentials, provider API keys, private stores, session data or
  real conversations. `*.env` is gitignored — keep it that way.
- Do not weaken SSRF, DNS/IP, redirect, MIME, scheme, size or media-origin
  validation to make a test pass.
- Encrypted-room plaintext stays memory-only; it must not reach on-disk caches.

Changes to authentication, E2EE, credentials, persistence or deletion,
Rust/C++ FFI, dependencies or packaging get an explicit review pass before they
land, so expect questions on those.

## Licence and authorship

Lightning is free software under the **GNU General Public License v3.0 or
later**. By sending a patch you agree it may be distributed under those terms.
Contributions keep their author's name and email in the commit history; there is
no CLA and no copyright assignment.

Releases, tags and packaging are maintainer-side — please do not include version
bumps or release notes in a contribution.
