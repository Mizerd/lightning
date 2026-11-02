#pragma once

#include <QString>

namespace gif {

QString buildKeyFor(const QString &providerId);

// The single authoritative provider-key resolution: a non-empty runtime
// override wins; otherwise the compiled build key; otherwise empty (missing).
// Both inputs are whitespace-trimmed, and an empty/whitespace override never
// hides a valid compiled fallback.
QString resolveProviderKey(const QString &runtimeOverride,
                           const QString &buildKey);

}
