# Windows cross-package builder

This directory defines an **unsigned Linux cross-build**, not a native Windows
build. The content-addressed Fedora 44 image pins MinGW GCC 16.1.1, Qt 6.11.1,
Rust 1.95.0, NSIS 3.11, msitools/wixl 0.106.58, and Wine 11. Fedora's MinGW Qt
multimedia package omits the `QtMultimedia` QML plugin, so the image builds only
that missing module from the official Qt 6.11.1 source tarball after verifying
its SHA-256 checksum. The target remains `x86_64-pc-windows-gnu` throughout.

`installer.nsi` creates a per-user setup executable. The MSI generator uses a
stable UpgradeCode, a ProductCode derived from application version plus source
commit, and component GUIDs derived from normalized staged paths. Both
installers preserve application user data during uninstall.

The runtime stager copies the target Qt QML tree, selected runtime plugins,
and then closes the DLL import graph recursively. It does not copy the Qt SDK.
`validate-windows-artifacts.sh` checks PE architecture/import closure, required
QML/plugins, MSI tables, forbidden paths/secrets, and hashes. Wine tests are
supplemental only; native Windows behavior remains untested.
