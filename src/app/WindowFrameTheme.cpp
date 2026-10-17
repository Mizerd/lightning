#include "app/WindowFrameTheme.h"

#include <QCoreApplication>
#include <QLoggingCategory>

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

Reapply reapplyFor(QEvent::Type type)
{
    switch (type) {
    case QEvent::ApplicationPaletteChange:
    case QEvent::ThemeChange:
    case QEvent::PlatformSurface:
    case QEvent::Show:
    case QEvent::WindowStateChange:
        return Reapply::AfterQt;
    default:
        return Reapply::Ignore;
    }
}

const char *frameModeName(bool dark)
{
    return dark ? "dark" : "light";
}

#ifdef Q_OS_WIN
Q_LOGGING_CATEGORY(lcWindowFrame, "lightning.window")

namespace {

using DwmSetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);

using DwmGetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);

DwmGetWindowAttributeFn dwmGetWindowAttribute()
{
    static const DwmGetWindowAttributeFn fn = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm ? reinterpret_cast<DwmGetWindowAttributeFn>(
                         reinterpret_cast<void *>(
                             GetProcAddress(dwm, "DwmGetWindowAttribute")))
                   : nullptr;
    }();
    return fn;
}

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
    // What DWM holds for the window, asked rather than assumed: after Qt's own
    // request the answer is the system scheme's, not ours.
    const DwmGetWindowAttributeFn get = dwmGetWindowAttribute();
    const auto heldNow = [&]() -> int {
        BOOL held = FALSE;
        if (get
            && (SUCCEEDED(get(hwnd, kDarkModeAttribute, &held, sizeof(held)))
                || SUCCEEDED(get(hwnd, kDarkModeAttributeBefore20H1, &held,
                                 sizeof(held)))))
            return held != FALSE ? 1 : 0;
        return -1;
    };
    const int before = heldNow();
    // Set every time, not only on a change: Qt sets the same attribute from
    // the SYSTEM's scheme when it creates a window and when that scheme
    // changes, which would otherwise win.
    if (FAILED(set(hwnd, kDarkModeAttribute, &value, sizeof(value)))
        && FAILED(set(hwnd, kDarkModeAttributeBefore20H1, &value,
                      sizeof(value))))
        return; // Windows before 1809: the frame stays as it is
    const QVariant previous = window->property(kAppliedProperty);
    window->setProperty(kAppliedProperty, dark);
    const int after = heldNow();
    const bool overridden = before >= 0 && (before != 0) != dark;
    // Logged when the answer changes, or when Qt had put the other one back
    // since our last pass (the case this exists to expose), not on every
    // deferral.
    if (!previous.isValid() || previous.toBool() != dark || overridden)
        qCInfo(lcWindowFrame).nospace()
            << "caption requested=" << frameModeName(dark) << " applied="
            << (after < 0 ? "unknown" : frameModeName(after != 0))
            << " found=" << (before < 0 ? "unknown" : frameModeName(before != 0))
            << " window=" << window->objectName();
    // Windows 10 repaints a visible caption only on its next activation; a
    // frame change repaints it now.
    if (window->isVisible()
        && (!previous.isValid() || previous.toBool() != dark || overridden))
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
        const bool isApp = watched == QCoreApplication::instance();
        const bool isWindow = qobject_cast<QWindow *>(watched) != nullptr;
        if (!isApp && !isWindow)
            return false;
        if (event->type() == QEvent::PlatformSurface) {
            // The window was just created: Qt's creation-time request (system
            // scheme) is already in, so ours can go in now as well as after.
            if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType()
                    != QPlatformSurfaceEvent::SurfaceCreated)
                return false;
            if (auto *window = qobject_cast<QWindow *>(watched))
                applyTo(window, paletteIsDark());
        }
        if (reapplyFor(event->type()) == Reapply::AfterQt)
            scheduleAfterQt();
        return false;
    }

private:
    // This runs BEFORE the event reaches the window, where Qt's plugin
    // answers it with the system scheme; a zero-delay timer is delivered
    // after that whole dispatch. One pending pass covers a burst (a palette
    // change reaches the application and every window).
    void scheduleAfterQt()
    {
        if (m_pending)
            return;
        m_pending = true;
        QTimer::singleShot(0, this, [this] {
            m_pending = false;
            applyToAll();
        });
    }
    bool m_pending = false;
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
