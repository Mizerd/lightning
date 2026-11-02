#include "app/TrayIcon.h"

#include "app/CloseBehavior.h"

#include <QColor>
#include <QEvent>
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
#include <QCursor>
#include <QSystemTrayIcon>
#include <QTimer>

#include <algorithm>

#ifdef Q_OS_WIN
#include <QLoggingCategory>
#include <QSettings>
#include <QThread>

#include <qt_windows.h>
#include <shellapi.h>

#include <cstring>
#include <cwchar>
#endif

namespace {

const QColor kBadgeFill = QColor(0xE5, 0x48, 0x4D);
const QColor kBadgeInk = QColor(0xFF, 0xFF, 0xFF);

constexpr int kBadgeSizes[] = { 16, 22, 24, 32, 48, 64 };

}

QPixmap TrayIcon::badged(const QPixmap &base, const QString &label)
{
    if (base.isNull() || label.isEmpty())
        return base;
    QPixmap out = base;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);

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
    QImage image =
        base.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal dpr =
        image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
    const QSizeF logical(image.width() / dpr, image.height() / dpr);
    const qreal side = qMin(logical.width(), logical.height());
    const bool dotOnly = (label == QStringLiteral("\u2022"));
    const qreal diameter = dotOnly ? side * 0.42 : side * 0.62;
    const QRectF circle(logical.width() - diameter,
                        logical.height() - diameter, diameter, diameter);

    const qreal moat = qMax<qreal>(1.0, side * 0.08);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.drawEllipse(circle.adjusted(-moat, -moat, moat, moat));

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setBrush(QColor(0, 0, 0, 255));
    painter.drawEllipse(circle);
    if (dotOnly) {
        painter.end();
        return QPixmap::fromImage(image);
    }

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

constexpr int kTemplateSizes[] = { 16, 18, 20, 22, 32, 36, 40, 44 };

QString templateResource(int size)
{
    return QStringLiteral(
               ":/qt/qml/MatrixClient/data/icons/menubar/"
               "lightning-template-%1.png")
        .arg(size);
}

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

}
#endif

TrayIcon::TrayIcon(QObject *parent)
    : QObject(parent)
{
    if (qApp)
        qApp->installEventFilter(this);
}

TrayIcon::~TrayIcon()
{
#ifdef Q_OS_WIN
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

    m_icon = new QSystemTrayIcon(this);
    refreshIcon();
    const bool nativeMenu = closebehavior::trayUsesNativeMenu(closebehavior::kIsMac);
    if (nativeMenu)
        m_icon->setContextMenu(contextMenu());
    connect(m_icon, &QSystemTrayIcon::activated, this,
            [this, nativeMenu](QSystemTrayIcon::ActivationReason raw) {
                const int reason = effectiveActivationReason(
                    raw, closebehavior::kIsMac,
                    QGuiApplication::queryKeyboardModifiers() & Qt::MetaModifier);
                if (activationOpensOwnMenu(reason, nativeMenu)) {
                    QTimer::singleShot(0, this, [this] { popUpMenu(); });
                    return;
                }
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
    m_showAction = m_menu->addAction(tr("Show Lightning"));
    connect(m_showAction, &QAction::triggered, this, &TrayIcon::showRequested);
    m_menu->addSeparator();
    m_quitAction = m_menu->addAction(tr("Quit Lightning"));
    connect(m_quitAction, &QAction::triggered, this, &TrayIcon::quitRequested);
    return m_menu.get();
}

int TrayIcon::effectiveActivationReason(int reason, bool isMac, bool controlHeld)
{
    if (isMac && controlHeld && reason == QSystemTrayIcon::Trigger)
        return QSystemTrayIcon::Context;
    return reason;
}

void TrayIcon::retranslate()
{
    if (m_showAction)
        m_showAction->setText(tr("Show Lightning"));
    if (m_quitAction)
        m_quitAction->setText(tr("Quit Lightning"));
    refreshTooltip();
}

bool TrayIcon::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::LanguageChange && watched == qApp)
        retranslate();
    return QObject::eventFilter(watched, event);
}

bool TrayIcon::activationShowsWindow(int reason)
{
    return reason != QSystemTrayIcon::Context;
}

bool TrayIcon::activationOpensOwnMenu(int reason, bool nativeMenu)
{
    return !nativeMenu && reason == QSystemTrayIcon::Context;
}

void TrayIcon::popUpMenu()
{
    if (!m_icon)
        return;
    QMenu *menu = contextMenu();
    const QRect geometry = m_icon->geometry();
    const QPoint at = geometry.isValid()
        ? QPoint(geometry.left(), geometry.bottom())
        : QCursor::pos();
    menu->popup(at);
}

QString TrayIcon::badgeLabel(int count, bool anyUnread)
{
    if (count > 9)
        return QStringLiteral("9+");
    if (count > 0)
        return QString::number(count);
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
    if (badgeLabel(m_unread, m_anyUnread) != before)
        refreshIcon();
}

void TrayIcon::refreshIcon()
{
    if (!m_icon)
        return;
    const QString label = badgeLabel(m_unread, m_anyUnread);
#ifdef Q_OS_MACOS
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
    m_icon->setToolTip(tooltipText(m_account, m_unread, m_anyUnread));
}

QString TrayIcon::tooltipText(const QString &account, int unread, bool anyUnread)
{
    QString text = QStringLiteral("Lightning");
    if (!account.isEmpty())
        text += QStringLiteral(" — ") + account;
    if (unread > 0) {
        text += QLatin1Char('\n')
            + tr("%n unread message(s)", "system tray tooltip", unread);
    } else if (anyUnread) {
        text += QLatin1Char('\n')
            + tr("Unread messages", "system tray tooltip, unread rooms without a count");
    }
    return text;
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

int TrayIcon::balloonTimeoutMs(bool isMac)
{
    // Qt's cocoa backend schedules removeDeliveredNotification after `msecs`
    // whenever it is non-zero, deleting the notification from Notification
    // Center, so macOS gets 0 (no removal). Windows treats it as a hint.
    return isMac ? 0 : 10000;
}

int TrayIcon::balloonHoldMsFor(int durationSeconds)
{
    constexpr int kDefaultSeconds = 5;
    constexpr int kLongestSeconds = 300;
    constexpr int kMarginMs = 1000;
    const int seconds = durationSeconds > 0
        ? std::min(durationSeconds, kLongestSeconds)
        : kDefaultSeconds;
    return seconds * 1000 + kMarginMs;
}

int TrayIcon::balloonHoldMs()
{
#ifdef Q_OS_WIN
    ULONG seconds = 0;
    if (SystemParametersInfoW(SPI_GETMESSAGEDURATION, 0, &seconds, 0))
        return balloonHoldMsFor(int(std::min<ULONG>(seconds, 3600)));
#endif
    return balloonHoldMsFor(0);
}

bool TrayIcon::canWithdrawBalloon()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

void TrayIcon::hideMessage()
{
#ifdef Q_OS_WIN
    if (m_icon && m_icon->isVisible())
        hideNativeBalloon();
#endif
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
    Q_UNUSED(platformSound);
#endif
    const int kMillis = balloonTimeoutMs(closebehavior::kIsMac);
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

bool balloonTipsEnabled()
{
    const QSettings advanced(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\"
                       "CurrentVersion\\Explorer\\Advanced"),
        QSettings::NativeFormat);
    return advanced.value(QStringLiteral("EnableBalloonTips"), 1).toInt() != 0;
}

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

}

void TrayIcon::releaseBalloonIcon()
{
    if (m_balloonIcon) {
        DestroyIcon(static_cast<HICON>(m_balloonIcon));
        m_balloonIcon = nullptr;
    }
}

void TrayIcon::hideNativeBalloon()
{
    if (QThread::currentThread() != thread())
        return;
    TrayWindowSearch search;
    EnumThreadWindows(GetCurrentThreadId(), findQtTrayWindow,
                      reinterpret_cast<LPARAM>(&search));
    if (search.matches != 1)
        return;
    NOTIFYICONDATAW data;
    std::memset(&data, 0, sizeof(data));
    data.cbSize = sizeof(data);
    data.hWnd = search.found;
    data.uID = kQtTrayIconId;
    data.uFlags = NIF_INFO | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

bool TrayIcon::showNativeBalloon(const QString &title, const QString &body,
                                 const QImage &image, bool platformSound)
{
    if (QThread::currentThread() != thread()) {
        logPathOnce(BalloonPath::WrongThread);
        return false;
    }
    if (!balloonTipsEnabled())
        return true;

    TrayWindowSearch search;
    EnumThreadWindows(GetCurrentThreadId(), findQtTrayWindow,
                      reinterpret_cast<LPARAM>(&search));
    if (search.matches == 0) {
        logPathOnce(BalloonPath::NoQtWindow);
        return false;
    }
    if (search.matches > 1) {
        logPathOnce(BalloonPath::Ambiguous);
        return false;
    }

    NOTIFYICONDATAW data;
    std::memset(&data, 0, sizeof(data));
    data.cbSize = sizeof(data);
    data.hWnd = search.found;
    data.uID = kQtTrayIconId;
    data.uFlags = NIF_INFO | NIF_SHOWTIP;
    data.uTimeout = 10000;
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
    releaseBalloonIcon();
    m_balloonIcon = icon;
    logPathOnce(BalloonPath::Native);
    return true;
}
#endif
