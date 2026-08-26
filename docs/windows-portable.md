# Windows x86_64 portable — what "portable" means and how it is built

Written 2026-08-21. Companion to `windows-packaging.md`, which covers the
cross-build itself; this document covers only the portable ZIP.

## The two halves of portable, and which one was broken

"Portable" is claimed in two independent senses. The distinction matters,
because only one of them was ever wrong.

**Runtime-portable** — the extracted folder runs on a clean Windows x86_64
machine with no Qt, FFmpeg, Rust, MinGW or developer tooling installed. This
was already true. `scripts/stage-windows-runtime.py` walks PE imports
recursively with `x86_64-w64-mingw32-objdump` against an explicit
`SYSTEM_DLLS` allowlist, and stages the Qt plugin set — `platforms/qwindows.dll`,
the TLS backends, image formats, `sqldrivers`, `networkinformation`, styles,
and the multimedia plugins including `ffmpegmediaplugin.dll`, whose own import
walk is what pulls in the FFmpeg runtime DLLs. Nothing about that changed.

**State-portable** — the whole folder can be moved to another path, drive or
PC and keeps the same settings, the same signed-in Matrix session and the
same Matrix device. **This was broken, in three separate places.**

## Why the old ZIP could not survive a move

1. **The access token was never in the ZIP.** `SecretStore::createDefault()`
   returns `WinCredStore` on Windows, and Windows Credential Manager entries
   are bound to the Windows **user and machine**. No amount of copying visible
   files — or exporting registry keys, which users tried — moves them. This is
   the whole of the "asks me to log in again on the new PC" report.
2. **Settings were in the registry.** A default-constructed `QSettings` uses
   `NativeFormat`, i.e. HKCU on Windows.
3. **Everything else was under `%LOCALAPPDATA%`** — the account records, the
   Matrix SDK store and the E2EE crypto store all resolve from one base
   directory, and on Windows that base was LocalAppData.

A fourth, quieter problem: the ZIP carried **no positive marker** saying it
was the portable package, so nothing could distinguish it from the MSI at
runtime.

## How portable mode is detected

A file named exactly `portable.marker` beside `Lightning.exe`. Presence is the
only signal; the contents are never parsed (they are addressed to the user).

Detection happens **before the first `QSettings` is constructed**, which is
the only moment it can happen: once a `QSettings` exists the registry has
already been touched, and redirecting afterwards would leave state in two
places. Concretely, `QGuiApplication` is constructed at `src/main.cpp:731`
while the first `QSettings()` is at `:710`, so the decision is made between
`setApplicationName()` and that line — where no Qt application instance exists
yet, and the executable's directory therefore has to come from
`GetModuleFileNameW` rather than `QCoreApplication::applicationDirPath()`.

Deleting `portable.marker` makes Lightning behave like an installed copy
again. The `data` folder is left alone but is no longer read.

## Layout

```text
Lightning/
    Lightning.exe
    lightning-updater.exe
    portable.marker
    qt.conf
    *.dll
    platforms/  imageformats/  tls/  multimedia/  sqldrivers/  qml/  ...
    gstreamer-1.0/   call media plugins, loaded from HERE by name
    data/
        config/     settings (INI, never the registry)
        matrix/     account records, Matrix SDK store, E2EE crypto store
        secrets/    the saved session
        cache/
        logs/
```

No Lightning-owned **persistent** state is written outside that tree in
portable mode: no registry keys, no `%APPDATA%`, no `%LOCALAPPDATA%`, no
Credential Manager.

One deliberate exception, stated precisely because the marker file makes a
claim to users: **ephemeral** media still uses the OS temporary directory
(`QDir::tempPath()`, i.e. `%TEMP%`, which on Windows normally lives under
`AppData\Local\Temp`). That is the decrypted payload of a video being played
and a voice message being recorded — session-scoped, `0600`, wiped on
sign-out, account switch and exit, and never read back on the next start. It
is deliberately NOT routed into the portable folder: nothing needs it to
travel, and writing decrypted media onto a USB stick would make an unwiped
crash worse rather than better. A crash can leave one behind, exactly as it
can for an installed build.

Windows itself still records unrelated things (SmartScreen, prefetch,
filesystem metadata). No application can prevent that, and this document does
not claim otherwise. The contract is about **Lightning-owned persistent
state**.

## Security: read this before recommending the portable build

The portable session file is encrypted, and **the key is in the same folder**.
It has to be — that is what makes the folder self-contained and movable, which
is the entire feature.

> **Possession of the complete portable directory is possession of the
> signed-in Matrix session and device.**

So the encryption is obfuscation against casual inspection, not protection
against someone who has the folder. Treat the folder like a password. On a USB
stick, treat losing the stick as losing the account until you sign that device
out from another client.

Owner-only file permissions are requested, but be clear about what that buys
on each platform. On Linux they are real POSIX bits. **On Windows they are
close to meaningless**: Qt's `setPermissions` does not write NTFS ACLs, so the
file inherits whatever the containing directory grants. And **FAT/exFAT — the
usual format for a USB stick — has no permission model at all.** Treat the
folder itself as the security boundary on every platform; the file bits are
not a second line of defence.

The installed MSI and Setup builds are unchanged and keep using Credential
Manager, which *is* machine-bound — that is the right trade for an install
that is not meant to travel.

## Copying the folder

**Close Lightning first, and confirm the process has exited.** Copying while
it runs can capture a SQLite store mid-transaction — including the crypto
store — and the copy may then be unusable. There is no way to make a live copy
reliable, so it is unsupported rather than merely discouraged.

## Build ordering, and the one thing that must not be edited carelessly

All three Windows packages come from **one** staged tree. `portable.marker` is
therefore written into the stage immediately before the ZIP is created and
removed immediately after, so that:

* the ZIP contains it — the extracted folder is portable;
* the MSI and NSIS payloads, built from the same stage afterwards, do not —
  an installed copy behaves exactly as it does today.

`scripts/build-windows.sh` spells this out at the call site because **neither
failure mode produces a build error**: moving the `zip` step below the
installer steps would silently make the ZIP non-portable *and* make both
installed builds claim to be portable.

## What validation actually checks

`scripts/validate-windows-artifacts.sh` runs against the **extracted final
ZIP** in a fresh temporary directory, not against the staging tree — "the
build directory works" is not evidence about the artifact users download. It
asserts the marker is present in the ZIP and absent from the MSI, that the
executable and every bundled PE is x86_64, that the Qt platform plugin, TLS,
multimedia and FFmpeg runtime are present, and that no absolute build path
leaks into packaged text files.

## Live validation status

The packaging and the path/mode logic are covered by automated tests on
Linux. **Everything that requires Windows is NOT TESTED**: clean-machine
startup, observing that no registry or AppData writes occur (Process Monitor),
Credential Manager absence, restart on the same PC, relocation to another path
or drive, the cross-PC move, and — the one that matters most — whether the
moved folder keeps the **same Matrix device id** and its encrypted-room state.

Do not describe the artifact as fully portable until that cross-machine test
has actually been run between two Windows environments.
