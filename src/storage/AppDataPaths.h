#pragma once

#include <QString>
#include <QStringList>

// Central resolver for the on-disk roots Lightning keeps per-account data
// under. Two callers must agree on this layout or things break subtly:
//
//   1. RustSdkMatrixClient — creates
//      <primaryRoot>/<safeUserId>/matrix-rust-sdk-store/ for real logins.
//   2. --reset-crypto-store (preflight in src/main.cpp) — deletes those
//      store directories WITHOUT constructing QGuiApplication first.
//
// Because reset runs before Qt is up, we cannot call
// `QStandardPaths::writableLocation(AppLocalDataLocation)` there. This
// helper computes the same path Qt would return, using only $HOME /
// $XDG_DATA_HOME and the compile-time OrganizationName / ApplicationName
// pair that `main.cpp` sets on `QCoreApplication`.
//
// The helper also lists LEGACY roots that earlier v0.5.0-prep+3 / +4
// builds wrote to (before this fix). Reset scans those as well so a
// migrated user gets everything cleaned up.
namespace matrix::app_data {

// Same path QStandardPaths::AppLocalDataLocation resolves to at runtime
// (with OrganizationName="MatrixClient" and ApplicationName="matrix-client").
// Returns empty when neither $HOME nor $XDG_DATA_HOME is set.
QString primaryRoot();

// Legacy roots that pre-fix builds may have created directories under.
// Currently just the "no org prefix" variant that the old reset code
// scanned. Never overlaps with `primaryRoot()`.
QStringList legacyRoots();

// primaryRoot() followed by legacyRoots(), deduplicated. Empty strings
// filtered out. Safe to call before QCoreApplication exists.
QStringList allRoots();

// For a given root, return absolute paths of every
// `<root>/<accountSlug>/matrix-rust-sdk-store` directory that currently
// exists. Never touches the filesystem beyond `stat` and one directory
// listing.
QStringList findRustStoresIn(const QString &root);

} // namespace matrix::app_data
