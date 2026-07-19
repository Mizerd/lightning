#pragma once

#include <QString>

// Compile-time GIF provider-key fallback.
//
// Official release packages embed application-level GIPHY/KLIPY keys so an
// installed client works without the user creating keys or setting environment
// variables. The values are supplied at build time through a generated header
// in the build tree (never tracked, installed, or logged). An ordinary source
// build has no generated header and behaves exactly as before: no embedded key,
// so an unconfigured provider shows the existing missing-key state.
//
// These are desktop application/provider keys, not Matrix credentials. A key
// compiled into a distributed binary is ultimately extractable; this fallback
// only spares normal users manual configuration. The raw value is never exposed
// to QML, settings, diagnostics, --version, or logs.
namespace gif {

// The compiled release key for a provider id ("giphy"/"klipy"), or an empty
// string in a build with no embedded key. Whitespace-trimmed.
QString buildKeyFor(const QString &providerId);

// The single authoritative provider-key resolution: a non-empty runtime
// override wins; otherwise the compiled build key; otherwise empty (missing).
// Both inputs are whitespace-trimmed, and an empty/whitespace override never
// hides a valid compiled fallback.
QString resolveProviderKey(const QString &runtimeOverride,
                           const QString &buildKey);

} // namespace gif
