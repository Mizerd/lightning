#include <QtTest/QtTest>

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <memory>

#include "ObjectCensus.h"
#include "app/AppController.h"
#include "app/SettingsManager.h"

namespace {
constexpr int kSignalTimeoutMs = 5000;
constexpr qreal kRowWidth = 640;
constexpr int kShow = 0;
constexpr int kOnHover = 1;
constexpr int kHide = 2;
}

// Settings are written through the property system so that this file builds
// against a tree without them, where every case must fail.
class TimelineVisibilityQmlTest : public QObject
{
    Q_OBJECT

    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
        QObject *view = nullptr;
    };

    static void set(AppController &app, const char *name, const QVariant &v)
    {
        app.settings()->setProperty(name, v);
        settle();
    }

    static QVariantMap baseFixture()
    {
        QVariantMap f;
        f.insert(QStringLiteral("isVirtual"), false);
        f.insert(QStringLiteral("isStateActivity"), false);
        f.insert(QStringLiteral("isCallEvent"), false);
        f.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        f.insert(QStringLiteral("showSenderIdentity"), true);
        f.insert(QStringLiteral("eventId"), QStringLiteral("$vis"));
        f.insert(QStringLiteral("itemId"), QStringLiteral("vis-item"));
        f.insert(QStringLiteral("sender"), QStringLiteral("@a:mock.local"));
        f.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Ada"));
        f.insert(QStringLiteral("senderInitials"), QStringLiteral("A"));
        f.insert(QStringLiteral("body"), QStringLiteral("hello there"));
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
        f.insert(QStringLiteral("roomMentionHighlighted"), false);
        f.insert(QStringLiteral("pushHighlighted"), false);
        f.insert(QStringLiteral("pushVerdictKnown"), false);
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
            r.insert(QStringLiteral("userId"), QStringLiteral("@r%1:mock.local").arg(i));
            r.insert(QStringLiteral("displayName"), QStringLiteral("Reader %1").arg(i));
            r.insert(QStringLiteral("avatarMxc"), QString{});
            out.append(r);
        }
        return out;
    }

    static QVariantList reactions(int count)
    {
        static const char *keys[] = {"\xf0\x9f\x91\x8d", "\xf0\x9f\x8e\x89",
                                     "\xf0\x9f\x94\xa5"};
        QVariantList out;
        for (int i = 0; i < count; ++i) {
            QVariantMap r;
            r.insert(QStringLiteral("key"), QString::fromUtf8(keys[i % 3]));
            r.insert(QStringLiteral("count"), i + 1);
            r.insert(QStringLiteral("byMe"), false);
            r.insert(QStringLiteral("senders"), QVariantList{QStringLiteral("@r:mock.local")});
            r.insert(QStringLiteral("names"), QVariantList{QStringLiteral("Reader")});
            out.append(r);
        }
        return out;
    }

    static QObject *viewStandIn(QQmlEngine *engine, QObject *owner, bool direct)
    {
        QQmlComponent component(engine);
        component.setData(R"QML(
import QtQuick
QtObject {
    property bool roomEncrypted: false
    property bool isDirectRoom: false
    property real contentY: 0
    property real height: 10000
    property bool speculativeMediaAllowed: true
    property bool stickToBottom: false
    property bool threadContext: false
    property string transientInteractionOwner: ""
    property string hoveredActionsKey: ""
    property string pinnedActionsKey: ""
    property string keyboardSelectedEventId: ""
    property bool rowHoverSuspended: false
    function stateGroupExpanded(groupId) { return false }
    function toggleStateGroup(groupId) {}
}
)QML",
                          QUrl());
        QObject *view = component.create();
        if (view) {
            view->setParent(owner);
            view->setProperty("isDirectRoom", direct);
        }
        return view;
    }

    bool build(AppController &app, const QVariantMap &fixture, Delegate &out,
               bool direct = false)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        out.engine->rootContext()->setContextProperty("app", &app);
        out.engine->rootContext()->setContextProperty("model", fixture);
        QSignalSpy created(out.engine.get(), &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("MessageDelegate"));
        if (created.isEmpty() && !created.wait(kSignalTimeoutMs))
            return false;
        out.root = qobject_cast<QQuickItem *>(created.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(800, 480);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(kRowWidth);
        out.view = viewStandIn(out.engine.get(), out.root, direct);
        if (!out.view)
            return false;
        out.root->setProperty("timelineView", QVariant::fromValue(out.view));
        QQmlProperty::write(out.root, QStringLiteral("isDirectRoom"), direct);
        out.window->show();
        settle();
        // Qt drops a move to the position it already has, and the last case
        // left the pointer at the parking spot: arrive there from elsewhere.
        QTest::mouseMove(out.window.get(), QPoint(700, 300));
        QTest::mouseMove(out.window.get(), QPoint(790, 470));
        settle();
        return true;
    }

    static void settle()
    {
        for (int i = 0; i < 6; ++i) {
            QCoreApplication::processEvents();
            QTest::qWait(20);
        }
        // A grab polishes, so layouts and positioners report settled sizes.
        for (QWindow *w : QGuiApplication::topLevelWindows()) {
            auto *quick = qobject_cast<QQuickWindow *>(w);
            if (quick && quick->isVisible())
                (void)quick->grabWindow();
        }
        QCoreApplication::processEvents();
    }

    static QString diag(const Delegate &d)
    {
        auto num = [](QQuickItem *i, const char *p) {
            return i ? QString::number(i->property(p).toReal()) : QStringLiteral("-");
        };
        auto *row = find(d, "messagePresentationRow");
        auto *column = find(d, "messageContentColumn");
        auto *body = find(d, "messageBody");
        auto *flow = find(d, "reactionsFlow");
        auto *hover = d.root->findChild<QObject *>(QStringLiteral("timelineRevealHover"));
        return QStringLiteral("implicit=%1 natural=%2 seed=%3 row=%4/%5 column=%6 body=%7 "
                              "flow=%8/%9 revealed=%10 hovered=%11 kb=%12 pinned=%13 menu=%14 "
                              "suspended=%15 window=%16")
            .arg(d.root->implicitHeight())
            .arg(d.root->property("naturalImplicitHeight").toReal())
            .arg(d.root->property("heightSeedActive").toBool())
            .arg(num(row, "implicitHeight"), num(row, "height"), num(column, "implicitHeight"),
                 num(body, "implicitHeight"), num(flow, "implicitHeight"),
                 num(flow, "height"))
            .arg(d.root->property("hoverRevealed").toBool())
            .arg(hover ? hover->property("hovered").toBool() : false)
            .arg(d.root->property("keyboardSelected").toBool())
            .arg(d.root->property("actionsPinned").toBool())
            .arg(d.root->property("moreMenuOpen").toBool())
            .arg(d.root->property("rowHoverSuspended").toBool())
            .arg(d.window->isExposed());
    }

    static QQuickItem *find(const Delegate &d, const char *name)
    {
        return d.root->findChild<QQuickItem *>(QLatin1String(name));
    }

    static int countNamed(const QSet<QObject *> &objects, const QString &name)
    {
        int n = 0;
        for (QObject *o : objects)
            if (o->objectName() == name)
                ++n;
        return n;
    }

    static qreal opacityUpTo(QQuickItem *item, QQuickItem *ground)
    {
        qreal o = 1.0;
        for (QQuickItem *it = item; it && it != ground; it = it->parentItem())
            o *= it->opacity();
        return o;
    }

    static qreal contentX(const Delegate &d)
    {
        auto *column = find(d, "messageContentColumn");
        return column ? column->mapToItem(d.root, QPointF(0, 0)).x() : -1;
    }

    // Every built part of the message lies inside the row's own height.
    static bool contentFitsTheRow(const Delegate &d, QString &why)
    {
        auto *column = find(d, "messageContentColumn");
        if (!column) {
            why = QStringLiteral("no content column");
            return false;
        }
        const qreal bottom = column->mapToItem(d.root, QPointF(0, column->height())).y();
        if (bottom > d.root->implicitHeight() + 0.5) {
            why = QStringLiteral("content ends at %1 in a %2px row")
                      .arg(bottom).arg(d.root->implicitHeight());
            return false;
        }
        if (auto *flow = find(d, "reactionsFlow")) {
            const qreal flowBottom =
                flow->mapToItem(d.root, QPointF(0, flow->height())).y();
            if (flowBottom > d.root->implicitHeight() + 0.5) {
                why = QStringLiteral("reactions end at %1 in a %2px row")
                          .arg(flowBottom).arg(d.root->implicitHeight());
                return false;
            }
        }
        return true;
    }

    void hoverRow(Delegate &d)
    {
        QTest::mouseMove(d.window.get(), QPoint(700, 300));
        QTest::mouseMove(d.window.get(), QPoint(int(kRowWidth / 2), 12));
    }
    void leaveRow(Delegate &d)
    {
        QTest::mouseMove(d.window.get(), QPoint(700, 300));
        QTest::mouseMove(d.window.get(), QPoint(790, 470));
    }

    QTemporaryDir m_configHome;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("timeline-visibility-qml-test"));
        for (const char *font : { "JetBrainsMono[wght].ttf", "Inter[wght].ttf",
                                  "Manrope[wght].ttf",
                                  "MaterialSymbolsRounded-subset.ttf" })
            QFontDatabase::addApplicationFont(
                QStringLiteral(":/qt/qml/MatrixClient/data/fonts/") + QLatin1String(font));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void everythingShowsByDefault()
    {
        SettingsManager settings;
        for (const char *name : {"timelineAvatars", "timelineReactions",
                                 "timelineReadReceipts"}) {
            const QVariant v = settings.property(name);
            QVERIFY2(v.isValid(), name);
            QCOMPARE(v.toInt(), kShow);
        }
        const QVariant big = settings.property("largeEmoji");
        QVERIFY(big.isValid());
        QCOMPARE(big.toBool(), true);
    }

    void theChoicesPersist()
    {
        {
            SettingsManager settings;
            settings.setProperty("timelineAvatars", kHide);
            settings.setProperty("timelineReactions", kOnHover);
            settings.setProperty("timelineReadReceipts", kHide);
            settings.setProperty("largeEmoji", false);
        }
        SettingsManager reopened;
        QCOMPARE(reopened.property("timelineAvatars").toInt(), kHide);
        QCOMPARE(reopened.property("timelineReactions").toInt(), kOnHover);
        QCOMPARE(reopened.property("timelineReadReceipts").toInt(), kHide);
        QVERIFY(reopened.property("largeEmoji").isValid());
        QCOMPARE(reopened.property("largeEmoji").toBool(), false);
    }

    // A mode written by a newer build, or garbage, means the default — not the
    // nearest end of the range, which would be Hide.
    void anUnknownModeFallsBackToShow()
    {
        for (const QVariant &stored : {QVariant(7), QVariant(-1), QVariant(3),
                                       QVariant(QStringLiteral("banana"))}) {
            {
                QSettings raw;
                raw.setValue(QStringLiteral("timeline/avatars"), stored);
                raw.setValue(QStringLiteral("timeline/reactions"), stored);
                raw.setValue(QStringLiteral("timeline/readReceipts"), stored);
                raw.sync();
            }
            SettingsManager settings;
            for (const char *name : {"timelineAvatars", "timelineReactions",
                                     "timelineReadReceipts"}) {
                const QVariant v = settings.property(name);
                QVERIFY2(v.isValid(), name);
                QVERIFY2(v.toInt() == kShow,
                         qPrintable(QStringLiteral("%1 read stored %2 as %3")
                                        .arg(QLatin1String(name), stored.toString())
                                        .arg(v.toInt())));
            }
        }

        SettingsManager settings;
        settings.setProperty("timelineReactions", kHide);
        settings.setProperty("timelineReactions", 5);
        QCOMPARE(settings.property("timelineReactions").toInt(), kShow);
    }

    void hiddenAvatarsCollapseTheGutter_data()
    {
        QTest::addColumn<int>("layout");
        QTest::addColumn<bool>("direct");
        QTest::addColumn<qreal>("shownX");
        QTest::addColumn<qreal>("hiddenX");
        QTest::addColumn<QString>("avatarName");
        QTest::newRow("classic") << 0 << false << qreal(40) << qreal(8)
                                 << QStringLiteral("senderAvatar");
        QTest::newRow("bubbles") << 1 << true << qreal(44) << qreal(8)
                                 << QStringLiteral("senderAvatar");
        QTest::newRow("compact") << 2 << false << qreal(8) << qreal(8)
                                 << QString();
        QTest::newRow("small-avatars") << 3 << false << qreal(8) << qreal(8)
                                       << QStringLiteral("senderHeaderAvatar");
    }

    void hiddenAvatarsCollapseTheGutter()
    {
        QFETCH(int, layout);
        QFETCH(bool, direct);
        QFETCH(qreal, shownX);
        QFETCH(qreal, hiddenX);
        QFETCH(QString, avatarName);
        AppController app(AppController::MockBackend);
        app.settings()->setMessageLayout(layout);

        Delegate shown;
        QVERIFY(build(app, baseFixture(), shown, direct));
        QCOMPARE(contentX(shown), shownX);
        if (!avatarName.isEmpty())
            QVERIFY2(shown.root->findChild<QQuickItem *>(avatarName),
                     "the avatar is not built in Show");
        auto *shownName = find(shown, "senderName");
        QVERIFY(shownName && shownName->isVisible());
        const qreal shownNameX = shownName->mapToItem(shown.root, QPointF(0, 0)).x();
        const qreal shownHeight = shown.root->implicitHeight();

        set(app, "timelineAvatars", kHide);
        Delegate hidden;
        QVERIFY(build(app, baseFixture(), hidden, direct));
        QCOMPARE(contentX(hidden), hiddenX);
        QVERIFY2(!hidden.root->findChild<QQuickItem *>(QStringLiteral("senderAvatar"))
                     && !hidden.root->findChild<QQuickItem *>(
                         QStringLiteral("senderHeaderAvatar")),
                 "an avatar is still built while avatars are hidden");
        auto *name = find(hidden, "senderName");
        QVERIFY2(name && name->isVisible(), "hiding avatars hid the sender name");
        const qreal nameX = name->mapToItem(hidden.root, QPointF(0, 0)).x();
        if (layout == 3)
            QVERIFY2(nameX < shownNameX,
                     qPrintable(QStringLiteral("the name stayed at %1 where the "
                                               "header avatar was").arg(nameX)));
        else
            QCOMPARE(nameX - hiddenX, shownNameX - shownX);
        QString why;
        QVERIFY2(contentFitsTheRow(hidden, why), qPrintable(why));
        QVERIFY(hidden.root->implicitHeight() <= shownHeight);

        // Toggled live, the built row lands on the height of a fresh one.
        set(app, "timelineAvatars", kShow);
        Delegate toggled;
        QVERIFY(build(app, baseFixture(), toggled, direct));
        set(app, "timelineAvatars", kHide);
        settle();
        QCOMPARE(contentX(toggled), hiddenX);
        QCOMPARE(toggled.root->implicitHeight(), hidden.root->implicitHeight());
        set(app, "timelineAvatars", kShow);
        settle();
        QCOMPARE(contentX(toggled), shownX);
        QCOMPARE(toggled.root->implicitHeight(), shownHeight);
    }

    void avatarsOnHoverKeepTheGutterAndAppearOnHover()
    {
        AppController app(AppController::MockBackend);
        app.settings()->setMessageLayout(0);
        set(app, "timelineAvatars", kOnHover);
        Delegate d;
        QVERIFY(build(app, baseFixture(), d));
        QCOMPARE(contentX(d), qreal(40));
        auto *avatar = find(d, "senderAvatar");
        QVERIFY2(avatar, "On hover must build the avatar so hovering builds nothing");
        const qreal height0 = d.root->implicitHeight();
        leaveRow(d);
        QTRY_VERIFY2(opacityUpTo(avatar, d.root) == 0.0, qPrintable(diag(d)));

        hoverRow(d);
        QTRY_COMPARE(opacityUpTo(avatar, d.root), 1.0);
        QCOMPARE(d.root->implicitHeight(), height0);
        QCOMPARE(find(d, "senderAvatar"), avatar);
        leaveRow(d);
        QTRY_COMPARE(opacityUpTo(avatar, d.root), 0.0);

        d.view->setProperty("keyboardSelectedEventId", QStringLiteral("$vis"));
        QTRY_COMPARE(opacityUpTo(avatar, d.root), 1.0);
    }

    void hiddenReactionsBuildNothing_data()
    {
        QTest::addColumn<int>("layout");
        QTest::addColumn<bool>("direct");
        QTest::addColumn<bool>("own");
        QTest::newRow("classic") << 0 << false << false;
        QTest::newRow("bubbles-other") << 1 << true << false;
        QTest::newRow("bubbles-own") << 1 << true << true;
        QTest::newRow("compact") << 2 << false << false;
    }

    void hiddenReactionsBuildNothing()
    {
        QFETCH(int, layout);
        QFETCH(bool, direct);
        QFETCH(bool, own);
        AppController app(AppController::MockBackend);
        app.settings()->setMessageLayout(layout);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), own);
        f.insert(QStringLiteral("reactions"), reactions(3));

        Delegate shown;
        QVERIFY(build(app, f, shown, direct));
        const auto shownObjects = census::objectsUnder(shown.root);
        QCOMPARE(countNamed(shownObjects, QStringLiteral("reactionChip")), 3);
        QVERIFY(find(shown, "reactionsFlow"));
        const qreal shownHeight = shown.root->implicitHeight();
        const QString shownDiag = diag(shown);

        set(app, "timelineReactions", kHide);
        Delegate hidden;
        QVERIFY(build(app, f, hidden, direct));
        const auto hiddenObjects = census::objectsUnder(hidden.root);
        QVERIFY2(countNamed(hiddenObjects, QStringLiteral("reactionsFlow")) == 0
                     && countNamed(hiddenObjects, QStringLiteral("reactionChip")) == 0,
                 qPrintable(census::describe(hiddenObjects, 12)));
        QVERIFY2(hiddenObjects.size() < shownObjects.size(),
                 qPrintable(QStringLiteral("hidden %1 objects, shown %2")
                                .arg(hiddenObjects.size()).arg(shownObjects.size())));
        // An own bubble without reactions takes the receipt band instead.
        const bool shorter = own ? hidden.root->implicitHeight() <= shownHeight
                                 : hidden.root->implicitHeight() < shownHeight;
        QVERIFY2(shorter,
                 qPrintable(QStringLiteral("hidden: %1\nshown: %2")
                                .arg(diag(hidden), shownDiag)));
        QString why;
        QVERIFY2(contentFitsTheRow(hidden, why), qPrintable(why));

        set(app, "timelineReactions", kShow);
        Delegate toggled;
        QVERIFY(build(app, f, toggled, direct));
        set(app, "timelineReactions", kHide);
        settle();
        QVERIFY(!find(toggled, "reactionsFlow"));
        QCOMPARE(toggled.root->implicitHeight(), hidden.root->implicitHeight());
    }

    void reactionsOnHoverHoldTheirSpaceAndAppearOnHoverOrSelection()
    {
        AppController app(AppController::MockBackend);
        app.settings()->setMessageLayout(0);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("reactions"), reactions(3));
        Delegate shown;
        QVERIFY(build(app, f, shown));
        const qreal shownHeight = shown.root->implicitHeight();

        set(app, "timelineReactions", kOnHover);
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *flow = find(d, "reactionsFlow");
        QVERIFY2(flow, "On hover must build the chips up front");
        QCOMPARE(d.root->implicitHeight(), shownHeight);
        leaveRow(d);
        QTRY_VERIFY2(opacityUpTo(flow, d.root) == 0.0, qPrintable(diag(d)));

        hoverRow(d);
        QTRY_COMPARE(opacityUpTo(flow, d.root), 1.0);
        QCOMPARE(find(d, "reactionsFlow"), flow);
        QCOMPARE(d.root->implicitHeight(), shownHeight);

        // The chips sit below the message line; reaching them keeps them shown.
        const QPointF chip = flow->mapToScene(QPointF(4, flow->height() / 2));
        QTest::mouseMove(d.window.get(), chip.toPoint());
        settle();
        QCOMPARE(opacityUpTo(flow, d.root), 1.0);

        leaveRow(d);
        QTRY_COMPARE(opacityUpTo(flow, d.root), 0.0);
        d.view->setProperty("keyboardSelectedEventId", QStringLiteral("$vis"));
        QTRY_COMPARE(opacityUpTo(flow, d.root), 1.0);
        d.view->setProperty("keyboardSelectedEventId", QString());
        QTRY_COMPARE(opacityUpTo(flow, d.root), 0.0);
    }

    void hiddenReceiptsBuildNothing_data()
    {
        QTest::addColumn<int>("layout");
        QTest::addColumn<bool>("direct");
        QTest::addColumn<bool>("own");
        QTest::newRow("classic") << 0 << false << false;
        QTest::newRow("bubbles-other") << 1 << true << false;
        QTest::newRow("bubbles-own") << 1 << true << true;
        QTest::newRow("compact") << 2 << false << false;
    }

    void hiddenReceiptsBuildNothing()
    {
        QFETCH(int, layout);
        QFETCH(bool, direct);
        QFETCH(bool, own);
        AppController app(AppController::MockBackend);
        app.settings()->setMessageLayout(layout);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), own);
        f.insert(QStringLiteral("readReceipts"), receipts(3));
        f.insert(QStringLiteral("readReceiptsTotal"), 3);

        Delegate shown;
        QVERIFY(build(app, f, shown, direct));
        QVERIFY(find(shown, "readReceiptStrip"));
        const qreal shownHeight = shown.root->implicitHeight();
        const QString shownDiag = diag(shown);

        set(app, "timelineReadReceipts", kHide);
        Delegate hidden;
        QVERIFY(build(app, f, hidden, direct));
        const auto objects = census::objectsUnder(hidden.root);
        QVERIFY2(countNamed(objects, QStringLiteral("readReceiptStrip")) == 0
                     && countNamed(objects, QStringLiteral("readReceiptRow")) == 0,
                 qPrintable(census::describe(objects, 12)));
        QCOMPARE(hidden.root->property("receiptRailReserve").toReal(), 0.0);
        QCOMPARE(hidden.root->property("bubbleReceiptInset").toReal(), 0.0);
        QCOMPARE(hidden.root->property("ownBubbleReceiptFooter").toBool(), false);
        if (layout == 1 && own)
            QVERIFY2(hidden.root->implicitHeight() < shownHeight,
                     qPrintable(QStringLiteral("an own bubble kept the receipt band: hidden: "
                                           "%1\nshown: %2").arg(diag(hidden), shownDiag)));
        else
            QVERIFY(hidden.root->implicitHeight() <= shownHeight);
        QString why;
        QVERIFY2(contentFitsTheRow(hidden, why), qPrintable(why));

        set(app, "timelineReadReceipts", kShow);
        Delegate toggled;
        QVERIFY(build(app, f, toggled, direct));
        set(app, "timelineReadReceipts", kHide);
        settle();
        QVERIFY(!find(toggled, "readReceiptStrip"));
        QCOMPARE(toggled.root->implicitHeight(), hidden.root->implicitHeight());
    }

    void receiptsOnHoverHoldTheirPlaceAndAppearOnHoverOrSelection()
    {
        AppController app(AppController::MockBackend);
        app.settings()->setMessageLayout(0);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(3));
        f.insert(QStringLiteral("readReceiptsTotal"), 3);
        Delegate shown;
        QVERIFY(build(app, f, shown));
        const qreal reserve = shown.root->property("receiptRailReserve").toReal();
        const qreal shownHeight = shown.root->implicitHeight();

        set(app, "timelineReadReceipts", kOnHover);
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *strip = find(d, "readReceiptStrip");
        QVERIFY2(strip, "On hover must build the receipts up front");
        QCOMPARE(d.root->implicitHeight(), shownHeight);
        QCOMPARE(d.root->property("receiptRailReserve").toReal(), reserve);
        leaveRow(d);
        QTRY_VERIFY2(opacityUpTo(strip, d.root) == 0.0, qPrintable(diag(d)));

        hoverRow(d);
        QTRY_COMPARE(opacityUpTo(strip, d.root), 1.0);
        QCOMPARE(find(d, "readReceiptStrip"), strip);
        leaveRow(d);
        QTRY_COMPARE(opacityUpTo(strip, d.root), 0.0);
        d.view->setProperty("keyboardSelectedEventId", QStringLiteral("$vis"));
        QTRY_COMPARE(opacityUpTo(strip, d.root), 1.0);
    }

    void showLeavesEverythingVisible()
    {
        AppController app(AppController::MockBackend);
        app.settings()->setMessageLayout(0);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("reactions"), reactions(2));
        f.insert(QStringLiteral("readReceipts"), receipts(2));
        f.insert(QStringLiteral("readReceiptsTotal"), 2);
        Delegate d;
        QVERIFY(build(app, f, d));
        for (const char *name : {"senderAvatar", "reactionsFlow", "readReceiptStrip"}) {
            auto *item = find(d, name);
            QVERIFY2(item, name);
            QVERIFY2(opacityUpTo(item, d.root) == 1.0, name);
        }
        auto *hover = d.root->findChild<QObject *>(QStringLiteral("timelineRevealHover"));
        QVERIFY2(hover, "no reveal handler on the row");
        QVERIFY2(!hover->property("enabled").toBool(),
                 "the reveal handler tracks the pointer with nothing to reveal");
    }

    void messageActionsChoicePersistsAndOnlyHideSticks()
    {
        {
            SettingsManager settings;
            QCOMPARE(settings.property("timelineMessageActions").toInt(), kShow);
            settings.setProperty("timelineMessageActions", kHide);
        }
        {
            SettingsManager reopened;
            QCOMPARE(reopened.property("timelineMessageActions").toInt(), kHide);
            reopened.setProperty("timelineMessageActions", kOnHover);
            QCOMPARE(reopened.property("timelineMessageActions").toInt(), kShow);
        }
        QSettings raw;
        raw.setValue(QStringLiteral("timeline/messageActions"), 7);
        raw.sync();
        SettingsManager settings;
        QCOMPARE(settings.property("timelineMessageActions").toInt(), kShow);
    }

    void hiddenMessageActionsBuildNoBarButTheMenuKeepsEveryAction()
    {
        AppController app(AppController::MockBackend);
        app.settings()->setMessageLayout(0);
        const QVariantMap f = baseFixture();

        Delegate shown;
        QVERIFY(build(app, f, shown));
        hoverRow(shown);
        auto *shownBar = find(shown, "messageActionBarLoader");
        QVERIFY(shownBar);
        QTRY_VERIFY2(shownBar->property("item").value<QObject *>(),
                     qPrintable(diag(shown)));

        set(app, "timelineMessageActions", kHide);
        Delegate hidden;
        QVERIFY(build(app, f, hidden));
        hoverRow(hidden);
        settle();
        QCOMPARE(hidden.view->property("hoveredActionsKey").toString().isEmpty(), false);
        auto *bar = find(hidden, "messageActionBarLoader");
        QVERIFY(bar);
        QVERIFY(!bar->property("active").toBool());
        QVERIFY(!bar->property("item").value<QObject *>());
        QVERIFY(!hidden.root->property("actionsVisible").toBool());

        QVERIFY(QMetaObject::invokeMethod(hidden.root, "openContextMenu",
                                          Q_ARG(QVariant, 10), Q_ARG(QVariant, 10),
                                          Q_ARG(QVariant, false)));
        auto *menu = hidden.root->findChild<QObject *>(QStringLiteral("messageContextMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(!bar->property("item").value<QObject *>());
        auto *strip = menu->findChild<QObject *>(QStringLiteral("quickReactionStrip"));
        QVERIFY2(strip && strip->property("visible").toBool(), "no reactions in the menu");
        QStringList texts;
        for (QObject *o : menu->findChildren<QObject *>())
            if (o->property("visible").toBool())
                texts << o->property("text").toString();
        QVERIFY2(texts.contains(QStringLiteral("Reply"))
                     && texts.contains(QStringLiteral("Reply in thread")),
                 qPrintable(texts.join(QStringLiteral(" | "))));
    }

    void largeEmojiCanBeTurnedOff()
    {
        AppController app(AppController::MockBackend);
        app.settings()->setMessageLayout(0);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("body"), QString::fromUtf8("\xf0\x9f\x8e\x89"));
        Delegate big;
        QVERIFY(build(app, f, big));
        auto *loader = find(big, "messageBodyLoader");
        QVERIFY(loader);
        QVERIFY2(loader->property("bigEmoji").toBool(), "the fixture is not emoji-only");
        const int bigSize = loader->property("bodyFont").value<QFont>().pixelSize();
        const qreal bigHeight = big.root->implicitHeight();
        const QString bigDiag = diag(big);

        set(app, "largeEmoji", false);
        Delegate small;
        QVERIFY(build(app, f, small));
        auto *smallLoader = find(small, "messageBodyLoader");
        QVERIFY(smallLoader);
        QVERIFY(!smallLoader->property("bigEmoji").toBool());
        const int smallSize = smallLoader->property("bodyFont").value<QFont>().pixelSize();
        QVERIFY2(smallSize < bigSize,
                 qPrintable(QStringLiteral("%1px off against %2px on")
                                .arg(smallSize).arg(bigSize)));
        QVERIFY2(small.root->implicitHeight() < bigHeight,
                 qPrintable(QStringLiteral("off: %1\non: %2").arg(diag(small), bigDiag)));
    }
};

QTEST_MAIN(TimelineVisibilityQmlTest)
#include "TimelineVisibilityQmlTest.moc"
