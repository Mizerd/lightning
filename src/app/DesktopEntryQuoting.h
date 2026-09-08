#pragma once

#include <QString>

// Quoting one path into a Desktop Entry `Exec=` field.
//
// This lives in its own translation unit for one reason: it is the part of
// the AppImage launcher publication with concrete right and wrong answers,
// and `src/main.cpp` cannot be linked into a test because it defines `main`.
// The tests that guarded it were source scans asserting a literal appears
// somewhere in that very large file, and a review pointed out that several of
// them would pass on broken code. A pure function can simply be called.
//
// TWO ESCAPING LAYERS APPLY, IN ORDER, and applying only the second produces
// a file that silently does not launch:
//
//   1. A desktop entry value is a "string" whose only valid escapes are
//      `\s \n \t \r \\`. A reader undoes that FIRST.
//   2. Only then does the Exec rule apply: inside a quoted argument, `"`,
//      `` ` ``, `$` and `\` are escaped with a backslash.
//
// So an Exec-level backslash has to be written `\\`, and one literal
// backslash in a path needs FOUR. Writing `\$` straight into the file is not
// a valid string escape at all: GKeyFile reports an invalid escape sequence
// and `g_key_file_get_string` returns NULL, so the entry ends up with no
// `Exec` and the menu item does nothing, on the path the user chose, with no
// error anywhere.
//
// `%` introduces a field code and is expanded after unescaping, so a literal
// one must be doubled or `…/Down%20loads/…` silently becomes
// `…/Down0loads/…` when the reader drops the unknown code.
namespace lightning::desktop_entry {

/// The quoted `Exec=` argument for `path`, or an EMPTY string when it cannot
/// be represented honestly.
///
/// A control character is refused rather than encoded: a newline in the path
/// would inject a key into the file, and there is no correct quoting for it.
/// The caller treats an empty result as "do not publish", which is the only
/// safe answer.
QString quoteExecArgument(const QString &path);

} // namespace lightning::desktop_entry
