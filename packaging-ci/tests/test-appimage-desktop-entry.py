#!/usr/bin/env python3
"""The launcher entry and the icons it names — the window icon on Wayland.

Reported against 0.9.x: after updating, the AppImage's window and taskbar icon
is a generic placeholder. The cause is not the artwork and not the payload.

Qt's Wayland client implements NO icon protocol — `xdg_toplevel_icon` appears
zero times in libQt6WaylandClient — so QGuiApplication::setWindowIcon() is inert
on a native Wayland session. The compositor's only route to an icon is the
toplevel's app id ("lightning"), which it resolves to `lightning.desktop` in
XDG_DATA_HOME/XDG_DATA_DIRS and then reads the Icon= key of. Under X11 and
XWayland the same setWindowIcon() call sets _NET_WM_ICON and the icon is right,
which is precisely why this hid for years: AppImages up to 0.9.0 shipped without
wayland-shell-integration, so Qt refused its own Wayland plugin and ran under
XWayland. Staging that plugin moved them onto native Wayland and the icon went
generic.

Two halves have to hold, and nothing asserted either of them:

  * the payload must carry a launcher entry that NAMES an icon, and the icon
    files it names (assert_desktop_launcher_payload / assert_appdir_root_icon);
  * a single-file bundle installs nothing, so the app must publish a copy of
    that entry where the session looks, and the shipped bundle must be asked
    whether it did (assert_desktop_status).

This exercises the real functions from scripts/lib.sh against synthetic trees
and transcripts, positive AND negative, and then runs the payload assertion over
the repository's own tracked desktop entries — which is what would have caught
packaging/common/lightning.desktop shipping with no Icon= key at all from 0.7
through 0.9.2.
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
# packaging-ci/, NOT the application repository root. Both trees carry a
# scripts/ and a tests/ directory since the packaging project was folded in, so
# a path that walks up one level too far finds a real directory with none of
# these files in it.
PACKAGING_ROOT = HERE.parent
APP_ROOT = PACKAGING_ROOT.parent
LIB = PACKAGING_ROOT / "scripts" / "lib.sh"

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if condition:
        print(f"  ok: {message}")
    else:
        print(f"  FAIL: {message}", file=sys.stderr)
        failures.append(message)


def run_helper(call: str) -> subprocess.CompletedProcess:
    """Run one lib.sh assertion in a real bash, exactly as a job would."""
    script = f'. "{LIB}"\n{call}\n'
    return subprocess.run(
        ["bash", "-c", script],
        capture_output=True,
        text=True,
        cwd=str(PACKAGING_ROOT),
    )


def expect_pass(call: str, message: str) -> None:
    result = run_helper(call)
    check(result.returncode == 0,
          f"{message} (exit {result.returncode}: {result.stderr.strip()})")


def expect_fail(call: str, needle: str, message: str) -> None:
    """A negative case is only evidence if it fails for the RIGHT reason."""
    result = run_helper(call)
    if result.returncode == 0:
        check(False, f"{message} — but it PASSED")
        return
    check(needle in result.stderr,
          f"{message} — and says so ({needle!r} in the error)")


# --- a payload tree, built to spec and then broken one key at a time ---------

GOOD_ENTRY = """[Desktop Entry]
Type=Application
Name=Lightning
Exec=lightning-matrix --backend=rust
Icon=lightning
Terminal=false
Categories=Network;Chat;InstantMessaging;
StartupNotify=true
StartupWMClass=lightning-matrix
"""

ICON_SIZES = (16, 32, 48, 64, 128, 192, 256, 512)


def make_tree(base: pathlib.Path, entry: str | None = GOOD_ENTRY,
              sizes=ICON_SIZES, scalable: bool = True,
              root_icon: bool = True) -> pathlib.Path:
    tree = base
    apps = tree / "usr/share/applications"
    apps.mkdir(parents=True, exist_ok=True)
    if entry is not None:
        (apps / "lightning.desktop").write_text(entry, encoding="utf-8")
    for size in sizes:
        d = tree / f"usr/share/icons/hicolor/{size}x{size}/apps"
        d.mkdir(parents=True, exist_ok=True)
        (d / "lightning.png").write_bytes(b"\x89PNG\r\n\x1a\n")
    if scalable:
        d = tree / "usr/share/icons/hicolor/scalable/apps"
        d.mkdir(parents=True, exist_ok=True)
        (d / "lightning.svg").write_text("<svg/>", encoding="utf-8")
    if root_icon:
        (tree / "lightning.desktop").write_text(entry or GOOD_ENTRY,
                                                encoding="utf-8")
        (tree / "lightning.png").write_bytes(b"\x89PNG\r\n\x1a\n")
        (tree / ".DirIcon").write_bytes(b"\x89PNG\r\n\x1a\n")
    return tree


print("payload: a complete tree passes")
with tempfile.TemporaryDirectory() as tmp:
    tree = make_tree(pathlib.Path(tmp) / "t")
    expect_pass(f'assert_desktop_launcher_payload Probe "{tree}"',
                "a complete payload passes")
    expect_pass(f'assert_appdir_root_icon Probe "{tree}"',
                "a complete AppDir root passes")

print("payload: each way it can be wrong is caught, by name")
with tempfile.TemporaryDirectory() as tmp:
    base = pathlib.Path(tmp)

    tree = make_tree(base / "no-entry", entry=None)
    expect_fail(f'assert_desktop_launcher_payload Probe "{tree}"',
                "no launcher entry",
                "a payload with no launcher entry is refused")

    # THE EXACT SHAPE packaging/common/lightning.desktop shipped in for four
    # releases: a valid, validating desktop entry that names no icon.
    tree = make_tree(base / "no-icon-key",
                     entry=GOOD_ENTRY.replace("Icon=lightning\n", ""))
    expect_fail(f'assert_desktop_launcher_payload Probe "{tree}"',
                "no Icon= key",
                "an entry with no Icon= key is refused")

    tree = make_tree(base / "wrong-icon",
                     entry=GOOD_ENTRY.replace("Icon=lightning",
                                              "Icon=application-x-executable"))
    expect_fail(f'assert_desktop_launcher_payload Probe "{tree}"',
                "it must be \"lightning\"",
                "an entry naming somebody else's icon is refused")

    tree = make_tree(base / "no-wmclass",
                     entry=GOOD_ENTRY.replace(
                         "StartupWMClass=lightning-matrix\n", ""))
    expect_fail(f'assert_desktop_launcher_payload Probe "{tree}"',
                "StartupWMClass",
                "an entry with no StartupWMClass is refused")

    tree = make_tree(base / "no-256", sizes=(16, 32, 48, 64, 128, 192, 512))
    expect_fail(f'assert_desktop_launcher_payload Probe "{tree}"',
                "256x256",
                "a missing raster icon size is refused")

    tree = make_tree(base / "no-svg", scalable=False)
    expect_fail(f'assert_desktop_launcher_payload Probe "{tree}"',
                "scalable",
                "a missing scalable icon is refused")

    tree = make_tree(base / "no-diricon")
    (tree / ".DirIcon").unlink()
    expect_fail(f'assert_appdir_root_icon Probe "{tree}"',
                ".DirIcon",
                "an AppDir with no .DirIcon is refused")


# --- the runtime transcript ---------------------------------------------------

GOOD_STATUS = """qt version: 6.8.2
app id (desktop file name): lightning
launcher entry basename: lightning.desktop
wm class: lightning-matrix
appimage runtime: yes
appimage: /tmp/Lightning-0.9.2-x86_64.AppImage
appdir: /tmp/squashfs-root
payload launcher entry: /tmp/squashfs-root/usr/share/applications/lightning.desktop
payload entry icon name: lightning
payload icon files: 9
user icon files copied: 9
user launcher entry: /home/probe/data/applications/lightning.desktop
launcher entry: written
data dirs searched: 2 (/home/probe/data /home/probe/empty)
visible launcher entry: /home/probe/data/applications/lightning.desktop
visible icon: /home/probe/data/icons/hicolor/512x512/apps/lightning.png

RESULT: the app id "lightning" resolves to a launcher entry and an icon this session can find.
"""


def status_case(base: pathlib.Path, name: str, text: str) -> pathlib.Path:
    path = base / name
    path.write_text(text, encoding="utf-8")
    return path


print("runtime: a good --desktop-status transcript passes, a bad one names why")
with tempfile.TemporaryDirectory() as tmp:
    base = pathlib.Path(tmp)

    log = status_case(base, "good.txt", GOOD_STATUS)
    expect_pass(f'assert_desktop_status Probe "{log}" 0 appimage',
                "a published entry and a resolvable icon pass")

    # THE REPORTED DEFECT, as the transcript would read.
    log = status_case(base, "invisible.txt", GOOD_STATUS
                      .replace("launcher entry: written",
                               "launcher entry: skipped: not an AppImage run")
                      .replace(
                          "visible launcher entry: "
                          "/home/probe/data/applications/lightning.desktop",
                          "visible launcher entry: NONE")
                      .replace("RESULT: the app id",
                               "RESULT: this session cannot resolve the app id"))
    expect_fail(f'assert_desktop_status Probe "{log}" 1 appimage',
                "did not publish a launcher entry",
                "a bundle that publishes nothing is refused")

    log = status_case(base, "no-icon.txt",
                      GOOD_STATUS.replace(
                          "visible icon: "
                          "/home/probe/data/icons/hicolor/512x512/apps/"
                          "lightning.png",
                          "visible icon: NONE"))
    expect_fail(f'assert_desktop_status Probe "{log}" 0 appimage',
                "names an icon that is not installed",
                "an entry naming an uninstalled icon is refused")

    log = status_case(base, "wrong-id.txt",
                      GOOD_STATUS.replace(
                          "app id (desktop file name): lightning",
                          "app id (desktop file name): lightning-matrix"))
    expect_fail(f'assert_desktop_status Probe "{log}" 0 appimage',
                "app id",
                "an app id that no entry can match is refused")

    log = status_case(base, "not-appimage.txt",
                      GOOD_STATUS.replace("appimage runtime: yes",
                                          "appimage runtime: no"))
    expect_fail(f'assert_desktop_status Probe "{log}" 0 appimage',
                "never even tried",
                "a bundle that did not see APPIMAGE/APPDIR is refused")

    # Text and exit status disagreeing means one of the two is lying.
    log = status_case(base, "status-mismatch.txt", GOOD_STATUS)
    expect_fail(f'assert_desktop_status Probe "{log}" 1 appimage',
                "exited 1",
                "a success transcript with a failing exit status is refused")

    log = status_case(base, "empty.txt", "")
    expect_fail(f'assert_desktop_status Probe "{log}" 0 appimage',
                "no output at all",
                "an empty transcript is refused (an older source has no flag)")


# --- the repository's own tracked entries ------------------------------------

print("tracked: the desktop entries this repository actually ships")
with tempfile.TemporaryDirectory() as tmp:
    base = pathlib.Path(tmp)
    icons = APP_ROOT / "data/icons"
    if not (APP_ROOT / "data/lightning.desktop").is_file():
        check(False, f"data/lightning.desktop not found under {APP_ROOT}")
    else:
        for label, entry in (
            ("source", APP_ROOT / "data/lightning.desktop"),
            ("packaging fallback",
             PACKAGING_ROOT / "packaging/common/lightning.desktop"),
        ):
            tree = base / label.replace(" ", "-")
            apps = tree / "usr/share/applications"
            apps.mkdir(parents=True)
            shutil.copyfile(entry, apps / "lightning.desktop")
            shutil.copytree(icons / "hicolor",
                            tree / "usr/share/icons/hicolor")
            scalable = tree / "usr/share/icons/hicolor/scalable/apps"
            scalable.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(icons / "lightning.svg",
                            scalable / "lightning.svg")
            expect_pass(
                f'assert_desktop_launcher_payload "{label}" "{tree}"',
                f"the tracked {label} launcher entry satisfies the contract")

if failures:
    print(f"\n{len(failures)} check(s) failed", file=sys.stderr)
    sys.exit(1)
print("\nappimage desktop-entry checks passed")
