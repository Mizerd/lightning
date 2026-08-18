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

    // ── v0.7 outgoing @-mentions ─────────────────────────────────────────
    // These run after the structural tests so they may select a room. The
    // mock seeds "!general:mock.local" with Alice/Bob/Carol members.

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
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    ComposerQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ComposerQmlTest.moc"
