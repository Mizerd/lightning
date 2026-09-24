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

// Deliberately not a theme token: the badge sits in the OS tray, not our window.
const QColor kBadgeFill = QColor(0xE5, 0x48, 0x4D);
const QColor kBadgeInk = QColor(0xFF, 0xFF, 0xFF);

// Several sizes so the tray host picks a sharp one instead of scaling.
constexpr int kBadgeSizes[] = { 16, 22, 24, 32, 48, 64 };

QPixmap withBadge(const QPixmap &base, const QString &label)
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
    if (dotOnly)
        return out;

    // Knock the digit out of the disc; white ink would vanish in a template.
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

    // No context menu by design: every activation brings the window back.
    //
    // The XEmbed fallback (X11 without a StatusNotifier watcher) is a QWidget,
    // which is why the process must be a QApplication (see main.cpp).
    m_icon = new QSystemTrayIcon(this);
    // refreshIcon() so an icon created while unread starts with its badge.
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
        badged.addPixmap(withBadge(pixmap, label));
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

bool TrayIcon::showMessage(const QString &title, const QString &body,
                           const QImage &image)
{
    if (!m_icon || !m_icon->isVisible())
        return false;
    // Windows treats this as a hint and macOS ignores it.
    constexpr int kMillis = 10000;
    if (image.isNull())
        m_icon->showMessage(title, body, QSystemTrayIcon::Information, kMillis);
    else
        m_icon->showMessage(title, body, QIcon(QPixmap::fromImage(image)), kMillis);
    return true;
}
