// Offscreen rendering proof for the design button system (SPEC §1):
// Style A bare icon buttons carry no background or border at rest and gain
// only the soft theme tint on hover; Style B is the accent-soft chip; Style C
// is the accent fill reserved for primary actions. Keyboard focus draws the
// shared 2px accent ring. Pixels are sampled from a grabbed offscreen window
// and compared against the theme tokens read back from the same scene —
// never hard-coded hex — so every theme keeps the same contract.

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
    width: 420
    height: 200
    visible: true
    color: AppTheme.background

    // Theme switched from the test through this property.
    property int themeMode: 9
    Binding { target: AppTheme; property: "mode"; value: win.themeMode }

    // Token probes: expected colors are read back from these, so assertions
    // track the active theme rather than hard-coded values.
    Rectangle { objectName: "tokBackground"; visible: false; color: AppTheme.background }
    Rectangle { objectName: "tokHover"; visible: false; color: AppTheme.hover }
    Rectangle { objectName: "tokAccentSoft"; visible: false; color: AppTheme.accentSoft }
    Rectangle { objectName: "tokAccent"; visible: false; color: AppTheme.accent }
    Rectangle { objectName: "tokFocusRing"; visible: false; color: AppTheme.focusRing }
    Rectangle { objectName: "tokIcon"; visible: false; color: AppTheme.icon }
    Rectangle { objectName: "tokOnAccent"; visible: false; color: AppTheme.accentText }
    Rectangle { objectName: "tokDisabledFill"; visible: false; color: AppTheme.cardElevated }

    Row {
        x: 24
        y: 60
        spacing: 40
        IconButton { objectName: "bareButton"; iconName: "forum" }
        IconButton { objectName: "chipButton"; iconName: "group"; active: true }
        IconButton {
            objectName: "fillButton"
            iconName: "send"
            fill: true
            radius: 9
            iconSize: 19
        }
        IconButton {
            objectName: "disabledFillButton"
            iconName: "send"
            fill: true
            enabled: false
        }
        IconButton {
            objectName: "toolbarButton"
            iconName: "format_bold"
            implicitWidth: 28
            implicitHeight: 28
            radius: 6
            iconSize: 18
        }
    }
}
)QML";

} // namespace

class ButtonSystemTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    QQuickItem *item(const char *name) const
    {
        auto *it = m_root->findChild<QQuickItem *>(QLatin1String(name));
        return it;
    }

    QColor token(const char *name) const
    {
        auto *it = m_root->findChild<QQuickItem *>(QLatin1String(name));
        return it ? it->property("color").value<QColor>() : QColor();
    }

    // Interior sample left of the glyph, vertically centered — inside the
    // rounded rect, outside the icon's ink.
    QRect insideRect(QQuickItem *button) const
    {
        const QPointF p = button->mapToScene(QPointF(4, button->height() / 2));
        return QRect(int(p.x()), int(p.y()) - 1, 2, 3);
    }

private slots:
    void initTestCase()
    {
        QQmlComponent component(&m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("buttonscene.qml")));
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

    void bareButtonIsTransparentAtRest()
    {
        auto *bare = item("bareButton");
        QVERIFY(bare);
        const QImage img = m_window->grabWindow();
        QVERIFY(!img.isNull());
        // Interior and edge midpoint both show the window background: no
        // fill, no border, no native frame.
        QVERIFY(channelDelta(sampleAvg(img, insideRect(bare)),
                             token("tokBackground")) <= kTolerance);
        const QPointF edge = bare->mapToScene(QPointF(1, bare->height() / 2));
        QVERIFY(channelDelta(sampleAvg(img, QRect(int(edge.x()), int(edge.y()), 1, 1)),
                             token("tokBackground")) <= kTolerance);
        // The glyph itself uses the theme icon token.
        auto *icon = bare->property("contentItem").value<QQuickItem *>();
        QVERIFY(icon);
        QCOMPARE(icon->property("color").value<QColor>(), token("tokIcon"));
    }

    void hoverShowsSoftTintWithoutBorder()
    {
        auto *bare = item("bareButton");
        QVERIFY(bare);
        const QPointF center = bare->mapToScene(
            QPointF(bare->width() / 2, bare->height() / 2));
        QTest::mouseMove(m_window, center.toPoint());
        QTRY_VERIFY(bare->property("hovered").toBool());
        const QImage img = m_window->grabWindow();
        QVERIFY(channelDelta(sampleAvg(img, insideRect(bare)),
                             token("tokHover")) <= kTolerance);
        // Move the pointer away again so later grabs see the rest state.
        QTest::mouseMove(m_window, QPoint(5, 5));
        QTRY_VERIFY(!bare->property("hovered").toBool());
    }

    void activeChipUsesAccentSoftAndAccentIcon()
    {
        auto *chip = item("chipButton");
        QVERIFY(chip);
        const QImage img = m_window->grabWindow();
        QVERIFY(channelDelta(sampleAvg(img, insideRect(chip)),
                             token("tokAccentSoft")) <= kTolerance);
        auto *icon = chip->property("contentItem").value<QQuickItem *>();
        QVERIFY(icon);
        QCOMPARE(icon->property("color").value<QColor>(), token("tokAccent"));
    }

    void accentFillUsesAccentAndOnAccentInk()
    {
        auto *fill = item("fillButton");
        QVERIFY(fill);
        QCOMPARE(fill->width(), 34.0);
        QCOMPARE(fill->height(), 34.0);
        QCOMPARE(fill->property("radius").toInt(), 9);
        const QImage img = m_window->grabWindow();
        QVERIFY(channelDelta(sampleAvg(img, insideRect(fill)),
                             token("tokAccent")) <= kTolerance);
        auto *icon = fill->property("contentItem").value<QQuickItem *>();
        QVERIFY(icon);
        QCOMPARE(icon->property("color").value<QColor>(), token("tokOnAccent"));
        // Rounded square, not a circle: the corner region outside a 9px
        // radius still shows the window background at (1,1), while a circle
        // of diameter 34 would leave (8,8) uncovered too — which must be
        // filled here.
        const QPointF corner = fill->mapToScene(QPointF(1, 1));
        QVERIFY(channelDelta(sampleAvg(img, QRect(int(corner.x()), int(corner.y()), 1, 1)),
                             token("tokBackground")) <= kTolerance);
        const QPointF ring = fill->mapToScene(QPointF(8, 8));
        QVERIFY(channelDelta(sampleAvg(img, QRect(int(ring.x()), int(ring.y()), 1, 1)),
                             token("tokAccent")) <= kTolerance);
    }

    void disabledFillIsNeutralNotOutlined()
    {
        auto *button = item("disabledFillButton");
        QVERIFY(button);
        const QImage img = m_window->grabWindow();
        QVERIFY(channelDelta(sampleAvg(img, insideRect(button)),
                             token("tokDisabledFill")) <= kTolerance);
    }

    void toolbarVariantKeepsSpecGeometry()
    {
        auto *button = item("toolbarButton");
        QVERIFY(button);
        QCOMPARE(button->width(), 28.0);
        QCOMPARE(button->height(), 28.0);
        QCOMPARE(button->property("radius").toInt(), 6);
        QCOMPARE(button->property("iconSize").toInt(), 18);
    }

    void keyboardFocusDrawsAccentRing()
    {
        auto *bare = item("bareButton");
        QVERIFY(bare);
        QMetaObject::invokeMethod(bare, "forceActiveFocus",
                                  Q_ARG(Qt::FocusReason, Qt::TabFocusReason));
        QTRY_VERIFY(bare->property("visualFocus").toBool());
        const QImage img = m_window->grabWindow();
        // Ring stroke sits 2px outside the button bounds.
        const QPointF ring = bare->mapToScene(QPointF(-3, bare->height() / 2));
        QVERIFY(channelDelta(sampleAvg(img, QRect(int(ring.x()), int(ring.y()) - 1, 1, 3)),
                             token("tokFocusRing")) <= kTolerance);
        // Rest visuals are unchanged by focus: interior stays background.
        QVERIFY(channelDelta(sampleAvg(img, insideRect(bare)),
                             token("tokBackground")) <= kTolerance);
        bare->setFocus(false);
    }

    void themeSwitchRetintsActiveStates()
    {
        const QColor indigoSoft = token("tokAccentSoft");
        m_root->setProperty("themeMode", 10); // Deep Teal
        QTRY_VERIFY(token("tokAccentSoft") != indigoSoft);
        auto *chip = item("chipButton");
        QVERIFY(chip);
        const QImage img = m_window->grabWindow();
        QVERIFY(channelDelta(sampleAvg(img, insideRect(chip)),
                             token("tokAccentSoft")) <= kTolerance);
        auto *icon = chip->property("contentItem").value<QQuickItem *>();
        QVERIFY(icon);
        QCOMPARE(icon->property("color").value<QColor>(), token("tokAccent"));
        m_root->setProperty("themeMode", 9);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    ButtonSystemTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ButtonSystemTest.moc"
