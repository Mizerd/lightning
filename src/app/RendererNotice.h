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

inline bool softwareRenderingChosenByUser(const QString &quickBackend,
                                          const QString &rhiBackend)
{
    return quickBackend.compare(QLatin1String("software"),
                                Qt::CaseInsensitive) == 0
        || rhiBackend.compare(QLatin1String("software"),
                              Qt::CaseInsensitive) == 0;
}

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

enum class SoftwareRendererContext {
    Generic,
    AppImageOnNixOS,
    AppImage,
    NixOS,
};

inline bool isNixOsProductType(const QString &productType)
{
    return productType.compare(QLatin1String("nixos"), Qt::CaseInsensitive)
        == 0;
}

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

}
