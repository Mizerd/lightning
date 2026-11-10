#pragma once

#include <QString>

#include <functional>

namespace lightning::desktop_entry {

/// The quoted `Exec=` argument for `path`, or empty when it cannot be
/// represented. Control characters are refused (a newline would inject a
/// key); the caller treats empty as "do not publish".
QString quoteExecArgument(const QString &path);

/// `body` plus a last `X-Lightning-Generated=` line carrying its SHA-256.
QString withGeneratedMarker(const QString &body);

/// Whether `entry` is still exactly what Lightning wrote: its marker matches
/// the rest, or it is byte for byte what 0.9.3 to 0.10.5 wrote. Anything else
/// is the user's and must not be overwritten.
bool isUntouchedGeneratedEntry(const QString &entry);

/// Lowercase hex SHA-256 of the file at `path`, empty when unreadable.
QString fileSha256(const QString &path);

/// `mimeapps` with `desktopId` added as the default for `mime`, or empty when
/// its [Default Applications] group already names one: the user's choice wins.
QString withDefaultHandler(const QString &mimeapps, const QString &mime,
                           const QString &desktopId);

/// Whether an `xdg-mime query default` result means another handler owns the
/// scheme. Fails closed: a query that did not exit normally with status 0
/// counts as another handler.
bool queryNamesAnotherHandler(bool exitedNormally, int exitCode,
                              const QString &output, const QString &ours);

/// Makes `desktopId` the default for `mime` in the mimeapps.list at `path`
/// unless one is already set there or `handledElsewhere` says another handler
/// exists. Never writes a file it could not read, nor through a symlink.
/// Returns a short outcome for the log.
QString claimDefaultHandler(const QString &path, const QString &mime,
                            const QString &desktopId,
                            const std::function<bool()> &handledElsewhere);

}
