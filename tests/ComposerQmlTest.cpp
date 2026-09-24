// Main composer structure and behaviour: one card with a formatting toolbar
// above the input row and a 1px divider, the control order, send and toolbar
// geometry, a borderless transparent input, a markdown formatting round-trip,
// and card pixels that track each theme's raised-surface token. Loads the
// production MessageComposerBar against the mock backend with no QML warnings.

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

// A real, decodable 2x2 PNG, so the chip's preview tile is exercised rather
// than the broken-image fallback.
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

    // Every match, not the first: a Repeater's delegates share an objectName
    // and live only in the visual item tree, which findChild cannot walk.
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

    // Read an int off the AppTheme singleton, so assertions pin the token
    // rather than a copy of its value.
    int themeInt(const char *token) const
    {
        // Evaluated in the scene's context: AppTheme comes from the
        // MatrixClient import, which only the loaded component's context has.
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

    // The mention popup is a Popup (a QObject, not a QQuickItem), found by
    // object name.
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
        QTest::qWait(60); // let the async member snapshot arrive and rebuild
    }

    // Tooltip probes. One ToolTip instance is shared by every control and its
    // x/y are relative to whichever control it is shown for, so every read
    // goes through the control and is only meaningful while it is showing.
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
    // ToolTip.delay is 500 ms here; 900 leaves room for layout polish.
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
        // Open the collapsible toolbar and let it lay out first.
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
        // Card geometry: 12px radius, 1px border, 20px side padding.
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
        // radiusControl, asserted against the token so the row cannot drift
        // from the control system through a literal.
        QCOMPARE(bold->property("radius").toInt(), themeInt("radiusControl"));
        QCOMPARE(bold->property("iconSize").toInt(), 18);
    }

    void inputRowHasExactControlOrder()
    {
        const char *order[] = {
            "composerAttachButton", "composerFormatToggleButton",
            "composerInput", "composerEmojiButton",
            // One button for GIFs and stickers; "send later" is a chevron
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

        // Collapsed by default: the toolbar takes no space.
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

    // The single GIF/sticker button is a glyph button like its neighbours:
    // same size, borderless, with a pressed state.
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
        // Open the toolbar so its raised surface is on screen.
        item("composerBar")->setProperty("toolbarExpanded", true);
        QTest::qWait(50);
        const int themes[] = { 9, 8, 10 }; // Indigo Night, Moss Light, Deep Teal
        for (int mode : themes) {
            m_root->setProperty("themeMode", mode);
            QCoreApplication::processEvents();
            const QImage img = m_window->grabWindow();
            QVERIFY(!img.isNull());
        // Sample the toolbar row's empty right side: card surface only.
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

    // Outgoing @-mentions. These run after the structural tests so they may
    // select a room; the mock seeds "!general:mock.local" with Alice/Bob/Carol.

    // @room must be offered while the room-info panel points at this room.
    // RoomInfoController::canNotifyRoom is false while the roster loads, after
    // clearSnapshot() and on backends that send no permission keys (like the
    // mock), so it must not gate the suggestion.
    void roomMentionSurvivesTheRoomInfoPanelPointingHere()
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *input = item("composerInput");
        QObject *model =
            m_controller->property("mentionSuggestions").value<QObject *>();
        QObject *info = m_controller->property("roomInfo").value<QObject *>();
        QVERIFY(input && model && info);

        // The panel points at the room being typed in.
        info->setProperty("roomId", QStringLiteral("!general:mock.local"));
        QTest::qWait(80);
        QCOMPARE(info->property("roomId").toString(),
                 QStringLiteral("!general:mock.local"));
        // The value an over-eager gate would consult really is false here.
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

        // Sent as a whole-room mention: the body keeps a literal "@room" with
        // no matrix.to link, and the id list carries the sentinel the Rust
        // bridge turns into m.mentions.room.
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

        // The input keeps focus while the popup is open.
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

        // Sending delivers the expanded matrix.to markdown body and records
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

        // Removing a character from inside the inserted name drops the ref,
        // so no mention id is sent.
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

    // Shift+Enter inserts a newline every time, and the composer grows with
    // the draft up to its scroll cap.
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

    // At the narrowest supported window the input stays wide enough to read.
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

    // A draft restored on room switch comes back intact with the caret at its
    // end.
    void restoredDraftKeepsTextAndPlacesCaretAtTheEnd()
    {
        // DraftStore only saves for a live session, so sign the mock in.
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        if (!mock->isLoggedIn()) {
            QSignalSpy spy(mock, &MatrixClient::loginSucceeded);
            mock->login(QStringLiteral("https://mock.local"),
                        QStringLiteral("alice"), QStringLiteral("x"));
            QVERIFY(spy.wait(4000));
            QTest::qWait(50);
        }
        // Persisted drafts are account-scoped; without an account record
        // SettingsManager writes nothing.
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
        // Leave no stored draft for the next case.
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

    // A dead-key composition (QInputMethodEvent preedit) must survive until
    // the next keystroke composes it. Rewriting the text, moving the cursor or
    // re-laying out the document cancels it, and the composer has three
    // per-keystroke candidates: the write-back to AppComposer, mention
    // re-anchoring and the MentionHighlighter (tested with a live mention).
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

        // The dead key: composing, nothing committed yet.
        sendIm(QStringLiteral("¨"), QString());
        QTest::qWait(30);
        QCOMPARE(input->property("preeditText").toString(),
                 QStringLiteral("¨"));
        QCOMPARE(input->property("text").toString(), QString());

        // The next keystroke composes it: one press, not two.
        sendIm(QString(), QStringLiteral("ä"));
        QTest::qWait(30);
        QCOMPARE(input->property("text").toString(), QStringLiteral("ä"));
        QVERIFY(input->property("preeditText").toString().isEmpty());
        QCOMPARE(m_controller->composer()->text(), QStringLiteral("ä"));

        // With a real mention the highlighter formats the same document.
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

        // Negative control: a write-back to the text does cancel a composition,
        // so the assertions above cannot pass vacuously.
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



    // The composer text sits on the icons' centre line. TextArea-in-Flickable
    // can park contentY at a small negative offset until clicked; it depends
    // on content height, so the harness loads the application's own fonts.
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
        // Where the placeholder is painted, not where its item sits: the offset
        // lives in the flickable's scroll position.
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

    // Settings > Appearance > Message box buttons hides buttons. Driven through
    // the real setting: hiddenComposerButtons notifies, which re-evaluates
    // each `visible` binding (a Q_INVOKABLE would not).
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
        // Measure, restore, then assert: all cases share one QSettings file, so
        // an assertion firing mid-loop would leave a button hidden for later
        // cases and runs.
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

    // GIFs and stickers are one window with two tabs: two picker components
    // that the host swaps in place (same anchor, same remembered size, no
    // transition). Driven through the host's entry points, since the swap is
    // the feature.
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

        // The inner strip asks the host to swap.
        QMetaObject::invokeMethod(bar, "swapMediaPicker",
                                  Q_ARG(QVariant, QStringLiteral("sticker")));
        QTest::qWait(80);
        QVERIFY2(opened("composerStickerPicker"),
                 "swapping to Stickers did not open the sticker panel");
        QVERIFY2(!opened("composerGifPicker"),
                 "swapping left the GIF panel open too: that is two windows, "
                 "which is exactly what this replaced");

        // Same anchor and size key, so it reads as one window.
        auto *gif = m_root->findChild<QObject *>(
            QStringLiteral("composerGifPicker"));
        auto *sticker = m_root->findChild<QObject *>(
            QStringLiteral("composerStickerPicker"));
        QVERIFY(gif && sticker);
        QCOMPARE(gif->property("anchorItem").value<QQuickItem *>(),
                 sticker->property("anchorItem").value<QQuickItem *>());
        QCOMPARE(gif->property("sizeSettingsKey").toString(),
                 sticker->property("sizeSettingsKey").toString());

        // The button toggles the pair: pressing it on the sticker half closes
        // the window.
        QMetaObject::invokeMethod(bar, "openMediaPicker",
                                  Q_ARG(QVariant, false));
        QTest::qWait(80);
        QVERIFY(!opened("composerStickerPicker"));
        QVERIFY(!opened("composerGifPicker"));

        m_controller->setCurrentRoomId(previousRoom);
        QTest::qWait(30);
    }

    // Send options are a chevron right of Send, not a glyph in the icon row.
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
        // Narrower than Send, same height: a split button.
        QCOMPARE(chevron->height(), send->height());
        QVERIFY(chevron->width() < send->width());

        for (const char *name : { "composerSendLaterMenuItem",
                                  "composerScheduledListMenuItem" }) {
            QVERIFY2(m_root->findChild<QObject *>(QLatin1String(name)), name);
        }
    }

    // Picker buttons toggle. The pickers close on press outside, and the
    // button's click arrives on release, reopening the panel. Needs a real
    // mouse click on the real button to reproduce the ordering.
    void aSecondPressOnAPickerButtonClosesItsPanel()
    {
        auto *bar = item("composerBar");
        QVERIFY(bar != nullptr);
        const QString previousRoom = m_controller->currentRoomId();
        // The icon buttons require an open room.
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QTest::qWait(30);

        // Pickers are Popups found by objectName; an `id` lives in
        // MessageComposerBar's own context, not the scene's.
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

        // A third press opens it again: the toggle must not latch shut.
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

    // Completion popups (mention, slash command, emoji shortcode) must clear
    // the whole composer card, not just the text field, or they cover the
    // formatting toolbar and the reply/thread banner.
    void aCompletionPopupClearsTheWholeComposerCard()
    {
        const int restoreHeight = m_window->height();
        const QString previousRoom = m_controller->currentRoomId();
        // Tall enough that the popup's bottom clamp is not the constraint
        // being measured.
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
        // The popup's parent is Overlay.overlay, so its y is in scene
        // coordinates.
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
        // ...and it stays attached: the gap is the popups' 4 px.
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

    // The `+` menu opens above the card, like its siblings, not at the
    // pointer over the composer. A real click: popup() also destroys the y
    // binding, so y must still follow after the click.
    void theAttachMenuOpensAboveTheCardNotOverIt()
    {
        const int restoreWidth = m_window->width();
        const QString previousRoom = m_controller->currentRoomId();
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        // Narrow, so the button opens the menu; a wide row with polls
        // unsupported goes straight to a native file dialog.
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
        // The anchor is a live binding: popup() would assign x/y and destroy
        // it, and offscreen a pointer-placed menu can land close enough to pass
        // a static check. Change the height and y must follow.
        menu->setProperty("height", h + 24.0);
        QTest::qWait(60);
        QVERIFY2(qAbs(menu->property("y").toReal() - (-(h + 24.0) - 4.0)) < 0.5,
                 qPrintable(QStringLiteral(
                     "the attach menu's y is no longer bound to its height "
                     "(y=%1 after height %2): something assigned it, and a "
                     "popup() at the button is what does that")
                        .arg(menu->property("y").toReal()).arg(h + 24.0)));
        // It stays inside the window.
        const qreal sceneBottom = card->mapToScene(QPointF(0, y + h)).y();
        QVERIFY2(sceneBottom <= m_window->height(),
                 "the attach menu runs off the bottom of the window");

        QMetaObject::invokeMethod(menu, "close");
        QTest::qWait(80);
        m_window->setWidth(restoreWidth);
        QTest::qWait(150);
        m_controller->setCurrentRoomId(previousRoom);
    }

    // Every attachment chip in the tray has the same height (image chips and
    // file chips differ in content), so the Flow has an even bottom edge.
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
        // Bottom edges are what the eye reads in a Flow.
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

    // In a narrow row the formatting toggle is offered from the overflow menu,
    // or an open toolbar could not be put away.
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
        // Menu rows report effective visibility, so open the menu first.
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
        // Clicked on the row with the menu open; emitting `triggered` directly
        // proves nothing about reachability.
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
        // It toggles: it opens the toolbar again.
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

    // The voice preview draws the waveform it is handed. Buckets are 0..100,
    // so bars must differ and a loud bucket must be taller than a quiet one;
    // clamping with Math.min(1, v) would draw a solid block.
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
        // No filePath, so nothing decodes or plays; the strip is drawn from
        // the buckets alone.
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

        // Cleared through the composer's own path: an empty file path makes
        // AppController::discardPreparedVoice refuse before touching the
        // recorder.
        QMetaObject::invokeMethod(bar, "discardPendingVoice");
        QTest::qWait(60);
        QVERIFY(!preview->isVisible());
        m_controller->setCurrentRoomId(previousRoom);
    }

    // The compact "…" menu must not elide its own rows; AppMenu's design
    // width is a floor. Asserted as implicit width against given width, so a
    // longer translation also fails here.
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
            // Measured even when hidden: the mock has no GIF provider, but a
            // real account shows the row.
            ++measured;
            QVERIFY2(row->implicitWidth() <= row->width() + 0.5,
                     qPrintable(QStringLiteral(
                         "%1 is elided: it needs %2 px and was given %3 "
                         "(menu width %4)")
                            .arg(QLatin1String(name))
                            .arg(row->implicitWidth()).arg(row->width())
                            .arg(menu->property("width").toReal())));
        }
        // Assert the count measured, so an all-invisible loop cannot pass.
        QCOMPARE(measured, 4);

        QMetaObject::invokeMethod(menu, "close");
        QTest::qWait(80);
        m_window->setWidth(restoreWidth);
        QTest::qWait(150);
        m_controller->setCurrentRoomId(previousRoom);
    }

    // The format toggle's tooltip must not be drawn over the toolbar it opens.
    // The Basic style draws a tip above its control and does not close it on
    // a press inside the control, and the card grows upward into it. Asserts
    // the tip exists while closed, that the toolbar overlaps its rectangle,
    // and that it is hidden once the toolbar is open.
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

    // A menu this bar opens (attach, send options) must not cover its own
    // button's tooltip: menus sit at `y: -height - 4` on the card and tips 3 px
    // above their control, so the tip would survive as a sliver under the menu.
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
        // Assert the count, so a hidden button cannot leave nothing asserted.
        QCOMPARE(measured, 2);

        m_window->setWidth(restoreWidth);
        QTest::qWait(150);
        m_controller->setCurrentRoomId(previousRoom);
    }

    // The mode chip (the formatting row's one worded chip) never paints outside
    // the card: the RowLayout will not shrink it below its implicit width, so
    // it moves into the overflow menu instead. `minWidth: 0` does not help:
    // AppButton's label does not elide. Both outcomes are counted.
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
        // Clicked on the row; emitting `triggered` proves nothing about it.
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
    // Match main.cpp: bundled fonts, Manrope as the application font and the
    // Basic style, so the harness measures the typeface the app renders.
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
