// The shared menu components (AppMenu, AppMenuItem, AppMenuSeparator,
// MenuKeycap, MenuSectionLabel, StatusChip) rendered offscreen: row geometry,
// highlight and ink, keycap chips, the danger treatment, radio flyout rows
// whose selection binding an internal toggle never destroys, and cascading
// flyouts that close innermost-first on Escape. Expected colours are read from
// token probes in the same scene, never hard-coded.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlContext>
#include <QQmlProperty>
#include <QSet>

#include <cmath>

namespace {

QColor sampleAvg(const QImage &img, const QRect &r)
{
    qint64 red = 0, green = 0, blue = 0, n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        for (int x = r.left(); x <= r.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            red += c.red();
            green += c.green();
            blue += c.blue();
            ++n;
        }
    }
    return n ? QColor(int(red / n), int(green / n), int(blue / n)) : QColor();
}

// WCAG 2.x relative luminance and contrast, plus the source-over composite of
// a translucent chip fill over its parent: a soft StatusChip is
// `Qt.alpha(ink, 0.14)`, so the pixel under the label depends on the parent.
double channelLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor &c)
{
    return 0.2126 * channelLinear(c.redF()) + 0.7152 * channelLinear(c.greenF())
        + 0.0722 * channelLinear(c.blueF());
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QColor over(const QColor &fg, const QColor &bg)
{
    const double a = fg.alphaF();
    return QColor::fromRgbF(a * fg.redF() + (1.0 - a) * bg.redF(),
                            a * fg.greenF() + (1.0 - a) * bg.greenF(),
                            a * fg.blueF() + (1.0 - a) * bg.blueF());
}

int channelDelta(const QColor &a, const QColor &b)
{
    return qMax(qMax(qAbs(a.red() - b.red()), qAbs(a.green() - b.green())),
                qAbs(a.blue() - b.blue()));
}

constexpr int kTolerance = 8;

// Keep this raw string free of apostrophes: a bare ' was suspected of making
// moc emit an empty .moc for this file.
const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    // 900x560, not 640x480: the neutral-chip probes below need a host
    // rectangle of their own painted in the two surfaces a storm chip is
    // actually dropped on, and a chip measured on the background of the window
    // is a chip measured on the wrong parent.
    width: 900
    height: 700
    visible: true
    color: AppTheme.background

    property int themeMode: 9
    Binding { target: AppTheme; property: "mode"; value: win.themeMode }

    // Owner-side radio state: the flyout rows bind to this, and the binding
    // must survive clicks (AppMenuItem never self-toggles radioSelected).
    property bool allMessagesSelected: true

    Rectangle { objectName: "tokAccentSoft"; visible: false; color: AppTheme.accentSoft }
    Rectangle { objectName: "tokSelectedText"; visible: false; color: AppTheme.selectedText }
    Rectangle { objectName: "tokCardElevated"; visible: false; color: AppTheme.cardElevated }
    Rectangle { objectName: "tokBorderStrong"; visible: false; color: AppTheme.borderStrong }
    Rectangle { objectName: "tokTextMuted"; visible: false; color: AppTheme.textMuted }
    Rectangle { objectName: "tokTextDisabled"; visible: false; color: AppTheme.textDisabled }
    Rectangle { objectName: "tokDangerInk"; visible: false; color: AppTheme.dangerInk }
    Rectangle { objectName: "tokAccent"; visible: false; color: AppTheme.accent }
    Rectangle { objectName: "tokSurface"; visible: false; color: AppTheme.surface }
    Rectangle { objectName: "tokPresenceOnline"; visible: false; color: AppTheme.presenceOnline }
    Rectangle { objectName: "tokMentionBadge"; visible: false; color: AppTheme.mentionBadge }
    Rectangle { objectName: "tokAccentText"; visible: false; color: AppTheme.accentText }
    Rectangle { objectName: "tokStormPanel"; visible: false; color: AppTheme.stormPanel }
    Rectangle { objectName: "tokStormSelection"; visible: false; color: AppTheme.stormSelection }
    Rectangle { objectName: "tokStormText"; visible: false; color: AppTheme.stormText }
    Rectangle { objectName: "tokStormTextMuted"; visible: false; color: AppTheme.stormTextMuted }
    Rectangle { objectName: "tokStormTextFaint"; visible: false; color: AppTheme.stormTextFaint }
    Rectangle { objectName: "tokStormBorderStrong"; visible: false; color: AppTheme.stormBorderStrong }
    Rectangle { objectName: "tokStormDanger"; visible: false; color: AppTheme.stormDanger }
    Rectangle { objectName: "tokBolt"; visible: false; color: AppTheme.bolt }
    Rectangle { objectName: "tokBoltInk"; visible: false; color: AppTheme.boltInk }

    AppMenu {
        id: menu
        objectName: "menu"
        menuWidth: AppTheme.menuWidthMessage

        AppMenuItem { objectName: "replyItem"; text: "Reply"; iconName: "reply"; accel: "R" }
        AppMenuItem { objectName: "threadItem"; text: "Reply in thread"; iconName: "forum"; accel: "T" }
        AppMenuSeparator { objectName: "groupSeparator" }
        AppMenuItem { objectName: "copyItem"; text: "Copy text"; iconName: "content_copy"; accel: "Ctrl+C" }
        AppMenu {
            id: flyout
            objectName: "flyout"
            title: "Notifications"
            submenuIconName: "notifications"
            menuWidth: AppTheme.menuWidthFlyout
            AppMenuItem {
                objectName: "radioAll"
                text: "All messages"
                radio: true
                radioSelected: win.allMessagesSelected
            }
            AppMenuItem {
                objectName: "radioMentions"
                text: "Mentions only"
                radio: true
                radioSelected: !win.allMessagesSelected
            }
        }
        // A SECOND flyout, at the same 150 px design width, carrying the row
        // the report was about and a non-MenuItem Label beside it. Separate
        // from the one above on purpose: the cases that assert the flyout
        // opens at its design width must keep measuring a menu whose rows
        // fit inside it.
        AppMenu {
            id: fitFlyout
            objectName: "fitFlyout"
            title: "Notify mode"
            submenuIconName: "notifications"
            menuWidth: AppTheme.menuWidthFlyout
            contextLabel: "Notify mode"
            contextBolt: false
            AppMenuItem { objectName: "fitShortRow"; text: "Muted"; radio: true }
            AppMenuItem {
                objectName: "fitLongRow"
                text: "Mentions & keywords"
                radio: true
            }
            Label {
                objectName: "fitDisclaimer"
                width: fitFlyout.width - fitFlyout.leftPadding
                       - fitFlyout.rightPadding
                leftPadding: AppTheme.menuItemPadding
                rightPadding: AppTheme.menuItemPadding
                text: "Local setting: it does not change the server "
                      + "push rules."
                color: AppTheme.stormTextFaint
                font.pixelSize: AppTheme.fontMicro
                wrapMode: Text.WordWrap
            }
        }
        AppMenu {
            id: ceilingFlyout
            objectName: "ceilingFlyout"
            title: "Ceiling"
            menuWidth: AppTheme.menuWidthFlyout
            AppMenuItem {
                objectName: "ceilingRow"
                text: "A room whose name is very much longer than any menu "
                      + "this product is ever going to draw for it"
            }
        }
        AppMenuSeparator {}
        AppMenuItem { objectName: "deleteItem"; text: "Delete message"; iconName: "delete"; danger: true }
    }

    function openMenu() { menu.popup(win.contentItem, 60, 40) }

    MenuKeycap { objectName: "keycapText"; keys: "ESC"; header: true; x: 24; y: 420 }
    MenuKeycap { objectName: "keycapIcon"; iconName: "keyboard_return"; x: 96; y: 420 }
    MenuSectionLabel { objectName: "sectionLabel"; text: "Rooms"; x: 160; y: 420 }
    QuickReactionStrip {
        objectName: "keyboardStrip"
        x: 24
        y: 448
        width: 192
    }
    StatusChip { objectName: "chipVerified"; label: "Verified"; iconName: "verified_user"; tone: "success"; x: 240; y: 416 }
    StatusChip { objectName: "chipActive"; label: "ACTIVE"; tone: "onAccent"; x: 340; y: 416 }
    StatusChip { objectName: "chipLoud"; label: "LOUD"; tone: "danger"; x: 430; y: 416 }
    StatusChip { objectName: "chipUnread"; label: "3"; tone: "danger"; solid: true; x: 500; y: 416 }

    // Neutral-chip probes, each on the surface its real hosts paint: the
    // member profile popover homeserver chip sits on a card, and since
    // fea70c63 a SettingsCard is stormPanel. A soft chip fill is a
    // percentage of its own ink OVER ITS PARENT, so the parent is part of
    // the measurement and a probe floating on the window background would
    // answer a question nobody asked.
    Rectangle {
        objectName: "chipHostPanel"
        x: 24; y: 476; width: 200; height: 34; color: AppTheme.stormPanel
        StatusChip {
            objectName: "chipNeutralPanel"
            anchors.centerIn: parent
            storm: true; tone: "neutral"; label: "matrix.example.org"
        }
    }
    Rectangle {
        objectName: "chipHostCanvas"
        x: 240; y: 476; width: 200; height: 34; color: AppTheme.stormCanvas
        StatusChip {
            objectName: "chipNeutralCanvas"
            anchors.centerIn: parent
            storm: true; tone: "neutral"; label: "matrix.example.org"
        }
    }
    Rectangle {
        objectName: "chipHostLegacy"
        x: 456; y: 476; width: 200; height: 34; color: AppTheme.surface
        StatusChip {
            objectName: "chipNeutralLegacy"
            anchors.centerIn: parent
            tone: "neutral"; label: "Upgraded"
        }
    }

    // ── The REST of the soft-chip family, on the same two real grounds ───
    //
    // `neutral` was fixed first and its probes are above. Every OTHER soft
    // tone has the identical shape — a 14% tint of its own ink, so the label
    // measures worse on its own pill than on the card behind it — and the
    // 2026-09-20 sweep measured all five under 4.5:1 AA somewhere in the
    // eleven palettes. One host rectangle per ground, painted in the surface
    // its real callers paint, because a chip measured on the window
    // background answers a question nobody asked.
    Rectangle {
        objectName: "toneHostPanel"
        x: 24; y: 520; width: 620; height: 34; color: AppTheme.stormPanel
        Row {
            anchors.centerIn: parent
            spacing: 8
            StatusChip { objectName: "tonePanelAccent";  storm: true; tone: "accent";  label: "Invited" }
            StatusChip { objectName: "tonePanelSuccess"; storm: true; tone: "success"; label: "Verified" }
            StatusChip { objectName: "tonePanelWarning"; storm: true; tone: "warning"; label: "Pending" }
            StatusChip { objectName: "tonePanelDanger";  storm: true; tone: "danger";  label: "Blocked" }
            StatusChip { objectName: "tonePanelInfo";    storm: true; tone: "info";    label: "Beta" }
        }
    }
    Rectangle {
        objectName: "toneHostCanvas"
        x: 24; y: 560; width: 620; height: 34; color: AppTheme.stormCanvas
        Row {
            anchors.centerIn: parent
            spacing: 8
            StatusChip { objectName: "toneCanvasAccent";  storm: true; tone: "accent";  label: "Invited" }
            StatusChip { objectName: "toneCanvasSuccess"; storm: true; tone: "success"; label: "Verified" }
            StatusChip { objectName: "toneCanvasWarning"; storm: true; tone: "warning"; label: "Pending" }
            StatusChip { objectName: "toneCanvasDanger";  storm: true; tone: "danger";  label: "Blocked" }
            StatusChip { objectName: "toneCanvasInfo";    storm: true; tone: "info";    label: "Beta" }
        }
    }
    // The LEGACY chip vocabulary (storm: false) has the same 14% fill and a
    // different set of tone tokens, so it is a separate question and gets its
    // own row rather than an assumption.
    Rectangle {
        objectName: "toneHostSurface"
        x: 24; y: 600; width: 620; height: 34; color: AppTheme.surface
        Row {
            anchors.centerIn: parent
            spacing: 8
            StatusChip { objectName: "toneLegacyAccent";  tone: "accent";  label: "Invited" }
            StatusChip { objectName: "toneLegacySuccess"; tone: "success"; label: "Verified" }
            StatusChip { objectName: "toneLegacyWarning"; tone: "warning"; label: "Pending" }
            StatusChip { objectName: "toneLegacyDanger";  tone: "danger";  label: "Blocked" }
            StatusChip { objectName: "toneLegacyInfo";    tone: "info";    label: "Beta" }
        }
    }

    // The DERIVED inks, so statusChipTonesResolveToTokens can name what a
    // soft label must now be without recomputing the derivation in C++ —
    // which would only prove the test agrees with itself.
    Rectangle { objectName: "candLegacySuccess"; visible: false; color: AppTheme.softChipInk(AppTheme.presenceOnline) }
    Rectangle { objectName: "candLegacyDanger";  visible: false; color: AppTheme.softChipInk(AppTheme.mentionBadge) }
    // The RAW tone tokens, so a case can assert that the pill still carries
    // its own family in its fill and border while only the label steps up.
    Rectangle { objectName: "toneStormAccent";  visible: false; color: AppTheme.stormLink }
    Rectangle { objectName: "toneStormSuccess"; visible: false; color: AppTheme.stormSuccess }
    Rectangle { objectName: "toneStormWarning"; visible: false; color: AppTheme.warning }
    Rectangle { objectName: "toneStormDanger";  visible: false; color: AppTheme.stormDanger }
    Rectangle { objectName: "toneStormInfo";    visible: false; color: AppTheme.info }
    Rectangle { objectName: "toneLegacyAccentTok";  visible: false; color: AppTheme.accent }
    Rectangle { objectName: "toneLegacySuccessTok"; visible: false; color: AppTheme.presenceOnline }
    Rectangle { objectName: "toneLegacyWarningTok"; visible: false; color: AppTheme.warning }
    Rectangle { objectName: "toneLegacyDangerTok";  visible: false; color: AppTheme.mentionBadge }
    Rectangle { objectName: "toneLegacyInfoTok";    visible: false; color: AppTheme.info }

    // ── The composer command autocomplete ────────────────────────────────
    //
    // Same menu language, and a row here is TWO surfaces: the panel, and the
    // selection chip over the panel. Only the panel had ever been graded.
    // NO APOSTROPHE in these strings, for the moc reason above the scene.
    SlashCommandPopup {
        id: slashPopup
        completions: [
            { name: "me", argsHint: "<message>",
              description: "Send an emote to the room", enabled: true },
            { name: "shrug", argsHint: "",
              description: "Append a shrug to your message", enabled: true },
            { name: "op", argsHint: "<user> [level]",
              description: "Set a power level", enabled: false }
        ]
        anchorInputTop: Qt.point(24, 660)
        anchorWidth: 360
    }
    function openSlash() { slashPopup.open() }
    function selectSlashRow(i) { slashPopup.currentIndex = i }
}
)QML";

} // namespace

class MenuSystemQmlTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    QQuickItem *item(const char *name) const
    {
        return m_root->findChild<QQuickItem *>(QLatin1String(name));
    }

    QColor token(const char *name) const
    {
        auto *it = m_root->findChild<QQuickItem *>(QLatin1String(name));
        return it ? it->property("color").value<QColor>() : QColor();
    }

    QQuickItem *rowChild(QQuickItem *menuItem, const char *objectName) const
    {
        return menuItem ? menuItem->findChild<QQuickItem *>(
                              QLatin1String(objectName))
                        : nullptr;
    }

    // The Icon and Label in an AppMenuItem's content row, found structurally
    // (first visible child with `name` = Icon; with `elide` but no `name` =
    // Label) within the row's contentItem, so the Storm background's
    // decorative Icon and the hidden StormNode are never mistaken for them.
    QQuickItem *leadingIcon(QQuickItem *menuItem) const
    {
        auto *content =
            menuItem->property("contentItem").value<QQuickItem *>();
        if (!content)
            return nullptr;
        const auto all = content->findChildren<QQuickItem *>();
        for (QQuickItem *child : all) {
            if (child->isVisible()
                && child->metaObject()->indexOfProperty("name") >= 0
                && child->metaObject()->indexOfProperty("size") >= 0)
                return child;
        }
        return nullptr;
    }

    QQuickItem *label(QQuickItem *menuItem) const
    {
        auto *content =
            menuItem->property("contentItem").value<QQuickItem *>();
        if (!content)
            return nullptr;
        const auto all = content->findChildren<QQuickItem *>();
        for (QQuickItem *child : all) {
            if (child->isVisible()
                && child->metaObject()->indexOfProperty("elide") >= 0
                && child->metaObject()->indexOfProperty("name") < 0)
                return child;
        }
        return nullptr;
    }

    // Open a nested AppMenu as the app does, by activating the row its parent
    // menu generated: QQuickMenu's cascade sizes and places the submenu
    // itself, and opening it by hand sizes content differently.
    void openViaParentRow(QObject *menu, QObject *flyout,
                          QQuickWindow *window) const
    {
        QQuickItem *parentRow = nullptr;
        const int count = menu->property("count").toInt();
        for (int i = 0; i < count; ++i) {
            QQuickItem *row = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt",
                                      Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(int, i));
            if (row && row->property("subMenu").value<QObject *>() == flyout) {
                parentRow = row;
                break;
            }
        }
        QVERIFY(parentRow);
        const QPointF centre = parentRow->mapToScene(
            QPointF(parentRow->width() / 2, parentRow->height() / 2));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          centre.toPoint());
        QTRY_VERIFY(flyout->property("opened").toBool());
    }

    void openMenu()
    {
        QMetaObject::invokeMethod(m_root, "openMenu");
        auto *menu = m_root->findChild<QObject *>(QStringLiteral("menu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
    }

    void closeMenu()
    {
        auto *menu = m_root->findChild<QObject *>(QStringLiteral("menu"));
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());
    }

private slots:
    void initTestCase()
    {
        QQmlComponent component(&m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("menuscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void cleanupTestCase()
    {
        delete m_root;
        m_root = nullptr;
    }

    void keycapChipImplementsSpecTreatment()
    {
        auto *chip = item("keycapText");
        QVERIFY(chip);
        QCOMPARE(chip->property("radius").toInt(), 4);
        // Resting keycaps are transparent chips with the strong storm outline.
        QCOMPARE(chip->property("color").value<QColor>().alpha(), 0);
        QCOMPARE(chip->property("border").value<QObject *>()
                     ->property("color").value<QColor>(),
                 token("tokStormBorderStrong"));
        auto *chipLabel = rowChild(chip, "keycapLabel");
        QVERIFY(chipLabel);
        QCOMPARE(chipLabel->property("text").toString(),
                 QStringLiteral("ESC"));
        const QFont font = chipLabel->property("font").value<QFont>();
        QCOMPARE(font.family(), QStringLiteral("JetBrains Mono"));
        QCOMPARE(font.pixelSize(), 10);
        QCOMPARE(chipLabel->property("color").value<QColor>(),
                 token("tokStormTextMuted"));
        // Icon mode: glyphs the mono face lacks (↵) render as an Icon.
        auto *iconChip = item("keycapIcon");
        QVERIFY(iconChip);
        auto *glyph = rowChild(iconChip, "keycapGlyph");
        QVERIFY(glyph);
        QVERIFY(glyph->property("visible").toBool());
        QCOMPARE(glyph->property("name").toString(),
                 QStringLiteral("keyboard_return"));
        QVERIFY(!rowChild(iconChip, "keycapLabel")->property("visible").toBool());
    }

    // The section label uses the UI face at 12/600 in sentence case, in an
    // ink that clears AA. Mono is reserved for genuinely monospaced content
    // (keycaps, code, Matrix identifiers).
    void sectionLabelUsesAccessibleMutedInkSentenceCase()
    {
        auto *sectionLabel = item("sectionLabel");
        QVERIFY(sectionLabel);
        const QFont font = sectionLabel->property("font").value<QFont>();
        QCOMPARE(font.pixelSize(), 12);
        QVERIFY2(font.family() != QStringLiteral("JetBrains Mono"),
                 "the section heading is sentence text, not code — mono here "
                 "is the 'out of place' treatment this round removed");
        QCOMPARE(int(font.weight()), int(QFont::DemiBold));
        QCOMPARE(int(font.capitalization()), int(QFont::MixedCase));
        QCOMPARE(qRound(font.letterSpacing()), 0);
        // Muted, not faint: a readable heading at 12 px.
        QCOMPARE(sectionLabel->property("color").value<QColor>(),
                 token("tokStormTextMuted"));
    }

    void quickReactionStripIsKeyboardOperable()
    {
        auto *strip = item("keyboardStrip");
        QVERIFY(strip);
        // Focusing the strip lands on a cell; Right moves focus; Return picks
        // the focused emoji (the keyboard path replacing the "React" row).
        QMetaObject::invokeMethod(strip, "forceActiveFocus");
        QTRY_VERIFY(m_window->activeFocusItem() != nullptr);
        QTest::keyClick(m_window, Qt::Key_Right);
        QSignalSpy picked(strip, SIGNAL(picked(QString)));
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(picked.count(), 1);
        // Second cell of the default set after one Right from cell 0.
        QCOMPARE(picked.first().first().toString(), QStringLiteral("🔥"));
    }

    // Chip fill and border resolve to the tone token; the label is that token
    // stepped in lightness until it clears the fill (a raw badge fill as text
    // ink failed contrast). A solid chip's fill is the token itself.
    void statusChipTonesResolveToTokens()
    {
        auto *verified = item("chipVerified");
        QVERIFY(verified);
        QCOMPARE(QColor(verified->property("color").value<QColor>().rgb()),
                 token("tokPresenceOnline"));
        QCOMPARE(rowChild(verified, "chipLabel")
                     ->property("color").value<QColor>(),
                 token("candLegacySuccess"));
        QVERIFY(rowChild(verified, "chipIcon")->property("visible").toBool());
        auto *loud = item("chipLoud");
        QVERIFY(loud);
        QCOMPARE(QColor(loud->property("color").value<QColor>().rgb()),
                 token("tokMentionBadge"));
        QCOMPARE(rowChild(loud, "chipLabel")
                     ->property("color").value<QColor>(),
                 token("candLegacyDanger"));
        auto *unread = item("chipUnread");
        QVERIFY(unread);
        QCOMPARE(unread->property("color").value<QColor>(),
                 token("tokMentionBadge"));
        auto *active = item("chipActive");
        QVERIFY(active);
        QCOMPARE(rowChild(active, "chipLabel")
                     ->property("color").value<QColor>(),
                 token("tokAccentText"));
    }

    void menuUsesSpecContainerAndRowMetrics()
    {
        openMenu();
        auto *menu = m_root->findChild<QObject *>(QStringLiteral("menu"));
        QCOMPARE(menu->property("width").toInt(), 252);
        auto *reply = item("replyItem");
        QVERIFY(reply);
        QCOMPARE(reply->height(), 32.0);
        auto *icon = leadingIcon(reply);
        QVERIFY(icon);
        QCOMPARE(icon->property("size").toInt(), 17);
        QCOMPARE(icon->property("color").value<QColor>(),
                 token("tokStormTextMuted"));
        // The accelerator keycap renders on the row.
        auto *accel = rowChild(reply, "keycapLabel");
        QVERIFY(accel);
        QCOMPARE(accel->property("text").toString(), QStringLiteral("R"));
        closeMenu();
    }

    void highlightedRowUsesAccentSoftWithSelectedInk()
    {
        openMenu();
        auto *menu = m_root->findChild<QObject *>(QStringLiteral("menu"));
        menu->setProperty("currentIndex", 0);
        auto *reply = item("replyItem");
        QVERIFY(reply);
        QTRY_VERIFY(reply->property("highlighted").toBool());
        const QImage img = m_window->grabWindow();
        QVERIFY(!img.isNull());
        const QPointF inside = reply->mapToScene(QPointF(3, reply->height() / 2));
        QVERIFY(channelDelta(sampleAvg(img, QRect(int(inside.x()),
                                                  int(inside.y()) - 1, 2, 3)),
                             token("tokStormSelection")) <= kTolerance);
        // Highlighted: the label brightens to stormText and the icon inks
        // bolt.
        QCOMPARE(label(reply)->property("color").value<QColor>(),
                 token("tokStormText"));
        QCOMPARE(leadingIcon(reply)->property("color").value<QColor>(),
                 token("tokBolt"));
        menu->setProperty("currentIndex", -1);
        closeMenu();
    }

    void dangerRowReadsInDangerInk()
    {
        openMenu();
        auto *deleteItem = item("deleteItem");
        QVERIFY(deleteItem);
        QCOMPARE(label(deleteItem)->property("color").value<QColor>(),
                 token("tokStormDanger"));
        QCOMPARE(leadingIcon(deleteItem)->property("color").value<QColor>(),
                 token("tokStormDanger"));
        closeMenu();
    }

    void flyoutOpensBesideParentAndEscClosesInnermostFirst()
    {
        openMenu();
        auto *menu = m_root->findChild<QObject *>(QStringLiteral("menu"));
        auto *flyout = m_root->findChild<QObject *>(QStringLiteral("flyout"));
        QVERIFY(flyout);
        QCOMPARE(flyout->property("width").toInt(), 150);

        // The generated parent row carries the flyout's icon and a chevron.
        QQuickItem *parentRow = nullptr;
        const int count = menu->property("count").toInt();
        for (int i = 0; i < count; ++i) {
            QQuickItem *row = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt",
                                      Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(int, i));
            if (row && row->property("subMenu").value<QObject *>() == flyout) {
                parentRow = row;
                break;
            }
        }
        QVERIFY2(parentRow, "no menu row exposes the nested flyout");
        QCOMPARE(parentRow->property("iconName").toString(),
                 QStringLiteral("notifications"));

        // Open the flyout via its parent row, check it lands beside the parent
        // menu, then Escape unwinds innermost-first.
        const QPointF center = parentRow->mapToScene(
            QPointF(parentRow->width() / 2, parentRow->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          center.toPoint());
        QTRY_VERIFY(flyout->property("opened").toBool());
        QVERIFY(menu->property("opened").toBool());

        auto *flyoutItem = item("radioAll");
        QVERIFY(flyoutItem);
        const qreal flyoutSceneX =
            flyoutItem->mapToScene(QPointF(0, 0)).x();
        const qreal parentRightX =
            parentRow->mapToScene(QPointF(parentRow->width(), 0)).x();
        QVERIFY2(flyoutSceneX >= parentRightX - 12,
                 qPrintable(QStringLiteral("flyout at %1, parent right %2")
                                .arg(flyoutSceneX).arg(parentRightX)));

        // Radio rows: the selected row has the bolt-filled node, the unselected
        // row the dashed ring.
        auto *selectedFill = flyoutItem->findChild<QQuickItem *>(
            QStringLiteral("stormNodeFill"));
        QVERIFY(selectedFill);
        QVERIFY(selectedFill->property("visible").toBool());
        QCOMPARE(selectedFill->property("color").value<QColor>(),
                 token("tokBolt"));
        auto *mentions = item("radioMentions");
        QVERIFY(mentions);
        auto *mentionsFill = mentions->findChild<QQuickItem *>(
            QStringLiteral("stormNodeFill"));
        auto *mentionsRing = mentions->findChild<QQuickItem *>(
            QStringLiteral("stormNodeDashRing"));
        QVERIFY(mentionsFill);
        QVERIFY(mentionsRing);
        QVERIFY(!mentionsFill->property("visible").toBool());
        QVERIFY(mentionsRing->property("visible").toBool());

        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!flyout->property("opened").toBool());
        QVERIFY(menu->property("opened").toBool());
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
    }

    void radioSelectionBindingSurvivesActivation()
    {
        openMenu();
        auto *menu = m_root->findChild<QObject *>(QStringLiteral("menu"));
        auto *flyout = m_root->findChild<QObject *>(QStringLiteral("flyout"));
        QQuickItem *parentRow = nullptr;
        const int count = menu->property("count").toInt();
        for (int i = 0; i < count; ++i) {
            QQuickItem *row = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt",
                                      Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(int, i));
            if (row && row->property("subMenu").value<QObject *>() == flyout) {
                parentRow = row;
                break;
            }
        }
        QVERIFY(parentRow);
        QPointF center = parentRow->mapToScene(
            QPointF(parentRow->width() / 2, parentRow->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          center.toPoint());
        QTRY_VERIFY(flyout->property("opened").toBool());

        // Clicking a row must not flip radioSelected itself: the owner's
        // binding stays authoritative (an internal write would destroy it).
        auto *mentions = item("radioMentions");
        QVERIFY(mentions);
        QVERIFY(!mentions->property("radioSelected").toBool());
        center = mentions->mapToScene(
            QPointF(mentions->width() / 2, mentions->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          center.toPoint());
        QTRY_VERIFY(!menu->property("opened").toBool());
        QVERIFY(!mentions->property("radioSelected").toBool());

        // The owner flipping its state re-renders both rows: binding alive.
        m_root->setProperty("allMessagesSelected", false);
        QTRY_VERIFY(mentions->property("radioSelected").toBool());
        auto *all = item("radioAll");
        QVERIFY(all);
        QVERIFY(!all->property("radioSelected").toBool());
        m_root->setProperty("allMessagesSelected", true);
    }

    void themeSwitchRoutesStormMenuLanguagePerLegacyTheme()
    {
        // The storm* namespace is theme-routed: under themes 1-10 it resolves
        // to that theme's own tones, and only Storm (11) uses the fixed
        // literals. Switching between legacy themes retints the keycap;
        // switching to Storm always lands on the Storm value.
        const QColor indigoSoft = token("tokAccentSoft");
        const QColor indigoBorderStrong = token("tokStormBorderStrong");
        auto *chip = item("keycapText");
        QVERIFY(chip);
        QCOMPARE(chip->property("border").value<QObject *>()
                     ->property("color").value<QColor>(),
                 indigoBorderStrong);

        m_root->setProperty("themeMode", 10); // Deep Teal
        QTRY_VERIFY(token("tokAccentSoft") != indigoSoft);
        const QColor tealBorderStrong = token("tokStormBorderStrong");
        QVERIFY2(tealBorderStrong != indigoBorderStrong,
                 "legacy->legacy theme switch must retint the routed "
                 "storm border, not stay invariant");
        QCOMPARE(chip->property("color").value<QColor>().alpha(), 0);
        QCOMPARE(chip->property("border").value<QObject *>()
                     ->property("color").value<QColor>(),
                 tealBorderStrong);

        // Storm resolves to one fixed value whichever theme preceded it; read
        // from the token rather than a copied hex, and checked across a round
        // trip.
        m_root->setProperty("themeMode", 11);
        QTRY_COMPARE(chip->property("border").value<QObject *>()
                         ->property("color").value<QColor>(),
                     token("tokStormBorderStrong"));
        const QColor stormBorderStrong = token("tokStormBorderStrong");
        QVERIFY2(stormBorderStrong != tealBorderStrong
                     && stormBorderStrong != indigoBorderStrong,
                 "Storm must resolve to its OWN border, not a legacy theme's");

        // Round trip: away to a legacy theme and back lands on the same value.
        m_root->setProperty("themeMode", 10);
        QTRY_COMPARE(token("tokStormBorderStrong"), tealBorderStrong);
        m_root->setProperty("themeMode", 11);
        QTRY_COMPARE(token("tokStormBorderStrong"), stormBorderStrong);
        QCOMPARE(chip->property("border").value<QObject *>()
                     ->property("color").value<QColor>(),
                 stormBorderStrong);

        m_root->setProperty("themeMode", 9);
    }

    // A menu's design width is a floor, not a clamp: it widens to its widest
    // row instead of eliding. Asserted as the user's condition (the row is not
    // truncated), since a slightly wider menu could still elide.
    void theFlyoutWidensToItsWidestRowInsteadOfEliding()
    {
        openMenu();
        auto *flyout = m_root->findChild<QObject *>(QStringLiteral("fitFlyout"));
        QVERIFY(flyout);
        // Unopened it is exactly its design width: a closed popup has no
        // measurable rows (a Layout skips items that are not visible).
        QCOMPARE(flyout->property("width").toInt(),
                 flyout->property("menuWidth").toInt());

        openViaParentRow(m_root->findChild<QObject *>(QStringLiteral("menu")),
                         flyout, m_window);

        auto *longRow = item("fitLongRow");
        QVERIFY(longRow);
        auto *longLabel = label(longRow);
        QVERIFY(longLabel);
        QTRY_VERIFY(longLabel->width() > 0);
        // QTRY: the menu resizes at `opened` and QQuickMenu passes the width
        // to its rows on the next polish.
        QTRY_VERIFY2(!longLabel->property("truncated").toBool(),
                 qPrintable(QStringLiteral(
                     "'%1' is elided at %2 px inside a %3 px menu (it needs "
                     "%4 px)")
                        .arg(longRow->property("text").toString())
                        .arg(longLabel->width())
                        .arg(flyout->property("width").toInt())
                        .arg(longLabel->property("contentWidth").toReal())));
        QTRY_VERIFY2(flyout->property("width").toInt()
                         > flyout->property("menuWidth").toInt(),
                     "the flyout did not widen past its design width at all");

        // The widest row decides the width, not the short one.
        auto *shortRow = item("fitShortRow");
        QVERIFY(shortRow);
        QVERIFY(longRow->implicitWidth() > shortRow->implicitWidth());

        QMetaObject::invokeMethod(flyout, "close");
        QTRY_VERIFY(!flyout->property("opened").toBool());
        closeMenu();
    }

    // Menu rows carry remote text, so widening stops at a ceiling, past which
    // the row elides.
    void theMenuFitStopsAtItsCeilingAndTheRowElidesThere()
    {
        openMenu();
        auto *flyout =
            m_root->findChild<QObject *>(QStringLiteral("ceilingFlyout"));
        QVERIFY(flyout);
        openViaParentRow(m_root->findChild<QObject *>(QStringLiteral("menu")),
                         flyout, m_window);

        const int ceiling = flyout->property("menuWidthMax").toInt();
        QVERIFY(ceiling > 0);
        QTRY_COMPARE(flyout->property("width").toInt(), ceiling);
        auto *row = item("ceilingRow");
        QVERIFY(row);
        QVERIFY2(label(row)->property("truncated").toBool(),
                 "a row wider than the ceiling must still elide");

        QMetaObject::invokeMethod(flyout, "close");
        QTRY_VERIFY(!flyout->property("opened").toBool());
        closeMenu();
    }

    // A non-MenuItem child of a menu sizes itself: `wrapMode` alone keeps the
    // implicitWidth of the unwrapped sentence, so the label must be given the
    // panel's width to wrap (a wrapping Text does not elide).
    void aWrappedLabelInAMenuStaysInsideItsPanel()
    {
        openMenu();
        auto *flyout = m_root->findChild<QObject *>(QStringLiteral("fitFlyout"));
        QVERIFY(flyout);
        openViaParentRow(m_root->findChild<QObject *>(QStringLiteral("menu")),
                         flyout, m_window);

        auto *disclaimer = item("fitDisclaimer");
        QVERIFY(disclaimer);
        QTRY_VERIFY(disclaimer->width() > 0);
        // It wrapped rather than being cut: more than one line, painted no
        // wider than its item.
        QTRY_VERIFY2(disclaimer->property("lineCount").toInt() > 1,
                     qPrintable(QStringLiteral(
                         "disclaimer drew %1 line(s) at %2 px — it is not "
                         "wrapping")
                            .arg(disclaimer->property("lineCount").toInt())
                            .arg(disclaimer->width())));
        const int panel = flyout->property("width").toInt();
        const int padding = flyout->property("leftPadding").toInt()
                            + flyout->property("rightPadding").toInt();
        QVERIFY2(disclaimer->width() <= panel - padding,
                 qPrintable(QStringLiteral("disclaimer is %1 px wide inside a "
                                           "%2 px panel (%3 of padding)")
                                .arg(disclaimer->width()).arg(panel)
                                .arg(padding)));
        QVERIFY(disclaimer->property("contentWidth").toReal()
                <= disclaimer->width() + 0.5);
        // And vertically inside the panel.
        const qreal bottom = disclaimer->y() + disclaimer->height();
        const qreal room = flyout->property("height").toReal()
                           - flyout->property("topPadding").toReal()
                           - flyout->property("bottomPadding").toReal();
        QVERIFY2(bottom <= room + 0.5,
                 qPrintable(QStringLiteral("disclaimer ends at y %1 in a "
                                           "%2 px content area")
                                .arg(bottom).arg(room)));

        QMetaObject::invokeMethod(flyout, "close");
        QTRY_VERIFY(!flyout->property("opened").toBool());
        closeMenu();
    }

    // The same properties on the real RoomActionsMenu (the compiled production
    // file), with only the four `app` values it reads stood in for.
    void theRealRoomNotificationsFlyoutFitsItsOwnText()
    {
        QQmlComponent fakeComponent(&m_engine);
        fakeComponent.setData(QByteArray(R"QML(
import QtQuick
QtObject {
    property QtObject roomList: QtObject {
        property bool roomFavouritesSupported: true
    }
    property bool serverRoomNotificationModes: false
    property QtObject settings: QtObject {
        signal roomNotificationModeChanged(string roomId)
        function roomNotificationMode(roomId) { return 1 }
    }
    signal roomNotificationModeSyncStateChanged(string roomId)
    function roomNotificationModeSyncFailed(roomId) { return false }
    function requestRoomNotificationMode(roomId) {}
}
)QML"), QUrl(QStringLiteral("menufakeapp.qml")));
        QScopedPointer<QObject> fake(fakeComponent.create());
        QVERIFY2(fake, qPrintable(fakeComponent.errorString()));

        QQmlContext ctx(m_engine.rootContext());
        ctx.setContextProperty(QStringLiteral("app"), fake.data());
        QQmlComponent sceneComponent(&m_engine);
        sceneComponent.setData(QByteArray(R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: w
    width: 900
    height: 560
    visible: true
    color: AppTheme.background
    property alias menu: roomMenu
    RoomActionsMenu {
        id: roomMenu
        objectName: "realRoomMenu"
        roomId: "!room:example.org"
        roomName: "General"
    }
    function openIt() { roomMenu.popup(w.contentItem, 40, 40) }
}
)QML"), QUrl(QStringLiteral("realroommenuscene.qml")));
        QScopedPointer<QObject> scene(sceneComponent.create(&ctx));
        QVERIFY2(scene, qPrintable(sceneComponent.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(scene.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *menu = scene->findChild<QObject *>(QStringLiteral("realRoomMenu"));
        QVERIFY(menu);
        QMetaObject::invokeMethod(scene.data(), "openIt");
        QTRY_VERIFY(menu->property("opened").toBool());

        auto *flyout =
            scene->findChild<QObject *>(QStringLiteral("roomNotificationsFlyout"));
        QVERIFY(flyout);
        // Opened directly: a click into this second window does not reach it
        // offscreen. The row check is about the menu's width; the disclaimer's
        // placement is covered by ContextMenuContractTest.
        QMetaObject::invokeMethod(flyout, "open");
        QTRY_VERIFY(flyout->property("opened").toBool());

        // The long notifications row.
        QQuickItem *mentionsRow = nullptr;
        const int count = flyout->property("count").toInt();
        for (int i = 0; i < count; ++i) {
            QQuickItem *row = nullptr;
            QMetaObject::invokeMethod(flyout, "itemAt",
                                      Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(int, i));
            if (row && row->property("text").toString().contains(
                           QStringLiteral("Mentions")))
                mentionsRow = row;
        }
        QVERIFY2(mentionsRow, "the flyout has no Mentions row");
        auto *mentionsLabel = label(mentionsRow);
        QVERIFY(mentionsLabel);
        QTRY_VERIFY(mentionsLabel->width() > 0);
        QTRY_VERIFY2(!mentionsLabel->property("truncated").toBool(),
                 qPrintable(QStringLiteral("'%1' is elided at %2 px (needs %3)")
                                .arg(mentionsRow->property("text").toString())
                                .arg(mentionsLabel->width())
                                .arg(mentionsLabel->property("contentWidth")
                                         .toReal())));

        // The notifications disclaimer.
        auto *disclaimer = scene->findChild<QQuickItem *>(
            QStringLiteral("roomNotificationDisclaimer"));
        QVERIFY(disclaimer);
        QTRY_VERIFY(disclaimer->width() > 0);
        QTRY_VERIFY2(disclaimer->property("lineCount").toInt() > 1,
                     "the disclaimer is still one clipped line");
        const int panel = flyout->property("width").toInt();
        const int padding = flyout->property("leftPadding").toInt()
                            + flyout->property("rightPadding").toInt();
        QVERIFY2(disclaimer->width() <= panel - padding,
                 qPrintable(QStringLiteral("disclaimer %1 px inside a %2 px "
                                           "panel").arg(disclaimer->width())
                                .arg(panel)));
        QVERIFY(disclaimer->property("contentWidth").toReal()
                <= disclaimer->width() + 0.5);
    }

    // A soft chip's label clears its own fill: the 14% fill lifts the
    // background towards the ink, so the label reads worse on the pill than on
    // the surface behind it. Eleven palettes must be demonstrably distinct, not
    // just iterated.
    void theNeutralChipInkClearsItsOwnFillOnEveryPalette()
    {
        struct Probe { const char *chip; const char *host; };
        const Probe probes[] = {
            { "chipNeutralPanel", "chipHostPanel" },
            { "chipNeutralCanvas", "chipHostCanvas" },
            { "chipNeutralLegacy", "chipHostLegacy" },
        };
        QSet<QRgb> distinctFills;
        for (int mode = 1; mode <= 11; ++mode) {
            m_root->setProperty("themeMode", mode);
            QTRY_COMPARE(item("chipHostPanel")->property("color").value<QColor>(),
                         token("tokStormPanel"));
            for (const Probe &p : probes) {
                auto *chip = item(p.chip);
                auto *host = item(p.host);
                QVERIFY(chip);
                QVERIFY(host);
                const QColor parent = host->property("color").value<QColor>();
                const QColor fill =
                    over(chip->property("color").value<QColor>(), parent);
                const QColor ink = rowChild(chip, "chipLabel")
                                       ->property("color").value<QColor>();
                const double ratio = contrastRatio(ink, fill);
                QVERIFY2(ratio >= 4.5,
                         qPrintable(QStringLiteral(
                             "theme %1: %2 label %3 on its own fill %4 "
                             "(parent %5) is %6:1, below 4.5 AA")
                                .arg(mode)
                                .arg(QString::fromLatin1(p.chip))
                                .arg(ink.name(), fill.name(), parent.name())
                                .arg(ratio, 0, 'f', 2)));
                if (qstrcmp(p.chip, "chipNeutralPanel") == 0)
                    distinctFills.insert(fill.rgb());
            }
        }
        QCOMPARE(distinctFills.size(), 11);
        m_root->setProperty("themeMode", 9);
    }

    // Every soft chip tone clears 4.5:1 against its own fill, on each of the
    // three surfaces chips are placed on, across all eleven palettes. Also:
    // the fill and border still carry the raw tone at the declared alpha, and
    // the ink stays in the tone's family (same hue and HSL saturation, only
    // lightness moves). `legibleChoice` stops as soon as 4.5 is cleared.
    void everySoftChipToneClearsItsOwnFillOnEveryPalette()
    {
        struct Probe { const char *chip; const char *host; const char *tone; };
        const Probe probes[] = {
            { "tonePanelAccent", "toneHostPanel", "toneStormAccent" },
            { "tonePanelSuccess", "toneHostPanel", "toneStormSuccess" },
            { "tonePanelWarning", "toneHostPanel", "toneStormWarning" },
            { "tonePanelDanger", "toneHostPanel", "toneStormDanger" },
            { "tonePanelInfo", "toneHostPanel", "toneStormInfo" },
            { "toneCanvasAccent", "toneHostCanvas", "toneStormAccent" },
            { "toneCanvasSuccess", "toneHostCanvas", "toneStormSuccess" },
            { "toneCanvasWarning", "toneHostCanvas", "toneStormWarning" },
            { "toneCanvasDanger", "toneHostCanvas", "toneStormDanger" },
            { "toneCanvasInfo", "toneHostCanvas", "toneStormInfo" },
            { "toneLegacyAccent", "toneHostSurface", "toneLegacyAccentTok" },
            { "toneLegacySuccess", "toneHostSurface", "toneLegacySuccessTok" },
            { "toneLegacyWarning", "toneHostSurface", "toneLegacyWarningTok" },
            { "toneLegacyDanger", "toneHostSurface", "toneLegacyDangerTok" },
            { "toneLegacyInfo", "toneHostSurface", "toneLegacyInfoTok" },
        };
        QSet<QRgb> distinctFills;
        QSet<QRgb> distinctPanels;
        int measured = 0;
        for (int mode = 1; mode <= 11; ++mode) {
            m_root->setProperty("themeMode", mode);
            QTRY_COMPARE(item("toneHostPanel")->property("color").value<QColor>(),
                         token("tokStormPanel"));
            distinctPanels.insert(
                item("toneHostPanel")->property("color").value<QColor>().rgb());
            for (const Probe &p : probes) {
                auto *chip = item(p.chip);
                auto *host = item(p.host);
                QVERIFY2(chip, p.chip);
                QVERIFY2(host, p.host);
                const QColor parent = host->property("color").value<QColor>();
                const QColor raw = token(p.tone);
                const QColor fillColor =
                    chip->property("color").value<QColor>();
                const QColor fill = over(fillColor, parent);
                const QColor ink = rowChild(chip, "chipLabel")
                                       ->property("color").value<QColor>();
                const double ratio = contrastRatio(ink, fill);
                QVERIFY2(ratio >= 4.5,
                         qPrintable(QStringLiteral(
                             "theme %1: %2 label %3 on its own fill %4 "
                             "(parent %5) is %6:1, below 4.5 AA")
                                .arg(mode)
                                .arg(QString::fromLatin1(p.chip))
                                .arg(ink.name(), fill.name(), parent.name())
                                .arg(ratio, 0, 'f', 2)));

                // The pill still is its tone: only the label moved.
                QVERIFY2(channelDelta(QColor(fillColor.rgb()), raw) <= 1,
                         qPrintable(QStringLiteral(
                             "theme %1: %2 fill is %3, not the raw tone %4 — "
                             "the tone family must stay in the fill")
                                .arg(mode)
                                .arg(QString::fromLatin1(p.chip))
                                .arg(fillColor.name(), raw.name())));
                QVERIFY2(qAbs(fillColor.alphaF() - 0.14) < 0.005,
                         qPrintable(QStringLiteral("theme %1: %2 fill alpha "
                                                   "is %3, not 0.14")
                                        .arg(mode)
                                        .arg(QString::fromLatin1(p.chip))
                                        .arg(fillColor.alphaF())));
                const QColor borderColor =
                    QQmlProperty::read(chip, QStringLiteral("border.color"))
                        .value<QColor>();
                QVERIFY2(channelDelta(QColor(borderColor.rgb()), raw) <= 1,
                         qPrintable(QStringLiteral(
                             "theme %1: %2 border is %3, not the raw tone %4")
                                .arg(mode)
                                .arg(QString::fromLatin1(p.chip))
                                .arg(borderColor.name(), raw.name())));

                // The ink keeps the hue and saturation; jumping to a neutral
                // text ink would clear AA but lose the tone.
                if (raw.hslSaturationF() >= 0.15) {
                    double hueGap = qAbs(ink.hslHueF() - raw.hslHueF());
                    if (hueGap > 0.5)
                        hueGap = 1.0 - hueGap;
                    QVERIFY2(hueGap * 360.0 <= 3.0,
                             qPrintable(QStringLiteral(
                                 "theme %1: %2 ink %3 is %4 degrees of hue "
                                 "from its tone %5 — it stopped being that "
                                 "tone")
                                    .arg(mode)
                                    .arg(QString::fromLatin1(p.chip))
                                    .arg(ink.name())
                                    .arg(hueGap * 360.0, 0, 'f', 1)
                                    .arg(raw.name())));
                    QVERIFY2(qAbs(ink.hslSaturationF()
                                  - raw.hslSaturationF()) <= 0.05,
                             qPrintable(QStringLiteral(
                                 "theme %1: %2 ink %3 saturation %4 against "
                                 "the tone %5 at %6")
                                    .arg(mode)
                                    .arg(QString::fromLatin1(p.chip))
                                    .arg(ink.name())
                                    .arg(ink.hslSaturationF())
                                    .arg(raw.name())
                                    .arg(raw.hslSaturationF())));
                }
                ++measured;
                if (qstrcmp(p.host, "toneHostPanel") == 0)
                    distinctFills.insert(fill.rgb());
            }
        }
        // Palettes must be distinct, and the number of chips measured is
        // asserted rather than loop iterations.
        QCOMPARE(distinctPanels.size(), 11);
        // Five tones times eleven palettes, all different fills: a loop that
        // read one palette eleven times would give 5.
        QCOMPARE(distinctFills.size(), 55);
        QCOMPARE(measured, 11 * int(std::size(probes)));
        m_root->setProperty("themeMode", 9);
    }

    // A slash-command row's inks clear AA on the fill that row paints: the
    // selected row paints `stormSelection` over the panel, which is where
    // both inks had failed. Also asserted: the name stays at least 1.3x the
    // description's contrast (a visible hierarchy), and the row's own
    // `rowFill` matches the composite computed here. Palettes must be
    // distinct.
    void theSlashCommandRowInksClearTheFillThatRowPaintsOnEveryPalette()
    {
        QMetaObject::invokeMethod(m_root, "openSlash");
        auto *popup = m_root->findChild<QObject *>(
            QStringLiteral("slashCommandPopup"));
        QVERIFY(popup);
        QTRY_VERIFY(popup->property("opened").toBool());
        auto *list = item("slashCommandPopupList");
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), 3);
        auto *surface = item("slashCommandPopupSurface");
        QVERIFY(surface);

        auto rowAt = [&](int i) -> QQuickItem * {
            QQuickItem *row = nullptr;
            QMetaObject::invokeMethod(list, "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(int, i));
            return row;
        };

        QSet<QRgb> distinctPanels;
        QSet<QRgb> distinctFills;
        int measured = 0;
        int rawTokenFailures = 0;
        double worstName = 100.0;
        double worstDesc = 100.0;
        double worstHierarchy = 100.0;
        QString worstNameWhere;
        QString worstDescWhere;
        const int restoreMode = m_root->property("themeMode").toInt();
        for (int mode = 1; mode <= 11; ++mode) {
            m_root->setProperty("themeMode", mode);
            QTRY_COMPARE(surface->property("color").value<QColor>(),
                         token("tokStormPanel"));
            const QColor panel = surface->property("color").value<QColor>();
            distinctPanels.insert(panel.rgb());

            for (int selected = 0; selected < 3; ++selected) {
                QMetaObject::invokeMethod(m_root, "selectSlashRow",
                                          Q_ARG(QVariant, QVariant(selected)));
                QTRY_COMPARE(popup->property("currentIndex").toInt(), selected);
                for (int i = 0; i < 3; ++i) {
                    QQuickItem *row = rowAt(i);
                    QVERIFY2(row, qPrintable(QStringLiteral(
                                 "row %1 not instantiated").arg(i)));
                    QTRY_COMPARE(row->property("selected").toBool(),
                                 i == selected);
                    const bool rowEnabled =
                        row->property("rowEnabled").toBool();
                    const QColor fill =
                        over(row->property("color").value<QColor>(), panel);
                    distinctFills.insert(fill.rgb());
                    // The ground the QML derives against must be the ground it
                    // paints.
                    QCOMPARE(QColor(row->property("rowFill").value<QColor>()
                                        .rgb()),
                             QColor(fill.rgb()));

                    auto *name = rowChild(row, "slashCommandName");
                    auto *desc = rowChild(row, "slashCommandDescription");
                    QVERIFY(name && desc);
                    const QColor nameInk =
                        name->property("color").value<QColor>();
                    const QColor descInk =
                        desc->property("color").value<QColor>();
                    const double nameRatio = contrastRatio(nameInk, fill);
                    const double descRatio = contrastRatio(descInk, fill);
                    if (nameRatio < worstName) {
                        worstName = nameRatio;
                        worstNameWhere =
                            QStringLiteral("theme %1 row %2 %3 %4 on %5")
                                .arg(mode).arg(i)
                                .arg(i == selected ? QStringLiteral("sel")
                                                   : QStringLiteral("rest"))
                                .arg(nameInk.name(), fill.name());
                    }
                    if (descRatio < worstDesc) {
                        worstDesc = descRatio;
                        worstDescWhere =
                            QStringLiteral("theme %1 row %2 %3 %4 on %5")
                                .arg(mode).arg(i)
                                .arg(i == selected ? QStringLiteral("sel")
                                                   : QStringLiteral("rest"))
                                .arg(descInk.name(), fill.name());
                    }
                    QVERIFY2(nameRatio >= 4.5,
                             qPrintable(QStringLiteral(
                                 "theme %1, row %2 (%3, %4): the command name "
                                 "%5 is %6:1 on the fill %7 this row paints")
                                    .arg(mode).arg(i)
                                    .arg(i == selected ? QStringLiteral("selected")
                                                       : QStringLiteral("resting"),
                                         rowEnabled ? QStringLiteral("enabled")
                                                    : QStringLiteral("disabled"))
                                    .arg(nameInk.name())
                                    .arg(nameRatio, 0, 'f', 2)
                                    .arg(fill.name())));
                    QVERIFY2(descRatio >= 4.5,
                             qPrintable(QStringLiteral(
                                 "theme %1, row %2 (%3, %4): the description "
                                 "%5 is %6:1 on the fill %7 this row paints")
                                    .arg(mode).arg(i)
                                    .arg(i == selected ? QStringLiteral("selected")
                                                       : QStringLiteral("resting"),
                                         rowEnabled ? QStringLiteral("enabled")
                                                    : QStringLiteral("disabled"))
                                    .arg(descInk.name())
                                    .arg(descRatio, 0, 'f', 2)
                                    .arg(fill.name())));
                    // The hierarchy. A disabled row uses one ink on both lines
                    // (weight and size carry its hierarchy), so it is excluded.
                    if (rowEnabled) {
                        worstHierarchy = qMin(worstHierarchy,
                                              nameRatio / descRatio);
                        QVERIFY2(nameRatio / descRatio >= 1.3,
                                 qPrintable(QStringLiteral(
                                     "theme %1, row %2 (%3): name %4 at %5:1 "
                                     "over description %6 at %7:1 is a ratio "
                                     "of %8 — the command you are running "
                                     "does not lead its own description")
                                        .arg(mode).arg(i)
                                        .arg(i == selected
                                                 ? QStringLiteral("selected")
                                                 : QStringLiteral("resting"))
                                        .arg(nameInk.name())
                                        .arg(nameRatio, 0, 'f', 2)
                                        .arg(descInk.name())
                                        .arg(descRatio, 0, 'f', 2)
                                        .arg(nameRatio / descRatio, 0, 'f', 2)));
                    }
                    ++measured;

                    // Not vacuous: the raw tokens the row used to use fail on
                    // many of these triples, so this count must be non-zero.
                    if (i == selected) {
                        if (contrastRatio(token("tokBolt"), fill) < 4.5)
                            ++rawTokenFailures;
                        if (contrastRatio(token("tokTextMuted"), fill) < 4.5)
                            ++rawTokenFailures;
                    }
                }
            }
        }
        QCOMPARE(distinctPanels.size(), 11);
        // Eleven palettes times two fills (panel, selection); one palette read
        // eleven times would give 2.
        QCOMPARE(distinctFills.size(), 22);
        QCOMPARE(measured, 11 * 3 * 3);
        QVERIFY2(rawTokenFailures >= 18,
                 qPrintable(QStringLiteral(
                     "only %1 of the raw-token pairs fail AA on the selection "
                     "fill — this case can no longer tell the fix from its "
                     "absence").arg(rawTokenFailures)));
        qInfo("slash-command rows: %d (palette,row,state) triples measured, "
              "%d raw-token AA failures on the selected fill; worst name "
              "%.2f:1 (%s), worst description %.2f:1 (%s), worst hierarchy "
              "%.2f",
              measured, rawTokenFailures,
              worstName, qPrintable(worstNameWhere),
              worstDesc, qPrintable(worstDescWhere), worstHierarchy);

        QMetaObject::invokeMethod(popup, "close");
        QTRY_VERIFY(!popup->property("opened").toBool());
        m_root->setProperty("themeMode", restoreMode);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    MenuSystemQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MenuSystemQmlTest.moc"
