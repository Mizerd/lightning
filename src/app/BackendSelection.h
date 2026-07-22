#pragma once

#include "app/AppController.h"

#include <QLatin1String>
#include <QString>

// Backend selection helpers, extracted from main() so the compiled default and
// the --backend=NAME parsing can be unit-tested without a QGuiApplication.
//
// The DEFAULT backend for a launch that passes no --backend flag is decided at
// COMPILE TIME. Official desktop builds compile the Matrix Rust SDK backend and
// must default to it so a packaged launcher (which passes no flag — unlike the
// Linux .desktop entry and run-dev.sh, which pass --backend=rust explicitly)
// gets SDK-owned end-to-end encryption instead of the non-E2EE HTTP development
// backend. Builds without the Rust SDK (mock/http CI configurations) keep the
// HTTP default. There is no silent runtime Rust->HTTP fallback: an explicit
// --backend=http is the only way to select HTTP in a Rust-enabled build.

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
// A Rust-only release build must have the Rust backend as its compiled default
// (the CMake guard already requires ENABLE_RUST_SDK_BACKEND). Assert it so a
// misconfiguration fails at compile time, not at a user's first launch.
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
