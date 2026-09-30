#!/usr/bin/env python3
"""The COPR source spec must not drift from the release RPM.

lightning.spec (release pipeline) and lightning-copr.spec.in (Fedora COPR,
mizerd/lightning) declare the same dlopen'd runtime dependencies by hand, and
the COPR build must configure CMake with the same fail-closed options as
configure-build.sh. Neither is visible to rpm's dependency generator or to a
build that happens to succeed. The copr-srpm CI job repeats the dependency
comparison on the specs as rpm expands them.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
REPO = os.path.join(ROOT, "..")
errors = []


def check(ok, message):
    if ok:
        print(f"  ok: {message}")
    else:
        sys.stdout.flush()
        print(f"  FAIL: {message}", file=sys.stderr)
        errors.append(message)


def read(*parts, base=ROOT):
    with open(os.path.join(base, *parts), encoding="utf-8") as fh:
        return fh.read()


def tags(spec, tag):
    return sorted(m.group(1).strip()
                  for m in re.finditer(rf"^{tag}:\s*(.+)$", spec, re.M))


def macros(spec):
    return sorted(m.group(0).strip()
                  for m in re.finditer(r"^%global (gst_element|qml_module)\(\).*$",
                                       spec, re.M))


release = read("packaging", "rpm", "lightning.spec")
copr = read("packaging", "rpm", "lightning-copr.spec.in")

# --- the same runtime dependencies ------------------------------------------
# desktop-file-utils serves only the release spec's %post scriptlet; Fedora's
# own file triggers refresh the desktop database for a repository install.
SCRIPTLET_ONLY = {"desktop-file-utils"}
for tag in ("Requires", "Recommends"):
    want = [r for r in tags(release, tag) if r not in SCRIPTLET_ONLY]
    check(len(want) >= (15 if tag == "Requires" else 2),
          f"the release spec still declares its {tag} ({len(want)})")
    check(want == tags(copr, tag),
          f"{tag} match: release {want} copr {tags(copr, tag)}")
# The lines above name macros; the macros must expand the same way too.
check(len(macros(release)) == 2 and macros(release) == macros(copr),
      "gst_element() and qml_module() are defined identically in both specs")

# --- distro-neutral, so one .rpm installs on Fedora and openSUSE -----------
_requires = "\n".join(tags(release, "Requires") + tags(release, "Recommends"))
for fedora_only in ("gstreamer1-plugins", "libnice-gstreamer1",
                    "pipewire-gstreamer", "enchant2"):
    check(fedora_only not in _requires,
          f"the release spec names no Fedora-only package ({fedora_only})")
for element in ("webrtcbin", "nicesrc", "opusenc", "vp8enc", "autoaudiosrc",
                "pipewiresrc"):
    check(f"%{{gst_element {element}}}" in _requires,
          f"the release spec requires the {element} element by capability")
# Every QML module the UI imports, by the capability each distro provides.
_qml_imports = set()
for dirpath, _dirs, files in os.walk(os.path.join(REPO, "qml")):
    for name in files:
        if name.endswith(".qml"):
            with open(os.path.join(dirpath, name), encoding="utf-8") as fh:
                for line in fh:
                    m = re.match(r"^import (Qt[A-Za-z.]+)\b", line)
                    if m:
                        _qml_imports.add(m.group(1))
check(len(_qml_imports) >= 5, f"found the UI's Qt QML imports ({sorted(_qml_imports)})")
for module in sorted(_qml_imports):
    check(f"%{{qml_module {module}}}" in _requires,
          f"the release spec requires the {module} QML module")
check("(qt6-qtimageformats or qt6-imageformats)" in _requires,
      "WebP comes from either distribution's image-format plugins")

# --- the same fail-closed build ----------------------------------------------
configure = read("scripts", "configure-build.sh")
for option in ("ENABLE_RUST_SDK_BACKEND=ON", "LIGHTNING_RUST_ONLY=ON",
               "LIGHTNING_ENABLE_WEBRTC=ON", "LIGHTNING_REQUIRE_WEBRTC=ON",
               "LIGHTNING_REQUIRE_QT_SVG=ON", "CMAKE_BUILD_TYPE",
               "BUILD_TESTING=OFF", "LIGHTNING_ARTIFACT_KIND=release"):
    check(option in configure, f"configure-build.sh still passes {option}")
    check(f"-D{option}" in copr, f"lightning-copr.spec.in passes -D{option}")
check("-DCMAKE_BUILD_TYPE=Release" in copr,
      "the COPR build is Release: CMake picks cargo --release only then")
# Both RPMs are portable: no Qt private-ABI import.
check("LIGHTNING_PORTABLE_QT_ABI=ON" in read("scripts", "build-rpm.sh"),
      "build-rpm.sh builds the release RPM with the portable Qt ABI")
check("-DLIGHTNING_PORTABLE_QT_ABI=ON" in copr,
      "the COPR build uses the portable Qt ABI")
# A COPR install is updated by dnf, never by the in-app installer.
check("-DLIGHTNING_INSTALL_TYPE=linux-rpm-repo" in copr,
      "the COPR build reports linux-rpm-repo to the updater")
check("LIGHTNING_INSTALL_TYPE=linux-rpm " in read("scripts", "build-rpm.sh"),
      "the release RPM stays linux-rpm (automatic install)")
# The update trust root is a public key, hard-coded like the Flathub manifest's.
_pub = re.search(r"-DLIGHTNING_UPDATE_PUBKEY_2026A=(\S+)", copr)
_flathub = re.search(r"-DLIGHTNING_UPDATE_PUBKEY_2026A=(\S+)",
                     read("org.lightning_matrix.Lightning.yaml", base=REPO))
check(_pub is not None and _flathub is not None
      and _pub.group(1) == _flathub.group(1),
      "the COPR build embeds the same update public key as the Flathub build")

# --- the two Fedora-only build fixes ----------------------------------------
_build = copr[copr.index("%build"):copr.index("%install")]
_cmake_at = _build.find("%cmake \\")
_unset_at = _build.find("unset LDFLAGS")
_rust_at = _build.find("%cmake_build --target matrix-client-rust-build")
_rest_at = _build.find("%cmake_build\n")
check(0 <= _cmake_at < _unset_at < _rust_at < _rest_at,
      "LDFLAGS is unset after %cmake and before cargo runs (aws-lc-sys probe)")
check(re.search(r"^export CARGO_BUILD_JOBS=2$", _build, re.M) is not None
      and _build.find("CARGO_BUILD_JOBS=2") < _rust_at,
      "the Rust target builds first, two crates at a time (8 GB builders)")

# No RPATH in either package: configure-build.sh strips the $ORIGIN ones
# qt_standard_project_setup() asks for; the COPR build never installs them,
# and its %check proves it as validate-rpm.sh does.
check('patchelf --remove-rpath "$STAGE_DIR/usr/bin/lightning-matrix"' in configure,
      "configure-build.sh still strips the RPATH from the release binary")
check("-DCMAKE_SKIP_INSTALL_RPATH=ON" in copr,
      "the COPR build installs its binaries without an RPATH")
check(re.search(r"readelf -d .*\n.*grep -E 'RPATH\|RUNPATH'", copr) is not None,
      "the COPR %check refuses an RPATH")

# The same fail-closed assertions configure-build.sh makes on its binary.
for needle in ("matrix_backend: rust", "http_backend_compiled: false",
               "mock_backend_compiled: false", "call media engine built in: yes"):
    check(needle in configure, f"configure-build.sh still asserts {needle!r}")
    check(needle in copr, f"lightning-copr.spec.in %check asserts {needle!r}")

# Never a GIF provider key: the source RPM is public.
check("GIPHY" not in copr and "KLIPY" not in copr,
      "lightning-copr.spec.in carries no GIF provider keys")

# The source RPM step: COPR's make_srpm entry point, world-readable output.
_srpm = read("scripts", "build-copr-srpm.sh")
check(re.search(r"^umask 022$", _srpm, re.M) is not None,
      "build-copr-srpm.sh sets umask 022 (a 0000 umask made 0666 files)")
check("git config --global" not in _srpm,
      "build-copr-srpm.sh writes nobody's git configuration")
_makefile = read(".copr", "Makefile", base=REPO)
check("packaging-ci/scripts/build-copr-srpm.sh" in _makefile
      and re.search(r"^srpm:", _makefile, re.M) is not None,
      ".copr/Makefile has the srpm target COPR's make_srpm method calls")

if errors:
    print(f"\nCOPR spec parity tests FAILED ({len(errors)})", file=sys.stderr)
    sys.exit(1)
print("COPR spec parity tests passed")
