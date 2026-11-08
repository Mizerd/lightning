#pragma once

#include <QString>

#include <functional>

namespace lightning::desktop_entry {

/// The quoted `Exec=` argument for `path`, or empty when it cannot be
/// represented. Control characters are refused (a newline would inject a
/// key); the caller treats empty as "do not publish".
QString quoteExecArgument(const QString &path);

/// `mimeapps` with `desktopId` added as the default for `mime`, or empty when
/// its [Default Applications] group already names one: the user's choice wins.
QString withDefaultHandler(const QString &mimeapps, const QString &mime,
                           const QString &desktopId);

/// Makes `desktopId` the default for `mime` in the mimeapps.list at `path`
/// unless one is already set there or `handledElsewhere` says another handler
/// exists. Never writes a file it could not read, nor through a symlink.
/// Returns a short outcome for the log.
QString claimDefaultHandler(const QString &path, const QString &mime,
                            const QString &desktopId,
                            const std::function<bool()> &handledElsewhere);

}
