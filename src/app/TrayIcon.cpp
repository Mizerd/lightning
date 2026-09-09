#include "app/TrayIcon.h"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QPixmap>
#include <QSystemTrayIcon>

namespace {

// The badge is red on every theme on purpose: it is the operating system's
// tray, not Lightning's window, and a badge tinted to match a user theme
// reads as decoration rather than as a count. This is the one colour in the
// application that is deliberately not a theme token.
const QColor kBadgeFill = QColor(0xE5, 0x48, 0x4D);
const QColor kBadgeInk = QColor(0xFF, 0xFF, 0xFF);

// The sizes a tray asks for. Several are rendered so the host picks a sharp
// one instead of scaling: a StatusNotifier host and the Windows notification
// area disagree about the size, and both change it with the display scale.
constexpr int kBadgeSizes[] = { 16, 22, 24, 32, 48, 64 };

// One rendered badge on top of one base pixmap.
QPixmap withBadge(const QPixmap &base, const QString &label)
{
    if (base.isNull() || label.isEmpty())
        return base;
    QPixmap out = base;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal side = qMin(out.width(), out.height());
    // A dot needs less room than a digit, and taking less of the icon keeps
    // the mark itself recognisable.
    const bool dotOnly = (label == QStringLiteral("\u2022"));
    const qreal diameter = dotOnly ? side * 0.42 : side * 0.62;
    const QRectF circle(out.width() - diameter, out.height() - diameter,
                        diameter, diameter);

    painter.setPen(Qt::NoPen);
    painter.setBrush(kBadgeFill);
    painter.drawEllipse(circle);
    if (dotOnly)
        return out;

    QFont font = QGuiApplication::font();
    font.setBold(true);
    // Sized from the circle rather than from a point size, because the same
    // code renders a 16px and a 64px icon.
    font.setPixelSize(qMax(6, qRound(diameter * 0.68)));
    painter.setFont(font);
    painter.setPen(kBadgeInk);
    painter.drawText(circle, Qt::AlignCenter, label);
    return out;
}

} // namespace

QPixmap TrayIcon::macTemplateBadged(const QPixmap &base, const QString &label)
{
    if (base.isNull() || label.isEmpty())
        return base;
    QPixmap out = base;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal side = qMin(out.width(), out.height());
    const bool dotOnly = (label == QStringLiteral("\u2022"));
    const qreal diameter = dotOnly ? side * 0.42 : side * 0.62;
    const QRectF circle(out.width() - diameter, out.height() - diameter,
                        diameter, diameter);

    // A MOAT FIRST. In a template only the alpha channel survives, so the
    // badge and the mark beneath it are painted in the same ink and would
    // fuse into one blob wherever they touch — the same reason the asset
    // itself cuts a gap where the bolt crosses the bubble. Clearing a
    // slightly larger disc is what makes the badge read as a separate thing.
    const qreal moat = qMax<qreal>(1.0, side * 0.08);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.drawEllipse(circle.adjusted(-moat, -moat, moat, moat));

    // Then the badge itself, fully opaque. The COLOUR is irrelevant to AppKit
    // — it repaints the shape for the current appearance — but it is written
    // as opaque black so the pixmap is also correct if it is ever drawn
    // without the template treatment.
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setBrush(QColor(0, 0, 0, 255));
    painter.drawEllipse(circle);
    if (dotOnly)
        return out;

    // And the digit KNOCKED OUT of the disc rather than drawn on top of it.
    // White ink would vanish: a template keeps no colour, so a white "3" on a
    // black disc is a black disc. Clearing those pixels lets the menu bar
    // show through, which is how a macOS counter actually looks.
    QFont font = QGuiApplication::font();
    font.setBold(true);
    font.setPixelSize(qMax(6, qRound(diameter * 0.68)));
    painter.setFont(font);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.setPen(QColor(0, 0, 0, 255));
    painter.drawText(circle, Qt::AlignCenter, label);
    return out;
}

#ifdef Q_OS_MACOS
namespace {

// The sizes the macOS template asset is shipped in.
//
// Qt's cocoa backend picks the largest available pixmap whose HEIGHT is at
// most `NSStatusBar.thickness - 4` times the device pixel ratio, then centres
// it in a full-thickness one (qcocoasystemtrayicon.mm). Thickness is 22 on
// most Macs and 24 on some, so 18/20/22 at 1x and 36/40/44 at 2x are exact
// hits that need no scaling at all; 16 is a floor so there is always
// something small enough to pick, and 32 covers a 1.5x scale.
constexpr int kTemplateSizes[] = { 16, 18, 20, 22, 32, 36, 40, 44 };

QString templateResource(int size)
{
    return QStringLiteral(
               ":/qt/qml/MatrixClient/data/icons/menubar/"
               "lightning-template-%1.png")
        .arg(size);
}

/// The macOS status-item icon: the monochrome template asset at every size it
/// ships in, badged. Null when the asset is not in this binary's resources —
/// which is every test target that compiles TrayIcon without the QML module,
/// and is why the caller keeps the colour icon as a fallback rather than
/// clearing the tray entry.
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

TrayIcon::~TrayIcon() = default;

bool TrayIcon::platformSupportsTray()
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayIcon::setEnabled(bool enabled)
{
    if (enabled == (m_icon != nullptr))
        return;
    if (!enabled) {
        delete m_icon;
        m_icon = nullptr;
        return;
    }
    if (!platformSupportsTray())
        return;

    // Deliberately NO context menu. A QMenu is a widget, and a tray menu
    // would need its own design (what it offers, how it tracks unread and
    // account state) rather than two entries bolted on. Instead EVERY
    // activation — left click, double click and right click — brings the
    // window back, so the icon is never a dead end, and quitting stays where
    // it already was: in the window (Ctrl+Q).
    //
    // The process IS a QApplication since 0.9.1 (see main.cpp): on X11 with
    // no StatusNotifier watcher Qt falls back to an XEmbed icon that is a
    // QWidget, and under a QGuiApplication that fallback aborted the process
    // the moment this setting was switched on.
    m_icon = new QSystemTrayIcon(this);
    // The window icon, so the tray matches the task switcher. A custom app
    // icon set by the user is already installed as the application icon, so
    // this follows it without a second code path. refreshIcon() rather than
    // setIcon(): an icon created while messages are already unread must open
    // WITH its badge, not gain one at the next change.
    refreshIcon();
    connect(m_icon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                Q_UNUSED(reason);
                Q_EMIT showRequested();
            });
    connect(m_icon, &QSystemTrayIcon::messageClicked, this,
            &TrayIcon::messageClicked);
    refreshTooltip();
    m_icon->show();
}

QString TrayIcon::badgeLabel(int count, bool anyUnread)
{
    if (count > 9)
        return QStringLiteral("9+");
    if (count > 0)
        return QString::number(count);
    // A count of zero with unread messages is not a contradiction: a
    // homeserver can report that a room has something unread without saying
    // how much, and a room the user marked unread by hand never had a count.
    // The dot claims exactly what is known.
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
    // THE WHOLE POINT: the icon is rasterised only when what it would SHOW
    // has changed. 40 unread becoming 41 is a tooltip change and nothing
    // else.
    if (badgeLabel(m_unread, m_anyUnread) != before)
        refreshIcon();
}

void TrayIcon::refreshIcon()
{
    if (!m_icon)
        return;
    const QString label = badgeLabel(m_unread, m_anyUnread);
#ifdef Q_OS_MACOS
    // THE MENU BAR IS NOT A TRAY. macOS wants a monochrome TEMPLATE image
    // that it recolours for the current appearance; the full-colour
    // application icon is wrong there, and so is a white copy of it, which
    // would be wrong in the opposite appearance. `setIsMask(true)` is what
    // Qt's cocoa backend turns into `[NSImage setTemplate:YES]`.
    //
    // The Dock, the Finder and the About box keep the colour icon: they read
    // `QGuiApplication::windowIcon()` and the bundle's own .icns, neither of
    // which this touches. Linux and Windows never reach this branch at all.
    //
    // A null icon means the template asset is not in this binary's
    // resources; falling through to the colour icon is better than clearing
    // the tray entry, which an empty QIcon would do.
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
        badged.addPixmap(withBadge(pixmap, label));
    }
    // A base icon that yielded nothing at any size is left alone rather than
    // replaced with an empty QIcon, which would clear the tray entry.
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

bool TrayIcon::showMessage(const QString &title, const QString &body,
                           const QImage &image)
{
    if (!m_icon || !m_icon->isVisible())
        return false;
    // Ten seconds: Windows treats the value as a hint and macOS ignores it,
    // so it only has to be reasonable.
    constexpr int kMillis = 10000;
    if (image.isNull())
        m_icon->showMessage(title, body, QSystemTrayIcon::Information, kMillis);
    else
        m_icon->showMessage(title, body, QIcon(QPixmap::fromImage(image)), kMillis);
    return true;
}
