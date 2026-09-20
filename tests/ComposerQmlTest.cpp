// Structure and behavior proof for the rebuilt main composer (SPEC §2):
// ONE card holding a formatting toolbar row above the input row with a 1px
// divider between them, the exact control order, spec geometry for the send
// and toolbar buttons, a borderless transparent input, a working markdown
// formatting round-trip, and card pixels that track each design theme's
// raised-surface token. Loads the production MessageComposerBar against the
// real AppController mock backend with a zero-QML-warning contract.

#include <QtTest/QtTest>

#include <QBuffer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QQuickStyle>
#include <QInputMethodEvent>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"

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

// A real, decodable 2x2 PNG: the chip's Image actually loads this one, so
// the preview tile is exercised rather than the broken-image fallback.
QByteArray tinyPng()
{
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QString writeFile(const QTemporaryDir &dir, const QString &name,
                  const QByteArray &content)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    file.close();
    return path;
}

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

    // Every match, not the first: a Repeater's delegates all carry the same
    // objectName and the defect under test is a DISAGREEMENT between them.
    static void collectItems(QQuickItem *parent, const QString &name,
                             QList<QQuickItem *> &out)
    {
        if (!parent)
            return;
        if (parent->objectName() == name)
            out.append(parent);
        const auto children = parent->childItems();
        for (QQuickItem *child : children)
            collectItems(child, name, out);
    }

    // Repeater-created delegates live only in the visual item tree (no
    // QObject parent chain), so findChild alone cannot see them.
    // Read an int straight off the AppTheme singleton, so a control-system
    // assertion pins the TOKEN rather than a copy of its current value —
    // a literal here silently stops tracking the design system.
    int themeInt(const char *token) const
    {
        // Evaluated in the SCENE's context, not the bare root context: the
        // AppTheme singleton comes from the MatrixClient import, which only
        // the loaded component's context carries.
        QQmlExpression expr(qmlContext(m_root), m_root,
                            QStringLiteral("AppTheme.") + QLatin1String(token));
        const QVariant v = expr.evaluate();
        if (expr.hasError())
            qWarning("themeInt(%s): %s", token,
                     qPrintable(expr.error().toString()));
        return v.toInt();
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

    // The mention popup is a Popup (QObject, not a QQuickItem), so it is found
    // by QObject name, not in the visual item tree.
    QObject *mentionPopup() const
    {
        return m_root->findChild<QObject *>(QStringLiteral("mentionPopup"));
    }
    bool popupVisible() const
    {
        QObject *p = mentionPopup();
        return p && p->property("visible").toBool();
    }
    // Open the mention popup by focusing the input and typing an '@' query.
    void openMention(QQuickItem *input, const QString &query)
    {
        QMetaObject::invokeMethod(input, "forceActiveFocus");
        input->setProperty("text", QLatin1Char('@') + query);
        input->setProperty("cursorPosition", query.length() + 1);
        QTest::qWait(60); // let the async member snapshot arrive + rebuild
    }

    // ── Tooltip probes ───────────────────────────────────────────────────
    //
    // A tooltip is a Popup, not an item in the visual tree, and there is ONE
    // instance shared by every control: `ToolTip.toolTip` returns the same
    // object whoever asks, and its x/y are expressed in the coordinates of
    // whichever control it is currently shown FOR. So every read has to go
    // through the control, and the rectangle is only meaningful while that
    // control is the one showing it.
    QObject *tipFor(QQuickItem *host) const
    {
        QQmlExpression expr(qmlContext(host), host,
                            QStringLiteral("ToolTip.toolTip"));
        const QVariant v = expr.evaluate();
        if (expr.hasError())
            qWarning("tipFor: %s", qPrintable(expr.error().toString()));
        return v.value<QObject *>();
    }
    bool tipVisible(QQuickItem *host) const
    {
        QQmlExpression expr(qmlContext(host), host,
                            QStringLiteral("ToolTip.visible"));
        const QVariant v = expr.evaluate();
        if (expr.hasError())
            qWarning("tipVisible: %s", qPrintable(expr.error().toString()));
        return v.toBool();
    }
    QRectF tipSceneRect(QQuickItem *host) const
    {
        QObject *tip = tipFor(host);
        if (!tip || !host)
            return {};
        const QPointF origin = host->mapToScene(
            QPointF(tip->property("x").toReal(), tip->property("y").toReal()));
        return QRectF(origin, QSizeF(tip->property("width").toReal(),
                                     tip->property("height").toReal()));
    }
    static QRectF sceneRect(QQuickItem *it)
    {
        if (!it)
            return {};
        return QRectF(it->mapToScene(QPointF(0, 0)),
                      QSizeF(it->width(), it->height()));
    }
    // A Popup's x/y are in its `parent` item's coordinates.
    QRectF popupSceneRect(QObject *popup) const
    {
        auto *host = qobject_cast<QQuickItem *>(popup->property("parent")
                                                    .value<QObject *>());
        if (!host)
            return {};
        const QPointF origin = host->mapToScene(
            QPointF(popup->property("x").toReal(),
                    popup->property("y").toReal()));
        return QRectF(origin, QSizeF(popup->property("width").toReal(),
                                     popup->property("height").toReal()));
    }
    void hoverOver(QQuickItem *it)
    {
        const QPointF centre = it->mapToScene(
            QPointF(it->width() / 2.0, it->height() / 2.0));
        QTest::mouseMove(m_window, centre.toPoint());
    }
    void hoverAway()
    {
        QTest::mouseMove(m_window, QPoint(2, 2));
        QTest::qWait(80);
    }
    // ToolTip.delay is 500 ms everywhere in this bar; 900 clears it with
    // room for the layout polish a state change schedules.
    bool hoverAndWaitForTip(QQuickItem *it, int ms = 900)
    {
        hoverOver(it);
        QTest::qWait(ms);
        return tipVisible(it);
    }
    static QString r2s(const QRectF &r)
    {
        return QStringLiteral("(%1,%2 %3x%4)")
            .arg(r.x(), 0, 'f', 1).arg(r.y(), 0, 'f', 1)
            .arg(r.width(), 0, 'f', 1).arg(r.height(), 0, 'f', 1);
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
        // radiusControl (7), not a raw 6. The 2026-08-21 audit found 66
        // IconButtons spanning 11 sizes and 7 corner radii, several of them
        // one pixel off the sibling surface they sit on — this row was one.
        // Asserted against the TOKEN so the row cannot drift from the
        // control system again by editing a literal.
        QCOMPARE(bold->property("radius").toInt(), themeInt("radiusControl"));
        QCOMPARE(bold->property("iconSize").toInt(), 18);
    }

    void inputRowHasExactControlOrder()
    {
        const char *order[] = {
            "composerAttachButton", "composerFormatToggleButton",
            "composerInput", "composerEmojiButton",
            // One button for GIFs and stickers since 2026-09-03, and the
            // "send later" clock moved out of the row into a chevron on the
            // right of Send.
            "composerMediaButton", "composerMicButton", "composerSendButton",
            "composerSendOptionsButton",
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

    // Was gifKeycapIsBorderedMonoChip, then
    // gifKeycapMatchesItsBorderlessGlyphRow. The 2026-08-21 audit found the
    // mono "GIF" keycap was the ONLY bordered chip in a row of five
    // borderless glyph buttons, at a radius nothing else used, visibly
    // shorter than its 28px siblings, and with no pressed state at all.
    //
    // 2026-09-03 finished that: GIFs and stickers became ONE button, and a
    // button covering both kinds cannot carry a word for one of them, so it
    // is a glyph like its neighbours. The chip is gone entirely; what this
    // case pins now is that the button that replaced it belongs to the row.
    void mediaButtonMatchesItsGlyphRow()
    {
        QVERIFY2(!item("composerGifKeycap"),
                 "the bordered mono GIF chip is back");
        auto *media = item("composerMediaButton");
        auto *emoji = item("composerEmojiButton");
        QVERIFY(media && emoji);
        QCOMPARE(media->width(), emoji->width());
        QCOMPARE(media->height(), emoji->height());
        QCOMPARE(media->property("radius").toInt(), themeInt("radiusControl"));
        QCOMPARE(media->property("iconSize").toInt(),
                 emoji->property("iconSize").toInt());
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

    // ── v0.7 outgoing @-mentions ─────────────────────────────────────────
    // These run after the structural tests so they may select a room. The
    // mock seeds "!general:mock.local" with Alice/Bob/Carol members.

    // The @room suggestion, in the condition it was REPORTED broken in: the
    // room-info panel open on the room you are typing in. The composer used to
    // gate @room on RoomInfoController::canNotifyRoom whenever that controller
    // pointed at the current room — and that value is false while the roster
    // loads, false after every clearSnapshot(), and false on any backend that
    // does not send the key (the mock sends no permission keys at all, which
    // is what this fixture reproduces). The panel is part of the default
    // layout, so the condition was usually true and the answer usually false:
    // @room was suppressed everywhere, and for the query "room" the popup was
    // left with nothing in it at all.
    void roomMentionSurvivesTheRoomInfoPanelPointingHere()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *input = item("composerInput");
        QObject *model =
            m_controller->property("mentionSuggestions").value<QObject *>();
        QObject *info = m_controller->property("roomInfo").value<QObject *>();
        QVERIFY(input && model && info);

        // The reported condition.
        info->setProperty("roomId", QStringLiteral("!general:mock.local"));
        QTest::qWait(80);
        QCOMPARE(info->property("roomId").toString(),
                 QStringLiteral("!general:mock.local"));
        // The value the old gate consulted really is false here — otherwise
        // this fixture would pass against the unfixed code for the wrong
        // reason.
        QVERIFY(!info->property("canNotifyRoom").toBool());

        openMention(input, QStringLiteral("room"));
        QVERIFY2(model->property("roomMentionAllowed").toBool(),
                 "@room must stay offered when nothing has said otherwise");
        QCOMPARE(model->property("count").toInt(), 1);
        QVariantMap row;
        QMetaObject::invokeMethod(model, "get", Q_RETURN_ARG(QVariantMap, row),
                                  Q_ARG(int, 0));
        QVERIFY(row.value(QStringLiteral("isRoom")).toBool());
        QCOMPARE(row.value(QStringLiteral("userId")).toString(),
                 QStringLiteral("@room"));

        // ...and it sends as a whole-room mention: the body keeps the literal
        // "@room" with no matrix.to link (there is none for "everyone here"),
        // and the id list carries the sentinel the Rust bridge turns into
        // m.mentions.room.
        QTest::keyClick(m_window, Qt::Key_Return);
        QTest::qWait(20);
        QCOMPARE(input->property("text").toString(), QStringLiteral("@room "));
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        m_controller->composer()->send();
        QTest::qWait(20);
        QCOMPARE(mock->lastMentionIdsForTest(),
                 QStringList{ QStringLiteral("@room") });
        QCOMPARE(mock->lastSentBodyForTest(), QStringLiteral("@room"));
        QVERIFY(!mock->lastSentBodyForTest().contains(
            QStringLiteral("matrix.to")));
        input->setProperty("text", QString());
        info->setProperty("roomId", QString());
        QTest::qWait(20);
    }

    void mentionPopupOpensOnAtToken()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *input = item("composerInput");
        QVERIFY(input);
        openMention(input, QString());
        QVERIFY2(popupVisible(), "the mention popup should open on '@'");
        QObject *popup = mentionPopup();
        QVERIFY(popup);
        QVERIFY(m_controller->property("mentionSuggestions").value<QObject *>());

        // Typing a real character in the input never left it: the input keeps
        // focus while the popup is open.
        QVERIFY(input->property("activeFocus").toBool());

        input->setProperty("text", QString());
        QTest::qWait(20);
        QVERIFY(!popupVisible());
    }

    void arrowSelectionInsertsMentionAndClosesKeepingFocus()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *input = item("composerInput");
        QVERIFY(input);
        openMention(input, QString());
        QVERIFY(popupVisible());
        QObject *popup = mentionPopup();
        QObject *model =
            m_controller->property("mentionSuggestions").value<QObject *>();
        QVERIFY(popup && model);

        QTest::keyClick(m_window, Qt::Key_Down);
        QTest::keyClick(m_window, Qt::Key_Down);
        const int idx = popup->property("currentIndex").toInt();
        QVariantMap sel;
        QMetaObject::invokeMethod(model, "get", Q_RETURN_ARG(QVariantMap, sel),
                                  Q_ARG(int, idx));
        const QString expectedName =
            sel.value(QStringLiteral("displayName")).toString();
        const QString expectedId =
            sel.value(QStringLiteral("userId")).toString();
        QVERIFY(!expectedId.isEmpty());

        QTest::keyClick(m_window, Qt::Key_Return);
        QTest::qWait(20);

        QCOMPARE(input->property("text").toString(),
                 QStringLiteral("@%1 ").arg(expectedName));
        QVERIFY(!popupVisible());
        QVERIFY(input->property("activeFocus").toBool());

        // Sending delivers the expanded matrix.to markdown body AND records
        // the mention id at the backend.
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        m_controller->composer()->send();
        QTest::qWait(20);
        QCOMPARE(mock->lastMentionIdsForTest(), QStringList{ expectedId });
        QVERIFY(mock->lastSentBodyForTest().contains(
            QStringLiteral("https://matrix.to/#/")));
        QVERIFY(mock->lastSentBodyForTest().contains(
            QStringLiteral("[@%1]").arg(expectedName)));
        input->setProperty("text", QString());
    }

    void deletingInsertedNameDropsTheMention()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *input = item("composerInput");
        QVERIFY(input);
        openMention(input, QString());
        QVERIFY(popupVisible());
        QTest::keyClick(m_window, Qt::Key_Return); // pick the first suggestion
        QTest::qWait(20);
        const QString inserted = input->property("text").toString();
        QVERIFY(inserted.startsWith(QLatin1Char('@')));

        // Remove one character from inside the inserted name: the ref must be
        // dropped (its slice no longer matches), so no mention id is sent.
        QString broken = inserted.trimmed();
        broken.chop(1); // drop the last name character
        input->setProperty("text", broken);
        input->setProperty("cursorPosition", broken.length());
        QTest::qWait(20);

        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        m_controller->composer()->send();
        QTest::qWait(20);
        QVERIFY(mock->lastMentionIdsForTest().isEmpty());
        QVERIFY(!mock->lastSentBodyForTest().contains(
            QStringLiteral("matrix.to")));
        input->setProperty("text", QString());
    }

    // ── 2026-08-18 tester report probes ──────────────────────────────────
    // "kai darai shift+enter max praleidzia tik viena eilute" — the composer
    // must keep growing with the draft, up to its scroll cap.
    // Shift+Enter must insert a newline every time, not only once.
    void shiftEnterInsertsEveryNewline()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *input = item("composerInput");
        QVERIFY(input);
        input->setProperty("text", QString());
        QMetaObject::invokeMethod(input, "forceActiveFocus");
        QTest::qWait(30);
        QTest::keyClick(m_window, Qt::Key_A);
        QTest::keyClick(m_window, Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClick(m_window, Qt::Key_B);
        QTest::keyClick(m_window, Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClick(m_window, Qt::Key_C);
        QTest::qWait(50);
        const QString typed = input->property("text").toString();
        QCOMPARE(typed, QStringLiteral("a\nb\nc"));
        input->setProperty("text", QString());
    }

    void composerGrowsWithEveryAddedLine()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *input = item("composerInput");
        auto *flick = item("composerInputFlick");
        QVERIFY(input);
        QVERIFY2(flick, "the composer input needs a named scroll surface");
        input->setProperty("text", QStringLiteral("one"));
        QTest::qWait(60);
        const qreal oneLine = flick->height();
        input->setProperty("text", QStringLiteral("one\ntwo"));
        QTest::qWait(60);
        const qreal twoLines = flick->height();
        input->setProperty("text", QStringLiteral("one\ntwo\nthree\nfour"));
        QTest::qWait(60);
        const qreal fourLines = flick->height();
        QVERIFY2(twoLines > oneLine + 4, "two lines must be taller than one");
        QVERIFY2(fourLines > twoLines + 4,
                 "the composer stopped growing after the second line");
        input->setProperty("text", QString());
    }

    // "kai sushrinkini app iki max net nematai pilnos vienos raides ka
    // typini" — at the narrowest supported window the input must still be
    // wide enough to read what is being typed.
    void narrowWindowKeepsTheInputUsable()
    {
        const int restoreWidth = m_window->width();
        m_window->setWidth(300);
        QTest::qWait(120);
        auto *flick = item("composerInputFlick");
        QVERIFY(flick);
        QVERIFY2(flick->width() >= 96,
                 "the composer input collapses at a narrow window width");
        m_window->setWidth(restoreWidth);
        QTest::qWait(120);
    }

    // "kai iseini ir grizti i chat tavo typewriteri numeti i gala o ne i
    // prieki" — a draft restored on room switch must come back intact with
    // the caret at its end, ready to continue typing.
    void restoredDraftKeepsTextAndPlacesCaretAtTheEnd()
    {
        // Drafts are only stored for a live session (DraftStore refuses a
        // save without a logged-in client), so sign the mock backend in.
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        if (!mock->isLoggedIn()) {
            QSignalSpy spy(mock, &MatrixClient::loginSucceeded);
            mock->login(QStringLiteral("https://mock.local"),
                        QStringLiteral("alice"), QStringLiteral("x"));
            QVERIFY(spy.wait(4000));
            QTest::qWait(50);
        }
        // A persisted draft is account-scoped; without an active account
        // record SettingsManager writes nothing at all.
        if (auto *settings = m_controller->settings()) {
            settings->saveSession(QStringLiteral("https://mock.local"),
                                  QStringLiteral("@alice:mock.local"),
                                  QStringLiteral("DEVICE"),
                                  QStringLiteral("token-fixture"));
        }
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *input = item("composerInput");
        QVERIFY(input);
        input->setProperty("text", QStringLiteral("half a sentence"));
        QTest::qWait(80);
        m_controller->setCurrentRoomId(QStringLiteral("!devs:mock.local"));
        QTest::qWait(80);
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QTest::qWait(120);
        const QString restored = input->property("text").toString();
        const int caret = input->property("cursorPosition").toInt();
        QCOMPARE(restored, QStringLiteral("half a sentence"));
        QCOMPARE(caret, restored.length());
        // Leave no stored draft behind for the next case.
        input->setProperty("text", QString());
        QTest::qWait(1200);
    }

    void escapeClosesPopupWithoutCancellingReply()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        m_controller->composer()->beginReply(QStringLiteral("$evt"),
                                             QStringLiteral("Bob"),
                                             QStringLiteral("hi"));
        QVERIFY(m_controller->composer()->isReplying());
        auto *input = item("composerInput");
        QVERIFY(input);
        openMention(input, QString());
        QVERIFY(popupVisible());

        QTest::keyClick(m_window, Qt::Key_Escape);
        QTest::qWait(20);
        QVERIFY(!popupVisible());
        // The reply state survives: Escape only closed the popup.
        QVERIFY(m_controller->composer()->isReplying());

        m_controller->composer()->cancelReplyOrEdit();
        input->setProperty("text", QString());
    }

    // A dead key — the tester's "¨", which they had to press twice — does not
    // arrive as a key press at all. The platform holds the composing
    // character in a QInputMethodEvent PREEDIT until the next keystroke
    // decides what it composes into, and anything that rewrites the field's
    // text, moves its cursor, or re-lays-out its document while that is
    // pending CANCELS the composition: the character is dropped and the user
    // presses the key again.
    //
    // This composer has three candidates for doing exactly that, all of them
    // running on every text and cursor change — the write-back to
    // AppComposer, the mention re-anchoring, and the MentionHighlighter
    // attached to the field's own QTextDocument. So the preedit path is
    // pinned here rather than reasoned about, including with a live mention
    // in the field, which is the state in which the highlighter actually
    // calls setFormat on the document the composition lives in.
    void aDeadKeyPreeditSurvivesAndComposesInOneKeystroke()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *input = item("composerInput");
        QVERIFY(input);
        input->setProperty("text", QString());
        input->forceActiveFocus();
        QTest::qWait(30);
        QVERIFY(input->hasActiveFocus());

        const auto sendIm = [](const QString &preedit, const QString &commit) {
            QList<QInputMethodEvent::Attribute> attributes;
            if (!preedit.isEmpty())
                attributes.append(QInputMethodEvent::Attribute(
                    QInputMethodEvent::Cursor, preedit.length(), 1,
                    QVariant()));
            QInputMethodEvent event(preedit, attributes);
            if (!commit.isEmpty())
                event.setCommitString(commit);
            QObject *focus = QGuiApplication::focusObject();
            QVERIFY(focus);
            QGuiApplication::sendEvent(focus, &event);
        };

        // The dead key: composing, and deliberately nothing committed yet.
        sendIm(QStringLiteral("¨"), QString());
        QTest::qWait(30);
        QCOMPARE(input->property("preeditText").toString(),
                 QStringLiteral("¨"));
        QCOMPARE(input->property("text").toString(), QString());

        // The NEXT keystroke composes it. One more press, not two.
        sendIm(QString(), QStringLiteral("ä"));
        QTest::qWait(30);
        QCOMPARE(input->property("text").toString(), QStringLiteral("ä"));
        QVERIFY(input->property("preeditText").toString().isEmpty());
        QCOMPARE(m_controller->composer()->text(), QStringLiteral("ä"));

        // Again with a real mention in the field: the highlighter now has a
        // range and formats the same document the composition sits in.
        input->setProperty("text", QString());
        QTest::qWait(20);
        openMention(input, QString());
        QVERIFY(popupVisible());
        QTest::keyClick(m_window, Qt::Key_Down);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTest::qWait(30);
        const QString withMention = input->property("text").toString();
        QVERIFY(!withMention.isEmpty());
        QVERIFY(!m_controller->composer()->mentionRanges().isEmpty());

        sendIm(QStringLiteral("¨"), QString());
        QTest::qWait(30);
        QCOMPARE(input->property("preeditText").toString(),
                 QStringLiteral("¨"));
        QCOMPARE(input->property("text").toString(), withMention);
        sendIm(QString(), QStringLiteral("ä"));
        QTest::qWait(30);
        QCOMPARE(input->property("text").toString(),
                 withMention + QStringLiteral("ä"));

        // Negative control, so none of the above can pass vacuously: a
        // write-back to the field's text IS what cancels a composition, and
        // when one happens the preedit is observably gone. That is the exact
        // failure the tester described, and it is what these assertions would
        // catch if any of this composer's three per-keystroke write-backs
        // ever stopped guarding itself.
        sendIm(QStringLiteral("\u00a8"), QString());
        QTest::qWait(30);
        QCOMPARE(input->property("preeditText").toString(),
                 QStringLiteral("\u00a8"));
        input->setProperty("text", QStringLiteral("rewritten"));
        QTest::qWait(30);
        QVERIFY2(input->property("preeditText").toString().isEmpty(),
                 "a text write-back must cancel the composition, or this "
                 "case proves nothing");

        input->setProperty("text", QString());
        QTest::qWait(20);
    }



    // The composer's text sits on the same line as the icons beside it.
    //
    // It did not: Qt's TextArea-in-Flickable integration parked contentY at
    // -6, painting the single line six pixels below the flickable that
    // contains it. Reported as "the text is not centred when the room is
    // opened, and moves to the right place when you click it" — clicking runs
    // the integration's ensureVisible and resets contentY.
    //
    // This suite could not see it for hours because it did not load the
    // application's own font. The offset depends on the content height, so
    // with the default family contentY happened to land at 0. main() now sets
    // up the bundled families and Manrope exactly as main.cpp does, which is
    // what makes this assertion meaningful at all.
    void theComposerTextSharesTheIconsCentreLine()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QTest::qWait(80);
        auto *input = item("composerInput");
        auto *attach = item("composerAttachButton");
        auto *flick = item("composerInputFlick");
        QVERIFY(input && attach && flick);
        input->setProperty("text", QString());
        QTest::qWait(80);

        auto centreOf = [](QQuickItem *it) {
            return it->mapToItem(nullptr, QPointF(0, it->height() / 2)).y();
        };
        // Where the PLACEHOLDER is actually painted, not merely where its
        // item sits: the offset lived in the flickable's scroll position, so
        // measuring the item alone reported everything as correct.
        auto textCentre = [&]() -> double {
            for (QQuickItem *k : input->childItems()) {
                const QVariant t = k->property("text");
                if (t.isValid()
                    && t.toString() == input->property("placeholderText").toString())
                    return input->mapToItem(nullptr,
                                            QPointF(0, k->y() + k->height() / 2)).y();
            }
            return -1;
        };

        for (int scale : { 100, 120, 140 }) {
            m_controller->settings()->setTextScale(scale);
            QTest::qWait(120);
            QCOMPARE(flick->property("contentY").toDouble(), 0.0);
            const double delta = textCentre() - centreOf(attach);
            QVERIFY2(qAbs(delta) <= 1.0,
                     qPrintable(QStringLiteral(
                         "text sits %1px from the icon centre at %2%% scale")
                         .arg(delta).arg(scale)));
        }
        m_controller->settings()->setTextScale(100);
    }

    // ── Settings › Appearance › Message box buttons hides them ───────────
    //
    // Requested by a tester who wanted a plainer send bar. The binding is the
    // whole feature, so it is driven through the real setting rather than by
    // poking `visible`: hiddenComposerButtons is a notifying property and
    // composerButtonShown() reads it, which is what makes every `visible`
    // binding in the row re-evaluate. A Q_INVOKABLE would not.
    void hidingAComposerButtonInSettingsRemovesItFromTheRow()
    {
        auto *settings = m_controller->settings();
        const QStringList previous = settings->hiddenComposerButtons();

        struct { const char *key; const char *item; } cases[] = {
            { "emoji", "composerEmojiButton" },
            { "media", "composerMediaButton" },
            { "voice", "composerMicButton" },
            { "formatting", "composerFormatToggleButton" },
            { "sendOptions", "composerSendOptionsButton" },
        };
        // MEASURE FIRST, RESTORE, THEN ASSERT. This binary shares one
        // QSettings file across every case in it, so an assertion that fires
        // mid-loop leaves a button hidden on disk for every LATER case and
        // every later run — which is exactly what happened while this was
        // being written (three cases failed the next run on a tree that was
        // fine). The same trap as smoothScrolling in TimelinePaneQmlTest.
        struct Observed { bool present; bool before; bool hidden; bool after; };
        QList<Observed> seen;
        for (const auto &c : cases) {
            auto *button = item(c.item);
            Observed o { button != nullptr, false, true, false };
            if (button) {
                o.before = button->isVisible();
                settings->setComposerButtonShown(QLatin1String(c.key), false);
                QTest::qWait(30);
                o.hidden = button->isVisible();
                settings->setComposerButtonShown(QLatin1String(c.key), true);
                QTest::qWait(30);
                o.after = button->isVisible();
            }
            seen.append(o);
        }
        settings->setHiddenComposerButtons(previous);
        QTest::qWait(30);

        for (int i = 0; i < seen.size(); ++i) {
            const auto &c = cases[i];
            const Observed &o = seen.at(i);
            QVERIFY2(o.present, c.item);
            QVERIFY2(o.before,
                     qPrintable(QStringLiteral("%1 was not visible to start "
                                               "with").arg(c.item)));
            QVERIFY2(!o.hidden,
                     qPrintable(QStringLiteral("turning %1 off left %2 in the "
                                               "row").arg(c.key).arg(c.item)));
            QVERIFY2(o.after,
                     qPrintable(QStringLiteral("turning %1 back on did not "
                                               "restore %2").arg(c.key)
                                    .arg(c.item)));
        }
    }

    // ── GIFs and stickers are ONE window with two tabs ───────────────────
    //
    // They used to be two composer buttons opening two popups; a tester asked
    // for one clean window. The two pickers stay two components (a pack is not
    // a GIF — see the header of GifPicker.qml) and the merge is that the host
    // swaps them in place: same anchor item, same remembered size, no enter or
    // exit transition, so it reads as the window changing tab.
    //
    // Driven through the host's own entry points rather than by opening the
    // popups directly, because the swap IS the feature.
    void theMediaPickerIsOneWindowWithTwoTabs()
    {
        auto *bar = item("composerBar");
        QVERIFY(bar != nullptr);
        const QString previousRoom = m_controller->currentRoomId();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QTest::qWait(30);

        auto opened = [this](const char *name) {
            QObject *p = m_root->findChild<QObject *>(QLatin1String(name));
            return p && p->property("opened").toBool();
        };

        QMetaObject::invokeMethod(bar, "openMediaPicker",
                                  Q_ARG(QVariant, false));
        QTest::qWait(80);
        QVERIFY2(opened("composerGifPicker"),
                 "the one media button did not open the GIF panel");
        QVERIFY(!opened("composerStickerPicker"));

        // The strip inside asks the HOST to swap — a picker that closed and
        // opened its sibling itself would have to know its own anchor item
        // and its host's other picker.
        QMetaObject::invokeMethod(bar, "swapMediaPicker",
                                  Q_ARG(QVariant, QStringLiteral("sticker")));
        QTest::qWait(80);
        QVERIFY2(opened("composerStickerPicker"),
                 "swapping to Stickers did not open the sticker panel");
        QVERIFY2(!opened("composerGifPicker"),
                 "swapping left the GIF panel open too: that is two windows, "
                 "which is exactly what this replaced");

        // Same anchor and the same remembered size key, which is what makes
        // the swap read as one window rather than two.
        auto *gif = m_root->findChild<QObject *>(
            QStringLiteral("composerGifPicker"));
        auto *sticker = m_root->findChild<QObject *>(
            QStringLiteral("composerStickerPicker"));
        QVERIFY(gif && sticker);
        QCOMPARE(gif->property("anchorItem").value<QQuickItem *>(),
                 sticker->property("anchorItem").value<QQuickItem *>());
        QCOMPARE(gif->property("sizeSettingsKey").toString(),
                 sticker->property("sizeSettingsKey").toString());

        // And the button toggles the PAIR: pressing it while the sticker half
        // is showing closes the window, it does not open the GIF half.
        QMetaObject::invokeMethod(bar, "openMediaPicker",
                                  Q_ARG(QVariant, false));
        QTest::qWait(80);
        QVERIFY(!opened("composerStickerPicker"));
        QVERIFY(!opened("composerGifPicker"));

        m_controller->setCurrentRoomId(previousRoom);
        QTest::qWait(30);
    }

    // ── Send options ride beside Send, not out in the glyph row ──────────
    //
    // "Send later" was a clock icon among the emoji and GIF buttons, where it
    // read as one more unrelated glyph and vanished entirely in a narrow
    // window with no menu entry standing in for it. Rokas asked for a chevron
    // on the RIGHT of the send button.
    void sendOptionsSitRightOfSendAndOfferBothActions()
    {
        auto *send = item("composerSendButton");
        auto *chevron = item("composerSendOptionsButton");
        QVERIFY(send && chevron);
        QVERIFY2(!item("composerSendLaterButton"),
                 "the standalone send-later clock is back in the glyph row");
        QVERIFY(chevron->isVisible());
        QVERIFY2(chevron->mapToScene(QPointF(0, 0)).x()
                     > send->mapToScene(QPointF(0, 0)).x(),
                 "the chevron is not to the right of Send");
        // Narrower than Send, and the same height: a split button, not a
        // second send.
        QCOMPARE(chevron->height(), send->height());
        QVERIFY(chevron->width() < send->width());

        for (const char *name : { "composerSendLaterMenuItem",
                                  "composerScheduledListMenuItem" }) {
            QVERIFY2(m_root->findChild<QObject *>(QLatin1String(name)), name);
        }
    }

    // ── The picker buttons TOGGLE ────────────────────────────────────────
    //
    // Reported by a tester: pressing the emoji icon while the emoji panel is
    // open made it blink and stay open. The pickers carry
    // Popup.CloseOnPressOutside and the icon that opens them is outside, so
    // the PRESS closed the panel and the button's own click — which arrives
    // on the RELEASE — opened it straight back.
    //
    // Driven with a REAL mouse click on the REAL button, because that is the
    // only thing that reproduces the ordering. Calling openEmojiPicker()
    // twice by hand proves nothing: the second call never sees the
    // press-outside close that is the whole defect.
    void aSecondPressOnAPickerButtonClosesItsPanel()
    {
        auto *bar = item("composerBar");
        QVERIFY(bar != nullptr);
        const QString previousRoom = m_controller->currentRoomId();
        // The icon buttons are gated on a room being open.
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QTest::qWait(30);

        // The pickers are Popups (QObject, not QQuickItem), reached by
        // objectName exactly like mentionPopup() above. An `id` cannot be
        // read from here: it lives in MessageComposerBar's own component
        // context, which is not the context the scene was created in.
        auto opened = [this](const char *name) {
            QObject *p = m_root->findChild<QObject *>(QLatin1String(name));
            return p && p->property("opened").toBool();
        };

        auto *button = item("composerEmojiButton");
        QVERIFY(button != nullptr);
        QVERIFY2(button->isVisible(),
                 "the emoji button is displaced into the attach menu at this "
                 "width; the gesture under test is unreachable");
        QVERIFY2(button->property("enabled").toBool(),
                 "the emoji button is disabled, so no click can reach it");

        const QPointF centre = button->mapToScene(
            QPointF(button->width() / 2.0, button->height() / 2.0));

        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          centre.toPoint());
        QTest::qWait(120);
        QVERIFY2(opened("composerEmojiPicker"),
                 "the first press did not open the emoji panel");

        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          centre.toPoint());
        QTest::qWait(120);
        QVERIFY2(!opened("composerEmojiPicker"),
                 "pressing the emoji button again left the panel open — it "
                 "closed on the press and the click reopened it");

        // And a third press opens it again: the toggle must not latch shut.
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          centre.toPoint());
        QTest::qWait(120);
        QVERIFY2(opened("composerEmojiPicker"),
                 "a third press did not reopen the panel");

        if (QObject *p =
                m_root->findChild<QObject *>(QStringLiteral("composerEmojiPicker")))
            QMetaObject::invokeMethod(p, "close");
        QTest::qWait(60);
        m_controller->setCurrentRoomId(previousRoom);
    }

    // ── 2026-09-20 composer GUI audit ───────────────────────────────────

    // FINDING 2 (MEDIUM). The mention / slash-command / emoji-shortcode
    // popups were placed at the TEXT FIELD's scene top, which is INSIDE the
    // composer card whenever anything sits above the input row. With the
    // formatting toolbar open that covered 38 px of the 43 px toolbar row
    // (measured at 1920x1400: toolbar 1266..1308, popup 1271..1316) and the
    // popup crossed the toolbar/input divider — B, I, S, code, link, list,
    // quote and the mode toggle all hidden behind the suggestion list while
    // you type. The reply/thread context banner sits in the same card.
    //
    // Asserted against the CARD, not against the toolbar, because the card
    // is what the popup has to clear for every one of its rows.
    void aCompletionPopupClearsTheWholeComposerCard()
    {
        const int restoreHeight = m_window->height();
        const QString previousRoom = m_controller->currentRoomId();
        // A short window makes the popup's own bottom clamp
        // (Math.max(spacing4, …)) the binding constraint rather than the
        // anchor under test, and then the case would measure the clamp.
        m_window->setHeight(820);
        QTest::qWait(120);
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *bar = item("composerBar");
        QVERIFY(bar);
        bar->setProperty("toolbarExpanded", true);
        QTest::qWait(80);

        auto *card = item("composerCard");
        auto *toolbar = item("composerToolbarRow");
        QVERIFY(card && toolbar);
        QVERIFY2(toolbar->isVisible(), "the toolbar row did not open");

        auto *input = item("composerInput");
        QVERIFY(input);
        openMention(input, QString());
        QVERIFY2(popupVisible(), "the mention popup should open on '@'");

        QObject *popup = mentionPopup();
        QVERIFY(popup);
        const qreal popupTop = popup->property("y").toReal();
        const qreal popupBottom = popupTop + popup->property("height").toReal();
        const qreal cardTop = card->mapToScene(QPointF(0, 0)).y();
        const qreal toolbarTop = toolbar->mapToScene(QPointF(0, 0)).y();
        // The popup's parent is Overlay.overlay, which fills the window, so
        // its y is already in scene coordinates.
        QVERIFY2(popupBottom <= toolbarTop + 0.5,
                 qPrintable(QStringLiteral(
                     "the popup (%1..%2) overlaps the formatting toolbar "
                     "(top %3) by %4 px")
                        .arg(popupTop).arg(popupBottom).arg(toolbarTop)
                        .arg(popupBottom - toolbarTop)));
        QVERIFY2(popupBottom <= cardTop + 0.5,
                 qPrintable(QStringLiteral(
                     "the popup (%1..%2) reaches into the composer card "
                     "(top %3)").arg(popupTop).arg(popupBottom).arg(cardTop)));
        // …and it is still ATTACHED to the card rather than floating: the
        // gap is the 4 px the popups place themselves with.
        QVERIFY2(cardTop - popupBottom <= 8.0,
                 qPrintable(QStringLiteral("the popup detached from the card "
                                           "by %1 px").arg(cardTop - popupBottom)));

        input->setProperty("text", QString());
        QTest::qWait(30);
        bar->setProperty("toolbarExpanded", false);
        m_controller->setCurrentRoomId(previousRoom);
        m_window->setHeight(restoreHeight);
        QTest::qWait(120);
    }

    // FINDING 3 (MEDIUM-LOW). The `+` button's menu used a bare popup(),
    // which opens AT THE POINTER and downward, over the composer it belongs
    // to: measured at 1920x1380 the menu spanned y 1304..1379 with the input
    // row at 1310..1362 and no clearance at all below it. Its two siblings
    // in the same file are anchored above the card for exactly this reason.
    //
    // Driven with a REAL click, because the two halves of the defect live in
    // two places: the missing parent/x/y on the menu, and the popup() call
    // at the button. An imperative popup() also DESTROYS the y binding, so
    // asserting y is still the declared -height-4 after the click is what
    // catches that half.
    void theAttachMenuOpensAboveTheCardNotOverIt()
    {
        const int restoreWidth = m_window->width();
        const QString previousRoom = m_controller->currentRoomId();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        // Narrow, so the button opens the MENU: with polls unsupported on the
        // mock backend a wide row goes straight to the file dialog instead,
        // and a native file dialog is not something a test may open.
        m_window->setWidth(360);
        QTest::qWait(150);

        auto *button = item("composerAttachButton");
        QVERIFY(button && button->isVisible()
                && button->property("enabled").toBool());
        auto *menu = m_root->findChild<QObject *>(
            QStringLiteral("composerAttachMenu"));
        QVERIFY(menu);

        const QPointF centre = button->mapToScene(
            QPointF(button->width() / 2.0, button->height() / 2.0));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          centre.toPoint());
        QTest::qWait(150);
        QVERIFY2(menu->property("visible").toBool(),
                 "the attach button did not open the attach menu");

        auto *card = item("composerCard");
        QVERIFY(card);
        QCOMPARE(menu->property("parent").value<QQuickItem *>(), card);
        const qreal y = menu->property("y").toReal();
        const qreal h = menu->property("height").toReal();
        QVERIFY(h > 0);
        QVERIFY2(qAbs(y - (-h - 4.0)) < 0.5,
                 qPrintable(QStringLiteral(
                     "the attach menu is not anchored above the card: "
                     "y=%1 height=%2 (expected y=%3). A popup() call at the "
                     "button overwrites this binding.")
                        .arg(y).arg(h).arg(-h - 4.0)));
        QVERIFY2(y + h <= 0.0,
                 "the attach menu still hangs down over the composer card");
        // …and the anchor is a LIVE BINDING, not a value that happened to be
        // right once. popup() assigns x/y imperatively, which destroys the
        // binding for the life of the object — the one-way door this project
        // has paid for five times — and under the offscreen platform a
        // cursor-placed menu can land close enough to the declared position
        // to pass a static check. Move the height and the y must follow.
        menu->setProperty("height", h + 24.0);
        QTest::qWait(60);
        QVERIFY2(qAbs(menu->property("y").toReal() - (-(h + 24.0) - 4.0)) < 0.5,
                 qPrintable(QStringLiteral(
                     "the attach menu's y is no longer bound to its height "
                     "(y=%1 after height %2): something assigned it, and a "
                     "popup() at the button is what does that")
                        .arg(menu->property("y").toReal()).arg(h + 24.0)));
        // And it clears the bottom of the window, which the pointer-placed
        // menu did not.
        const qreal sceneBottom = card->mapToScene(QPointF(0, y + h)).y();
        QVERIFY2(sceneBottom <= m_window->height(),
                 "the attach menu runs off the bottom of the window");

        QMetaObject::invokeMethod(menu, "close");
        QTest::qWait(80);
        m_window->setWidth(restoreWidth);
        QTest::qWait(150);
        m_controller->setCurrentRoomId(previousRoom);
    }

    // FINDING 4 (LOW-MEDIUM). The attachment tray is a Flow with no
    // alignment, so chips are top-aligned and each was sized by its own
    // content: an image chip by its 64x48 preview tile, a plain file chip by
    // the two-label column. Measured with four files attached at once, the
    // image chips spanned 56 px and the .txt chip 42 — a ragged bottom edge.
    void everyAttachmentChipIsTheSameHeight()
    {
        const QString previousRoom = m_controller->currentRoomId();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QTest::qWait(30);
        auto *composer =
            m_controller->property("composer").value<QObject *>();
        QVERIFY(composer);
        QVERIFY(composer->property("attachmentsSupported").toBool());

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString png = writeFile(dir, QStringLiteral("photo.png"),
                                      tinyPng());
        const QString txt = writeFile(dir, QStringLiteral("notes.txt"),
                                      QByteArrayLiteral("hello there"));
        QVERIFY(!png.isEmpty() && !txt.isEmpty());
        QMetaObject::invokeMethod(composer, "addAttachment",
                                  Q_ARG(QUrl, QUrl::fromLocalFile(png)));
        QMetaObject::invokeMethod(composer, "addAttachment",
                                  Q_ARG(QUrl, QUrl::fromLocalFile(txt)));
        QTest::qWait(200);

        QList<QQuickItem *> chips;
        collectItems(m_window->contentItem(),
                     QStringLiteral("composerAttachmentChip"), chips);
        QCOMPARE(chips.size(), 2);
        QVERIFY(chips[0]->height() > 0);
        QVERIFY2(qAbs(chips[0]->height() - chips[1]->height()) < 0.5,
                 qPrintable(QStringLiteral(
                     "the attachment chips disagree by %1 px (%2 vs %3): a "
                     "file chip floats above the image chip beside it")
                        .arg(qAbs(chips[0]->height() - chips[1]->height()))
                        .arg(chips[0]->height()).arg(chips[1]->height())));
        // Bottom edges, which is what the eye actually reads in a Flow.
        QVERIFY2(qAbs(chips[0]->mapToScene(QPointF(0, chips[0]->height())).y()
                      - chips[1]->mapToScene(QPointF(0, chips[1]->height())).y())
                     < 0.5,
                 "the attachment tray has a ragged bottom edge");

        auto *queue = composer->property("attachments").value<QObject *>();
        QVERIFY(queue);
        QMetaObject::invokeMethod(queue, "clearAll");
        QTest::qWait(80);
        m_controller->setCurrentRoomId(previousRoom);
    }

    // FINDING 5 (LOW-MEDIUM). composerFormatToggleButton is
    // `visible: !compactInputRow`, but the toolbar row it controls is not —
    // so opening the toolbar in a wide window and then narrowing it left the
    // toolbar drawn with no way to put it away. Every other control the
    // narrow row hides is offered from a menu; this one was dropped, which
    // is the opposite of what that row's own comments commit to.
    void aNarrowRowStillOffersTheFormattingToggle()
    {
        const int restoreWidth = m_window->width();
        const QString previousRoom = m_controller->currentRoomId();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *bar = item("composerBar");
        QVERIFY(bar);
        bar->setProperty("toolbarExpanded", true);
        QTest::qWait(60);

        m_window->setWidth(360);
        QTest::qWait(150);
        QVERIFY2(bar->property("compactInputRow").toBool(),
                 "360 px is not a compact row; the case measures nothing");
        auto *toggle = item("composerFormatToggleButton");
        QVERIFY2(!toggle || !toggle->isVisible(),
                 "the format toggle is still in the row, so nothing is lost "
                 "and this case measures nothing");
        QVERIFY2(bar->property("toolbarExpanded").toBool(),
                 "narrowing the window closed the toolbar on its own");
        auto *toolbarRow = item("composerToolbarRow");
        QVERIFY2(toolbarRow && toolbarRow->isVisible(),
                 "the toolbar row is hidden in a compact row, so there is "
                 "nothing to close");

        auto *formatting = m_root->findChild<QObject *>(
            QStringLiteral("composerOverflowFormattingItem"));
        QVERIFY2(formatting != nullptr,
                 "the compact overflow menu offers no Formatting item, so an "
                 "open toolbar cannot be closed at this width");
        // A menu's rows report effective visibility, so the menu has to be
        // open before `visible` means anything about the row.
        auto *overflow = m_root->findChild<QObject *>(
            QStringLiteral("composerOverflowMenu"));
        QVERIFY(overflow);
        QMetaObject::invokeMethod(overflow, "open");
        QTest::qWait(150);
        QVERIFY2(formatting->property("visible").toBool(),
                 "the Formatting item is in the menu but not shown");
        QVERIFY2(formatting->property("text").toString()
                     .contains(QStringLiteral("formatting"), Qt::CaseInsensitive),
                 "the Formatting item does not name what it does");
        // Clicked for real, on the row, with the menu open: a directly
        // emitted `triggered` would prove the handler compiles and nothing
        // about the row being reachable.
        auto clickTheRow = [this, formatting, overflow]() {
            auto *row = qobject_cast<QQuickItem *>(formatting);
            QVERIFY(row);
            QVERIFY(row->width() > 0 && row->height() > 0);
            const QPointF centre = row->mapToScene(
                QPointF(row->width() / 2.0, row->height() / 2.0));
            QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                              centre.toPoint());
            QTest::qWait(120);
            QVERIFY(!overflow->property("visible").toBool());
        };
        clickTheRow();
        QVERIFY2(!bar->property("toolbarExpanded").toBool(),
                 "the overflow menu's Formatting item did not close the "
                 "toolbar");
        // And it opens it again — a toggle, not a one-way close.
        QMetaObject::invokeMethod(overflow, "open");
        QTest::qWait(150);
        clickTheRow();
        QVERIFY2(bar->property("toolbarExpanded").toBool(),
                 "the Formatting item closes the toolbar but cannot reopen it");

        bar->setProperty("toolbarExpanded", false);
        m_window->setWidth(restoreWidth);
        QTest::qWait(150);
        m_controller->setCurrentRoomId(previousRoom);
    }

    // FINDING 7 (LOW). `VoicePreviewBar.waveform` was declared, bound by
    // both hosts with the recorder's real MSC3245 buckets, and read by
    // nothing — `grep waveform VoicePreviewBar.qml` returned the
    // declaration alone. The preview showed play / time / discard / send and
    // no waveform at all.
    //
    // The heights are what this case is really about. The buckets are
    // 0..=100, and the renderer this was modelled on clamps with
    // `Math.min(1, wf[at])` — against 0..=100 data that makes EVERY bar
    // full height, which is a solid block, not a waveform. So the assertion
    // is that the bars DIFFER and that a loud bucket is drawn taller than a
    // quiet one, not merely that something was drawn.
    void theVoicePreviewDrawsTheWaveformItIsHanded()
    {
        const QString previousRoom = m_controller->currentRoomId();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *bar = item("composerBar");
        QVERIFY(bar);

        QVariantList wave;
        for (int i = 0; i < 18; ++i)
            wave << (i % 2 == 0 ? 100 : 10);
        QVariantMap pending;
        // No filePath: the MediaPlayer stays sourceless, so nothing decodes
        // and nothing plays in a headless run. The strip is drawn from the
        // buckets alone.
        pending.insert(QStringLiteral("filePath"), QString());
        pending.insert(QStringLiteral("mime"), QStringLiteral("audio/ogg"));
        pending.insert(QStringLiteral("durationMs"), 29000);
        pending.insert(QStringLiteral("waveform"), wave);
        bar->setProperty("pendingVoice", pending);
        QTest::qWait(120);

        auto *preview = item("composerVoicePreview");
        QVERIFY2(preview && preview->isVisible(),
                 "the voice preview bar did not appear");
        auto *strip = item("voicePreviewWave");
        QVERIFY2(strip != nullptr,
                 "the voice preview has no waveform at all");
        QVERIFY2(strip->isVisible(),
                 "the voice preview was handed a waveform and drew none");
        auto *row = item("voicePreviewWaveRow");
        QVERIFY(row);

        QList<QQuickItem *> bars;
        const auto children = row->childItems();
        for (QQuickItem *child : children) {
            if (child->width() > 0 && child->height() > 0)
                bars.append(child);
        }
        QVERIFY2(bars.size() >= 8,
                 qPrintable(QStringLiteral("only %1 waveform bars were drawn")
                                .arg(bars.size())));
        qreal minH = bars.first()->height();
        qreal maxH = minH;
        for (QQuickItem *b : bars) {
            minH = qMin(minH, b->height());
            maxH = qMax(maxH, b->height());
        }
        QVERIFY2(maxH - minH > 1.0,
                 qPrintable(QStringLiteral(
                     "every waveform bar is %1 px tall — the 0..=100 buckets "
                     "were clamped to 1 instead of divided by 100, so this is "
                     "a solid block, not a waveform").arg(maxH)));
        QVERIFY2(maxH <= strip->height() + 0.5,
                 "a waveform bar is taller than the strip containing it");

        // Cleared through the composer's own path: the file path is empty,
        // so AppController::discardPreparedVoice refuses it before it can
        // touch the recorder, and the slot goes back to null.
        QMetaObject::invokeMethod(bar, "discardPendingVoice");
        QTest::qWait(60);
        QVERIFY(!preview->isVisible());
        m_controller->setCurrentRoomId(previousRoom);
    }

    // FINDING 6 (LOW). The compact "…" menu rendered at exactly 220 px —
    // AppTheme.menuWidthDefault — and elided its own longest row to "Record
    // a voice messa…", although AppMenu's own comment promises the design
    // width is "a floor, not a clamp". Asserted as the elision condition
    // itself (a row's implicit width against the width it was given), so a
    // translation that outgrows the new width fails here rather than on a
    // user's screen.
    void theCompactOverflowMenuDoesNotElideItsOwnRows()
    {
        const int restoreWidth = m_window->width();
        const QString previousRoom = m_controller->currentRoomId();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        m_window->setWidth(360);
        QTest::qWait(150);

        auto *menu = m_root->findChild<QObject *>(
            QStringLiteral("composerOverflowMenu"));
        QVERIFY(menu);
        QMetaObject::invokeMethod(menu, "open");
        QTest::qWait(150);
        QVERIFY(menu->property("visible").toBool());

        int measured = 0;
        for (const char *name : { "composerOverflowFormattingItem",
                                  "composerOverflowEmojiItem",
                                  "composerOverflowMediaItem",
                                  "composerOverflowVoiceItem" }) {
            auto *row = m_root->findChild<QQuickItem *>(QLatin1String(name));
            QVERIFY2(row != nullptr, name);
            // Measured whether or not it is currently visible: the mock
            // backend has no GIF or sticker provider, so "GIFs and
            // stickers" is hidden here — and a row that is hidden on this
            // harness and shown on a real account must still fit.
            ++measured;
            QVERIFY2(row->implicitWidth() <= row->width() + 0.5,
                     qPrintable(QStringLiteral(
                         "%1 is elided: it needs %2 px and was given %3 "
                         "(menu width %4)")
                            .arg(QLatin1String(name))
                            .arg(row->implicitWidth()).arg(row->width())
                            .arg(menu->property("width").toReal())));
        }
        // Assert the COUNT that was actually measured: a loop whose rows all
        // resolved invisible would otherwise pass having checked nothing.
        QCOMPARE(measured, 4);

        QMetaObject::invokeMethod(menu, "close");
        QTest::qWait(80);
        m_window->setWidth(restoreWidth);
        QTest::qWait(150);
        m_controller->setCurrentRoomId(previousRoom);
    }

    // ── GUI sweep 2026-09-20 ────────────────────────────────────────────
    //
    // D-3(a) (MEDIUM). Hover the composer's format toggle and then CLICK it:
    // the formatting toolbar expands into exactly the band the tooltip
    // occupies and the tooltip is never dismissed. Two facts make it
    // inevitable rather than accidental, and both are in the Basic style's
    // own ToolTip.qml: the tip is `y: -implicitHeight - 3`, i.e. drawn ABOVE
    // its control, and its closePolicy is `CloseOnPressOutsideParent`, which
    // does not fire for a press INSIDE the control. The bar is anchored to
    // the bottom of the window, so the card grows UPWARD and the new row
    // lands under the tip. On the real GUI that hid 5 of the 12 px of each
    // of B / I / S / <>; here it is 13 px of each button's 28.
    //
    // The case asserts three things in order, because the middle one is what
    // stops the last from passing for the wrong reason: the tip EXISTS with
    // the toolbar closed (so removing it outright fails here), the toolbar
    // really is inside the rectangle that tip is drawn in, and it is not
    // shown once the toolbar is open.
    void theFormatTipIsNotDrawnOverTheToolbarItOpens()
    {
        const QString previousRoom = m_controller->currentRoomId();
        const int restoreWidth = m_window->width();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        m_window->setWidth(900);
        QTest::qWait(150);
        auto *bar = item("composerBar");
        auto *toggle = item("composerFormatToggleButton");
        QVERIFY(bar && toggle);
        bar->setProperty("toolbarExpanded", false);
        QTest::qWait(150);
        QVERIFY2(toggle->isVisible(),
                 "the format toggle is not in the row at 900 px, so this "
                 "case measures nothing");

        QVERIFY2(hoverAndWaitForTip(toggle),
                 "the format toggle shows no tooltip at all with the toolbar "
                 "closed — this case can no longer tell a fix from a removal");
        const QRectF band = tipSceneRect(toggle);
        hoverAway();
        QVERIFY2(band.width() > 0 && band.height() > 0,
                 qPrintable(QStringLiteral("no tooltip rectangle: %1")
                                .arg(r2s(band))));

        bar->setProperty("toolbarExpanded", true);
        QTest::qWait(200);
        int covered = 0;
        qreal deepest = 0;
        QString detail;
        for (const char *name : { "composerFormat_bold",
                                  "composerFormat_italic",
                                  "composerFormat_strike",
                                  "composerFormat_code" }) {
            auto *btn = item(name);
            QVERIFY2(btn, name);
            const QRectF over = sceneRect(btn).intersected(band);
            if (over.width() > 0 && over.height() > 0) {
                ++covered;
                deepest = qMax(deepest, over.height());
                detail += QStringLiteral(" %1 %2x%3;")
                              .arg(QLatin1String(name))
                              .arg(over.width(), 0, 'f', 1)
                              .arg(over.height(), 0, 'f', 1);
            }
        }
        QVERIFY2(covered >= 3 && deepest >= 8,
                 qPrintable(QStringLiteral(
                     "the tooltip band %1 does not land on the toolbar "
                     "(covered %2 of 4, deepest %3 px):%4 — the assertion "
                     "below would pass for free")
                        .arg(r2s(band)).arg(covered)
                        .arg(deepest, 0, 'f', 1).arg(detail)));

        const bool shownOverTheToolbar = hoverAndWaitForTip(toggle);
        hoverAway();
        QVERIFY2(!shownOverTheToolbar,
                 qPrintable(QStringLiteral(
                     "the format toggle's tooltip is drawn over the toolbar "
                     "it opened: the tip occupies %1 and covers%2")
                        .arg(r2s(band)).arg(detail)));

        bar->setProperty("toolbarExpanded", false);
        m_window->setWidth(restoreWidth);
        QTest::qWait(150);
        m_controller->setCurrentRoomId(previousRoom);
    }

    // D-1 (LOW), and its unreported twin. Every menu this bar opens is
    // parented to the composer CARD at `y: -height - 4`; every tooltip is
    // drawn 3 px above its own control. With the toolbar collapsed — the
    // default — the menu's bottom edge therefore lands INSIDE the tip of the
    // very button that opened it, and the tip survives as a sliced strip
    // underneath it (the sweep measured 3 px of a ~10 px cap height on the
    // attach button). The send-options button has the identical shape and
    // was found while fixing the attach one.
    void aMenuThisBarOpensDoesNotCoverItsOwnButtonsTip()
    {
        const QString previousRoom = m_controller->currentRoomId();
        const int restoreWidth = m_window->width();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        m_window->setWidth(900);
        QTest::qWait(150);
        auto *bar = item("composerBar");
        QVERIFY(bar);
        bar->setProperty("toolbarExpanded", false);
        QTest::qWait(150);

        struct Pair { const char *button; const char *menu; };
        const Pair pairs[] = {
            { "composerAttachButton", "composerAttachMenu" },
            { "composerSendOptionsButton", "composerSendOptionsMenu" },
        };
        int measured = 0;
        for (const Pair &pair : pairs) {
            auto *button = item(pair.button);
            QVERIFY2(button, pair.button);
            QVERIFY2(button->isVisible(), pair.button);
            auto *menu = m_root->findChild<QObject *>(
                QLatin1String(pair.menu));
            QVERIFY2(menu, pair.menu);

            QVERIFY2(hoverAndWaitForTip(button),
                     qPrintable(QStringLiteral(
                         "%1 shows no tooltip with its menu closed — this "
                         "case can no longer tell a fix from a removal")
                            .arg(QLatin1String(pair.button))));
            const QRectF band = tipSceneRect(button);
            hoverAway();

            QMetaObject::invokeMethod(menu, "open");
            QTest::qWait(220);
            QVERIFY2(menu->property("visible").toBool(), pair.menu);
            const QRectF menuRect = popupSceneRect(menu);
            QVERIFY2(menuRect.intersects(band),
                     qPrintable(QStringLiteral(
                         "%1's menu %2 does not reach its tooltip %3, so the "
                         "assertion below would pass for free")
                            .arg(QLatin1String(pair.menu))
                            .arg(r2s(menuRect)).arg(r2s(band))));

            const bool shownUnderTheMenu = hoverAndWaitForTip(button);
            QMetaObject::invokeMethod(menu, "close");
            hoverAway();
            QTest::qWait(120);
            QVERIFY2(!shownUnderTheMenu,
                     qPrintable(QStringLiteral(
                         "%1's tooltip %2 is still shown under its own open "
                         "menu %3")
                            .arg(QLatin1String(pair.button))
                            .arg(r2s(band)).arg(r2s(menuRect))));
            ++measured;
        }
        // The count, not the loop: a pair whose button resolved hidden would
        // otherwise leave this case asserting nothing.
        QCOMPARE(measured, 2);

        m_window->setWidth(restoreWidth);
        QTest::qWait(150);
        m_controller->setCurrentRoomId(previousRoom);
    }

    // H4 (MEDIUM). The formatting row's chips before the mode switch are
    // fixed 28 px icon buttons, and a RowLayout takes a child's implicit
    // width as its minimum — so the row does not shrink, and the one chip
    // that carries a WORD was painted outside the card: 19 px over at the
    // application's own minimum client width, and measured here at 27 px
    // (Markdown) and 80 px (rich) once the card is narrow enough.
    //
    // `minWidth: 0` — which the sweep proposed and the two chips above it
    // carry — is NOT the fix and was measured not to be: the chip's implicit
    // width is its content's (75 px), already above AppButton's 72 px floor,
    // and AppButton centres its label in an unconstrained Row so the label
    // does not elide however small the box gets.
    //
    // The invariant is the assertion: while the chip is shown, all of its
    // ink is inside the card. Both outcomes are counted so neither half can
    // go missing, and the narrow half also proves the action was DISPLACED
    // into the overflow menu rather than dropped.
    void theModeChipNeverPaintsOutsideTheCard()
    {
        const QString previousRoom = m_controller->currentRoomId();
        const int restoreWidth = m_window->width();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *settings = m_controller->findChild<SettingsManager *>();
        QVERIFY(settings);
        const QString previousMode = settings->composerMode();
        auto *bar = item("composerBar");
        auto *card = item("composerCard");
        auto *chip = item("composerModeToggle");
        QVERIFY(bar && card && chip);
        bar->setProperty("toolbarExpanded", true);
        QTest::qWait(150);
        QQuickItem *ink = findItem(chip, QStringLiteral("buttonLabel"));
        QVERIFY2(ink, "the mode chip has no label to measure");

        int shown = 0, displaced = 0, measured = 0;
        for (const char *mode : { "markdown", "rich" }) {
            settings->setComposerMode(QLatin1String(mode));
            QTest::qWait(180);
            for (int w : { 900, 640, 480, 440, 420, 400, 380, 360, 340, 320 }) {
                m_window->setWidth(w);
                QTest::qWait(150);
                ++measured;
                const QRectF cardRect = sceneRect(card);
                if (!chip->isVisible()) {
                    ++displaced;
                    continue;
                }
                ++shown;
                const QRectF chipRect = sceneRect(chip);
                const QRectF inkRect = sceneRect(ink);
                QVERIFY2(chipRect.right() <= cardRect.right() + 0.5,
                         qPrintable(QStringLiteral(
                             "%1 mode at %2 px: the mode chip %3 crosses the "
                             "card's right edge (%4) by %5 px")
                                .arg(QLatin1String(mode)).arg(w)
                                .arg(r2s(chipRect))
                                .arg(cardRect.right(), 0, 'f', 1)
                                .arg(chipRect.right() - cardRect.right(),
                                     0, 'f', 1)));
                QVERIFY2(inkRect.right() <= cardRect.right() + 0.5,
                         qPrintable(QStringLiteral(
                             "%1 mode at %2 px: the mode chip's LABEL %3 is "
                             "painted past the card's right edge (%4) by %5 "
                             "px — the box fits and the ink does not")
                                .arg(QLatin1String(mode)).arg(w)
                                .arg(r2s(inkRect))
                                .arg(cardRect.right(), 0, 'f', 1)
                                .arg(inkRect.right() - cardRect.right(),
                                     0, 'f', 1)));
            }
        }
        QCOMPARE(measured, 20);
        QVERIFY2(shown >= 8,
                 qPrintable(QStringLiteral(
                     "the mode chip was shown at only %1 of 20 widths — the "
                     "fit assertion above is nearly vacuous").arg(shown)));
        QVERIFY2(displaced >= 8,
                 qPrintable(QStringLiteral(
                     "the mode chip was hidden at only %1 of 20 widths, so "
                     "the narrow half of this case measures nothing")
                        .arg(displaced)));

        // Displaced, not dropped: the overflow menu carries it and it works.
        settings->setComposerMode(QStringLiteral("markdown"));
        m_window->setWidth(320);
        QTest::qWait(180);
        QVERIFY2(!chip->isVisible(), "the chip is still in the row at 320 px");
        auto *overflow = m_root->findChild<QObject *>(
            QStringLiteral("composerOverflowMenu"));
        QVERIFY(overflow);
        QMetaObject::invokeMethod(overflow, "open");
        QTest::qWait(200);
        auto *row = m_root->findChild<QQuickItem *>(
            QStringLiteral("composerOverflowModeItem"));
        QVERIFY2(row, "the overflow menu has no composing-mode row, so the "
                      "mode switch is unreachable at this width");
        QVERIFY2(row->property("visible").toBool(),
                 "the composing-mode row is in the menu but not shown");
        QVERIFY2(row->implicitWidth() <= row->width() + 0.5,
                 qPrintable(QStringLiteral(
                     "the composing-mode row is elided: it needs %1 px and "
                     "was given %2")
                        .arg(row->implicitWidth()).arg(row->width())));
        // Clicked on the row, not a bare `triggered`: a directly emitted
        // signal proves the handler compiles and nothing about the row.
        const QPointF centre = row->mapToScene(
            QPointF(row->width() / 2.0, row->height() / 2.0));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          centre.toPoint());
        QTest::qWait(200);
        QCOMPARE(settings->composerMode(), QStringLiteral("rich"));

        settings->setComposerMode(previousMode);
        bar->setProperty("toolbarExpanded", false);
        m_window->setWidth(restoreWidth);
        QTest::qWait(180);
        m_controller->setCurrentRoomId(previousRoom);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    // Match main.cpp: the bundled families, Manrope as the application font,
    // and the Basic style. Without these the harness measures a DIFFERENT
    // typeface from the one the application renders, which is how a composer
    // that measured perfectly centred here could sit visibly low there.
    for (const char *font : { "Manrope[wght].ttf", "JetBrainsMono[wght].ttf",
                              "Inter[wght].ttf", "IBMPlexSans[wght].ttf",
                              "SourceSans3[wght].ttf",
                              "PlusJakartaSans[wght].ttf",
                              "SpaceGrotesk[wght].ttf",
                              "MaterialSymbolsRounded-subset.ttf" }) {
        QFontDatabase::addApplicationFont(
            QStringLiteral(":/qt/qml/MatrixClient/data/fonts/")
            + QLatin1String(font));
    }
    QFont uiFont(QStringLiteral("Manrope"));
    uiFont.setPixelSize(14);
    QGuiApplication::setFont(uiFont);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    ComposerQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ComposerQmlTest.moc"
