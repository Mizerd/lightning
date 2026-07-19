# Windows .exe / .msi packaging — blocker record (2026-07-19)

Windows installer formats were requested alongside Flatpak, AppImage, and
Snap. The three Linux formats are implemented; the two Windows formats are
**blocked** at the application and infrastructure level. This file records
the real state so nothing is falsely reported as working. No Windows CI job
is wired: a job with a `windows` tag would sit permanently stuck in the
queue, and a Linux runner must never be labelled as a Windows one.

## Blockers (all three must be resolved, in this order)

1. **No Windows secret storage in the application.** The source tree
   (`src/storage/`) implements exactly two SecretStore backends: libsecret
   (Freedesktop Secret Service) and the insecure QSettings fallback. There
   is no Windows Credential Manager / DPAPI backend, so a Windows build
   could persist the Matrix access token only through the plaintext
   fallback. That is not shippable. This is an upstream (project 6)
   feature decision, deliberately deferred until the Linux path is stable.

2. **No Windows runner.** The whole fleet (`/srv/gitlab-package-runners`)
   is Docker-outside-of-Docker on a Debian VM; it cannot build or validate
   Windows binaries. Resolving this means a dedicated Windows VM in the
   XCP-ng pool running gitlab-runner with the shell or docker-windows
   executor, registered as its own instance-scoped runner (suggested tags:
   `[windows, exe, package]` / a shared `windows` selector), following the
   same token-handoff workflow as the Linux fleet. A cross-compile
   (mingw/MSVC-wine) pipeline without a Windows host could produce
   binaries but could not honestly validate an installer, so it is not an
   acceptable substitute for release artifacts.

3. **No code-signing certificate.** No signing material exists among the
   project's CI variables. Unsigned installers trigger SmartScreen; a
   distributable exe/msi needs an Authenticode certificate stored as a
   protected+masked CI variable (never committed or logged) and a
   `signtool` step on the Windows runner. Until then any Windows build
   would have to ship explicitly marked as unsigned.

## Planned shape once unblocked (not implemented)

- Qt for Windows (MSVC) + the Rust bridge for `x86_64-pc-windows-msvc`,
  bundled with `windeployqt`; GIF keys embedded through the existing
  build-only environment-variable pattern.
- `packaging/windows/` with an NSIS (or Inno Setup) script producing
  `Lightning-<version>-setup-x64.exe` (primary, per-user installer) and a
  WiX project producing `Lightning-<version>-x64.msi` (enterprise/GPO
  deployment) from the same payload, sharing one signing step and a stable
  MSI upgrade code.
- Validation on the Windows runner: silent install, `--version` +
  offscreen-equivalent smoke run, GIF status/selftest, silent uninstall,
  clean-removal check.
- One manifest entry and one release link per format, exactly like the
  Linux formats ("Lightning <version> — Windows x64 installer" / "… MSI").

## Status

| Item | State |
| --- | --- |
| Windows SecretStore backend | **Blocked** — absent in project 6 |
| Windows runner | **Blocked** — no Windows host in the fleet |
| Code-signing certificate | **Blocked** — no signing material |
| exe/msi build, validation, publication | Not started (gated on all three) |
