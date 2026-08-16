// Rendered proof for the shared Lightning control system (the runtime
// screenshots showed Fusion bevels/gradients leaking through Room
// Information, Settings, and the GIF picker). Renders the production
// AppButton kinds, SegmentedControl, AppTextField, and AppComboBox (popup
// open) offscreen and asserts sampled pixels: flat fills (no vertical
// gradient), token-driven colors per state, themed popup surface — never a
// native look. Expected colors are read back from token probes in the same
// scene, so every theme keeps the contract.

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
    height: 420
    visible: true
    color: AppTheme.background

    property int themeMode: 9
    Binding { target: AppTheme; property: "mode"; value: win.themeMode }

    Rectangle { objectName: "tokBackground"; visible: false; color: AppTheme.background }
    Rectangle { objectName: "tokAccent"; visible: false; color: AppTheme.accent }
    Rectangle { objectName: "tokAccentSoft"; visible: false; color: AppTheme.accentSoft }
    Rectangle { objectName: "tokAccentText"; visible: false; color: AppTheme.accentText }
    Rectangle { objectName: "tokSelectedText"; visible: false; color: AppTheme.selectedText }
    Rectangle { objectName: "tokBolt"; visible: false; color: AppTheme.bolt }
    Rectangle { objectName: "tokBoltInk"; visible: false; color: AppTheme.boltInk }
    Rectangle { objectName: "tokSurface"; visible: false; color: AppTheme.surface }
    Rectangle { objectName: "tokStormPanel"; visible: false; color: AppTheme.stormPanel }
    Rectangle { objectName: "tokInputBg"; visible: false; color: AppTheme.inputBackground }
    Rectangle { objectName: "tokCardElevated"; visible: false; color: AppTheme.cardElevated }

    Column {
        x: 24
        y: 24
        spacing: 18

        Row {
            spacing: 18
            AppButton { objectName: "secondaryButton"; text: "Copy room ID" }
            AppButton { objectName: "primaryButton"; kind: "primary"; text: "Save" }
            AppButton { objectName: "dangerButton"; kind: "danger"; text: "Remove" }
            AppButton {
                objectName: "disabledPrimary"
                kind: "primary"
                text: "Save"
                enabled: false
            }
        }

        SegmentedControl {
            objectName: "segments"
            model: [
                { label: "Overview", value: "overview" },
                { label: "People", value: "people" },
                { label: "Media", value: "media" },
            ]
            current: "people"
        }

        AppTextField {
            objectName: "field"
            width: 260
            placeholderText: "Room name"
        }

        AppComboBox {
            objectName: "combo"
            width: 260
            model: ["All messages", "Mentions only", "Mute"]
        }
    }
}
)QML";

} // namespace

class ControlSystemTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        if (parent->objectName() == name)
            return parent;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        }
        return nullptr;
    }

    QQuickItem *item(const char *name) const
    {
        if (auto *hit = m_root->findChild<QQuickItem *>(QLatin1String(name)))
            return hit;
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    QColor token(const char *name) const
    {
        auto *it = m_root->findChild<QQuickItem *>(QLatin1String(name));
        return it ? it->property("color").value<QColor>() : QColor();
    }

    // Sample near the top and bottom of an item's interior: a flat fill has
    // identical rows; a Fusion-style gradient/bevel does not.
    void assertFlatFill(const QImage &img, QQuickItem *it, const QColor &expected,
                        const char *what)
    {
        const QPointF topLeft = it->mapToScene(QPointF(8, 5));
        const QPointF bottomLeft =
            it->mapToScene(QPointF(8, it->height() - 6));
        const QColor top = sampleAvg(
            img, QRect(int(topLeft.x()), int(topLeft.y()), 2, 2));
        const QColor bottom = sampleAvg(
            img, QRect(int(bottomLeft.x()), int(bottomLeft.y()), 2, 2));
        QVERIFY2(channelDelta(top, bottom) <= 2,
                 qPrintable(QStringLiteral("%1 has a vertical gradient: "
                                           "%2 vs %3")
                                .arg(QLatin1String(what))
                                .arg(top.name(), bottom.name())));
        QVERIFY2(channelDelta(top, expected) <= kTolerance,
                 qPrintable(QStringLiteral("%1: %2 vs expected %3")
                                .arg(QLatin1String(what))
                                .arg(top.name(), expected.name())));
    }

private slots:
    void initTestCase()
    {
        QQmlComponent component(&m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("controlscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QCoreApplication::processEvents();
    }

    void cleanupTestCase()
    {
        delete m_root;
    }

    void primaryIsFlatAccentAndSecondaryIsFlatSurface()
    {
        const QImage img = m_window->grabWindow();
        QVERIFY(!img.isNull());
        auto *primary = item("primaryButton");
        QVERIFY(primary);
        assertFlatFill(img, primary, token("tokAccent"), "primary");
        auto *secondary = item("secondaryButton");
        QVERIFY(secondary);
        // Secondary rests on the window background with only its 1px border.
        assertFlatFill(img, secondary, token("tokBackground"), "secondary");
        QCOMPARE(secondary->property("kind").toString(),
                 QStringLiteral("secondary"));
    }

    void dangerAndDisabledStayFlatAndDistinct()
    {
        const QImage img = m_window->grabWindow();
        auto *danger = item("dangerButton");
        auto *disabled = item("disabledPrimary");
        QVERIFY(danger && disabled);
        assertFlatFill(img, danger, token("tokBackground"), "danger-rest");
        assertFlatFill(img, disabled, token("tokCardElevated"), "disabled");
        // Danger label is the danger tone, not the primary/secondary text.
        auto *label = danger->property("contentItem").value<QQuickItem *>();
        QVERIFY(label);
        const QColor dangerText = label->property("color").value<QColor>();
        auto *saveLabel =
            item("primaryButton")->property("contentItem").value<QQuickItem *>();
        QVERIFY(saveLabel);
        QVERIFY(dangerText != saveLabel->property("color").value<QColor>());
    }

    void segmentedSelectionIsAccentSoftChipNotOutlinedRectangles()
    {
        const QImage img = m_window->grabWindow();
        auto *selected = item("segments_people");
        auto *unselected = item("segments_overview");
        QVERIFY(selected && unselected);
        assertFlatFill(img, selected, token("tokAccentSoft"),
                       "selected-segment");
        assertFlatFill(img, unselected, token("tokBackground"),
                       "unselected-segment");
    }

    void textFieldIsFlatThemedInput()
    {
        const QImage img = m_window->grabWindow();
        auto *field = item("field");
        QVERIFY(field);
        assertFlatFill(img, field, token("tokInputBg"), "text-field");
    }

    void comboFieldAndPopupFollowTheme()
    {
        const QImage img = m_window->grabWindow();
        auto *combo = item("combo");
        QVERIFY(combo);
        assertFlatFill(img, combo, token("tokInputBg"), "combo-field");

        // Open the popup: its surface and delegates are themed, flat, and
        // carry the accent-soft selected row.
        auto *popupObject = combo->property("popup").value<QObject *>();
        QVERIFY(popupObject);
        QMetaObject::invokeMethod(popupObject, "open");
        QTRY_VERIFY(popupObject->property("visible").toBool());
        QCoreApplication::processEvents();
        const QImage open = m_window->grabWindow();
        auto *popupItem =
            popupObject->property("contentItem").value<QQuickItem *>();
        QVERIFY(popupItem);
        const QPointF inPopup = popupItem->mapToScene(QPointF(
            popupItem->width() - 10, popupItem->height() / 2));
        const QColor sampled = sampleAvg(
            open, QRect(int(inPopup.x()), int(inPopup.y()), 2, 2));
        // Storm §5: dropdown popups are part of the menu system and always
        // render stormPanel. Since the 0.6.5 routing correction stormPanel
        // itself is theme-ROUTED (Storm literal under theme 11, the user's
        // own surface tone under every legacy theme) rather than invariant
        // — this assertion stays self-consistent because it compares the
        // live-sampled popup pixel against the SAME routed token, not a
        // hardcoded literal, so it holds under any active theme.
        QVERIFY2(channelDelta(sampled, token("tokStormPanel")) <= kTolerance,
                 qPrintable(QStringLiteral("popup surface %1 vs %2")
                                .arg(sampled.name(),
                                     token("tokStormPanel").name())));
        QMetaObject::invokeMethod(popupObject, "close");
    }

    void themeSwitchRetintsEveryKind()
    {
        m_root->setProperty("themeMode", 8); // Moss Light
        QCoreApplication::processEvents();
        const QImage img = m_window->grabWindow();
        auto *primary = item("primaryButton");
        QVERIFY(primary);
        assertFlatFill(img, primary, token("tokAccent"), "primary-moss");
        auto *selected = item("segments_people");
        QVERIFY(selected);
        assertFlatFill(img, selected, token("tokAccentSoft"),
                       "selected-segment-moss");
        m_root->setProperty("themeMode", 9);
        QCoreApplication::processEvents();
    }

    // 2026-08-15 report: under the Storm theme the themed selected chip was
    // accentSoft (14% translucent bolt = khaki) carrying accentText
    // (canvas-dark ink, meant for a SOLID bolt fill) — dark-on-dark. The
    // selected segment must be the solid bolt "current selection" moment
    // with boltInk on it. This fails on the pre-fix SegmentedControl.
    void stormRoutesSelectedSegmentToSolidBolt()
    {
        m_root->setProperty("themeMode", 11); // Storm
        QCoreApplication::processEvents();
        const QImage img = m_window->grabWindow();
        auto *selected = item("segments_people");
        QVERIFY(selected);
        assertFlatFill(img, selected, token("tokBolt"),
                       "selected-segment-storm");
        auto *label =
            selected->property("contentItem").value<QQuickItem *>();
        QVERIFY(label);
        QCOMPARE(label->property("color").value<QColor>(),
                 token("tokBoltInk"));
        m_root->setProperty("themeMode", 9);
        QCoreApplication::processEvents();
    }

    // 2026-08-16 report (Deep Teal): the same defect on the OTHER side of the
    // Storm routing. The selected chip's fill is accentSoft -- a TINT -- and
    // it carried accentText, which is the ink for a SOLID accent fill and is
    // white in every theme that does not override it. Measured contrast was
    // 1.00 on Deep Teal (#062A25 on #112928), 1.14 on Moss Light and 1.42 on
    // Lightning Light: the label was simply not there. Only Deep Teal was
    // reported because that is the theme in use; the light themes were just
    // as broken.
    //
    // The ink must be selectedText -- the ink meant for a selected row/chip --
    // in every non-Storm theme. ThemeTokensTest pins the contrast numbers;
    // this pins that the control actually asks for that token. Fails on the
    // pre-fix SegmentedControl, which returned accentText here.
    void designThemesKeepTheSelectedSegmentLabelReadable()
    {
        // Deep Teal (10), Moss Light (8) and Lightning Light (1) are the
        // three that measured below AA before the fix.
        for (int mode : { 10, 8, 1 }) {
            m_root->setProperty("themeMode", mode);
            QCoreApplication::processEvents();

            auto *selected = item("segments_people");
            QVERIFY(selected);
            auto *label =
                selected->property("contentItem").value<QQuickItem *>();
            QVERIFY(label);
            const QColor ink = label->property("color").value<QColor>();

            QCOMPARE(ink, token("tokSelectedText"));
            // And explicitly NOT the solid-fill ink that made it invisible.
            QVERIFY2(ink != token("tokAccentText"),
                     qPrintable(QStringLiteral("theme %1 still inks the selected "
                                               "segment with accentText")
                                    .arg(mode)));
        }
        m_root->setProperty("themeMode", 9);
        QCoreApplication::processEvents();
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    ControlSystemTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ControlSystemTest.moc"
