# Windows signing inventory

What Lightning will sign, and what it will deliberately **not** sign, once
SignPath Foundation code signing is active. Nothing described here is signed
today — see [`docs/code-signing-policy.md`](code-signing-policy.md).

The machine-readable form of this document is
`packaging/windows/signing-inventory.json` in
[lightning-deploy](https://gitlab.smetonis.net/Mizerd/lightning-deploy), which is
enforced during packaging by `scripts/verify-windows-metadata.py`. If the two
disagree, the packaging job fails.

## The Windows release set

Every Lightning release publishes three Windows x86-64 artifacts, all built from
one source commit:

| Artifact | Role | Eventually signed? |
|---|---|---|
| `Lightning-<version>-<sha>-windows-x86_64-portable.zip` | Portable, no installation | The ZIP is a container and is not Authenticode-signable; the **payload inside it is signed** |
| `Lightning-<version>-<sha>-windows-x86_64.msi` | Windows Installer package | **Yes** — the MSI itself, after its signed payload is embedded |
| `Lightning-<version>-<sha>-windows-x86_64-setup.exe` | NSIS setup program | **Yes** — the setup EXE itself, after its signed payload is embedded |

Ordering matters: **the application payload is signed before the installers are
built**, so the signature is inside the MSI and the setup EXE rather than being
applied only to the outer wrapper. The existing packaging script already signs
the staged executable before the ZIP, MSI, and NSIS steps run.

## Lightning-owned — intended to be signed

| File | What it is |
|---|---|
| `Lightning.exe` | The application. Built from this repository by CMake as `matrix-client.exe` and staged under its shipped name `Lightning.exe`. Its version resource declares `ProductName=Lightning` and the canonical release version |

That is the whole list. Lightning ships **no** helper executables and **no**
Lightning-authored DLLs on Windows: the C++ application, the QML, and the Rust
bridge are all linked into that single PE.

## Upstream / system — never signed as Lightning

These are redistributed exactly as their upstream projects built them. Signing
them with a Lightning certificate would misrepresent someone else's binary as
ours, which SignPath Foundation explicitly forbids.

| Group | Examples |
|---|---|
| Qt 6 runtime | `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Network.dll`, `Qt6Qml.dll`, `Qt6Quick.dll`, `Qt6Multimedia.dll`, `Qt6Svg.dll`, and the rest of the staged Qt libraries |
| Qt plugins | `plugins/platforms/qwindows.dll`, `plugins/multimedia/{ffmpeg,windows}mediaplugin.dll`, `plugins/imageformats/*.dll`, `plugins/tls/*.dll`, `plugins/sqldrivers/qsqlite.dll`, `plugins/styles/qmodernwindowsstyle.dll`, `plugins/iconengines/qsvgicon.dll`, `plugins/networkinformation/*.dll` |
| Qt QML module plugins | `qml/**/**.dll` (for example `qml/QtMultimedia/quickmultimediaplugin.dll`) |
| FFmpeg | `avcodec-*.dll`, `avformat-*.dll`, `avutil-*.dll`, `swresample-*.dll`, `swscale-*.dll` |
| MinGW-w64 runtime | `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` |
| Other upstream libraries pulled in by the dependency walk | for example zlib, brotli, freetype, harfbuzz, pcre2, libpng, zstd |

**Operating-system libraries** (`kernel32.dll`, `user32.dll`, `crypt32.dll`,
`bcrypt.dll`, `d3d11.dll`, the `api-ms-win-*` set, and the rest of the list in
`scripts/stage-windows-runtime.py`) are **never redistributed at all** — they are
resolved from Windows itself. They fall under the System Library exception and
are not part of any signing decision.

The NSIS-generated `Uninstall.exe` is written by NSIS on the user's machine at
install time. It does not exist in the release artifacts and therefore cannot be
signed as part of the release.

## The signing boundary

```text
canonical GitLab source commit (Mizerd/lightning)
        │
        ▼
automated GitLab Windows build          ← no developer workstation, ever
        │
        ├── Lightning.exe               ← the ONLY Lightning-owned PE
        │
        ▼
GitLab pipeline artifact (unsigned payload, checksummed)
        │
        ▼
SignPath signing request  ── manual approval by the maintainer
        │
        ▼
signed Lightning.exe
        │
        ├─► portable ZIP        (container; signed payload inside)
        ├─► MSI                 (signed payload inside, then MSI signed)
        └─► setup EXE           (signed payload inside, then EXE signed)
        │
        ▼
structural validation + metadata verification
        │
        ▼
publish to the GitLab Package Registry and attach to the Release
```

The provenance requirements behind that first arrow are documented in
[`docs/signpath-build-provenance.md`](signpath-build-provenance.md).

## Metadata enforced on the signed binary

SignPath applies file metadata restrictions to signed artifacts, so packaging
verifies them *before* anything is submitted. `verify-windows-metadata.py` parses
the real PE version resource of every staged binary and fails the build unless:

- `Lightning.exe` declares `ProductName = Lightning`;
- its `ProductVersion` and `FileVersion` equal the canonical release version
  (the one CMake declares — see [`docs/signpath-build-provenance.md`](signpath-build-provenance.md));
- its `CompanyName`, `LegalCopyright`, `FileDescription` and `OriginalFilename`
  are present and match the packaging contract;
- **no other staged PE claims `ProductName = Lightning`**, so an upstream DLL can
  never be mistaken for — or signed as — a Lightning binary;
- every staged PE is accounted for in `signing-inventory.json` as either
  Lightning-owned or upstream.
