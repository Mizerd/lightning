#pragma once

#include <QString>

namespace lightning {

/// True when an OpenGL GL_RENDERER string names a CPU rasteriser. Matched as a
/// case-insensitive substring because drivers append their own detail
/// ("llvmpipe (LLVM 19.1.7, 256 bits)", "SwiftShader Device (Subzero)").
inline bool isSoftwareRasterizerRenderer(const QString &glRenderer)
{
    static const char *const kNames[] = {
        "llvmpipe", "softpipe", "swiftshader", "software rasterizer",
        "microsoft basic render",
    };
    for (const char *name : kNames) {
        if (glRenderer.contains(QLatin1String(name), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

/// True when the user asked for software rendering themselves, so a notice
/// about it would be noise. Takes the two environment values rather than
/// reading them, so the decision is testable.
inline bool softwareRenderingChosenByUser(const QString &quickBackend,
                                          const QString &rhiBackend)
{
    return quickBackend.compare(QLatin1String("software"),
                                Qt::CaseInsensitive) == 0
        || rhiBackend.compare(QLatin1String("software"),
                              Qt::CaseInsensitive) == 0;
}

/// Whether to show the "this is software rendering" notice: a known CPU
/// rasteriser, not chosen on purpose, and not already dismissed for this
/// version of the application.
inline bool shouldShowSoftwareRendererNotice(const QString &glRenderer,
                                             const QString &quickBackend,
                                             const QString &rhiBackend,
                                             const QString &dismissedVersion,
                                             const QString &currentVersion)
{
    if (!isSoftwareRasterizerRenderer(glRenderer))
        return false;
    if (softwareRenderingChosenByUser(quickBackend, rhiBackend))
        return false;
    return dismissedVersion != currentVersion;
}

} // namespace lightning
