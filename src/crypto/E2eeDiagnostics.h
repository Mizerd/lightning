#pragma once

#include <QLoggingCategory>
#include <QString>

// v0.6.1: safe E2EE recovery diagnostics.
//
// The category is OFF by default (a qCDebug/qCInfo category) so it never spams
// normal operation; a live tester enables it with, e.g.:
//
//   QT_LOGGING_RULES="lightning.e2ee.debug=true" ./matrix-client --backend=rust
//
// Everything logged through it must be non-sensitive: use redactId() for room,
// event, and session identifiers so the log correlates the recovery lifecycle
// without ever revealing a full Matrix id, and never pass message bodies,
// ciphertext, keys, recovery material, or tokens.
Q_DECLARE_LOGGING_CATEGORY(lcE2ee)

namespace matrix::e2ee {

// Reduce a Matrix identifier (room !…, event $…, user @…, session id, …) to a
// short, STABLE, NON-REVERSIBLE token safe to log. The leading Matrix sigil is
// preserved as a type hint; the remainder is replaced by a truncated hash, so
// the same id always maps to the same token (for correlation) but the original
// value cannot be recovered from the log. An empty id maps to a fixed marker.
QString redactId(const QString &id);

} // namespace matrix::e2ee
