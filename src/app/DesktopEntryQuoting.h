#pragma once

#include <QString>

namespace lightning::desktop_entry {

/// The quoted `Exec=` argument for `path`, or empty when it cannot be
/// represented. Control characters are refused (a newline would inject a
/// key); the caller treats empty as "do not publish".
QString quoteExecArgument(const QString &path);

}
