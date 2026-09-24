#pragma once

#include <QString>

// Quoting one path into a Desktop Entry `Exec=` field. Kept out of main.cpp
// so it can be unit-tested.
//
// Two escaping layers apply, and a reader undoes them in this order:
//
//   1. A desktop entry value is a "string" whose only valid escapes are
//      `\s \n \t \r \\`.
//   2. Then the Exec rule: inside a quoted argument, `"`, `` ` ``, `$` and
//      `\` are escaped with a backslash.
//
// So one literal backslash in a path needs four. A bare `\$` is an invalid
// string escape: GKeyFile then returns no Exec and the entry silently does
// nothing. `%` starts a field code and must be doubled.
namespace lightning::desktop_entry {

/// The quoted `Exec=` argument for `path`, or empty when it cannot be
/// represented. Control characters are refused (a newline would inject a
/// key); the caller treats empty as "do not publish".
QString quoteExecArgument(const QString &path);

} // namespace lightning::desktop_entry
