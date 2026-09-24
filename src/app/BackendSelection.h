#pragma once

#include "app/AppController.h"

#include <QLatin1String>
#include <QString>

// Backend selection, extracted from main() for unit testing.
//
// The default backend is fixed at compile time: Rust SDK builds default to the
// Rust backend (so launchers passing no flag get E2EE), others to HTTP. There is
// no runtime Rust->HTTP fallback; only an explicit --backend=http selects it.

namespace lightning {

// The compile-time default backend for this build.
constexpr AppController::Backend defaultBackend()
{
#ifdef ENABLE_RUST_SDK_BACKEND
    return AppController::RustBackend;
#else
    return AppController::HttpBackend;
#endif
}

#ifdef LIGHTNING_RUST_ONLY
// Fail a misconfigured Rust-only release build at compile time.
static_assert(defaultBackend() == AppController::RustBackend,
    "LIGHTNING_RUST_ONLY requires the Rust backend to be the compiled default");
#endif

// Resolve --backend=NAME (case-insensitive). Returns HttpBackend on unknown
// values and reports the ambiguity via *ok.
inline AppController::Backend backendFromName(const QString &name, bool *ok)
{
    if (ok) *ok = true;
    const QString v = name.trimmed().toLower();
    if (v == QLatin1String("mock"))
        return AppController::MockBackend;
    if (v == QLatin1String("http"))
        return AppController::HttpBackend;
    if (v == QLatin1String("rust"))
        return AppController::RustBackend;
    if (ok) *ok = false;
    return AppController::HttpBackend;
}

inline QString backendNameFor(AppController::Backend backend)
{
    switch (backend) {
    case AppController::MockBackend: return QStringLiteral("mock");
    case AppController::HttpBackend: return QStringLiteral("http");
    case AppController::RustBackend: return QStringLiteral("rust");
    }
    return QStringLiteral("http");
}

} // namespace lightning
