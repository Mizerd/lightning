#!/usr/bin/env python3
"""The setup EXE must refuse to touch an install whose Lightning.exe runs.

Source-level assertions (a real uninstall only runs on Windows): the guard
exists for both the installer and the uninstaller, runs BEFORE anything is
removed or written, is Retry/Cancel interactively and a non-zero exit when
silent, and uses no plugin the builder image lacks (System and nsDialogs only).
Regression: the uninstaller used to delete the shortcuts and registry entries
first and let `RMDir /r` skip the locked files.
"""
import pathlib
import re
import sys

NSI = (pathlib.Path(__file__).resolve().parent.parent
       / "packaging" / "windows" / "installer.nsi").read_text()
failures = []


def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        failures.append(msg)


def body(start_marker, end_marker):
    a = NSI.index(start_marker)
    return NSI[a:NSI.index(end_marker, a)]


check("!insertmacro LIGHTNING_RUNNING_GUARD \"\"" in NSI
      and "!insertmacro LIGHTNING_RUNNING_GUARD \"un.\"" in NSI,
      "the guard is generated for the installer and the uninstaller")
check("!define EXIT_APP_RUNNING 32" in NSI, "a distinct exit code is defined")

guard = body("!macro LIGHTNING_RUNNING_GUARD", "!macroend")
check("MB_RETRYCANCEL" in guard, "interactive: Retry / Cancel")
check("${If} ${Silent}" in guard and "SetErrorLevel ${EXIT_APP_RUNNING}" in guard,
      "silent: quits with the non-zero code")
check("kernel32::CreateFileW" in guard and "$3 != 32" in guard,
      "only a sharing violation counts as running")

# The uninstaller checks before the Uninstall section removes anything.
unonit = body("Function un.onInit", 'Section "Uninstall"')
check("Call un.RequireLightningClosed" in unonit, "un.onInit calls the guard")
uninstall_section = NSI[NSI.index('Section "Uninstall"'):]
first_removal = min(uninstall_section.index(x) for x in ("Delete ", "DeleteRegKey", "RMDir"))
check(0 <= uninstall_section.find("Call un.RequireLightningClosed") < first_removal
      and all(x not in unonit for x in ("Delete ", "DeleteRegKey", "RMDir")),
      "the Uninstall section re-checks before it removes anything")
check(unonit.rindex("Call un.RequireLightningClosed")
      > unonit.rindex("LIGHTNING_RUN_ELEVATED"),
      "an unelevated per-machine copy hands over before the guard")

# The installer checks before any file is written.
oninit = body("Function .onInit", "; The elevated copy was started")
check("Call RequireLightningClosed" in oninit, ".onInit calls the guard")
check("File /r" not in oninit, "no payload is written in .onInit")
sec = NSI[NSI.index('Section "Lightning application'):]
check(0 <= sec.find("Call RequireLightningClosed") < sec.index("File /r"),
      "SEC_APP re-checks before the payload is written")
leave = body("Function ModePageLeave", "; Install\n")
check(leave.index("Call ApplyScope") < leave.index("Call RequireLightningClosed"),
      "choosing 'Just for me' checks the per-user directory")

plugins = set(re.findall(r"\b(\w+)::\w+", NSI)) - {"kernel32", "shell32"}
check(plugins <= {"System", "nsDialogs"},
      f"no plugin beyond System and nsDialogs ({sorted(plugins)})")

sys.exit(1 if failures else 0)
