# Security policy

## Reporting a vulnerability

Please report security problems privately, not in a public issue:

- GitHub: [Report a vulnerability](https://github.com/lightning-matrix/lightning/security/advisories/new)
- Email: <contact@lightning-matrix.org>

Include the version (`lightning-matrix --version`), your platform and package
format, and the steps to reproduce. You will get a reply within a few days.
Please give a reasonable time to ship a fix before disclosing it publicly.

## Supported versions

Only the latest release receives security fixes. The in-app updater and the
package repositories deliver them.

## Scope

Lightning uses the official Matrix Rust SDK for all end-to-end encryption.
Problems in the SDK itself belong upstream with
[matrix-org](https://github.com/matrix-org/matrix-rust-sdk/security); if you
are unsure where a problem lies, report it here and it will be forwarded.
