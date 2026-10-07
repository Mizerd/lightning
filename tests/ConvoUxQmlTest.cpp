// The 2026-10-07 conversation-area UX round ("I don't know how to set the
// custom background; UI and UX is bad"). Real components against the mock
// backend, with geometric and per-theme assertions where a source scan could
// not see the defect:
//
//  * Room information: the chat background is its own titled group directly
//    under the room's identity, above the edit-room and room-profile groups.
//  * The background editor's empty preview is the way in: it says there is no
//    background and a click chooses a picture; choosing is the primary action.
//  * The message hover bar offers Reply in thread (not inside the thread
//    panel), and still ends before the receipt facepile on a short row.
//  * The hovered row's tint moves the canvas by a visible but quiet amount on
//    EVERY preset, and reaction chips are outlined visibly on every preset.
//  * Call controls: the microphone leads the bar; device menus open against
//    their chevron (below it at the top of a window, above it at the bottom),
//    never at the pointer over the bar; Collapse no longer wears the pop-out
//    glyph.
//  * Opening room search gives its field the keyboard.

#include <QtTest/QtTest>

#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <cmath>
#include <memory>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "backdrop/ChatBackdropController.h"
#include "threads/ThreadController.h"

namespace {
constexpr int kSignalTimeoutMs = 5000;

QString readQml(const QString &name)
{
    QFile file(QStringLiteral(QML_DIR "/") + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                          : QString{};
}

// CIE L* of an opaque sRGB colour.
double lstar(const QColor &c)
{
    auto lin = [](double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    const double y = 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF())
                     + 0.0722 * lin(c.blueF());
    return y > 0.008856 ? 116.0 * std::cbrt(y) - 16.0 : 903.3 * y;
}

// `top` (possibly translucent) composited over opaque `base`.
QColor over(const QColor &top, const QColor &base)
{
    const double a = top.alphaF();
    return QColor::fromRgbF(top.redF() * a + base.redF() * (1 - a),
                            top.greenF() * a + base.greenF() * (1 - a),
                            top.blueF() * a + base.blueF() * (1 - a));
}
} // namespace

class ConvoUxQmlTest : public QObject
{
    Q_OBJECT

    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
    };

    static bool login(AppController &controller)
    {
        QSignalSpy spy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        return spy.wait(3000);
    }

    /// A complete role map for MessageDelegate (the shape
    /// MessageRailCollisionTest uses), so it binds without undefined reads.
    static QVariantMap baseFixture()
    {
        QVariantMap f;
        f.insert(QStringLiteral("isVirtual"), false);
        f.insert(QStringLiteral("isStateActivity"), false);
        f.insert(QStringLiteral("isCallEvent"), false);
        f.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        f.insert(QStringLiteral("showSenderIdentity"), true);
        f.insert(QStringLiteral("eventId"), QStringLiteral("$convo"));
        f.insert(QStringLiteral("itemId"), QStringLiteral("convo-item"));
        f.insert(QStringLiteral("sender"), QStringLiteral("@a:mock.local"));
        f.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Alice"));
        f.insert(QStringLiteral("senderInitials"), QStringLiteral("A"));
        f.insert(QStringLiteral("body"), QStringLiteral("ok"));
        f.insert(QStringLiteral("eventType"), 0);
        f.insert(QStringLiteral("status"), 0);
        f.insert(QStringLiteral("isOwn"), false);
        f.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc());
        f.insert(QStringLiteral("redacted"), false);
        f.insert(QStringLiteral("edited"), false);
        f.insert(QStringLiteral("isEncrypted"), false);
        f.insert(QStringLiteral("isDecrypted"), true);
        f.insert(QStringLiteral("undecryptable"), false);
        f.insert(QStringLiteral("errorKind"), QString{});
        f.insert(QStringLiteral("isImage"), false);
        f.insert(QStringLiteral("isFile"), false);
        f.insert(QStringLiteral("isVideo"), false);
        f.insert(QStringLiteral("isAudio"), false);
        f.insert(QStringLiteral("isSticker"), false);
        f.insert(QStringLiteral("mediaIsVoice"), false);
        f.insert(QStringLiteral("mediaDurationMs"), 0);
        f.insert(QStringLiteral("mediaWidth"), 0);
        f.insert(QStringLiteral("mediaHeight"), 0);
        f.insert(QStringLiteral("mediaSize"), 0);
        f.insert(QStringLiteral("mediaSourceAvailable"), false);
        f.insert(QStringLiteral("mediaMimetype"), QString{});
        f.insert(QStringLiteral("reactions"), QVariantList{});
        f.insert(QStringLiteral("replyToEventId"), QString{});
        f.insert(QStringLiteral("isThreadRoot"), false);
        f.insert(QStringLiteral("mentionsMe"), false);
        f.insert(QStringLiteral("mentionsRoom"), false);
        f.insert(QStringLiteral("isLocalEcho"), false);
        f.insert(QStringLiteral("readReceipts"), QVariantList{});
        f.insert(QStringLiteral("readReceiptsTotal"), 0);
        f.insert(QStringLiteral("sentReceipt"), false);
        return f;
    }

    static QVariantList receipts(int count)
    {
        QVariantList out;
        for (int i = 0; i < count; ++i) {
            QVariantMap r;
            r.insert(QStringLiteral("userId"),
                     QStringLiteral("@r%1:mock.local").arg(i));
            r.insert(QStringLiteral("displayName"),
                     QStringLiteral("Reader %1").arg(i));
            r.insert(QStringLiteral("initials"), QStringLiteral("R%1").arg(i));
            r.insert(QStringLiteral("avatarUrl"), QString{});
            out.append(r);
        }
        return out;
    }

    bool buildDelegate(AppController &controller, const QVariantMap &fixture,
                       Delegate &out)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        out.engine->rootContext()->setContextProperty("app", &controller);
        out.engine->rootContext()->setContextProperty("model", fixture);
        QSignalSpy created(out.engine.get(),
                           &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("MessageDelegate"));
        if (created.isEmpty() && !created.wait(kSignalTimeoutMs))
            return false;
        out.root = qobject_cast<QQuickItem *>(
            created.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(800, 480);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(640);
        out.window->show();
        for (int i = 0; i < 6; ++i)
            QCoreApplication::processEvents();
        return true;
    }

    static QQuickItem *forceActionBar(QQuickItem *root)
    {
        auto *loader = root->findChild<QQuickItem *>(
            QStringLiteral("messageActionBarLoader"));
        if (!loader)
            return nullptr;
        QQmlProperty::write(loader, QStringLiteral("active"), true);
        QQmlProperty::write(loader, QStringLiteral("visible"), true);
        QCoreApplication::processEvents();
        return loader;
    }

    // Every item with this objectName in the visual tree (Loader content and
    // Repeater delegates are not reachable through findChild).
    static void collect(QQuickItem *parent, const QString &name,
                        QList<QQuickItem *> &out)
    {
        if (!parent)
            return;
        if (parent->objectName() == name)
            out.append(parent);
        const auto children = parent->childItems();
        for (QQuickItem *child : children)
            collect(child, name, out);
    }
    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        QList<QQuickItem *> hits;
        collect(parent, name, hits);
        return hits.isEmpty() ? nullptr : hits.first();
    }

    static QRectF sceneRect(QQuickItem *it)
    {
        return QRectF(it->mapToScene(QPointF(0, 0)),
                      QSizeF(it->width(), it->height()));
    }
    // A Popup's x/y are in its `parent` item's coordinates.
    static QRectF popupSceneRect(QObject *popup)
    {
        auto *host = qobject_cast<QQuickItem *>(
            popup->property("parent").value<QObject *>());
        if (!host)
            return {};
        const QPointF origin = host->mapToScene(
            QPointF(popup->property("x").toReal(),
                    popup->property("y").toReal()));
        return QRectF(origin, QSizeF(popup->property("width").toReal(),
                                     popup->property("height").toReal()));
    }
    static QObject *menuOf(QObject *chevron)
    {
        const auto all = chevron->findChildren<QObject *>();
        for (QObject *o : all) {
            if (o->inherits("QQuickMenu"))
                return o;
        }
        return nullptr;
    }

    // A CallHeaderBar in preview mode, pinned to the top or bottom edge of a
    // tall window.
    std::unique_ptr<QObject> callBarScene(QQmlEngine &engine, bool atBottom)
    {
        QQmlComponent component(&engine);
        const QByteArray anchor = atBottom ? "anchors.bottom: parent.bottom"
                                           : "anchors.top: parent.top";
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 1200
    height: 720
    visible: true
    property alias bar: b
    CallHeaderBar {
        id: b
        anchors.left: parent.left
        anchors.right: parent.right
        )") + anchor + QByteArrayLiteral(R"(
        previewMode: true
    }
}
)"), QUrl(QStringLiteral("qrc:/convo-callbar.qml")));
        if (!component.errors().isEmpty()) {
            qWarning("%s", qPrintable(component.errorString()));
            return {};
        }
        return std::unique_ptr<QObject>(component.create());
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("convo-ux-qml-test"));
    }

    // Fails on the old layout: there was no titled group, and the bare editor
    // sat after "Your profile in this room", the fifth group on the page.
    void chatBackgroundIsATitledGroupDirectlyUnderTheRoomIdentity()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("RoomInfoPanel"));
        if (created.isEmpty())
            QVERIFY(created.wait(8000));
        QObject *root = created.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);

        auto *section = root->findChild<QQuickItem *>(
            QStringLiteral("roomChatBackgroundSection"));
        QVERIFY2(section, "Room information has no chat background group");
        auto *title = section->findChild<QQuickItem *>(
            QStringLiteral("roomChatBackgroundTitle"));
        QVERIFY2(title, "the chat background group has no heading");
        QCOMPARE(title->property("text").toString(),
                 QStringLiteral("Chat background"));
        auto *editor = root->findChild<QQuickItem *>(
            QStringLiteral("roomChatBackgroundEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->parentItem(), section);
        // The group carries the title, so the editor's own is off.
        QCOMPARE(editor->property("showTitle").toBool(), false);

        auto *editRoom = root->findChild<QQuickItem *>(
            QStringLiteral("roomEditSection"));
        auto *profile = root->findChild<QQuickItem *>(
            QStringLiteral("roomProfileSection"));
        QVERIFY(editRoom && profile);
        QQuickItem *column = section->parentItem();
        QVERIFY(column);
        QCOMPARE(editRoom->parentItem(), column);
        QCOMPARE(profile->parentItem(), column);
        const auto order = column->childItems();
        const int at = order.indexOf(section);
        QVERIFY2(at >= 0 && at < order.indexOf(editRoom)
                     && at < order.indexOf(profile),
                 "the chat background group is below the edit-room or "
                 "room-profile group again");
    }

    // Fails on the old editor: the empty preview said "No background" in a
    // box that did nothing, and Choose was a secondary button below the
    // privacy text.
    void anEmptyBackgroundPreviewIsTheWayToChooseOne()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        QVERIFY2(controller.backdrops(), "no backdrop controller on mock");

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 520
    height: 600
    visible: true
    property alias editor: e
    ChatBackgroundEditor {
        id: e
        width: 480
        scopeKind: "default"
        showTitle: false
    }
}
)"), QUrl(QStringLiteral("qrc:/convo-bgeditor.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner);
        auto *editor = owner->property("editor").value<QQuickItem *>();
        QVERIFY(editor);
        QCoreApplication::processEvents();

        // The fixture must really be empty, or this case proves nothing.
        QCOMPARE(editor->property("hasCurrent").toBool(), false);
        QCOMPARE(editor->property("hasPrepared").toBool(), false);

        auto *empty = findItem(editor, QStringLiteral("chatBackgroundEmptyState"));
        QVERIFY2(empty, "the preview has no empty state");
        QVERIFY(empty->isVisible());
        auto *preview = findItem(editor, QStringLiteral("chatBackgroundPreview"));
        QVERIFY(preview);
        QVERIFY2(preview->property("pickable").toBool(),
                 "an empty, editable preview must choose a picture on click");
        QVERIFY2(preview->property("activeFocusOnTab").toBool(),
                 "the preview's click target must be reachable by keyboard");

        auto *choose = findItem(editor, QStringLiteral("chatBackgroundChoose"));
        QVERIFY(choose);
        QCOMPARE(choose->property("kind").toString(), QStringLiteral("primary"));
        // At rest with nothing set: no Apply, no Cancel, no Remove.
        for (const char *name : { "chatBackgroundApply", "chatBackgroundCancel",
                                  "chatBackgroundRemove" }) {
            auto *b = findItem(editor, QLatin1String(name));
            QVERIFY2(b, name);
            QVERIFY2(!b->isVisible(), name);
        }
    }

    // app.backdrops.lastError is one value for the app, and several editors
    // can be alive at once. An error from an action in one (the room header's
    // dialog) showed in the other (Settings) though nobody touched it. Fails
    // on the old editor, which showed lastError in every instance.
    void aBackgroundErrorShowsOnlyInTheEditorThatActed()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        QObject *backdrops = controller.backdrops();
        QVERIFY(backdrops);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 520
    height: 900
    visible: true
    property alias idle: a
    property alias acting: b
    Column {
        width: 480
        ChatBackgroundEditor { id: a; width: 480; scopeKind: "default" }
        ChatBackgroundEditor { id: b; width: 480; scopeKind: "default" }
    }
}
)"), QUrl(QStringLiteral("qrc:/convo-bgerror.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner);
        auto *idle = owner->property("idle").value<QQuickItem *>();
        auto *acting = owner->property("acting").value<QQuickItem *>();
        QVERIFY(idle && acting);

        // An action in `acting` only. Nothing is prepared, so the controller
        // refuses and records an error.
        QVERIFY(acting->setProperty("actedHere", true));
        bool ok = true;
        QMetaObject::invokeMethod(backdrops, "setPersonal",
                                  Q_RETURN_ARG(bool, ok),
                                  Q_ARG(QString, QString()),
                                  Q_ARG(QVariantMap, QVariantMap{}));
        QCoreApplication::processEvents();
        QVERIFY2(!backdrops->property("lastError").toString().isEmpty(),
                 "setPersonal with nothing prepared set no error, so this case "
                 "proves nothing");

        auto errorOf = [](QQuickItem *editor) {
            auto *label = findItem(editor, QStringLiteral("chatBackgroundError"));
            return label ? label->property("text").toString() : QStringLiteral("?");
        };
        QVERIFY2(!errorOf(acting).isEmpty() && errorOf(acting) != QStringLiteral("?"),
                 "the editor that acted does not show the error");
        QVERIFY2(errorOf(idle).isEmpty(),
                 qPrintable(QStringLiteral("an untouched editor shows \"%1\"")
                                .arg(errorOf(idle))));
    }

    // Fails on the old bar: Reply in thread was in the overflow menu only.
    void theHoverBarOffersReplyInThread()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(controller.thread());
        QVERIFY2(controller.thread()->property("supported").toBool(),
                 "threads are unsupported on mock, so this case is vacuous");

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(4));
        f.insert(QStringLiteral("readReceiptsTotal"), 4);
        Delegate d;
        QVERIFY(buildDelegate(controller, f, d));
        QQuickItem *bar = forceActionBar(d.root);
        QVERIFY(bar);
        auto *thread = findItem(bar, QStringLiteral("messageThreadReplyButton"));
        QVERIFY2(thread, "the hover bar has no Reply in thread button");
        QVERIFY(thread->isVisible());
        QVERIFY(thread->isEnabled());

        // The wider bar still ends before the receipt facepile (the shared
        // right rail, CLAUDE.md §16) and inside the row.
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(pile && pile->width() > 0);
        const qreal barRight =
            bar->mapToItem(d.root, QPointF(bar->width(), 0)).x();
        const qreal pileLeft = pile->mapToItem(d.root, QPointF(0, 0)).x();
        QVERIFY2(barRight <= pileLeft + 1.0,
                 qPrintable(QStringLiteral("bar ends at %1, facepile starts "
                                           "at %2").arg(barRight).arg(pileLeft)));
        QVERIFY(bar->mapToItem(d.root, QPointF(0, 0)).x() >= 0);
        // Every button of the bar sits inside the bar.
        const QRectF barRect = sceneRect(bar);
        const QRectF threadRect = sceneRect(thread);
        QVERIFY(barRect.adjusted(-0.5, -0.5, 0.5, 0.5).contains(threadRect));
    }

    // The thread panel's own rows: its composer already replies in the
    // thread, so the button would only reopen the same thread.
    void theThreadPanelsRowsDoNotOfferIt()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(buildDelegate(controller, baseFixture(), d));
        QQmlComponent view(d.engine.get());
        view.setData(QByteArrayLiteral(
                         "import QtQuick\nQtObject { property bool threadContext: true }"),
                     QUrl(QStringLiteral("qrc:/convo-threadview.qml")));
        std::unique_ptr<QObject> fakeView(view.create());
        QVERIFY(fakeView);
        QQmlProperty::write(d.root, QStringLiteral("timelineView"),
                            QVariant::fromValue(fakeView.get()));
        QCoreApplication::processEvents();
        QVERIFY(d.root->property("inThreadPanel").toBool());
        QQuickItem *bar = forceActionBar(d.root);
        QVERIFY(bar);
        auto *thread = findItem(bar, QStringLiteral("messageThreadReplyButton"));
        QVERIFY(thread);
        QVERIFY(!thread->isVisible());
        QQmlProperty::write(d.root, QStringLiteral("timelineView"), QVariant());
    }

    // A hovered row must be visible and quiet on EVERY preset. Fails on the
    // old tint (AppTheme.hover): +0.4 L* on Light and Moss Light, +1.0 on
    // Warm, and 15-27 L* on the dark presets.
    void theHoveredRowIsVisibleAndQuietOnEveryPreset()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(buildDelegate(controller, baseFixture(), d));

        QSet<QRgb> backgrounds;
        int checked = 0;
        for (int mode = 1; mode <= 11; ++mode) {
            QQmlExpression set(qmlContext(d.root), d.root,
                               QStringLiteral("AppTheme.mode = %1").arg(mode));
            set.evaluate();
            QVERIFY2(!set.hasError(), qPrintable(set.error().toString()));
            QCoreApplication::processEvents();
            QQmlExpression bgExpr(qmlContext(d.root), d.root,
                                  QStringLiteral("AppTheme.background"));
            const QColor bg = bgExpr.evaluate().value<QColor>();
            QVERIFY(bg.isValid());
            backgrounds.insert(bg.rgb());

            const QVariant tintValue = d.root->property("rowHoverTint");
            QVERIFY2(tintValue.isValid(),
                     "MessageDelegate has no rowHoverTint: the row hover is "
                     "the control token again");
            const QColor tint = tintValue.value<QColor>();
            const double delta = std::abs(lstar(over(tint, bg)) - lstar(bg));
            QVERIFY2(delta >= 2.5 && delta <= 9.0,
                     qPrintable(QStringLiteral(
                         "theme %1: a hovered row moves the canvas by %2 L* "
                         "(want 2.5-9: visible, never a slab)")
                                    .arg(mode).arg(delta, 0, 'f', 1)));
            ++checked;
        }
        QCOMPARE(checked, 11);
        // Prove the palettes really changed (see CLAUDE.md §16, "the suite that
        // certified eleven themes was measuring one").
        QVERIFY2(backgrounds.size() >= 9,
                 qPrintable(QStringLiteral("only %1 distinct backgrounds")
                                .arg(backgrounds.size())));
        QQmlExpression reset(qmlContext(d.root), d.root,
                             QStringLiteral("AppTheme.mode = 0"));
        reset.evaluate();
    }

    // Fails on the old chip: at rest its outline was AppTheme.border, within
    // 1 L* of Moss Light's canvas, with a fill as close.
    void aReactionChipIsOutlinedOnEveryPreset()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap f = baseFixture();
        QVariantMap r;
        r.insert(QStringLiteral("key"), QStringLiteral("x"));
        r.insert(QStringLiteral("count"), 2);
        r.insert(QStringLiteral("byMe"), false);
        r.insert(QStringLiteral("senders"),
                 QVariantList{QStringLiteral("@r:mock.local")});
        r.insert(QStringLiteral("names"), QVariantList{QStringLiteral("R")});
        f.insert(QStringLiteral("reactions"), QVariantList{r});
        Delegate d;
        QVERIFY(buildDelegate(controller, f, d));
        QQuickItem *chip = findItem(d.root, QStringLiteral("reactionChip"));
        QVERIFY2(chip, "no reaction chip was built");

        QSet<QRgb> backgrounds;
        int checked = 0;
        for (int mode = 1; mode <= 11; ++mode) {
            QQmlExpression set(qmlContext(d.root), d.root,
                               QStringLiteral("AppTheme.mode = %1").arg(mode));
            set.evaluate();
            QCoreApplication::processEvents();
            QQmlExpression bgExpr(qmlContext(d.root), d.root,
                                  QStringLiteral("AppTheme.background"));
            const QColor bg = bgExpr.evaluate().value<QColor>();
            backgrounds.insert(bg.rgb());
            QObject *border = chip->property("border").value<QObject *>();
            QVERIFY(border);
            const QColor ink = border->property("color").value<QColor>();
            const double delta =
                std::abs(lstar(over(ink, bg)) - lstar(bg));
            QVERIFY2(delta >= 10.0,
                     qPrintable(QStringLiteral(
                         "theme %1: the chip outline is %2 L* off the canvas "
                         "(want >= 10, or the chip reads as bare text)")
                                    .arg(mode).arg(delta, 0, 'f', 1)));
            ++checked;
        }
        QCOMPARE(checked, 11);
        QVERIFY(backgrounds.size() >= 9);
        QQmlExpression reset(qmlContext(d.root), d.root,
                             QStringLiteral("AppTheme.mode = 0"));
        reset.evaluate();
    }

    // Fails on the old bar: camera, share, hand, reactions, people and pop-out
    // all came before the microphone.
    void theMicrophoneLeadsTheCallBar()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        auto owner = callBarScene(engine, false);
        QVERIFY(owner);
        auto *bar = owner->property("bar").value<QQuickItem *>();
        QVERIFY(bar);
        auto x = [bar](const char *name) -> qreal {
            auto *it = bar->findChild<QQuickItem *>(QLatin1String(name));
            return it && it->isVisible() ? it->mapToScene(QPointF(0, 0)).x()
                                         : -1.0;
        };
        QTRY_VERIFY_WITH_TIMEOUT(x("callBarCameraButton") >= 0, kSignalTimeoutMs);
        const qreal mic = x("callBarMicButton");
        const qreal deafen = x("callBarDeafenButton");
        const qreal camera = x("callBarCameraButton");
        const qreal share = x("callBarScreenShareButton");
        const qreal leave = x("callBarHangUpButton");
        QVERIFY(mic >= 0 && deafen >= 0 && share >= 0 && leave >= 0);
        QVERIFY2(mic < deafen && deafen < camera && camera < share
                     && share < leave,
                 qPrintable(QStringLiteral("mic %1, deafen %2, camera %3, "
                                           "share %4, leave %5")
                                .arg(mic).arg(deafen).arg(camera).arg(share)
                                .arg(leave)));
    }

    // Fails on the old chevron: popup() with no position opened the menu at
    // the pointer, over the bar (Leave included).
    void aDeviceMenuOpensAgainstItsChevron()
    {
        AppController controller(AppController::MockBackend);
        for (const bool atBottom : { false, true }) {
            QQmlApplicationEngine engine;
            engine.rootContext()->setContextProperty("app", &controller);
            auto owner = callBarScene(engine, atBottom);
            QVERIFY(owner);
            auto *window = qobject_cast<QQuickWindow *>(owner.get());
            QVERIFY(window);
            QVERIFY(QTest::qWaitForWindowExposed(window));
            auto *bar = owner->property("bar").value<QQuickItem *>();
            QVERIFY(bar);
            auto *chevron = bar->findChild<QQuickItem *>(
                QStringLiteral("callBarMicChevron"));
            QVERIFY(chevron);
            QTRY_VERIFY_WITH_TIMEOUT(chevron->isVisible()
                                         && chevron->width() > 0,
                                     kSignalTimeoutMs);
            QObject *menu = menuOf(chevron);
            QVERIFY2(menu, "the chevron has no device menu");

            QVERIFY(QMetaObject::invokeMethod(chevron, "click"));
            QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(),
                                     kSignalTimeoutMs);
            const QRectF c = sceneRect(chevron);
            QRectF m;
            if (!atBottom) {
                QTRY_VERIFY_WITH_TIMEOUT(
                    (m = popupSceneRect(menu)).top() >= c.bottom() - 1.0,
                    kSignalTimeoutMs);
            } else {
                QTRY_VERIFY2_WITH_TIMEOUT(
                    (m = popupSceneRect(menu)).bottom() <= c.top() + 1.0,
                    qPrintable(QStringLiteral("chevron y %1-%2, menu y %3-%4, window h %5")
                                   .arg(c.top()).arg(c.bottom())
                                   .arg(m.top()).arg(m.bottom())
                                   .arg(window->height())),
                    kSignalTimeoutMs);
            }
            // And beside it, not across the window.
            QVERIFY2(m.left() <= c.right() + 1.0
                         && m.right() >= c.left() - 1.0,
                     qPrintable(QStringLiteral("chevron x %1-%2, menu x %3-%4")
                                    .arg(c.left()).arg(c.right())
                                    .arg(m.left()).arg(m.right())));
            // Never over the bar's Leave button.
            auto *leave = bar->findChild<QQuickItem *>(
                QStringLiteral("callBarHangUpButton"));
            QVERIFY(leave);
            QVERIFY(!m.intersects(sceneRect(leave)));
            QMetaObject::invokeMethod(menu, "close");
            QCoreApplication::processEvents();
        }
    }

    // Collapse (the stage folding into a strip) and pop-out (a floating
    // window) sat side by side wearing the same close_fullscreen glyph. Fails
    // on the old source.
    void collapseAndPopOutDoNotShareAGlyph()
    {
        const QString stage = readQml(QStringLiteral("CallStage.qml"));
        const QString bar = readQml(QStringLiteral("CallHeaderBar.qml"));
        QVERIFY(!stage.isEmpty() && !bar.isEmpty());
        auto iconAfter = [](const QString &src, const QString &objectName) {
            const int at = src.indexOf(
                QStringLiteral("objectName: \"%1\"").arg(objectName));
            if (at < 0)
                return QStringList{};
            const int line = src.indexOf(QStringLiteral("iconName:"), at);
            const int end = src.indexOf(QLatin1Char('\n'), line);
            static const QRegularExpression quoted(
                QStringLiteral("\"([a-z_]+)\""));
            QStringList names;
            auto it = quoted.globalMatch(src.mid(line, end - line));
            while (it.hasNext())
                names << it.next().captured(1);
            return names;
        };
        const QStringList collapse =
            iconAfter(stage, QStringLiteral("callCollapseButton"));
        const QStringList popOut =
            iconAfter(bar, QStringLiteral("callBarPipButton"));
        QVERIFY2(!collapse.isEmpty() && !popOut.isEmpty(),
                 "could not read the two buttons' glyphs");
        for (const QString &name : collapse)
            QVERIFY2(!popOut.contains(name),
                     qPrintable(QStringLiteral("collapse and pop-out both "
                                               "use %1").arg(name)));
    }

    // Fails on the old panel: it opened with the keyboard elsewhere.
    void openingRoomSearchFocusesTheQuery()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(login(controller));
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 420
    height: 640
    visible: true
    property alias panel: p
    SearchPanel {
        id: p
        anchors.fill: parent
        visible: false
        historyAvailable: true
    }
}
)"), QUrl(QStringLiteral("qrc:/convo-search.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner);
        auto *window = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QCoreApplication::processEvents();
        auto *panel = owner->property("panel").value<QQuickItem *>();
        QVERIFY(panel);
        auto *field = findItem(panel, QStringLiteral("roomSearchField"));
        QVERIFY(field);
        QVERIFY(!field->hasActiveFocus());

        panel->setVisible(true);
        QTRY_VERIFY2_WITH_TIMEOUT(field->hasActiveFocus(),
                                  "the search field did not take the keyboard",
                                  kSignalTimeoutMs);
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(ConvoUxQmlTest)
#include "ConvoUxQmlTest.moc"
