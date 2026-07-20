// Structure and behavior proof for the rebuilt main composer (SPEC §2):
// ONE card holding a formatting toolbar row above the input row with a 1px
// divider between them, the exact control order, spec geometry for the send
// and toolbar buttons, a borderless transparent input, a working markdown
// formatting round-trip, and card pixels that track each design theme's
// raised-surface token. Loads the production MessageComposerBar against the
// real AppController mock backend with a zero-QML-warning contract.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>

#include "app/AppController.h"

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
    width: 900
    height: 420
    visible: true
    color: AppTheme.background

    property int themeMode: 9
    Binding { target: AppTheme; property: "mode"; value: win.themeMode }

    Rectangle { objectName: "tokSurface"; visible: false; color: AppTheme.surface }
    Rectangle { objectName: "tokBackground"; visible: false; color: AppTheme.background }
    Rectangle { objectName: "tokBorder"; visible: false; color: AppTheme.border }

    MessageComposerBar {
        objectName: "composerBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
)QML";

} // namespace

class ComposerQmlTest : public QObject
{
    Q_OBJECT

private:
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QStringList m_warnings;

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

    // Repeater-created delegates live only in the visual item tree (no
    // QObject parent chain), so findChild alone cannot see them.
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

private slots:
    void initTestCase()
    {
        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlEngine(this);
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("composerscene.qml")));
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
        delete m_controller;
    }

    void loadsWithoutQmlWarnings()
    {
        QCOMPARE(m_warnings, QStringList{});
    }

    void oneCardWithToolbarAboveDividerAboveInput()
    {
        // The toolbar is collapsible; open it (and let the layout polish)
        // before asserting its geometry.
        item("composerBar")->setProperty("toolbarExpanded", true);
        QTest::qWait(50);
        auto *card = item("composerCard");
        auto *toolbar = item("composerToolbarRow");
        auto *divider = item("composerRowDivider");
        auto *inputRow = item("composerInputRow");
        QVERIFY(card && toolbar && divider && inputRow);
        const qreal toolbarY = toolbar->mapToScene(QPointF(0, 0)).y();
        const qreal dividerY = divider->mapToScene(QPointF(0, 0)).y();
        const qreal inputY = inputRow->mapToScene(QPointF(0, 0)).y();
        QVERIFY(toolbarY < dividerY);
        QVERIFY(dividerY < inputY);
        QCOMPARE(divider->height(), 1.0);
        // The divider spans the card.
        QVERIFY(divider->width() >= card->width() - 4);
        // Both rows live inside the same card.
        QVERIFY(toolbar->mapToScene(QPointF(0, 0)).y()
                >= card->mapToScene(QPointF(0, 0)).y());
        QVERIFY(inputRow->mapToScene(QPointF(0, inputRow->height())).y()
                <= card->mapToScene(QPointF(0, card->height())).y() + 1);
        // Card geometry per spec: 12px radius, 1px border, 20px side
        // padding inside the timeline area.
        QCOMPARE(card->property("radius").toInt(), 12);
        QCOMPARE(QQmlProperty::read(card, QStringLiteral("border.width")).toInt(), 1);
        QCOMPARE(card->mapToScene(QPointF(0, 0)).x(), 20.0);
    }

    void toolbarHasExactControlOrder()
    {
        item("composerBar")->setProperty("toolbarExpanded", true);
        QTest::qWait(50);
        const char *order[] = {
            "composerFormat_bold", "composerFormat_italic",
            "composerFormat_strike", "composerFormat_code",
            "composerToolbarDivider",
            "composerFormat_link", "composerFormat_list",
            "composerFormat_quote",
        };
        qreal lastX = -1;
        for (const char *name : order) {
            auto *it = item(name);
            QVERIFY2(it, name);
            const qreal x = it->mapToScene(QPointF(0, 0)).x();
            QVERIFY2(x > lastX, name);
            lastX = x;
        }
        auto *divider = item("composerToolbarDivider");
        QCOMPARE(divider->width(), 1.0);
        QCOMPARE(divider->height(), 16.0);
        auto *bold = item("composerFormat_bold");
        QCOMPARE(bold->width(), 28.0);
        QCOMPARE(bold->height(), 28.0);
        QCOMPARE(bold->property("radius").toInt(), 6);
        QCOMPARE(bold->property("iconSize").toInt(), 18);
    }

    void inputRowHasExactControlOrder()
    {
        const char *order[] = {
            "composerAttachButton", "composerFormatToggleButton",
            "composerInput", "composerEmojiButton",
            "composerGifButton", "composerMicButton", "composerSendButton",
        };
        qreal lastX = -1;
        for (const char *name : order) {
            auto *it = item(name);
            QVERIFY2(it, name);
            const qreal x = it->mapToScene(QPointF(0, 0)).x();
            QVERIFY2(x > lastX, name);
            lastX = x;
        }
    }

    void formatToggleCollapsesAndExpandsToolbar()
    {
        auto *bar = item("composerBar");
        auto *toggle = item("composerFormatToggleButton");
        auto *toolbar = item("composerToolbarRow");
        QVERIFY(bar && toggle && toolbar);

        // Collapsed compact composer by default: the toolbar takes no space.
        bar->setProperty("toolbarExpanded", false);
        QTest::qWait(30);
        QVERIFY(!toolbar->isVisible());

        // Activating the toggle raises the toolbar above the input row.
        QMetaObject::invokeMethod(toggle, "click");
        QTest::qWait(50);
        QVERIFY(bar->property("toolbarExpanded").toBool());
        QVERIFY(toolbar->isVisible());
        QVERIFY(toggle->property("active").toBool());
        auto *inputRow = item("composerInputRow");
        QVERIFY(toolbar->mapToScene(QPointF(0, 0)).y()
                < inputRow->mapToScene(QPointF(0, 0)).y());

        // Toggling again returns the compact composer.
        QMetaObject::invokeMethod(toggle, "click");
        QTest::qWait(30);
        QVERIFY(!bar->property("toolbarExpanded").toBool());
        QVERIFY(!toolbar->isVisible());
    }

    void sendButtonIsAccentFillRoundedSquare()
    {
        auto *send = item("composerSendButton");
        QVERIFY(send);
        QCOMPARE(send->width(), 34.0);
        QCOMPARE(send->height(), 34.0);
        QCOMPARE(send->property("radius").toInt(), 9);
        QCOMPARE(send->property("fill").toBool(), true);
        // Empty composer: send is disabled but never a native outlined button.
        QCOMPARE(send->property("enabled").toBool(), false);
    }

    void inputIsBorderlessAndTransparent()
    {
        auto *input = item("composerInput");
        QVERIFY(input);
        auto *background =
            input->property("background").value<QQuickItem *>();
        QVERIFY(background);
        QCOMPARE(background->property("color").value<QColor>().alpha(), 0);
        QCOMPARE(input->property("placeholderText").toString(),
                 QStringLiteral("Select a room to start typing"));
    }

    void gifKeycapIsBorderedMonoChip()
    {
        auto *keycap = item("composerGifKeycap");
        QVERIFY(keycap);
        QCOMPARE(QQmlProperty::read(keycap, QStringLiteral("border.width"))
                     .toReal(), 1.5);
        QCOMPARE(keycap->property("radius").toInt(), 5);
    }

    void micIsHonestlyUnavailable()
    {
        auto *mic = item("composerMicButton");
        QVERIFY(mic);
        QCOMPARE(mic->property("enabled").toBool(), false);
    }

    void formattingToolbarRoundTripsMarkdown()
    {
        auto *bar = item("composerBar");
        auto *input = item("composerInput");
        QVERIFY(bar && input);
        input->setProperty("text", QStringLiteral("hello"));
        QMetaObject::invokeMethod(input, "select", Q_ARG(int, 0), Q_ARG(int, 5));
        QMetaObject::invokeMethod(bar, "applyFormat",
                                  Q_ARG(QVariant, QStringLiteral("bold")));
        QCOMPARE(input->property("text").toString(), QStringLiteral("**hello**"));
        QCOMPARE(m_controller->composer()->text(), QStringLiteral("**hello**"));
        const QVariantMap flags = bar->property("formatFlags").toMap();
        QVERIFY(flags.value(QStringLiteral("bold")).toBool());
        // Toggle off restores the plain text and clears the chip state.
        QMetaObject::invokeMethod(bar, "applyFormat",
                                  Q_ARG(QVariant, QStringLiteral("bold")));
        QCOMPARE(input->property("text").toString(), QStringLiteral("hello"));
        QVERIFY(!bar->property("formatFlags").toMap()
                     .value(QStringLiteral("bold")).toBool());
        input->setProperty("text", QString());
    }

    void cardTracksThemeRaisedSurface()
    {
        auto *card = item("composerCard");
        auto *toolbar = item("composerToolbarRow");
        QVERIFY(card && toolbar);
        // Open the collapsible toolbar so its raised surface is on screen.
        item("composerBar")->setProperty("toolbarExpanded", true);
        QTest::qWait(50);
        const int themes[] = { 9, 8, 10 }; // Indigo Night, Moss Light, Deep Teal
        for (int mode : themes) {
            m_root->setProperty("themeMode", mode);
            QCoreApplication::processEvents();
            const QImage img = m_window->grabWindow();
            QVERIFY(!img.isNull());
            // Sample the toolbar row's empty right side — card surface,
            // clear of glyphs and buttons.
            const QPointF p = card->mapToScene(
                QPointF(card->width() - 24,
                        toolbar->mapToItem(card, QPointF(0, 0)).y()
                            + toolbar->height() / 2));
            const QColor sampled = sampleAvg(
                img, QRect(int(p.x()), int(p.y()) - 1, 3, 3));
            QVERIFY2(channelDelta(sampled, token("tokSurface")) <= kTolerance,
                     qPrintable(QStringLiteral("theme %1: %2 vs %3")
                                    .arg(mode)
                                    .arg(sampled.name(),
                                         token("tokSurface").name())));
        }
        m_root->setProperty("themeMode", 9);
        QCoreApplication::processEvents();
        QCOMPARE(m_warnings, QStringList{});
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    ComposerQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ComposerQmlTest.moc"
