#include "app/WindowFrameTheme.h"

#include <QCoreApplication>

#include <cmath>

#ifdef Q_OS_WIN
#include <QEvent>
#include <QGuiApplication>
#include <QObject>
#include <QPalette>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QTimer>
#include <QWindow>

#include <qt_windows.h>
#endif

namespace lightning::windowframe {

bool prefersDarkFrame(const QColor &window)
{
    // sRGB to linear, then Rec. 709 luminance (WCAG's relative luminance).
    const auto linear = [](double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const QColor rgb = window.toRgb();
    const double y = 0.2126 * linear(rgb.redF())
        + 0.7152 * linear(rgb.greenF()) + 0.0722 * linear(rgb.blueF());
    // L* = 50 is Y = 0.1842: the perceptual midpoint between black and white.
    return y < 0.1842;
}

#ifdef Q_OS_WIN
namespace {

using DwmSetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);

DwmSetWindowAttributeFn dwmSetWindowAttribute()
{
    static const DwmSetWindowAttributeFn fn = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<DwmSetWindowAttributeFn>(
                         reinterpret_cast<void *>(
                             GetProcAddress(dwm, "DwmSetWindowAttribute")))
                   : nullptr;
    }();
    return fn;
}

// DWMWA_USE_IMMERSIVE_DARK_MODE: 20 from Windows 10 20H1 (and on 11); the
// same request was 19 on 1809 to 1909. Not in older SDK headers.
constexpr DWORD kDarkModeAttribute = 20;
constexpr DWORD kDarkModeAttributeBefore20H1 = 19;
// What this window was last asked for, so a caption is redrawn only when the
// answer changes.
constexpr char kAppliedProperty[] = "_lightningDarkFrame";

bool paletteIsDark()
{
    return prefersDarkFrame(QGuiApplication::palette().color(QPalette::Window));
}

void applyTo(QWindow *window, bool dark)
{
    // No native window yet: SurfaceCreated brings it here once there is.
    if (!window || !window->handle() || window->parent())
        return;
    const DwmSetWindowAttributeFn set = dwmSetWindowAttribute();
    if (!set)
        return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const BOOL value = dark ? TRUE : FALSE;
    // Set every time, not only on a change: Qt sets the same attribute from
    // the SYSTEM's scheme when it creates a window and when that scheme
    // changes, which would otherwise win.
    if (FAILED(set(hwnd, kDarkModeAttribute, &value, sizeof(value)))
        && FAILED(set(hwnd, kDarkModeAttributeBefore20H1, &value,
                      sizeof(value))))
        return; // Windows before 1809: the frame stays as it is
    const QVariant previous = window->property(kAppliedProperty);
    window->setProperty(kAppliedProperty, dark);
    // Windows 10 repaints a visible caption only on its next activation; a
    // frame change repaints it now.
    if (window->isVisible() && (!previous.isValid() || previous.toBool() != dark))
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                         | SWP_FRAMECHANGED);
}

void applyToAll()
{
    const bool dark = paletteIsDark();
    const auto windows = QGuiApplication::topLevelWindows();
    for (QWindow *window : windows)
        applyTo(window, dark);
}

class FrameFollower : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::ApplicationPaletteChange:
            // Sent to the application and to every window; once is enough.
            if (watched == QCoreApplication::instance())
                applyToAll();
            break;
        case QEvent::PlatformSurface:
            if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType()
                == QPlatformSurfaceEvent::SurfaceCreated) {
                if (auto *window = qobject_cast<QWindow *>(watched))
                    applyTo(window, paletteIsDark());
            }
            break;
        case QEvent::ThemeChange:
            // The system scheme changed and Qt re-set this window's frame
            // from it; ours again, after Qt has finished.
            if (auto *window = qobject_cast<QWindow *>(watched)) {
                QTimer::singleShot(0, window, [w = QPointer<QWindow>(window)] {
                    if (w)
                        applyTo(w, paletteIsDark());
                });
            }
            break;
        default:
            break;
        }
        return false;
    }
};

} // namespace
#endif

void install()
{
#ifdef Q_OS_WIN
    QCoreApplication *app = QCoreApplication::instance();
    if (!app || !qobject_cast<QGuiApplication *>(app))
        return;
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    app->installEventFilter(new FrameFollower(app));
    applyToAll();
#endif
}

} // namespace lightning::windowframe
