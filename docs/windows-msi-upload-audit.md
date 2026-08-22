# Windows MSI upload failure — source and packaging audit

For the report: **uploads fail when Lightning is installed from the MSI, while
the same version installed from the Setup EXE or extracted from the portable
ZIP uploads normally.**

**Status: NOT reproduced.** No Windows machine was available for this pass, so
nothing below is a validated fix. What it does is remove causes from the search
and make the next report say where it failed.

## What was ruled out

**The upload code is not different per package.** There is one send path. No
packaging format compiles a different one, and no install-type marker reaches
it — `.lightning-install-type` is read by the *updater* to choose an install
strategy, and by nothing on the attachment path.

**The payload was believed identical, and now it is asserted.** All three
Windows packages are produced from one staged tree (`build-windows.sh`), and the
MSI differs from the portable ZIP by exactly two markers: `portable.marker` is
added for the ZIP and removed immediately after, `.lightning-install-type` is
written for the MSI and removed before the NSIS build.

But "believed" was doing real work there. `validate-windows-artifacts.sh`
asserted the MSI contained `Lightning.exe`, `lightning-updater.exe` and the two
markers, and said **nothing about the other several hundred files**. A missing
Qt plugin does not stop Lightning launching — it removes one capability, so the
app starts, signs in and syncs, and then a single feature fails on a machine
where the other two formats work. That is the exact shape of this report.

So the validation now compares the MSI's File table against the portable ZIP's
contents and fails the build on any difference beyond those two markers. It was
proven against a real MSI built with `wixl` from a synthetic stage, with a
plugin removed: the check names the missing file. The parse handles wixl's
`SHORT|Long` `FileName` form, and `tests/test-msi-payload-completeness.py`
reads the awk program out of the validator so the two cannot drift.

This did **not** find a missing file in the current build. It means that if one
ever goes missing, CI says so instead of shipping it.

**Component-id collisions were checked and are not possible**: `wix_id()`
appends 12 hex characters of a SHA-256 of the full relative path, so two files
cannot collapse onto one component and silently drop.

## What remains plausible, and needs a Windows machine

Ordered by how well each fits "everything else works":

1. **A mixed or leftover install.** MSI and NSIS both target
   `%LOCALAPPDATA%\Programs\Lightning`. An MSI installed over an existing NSIS
   install (or the reverse) can leave files Windows Installer does not own, and
   a repair or upgrade can then remove or replace a subset of them. First test:
   uninstall everything, delete the directory, install the MSI alone.
2. **Windows Installer resiliency triggering a repair** mid-session, which can
   momentarily remove and reinstate files.
3. **Antivirus treating the MSI-installed binary differently** from the
   NSIS-installed one — different install provenance, same bytes.
4. A file-picker or temp-directory difference specific to how the process was
   launched (Start Menu shortcut created by MSI vs by NSIS).

## The diagnostics added for it

`AttachmentQueueModel::addFile()` refuses an attachment on five paths and used
to log none of them, so a failing user could report only the UI message.
Each now logs a stable reason token under `lightning.attach`:

```
attachment refused reason=unreadable exists=true isFile=true readable=false len=63 space=yes nonAscii=no unc=no
```

```sh
QT_LOGGING_RULES='lightning.attach=true' Lightning.exe
```

**No path is ever logged** — a Windows path contains the account name, and this
is the log a user is most likely to paste into a bug report. What is logged is
the path's *shape*: length, whether it contains spaces, whether it is non-ASCII,
whether it is UNC. Those are precisely the Windows path hazards worth
distinguishing, and none of them identifies the user.

`exists` / `isFile` / `readable` are reported separately on purpose: `exists`
true with `readable` false is a permissions or path-translation failure, which
is an environment problem, whereas all three false is a wrong path.

## The reproduction matrix, for whoever has the machine

1. Clean machine, no prior Lightning. Install the MSI. Sign in. Send: small
   image, large image, generic file, audio, video.
2. Repeat with a path containing spaces, and a Unicode filename.
3. Uninstall, reinstall, retest. Then install the Setup EXE on the same machine
   and account and compare directly.
4. Capture `QT_LOGGING_RULES='lightning.attach=true'` for any failure.

Never "fix" this by running as Administrator, disabling TLS validation, writing
user data into the install directory, or relaxing filesystem permissions.
