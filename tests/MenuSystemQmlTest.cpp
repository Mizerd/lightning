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

int channelDelta(const QColor &a, const QColor &b)
{
    return qMax(qMax(qAbs(a.red() - b.red()), qAbs(a.green() - b.green())),
                qAbs(a.blue() - b.blue()));
}

constexpr int kTolerance = 8;

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 640
    height: 480
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

    void statusChipTonesResolveToTokens()
    {
        auto *verified = item("chipVerified");
        QVERIFY(verified);
        QCOMPARE(rowChild(verified, "chipLabel")
                     ->property("color").value<QColor>(),
                 token("tokPresenceOnline"));
        QVERIFY(rowChild(verified, "chipIcon")->property("visible").toBool());
        auto *loud = item("chipLoud");
        QVERIFY(loud);
        QCOMPARE(rowChild(loud, "chipLabel")
                     ->property("color").value<QColor>(),
                 token("tokMentionBadge"));
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
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    MenuSystemQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MenuSystemQmlTest.moc"
