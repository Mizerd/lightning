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

/// Which explanation the notice gives. The likely cause depends on the package
/// and the platform, and naming one that does not apply misleads: the notice
/// used to say "common with the AppImage on NixOS" on every package,
/// including a .deb in a virtual machine (VM test, 2026-10-07).
enum class SoftwareRendererContext {
    Generic,          // e.g. a VM or remote session, or a missing driver
    AppImageOnNixOS,  // the documented case, with a documented fix
    AppImage,         // an AppImage elsewhere
    NixOS,            // NixOS, not an AppImage
};

/// True when QSysInfo::productType() names NixOS (os-release ID=nixos).
inline bool isNixOsProductType(const QString &productType)
{
    return productType.compare(QLatin1String("nixos"), Qt::CaseInsensitive)
        == 0;
}

/// Names NixOS only on NixOS and the AppImage only for an AppImage. Takes the
/// two facts rather than detecting them, so the choice is testable.
inline SoftwareRendererContext softwareRendererContext(bool appImage,
                                                       bool nixos)
{
    if (appImage && nixos)
        return SoftwareRendererContext::AppImageOnNixOS;
    if (appImage)
        return SoftwareRendererContext::AppImage;
    if (nixos)
        return SoftwareRendererContext::NixOS;
    return SoftwareRendererContext::Generic;
}

/// The stable id QML words the notice from.
inline QString softwareRendererContextId(SoftwareRendererContext context)
{
    switch (context) {
    case SoftwareRendererContext::AppImageOnNixOS:
        return QStringLiteral("appimage-nixos");
    case SoftwareRendererContext::AppImage:
        return QStringLiteral("appimage");
    case SoftwareRendererContext::NixOS:
        return QStringLiteral("nixos");
    case SoftwareRendererContext::Generic:
        break;
    }
    return QStringLiteral("generic");
}

} // namespace lightning
