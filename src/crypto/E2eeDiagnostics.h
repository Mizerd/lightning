#pragma once

#include <QLoggingCategory>
#include <QString>

// Safe E2EE recovery diagnostics, off by default:
//
//   QT_LOGGING_RULES="lightning.e2ee.debug=true" ./lightning-matrix --backend=rust
//
// Log only non-sensitive data: redactId() for room, event and session ids,
// never bodies, ciphertext, keys, recovery material or tokens.
Q_DECLARE_LOGGING_CATEGORY(lcE2ee)

namespace matrix::e2ee {

QString redactId(const QString &id);

}
