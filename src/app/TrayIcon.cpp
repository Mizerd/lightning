#include "app/TrayIcon.h"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QPixmap>
#include <QAction>
#include <QMenu>
#include <QApplication>
#include <QSystemTrayIcon>

#ifdef Q_OS_WIN
#include <QLoggingCategory>
#include <QSettings>
#include <QThread>

#include <qt_windows.h>
// qt_windows.h may define WIN32_LEAN_AND_MEAN, which keeps shellapi.h out of
// windows.h.
#include <shellapi.h>

#include <cstring>
#include <cwchar>
#endif

namespace {

// Deliberately not a theme token: the badge sits in the OS tray, not our window.
const QColor kBadgeFill = QColor(0xE5, 0x48, 0x4D);
const QColor kBadgeInk = QColor(0xFF, 0xFF, 0xFF);

// Several sizes so the tray host picks a sharp one instead of scaling.
constexpr int kBadgeSizes[] = { 16, 22, 24, 32, 48, 64 };

} // namespace

QPixmap TrayIcon::badged(const QPixmap &base, const QString &label)
{
    if (base.isNull() || label.isEmpty())
        return base;
    QPixmap out = base;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // QPainter on a pixmap works in device-independent units (it scales by
    // devicePixelRatio itself), so the geometry must be derived from the
    // LOGICAL size. At a non-1 ratio (interface zoom) QIcon::pixmap() returns
    // size*dpr physical pixels, and using those here pushes the disc past
    // the corner and clips it (#23).
    const qreal dpr = out.devicePixelRatio() > 0 ? out.devicePixelRatio() : 1.0;
    const QSizeF logical(out.width() / dpr, out.height() / dpr);
    const qreal side = qMin(logical.width(), logical.height());
    const bool dotOnly = (label == QStringLiteral("\u2022"));
    const qreal diameter = dotOnly ? side * 0.42 : side * 0.62;
    const QRectF circle(logical.width() - diameter, logical.height() - diameter,
                        diameter, diameter);

    painter.setPen(Qt::NoPen);
    painter.setBrush(kBadgeFill);
    painter.drawEllipse(circle);
    if (dotOnly)
        return out;

    QFont font = QGuiApplication::font();
    font.setBold(true);
    font.setPixelSize(qMax(6, qRound(diameter * 0.68)));
    painter.setFont(font);
    painter.setPen(kBadgeInk);
    painter.drawText(circle, Qt::AlignCenter, label);
    return out;
}


QPixmap TrayIcon::macTemplateBadged(const QPixmap &base, const QString &label)
{
    if (base.isNull() || label.isEmpty())
        return base;
    // Painted on a QImage, never on `base` (or a copy of it) directly: a
    // QPixmap's platform backing store is not guaranteed to carry a real
    // alpha channel. Measured on the offscreen QPA platform this dev shell's
    // tests run under (Qt 6.11.2): QPainter(&pixmap) makes EVERY
    // CompositionMode_Clear draw \u2014 fillRect, drawEllipse, drawText alike \u2014 a
    // silent no-op, because the platform pixmap reports
    // hasAlphaChannel()==false however the source image was formatted. A
    // QImage always has one, and painting there start to finish, converting
    // to QPixmap only once at the end, preserves it losslessly (verified
    // with a standalone probe: 0 pixels cleared painting on a QPixmap, 100/100
    // cleared painting the same operation on a QImage first).
    QImage image =
        base.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Logical units, as in badged(): the painter scales by the ratio itself.
    const qreal dpr =
        image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
    const QSizeF logical(image.width() / dpr, image.height() / dpr);
    const qreal side = qMin(logical.width(), logical.height());
    const bool dotOnly = (label == QStringLiteral("\u2022"));
    const qreal diameter = dotOnly ? side * 0.42 : side * 0.62;
    const QRectF circle(logical.width() - diameter,
                        logical.height() - diameter, diameter, diameter);

    // Clear a moat first: in a template, badge and mark share one ink and
    // would fuse where they touch.
    const qreal moat = qMax<qreal>(1.0, side * 0.08);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.drawEllipse(circle.adjusted(-moat, -moat, moat, moat));

    // AppKit ignores the colour; opaque black keeps it correct without the
    // template treatment too.
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setBrush(QColor(0, 0, 0, 255));
    painter.drawEllipse(circle);
    if (dotOnly) {
        painter.end();
        return QPixmap::fromImage(image);
    }

    // Knock the digit out of the disc; white ink would vanish in a template.
    QFont font = QGuiApplication::font();
    font.setBold(true);
    font.setPixelSize(qMax(6, qRound(diameter * 0.68)));
    painter.setFont(font);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.setPen(QColor(0, 0, 0, 255));
    painter.drawText(circle, Qt::AlignCenter, label);
    painter.end();
    return QPixmap::fromImage(image);
}

#ifdef Q_OS_MACOS
namespace {

// Qt's cocoa backend picks the largest pixmap no taller than
// (NSStatusBar.thickness - 4) * dpr; thickness is 22 or 24, so these are exact
// hits at 1x and 2x, with 16 as a floor and 32 for 1.5x.
constexpr int kTemplateSizes[] = { 16, 18, 20, 22, 32, 36, 40, 44 };

QString templateResource(int size)
{
    return QStringLiteral(
               ":/qt/qml/MatrixClient/data/icons/menubar/"
               "lightning-template-%1.png")
        .arg(size);
}

/// The badged template icon, or null when the asset is not in this binary's
/// resources (test targets); the caller then keeps the colour icon.
QIcon macTemplateIcon(const QString &label)
{
    QIcon icon;
    for (const int size : kTemplateSizes) {
        const QPixmap asset(templateResource(size));
        if (asset.isNull())
            continue;
        icon.addPixmap(TrayIcon::macTemplateBadged(asset, label));
    }
    return icon;
}

} // namespace
#endif // Q_OS_MACOS

TrayIcon::TrayIcon(QObject *parent)
    : QObject(parent)
{
}

TrayIcon::~TrayIcon()
{
#ifdef Q_OS_WIN
    // The icon goes first (NIM_DELETE takes its balloon with it), then the
    // HICON that balloon was showing.
    delete m_icon;
    m_icon = nullptr;
    releaseBalloonIcon();
#endif
}

bool TrayIcon::platformSupportsTray()
{
    // QSystemTrayIcon's backends are widgets: under a QGuiApplication (the
    // test harnesses) the query itself dereferences what does not exist.
    if (!qobject_cast<QApplication *>(QCoreApplication::instance()))
        return false;
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayIcon::setEnabled(bool enabled)
{
    if (enabled == (m_icon != nullptr))
        return;
    if (!enabled) {
        delete m_icon;
        m_icon = nullptr;
#ifdef Q_OS_WIN
        releaseBalloonIcon();
#endif
        return;
    }
    if (!platformSupportsTray())
        return;

    // A left click brings the window back; the right-click menu adds the way
    // to quit that close-to-tray otherwise leaves only to Ctrl+Q (GitHub #17).
    //
    // The XEmbed fallback (X11 without a StatusNotifier watcher) and the menu
    // are QWidgets, which is why the process must be a QApplication (see
    // main.cpp).
    m_icon = new QSystemTrayIcon(this);
    // refreshIcon() so an icon created while unread starts with its badge.
    refreshIcon();
    m_icon->setContextMenu(contextMenu());
    connect(m_icon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (activationShowsWindow(reason))
                    Q_EMIT showRequested();
            });
    connect(m_icon, &QSystemTrayIcon::messageClicked, this,
            &TrayIcon::messageClicked);
    refreshTooltip();
    m_icon->show();
}

QMenu *TrayIcon::contextMenu()
{
    if (m_menu)
        return m_menu.get();
    m_menu = std::make_unique<QMenu>();
    QAction *show = m_menu->addAction(tr("Show Lightning"));
    connect(show, &QAction::triggered, this, &TrayIcon::showRequested);
    m_menu->addSeparator();
    QAction *quit = m_menu->addAction(tr("Quit Lightning"));
    connect(quit, &QAction::triggered, this, &TrayIcon::quitRequested);
    return m_menu.get();
}

bool TrayIcon::activationShowsWindow(int reason)
{
    return reason != QSystemTrayIcon::Context;
}

QString TrayIcon::badgeLabel(int count, bool anyUnread)
{
    if (count > 9)
        return QStringLiteral("9+");
    if (count > 0)
        return QString::number(count);
    // Unread without a count: claim only what is known.
    if (anyUnread)
        return QStringLiteral("\u2022");
    return QString{};
}

void TrayIcon::setUnread(int count, bool anyUnread)
{
    const int clamped = qMax(0, count);
    if (clamped == m_unread && anyUnread == m_anyUnread)
        return;
    const QString before = badgeLabel(m_unread, m_anyUnread);
    m_unread = clamped;
    m_anyUnread = anyUnread;
    refreshTooltip();
    // Rasterise only when the displayed badge changes.
    if (badgeLabel(m_unread, m_anyUnread) != before)
        refreshIcon();
}

void TrayIcon::refreshIcon()
{
    if (!m_icon)
        return;
    const QString label = badgeLabel(m_unread, m_anyUnread);
#ifdef Q_OS_MACOS
    // The macOS menu bar wants a monochrome template image, which Qt's cocoa
    // backend sets via setIsMask(true). If the asset is missing, fall through
    // to the colour icon rather than clearing the entry.
    QIcon templated = macTemplateIcon(label);
    if (!templated.isNull()) {
        templated.setIsMask(true);
        m_icon->setIcon(templated);
        return;
    }
#endif
    const QIcon base = QGuiApplication::windowIcon();
    if (base.isNull() || label.isEmpty()) {
        m_icon->setIcon(base);
        return;
    }
    QIcon badged;
    for (const int size : kBadgeSizes) {
        const QPixmap pixmap = base.pixmap(QSize(size, size));
        if (pixmap.isNull())
            continue;
        badged.addPixmap(TrayIcon::badged(pixmap, label));
    }
    // An empty QIcon would clear the tray entry.
    m_icon->setIcon(badged.isNull() ? base : badged);
}

void TrayIcon::setAccountLabel(const QString &label)
{
    if (label == m_account)
        return;
    m_account = label;
    refreshTooltip();
}

void TrayIcon::refreshTooltip()
{
    if (!m_icon)
        return;
    QString text = QStringLiteral("Lightning");
    if (!m_account.isEmpty())
        text += QStringLiteral(" — ") + m_account;
    if (m_unread > 0) {
        text += QLatin1Char('\n')
            + tr("%n unread message(s)", "system tray tooltip", m_unread);
    }
    m_icon->setToolTip(text);
}

unsigned TrayIcon::balloonInfoFlags(bool hasImage, bool platformSound)
{
    unsigned flags = hasImage ? (kBalloonUser | kBalloonLargeIcon)
                              : kBalloonInfo;
    if (!platformSound)
        flags |= kBalloonNoSound;
    return flags;
}

QImage TrayIcon::balloonImage(const QImage &avatar)
{
    constexpr int kMaxSide = 256;
    if (avatar.isNull())
        return {};
    if (avatar.width() <= kMaxSide && avatar.height() <= kMaxSide)
        return avatar;
    return avatar.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
}

bool TrayIcon::showMessage(const QString &title, const QString &body,
                           const QImage &image, bool platformSound)
{
    if (!m_icon || !m_icon->isVisible())
        return false;
#ifdef Q_OS_WIN
    if (showNativeBalloon(title, body, image, platformSound))
        return true;
#else
    // macOS: Qt's NSUserNotification sets no soundName, so it is already
    // silent (qcocoasystemtrayicon.mm, Qt 6.11.2). A Linux tray balloon
    // carries no sound of its own.
    Q_UNUSED(platformSound);
#endif
    // Windows treats this as a hint and macOS ignores it.
    constexpr int kMillis = 10000;
    if (image.isNull())
        m_icon->showMessage(title, body, QSystemTrayIcon::Information, kMillis);
    else
        m_icon->showMessage(title, body, QIcon(QPixmap::fromImage(image)), kMillis);
    return true;
}

#ifdef Q_OS_WIN
namespace {

Q_LOGGING_CATEGORY(lcTrayBalloon, "matrix.notify")

static_assert(TrayIcon::kBalloonInfo == NIIF_INFO);
static_assert(TrayIcon::kBalloonUser == NIIF_USER);
static_assert(TrayIcon::kBalloonNoSound == NIIF_NOSOUND);
static_assert(TrayIcon::kBalloonLargeIcon == NIIF_LARGE_ICON);

// How Qt 6.11.2 registers a QSystemTrayIcon (qwindowssystemtrayicon.cpp):
// uID is the constant q_uNOTIFYICONID = 0 (no NIF_GUID), on a hidden
// top-level window (deliberately NOT HWND_MESSAGE: message-only windows miss
// "TaskbarCreated") titled "QTrayIconMessageWindow", whose class is
// "Qt<version>[d][namespace]TrayIconMessageWindowClass[uuid]", created on the
// GUI thread. One such window per QSystemTrayIcon. Its window procedure turns
// NIN_BALLOONUSERCLICK on that window into messageClicked(), so a balloon
// raised here routes its click exactly as Qt's own.
constexpr UINT kQtTrayIconId = 0;
constexpr wchar_t kQtTrayWindowTitle[] = L"QTrayIconMessageWindow";
constexpr wchar_t kQtTrayWindowClassPart[] = L"TrayIconMessageWindowClass";

struct TrayWindowSearch {
    HWND found = nullptr;
    int matches = 0;
};

BOOL CALLBACK findQtTrayWindow(HWND hwnd, LPARAM param)
{
    auto *search = reinterpret_cast<TrayWindowSearch *>(param);
    // The class name first: it is read without a message. Only then the
    // title, which is a WM_GETTEXT; safe, because EnumThreadWindows offers
    // only windows of the calling (GUI) thread, so it cannot block on another.
    wchar_t className[256] = {};
    if (GetClassNameW(hwnd, className, 256) <= 0
        || !std::wcsstr(className, kQtTrayWindowClassPart))
        return TRUE;
    wchar_t title[64] = {};
    if (GetWindowTextW(hwnd, title, 64) <= 0
        || std::wcscmp(title, kQtTrayWindowTitle) != 0)
        return TRUE;
    search->found = hwnd;
    ++search->matches;
    return TRUE;
}

// Qt's supportsMessages(): with balloon tips switched off by policy Qt shows
// nothing, and neither do we. An absent key (usual on Windows 10+) means on.
bool balloonTipsEnabled()
{
    const QSettings advanced(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\"
                       "CurrentVersion\\Explorer\\Advanced"),
        QSettings::NativeFormat);
    return advanced.value(QStringLiteral("EnableBalloonTips"), 1).toInt() != 0;
}

// A QString into a fixed NOTIFYICONDATAW field, NUL included, as Qt's
// qStringToLimitedWCharArray, but never splitting a surrogate pair at the cut.
template <size_t N>
void copyLimited(const QString &in, wchar_t (&target)[N])
{
    qsizetype length = qMin<qsizetype>(qsizetype(N) - 1, in.size());
    if (length > 0 && length < in.size() && in.at(length - 1).isHighSurrogate())
        --length;
    std::memset(target, 0, sizeof(target));
    for (qsizetype i = 0; i < length; ++i)
        target[i] = wchar_t(in.at(i).unicode());
}

enum class BalloonPath { Native, NoQtWindow, Ambiguous, Refused, WrongThread };

// Which path the balloons take, logged once per path for the life of the
// process (category matrix.notify, as NotificationManager's own lines). Never
// the title or body.
void logPathOnce(BalloonPath path)
{
    static unsigned logged = 0;
    const unsigned bit = 1u << unsigned(path);
    if (logged & bit)
        return;
    logged |= bit;
    switch (path) {
    case BalloonPath::Native:
        qCInfo(lcTrayBalloon) << "tray balloon raised natively on Qt's icon; "
                                 "the platform sound follows the sound plan";
        break;
    case BalloonPath::NoQtWindow:
        qCWarning(lcTrayBalloon) << "tray balloon fell back to Qt (its tray "
                                    "window was not found): Windows may add "
                                    "its own notification sound";
        break;
    case BalloonPath::Ambiguous:
        qCWarning(lcTrayBalloon) << "tray balloon fell back to Qt (more than "
                                    "one Qt tray window): Windows may add its "
                                    "own notification sound";
        break;
    case BalloonPath::Refused:
        qCWarning(lcTrayBalloon) << "tray balloon fell back to Qt (the shell "
                                    "refused NIM_MODIFY): Windows may add its "
                                    "own notification sound";
        break;
    case BalloonPath::WrongThread:
        qCWarning(lcTrayBalloon) << "tray balloon fell back to Qt (not on the "
                                    "GUI thread): Windows may add its own "
                                    "notification sound";
        break;
    }
}

} // namespace

void TrayIcon::releaseBalloonIcon()
{
    if (m_balloonIcon) {
        DestroyIcon(static_cast<HICON>(m_balloonIcon));
        m_balloonIcon = nullptr;
    }
}

bool TrayIcon::showNativeBalloon(const QString &title, const QString &body,
                                 const QImage &image, bool platformSound)
{
    // Qt's tray window lives on the GUI thread, and only a window of the
    // calling thread may be asked its title without risking a block.
    if (QThread::currentThread() != thread()) {
        logPathOnce(BalloonPath::WrongThread);
        return false;
    }
    if (!balloonTipsEnabled())
        return true; // what Qt's own call would have shown: nothing

    TrayWindowSearch search;
    EnumThreadWindows(GetCurrentThreadId(), findQtTrayWindow,
                      reinterpret_cast<LPARAM>(&search));
    if (search.matches == 0) {
        logPathOnce(BalloonPath::NoQtWindow);
        return false;
    }
    if (search.matches > 1) {
        // Another QSystemTrayIcon in the process: which window is ours is
        // unknowable from outside Qt.
        logPathOnce(BalloonPath::Ambiguous);
        return false;
    }

    NOTIFYICONDATAW data;
    std::memset(&data, 0, sizeof(data));
    data.cbSize = sizeof(data);
    data.hWnd = search.found;
    data.uID = kQtTrayIconId;
    // As Qt: NIF_SHOWTIP keeps the standard tooltip under
    // NOTIFYICON_VERSION_4, which Qt set on the icon after NIM_ADD.
    data.uFlags = NIF_INFO | NIF_SHOWTIP;
    // Shares a union with uVersion and is ignored since Vista (the
    // accessibility timeout decides); Qt's value anyway.
    data.uTimeout = 10000;
    // As Qt: a title with an empty body still shows.
    QString message = body;
    if (message.isEmpty() && !title.isEmpty())
        message = QStringLiteral(" ");
    copyLimited(message, data.szInfo);
    copyLimited(title, data.szInfoTitle);

    HICON icon = nullptr;
    const QImage scaled = balloonImage(image);
    if (!scaled.isNull())
        icon = scaled.toHICON();
    data.dwInfoFlags = balloonInfoFlags(icon != nullptr, platformSound);
    data.hBalloonIcon = icon;

    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
        if (icon)
            DestroyIcon(icon);
        logPathOnce(BalloonPath::Refused);
        return false;
    }
    // The previous balloon's icon is no longer on screen.
    releaseBalloonIcon();
    m_balloonIcon = icon;
    logPathOnce(BalloonPath::Native);
    return true;
}
#endif // Q_OS_WIN
