// v0.6.5 shared menu language (SPEC §0): offscreen proof that the upgraded
// AppMenu/AppMenuItem/AppMenuSeparator and the new MenuKeycap /
// MenuSectionLabel / StatusChip primitives implement the design contract —
// 32px radius-8 rows with an 18px muted icon, accentSoft highlight with
// selectedText ink, filled mono keycap chips, the danger treatment, radio
// flyout rows whose selection binding an internal toggle can never destroy,
// and a cascading flyout that closes before its parent on Escape. Expected
// colors are read back from token probes in the same scene, never hard-coded.

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

// WCAG 2.x relative luminance / contrast, and the SOURCE-OVER composite a
// translucent chip fill performs against its parent. A soft StatusChip is
// `Qt.alpha(ink, 0.14)` — the pixel under the label is not the chip's colour
// property, it is that colour over whatever the chip was dropped on, which
// is why the parent is read out of the scene too.
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

// NO APOSTROPHE MAY APPEAR INSIDE THIS RAW STRING. moc lexes a bare `'`
// as the start of a character literal even inside R"QML(...)QML", runs off
// the end of the file, reports "No relevant classes found", generates an
// EMPTY .moc — and the only symptom is `undefined reference to vtable for
// MenuSystemQmlTest` at link time, which names nothing. Measured
// 2026-09-20 on moc 6.11.1.
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

    // The Icon and Label inside an AppMenuItem's content row, located
    // structurally (first VISIBLE child with a "name" property = Icon;
    // first visible one with "elide" but no "name" = Label) so the test
    // does not depend on private names. Scoped to the row's contentItem:
    // the Storm background carries a decorative edge-bolt Icon (and every
    // row hosts an invisible StormNode) that must never be mistaken for
    // the row's own icon or label.
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

    // Open a nested AppMenu THE WAY THE APPLICATION DOES: by activating the
    // row its parent menu generated for it. Calling `open()` on the submenu
    // directly is not the same path — QQuickMenu's cascade sizes and places
    // a submenu itself, and a flyout opened by hand sized its own content
    // items where the real one did not. Measured 2026-09-20: a disclaimer
    // that was visibly cut mid-word in the running app came out correctly
    // wrapped in a test that opened the flyout by hand. A probe is only
    // evidence if it shares the path under test.
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
        // Storm §3.2: resting keycaps are transparent chips with the strong
        // storm outline.
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

    // 2026-08-21: this label used to be JetBrains Mono at 10px, ALL CAPS,
    // 1.6px tracking, in the faint ink — a third typeface inside a menu head
    // whose actual content ("Reply", "Copy text") was set quieter in a
    // different face, applied on the light themes too where the Storm
    // language was never meant to reach. The user's report called out "the
    // font in a lot of places looks out of place" and supplied a screenshot
    // of exactly this treatment.
    //
    // The guard is kept, and still has teeth — it pins a SPECIFIC
    // typographic contract, just the current one: the UI face at 12/600 in
    // sentence case, in the ink that clears AA rather than the decorative
    // faint one. Mono survives where something is genuinely monospaced
    // (MenuKeycap, CodeBlock, Matrix identifiers), which the keycap cases
    // above still assert.
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
        // Muted, not faint: at 12px sentence case this is a readable heading,
        // so it takes an ink that clears AA rather than a decorative one.
        QCOMPARE(sectionLabel->property("color").value<QColor>(),
                 token("tokStormTextMuted"));
    }

    void quickReactionStripIsKeyboardOperable()
    {
        auto *strip = item("keyboardStrip");
        QVERIFY(strip);
        // Focusing the strip root lands on a concrete cell; Right moves the
        // cell focus; Return picks the focused emoji — the arrow-reachable
        // replacement contract for the removed "React" menu row.
        QMetaObject::invokeMethod(strip, "forceActiveFocus");
        QTRY_VERIFY(m_window->activeFocusItem() != nullptr);
        QTest::keyClick(m_window, Qt::Key_Right);
        QSignalSpy picked(strip, SIGNAL(picked(QString)));
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(picked.count(), 1);
        // Second cell of the default set after one Right from cell 0.
        QCOMPARE(picked.first().first().toString(), QStringLiteral("🔥"));
    }

    // The FILL and the BORDER resolve to the tone token; the LABEL resolves
    // to that token stepped in lightness until it clears the fill. It used to
    // be the raw token here too, and that is what made a soft `danger` chip
    // 2.02:1 on Nordic — `mentionBadge` is a badge FILL, and inking text in
    // it was the defect. A solid chip is unaffected: its fill IS the token.
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
        // Storm §3.2: label brightens to stormText, the icon inks bolt.
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

        // Open the flyout via its parent row and verify it lands beside the
        // parent menu, then Escape unwinds innermost-first.
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

        // Radio treatment — Storm §3.3 node states: the selected row
        // carries the bolt-filled node, the unselected row the dashed ring.
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

        // Clicking the row must not flip radioSelected by itself — the
        // owner's binding stays authoritative (an internal toggle would
        // destroy it, the classic destroyed-binding bug class).
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
        // 0.6.5 correction: Storm became a REAL selectable theme (id 11)
        // and the storm* namespace is theme-ROUTED, not invariant — under
        // every legacy theme (1-10) it now resolves to that theme's own
        // semantic tones, so a Deep Teal user gets Deep Teal menus again.
        // Only Storm itself (11) still renders the fixed navy/bolt literal.
        // This inverts the old invariance contract: a legacy->legacy switch
        // must RETINT the storm-skinned keycap and land on the new theme's
        // own routed stormBorderStrong, and switching TO Storm must always
        // land on the fixed Storm literal regardless of which legacy theme
        // was active immediately before.
        const QColor indigoSoft = token("tokAccentSoft");
        const QColor indigoBorderStrong = token("tokStormBorderStrong");
        auto *chip = item("keycapText");
        QVERIFY(chip);
        QCOMPARE(chip->property("border").value<QObject *>()
                     ->property("color").value<QColor>(),
                 indigoBorderStrong);

        m_root->setProperty("themeMode", 10); // Deep Teal — still legacy.
        QTRY_VERIFY(token("tokAccentSoft") != indigoSoft);
        const QColor tealBorderStrong = token("tokStormBorderStrong");
        QVERIFY2(tealBorderStrong != indigoBorderStrong,
                 "legacy->legacy theme switch must retint the routed "
                 "storm border, not stay invariant");
        QCOMPARE(chip->property("color").value<QColor>().alpha(), 0);
        QCOMPARE(chip->property("border").value<QObject *>()
                     ->property("color").value<QColor>(),
                 tealBorderStrong);

        // Storm (11) always resolves to ONE fixed value, regardless of which
        // legacy theme was active immediately beforehand — that invariance is
        // what this case is about, not the particular hex.
        //
        // It used to assert a copied literal, and the 2026-08-21 rounds moved
        // that literal twice (the ladder widening, then the re-saturation),
        // breaking a test whose subject had not changed either time. It now
        // reads the TOKEN and checks the property against it, then leaves and
        // returns to prove the value is stable across a round trip. A copied
        // hex here only ever tested that nobody had touched the palette.
        m_root->setProperty("themeMode", 11);
        QTRY_COMPARE(chip->property("border").value<QObject *>()
                         ->property("color").value<QColor>(),
                     token("tokStormBorderStrong"));
        const QColor stormBorderStrong = token("tokStormBorderStrong");
        QVERIFY2(stormBorderStrong != tealBorderStrong
                     && stormBorderStrong != indigoBorderStrong,
                 "Storm must resolve to its OWN border, not a legacy theme's");

        // Round trip: away to a legacy theme and back. Storm must land on the
        // same value both times.
        m_root->setProperty("themeMode", 10);
        QTRY_COMPARE(token("tokStormBorderStrong"), tealBorderStrong);
        m_root->setProperty("themeMode", 11);
        QTRY_COMPARE(token("tokStormBorderStrong"), stormBorderStrong);
        QCOMPARE(chip->property("border").value<QObject *>()
                     ->property("color").value<QColor>(),
                 stormBorderStrong);

        m_root->setProperty("themeMode", 9);
    }

    // ── F2: THE MENU WIDTH PROMISE ───────────────────────────────────────
    //
    // AppMenu has always carried a comment saying its design width is "a
    // floor, not a clamp… menus widen to fit rather than eliding at a fixed
    // pin". Until 2026-09-20 that was false and had never been true: the
    // binding under it read `implicitContentWidth`, and a Menu's contentItem
    // is the Basic style's ListView, which declares implicitHeight and no
    // implicitWidth — so the expression was `Math.max(menuWidth, 12)`.
    // On screen, "Mentions & keywords" rendered as "Mentions & …" in the
    // 150 px notifications flyout.
    //
    // Asserted as the USER'S condition, not as a width: the row must not be
    // truncated. A width assertion would pass on a menu that widened by two
    // pixels and still elided.
    void theFlyoutWidensToItsWidestRowInsteadOfEliding()
    {
        openMenu();
        auto *flyout = m_root->findChild<QObject *>(QStringLiteral("fitFlyout"));
        QVERIFY(flyout);
        // Unopened, it is still exactly its design width: the fit is a
        // measurement of rows, and a closed popup has no measurable rows
        // (a Layout skips items that are not effectively visible, so every
        // row of a closed menu reports its padding alone).
        QCOMPARE(flyout->property("width").toInt(),
                 flyout->property("menuWidth").toInt());

        openViaParentRow(m_root->findChild<QObject *>(QStringLiteral("menu")),
                         flyout, m_window);

        auto *longRow = item("fitLongRow");
        QVERIFY(longRow);
        auto *longLabel = label(longRow);
        QVERIFY(longLabel);
        QTRY_VERIFY(longLabel->width() > 0);
        // QTRY, not QVERIFY: the menu resizes at `opened` and QQuickMenu
        // hands the new width down to its rows on the next polish, so the
        // row is one pass behind the panel. Both happen before a frame is
        // drawn; a test that reads the row in the same instruction does
        // not.
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

        // And the short row is NOT what decided the width — the widest row
        // is, which is the whole contract.
        auto *shortRow = item("fitShortRow");
        QVERIFY(shortRow);
        QVERIFY(longRow->implicitWidth() > shortRow->implicitWidth());

        QMetaObject::invokeMethod(flyout, "close");
        QTRY_VERIFY(!flyout->property("opened").toBool());
        closeMenu();
    }

    // The other half of the same contract: menu rows carry REMOTE text, so
    // "widen to fit" without a stop is a menu as wide as whatever someone
    // called their room. Past the ceiling the row elides exactly as it used
    // to, which is the correct behaviour there.
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

    // A non-MenuItem child of a menu sizes ITSELF. `wrapMode` alone wraps
    // nothing — measured, the room menu's notifications disclaimer kept its
    // implicitWidth (the whole unwrapped sentence) and painted straight
    // through the panel, so it read "Local setting: it does not chang", cut
    // mid-word with no ellipsis because a wrapping Text does not elide.
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
        // It WRAPPED rather than being cut: more than one line, and the
        // painted text no wider than the item carrying it. A wrapping Text
        // does not elide, so the unfixed version simply painted past the
        // panel and was scissored mid-word.
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
        // And VERTICALLY inside it: in the running app the wrapped second
        // line fell below the panel's own edge, which is the same defect
        // one axis over.
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

    // The same two properties on the REAL RoomActionsMenu, because the two
    // cases above prove AppMenu's contract and not that the application's
    // own flyout honours it. The component is the compiled production file;
    // only the four data points it reads off `app` are stood in for.
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
        // Opened directly: a click into this second window does not reach
        // it under the offscreen platform. That is fine for the row below,
        // which is a property of the MENU's width; the disclaimer's own
        // proof is the source contract in ContextMenuContractTest plus the
        // captures named in this round's notes.
        QMetaObject::invokeMethod(flyout, "open");
        QTRY_VERIFY(flyout->property("opened").toBool());

        // The reported row.
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

        // The reported disclaimer.
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

    // ── F5: A SOFT CHIP'S INK MUST CLEAR THE CHIP ────────────────────────
    //
    // The soft fill is 14% of the tone's own colour over the parent, so it
    // lifts the background TOWARDS the ink and the label measures WORSE on
    // its own pill than on the surface behind it. `neutral` starts from the
    // muted text ink, the dimmest there is, and on the unfixed tree it
    // failed 4.5:1 AA on eight of eleven palettes over `stormPanel` — the
    // surface a SettingsCard has painted since fea70c63.
    //
    // Eleven palettes are DEMANDED to be distinct, not counted: a loop that
    // writes `settings.theme` without `AppTheme.mode` reaching the singleton
    // measures one palette eleven times and passes (the 2026-09-19 lesson).
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

    // ── F5b: AND IT WAS NEVER ONLY THE NEUTRAL TONE ──────────────────────
    //
    // Same defect, the rest of the family, measured 2026-09-20 on the three
    // surfaces a chip is really dropped on. Storm vocabulary, worst per tone:
    // accent 3.72 (Moss Light on stormCanvas), success 4.27, warning 4.22,
    // danger 4.20, info 4.33 (all Nordic on stormPanel). The LEGACY
    // vocabulary is far worse and it SHIPS — RoomInfoPanel paints a soft
    // `danger` chip ("Banned") in the member list with no `storm: true`, so
    // its ink is `mentionBadge`, a BADGE FILL used as text ink, and it fails
    // on ALL ELEVEN palettes, worst Nordic 2.02:1. Legacy `accent` fails on
    // nine.
    //
    // After: the floor across all three grounds and all eleven palettes is
    // 4.52 (Warm, storm accent on stormCanvas). `legibleChoice` stops the
    // moment it clears 4.5, so the tight cells are by construction.
    //
    // The three assertions are deliberately different questions: the label
    // clears its own fill (the defect), the FILL and BORDER still carry the
    // raw tone at the alpha the token declares (the chip still reads as its
    // own family — the label is the only thing that moved), and the ink is
    // still in that family, hue and HSL saturation, rather than having
    // wandered off to a neutral. Measured drift of the derivation over every
    // tone and palette: 0.79 degrees of hue and zero saturation.
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

                // The pill still IS its tone: only the label moved.
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

                // And so does the ink: same hue, same HSL saturation, a
                // different lightness. A jump to a neutral text ink would
                // clear AA and lose the tone, which is not the fix.
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
        // Eleven palettes DEMANDED to be distinct, not counted (see the
        // neutral case above), and the number of chips actually measured
        // asserted rather than the number of loop iterations.
        QCOMPARE(distinctPanels.size(), 11);
        // Five tones times eleven palettes, every one of them a different
        // fill: the palettes really moved AND the tones are really five
        // colours rather than one repeated. A loop that read one palette
        // eleven times would return 5 here.
        QCOMPARE(distinctFills.size(), 55);
        QCOMPARE(measured, 11 * int(std::size(probes)));
        m_root->setProperty("themeMode", 9);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    MenuSystemQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MenuSystemQmlTest.moc"
