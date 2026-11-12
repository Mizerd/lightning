// Loads the real TimelinePane.qml through the "MatrixClient" QML module over a
// real AppController on the mock backend, and asserts zero engine warnings
// across its presentation states. Binding and registration errors only show
// up when a QQmlEngine evaluates the component, which a text scan cannot do.
#include <QtTest/QtTest>

#include <functional>

#include <limits>

#include <QGuiApplication>
#include <QClipboard>
#include <QWheelEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <cmath>
#include <QQmlExpression>
#include <QQmlComponent>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTimer>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <QBuffer>
#include <QImage>

#include "app/AppController.h"
#include "app/ForwardController.h"
#include "app/RoomDiscoveryController.h"
#include "app/RoomInfoController.h"
#include "auth/AuthManager.h"
#include "calls/CallController.h"
#include "calls/CallMediaBackend.h"
#include "calls/OneToOneDm.h"
#include "media/MediaImageProvider.h"
#include "models/ContextController.h"
#include "models/PaginationController.h"
#include "models/ReverseListProxyModel.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"
#include "models/TimelineScrollController.h"
#include "threads/ThreadController.h"
#include "matrix/MockMatrixClient.h"
#include "spaces/SpaceManager.h"

namespace {
constexpr int kSignalTimeoutMs = 4000;
constexpr int kAnchorRowRevealTimeoutMs = 30000;
}

namespace {
class GestureHold
{
public:
    explicit GestureHold(QObject *settleTimer)
    {
        QMetaObject::invokeMethod(settleTimer, "restart");
        QObject::connect(&m_ticker, &QTimer::timeout, settleTimer,
                         [settleTimer] {
                             QMetaObject::invokeMethod(settleTimer, "restart");
                         });
        m_ticker.start(20);
    }
    ~GestureHold() { m_ticker.stop(); }

private:
    QTimer m_ticker;
};

class LogCapture
{
public:
    LogCapture()
    {
        s_messages.clear();
        m_previous = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext &, const QString &msg) {
                s_messages.append(msg);
            });
    }
    ~LogCapture() { qInstallMessageHandler(m_previous); }
    static QStringList messages() { return s_messages; }

private:
    QtMessageHandler m_previous = nullptr;
    inline static QStringList s_messages;
};
}

class TimelinePaneQmlTest : public QObject
{
    Q_OBJECT

private:
    static QObject *settingsItemNamed(QObject *root, const QString &name)
    {
        root->setProperty("section", QStringLiteral("chat"));
        std::function<QObject *(QQuickItem *)> walk =
            [&](QQuickItem *node) -> QObject * {
            if (node->objectName() == name)
                return node;
            for (QQuickItem *child : node->childItems()) {
                if (QObject *hit = walk(child))
                    return hit;
            }
            return nullptr;
        };
        auto *top = qobject_cast<QQuickItem *>(root);
        return top ? walk(top) : nullptr;
    }

    // Logs in on the mock backend and waits for the room list to populate.
    // Returns the room id at `row` of the (client-order) room list.
    static QString loginAndRoomIdAt(AppController &controller, int row)
    {
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                  QStringLiteral("alice"),
                                  QStringLiteral("unused"));
        if (!loginSpy.wait(kSignalTimeoutMs))
            return {};
        for (int i = 0; i < 50 && controller.roomList()->rowCount() <= row; ++i)
            QTest::qWait(20);
        if (controller.roomList()->rowCount() <= row)
            return {};
        const QModelIndex idx = controller.roomList()->index(row, 0);
        return controller.roomList()
            ->data(idx, RoomListModel::RoomIdRole)
            .toString();
    }

    // The first loaded event in the current room that other events name as
    // their thread root (the mock thread fixture).
    static QString fixtureThreadRootId(AppController &controller)
    {
        auto *timeline = controller.timeline();
        for (int row = 0; row < timeline->rowCount(); ++row) {
            const QString rootId = timeline
                ->data(timeline->index(row, 0), TimelineModel::ThreadRootIdRole)
                .toString();
            if (!rootId.isEmpty())
                return rootId;
        }
        return {};
    }

    static QString firstStateGroupId(TimelineModel *timeline)
    {
        for (int i = 0; i < timeline->rowCount(); ++i) {
            const QModelIndex idx = timeline->index(i);
            if (timeline->data(idx, TimelineModel::StateGroupLeaderRole).toBool())
                return timeline->data(idx, TimelineModel::StateGroupIdRole).toString();
        }
        return {};
    }

    static QList<QQuickItem *> findVisualChildren(QQuickItem *parent,
                                                  const QString &name)
    {
        QList<QQuickItem *> result;
        if (!parent)
            return result;
        const auto kids = parent->childItems();
        for (auto *child : kids) {
            if (child->objectName() == name)
                result.append(child);
            result.append(findVisualChildren(child, name));
        }
        return result;
    }

    static int viewRowForSourceRow(QQuickItem *timeline, int sourceRow)
    {
        QVariant out;
        if (!QMetaObject::invokeMethod(timeline, "viewRowForSourceRow",
                                       Q_RETURN_ARG(QVariant, out),
                                       Q_ARG(QVariant, QVariant(sourceRow))))
            return -1;
        return out.toInt();
    }

    static bool waitForRowsToStopArriving(AppController &controller)
    {
        auto *view = qobject_cast<ReverseListProxyModel *>(
            controller.timelineView());
        if (!view)
            return false;
        return QTest::qWaitFor([view] { return view->revealIdle(); },
                               kAnchorRowRevealTimeoutMs);
    }

    struct AnchorSettle {
        bool sawRow = false;
        bool neededFlush = false;
        bool rightRow = true;
        QString measuredId;
        QString wantedId;
        QString counters;
        double offset = 0;
        bool moved(double offsetBefore) const
        {
            return !sawRow || !rightRow
                || qAbs(offset - offsetBefore) >= 2.0;
        }
        QString detail(double offsetBefore) const
        {
            if (!sawRow)
                return QStringLiteral("the anchor's row was never built "
                                      "within %1 ms — a paced-reveal "
                                      "timeout, NOT a compensation failure")
                    .arg(kAnchorRowRevealTimeoutMs);
            if (!rightRow)
                return QStringLiteral("the row measured was %1, not the "
                                      "anchor %2 — a source-row/view-row "
                                      "MAPPING failure, NOT a compensation "
                                      "failure (offset read %3)")
                    .arg(measuredId.isEmpty() ? QStringLiteral("(none)")
                                              : measuredId)
                    .arg(wantedId).arg(offset);
            return QStringLiteral("viewport offset %1 -> %2, read %3 a "
                                  "layout flush; %4")
                .arg(offsetBefore)
                .arg(offset)
                .arg(neededFlush ? QStringLiteral("after")
                                 : QStringLiteral("without"))
                .arg(counters);
        }
    };
    static AnchorSettle anchorOffsetOnceItsRowExists(QQuickItem *timeline,
                                                     int rowAfter,
                                                     double offsetBefore,
                                                     const QString &anchorId)
    {
        AnchorSettle settle;
        QQuickItem *item = nullptr;
        settle.sawRow = QTest::qWaitFor(
            [&] {
                item = itemForSourceRow(timeline, rowAfter);
                return item != nullptr;
            },
            kAnchorRowRevealTimeoutMs);
        if (!settle.sawRow)
            return settle;
        const auto read = [&] {
            settle.offset =
                item->y() - timeline->property("contentY").toDouble();
        };
        const auto identify = [&] {
            settle.wantedId = anchorId;
            QVariant out;
            const int viewRow = viewRowForSourceRow(timeline, rowAfter);
            if (viewRow >= 0
                && QMetaObject::invokeMethod(
                       timeline, "stableIdAtViewRow", Q_RETURN_ARG(QVariant, out),
                       Q_ARG(QVariant, QVariant(viewRow))))
                settle.measuredId = out.toString();
            settle.rightRow = settle.measuredId == anchorId;
        };
        const auto snapshot = [&] {
            static const char *kNames[] = {
                "diagAnchorCorrections", "diagGrowthCorrections",
                "diagNoAnchorReturns", "diagStickToBottomReturns",
                "diagPrependFirings", "diagPrependOriginShiftSum",
                "diagPrependMaxAbsOriginShift",
                "diagDisplacedFirings", "diagMaterializedFirings",
                "diagMaterializedMaxAbsDelta", "diagActiveDeferrals",
                "diagUnresolvedIdFallbacks", "diagEvictedNoInsertFallbacks",
                "originY", "contentY", "contentHeight", "rowWindowSkip",
            };
            QStringList parts;
            QStringList missing;
            for (const char *name : kNames) {
                const QVariant v = timeline->property(name);
                if (v.isValid())
                    parts << QStringLiteral("%1=%2").arg(
                        QString::fromLatin1(name).startsWith(QLatin1String("diag"))
                            ? QString::fromLatin1(name).mid(4)
                            : QString::fromLatin1(name),
                        v.toString());
                else
                    missing << QString::fromLatin1(name);
            }
            if (!missing.isEmpty())
                parts << QStringLiteral("UNREADABLE[%1]")
                             .arg(missing.join(QLatin1Char(',')));
            settle.counters = parts.join(QLatin1Char(' '));
        };
        read();
        identify();
        snapshot();
        if (settle.moved(offsetBefore)) {
            settle.neededFlush = true;
            if (QQuickWindow *window = timeline->window())
                window->requestUpdate();
            QTest::qWait(0);
            if (QQuickItem *again = itemForSourceRow(timeline, rowAfter))
                item = again;
            read();
            identify();
            snapshot();
        }
        return settle;
    }

    static QQuickItem *itemForSourceRow(QQuickItem *timeline, int sourceRow)
    {
        const int viewRow = viewRowForSourceRow(timeline, sourceRow);
        if (viewRow < 0)
            return nullptr;
        QVariant out;
        if (!QMetaObject::invokeMethod(timeline, "itemAtViewRow",
                                       Q_RETURN_ARG(QVariant, out),
                                       Q_ARG(QVariant, QVariant(viewRow))))
            return nullptr;
        return out.value<QQuickItem *>();
    }

    static QQuickItem *itemAtViewRow(QQuickItem *timeline, int viewRow)
    {
        QVariant out;
        if (!QMetaObject::invokeMethod(timeline, "itemAtViewRow",
                                       Q_RETURN_ARG(QVariant, out),
                                       Q_ARG(QVariant, QVariant(viewRow))))
            return nullptr;
        return out.value<QQuickItem *>();
    }

    static QQuickItem *onScreenMessageRow(QQuickItem *timeline,
                                          QString *actionKey)
    {
        for (int row = 0; row < 12; ++row) {
            QQuickItem *candidate = itemAtViewRow(timeline, row);
            if (!candidate)
                continue;
            const QString key = candidate->property("actionKey").toString();
            if (key.isEmpty())
                continue;
            *actionKey = key;
            return candidate;
        }
        return nullptr;
    }

    static QString onScreenRowsOutsideTheMediaBand(QQuickItem *timeline)
    {
        const double top = timeline->property("contentY").toDouble();
        const double bottom = top + timeline->property("height").toDouble();
        const int count = timeline->property("count").toInt();
        QStringList outside;
        int onScreen = 0;
        for (int row = 0; row < count; ++row) {
            QQuickItem *item = itemAtViewRow(timeline, row);
            if (!item || item->height() <= 0)
                continue;
            if (item->y() >= bottom)
                break;
            if (item->y() + item->height() <= top)
                continue;
            ++onScreen;
            if (!item->property("mediaInBand").toBool())
                outside << QStringLiteral("%1@y%2").arg(row).arg(item->y());
        }
        if (onScreen == 0)
            return QStringLiteral("no row is on screen (count %1)").arg(count);
        if (outside.isEmpty())
            return {};
        return QStringLiteral("%1 of %2 rows on screen are outside the media "
                              "band [%3..%4] (count %5, contentY %6): %7")
            .arg(outside.size()).arg(onScreen)
            .arg(timeline->property("mediaBandFirstRow").toInt())
            .arg(timeline->property("mediaBandLastRow").toInt())
            .arg(count).arg(top).arg(outside.join(QStringLiteral(", ")));
    }

    static int lastViewRowOnScreen(QQuickItem *timeline)
    {
        const double top = timeline->property("contentY").toDouble();
        const double bottom = top + timeline->property("height").toDouble();
        const int count = timeline->property("count").toInt();
        int last = -1;
        for (int row = 0; row < count; ++row) {
            QQuickItem *item = itemAtViewRow(timeline, row);
            if (!item || item->height() <= 0)
                continue;
            if (item->y() >= bottom)
                break;
            if (item->y() + item->height() > top)
                last = row;
        }
        return last;
    }

    static int firstViewRowBeyond(QQuickItem *timeline, double y)
    {
        const int count = timeline->property("count").toInt();
        for (int row = 0; row < count; ++row) {
            QQuickItem *item = itemAtViewRow(timeline, row);
            if (item && item->height() > 0 && item->y() > y)
                return row;
        }
        return -1;
    }

    static bool positionAtSourceRow(QQuickItem *timeline, int sourceRow)
    {
        const int viewRow = viewRowForSourceRow(timeline, sourceRow);
        if (viewRow < 0)
            return false;
        return QMetaObject::invokeMethod(timeline, "positionViewAtViewRow",
                                         Q_ARG(QVariant, QVariant(viewRow)),
                                         Q_ARG(QVariant, QVariant(false)));
    }

    static bool positionAtTopEdge(QQuickItem *timeline)
    {
        QVariant maxY;
        if (!QMetaObject::invokeMethod(timeline, "wheelMaxY",
                                       Q_RETURN_ARG(QVariant, maxY)))
            return false;
        return timeline->setProperty("contentY", maxY.toDouble());
    }

    static bool wheelBounds(QQuickItem *timeline, double *minY, double *maxY)
    {
        QVariant minV, maxV;
        if (!QMetaObject::invokeMethod(timeline, "wheelMinY",
                                       Q_RETURN_ARG(QVariant, minV)))
            return false;
        if (!QMetaObject::invokeMethod(timeline, "wheelMaxY",
                                       Q_RETURN_ARG(QVariant, maxV)))
            return false;
        *minY = minV.toDouble();
        *maxY = maxV.toDouble();
        return true;
    }

    // Filters the one warning that belongs to the mock fixture: `mediaThumbUrl`
    // is a plain http URL there, so image rows make Qt fail to resolve
    // `mock.local`. Pinned to that host so a fixture reaching a real host still
    // fails.
    // Every input to the pane's row-hover suspension, for failure messages:
    // a hover that never arrives must say whether hover was suspended and by
    // what, or whether it was free and simply not delivered.
    static QString rowHoverState(QQuickItem *timeline, QObject *rowHover)
    {
        QQuickItem *root = timeline;
        while (auto *parent = qobject_cast<QQuickItem *>(root->parent()))
            root = parent;
        auto *scroller = root->findChild<QQuickItem *>(
            QStringLiteral("timelineMiddleClickScroller"));
        return QStringLiteral(
                   "suspended=%1 motion=%2 moving=%3 wheel=%4 barHeld=%5 "
                   "autoscroll=%6 handlerEnabled=%7 hovered=%8 key='%9' "
                   "exposed=%10")
            .arg(timeline->property("rowHoverSuspended").toBool())
            .arg(timeline->property("viewportMotionActive").toBool())
            .arg(timeline->property("moving").toBool())
            .arg(timeline->property("wheelAnimating").toBool())
            .arg(timeline->property("scrollBarHeld").toBool())
            .arg(scroller ? scroller->property("active").toBool() : false)
            .arg(rowHover ? rowHover->property("enabled").toBool() : false)
            .arg(rowHover ? rowHover->property("hovered").toBool() : false)
            .arg(timeline->property("hoveredActionsKey").toString())
            .arg(timeline->window() && timeline->window()->isExposed());
    }

    static QStringList realWarnings(const QStringList &warnings)
    {
        QStringList out;
        for (const QString &w : warnings) {
            if (w.contains(QLatin1String("QQuickImage: Host mock.local"))
                && w.contains(QLatin1String("not found"))) {
                continue;
            }
            out << w;
        }
        return out;
    }

    static QQuickItem *paneWithEvents(AppController &controller,
                                      QQmlApplicationEngine &engine,
                                      QQuickWindow &window,
                                      const QString &roomId,
                                      const QList<TimelineEvent> &events,
                                      int paginationPages,
                                      int viewportHeight,
                                      QQuickItem **timelineOut)
    {
        auto *mock = controller.findChild<MockMatrixClient *>();
        if (!mock)
            return nullptr;
        controller.setCurrentRoomId(roomId);

        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return nullptr;
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!root)
            return nullptr;
        window.resize(700, viewportHeight);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QTest::qWaitForWindowExposed(&window, 2000);
        window.requestActivate();
        QCoreApplication::processEvents();

        mock->resetTimelineForTest(roomId, events, paginationPages);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        if (!timeline)
            return nullptr;
        if (!QTest::qWaitFor([&] {
                return timeline->property("presentationReady").toBool();
            }, 5000))
            return nullptr;
        if (timelineOut)
            *timelineOut = timeline;
        return root;
    }

    static QQuickItem *paneRootOf(QQuickItem *item)
    {
        QQuickItem *root = item;
        while (auto *parent = qobject_cast<QQuickItem *>(root->parent()))
            root = parent;
        return root;
    }

    static QList<TimelineEvent> textFixture(const QString &roomId, int count,
                                            const QString &prefix,
                                            const QString &body,
                                            int baseSecondsAgo = 40000)
    {
        const QDateTime base =
            QDateTime::currentDateTimeUtc().addSecs(-baseSecondsAgo);
        QList<TimelineEvent> events;
        for (int i = 0; i < count; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$%1%2").arg(prefix).arg(i);
            e.itemId = QStringLiteral("uid-%1%2").arg(prefix).arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("%1 %2").arg(body).arg(i);
            e.timestamp = base.addSecs(i * 30);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        return events;
    }

    static void sendWheelNotch(QQuickWindow &window, const QPointF &pos,
                               int angleY, bool inverted)
    {
        QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                          QPoint(0, 0), QPoint(0, angleY), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, inverted);
        QCoreApplication::sendEvent(&window, &wheel);
        QCoreApplication::processEvents();
    }

    // A stand-in for PresenceManager with the same QML surface, so a test can
    // set a peer's presence. The mock backend has no presence, so the real
    // manager never answers.
    static QObject *fakePresenceService(QQmlEngine &engine)
    {
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"QML(
import QtQuick
QtObject {
    property int revision: 0
    // Differs from the real manager's epoch, so an injected dot re-watches.
    property int sessionEpoch: 4242
    property var infos: ({})
    // When set, every user without an explicit report reads this state.
    property string everyone: ""
    property var watched: []
    readonly property string watchedText: watched.join("\n")
    function infoFor(u) {
        if (infos[u] !== undefined)
            return infos[u]
        if (everyone !== "")
            return ({ state: everyone, lastActiveAgoMs: -1, statusMsg: "" })
        return ({})
    }
    function stateFor(u) {
        var i = infoFor(u)
        return i.state === undefined ? "" : i.state
    }
    function watch(u) { watched = watched.concat([u]) }
    function unwatch(u) { watched = watched.filter(function (w) { return w !== u }) }
    function report(u, info) {
        var m = Object.assign({}, infos)
        m[u] = info
        infos = m
        revision = revision + 1
    }
    function reportEveryone(s) {
        everyone = s
        revision = revision + 1
    }
}
)QML"), QUrl());
        QObject *fake = component.create(engine.rootContext());
        if (!fake) {
            qWarning("fakePresenceService: %s", qPrintable(component.errorString()));
            return nullptr;
        }
        QQmlEngine::setObjectOwnership(fake, QQmlEngine::CppOwnership);
        fake->setParent(&engine);
        return fake;
    }

    static void reportPresence(QObject *fake, const QString &userId,
                               const QString &state, qint64 lastActiveAgoMs)
    {
        QVariantMap info{
            { QStringLiteral("state"), state },
            { QStringLiteral("lastActiveAgoMs"), lastActiveAgoMs },
            { QStringLiteral("statusMsg"), QString() },
        };
        QMetaObject::invokeMethod(fake, "report", Q_ARG(QVariant, userId),
                                  Q_ARG(QVariant, info));
        QCoreApplication::processEvents();
    }

    static bool injectPresence(QQuickItem *dot, QObject *fake)
    {
        const bool ok = QQmlProperty::write(dot, QStringLiteral("presenceService"),
                                            QVariant::fromValue(fake));
        QCoreApplication::processEvents();
        return ok;
    }

    static QStringList watchedBy(QObject *fake)
    {
        return fake->property("watchedText").toString().split(
            QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

    static QColor themeColor(QQuickItem *root, const QString &token)
    {
        QQmlExpression expr(qmlContext(root), root,
                            QStringLiteral("AppTheme.") + token);
        return expr.evaluate().value<QColor>();
    }

    static QString presenceDotPlacement(QQuickItem *dot, QQuickItem *avatar,
                                        QQuickItem *container)
    {
        if (dot->width() <= 0 || dot->height() <= 0)
            return QStringLiteral("the dot has no size");
        const QRectF inAvatar(dot->mapToItem(avatar, QPointF(0, 0)), dot->size());
        const QRectF avatarRect(0, 0, avatar->width(), avatar->height());
        if (!avatarRect.adjusted(-0.5, -0.5, 0.5, 0.5).contains(inAvatar))
            return QStringLiteral("dot %1,%2 %3x%4 is not inside its %5x%6 avatar")
                .arg(inAvatar.x()).arg(inAvatar.y()).arg(inAvatar.width())
                .arg(inAvatar.height()).arg(avatar->width()).arg(avatar->height());
        const QPointF centre = inAvatar.center();
        if (centre.x() <= avatar->width() / 2.0
                || centre.y() <= avatar->height() / 2.0)
            return QStringLiteral("dot centre %1,%2 is not in the bottom-right "
                                  "quadrant of its %3x%4 avatar")
                .arg(centre.x()).arg(centre.y())
                .arg(avatar->width()).arg(avatar->height());
        if (qAbs(inAvatar.bottom() - avatar->height()) > 0.5
                || qAbs(inAvatar.right() - avatar->width()) > 0.5)
            return QStringLiteral("dot %1,%2 %3x%4 is not at the bottom-right of "
                                  "its %5x%6 avatar")
                .arg(inAvatar.x()).arg(inAvatar.y()).arg(inAvatar.width())
                .arg(inAvatar.height()).arg(avatar->width()).arg(avatar->height());
        const QRectF inContainer(dot->mapToItem(container, QPointF(0, 0)),
                                 dot->size());
        const QRectF containerRect(0, 0, container->width(), container->height());
        if (!containerRect.adjusted(-0.5, -0.5, 0.5, 0.5).contains(inContainer))
            return QStringLiteral("dot %1,%2 is outside its %3x%4 container")
                .arg(inContainer.x()).arg(inContainer.y())
                .arg(container->width()).arg(container->height());
        return {};
    }

    static QVariant attached(QQuickItem *item, const QString &name)
    {
        return QQmlProperty::read(item, name, qmlContext(item));
    }

    static QMetaMethod signalNamed(QObject *object, const char *name)
    {
        const QMetaObject *mo = object->metaObject();
        for (int i = 0; i < mo->methodCount(); ++i) {
            const QMetaMethod method = mo->method(i);
            if (method.methodType() == QMetaMethod::Signal && method.name() == name)
                return method;
        }
        return {};
    }

    static QList<TimelineEvent> hiddenActivitySandwich(const QString &roomId,
                                                       bool withMarker)
    {
        QList<TimelineEvent> events =
            textFixture(roomId, 20, QStringLiteral("vpold"),
                        QStringLiteral("older message"),40000);
        for (int i = 0; i < 10; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$vphidden%1").arg(i);
            e.itemId = QStringLiteral("uid-vphidden%1").arg(i);
            e.roomId = roomId;
            e.type = TimelineEvent::StateChange;
            e.stateKind = QStringLiteral("membership");
            e.sender = QStringLiteral("@bob:mock.local");
            e.senderDisplayName = QStringLiteral("Bob");
            e.body = QStringLiteral("membership churn");
            e.timestamp = QDateTime::currentDateTimeUtc().addSecs(-20000 + i * 30);
            events.append(e);
        }
        if (withMarker) {
            TimelineEvent marker;
            marker.roomId = roomId;
            marker.type = TimelineEvent::ReadMarker;
            events.append(marker);
        }
        events.append(textFixture(roomId, 25, QStringLiteral("vpnew"),
                                  QStringLiteral("newer message"),
7200));
        return events;
    }

    static int laidOutRowAt(QQuickItem *timeline, qreal y)
    {
        const int count = timeline->property("count").toInt();
        int best = -1;
        for (int row = 0; row < count; ++row) {
            QQuickItem *item = itemAtViewRow(timeline, row);
            if (item && item->isVisible() && item->width() > 0
                && item->height() > 0 && item->y() <= y)
                best = row;
        }
        return best;
    }

private Q_SLOTS:
    void roomActivityComponentExpandsVisibleTypedEntries()
    {
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("RoomActivityDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QVERIFY(!createdSpy.isEmpty());
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(420, 240);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        window.show();

        QVariantList entries;
        entries.append(QVariantMap{
            { QStringLiteral("stableEventId"), QStringLiteral("item-join") },
            { QStringLiteral("eventKind"), QStringLiteral("membership") },
            { QStringLiteral("actorDisplayName"), QStringLiteral("Alice") },
            { QStringLiteral("affectedMemberDisplayName"), QStringLiteral("Bob") },
            { QStringLiteral("description"), QStringLiteral("Bob joined the room.") },
        });
        entries.append(QVariantMap{
            { QStringLiteral("stableEventId"), QStringLiteral("item-topic") },
            { QStringLiteral("eventKind"), QStringLiteral("m.room.topic") },
            { QStringLiteral("actorDisplayName"), QStringLiteral("Alice") },
            { QStringLiteral("description"), QStringLiteral("Alice changed the room topic.") },
        });
        QVERIFY(root->setProperty("entries", entries));

        auto *summary = root->findChild<QQuickItem *>(
            QStringLiteral("stateActivitySummaryLabel"));
        QVERIFY(summary != nullptr);
        QVERIFY(!summary->property("text").toString().contains(
            QStringLiteral("security")));
        entries.append(QVariantMap{
            { QStringLiteral("stableEventId"), QStringLiteral("item-grant") },
            { QStringLiteral("eventKind"), QStringLiteral("m.room.power_levels") },
            { QStringLiteral("description"), QStringLiteral("Permissions changed") },
        });
        QVERIFY(root->setProperty("entries", entries));
        QTRY_VERIFY_WITH_TIMEOUT(summary->property("text").toString().contains(
            QStringLiteral("permissions or security changed")), kSignalTimeoutMs);

        auto *expanded = root->findChild<QQuickItem *>(
            QStringLiteral("stateActivityExpandedContent"));
        QVERIFY(expanded != nullptr);
        QCOMPARE(root->property("entryCount").toInt(), 3);
        QCOMPARE(expanded->isVisible(), false);
        QCOMPARE(expanded->height(), 0.0);

        QVERIFY(root->setProperty("expanded", true));
        QTRY_VERIFY_WITH_TIMEOUT(expanded->isVisible(), kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("renderedEntryCount").toInt(),
                                  3, kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(root->property("expandedContentHeight").toReal() > 0.0,
                                 kSignalTimeoutMs);

        QVERIFY(root->setProperty("expanded", false));
        QTRY_VERIFY_WITH_TIMEOUT(!expanded->isVisible(), kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(expanded->height(), 0.0, kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void timelinePaneInstantiatesWithoutReferenceError()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);

        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QVERIFY(!createdSpy.isEmpty());
        QVERIFY(createdSpy.at(0).at(0).value<QObject *>() != nullptr);
        QCOMPARE(realWarnings(warnings), QStringList{});

        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *header = root->findChild<QQuickItem *>(
            QStringLiteral("paginationHeader"));
        QVERIFY(header != nullptr);
        QCOMPARE(header->height(), 0.0);
    }

    void theCallButtonAppearsWhenTheRoomBecomesCallableAfterOpening()
    {
        class Engine : public CallMediaBackend
        {
        public:
            using CallMediaBackend::CallMediaBackend;
            void createOffer(const QString &) override {}
            void createAnswer(const QString &, const QString &) override {}
            void setRemoteAnswer(const QString &, const QString &) override {}
            void addRemoteCandidate(const QString &, const QString &,
                                    const QString &, int) override {}
            void setIceServers(const QStringList &, const QString &,
                               const QString &) override {}
            void close(const QString &) override {}
        };
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, 0).isEmpty());
        const auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock);
        QString dm;
        for (const RoomInfo &room : mock->rooms()) {
            if (!calls::oneToOneDmPeer(room, true).isEmpty()) {
                dm = room.id;
                break;
            }
        }
        QVERIFY2(!dm.isEmpty(), "the mock needs an unambiguous 1:1 DM");
        controller.setCurrentRoomId(dm);
        QVERIFY(!controller.canStartCall(dm));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root);
        auto *button = root->findChild<QObject *>(
            QStringLiteral("startVoiceCallButton"));
        QVERIFY(button);
        QVERIFY(!button->property("available").toBool());

        Engine media;
        controller.calls()->setMediaBackend(&media);
        auto detach = qScopeGuard(
            [&controller] { controller.calls()->setMediaBackend(nullptr); });
        QVERIFY(controller.canStartCall(dm));
        const int revision = controller.callGateRevision();
        controller.rtc()->setMediaAvailable(
            !controller.rtc()->mediaAvailable());
        QVERIFY(controller.callGateRevision() > revision);
        QCoreApplication::processEvents();

        QVERIFY2(button->property("available").toBool(),
                 "the room became callable after it was opened and the call "
                 "gate's revision moved, but the button kept the answer it "
                 "had at room-open");
    }

    void homeSurfaceShownWithNoRoomAndComposerHidden()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomId.isEmpty());
        controller.setCurrentRoomId(QString());

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QCOMPARE(realWarnings(warnings), QStringList{});

        auto *home = root->findChild<QQuickItem *>(QStringLiteral("homePane"));
        auto *composer =
            root->findChild<QQuickItem *>(QStringLiteral("composerCard"));
        QVERIFY(home != nullptr);
        QVERIFY(composer != nullptr);
        QVERIFY(home->isVisible());
        QVERIFY(!composer->isVisible());

        controller.setCurrentRoomId(roomId);
        QCoreApplication::processEvents();
        QVERIFY(!home->isVisible());
        QVERIFY(composer->isVisible());
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void loadingStateExpandsHeaderThenCollapses()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomId.isEmpty());
        // "!general:mock.local" is seeded with 2 pages remaining, so the
        // request is accepted.
        const QString generalId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(generalId);
        QCOMPARE(controller.pagination()->roomId(), generalId);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QVERIFY(!createdSpy.isEmpty());
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *header = root->findChild<QQuickItem *>(
            QStringLiteral("paginationHeader"));
        QVERIFY(header != nullptr);

        auto expectedHeight = [&] {
            return controller.pagination()->presentationState()
                           == PaginationController::Loading
                       ? 32.0
                       : 0.0;
        };
        QCOMPARE(header->height(), expectedHeight());
        QVERIFY(controller.pagination()->presentationState()
                != PaginationController::Failed);

        // Let every automatic batch resolve (the mock settles each ~300 ms
        // later; 2 seeded pages, so this terminates). The header height is
        // asserted too: it only re-evaluates on stateChanged(), which a C++
        // test cannot observe.
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.pagination()->presentationState(),
                                  PaginationController::Hidden, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(header->height(), 0.0, 5000);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void receiptListOpenerIsReachableFromDelegatesAndOpens()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(680, 480);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        const QVariant opener = timeline->property("openReceiptList");
        QVERIFY(opener.isValid());
        QVERIFY(qvariant_cast<QJSValue>(opener).isCallable());

        auto *popover =
            root->findChild<QObject *>(QStringLiteral("receiptListPopover"));
        QVERIFY(popover != nullptr);
        QVERIFY(!popover->property("visible").toBool());
        QQmlExpression bottomCall(
            qmlContext(timeline), timeline,
            QStringLiteral(
                "openReceiptList([{userId: \"@u0:mock.local\","
                " displayName: \"User 0\", avatarMxc: \"\","
                " tsMs: 1000}], 30,"
                " Qt.point(40, Overlay.overlay.height - 12))"));
        bottomCall.evaluate();
        QVERIFY2(!bottomCall.hasError(),
                 bottomCall.error().toString().toUtf8().constData());
        QTRY_VERIFY_WITH_TIMEOUT(popover->property("visible").toBool(), 5000);
        QCOMPARE(popover->property("totalOthers").toInt(), 30);
        const qreal placedHeight = popover->property("height").toReal();
        QQmlExpression grow(
            qmlContext(timeline), timeline,
            QStringLiteral(
                "receiptListPopover.readers ="
                " Array.from({length: 30}, function(v, i) {"
                " return {userId: \"@u\" + i + \":mock.local\","
                " displayName: \"User \" + i, avatarMxc: \"\","
                " tsMs: 1000 + i}; })"));
        grow.evaluate();
        QVERIFY2(!grow.hasError(),
                 grow.error().toString().toUtf8().constData());
        QTRY_VERIFY_WITH_TIMEOUT(
            popover->property("height").toReal() > placedHeight + 40, 5000);
        const qreal manyHeight = popover->property("height").toReal();
        QVERIFY(manyHeight <= window.height() * 0.5 + 1.0);
        auto *readerList = popover->findChild<QQuickItem *>(
            QStringLiteral("receiptReaderList"));
        QVERIFY(readerList != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            readerList->property("contentHeight").toReal()
                > readerList->height() + 1.0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            popover->property("y").toReal()
                    + popover->property("height").toReal()
                <= window.height() + 0.5, 5000);

        QQmlExpression closeCall(qmlContext(timeline), timeline,
                                 QStringLiteral("receiptListPopover.close()"));
        closeCall.evaluate();
        QVERIFY(!closeCall.hasError());
        QQmlExpression call(
            qmlContext(timeline), timeline,
            QStringLiteral(
                "openReceiptList([{userId: \"@a:mock.local\","
                " displayName: \"A\", avatarMxc: \"\", tsMs: 0}],"
                " 3, Qt.point(40, 40))"));
        call.evaluate();
        QVERIFY2(!call.hasError(),
                 call.error().toString().toUtf8().constData());
        QTRY_VERIFY_WITH_TIMEOUT(popover->property("visible").toBool(), 5000);
        QCOMPARE(popover->property("totalOthers").toInt(), 3);
        QCOMPARE(popover->property("readers").toList().size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(
            popover->property("height").toReal() < 140, 5000);
    }

    void timelineRowsCarryNoPermanentViewportObservers()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-3600);
        QList<TimelineEvent> events;
        for (int i = 0; i < 40; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$vp%1").arg(i);
            e.itemId = QStringLiteral("uid-vp%1").arg(i);
            e.roomId = roomId;
            e.sender = (i % 5 == 0)
                           ? controller.settings()->userId()
                           : QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("viewport observer probe %1").arg(i);
            e.timestamp = base.addSecs(i * 60);
            events.append(e);
        }
        TimelineEvent threadRoot;
        threadRoot.eventId = QStringLiteral("$vp-threadroot");
        threadRoot.itemId = QStringLiteral("uid-vp-threadroot");
        threadRoot.roomId = roomId;
        threadRoot.sender = QStringLiteral("@alice:mock.local");
        threadRoot.senderDisplayName = QStringLiteral("Alice");
        threadRoot.body = QStringLiteral("thread root with no latest ts");
        threadRoot.timestamp = base.addSecs(2500);
        threadRoot.isThreadRoot = true;
        threadRoot.threadReplyCount = 3;
        events.append(threadRoot);
        TimelineEvent editedRow;
        editedRow.eventId = QStringLiteral("$vp-edited");
        editedRow.itemId = QStringLiteral("uid-vp-edited");
        editedRow.roomId = roomId;
        editedRow.sender = QStringLiteral("@bob:mock.local");
        editedRow.senderDisplayName = QStringLiteral("Bob");
        editedRow.body = QStringLiteral("an edited message");
        editedRow.timestamp = base.addSecs(2560);
        editedRow.edited = true;
        events.append(editedRow);
        TimelineEvent ambiguous;
        ambiguous.eventId = QStringLiteral("$vp-ambiguous");
        ambiguous.itemId = QStringLiteral("uid-vp-ambiguous");
        ambiguous.roomId = roomId;
        ambiguous.sender = QStringLiteral("@bob2:mock.local");
        ambiguous.senderDisplayName = QStringLiteral("Bob");
        ambiguous.senderNameAmbiguous = true;
        ambiguous.body = QStringLiteral("same display name as Bob");
        ambiguous.timestamp = base.addSecs(2620);
        events.append(ambiguous);
        TimelineEvent divider;
        divider.itemId = QStringLiteral("uid-vp-divider");
        divider.roomId = roomId;
        divider.type = TimelineEvent::DateDivider;
        divider.timestamp = base;
        events.append(divider);
        TimelineEvent marker;
        marker.itemId = QStringLiteral("uid-vp-marker");
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        events.append(marker);
        mock->resetTimelineForTest(roomId, events,0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 20,
                                 5000);

        int totalItems = 0;
        QStringList observers;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
            ++totalItems;
            if (item->flags() & QQuickItem::ItemObservesViewport) {
                QStringList chain;
                const QQuickItem *up = item;
                for (int d = 0; d < 6 && up; ++d) {
                    QString seg = QString::fromLatin1(
                                      up->metaObject()->className())
                                      .section(QLatin1Char('_'), 0, 0);
                    if (!up->objectName().isEmpty())
                        seg += QLatin1Char('#') + up->objectName();
                    chain.prepend(seg);
                    up = up->parentItem();
                }
                observers << QStringLiteral("[vis=%1 text='%2'] %3")
                                 .arg(item->isVisible())
                                 .arg(item->property("text").toString()
                                          .left(20))
                                 .arg(chain.join(QStringLiteral(" > ")));
            }
            const auto children = item->childItems();
            for (QQuickItem *c : children)
                walk(c);
        };
        walk(timeline);

        const auto names =
            findVisualChildren(timeline, QStringLiteral("senderName"));
        QVERIFY2(!names.isEmpty(), "no senderName Label was created at all");
        int visibleNamed = 0;
        for (QQuickItem *n : names) {
            if (n->isVisible()
                && n->property("text").toString() == QStringLiteral("Alice"))
                ++visibleNamed;
        }
        QVERIFY2(visibleNamed > 0,
                 "the identity header Loader produced no visible sender name");
        const auto headers =
            findVisualChildren(timeline, QStringLiteral("senderIdentityHeader"));
        QVERIFY(!headers.isEmpty());
        QQuickItem *header = headers.first();
        QVERIFY2(header->width() > 0 && header->height() > 0,
                 qPrintable(QStringLiteral("header collapsed: %1x%2")
                                .arg(header->width()).arg(header->height())));

        QVERIFY2(totalItems > 200,
                 qPrintable(QStringLiteral("only %1 items instantiated — the "
                                           "fixture did not build real rows")
                                .arg(totalItems)));
        QVERIFY2(observers.isEmpty(),
                 qPrintable(QStringLiteral("%1 permanent viewport observers:\n%2")
                                .arg(observers.size())
                                .arg(observers.join(QStringLiteral("\n")))));
    }

    void aDownloadingFileRowAloneBuildsItsNoticeAndObservesNothing()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        SettingsManager *settings = controller.settings();
        const bool askedBefore = settings->alwaysAskWhereToSave();
        const QString folderBefore = settings->downloadFolder();
        const auto restore = qScopeGuard([&] {
            settings->setAlwaysAskWhereToSave(askedBefore);
            settings->setDownloadFolder(folderBefore);
        });
        settings->setAlwaysAskWhereToSave(false);
        settings->setDownloadFolder(folder.path());
        controller.setCurrentRoomId(roomId);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(1100, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QString key = QStringLiteral("mxc-probe-downloading");
        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-3600);
        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$dl%1").arg(i);
            e.itemId = QStringLiteral("uid-dl%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("download notice probe %1").arg(i);
            e.timestamp = base.addSecs(i * 60);
            events.append(e);
        }
        const auto fileRow = [&](const QString &id, const QString &mediaKey,
                                 int minute) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$") + id;
            e.itemId = QStringLiteral("uid-") + id;
            e.roomId = roomId;
            e.type = TimelineEvent::File;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("probe.pdf");
            e.mediaFilename = QStringLiteral("probe.pdf");
            e.mediaMimetype = QStringLiteral("application/pdf");
            e.mediaSize = 3000000;
            e.mediaKey = mediaKey;
            e.timestamp = base.addSecs(minute * 60);
            return e;
        };
        events.append(fileRow(QStringLiteral("dl-idle"),
                              QStringLiteral("mxc-probe-idle"), 40));
        events.append(fileRow(QStringLiteral("dl-busy"), key, 41));
        const int busyIndex = int(events.size()) - 1;
        mock->resetTimelineForTest(roomId, events,0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 20,
                                 5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            findVisualChildren(timeline, QStringLiteral("fileCard")).size(),
            qsizetype(2), 5000);
        const QString notice = QStringLiteral("attachmentDownloadNotice");
        QCOMPARE(findVisualChildren(timeline, notice).size(), qsizetype(0));

        controller.downloads()->download(key, QStringLiteral("probe.pdf"),
                                         QStringLiteral("application/pdf"));
        QCOMPARE(controller.downloads()->items().size(), qsizetype(1));
        QTRY_COMPARE_WITH_TIMEOUT(findVisualChildren(timeline, notice).size(),
                                  qsizetype(1), 5000);
        QPointer<QQuickItem> built = findVisualChildren(timeline, notice).first();
        QCOMPARE(built->property("mediaKey").toString(), key);
        QPointer<QQuickItem> row =
            qvariant_cast<QQuickItem *>(built->property("row"));
        QVERIFY(row);
        QCOMPARE(row->objectName(), QStringLiteral("messageDelegateRoot"));

        auto *pane = root->findChild<QQuickItem *>(
            QStringLiteral("paneDownloadsCard"));
        QVERIFY(pane != nullptr);
        const auto paneCount = [&] {
            QQmlExpression count(qmlContext(pane), pane,
                                 QStringLiteral("items.length"));
            return count.evaluate().toInt();
        };
        QTRY_COMPARE_WITH_TIMEOUT(int(built->isVisible()) + paneCount(), 1,
                                  5000);

        int totalItems = 0;
        const auto observersIn = [&](QQuickItem *top) {
            QStringList found;
            std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
                ++totalItems;
                if (item->flags() & QQuickItem::ItemObservesViewport) {
                    QStringList chain;
                    const QQuickItem *up = item;
                    for (int d = 0; d < 6 && up; ++d) {
                        QString seg = QString::fromLatin1(
                                          up->metaObject()->className())
                                          .section(QLatin1Char('_'), 0, 0);
                        if (!up->objectName().isEmpty())
                            seg += QLatin1Char('#') + up->objectName();
                        chain.prepend(seg);
                        up = up->parentItem();
                    }
                    found << chain.join(QStringLiteral(" > "));
                }
                const auto children = item->childItems();
                for (QQuickItem *c : children)
                    walk(c);
            };
            walk(top);
            return found;
        };
        QVERIFY(!findVisualChildren(built, QStringLiteral("downloadNoticeStatus"))
                     .isEmpty());
        QVERIFY(!findVisualChildren(built, QStringLiteral("downloadDismissButton"))
                     .isEmpty()
                || !findVisualChildren(built, QStringLiteral("downloadCancelButton"))
                        .isEmpty());
        QStringList observers = observersIn(timeline);
        QVERIFY2(totalItems > 200,
                 qPrintable(QStringLiteral("only %1 items instantiated")
                                .arg(totalItems)));
        QVERIFY2(observers.isEmpty(),
                 qPrintable(QStringLiteral("%1 permanent viewport observers "
                                           "with a download:\n%2")
                                .arg(observers.size())
                                .arg(observers.join(QLatin1Char('\n')))));

        QQmlExpression indexOf(qmlContext(row), row, QStringLiteral("index"));
        const int rowIndex = indexOf.evaluate().toInt();
        QVERIFY(!indexOf.hasError());
        QQmlExpression minY(qmlContext(timeline), timeline,
                            QStringLiteral("wheelMinY()"));
        const qreal bottomY = minY.evaluate().toReal();
        QVERIFY(!minY.hasError());
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("viewportNewestRow").toInt() <= rowIndex
                && timeline->property("viewportOldestRow").toInt() >= rowIndex,
            5000);
        QTRY_COMPARE_WITH_TIMEOUT(int(built->isVisible()) + paneCount(), 1,
                                  5000);
        const bool besideBefore = built->isVisible();
        QVERIFY(timeline->setProperty("stickToBottom", false));
        timeline->setProperty("contentY", row->y() + row->height() + 40);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("viewportNewestRow").toInt() > rowIndex, 5000);
        QVERIFY2(row->property("rowOnScreen").toBool(),
                 "the row left the activation range: the case lost its point");
        QTRY_COMPARE_WITH_TIMEOUT(paneCount(), 1, 5000);
        QVERIFY(built);
        QVERIFY(!built->isVisible());
        timeline->setProperty("contentY", bottomY);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("viewportNewestRow").toInt() <= rowIndex
                && timeline->property("viewportOldestRow").toInt() >= rowIndex,
            5000);
        QVERIFY(built);
        QTRY_COMPARE_WITH_TIMEOUT(built->isVisible(), besideBefore, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(paneCount(), besideBefore ? 0 : 1, 5000);

        TimelineEvent changed = events.at(busyIndex);
        changed.mediaKey.clear();
        mock->changeEventAtForTest(roomId, busyIndex, changed);
        QTRY_VERIFY_WITH_TIMEOUT(built.isNull(), 5000);
        QVERIFY2(row, "the row was rebuilt; the case must exercise the Loader");
        QCOMPARE(findVisualChildren(timeline, notice).size(), qsizetype(0));
        QTRY_COMPARE_WITH_TIMEOUT(paneCount(), 1, 5000);
        totalItems = 0;
        observers = observersIn(timeline);
        QVERIFY2(observers.isEmpty(),
                 qPrintable(observers.join(QLatin1Char('\n'))));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void viewportRowsIgnoreHiddenRoomActivity_data()
    {
        QTest::addColumn<bool>("roomActivity");
        QTest::addColumn<int>("notLaidOut");
        QTest::newRow("activity-off-invisible") << false << 10;
        QTest::newRow("activity-on-collapsed-zero-height") << true << 9;
    }
    void viewportRowsIgnoreHiddenRoomActivity()
    {
        QFETCH(bool, roomActivity);
        QFETCH(int, notLaidOut);
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const bool activityBefore = controller.settings()->showRoomActivity();
        const auto restore = qScopeGuard([&] {
            controller.settings()->setShowRoomActivity(activityBefore);
        });
        controller.settings()->setShowRoomActivity(roomActivity);
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            hiddenActivitySandwich(roomId,false),
0,420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        int hidden = 0;
        int visibleZero = 0;
        for (int row = 25; row < 35; ++row) {
            QQuickItem *item = itemAtViewRow(timeline, row);
            if (item && (!item->isVisible() || item->height() <= 0))
                ++hidden;
            if (item && item->isVisible() && item->height() <= 0
                && item->y() <= 0)
                ++visibleZero;
        }
        QCOMPARE(hidden, notLaidOut);
        if (roomActivity)
            QCOMPARE(visibleZero, notLaidOut);

        const qreal height = timeline->property("height").toReal();
        const qreal topMargin = timeline->property("topMargin").toReal();
        const auto check = [&](const char *where) {
            const qreal contentY = timeline->property("contentY").toReal();
            const qreal topY = contentY + std::max<qreal>(0, height - topMargin - 1);
            const int expectedOldest = laidOutRowAt(timeline, topY);
            const int expectedNewest =
                laidOutRowAt(timeline, std::max<qreal>(0, contentY));
            QTRY_COMPARE_WITH_TIMEOUT(
                timeline->property("viewportOldestRow").toInt(),
                expectedOldest, 3000);
            QCOMPARE(timeline->property("viewportNewestRow").toInt(),
                     std::max(0, expectedNewest));
            QVERIFY2(expectedOldest >= 0 && expectedOldest < 25,
                     qPrintable(QStringLiteral("%1: the physical top is not "
                                               "in the newer run (row %2)")
                                    .arg(QLatin1String(where))
                                    .arg(expectedOldest)));
        };
        check("live edge");
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QQuickItem *row20 = itemAtViewRow(timeline, 20);
        QVERIFY(row20 != nullptr);
        timeline->setProperty(
            "contentY", row20->y() + row20->height() / 2
                            - std::max<qreal>(0, height - topMargin - 1));
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("viewportNewestRow").toInt() > 0, 3000);
        check("parked");
    }

    void firstUnreadAboveHiddenRoomActivityIsNotOnScreen_data()
    {
        QTest::addColumn<bool>("roomActivity");
        QTest::newRow("activity-off-invisible") << false;
        QTest::newRow("activity-on-collapsed-zero-height") << true;
    }
    void firstUnreadAboveHiddenRoomActivityIsNotOnScreen()
    {
        QFETCH(bool, roomActivity);
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const bool activityBefore = controller.settings()->showRoomActivity();
        const auto restore = qScopeGuard([&] {
            controller.settings()->setShowRoomActivity(activityBefore);
        });
        controller.settings()->setShowRoomActivity(roomActivity);
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            hiddenActivitySandwich(roomId,true),
0,420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        QCOMPARE(controller.timeline()->readMarkerRow(), 30);
        QCOMPARE(timeline->property("firstUnreadViewRow").toInt(), 25);
        QVERIFY(timeline->property("stickToBottom").toBool());

        QQuickItem *markerRow = itemAtViewRow(timeline, 25);
        QVERIFY(markerRow != nullptr);
        const qreal height = timeline->property("height").toReal();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("viewportOldestRow").toInt() >= 0, 3000);
        QVERIFY2(markerRow->y()
                     > timeline->property("contentY").toReal() + height,
                 "fixture: the marker is on screen at the live edge");
        QTest::qWait(100);
        QVERIFY2(!timeline->property("firstUnreadOnScreen").toBool(),
                 qPrintable(QStringLiteral("the marker a viewport away counts "
                                           "as on screen (oldest row %1)")
                                .arg(timeline->property("viewportOldestRow")
                                         .toInt())));
        QVERIFY(!timeline->property("firstUnreadBelow").toBool());

        QVERIFY(timeline->setProperty("stickToBottom", false));
        timeline->setProperty("contentY", markerRow->y() - height / 2);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("firstUnreadOnScreen").toBool(), 3000);
    }

    void jumpToLatestGlidesFromNearbyAndJumpsFromFar()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(700, 400);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-9000);
        QList<TimelineEvent> events;
        for (int i = 0; i < 220; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$jump%1").arg(i);
            e.itemId = QStringLiteral("uid-jump%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("jump probe %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 200,
                                 5000);

        const qreal viewportHeight = timeline->property("height").toReal();
        QVERIFY(viewportHeight > 0);
        const int smoothViewports =
            timeline->property("smoothJumpViewports").toInt();
        QVERIFY(smoothViewports > 0);
        QQmlExpression minY(qmlContext(timeline), timeline,
                            QStringLiteral("wheelMinY()"));
        const qreal bottomY = minY.evaluate().toReal();
        QVERIFY(!minY.hasError());
        QQmlExpression maxY(qmlContext(timeline), timeline,
                            QStringLiteral("wheelMaxY()"));
        const qreal topY = maxY.evaluate().toReal();
        QVERIFY(!maxY.hasError());
        QVERIFY2(topY - bottomY > viewportHeight * (smoothViewports + 1),
                 qPrintable(QStringLiteral("content only %1px for a %2px "
                                           "threshold")
                                .arg(topY - bottomY)
                                .arg(viewportHeight * smoothViewports)));

        QVERIFY(timeline->setProperty("stickToBottom", false));
        timeline->setProperty("contentY", bottomY + viewportHeight);
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentY").toReal() > bottomY + 1.0, 5000);
        QQmlExpression jumpNear(qmlContext(timeline), timeline,
                                QStringLiteral("goToLatest()"));
        jumpNear.evaluate();
        QVERIFY2(!jumpNear.hasError(),
                 jumpNear.error().toString().toUtf8().constData());
        QVERIFY(controller.timelineScroll()->motionActive());
        QVERIFY(timeline->property("followLatestOnArrival").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
            !controller.timelineScroll()->motionActive(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("stickToBottom").toBool(),
                                 5000);
        QVERIFY(!timeline->property("followLatestOnArrival").toBool());

        QVERIFY(timeline->setProperty("stickToBottom", false));
        timeline->setProperty(
            "contentY",
            bottomY + viewportHeight * (smoothViewports + 0.5));
        QCoreApplication::processEvents();
        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("contentY").toReal()
                > bottomY + viewportHeight * smoothViewports,
            qPrintable(QStringLiteral("contentY %1 bottomY %2 vp %3 smooth %4 "
                                      "max %5")
                           .arg(timeline->property("contentY").toReal())
                           .arg(bottomY).arg(viewportHeight)
                           .arg(smoothViewports).arg(topY)), 5000);
        QQmlExpression jumpFar(qmlContext(timeline), timeline,
                               QStringLiteral("goToLatest()"));
        jumpFar.evaluate();
        QVERIFY2(!jumpFar.hasError(),
                 jumpFar.error().toString().toUtf8().constData());
        QVERIFY(!controller.timelineScroll()->motionActive());
        QVERIFY(!timeline->property("followLatestOnArrival").toBool());
        QVERIFY(timeline->property("stickToBottom").toBool());
    }

    static QQuickItem *deepHistoryPane(AppController &controller,
                                       QQmlApplicationEngine &engine,
                                       QQuickWindow &window,
                                       int rows,
                                       double parkFraction,
                                       int viewportHeight = 420)
    {
        const QString roomId = loginAndRoomIdAt(controller,0);
        if (roomId.isEmpty())
            return nullptr;
        auto *mock = controller.findChild<MockMatrixClient *>();
        if (!mock)
            return nullptr;
        controller.setCurrentRoomId(roomId);

        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return nullptr;
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!root)
            return nullptr;
        window.resize(700, viewportHeight);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-40000);
        QList<TimelineEvent> events;
        for (int i = 0; i < rows; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$win%1").arg(i);
            e.itemId = QStringLiteral("uid-win%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("window probe %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        if (!timeline)
            return nullptr;
        if (!QTest::qWaitFor([&] {
                return timeline->property("presentationReady").toBool()
                       && timeline->property("count").toInt() > rows - 100;
            }, 5000))
            return nullptr;

        timeline->setProperty("stickToBottom", false);
        double minY = 0.0;
        double maxY = 0.0;
        if (!wheelBounds(timeline, &minY, &maxY))
            return nullptr;
        timeline->setProperty("contentY", maxY * parkFraction);
        QCoreApplication::processEvents();
        return timeline;
    }

    double pointerEventMicros(int rows, bool hover)
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, rows, 0.5);
        if (!timeline)
            return -1;
        QTest::qWait(300);
        const QPointF pos(window.width() / 2.0, window.height() / 2.0);
        const int events = 400;
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < events; ++i) {
            if (hover) {
                const QPoint p(int(pos.x()) + (i % 2 ? 3 : -3),
                               int(pos.y()) + (i % 5));
                QMouseEvent move(QEvent::MouseMove, p, window.mapToGlobal(p),
                                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&window, &move);
            } else {
                QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                                  QPoint(0, 0), QPoint(0, i % 2 ? 120 : -120),
                                  Qt::NoButton, Qt::NoModifier,
                                  Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(&window, &wheel);
            }
        }
        return t.nsecsElapsed() / 1000.0 / events;
    }

    void pointerDeliveryCostDoesNotGrowWithLoadedRows()
    {
        for (const bool hover : {false, true}) {
            const double n500 = pointerEventMicros(500, hover);
            const double n1000 = pointerEventMicros(1000, hover);
            const double n2000 = pointerEventMicros(2000, hover);
            qInfo("pointer delivery %s us/event: 500 rows %.1f, 1000 rows %.1f, "
                  "2000 rows %.1f", hover ? "hover" : "wheel", n500, n1000,
                  n2000);
            QVERIFY(n500 > 0 && n2000 > 0);
            if (hover)
                continue;
            QVERIFY2(n2000 < n500 * 2.5 + 40,
                     qPrintable(QStringLiteral("%1 delivery grew with rows: "
                                "%2 -> %3 us/event")
                                    .arg(hover ? "hover" : "wheel")
                                    .arg(n500).arg(n2000)));
        }
    }

    void renderCostWithOffscreenRowClipping()
    {
        if (qEnvironmentVariableIsEmpty("LIGHTNING_RENDER_BENCH"))
            QSKIP("set LIGHTNING_RENDER_BENCH=1 to run");
        for (const int rows : {500, 1000, 2000}) {
            AppController controller(AppController::MockBackend);
            QQmlApplicationEngine engine;
            QQuickWindow window;
            QQuickItem *timeline =
                deepHistoryPane(controller, engine, window, rows, 0.5);
            QVERIFY(timeline != nullptr);
            QTest::qWait(500);
            window.grabWindow();
            QElapsedTimer t;
            t.start();
            const int frames = 60;
            for (int i = 0; i < frames; ++i) {
                timeline->setProperty("contentY",
                    timeline->property("contentY").toReal() + 7);
                window.grabWindow();
            }
            qInfo("render bench rows=%d: %.2f ms/frame", rows,
                  t.nsecsElapsed() / 1e6 / frames);
        }
    }

    void rowWindowBoundsRowsWithoutMovingTheReadersMessage()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(700, 420);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-40000);
        QList<TimelineEvent> events;
        for (int i = 0; i < 900; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$win%1").arg(i);
            e.itemId = QStringLiteral("uid-win%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("window probe %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 800,
                                 5000);
        const int rowsBefore = timeline->property("count").toInt();

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QQmlExpression maxY(qmlContext(timeline), timeline,
                            QStringLiteral("wheelMaxY()"));
        const qreal topY = maxY.evaluate().toReal();
        QVERIFY(!maxY.hasError());
        timeline->setProperty("contentY", topY * 0.6);
        QCoreApplication::processEvents();

        QQmlExpression probe(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ updateVisibleRowRange();"
                           " var r = viewRowAtContentY(contentY);"
                           " var it = itemAtViewRow(r);"
                           " return [eventIdAtViewRow(r),"
                           "         it ? it.y - contentY : 0]; })()"));
        const QVariantList before = probe.evaluate().toList();
        QVERIFY2(!probe.hasError(),
                 probe.error().toString().toUtf8().constData());
        const QString anchorId = before.value(0).toString();
        const qreal anchorOffset = before.value(1).toReal();
        QVERIFY2(!anchorId.isEmpty(), "no anchor event under the viewport");

        QSignalSpy removed(controller.timelineView(),
                           &QAbstractItemModel::rowsRemoved);
        QSignalSpy inserted(controller.timelineView(),
                            &QAbstractItemModel::rowsInserted);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);

        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        const int rowsAfter = timeline->property("count").toInt();
        QVERIFY2(rowsAfter < rowsBefore,
                 qPrintable(QStringLiteral("no rows released: %1 -> %2")
                                .arg(rowsBefore).arg(rowsAfter)));
        QVERIFY2(rowsAfter <= 520,
                 qPrintable(QStringLiteral("window too loose: %1 rows")
                                .arg(rowsAfter)));
        QVERIFY(QMetaObject::invokeMethod(timeline, "refreshMediaBand"));
        QVERIFY2(timeline->property("mediaRetainFirstRow").toInt() > 0,
                 "the decoded-image range still retains every newer row");
        auto *outsidePictureRange = itemAtViewRow(timeline, 0);
        QVERIFY(outsidePictureRange != nullptr);
        QVERIFY(!outsidePictureRange->property("mediaPixmapRetained").toBool());
        auto *readerPictureRange = itemAtViewRow(
            timeline, timeline->property("visibleFirstRow").toInt());
        QVERIFY(readerPictureRange != nullptr);
        QVERIFY(readerPictureRange->property("mediaPixmapRetained").toBool());
        QVERIFY2(removed.size() > 1,
                 "the window was trimmed in a single GUI-thread operation");
        for (const auto &change : removed) {
            const int first = change.at(1).toInt();
            const int last = change.at(2).toInt();
            QVERIFY2(last - first + 1 <= 8,
                     "a trim removed more than one frame's row budget");
        }
        for (const auto &change : inserted) {
            const int first = change.at(1).toInt();
            const int last = change.at(2).toInt();
            QVERIFY2(last - first + 1 <= 8,
                     "a trim inserted more than one frame's row budget");
        }

        QQmlExpression after(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = itemAtViewRow(r);"
                           " return it ? it.y - contentY : 1e9; })('")
                + anchorId + QStringLiteral("')"));
        const qreal afterOffset = after.evaluate().toReal();
        QVERIFY2(!after.hasError(),
                 after.error().toString().toUtf8().constData());
        QVERIFY2(qAbs(afterOffset - anchorOffset) <= 2.0,
                 qPrintable(QStringLiteral("reader moved %1 px (%2 -> %3)")
                                .arg(afterOffset - anchorOffset)
                                .arg(anchorOffset).arg(afterOffset)));

        const int skipAfter = timeline->property("rowWindowSkip").toInt();
        QVERIFY2(skipAfter > 0,
                 "the window only trimmed the oldest end — the skip path, "
                 "which is the one that shifts the reader, was never taken");
        QQmlExpression atBottom(qmlContext(timeline), timeline,
                                QStringLiteral("atBottomEdge()"));
        QCOMPARE(atBottom.evaluate().toBool(), false);
        QVERIFY(!timeline->property("stickToBottom").toBool());

        QQmlExpression probe2(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ contentY = wheelMinY() + 40;"
                           " updateVisibleRowRange();"
                           " var r = viewRowAtContentY(contentY);"
                           " var it = itemAtViewRow(r);"
                           " return [eventIdAtViewRow(r),"
                           "         it ? it.y - contentY : 0]; })()"));
        const QVariantList back = probe2.evaluate().toList();
        QVERIFY2(!probe2.hasError(),
                 probe2.error().toString().toUtf8().constData());
        const QString backId = back.value(0).toString();
        const qreal backOffset = back.value(1).toReal();
        QVERIFY(!backId.isEmpty());
        QCoreApplication::processEvents();

        QQmlExpression apply2(qmlContext(timeline), timeline,
                              QStringLiteral("applyRowWindow()"));
        apply2.evaluate();
        QVERIFY2(!apply2.hasError(),
                 apply2.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);

        QVERIFY2(timeline->property("rowWindowSkip").toInt() < skipAfter,
                 qPrintable(QStringLiteral("skip did not come back down: %1")
                                .arg(timeline->property("rowWindowSkip")
                                         .toInt())));
        QQmlExpression after2(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = itemAtViewRow(r);"
                           " return it ? it.y - contentY : 1e9; })('")
                + backId + QStringLiteral("')"));
        const qreal after2Offset = after2.evaluate().toReal();
        QVERIFY2(!after2.hasError(),
                 after2.error().toString().toUtf8().constData());
        QVERIFY2(qAbs(after2Offset - backOffset) <= 2.0,
                 qPrintable(QStringLiteral("reader moved %1 px on restore "
                                           "(%2 -> %3)")
                                .arg(after2Offset - backOffset)
                                .arg(backOffset).arg(after2Offset)));
        QCOMPARE(timeline->property("diagWindowUnmeasuredRows").toInt(), 0);
    }

    void newestWindowExtensionBuildsOnlyAFrameBudgetAndKeepsTheAnchor()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);
        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        QVERIFY(timeline->property("rowWindowSkip").toInt() > 3);

        QQmlExpression anchor(qmlContext(timeline), timeline,
            QStringLiteral("(function(){ var r = viewRowAtContentY(contentY);"
                           " var it = itemAtViewRow(r);"
                           " return [eventIdAtViewRow(r), it.y - contentY]; })()"));
        const QVariantList before = anchor.evaluate().toList();
        QVERIFY2(!anchor.hasError(),
                 anchor.error().toString().toUtf8().constData());
        QVERIFY(!before.at(0).toString().isEmpty());
        QSignalSpy inserted(controller.timelineView(),
                            &QAbstractItemModel::rowsInserted);
        QQmlExpression extend(qmlContext(timeline), timeline,
                              QStringLiteral("extendRowWindowAtNewEnd()"));
        QVERIFY(extend.evaluate().toBool());
        QVERIFY2(!extend.hasError(),
                 extend.error().toString().toUtf8().constData());
        QVERIFY(!inserted.isEmpty());
        QCOMPARE(inserted.first().at(2).toInt()
                 - inserted.first().at(1).toInt() + 1, 3);

        QQmlExpression after(qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = itemAtViewRow(r);"
                           " return it.y - contentY; })('")
                + before.at(0).toString() + QStringLiteral("')"));
        const qreal offset = after.evaluate().toReal();
        QVERIFY2(!after.hasError(),
                 after.error().toString().toUtf8().constData());
        QVERIFY(qAbs(offset - before.at(1).toReal()) <= 2.0);
    }

    void rowWindowTrimNeverFeedsTheNearTopPaginationBand()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.6, 2160);
        QVERIFY(timeline != nullptr);
        const int rowsBefore = timeline->property("count").toInt();

        QVERIFY(timeline->setProperty("nearTopArmed", true));
        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);

        QVERIFY2(timeline->property("count").toInt() < rowsBefore,
                 "nothing was released, so the hazard was never exercised");
        QQmlExpression fromTop(
            qmlContext(timeline), timeline,
            QStringLiteral("[distanceFromTop(), nearTopEnterDistance]"));
        const QVariantList d = fromTop.evaluate().toList();
        QVERIFY2(!fromTop.hasError(),
                 fromTop.error().toString().toUtf8().constData());
        QVERIFY2(d.value(0).toReal() > d.value(1).toReal(),
                 qPrintable(QStringLiteral("trim left the reader INSIDE the "
                                           "near-top band: %1 <= %2")
                                .arg(d.value(0).toReal())
                                .arg(d.value(1).toReal())));
    }

    void jumpToLatestRestoresTheLiveEdgeWhenAWindowIsActive()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.6);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        QVERIFY2(timeline->property("rowWindowSkip").toInt() > 0,
                 "no window was established, so the bug is unreachable here");

        QQmlExpression jump(qmlContext(timeline), timeline,
                            QStringLiteral("goToLatest()"));
        jump.evaluate();
        QVERIFY2(!jump.hasError(),
                 jump.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();

        // The mock has no event cache, so the history trim refuses and this
        // falls through to settleAtLatest(), which must restore the live edge.
        QCOMPARE(timeline->property("rowWindowSkip").toInt(), 0);
        QVERIFY(timeline->property("stickToBottom").toBool());
        QQmlExpression newest(
            qmlContext(timeline), timeline,
            QStringLiteral("[eventIdAtViewRow(0), atBottomEdge()]"));
        const QVariantList landed = newest.evaluate().toList();
        QVERIFY2(!newest.hasError(),
                 newest.error().toString().toUtf8().constData());
        QCOMPARE(landed.value(0).toString(), QStringLiteral("$win899"));
        QCOMPARE(landed.value(1).toBool(), true);
    }

    void lateHeightChangeAfterAWindowRestoreIsAbsorbedByTheAnchor()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.6);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        QVERIFY(timeline->property("rowWindowSkip").toInt() > 0);

        QQmlExpression probe(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ updateVisibleRowRange();"
                           " var r = viewRowAtContentY(contentY);"
                           " var it = itemAtViewRow(r);"
                           " return [eventIdAtViewRow(r),"
                           "         it ? it.y - contentY : 0, r]; })()"));
        const QVariantList before = probe.evaluate().toList();
        QVERIFY2(!probe.hasError(),
                 probe.error().toString().toUtf8().constData());
        const QString anchorId = before.value(0).toString();
        const qreal anchorOffset = before.value(1).toReal();
        const int anchorRow = before.value(2).toInt();
        QVERIFY(!anchorId.isEmpty());
        QVERIFY2(anchorRow > 4, "reader too close to the newest row to have "
                                "a newer row to grow");

        QQmlExpression grow(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ var it = itemAtViewRow(%1);"
                           " if (!it) return false;"
                           " it.height = it.height + 140; return true; })()")
                .arg(anchorRow - 3));
        QVERIFY2(grow.evaluate().toBool(), "could not grow a newer row");

        QQmlExpression after(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = itemAtViewRow(r);"
                           " return it ? it.y - contentY : 1e9; })('")
                + anchorId + QStringLiteral("')"));
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(after.evaluate().toReal() - anchorOffset) <= 2.0, 3000);
    }

    void reachingTheWindowsOldEdgeExposesLocalRowsInsteadOfAskingTheServer()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.6);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);

        const int skip = timeline->property("rowWindowSkip").toInt();
        const int exposed = timeline->property("count").toInt();
        const int sourceTotal = controller.timeline()->rowCount();
        QVERIFY2(skip > 0, "no window established");
        QVERIFY2(skip + exposed < sourceTotal,
                 qPrintable(QStringLiteral(
                     "the window holds nothing back at the OLD end "
                     "(skip %1 + exposed %2 vs total %3), so the boundary "
                     "this test is about does not exist here")
                                .arg(skip).arg(exposed).arg(sourceTotal)));

        QQmlExpression toEdge(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ contentY = wheelMaxY();"
                           " nearTopArmed = true;"
                           " nearTopRequestDistance = Infinity;"
                           " checkNearTopEdge(true); return true; })()"));
        QVERIFY(toEdge.evaluate().toBool());
        QVERIFY2(!toEdge.hasError(),
                 toEdge.error().toString().toUtf8().constData());

        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("count").toInt() > exposed, 4000);
        QCOMPARE(controller.timeline()->rowCount(), sourceTotal);
        QCOMPARE(timeline->property("rowWindowSkip").toInt(), skip);
    }

    void wheelScrollingIntoHistoryEventuallyBoundsRowsThroughTheSettleTimer()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.0);
        QVERIFY(timeline != nullptr);
        const int rowsBefore = timeline->property("count").toInt();
        QVERIFY(rowsBefore > 800);

        const QPointF pos(window.width() / 2.0, window.height() / 2.0);
        const qreal startY = timeline->property("contentY").toReal();
        for (int i = 0; i < 240; ++i) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 0), QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentY").toReal() > startY + 2000, 5000);
        const qreal deepY = timeline->property("contentY").toReal();
        QVERIFY2(deepY > startY + 2000,
                 qPrintable(QStringLiteral(
                     "the wheel notches did not travel into history "
                     "(contentY %1 -> %2); this test proves nothing")
                                .arg(startY).arg(deepY)));

        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("rowWindowSkip").toInt() > 0, 5000);
        QVERIFY2(timeline->property("count").toInt() < rowsBefore,
                 qPrintable(QStringLiteral(
                     "a window was established but bounded nothing: %1 rows")
                                .arg(timeline->property("count").toInt())));
    }

    void speculativeMediaIsBlockedDuringAGestureAndResumesOnSettle()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(700, 420);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-6000);
        QList<TimelineEvent> events;
        for (int i = 0; i < 160; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$spec%1").arg(i);
            e.itemId = QStringLiteral("uid-spec%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("speculative probe %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 100,
                                 5000);

        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("speculativeMediaAllowed").toBool(), 5000);

        const QPointF pos(window.width() / 2.0, window.height() / 2.0);
        bool blockedDuringGesture = false;
        for (int i = 0; i < 12; ++i) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 0), QPoint(0, -120),
                              Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            if (!timeline->property("speculativeMediaAllowed").toBool())
                blockedDuringGesture = true;
        }
        QVERIFY2(blockedDuringGesture,
                 "speculative media was never gated during a wheel gesture");

        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("speculativeMediaAllowed").toBool(), 5000);
    }

    void historyTrimPolicyRefusesEveryUnsafeCombination()
    {
        const int rows = 500;
        const int threshold = 400;
        QVERIFY(AppController::historyTrimAllowed(
true,true,false,
false, rows, threshold));
        QVERIFY(!AppController::historyTrimAllowed(false, true, false, false,
                                                  rows, threshold));
        QVERIFY(!AppController::historyTrimAllowed(true, false, false, false,
                                                  rows, threshold));
        QVERIFY(!AppController::historyTrimAllowed(true, true, true, false,
                                                  rows, threshold));
        QVERIFY(!AppController::historyTrimAllowed(true, true, false, true,
                                                  rows, threshold));
        QVERIFY(!AppController::historyTrimAllowed(true, true, false, false,
                                                  threshold, threshold));
        QVERIFY(AppController::historyTrimAllowed(true, true, false, false,
                                                 threshold + 1, threshold));
        QVERIFY(!AppController::historyTrimAllowed(true, true, false, false,
                                                  12, threshold));
    }

    void idleLiveTrimRunsForAQuietUnfocusedReaderAtTheLiveEdge()
    {
        const qint64 q = AppController::kIdleLiveTrimQuietMs;
        QVERIFY(AppController::idleLiveTrimDue(true, q, false, false, false));
        QVERIFY(!AppController::idleLiveTrimDue(true, q - 1, false, false, false));
        QVERIFY(!AppController::idleLiveTrimDue(false, q * 10, false, false, false));
    }

    void idleLiveTrimIsRefusedWhileTheWindowIsActive()
    {
        const qint64 q = AppController::kIdleLiveTrimQuietMs;
        QVERIFY(!AppController::idleLiveTrimDue(true, q * 10, true, false, false));
    }

    void idleLiveTrimIsRefusedWhileAResetWouldDestroySomething()
    {
        const qint64 q = AppController::kIdleLiveTrimQuietMs;
        QVERIFY(!AppController::idleLiveTrimDue(true, q * 10, false, true, false));
    }

    void idleLiveTrimIsRefusedDuringAnAccountSwitch()
    {
        const qint64 q = AppController::kIdleLiveTrimQuietMs;
        QVERIFY(!AppController::idleLiveTrimDue(true, q * 10, false, false, true));
    }

    void jumpToLiveTrimIsRefusedByABackendWithNoEventCacheAndIsThenANoOp()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

        QVERIFY(controller.historyTrimRowThreshold() > 0);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(700, 400);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-9000);
        QList<TimelineEvent> events;
        for (int i = 0; i < 220; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$trim%1").arg(i);
            e.itemId = QStringLiteral("uid-trim%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("trim probe %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 200,
                                 5000);
        const int rowsBefore = controller.timeline()->rowCount();
        QVERIFY(rowsBefore > 200);

        QVERIFY(!controller.trimHistoryAndJumpToLive());
        const QString openRoom = controller.currentRoomId();
        controller.setCurrentRoomId(QString());
        QVERIFY(!controller.trimHistoryAndJumpToLive());
        controller.setCurrentRoomId(openRoom);
        QTRY_VERIFY_WITH_TIMEOUT(
            controller.timeline()->rowCount() > 0, 5000);

        const int smoothViewports =
            timeline->property("smoothJumpViewports").toInt();
        const qreal viewportHeight = timeline->property("height").toReal();
        QQmlExpression minY(qmlContext(timeline), timeline,
                            QStringLiteral("wheelMinY()"));
        const qreal bottomY = minY.evaluate().toReal();
        QVERIFY(!minY.hasError());
        QVERIFY(timeline->setProperty("stickToBottom", false));
        timeline->setProperty(
            "contentY",
            bottomY + viewportHeight * (smoothViewports + 0.5));
        QCoreApplication::processEvents();
        QQmlExpression jumpFar(qmlContext(timeline), timeline,
                               QStringLiteral("goToLatest()"));
        jumpFar.evaluate();
        QVERIFY2(!jumpFar.hasError(),
                 jumpFar.error().toString().toUtf8().constData());
        QVERIFY(timeline->property("stickToBottom").toBool());
        QCOMPARE(controller.timeline()->rowCount(), rowsBefore);
    }

    void paginationGeometryChangesAreQueuedAndBounded()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->failNextPaginationForTest();

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) {
                        // The offscreen host cannot resolve the mock media
                        // origin; the image fixture row's fetch logs this.
                        if (e.toString().contains(
                                QLatin1String("Host mock.local not found")))
                            continue;
                        warnings << e.toString();
                    }
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy stateSpy(controller.pagination(),
                            &PaginationController::stateChanged);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        auto *header = root->findChild<QQuickItem *>(
            QStringLiteral("paginationHeader"));
        QVERIFY(timeline != nullptr);
        QVERIFY(header != nullptr);

        for (const QSize size : { QSize(680, 420), QSize(900, 720),
                                  QSize(560, 360), QSize(720, 640) }) {
            root->setSize(QSizeF(size));
            QCoreApplication::processEvents();
        }

        QTRY_VERIFY_WITH_TIMEOUT(controller.pagination()->failed(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(header->height(), 32.0, 5000);
        controller.pagination()->retry();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(), 5000);
        QVERIFY(!controller.pagination()->failed());
        controller.pagination()->requestNearTop();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.pagination()->reachedStart(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(header->height(), 0.0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !timeline->property("viewportFillCheckScheduled").toBool(), 5000);
        QVERIFY2(stateSpy.count() < 40,
                 qPrintable(QStringLiteral("pagination state storm: %1")
                                .arg(stateSpy.count())));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void transientInitialHistoryFailureRetriesWithoutUserAction()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->failNextPaginationForTest(true);
        controller.pagination()->setAutomaticRetryPolicyForTest(3, 1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QVERIFY(createdSpy.at(0).at(0).value<QObject *>() != nullptr);

        QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), 5000);
        QVERIFY(!controller.pagination()->failed());
        QVERIFY(controller.timeline()->rowCount() > 0);
        QVERIFY2(completedSpy.count() < 10, "initial history request storm");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void populatedEncryptedTimelineRemainsResponsive()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString encryptedId = QStringLiteral("!devs:mock.local");
        controller.setCurrentRoomId(encryptedId);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        QTimer heartbeat;
        heartbeat.setSingleShot(true);
        QSignalSpy heartbeatSpy(&heartbeat, &QTimer::timeout);
        heartbeat.start(100);
        QVERIFY2(heartbeatSpy.wait(kSignalTimeoutMs),
                 "GUI event loop was starved by timeline delegate layout");
        QVERIFY(window.isVisible());

        for (const QSize size : { QSize(520, 360), QSize(940, 720),
                                  QSize(760, 620) }) {
            window.resize(size);
            root->setSize(QSizeF(size));
            QCoreApplication::processEvents();
        }
        controller.setCurrentRoomId(QStringLiteral("!dm-bob:mock.local"));
        QCoreApplication::processEvents();
        controller.setCurrentRoomId(encryptedId);

        heartbeatSpy.clear();
        heartbeat.start(100);
        QVERIFY2(heartbeatSpy.wait(kSignalTimeoutMs),
                 "room switch left timeline layout unresponsive");
        QVERIFY(window.isVisible());
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void longEncryptedMessageDelegateUsesBoundedStartupWidth()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QString{});
        fixture.insert(QStringLiteral("eventId"), QString{});
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"), QString{});
        fixture.insert(QStringLiteral("senderInitials"),
                       QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("Large encrypted fixture line.\n")
                           .repeated(160));
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("isEncrypted"), true);
        fixture.insert(QStringLiteral("isDecrypted"), true);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        root->setWidth(640);
        QCoreApplication::processEvents();

        auto *body = root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(body != nullptr);
        QVERIFY(body->property("text").toString().size() > 4000);
        QVERIFY(body->width() > 100.0);
        QVERIFY(body->height() > 0.0);
        QVERIFY(body->height() < 20000.0);
        QVERIFY(root->implicitHeight() < 20000.0);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void reactionChipsShareOneHeightAcrossDifferentEmoji()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QStringLiteral("fixture-item"));
        fixture.insert(QStringLiteral("eventId"), QStringLiteral("$fixture"));
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("Reaction fixture"));
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("isEncrypted"), false);
        fixture.insert(QStringLiteral("isDecrypted"), false);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{
            QVariantMap{ { QStringLiteral("key"), QStringLiteral("👍") },
                        { QStringLiteral("count"), 1 },
                        { QStringLiteral("byMe"), false } },
            QVariantMap{ { QStringLiteral("key"),
                          QStringLiteral("❤️") },
                        { QStringLiteral("count"), 12 },
                        { QStringLiteral("byMe"), true } },
            QVariantMap{ { QStringLiteral("key"),
                          QStringLiteral("👨‍👩‍👧‍👦") },
                        { QStringLiteral("count"), 2 },
                        { QStringLiteral("byMe"), false } },
            QVariantMap{ { QStringLiteral("key"), QStringLiteral("👍🏽") },
                        { QStringLiteral("count"), 3 },
                        { QStringLiteral("byMe"), false } },
            QVariantMap{ { QStringLiteral("key"), QStringLiteral("1️⃣") },
                        { QStringLiteral("count"), 4 },
                        { QStringLiteral("byMe"), false } },
            QVariantMap{ { QStringLiteral("key"), QStringLiteral("🇱🇹") },
                        { QStringLiteral("count"), 5 },
                        { QStringLiteral("byMe"), false } },
        });

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(640, 240);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        window.show();
        QCoreApplication::processEvents();

        const auto chips = findVisualChildren(root, QStringLiteral("reactionChip"));
        QCOMPARE(chips.size(), 6);
        QVERIFY2(chips.at(0)->height() >= 20.0,
                 "chip height below the 20px design floor");
        for (int i = 1; i < chips.size(); ++i) {
            QVERIFY2(qFuzzyCompare(chips.at(i)->height() + 1.0,
                                   chips.at(0)->height() + 1.0),
                     qPrintable(QStringLiteral("chip %1 is %2px tall against "
                                               "%3px for the first — a glyph "
                                               "grew its pill")
                                    .arg(i)
                                    .arg(chips.at(i)->height())
                                    .arg(chips.at(0)->height())));
        }

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void rightClickMenuSnapshotsStableEventAndClosesOnRoomSwitch()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!devs:mock.local"));

        int messageRow = -1;
        for (int row = 0; row < controller.timeline()->rowCount(); ++row) {
            const QModelIndex idx = controller.timeline()->index(row);
            if (!controller.timeline()->data(
                    idx, TimelineModel::IsVirtualRole).toBool()
                && !controller.timeline()->data(
                    idx, TimelineModel::IsStateActivityRole).toBool()) {
                messageRow = row;
                break;
            }
        }
        QVERIFY(messageRow >= 0);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            fixture.insert(QString::fromUtf8(it.value()),
                           controller.timeline()->data(
                               controller.timeline()->index(messageRow), it.key()));
        }
        const QString eventId = fixture.value(QStringLiteral("eventId")).toString();
        QVERIFY(!eventId.isEmpty());

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(640, 240);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        window.show();
        QCoreApplication::processEvents();

        QTest::mouseClick(&window, Qt::RightButton, Qt::NoModifier,
                          QPoint(180, qMax(4, qRound(root->height() / 2))));
        QTRY_COMPARE_WITH_TIMEOUT(root->property("menuEventId").toString(),
                                  eventId, kSignalTimeoutMs);
        auto *menu = root->findChild<QObject *>(
            QStringLiteral("messageContextMenu"));
        QVERIFY(menu != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("opened").toBool(),
                                 kSignalTimeoutMs);

        const QString visibleText = controller.timeline()->visibleTextForEvent(eventId);
        QVERIFY(!visibleText.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(root, "copyToClipboard",
                                          Q_ARG(QVariant, visibleText)));
        QCOMPARE(QGuiApplication::clipboard()->text(), visibleText);
        const QString permalink = controller.timeline()->messagePermalink(eventId);
        QVERIFY(QMetaObject::invokeMethod(root, "copyToClipboard",
                                          Q_ARG(QVariant, permalink)));
        QCOMPARE(QGuiApplication::clipboard()->text(), permalink);
        QVERIFY(!permalink.contains(QStringLiteral("access_token"),
                                    Qt::CaseInsensitive));

        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY_WITH_TIMEOUT(!menu->property("opened").toBool(),
                                 kSignalTimeoutMs);
        root->forceActiveFocus();
        QVERIFY(root->hasActiveFocus());
        QTest::keyClick(&window, Qt::Key_Menu);
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("opened").toBool(),
                                 kSignalTimeoutMs);
        QCOMPARE(root->property("menuEventId").toString(), eventId);

        controller.setCurrentRoomId(QStringLiteral("!dm-bob:mock.local"));
        QTRY_VERIFY_WITH_TIMEOUT(!menu->property("opened").toBool(),
                                 kSignalTimeoutMs);
        QCOMPARE(root->property("menuEventId").toString(), QString{});
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void pooledDelegateReuseScrubsTransientState()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!devs:mock.local"));

        int messageRow = -1;
        for (int row = 0; row < controller.timeline()->rowCount(); ++row) {
            const QModelIndex idx = controller.timeline()->index(row);
            if (!controller.timeline()->data(idx, TimelineModel::IsVirtualRole).toBool()
                && !controller.timeline()->data(
                       idx, TimelineModel::IsStateActivityRole).toBool()) {
                messageRow = row;
                break;
            }
        }
        QVERIFY(messageRow >= 0);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            fixture.insert(QString::fromUtf8(it.value()),
                           controller.timeline()->data(
                               controller.timeline()->index(messageRow), it.key()));
        }

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QVERIFY(root->setProperty("menuEventId",
                                  QStringLiteral("$stale-menu:mock.local")));
        QCOMPARE(root->property("menuEventId").toString(),
                 QStringLiteral("$stale-menu:mock.local"));
        QVERIFY(!root->property("reactionEventId").isValid());

        QVERIFY(QMetaObject::invokeMethod(root, "resetForReuse"));

        QCOMPARE(root->property("menuEventId").toString(), QString{});
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void roomActivitySettingCollapsesOnlyActivityDelegates()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!devs:mock.local"));
        controller.settings()->setShowRoomActivity(true);

        int activityRow = -1;
        for (int row = 0; row < controller.timeline()->rowCount(); ++row) {
            if (controller.timeline()->data(
                    controller.timeline()->index(row),
                    TimelineModel::StateGroupLeaderRole).toBool()) {
                activityRow = row;
                break;
            }
        }
        QVERIFY(activityRow >= 0);
        const int underlyingCount = controller.timeline()->rowCount();

        const auto snapshot = [&controller, activityRow] {
            QVariantMap fixture;
            const auto roles = controller.timeline()->roleNames();
            for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
                fixture.insert(QString::fromUtf8(it.value()),
                               controller.timeline()->data(
                                   controller.timeline()->index(activityRow),
                                   it.key()));
            }
            return fixture;
        };
        const QVariantMap fixture = snapshot();

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *activity = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(activity != nullptr);
        QQuickWindow window;
        window.resize(640, 320);
        activity->setParentItem(window.contentItem());
        activity->setWidth(window.width());
        window.show();
        QCoreApplication::processEvents();
        QVERIFY(activity->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(activity->implicitHeight() > 0.0,
                                 kSignalTimeoutMs);

        controller.settings()->setShowRoomActivity(false);
        engine.rootContext()->setContextProperty("model", snapshot());
        QTRY_VERIFY_WITH_TIMEOUT(!activity->isVisible(), kSignalTimeoutMs);
        QCOMPARE(activity->implicitHeight(), 0.0);
        QCOMPARE(controller.timeline()->rowCount(), underlyingCount);

        controller.settings()->setShowRoomActivity(true);
        engine.rootContext()->setContextProperty("model", snapshot());
        QTRY_VERIFY_WITH_TIMEOUT(activity->isVisible(), kSignalTimeoutMs);
        QVERIFY(activity->implicitHeight() > 0.0);
        QCOMPARE(controller.timeline()->rowCount(), underlyingCount);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void settingsControlTracksRoomActivityPreference()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setShowRoomActivity(true);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("SettingsScreen"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *check = settingsItemNamed(root, QStringLiteral("showRoomActivityCheck"));
        QVERIFY(check != nullptr);
        QCOMPARE(check->property("checked").toBool(), true);

        controller.settings()->setShowRoomActivity(false);
        QTRY_COMPARE_WITH_TIMEOUT(check->property("checked").toBool(), false,
                                  kSignalTimeoutMs);
        controller.settings()->setShowRoomActivity(true);
        QTRY_COMPARE_WITH_TIMEOUT(check->property("checked").toBool(), true,
                                  kSignalTimeoutMs);
        QSignalSpy settingSpy(controller.settings(),
                              &SettingsManager::showRoomActivityChanged);
        QVERIFY(QMetaObject::invokeMethod(check, "click"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.settings()->showRoomActivity(),
                                  false, kSignalTimeoutMs);
        QCOMPARE(settingSpy.count(), 1);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void settingsControlTracksWheelSpeedPreference()
    {
        AppController controller(AppController::MockBackend);
        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);
        controller.settings()->setTimelineWheelSpeed(1);
        QCOMPARE(controller.settings()->timelineWheelSpeed(), 1);
        QCOMPARE(scroll->wheelSpeed(), TimelineScrollController::Fast);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("SettingsScreen"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *combo = settingsItemNamed(root, QStringLiteral("timelineWheelSpeedCombo"));
        QVERIFY(combo != nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentValue").toInt(), 1,
                                  kSignalTimeoutMs);

        controller.settings()->setTimelineWheelSpeed(2);
        QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentValue").toInt(), 2,
                                  kSignalTimeoutMs);
        QCOMPARE(scroll->wheelSpeed(), TimelineScrollController::VeryFast);

        controller.settings()->setTimelineWheelSpeed(0);
        QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentValue").toInt(), 0,
                                  kSignalTimeoutMs);
        QCOMPARE(scroll->wheelSpeed(), TimelineScrollController::Standard);
        QCOMPARE(realWarnings(warnings), QStringList{});
        controller.settings()->setTimelineWheelSpeed(1);
    }

    void sdkReadMarkerRendersNewMessagesDivider()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("itemId"),
                       QStringLiteral("read-marker-stable"));
        fixture.insert(QStringLiteral("isVirtual"), true);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("isRoutineActivity"), false);
        fixture.insert(QStringLiteral("eventType"), 8);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("eventId"), QString{});
        fixture.insert(QStringLiteral("sender"), QString{});
        fixture.insert(QStringLiteral("senderDisplayName"), QString{});
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("?"));
        fixture.insert(QStringLiteral("showSenderIdentity"), false);
        fixture.insert(QStringLiteral("body"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("edited"), false);
        fixture.insert(QStringLiteral("isEncrypted"), false);
        fixture.insert(QStringLiteral("isDecrypted"), false);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), false);
        fixture.insert(QStringLiteral("mediaThumbAvailable"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(640, 160);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        window.show();
        QCoreApplication::processEvents();

        auto *divider = root->findChild<QQuickItem *>(
            QStringLiteral("unreadDivider"));
        auto *label = root->findChild<QObject *>(
            QStringLiteral("unreadDividerLabel"));
        QVERIFY(divider != nullptr);
        QVERIFY(divider->isVisible());
        QVERIFY(root->implicitHeight() >= 28.0);
        QVERIFY(label != nullptr);
        QCOMPARE(label->property("text").toString(),
                 QStringLiteral("New messages"));

        fixture.insert(QStringLiteral("dividerIntroducesVisibleContent"), false);
        engine.rootContext()->setContextProperty("model", fixture);
        QCoreApplication::processEvents();
        QVERIFY(!root->isVisible());
        QCOMPARE(root->implicitHeight(), 0.0);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void jumpToLatestPreservesReaderUntilExplicitClick()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QObject *jump = root->findChild<QObject *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(timeline != nullptr);
        QVERIFY(jump != nullptr);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QTRY_COMPARE_WITH_TIMEOUT(jump->property("visible").toBool(), true,
                                  kSignalTimeoutMs);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const int before = controller.timeline()->rowCount();
        mock->sendTextMessage(roomId, QStringLiteral("new fixture event"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.timeline()->rowCount(), before + 1,
                                  kSignalTimeoutMs);
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);

        QVERIFY(QMetaObject::invokeMethod(jump, "click"));
        QTRY_COMPARE_WITH_TIMEOUT(timeline->property("stickToBottom").toBool(),
                                  true, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(jump->property("visible").toBool(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void wheelHandlerIsPresentAndScopedToTimeline()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QObject *handler = timeline->findChild<QObject *>(
            QStringLiteral("timelineWheelHandler"));
        QVERIFY(handler != nullptr);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void wheelUpwardLeavesFollowLatest()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QVERIFY(timeline->setProperty("stickToBottom", true));
        const double startY = timeline->property("contentY").toDouble();
        QVERIFY(QMetaObject::invokeMethod(timeline, "beginWheelTo",
                                          Q_ARG(QVariant, startY + 200.0)));
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void jumpToLatestCancelsWheelMotion()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QObject *jump = root->findChild<QObject *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(timeline != nullptr);
        QVERIFY(jump != nullptr);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QTRY_COMPARE_WITH_TIMEOUT(jump->property("visible").toBool(), true,
                                  kSignalTimeoutMs);
        scroll->wheelTargetY(120.0, 5000.0, 0.0, 10000.0, 900.0);
        QVERIFY(scroll->motionActive());

        QVERIFY(QMetaObject::invokeMethod(jump, "click"));
        QTRY_COMPARE_WITH_TIMEOUT(scroll->motionActive(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(timeline->property("wheelAnimating").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void roomSwitchCancelsWheelMotion()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        scroll->wheelTargetY(120.0, 5000.0, 0.0, 10000.0, 900.0);
        QVERIFY(scroll->motionActive());

        controller.setCurrentRoomId(QStringLiteral("!dm-bob:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(scroll->motionActive(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(timeline->property("wheelAnimating").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void realWheelEventEngagesControllerAndLeavesFollowLatest()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", true));
        QVERIFY(!scroll->motionActive());

        const QPointF pos(320, 300);
        auto sendNotch = [&] {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                              Qt::NoModifier, Qt::NoScrollPhase,false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
        };
        bool engaged = false;
        for (int attempt = 0; attempt < 50 && !engaged; ++attempt) {
            sendNotch();
            engaged = scroll->motionActive();
            if (!engaged)
                QTest::qWait(10);
        }

        QVERIFY2(engaged, "a mouse-wheel notch must engage the motion engine");
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        // Ignore the benign mock.local DNS warning from image rows.
        warnings.removeIf([](const QString &w) {
            return w.contains(QStringLiteral("Host mock.local not found"));
        });
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void touchpadWheelUpwardLeavesFollowLatest()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", true));

        const QPointF pos(320, 300);
        auto sendUp = [&] {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 48), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
        };
        bool left = false;
        for (int attempt = 0; attempt < 50 && !left; ++attempt) {
            sendUp();
            left = !timeline->property("stickToBottom").toBool();
            if (!left)
                QTest::qWait(10);
        }
        QVERIFY2(left, "an upward touchpad delta must leave follow-latest");
    }

    void touchpadZeroPixelFramesNeverEngageTheNotchGlide()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();
        QVERIFY(timeline->setProperty("stickToBottom", false));

        const QPointF pos(320, 300);
        auto send = [&](QPoint pixel, QPoint angle, Qt::ScrollPhase phase) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()), pixel,
                              angle, Qt::NoButton, Qt::NoModifier, phase,
false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
        };
        auto counter = [&](const char *name) {
            return timeline->property(name).toInt();
        };

        const double before = timeline->property("contentY").toDouble();
        for (int attempt = 0; attempt < 50 && counter("diagEvents") < 6;
             ++attempt) {
            send(QPoint(0, 0), QPoint(0, 1), Qt::ScrollUpdate);
            send(QPoint(0, 0), QPoint(0, -1), Qt::ScrollUpdate);
            if (counter("diagEvents") < 6)
                QTest::qWait(10);
        }
        QVERIFY2(counter("diagEvents") >= 6,
                 "the fixture never delivered a wheel event to the handler");
        QCOMPARE(counter("diagAngleEvents"), 0);
        QCOMPARE(counter("diagPixelEvents"), counter("diagEvents"));
        QCOMPARE(controller.timelineScroll()->motionActive(), false);
        QCOMPARE(timeline->property("contentY").toDouble(), before);

        const int anglesBefore = counter("diagAngleEvents");
        for (int attempt = 0;
             attempt < 50 && counter("diagAngleEvents") == anglesBefore;
             ++attempt) {
            send(QPoint(0, 0), QPoint(0, 120), Qt::NoScrollPhase);
            if (counter("diagAngleEvents") == anglesBefore)
                QTest::qWait(10);
        }
        QVERIFY2(counter("diagAngleEvents") > anglesBefore,
                 "a phase-less wheel notch must still take the notch branch");
    }

    void touchpadGestureOpensScrollSessionThatGatesCorrections()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("userScrollActive").toBool(), false);

        const QPointF pos(320, 300);
        auto sendDelta = [&] {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
        };
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            sendDelta();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");

        const double before = timeline->property("contentY").toDouble();
        QMetaObject::invokeMethod(timeline, "maintainViewAnchor");
        QCOMPARE(timeline->property("contentY").toDouble(), before);

        for (int i = 0; i < 5; ++i)
            sendDelta();
        QVERIFY(timeline->property("userScrollActive").toBool());

        QCOMPARE(timeline->property("stickToBottom").toBool(), false);

        QTRY_VERIFY_WITH_TIMEOUT(
            !timeline->property("userScrollActive").toBool(), 3000);

        QCOMPARE(timeline->property("diagAnchorCorrections").toInt(), 0);
    }

    void contentGrowthWhileBrowsingDoesNotResnap()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *timeline = root->findChild<QObject *>(
            QStringLiteral("timelineListView"));
        QObject *jump = root->findChild<QObject *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(timeline != nullptr);
        QVERIFY(jump != nullptr);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QTRY_COMPARE_WITH_TIMEOUT(jump->property("visible").toBool(), true,
                                  kSignalTimeoutMs);

        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        for (int i = 0; i < 4; ++i) {
            const int before = controller.timeline()->rowCount();
            mock->sendTextMessage(
                roomId, QStringLiteral("growth event %1").arg(i));
            QTRY_COMPARE_WITH_TIMEOUT(controller.timeline()->rowCount(),
                                      before + 1, kSignalTimeoutMs);
            QCoreApplication::processEvents();
            QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        }
        QCOMPARE(jump->property("visible").toBool(), true);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void keyboardEndKeyReturnsToLatest()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        timeline->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(), kSignalTimeoutMs);
        QVERIFY(timeline->setProperty("stickToBottom", false));

        QTest::keyClick(&window, Qt::Key_End);
        QTRY_COMPARE_WITH_TIMEOUT(timeline->property("stickToBottom").toBool(),
                                  true, kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void keyboardNavigationKeysStartTimelineMotion()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        const QList<QPair<Qt::Key, Qt::KeyboardModifiers>> keys = {
            {Qt::Key_PageUp, Qt::NoModifier},
            {Qt::Key_PageDown, Qt::NoModifier},
            {Qt::Key_Space, Qt::ShiftModifier},
            {Qt::Key_Space, Qt::NoModifier},
        };
        for (const auto &k : keys) {
            QVERIFY(QMetaObject::invokeMethod(timeline, "cancelWheelMotion"));
            timeline->forceActiveFocus();
            QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(),
                                     kSignalTimeoutMs);
            QVERIFY(!timeline->property("wheelAnimating").toBool());
            QTest::keyClick(&window, k.first, k.second);
            QVERIFY2(timeline->property("wheelAnimating").toBool(),
                     "navigation key did not start timeline motion");
        }

        QVERIFY(QMetaObject::invokeMethod(timeline, "cancelWheelMotion"));
        timeline->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(), kSignalTimeoutMs);
        QTest::keyClick(&window, Qt::Key_Home, Qt::NoModifier);
        QVERIFY2(!timeline->property("wheelAnimating").toBool(),
                 "Home must jump instantly, not start wheel motion");
        QVERIFY(QMetaObject::invokeMethod(timeline, "goToEarliestLoaded"));
        QVariant maxY;
        QVERIFY(QMetaObject::invokeMethod(timeline, "wheelMaxY",
                                          Q_RETURN_ARG(QVariant, maxY)));
        QCOMPARE(timeline->property("contentY").toDouble(), maxY.toDouble());
        QVERIFY(QMetaObject::invokeMethod(timeline, "cancelWheelMotion"));
    }

    void composerFocusKeepsItsCaretKeysAndForwardsThePagingKeys()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        auto *composer = root->findChild<QQuickItem *>(
            QStringLiteral("composerInput"));
        QVERIFY(timeline != nullptr);
        QVERIFY(composer != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", false));
        composer->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(composer->hasActiveFocus(), kSignalTimeoutMs);

        QTest::keyClick(&window, Qt::Key_End);
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        QCOMPARE(timeline->property("wheelAnimating").toBool(), false);

        QTest::keyClick(&window, Qt::Key_PageUp);
        QCoreApplication::processEvents();
        QVERIFY2(timeline->property("wheelAnimating").toBool(),
                 "PgUp from an empty composer must page the timeline");
        QVERIFY(QMetaObject::invokeMethod(timeline, "cancelWheelMotion"));
        QCOMPARE(timeline->property("wheelAnimating").toBool(), false);

        QVERIFY(composer->setProperty("text", QStringLiteral("a\nb\nc\nd")));
        QVERIFY(composer->setProperty("cursorPosition", 0));
        QCoreApplication::processEvents();
        QTest::keyClick(&window, Qt::Key_PageDown);
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("wheelAnimating").toBool(), false);
        QVERIFY(composer->setProperty("cursorPosition",
                                      composer->property("length").toInt()));
        QCoreApplication::processEvents();
        QTest::keyClick(&window, Qt::Key_PageDown);
        QCoreApplication::processEvents();
        QVERIFY2(timeline->property("wheelAnimating").toBool(),
                 "PgDn on the draft's last line must page the timeline");
        QVERIFY(QMetaObject::invokeMethod(timeline, "cancelWheelMotion"));

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QTest::keyClick(&window, Qt::Key_Up, Qt::ControlModifier);
        QCoreApplication::processEvents();
        QVariant maxY;
        QVERIFY(QMetaObject::invokeMethod(timeline, "wheelMaxY",
                                          Q_RETURN_ARG(QVariant, maxY)));
        QCOMPARE(timeline->property("contentY").toDouble(), maxY.toDouble());
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void jumpToLatestAndComposeLandsAtTheLatestWithTheComposerFocused()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *shortcuts = controller.property("shortcuts").value<QObject *>();
        QVERIFY(shortcuts != nullptr);
        QString bindError;
        QVERIFY(QMetaObject::invokeMethod(
            shortcuts, "setBinding", Q_RETURN_ARG(QString, bindError),
            Q_ARG(QString, QStringLiteral("timeline.jumpToLatestAndCompose")),
            Q_ARG(QString, QStringLiteral("Ctrl+Alt+L"))));
        QCOMPARE(bindError, QString());

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        auto *composer = root->findChild<QQuickItem *>(
            QStringLiteral("composerInput"));
        QVERIFY(timeline != nullptr);
        QVERIFY(composer != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        timeline->forceActiveFocus();
        QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(), kSignalTimeoutMs);
        QTest::keyClick(&window, Qt::Key_Up, Qt::ControlModifier);
        QCoreApplication::processEvents();
        QVariant minY;
        QVERIFY(QMetaObject::invokeMethod(timeline, "wheelMinY",
                                          Q_RETURN_ARG(QVariant, minY)));
        QVERIFY2(timeline->property("contentY").toDouble() > minY.toDouble() + 1,
                 "the fixture must start away from the latest message");
        QVERIFY(!composer->hasActiveFocus());

        QTest::keyClick(&window, Qt::Key_L, Qt::ControlModifier | Qt::AltModifier);
        QTRY_VERIFY_WITH_TIMEOUT(composer->hasActiveFocus(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                QVariant lo;
                QMetaObject::invokeMethod(timeline, "wheelMinY",
                                          Q_RETURN_ARG(QVariant, lo));
                return qAbs(timeline->property("contentY").toDouble()
                            - lo.toDouble()) < 1.0;
            }(),
            kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void ctrlGStepsTheFindResultsAndCtrlShiftGGoesBack()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QCOMPARE(int(controller.currentScreen()), 1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(900, 700);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        auto *model = controller.timeline();
        QVERIFY(QMetaObject::invokeMethod(root, "openFind"));
        QVERIFY(model->searchActive());
        model->updateSearch(QStringLiteral("e"));
        QTRY_VERIFY_WITH_TIMEOUT(model->searchResultCount() >= 2,
                                 kSignalTimeoutMs);
        const int first = model->searchCurrentPosition();

        QTest::keyClick(&window, Qt::Key_G, Qt::ControlModifier);
        QCoreApplication::processEvents();
        QVERIFY2(model->searchCurrentPosition() != first,
                 "Ctrl+G did not step to the next find result");
        QTest::keyClick(&window, Qt::Key_G,
                        Qt::ControlModifier | Qt::ShiftModifier);
        QCoreApplication::processEvents();
        QCOMPARE(model->searchCurrentPosition(), first);

        QVERIFY(QMetaObject::invokeMethod(root, "closeFind"));
        QVERIFY(!model->searchActive());
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void findBarIsDetachedAndNeverMovesTheComposer()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(900, 700);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        auto *composer = root->findChild<QQuickItem *>(
            QStringLiteral("composerCard"));
        QVERIFY(composer != nullptr);
        const qreal composerYBeforeOpen =
            composer->mapToScene(QPointF(0, 0)).y();

        QVERIFY(QMetaObject::invokeMethod(root, "openFind"));
        QCoreApplication::processEvents();

        auto *findBar = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindBar"));
        QVERIFY(findBar != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(findBar->isVisible(), kSignalTimeoutMs);

        const QPointF findBarTopLeft = findBar->mapToScene(QPointF(0, 0));
        const QPointF findBarTopRight =
            findBar->mapToScene(QPointF(findBar->width(), 0));
        QVERIFY2(findBarTopLeft.x() > 0.0,
                 "find bar must not touch the pane's left edge");
        QVERIFY2(findBarTopRight.x() < root->width(),
                 "find bar must not touch the pane's right edge");

        QCOMPARE(composer->mapToScene(QPointF(0, 0)).y(), composerYBeforeOpen);

        auto *findField = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindField"));
        QVERIFY(findField != nullptr);
        QVERIFY(findField->hasActiveFocus());

        QVERIFY(QMetaObject::invokeMethod(root, "closeFind"));
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(!findBar->isVisible(), kSignalTimeoutMs);
        QCOMPARE(composer->mapToScene(QPointF(0, 0)).y(), composerYBeforeOpen);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->hasActiveFocus(), kSignalTimeoutMs);

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void roomSwitchResetsPresentationState()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());

        const QString generalId = QStringLiteral("!general:mock.local");
        const QString dmId = QStringLiteral("!dm-bob:mock.local");

        controller.setCurrentRoomId(generalId);
        controller.pagination()->requestNearTop();
        QCOMPARE(controller.pagination()->presentationState(),
                 PaginationController::Loading);

        controller.setCurrentRoomId(dmId);
        QCOMPARE(controller.pagination()->roomId(), dmId);
        QCOMPARE(controller.pagination()->presentationState(),
                 PaginationController::Hidden);
    }

    // Drives the state-group expand/collapse functions the summary row's
    // TapHandler and Keys call, on the real pane and the seeded
    // "!devs:mock.local" group, and checks the reset on room switch. Invoked
    // directly because the offscreen QPA never incubates list delegates, so
    // there is nothing to click.
    void stateGroupExpansionTogglesAndResetsOnRoomSwitch()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        // "!devs:mock.local" has two consecutive state events forming one
        // group.
        const QString devsId = QStringLiteral("!devs:mock.local");
        const QString generalId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(devsId);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QVERIFY(!createdSpy.isEmpty());
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *listView = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(listView != nullptr);

        const QString groupId = firstStateGroupId(controller.timeline());
        QVERIFY(!groupId.isEmpty());

        auto isExpanded = [&] {
            QVariant result;
            QMetaObject::invokeMethod(listView, "stateGroupExpanded",
                                      Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, groupId));
            return result.toBool();
        };
        auto toggle = [&] {
            QMetaObject::invokeMethod(listView, "toggleStateGroup",
                                      Q_ARG(QVariant, groupId));
        };

        QVERIFY(!isExpanded());
        toggle();
        QVERIFY(isExpanded());
        toggle();
        QVERIFY(!isExpanded());
        toggle();
        QVERIFY(isExpanded());

        controller.setCurrentRoomId(generalId);
        QVERIFY(!isExpanded());

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void threadPanelOpensAndClosesWithController()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *panel = root->findChild<QObject *>(
            QStringLiteral("threadPanel"));
        QVERIFY(panel != nullptr);
        QCOMPARE(panel->property("visible").toBool(), false);

        const QString rootId = fixtureThreadRootId(controller);
        QVERIFY(!rootId.isEmpty());
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(panel->property("visible").toBool(), true,
                                  kSignalTimeoutMs);

        QObject *rootHeader = root->findChild<QObject *>(
            QStringLiteral("threadRootHeader"));
        QVERIFY(rootHeader != nullptr);
        const QVariantMap rootInfo = controller.thread()->rootInfo();
        QCOMPARE(rootInfo.value(QStringLiteral("loaded")).toBool(), true);
        QCOMPARE(rootInfo.value(QStringLiteral("eventId")).toString(), rootId);

        QObject *closeButton = root->findChild<QObject *>(
            QStringLiteral("threadCloseButton"));
        QVERIFY(closeButton != nullptr);
        QVERIFY(QMetaObject::invokeMethod(closeButton, "click"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(panel->property("visible").toBool(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void roomSwitchClosesThreadPanel()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);
        QObject *panel = root->findChild<QObject *>(
            QStringLiteral("threadPanel"));
        QVERIFY(panel != nullptr);

        const QString rootId = fixtureThreadRootId(controller);
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);

        controller.setCurrentRoomId(QStringLiteral("!dm-bob:mock.local"));
        QCOMPARE(controller.thread()->state(), ThreadController::Closed);
        QTRY_COMPARE_WITH_TIMEOUT(panel->property("visible").toBool(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void threadComposerSendsThreadReply()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);

        const QString rootId = fixtureThreadRootId(controller);
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        auto *threadModel = controller.thread()->model();
        const int rowsBefore = threadModel->rowCount();

        QObject *input = root->findChild<QObject *>(
            QStringLiteral("threadComposerInput"));
        QObject *send = root->findChild<QObject *>(
            QStringLiteral("threadSendButton"));
        QVERIFY(input != nullptr);
        QVERIFY(send != nullptr);
        QVERIFY(input->setProperty("text",
                                   QStringLiteral("panel thread reply")));
        QVERIFY(QMetaObject::invokeMethod(send, "click"));

        QTRY_COMPARE_WITH_TIMEOUT(threadModel->rowCount(), rowsBefore + 1,
                                  kSignalTimeoutMs);
        const QModelIndex last =
            threadModel->index(threadModel->rowCount() - 1, 0);
        QCOMPARE(threadModel->data(last, TimelineModel::BodyRole).toString(),
                 QStringLiteral("panel thread reply"));
        QCOMPARE(threadModel
                     ->data(last, TimelineModel::ThreadRootIdRole).toString(),
                 rootId);
        QCOMPARE(input->property("text").toString(), QString{});
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void narrowWindowThreadPanelReplacesRoomColumn()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        root->setWidth(560);
        root->setHeight(480);
        QObject *roomColumn = root->findChild<QObject *>(
            QStringLiteral("roomColumn"));
        QObject *panel = root->findChild<QObject *>(
            QStringLiteral("threadPanel"));
        QVERIFY(roomColumn != nullptr);
        QVERIFY(panel != nullptr);
        QCOMPARE(roomColumn->property("visible").toBool(), true);

        const QString rootId = fixtureThreadRootId(controller);
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(panel->property("visible").toBool(), true,
                                  kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(roomColumn->property("visible").toBool(),
                                  false, kSignalTimeoutMs);

        auto *panelItem = qobject_cast<QQuickItem *>(panel);
        QVERIFY(panelItem);
        root->setWidth(580);
        QTRY_COMPARE_WITH_TIMEOUT(roomColumn->property("visible").toBool(),
                                  false, kSignalTimeoutMs);
        QQmlExpression widthAt580(qmlContext(panelItem), panelItem,
                                  QStringLiteral("Layout.preferredWidth"));
        QCOMPARE(widthAt580.evaluate().toReal(), 580.0);
        root->setWidth(581);
        QTRY_COMPARE_WITH_TIMEOUT(roomColumn->property("visible").toBool(),
                                  true, kSignalTimeoutMs);
        QQmlExpression widthAt581(qmlContext(panelItem), panelItem,
                                  QStringLiteral("Layout.preferredWidth"));
        QCOMPARE(widthAt581.evaluate().toReal(), 260.0);

        root->setWidth(800);
        QTRY_COMPARE_WITH_TIMEOUT(roomColumn->property("visible").toBool(),
                                  true, kSignalTimeoutMs);
        QCOMPARE(panel->property("visible").toBool(), true);
        QQmlExpression widthAt800(qmlContext(panelItem), panelItem,
                                  QStringLiteral("Layout.preferredWidth"));
        QCOMPARE(widthAt800.evaluate().toReal(), 320.0);
        root->setWidth(1200);
        QQmlExpression widthAt1200(qmlContext(panelItem), panelItem,
                                   QStringLiteral("Layout.preferredWidth"));
        QCOMPARE(widthAt1200.evaluate().toReal(), 320.0);
        QCOMPARE(roomColumn->property("visible").toBool(), true);

        controller.thread()->close();
        QTRY_COMPARE_WITH_TIMEOUT(panel->property("visible").toBool(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void threadPanelIsExclusiveWithMemberPanel()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        root->setWidth(1200);
        root->setHeight(700);

        QObject *forumButton = root->findChild<QObject *>(
            QStringLiteral("threadsViewButton"));
        QObject *infoPanel = root->findChild<QObject *>(
            QStringLiteral("roomInfoPanel"));
        QVERIFY(forumButton && infoPanel);

        QVERIFY(infoPanel->setProperty("section", QStringLiteral("people")));
        QVERIFY(root->setProperty("infoOpen", true));
        QCOMPARE(root->property("infoOpen").toBool(), true);
        QObject *groupButton = root->findChild<QObject *>(
            QStringLiteral("memberPanelButton"));
        QVERIFY(groupButton);
        QCOMPARE(groupButton->property("active").toBool(), true);
        QCOMPARE(forumButton->property("active").toBool(), false);

        const QString rootId = fixtureThreadRootId(controller);
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("infoOpen").toBool(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(forumButton->property("active").toBool(), true);
        QCOMPARE(groupButton->property("active").toBool(), false);
        QObject *roomColumn = root->findChild<QObject *>(
            QStringLiteral("roomColumn"));
        QVERIFY(roomColumn);
        QCOMPARE(roomColumn->property("visible").toBool(), true);

        QObject *threadInput = root->findChild<QObject *>(
            QStringLiteral("threadComposerInput"));
        QVERIFY(threadInput);
        controller.composer()->setText(QStringLiteral("main draft"));
        QVERIFY(threadInput->setProperty("text",
                                         QStringLiteral("thread draft")));
        QCOMPARE(controller.composer()->text(), QStringLiteral("main draft"));
        QCOMPARE(threadInput->property("text").toString(),
                 QStringLiteral("thread draft"));
        controller.composer()->setText(QString{});

        QVERIFY(root->setProperty("infoOpen", true));
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);
        QCOMPARE(root->property("infoOpen").toBool(), true);
        QCOMPARE(groupButton->property("active").toBool(), true);
        QCOMPARE(forumButton->property("active").toBool(), false);

        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("infoOpen").toBool(), false,
                                  kSignalTimeoutMs);
        QObject *closeButton = root->findChild<QObject *>(
            QStringLiteral("threadCloseButton"));
        QVERIFY(closeButton);
        QVERIFY(QMetaObject::invokeMethod(closeButton, "click"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);
        QCOMPARE(root->property("rightPanelState").toString(),
                 QStringLiteral("none"));
        QCOMPARE(root->property("infoOpen").toBool(), false);
        QObject *panelObject = root->findChild<QObject *>(
            QStringLiteral("threadPanel"));
        QVERIFY(panelObject);
        QCOMPARE(panelObject->property("visible").toBool(), false);
        QCOMPARE(forumButton->property("active").toBool(), false);
        QCOMPARE(groupButton->property("active").toBool(), false);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void messageLayoutModesReshapeTheDelegate()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QString{});
        fixture.insert(QStringLiteral("eventId"), QString{});
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"),
                       QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("Layout fixture body"));
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        engine.rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        root->setWidth(640);
        QCoreApplication::processEvents();

        auto *content = root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *avatar = root->findChild<QQuickItem *>(
            QStringLiteral("senderAvatar"));
        auto *body = root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(content && avatar && body);

        QCOMPARE(controller.settings()->messageLayout(), 0);
        QCOMPARE(root->property("avatarGutterWidth").toReal(), 40.0);
        QVERIFY(avatar->isVisible());
        QCOMPARE(content->property("color").value<QColor>().alpha(), 0);

        controller.settings()->setMessageLayout(2);
        QCoreApplication::processEvents();
        QCOMPARE(root->property("avatarGutterWidth").toReal(), 8.0);
        QVERIFY(!avatar->isVisible());
        QCOMPARE(body->property("font").value<QFont>().pixelSize(), 13);

        controller.settings()->setMessageLayout(1);
        QCoreApplication::processEvents();
        QVERIFY(!root->property("bubbleMode").toBool());
        QCOMPARE(content->property("color").value<QColor>().alpha(), 0);

        root->setProperty("isDirectRoom", true);
        QCoreApplication::processEvents();
        QVERIFY(root->property("bubbleMode").toBool());
        QQmlExpression otherExpr(qmlContext(root), root,
                                 QStringLiteral("AppTheme.otherBubble"));
        QCOMPARE(content->property("color").value<QColor>(),
                 otherExpr.evaluate().value<QColor>());
        const qreal incomingX = content->x();

        fixture.insert(QStringLiteral("isOwn"), true);
        engine.rootContext()->setContextProperty("model", fixture);
        QCoreApplication::processEvents();
        QQmlExpression ownExpr(qmlContext(root), root,
                               QStringLiteral("AppTheme.ownBubble"));
        QCOMPARE(content->property("color").value<QColor>(),
                 ownExpr.evaluate().value<QColor>());
        QVERIFY(content->x() > incomingX);
        QVERIFY(content->x() + content->width() <= 640.0 + 1.0);

        controller.settings()->setMessageLayout(0);
        QQmlExpression setScale(qmlContext(root), root,
                                QStringLiteral("AppTheme.textScale = 1.4"));
        setScale.evaluate();
        QCoreApplication::processEvents();
        QCOMPARE(body->property("font").value<QFont>().pixelSize(),
                 qRound(14 * 1.4));
        QQmlExpression resetScale(qmlContext(root), root,
                                  QStringLiteral("AppTheme.textScale = 1.0"));
        resetScale.evaluate();

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void theRoomHeaderKeepsItsContentInsideItsOwnBandAtEveryTextScale()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        LogCapture capture;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window,
                                          QStringLiteral("!general:example.org"),
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        auto *band = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderBand"));
        auto *actions = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderActions"));
        QVERIFY(band);
        QVERIFY(actions);

        const auto actionsBottomInBand = [&] {
            const QPointF topLeft = actions->mapToItem(band, QPointF(0, 0));
            return topLeft.y() + actions->height();
        };
        const auto report = [&](double scale) {
            return qPrintable(QStringLiteral(
                "at textScale %1 the header band is %2px tall and its action "
                "row runs to %3px: the icons are drawn outside the band, over "
                "the timeline")
                .arg(scale).arg(band->height()).arg(actionsBottomInBand()));
        };

        QVERIFY2(actionsBottomInBand() <= band->height() + 0.5, report(1.0));

        for (const double scale : { 1.4, 1.8, 2.0 }) {
            QQmlExpression set(qmlContext(root), root,
                               QStringLiteral("AppTheme.textScale = %1").arg(scale));
            set.evaluate();
            QCoreApplication::processEvents();
            QVERIFY2(actionsBottomInBand() <= band->height() + 0.5, report(scale));
            QVERIFY2(band->height() >= 60.0, report(scale));
        }
        QQmlExpression reset(qmlContext(root), root,
                             QStringLiteral("AppTheme.textScale = 1.0"));
        reset.evaluate();
        QCoreApplication::processEvents();

        const auto setRoom = [&](const QString &topic) {
            QVariantMap room;
            room.insert(QStringLiteral("name"), QStringLiteral("Minecraft"));
            room.insert(QStringLiteral("topic"), topic);
            root->setProperty("currentRoom", room);
            QTest::qWait(60);
            QCoreApplication::processEvents();
        };
        auto *identity = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderIdentity"));
        QVERIFY(identity);

        setRoom(QStringLiteral("Cutefunny minecraft server"));
        QVERIFY2(actionsBottomInBand() <= band->height() + 0.5,
                 "a one-line topic already overflows the header band");

        setRoom(QStringLiteral("Instructions:\nhttps://polymc.org\ndrag and drop "
                               "the zip into it\nit will already be listed\nfour"));
        QVERIFY2(actionsBottomInBand() <= band->height() + 0.5,
                 qPrintable(QStringLiteral(
                     "a multi-line room topic pushed the header's action row "
                     "to %1px inside a %2px band: the icons are drawn over the "
                     "message list").arg(actionsBottomInBand()).arg(band->height())));

        for (const int h : { 520, 400, 300, 240, 180 }) {
            root->setSize(QSizeF(700, h));
            QCoreApplication::processEvents();
            QVERIFY2(actionsBottomInBand() <= band->height() + 0.5,
                     qPrintable(QStringLiteral(
                         "at pane height %1 the action row runs to %2px inside "
                         "a %3px band").arg(h).arg(actionsBottomInBand())
                             .arg(band->height())));
        }
        root->setSize(QSizeF(700, 700));
        QCoreApplication::processEvents();
    }

    void aDmHeaderAvatarCarriesThePeersPresenceDot()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString dmId = QStringLiteral("!dm-bob:mock.local");
        const QString bob = QStringLiteral("@bob:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, dmId,
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        window.resize(1000, 700);
        root->setSize(QSizeF(1000, 700));

        auto *band = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderBand"));
        auto *avatar = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderAvatar"));
        auto *dot = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderPresenceDot"));
        QVERIFY(band && avatar);
        QVERIFY2(dot, "a DM's header avatar has no presence dot");
        QCOMPARE(dot->property("userId").toString(), bob);
        auto *disc = dot->findChild<QQuickItem *>(QStringLiteral("presenceDotDisc"));
        QVERIFY(disc);

        QObject *fake = fakePresenceService(engine);
        QVERIFY(fake);
        QVERIFY(injectPresence(dot, fake));
        QCoreApplication::processEvents();
        QVERIFY2(watchedBy(fake).contains(bob), "the header dot watches nobody");

        QVERIFY(!dot->isVisible());

        reportPresence(fake, bob, QStringLiteral("online"), 0);
        QTRY_VERIFY_WITH_TIMEOUT(dot->isVisible(), 2000);
        QTest::qWait(60);
        QCoreApplication::processEvents();
        const QString placement = presenceDotPlacement(dot, avatar, band);
        QVERIFY2(placement.isEmpty(), qPrintable(placement));
        QCOMPARE(disc->property("color").value<QColor>(),
                 themeColor(root, QStringLiteral("presenceOnline")));
        QCOMPARE(dot->property("statusText").toString(), QStringLiteral("Online"));
        QCOMPARE(attached(dot, QStringLiteral("ToolTip.text")).toString(),
                 QStringLiteral("Online"));
        QCOMPARE(attached(dot, QStringLiteral("Accessible.name")).toString(),
                 QStringLiteral("Online"));
        QVERIFY(!attached(dot, QStringLiteral("Accessible.ignored")).toBool());

        const QPointF centre = dot->mapToScene(
            QPointF(dot->width() / 2.0, dot->height() / 2.0));
        QTest::mouseMove(&window, centre.toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(
            attached(dot, QStringLiteral("ToolTip.visible")).toBool(), 2000);
        QTest::mouseMove(&window, QPoint(2, 400));
        QTRY_VERIFY_WITH_TIMEOUT(
            !attached(dot, QStringLiteral("ToolTip.visible")).toBool(), 2000);

        reportPresence(fake, bob, QStringLiteral("unavailable"), 0);
        QCOMPARE(disc->property("color").value<QColor>(),
                 themeColor(root, QStringLiteral("presenceAway")));
        QCOMPARE(dot->property("statusText").toString(), QStringLiteral("Away"));

        reportPresence(fake, bob, QStringLiteral("offline"), 5 * 60000 + 1000);
        QCOMPARE(disc->property("color").value<QColor>().alpha(), 0);
        auto *discBorder = disc->property("border").value<QObject *>();
        QVERIFY(discBorder);
        QVERIFY(discBorder->property("width").toReal() >= 1.0);
        QCOMPARE(discBorder->property("color").value<QColor>(),
                 themeColor(root, QStringLiteral("presenceOffline")));
        QCOMPARE(dot->property("statusText").toString(),
                 QStringLiteral("Offline \u2014 active 5 min ago"));
        QCOMPARE(attached(dot, QStringLiteral("ToolTip.text")).toString(),
                 QStringLiteral("Offline \u2014 active 5 min ago"));

        auto *panel = root->findChild<QQuickItem *>(QStringLiteral("roomInfoPanel"));
        QVERIFY(panel);
        QVERIFY(QMetaObject::invokeMethod(panel, "openForRoom",
                                          Q_ARG(QVariant, dmId)));
        QVERIFY(root->setProperty("infoOpen", true));
        QTRY_VERIFY_WITH_TIMEOUT(panel->isVisible() && panel->width() > 0, 2000);
        auto *infoAvatar = panel->findChild<QQuickItem *>(
            QStringLiteral("roomInfoAvatar"));
        auto *infoDot = panel->findChild<QQuickItem *>(
            QStringLiteral("roomInfoPresenceDot"));
        QVERIFY(infoAvatar);
        QVERIFY2(infoDot, "Room Information's avatar has no presence dot");
        QCOMPARE(infoDot->property("userId").toString(), bob);
        QVERIFY(injectPresence(infoDot, fake));
        reportPresence(fake, bob, QStringLiteral("online"), 0);
        QTRY_VERIFY_WITH_TIMEOUT(infoDot->isVisible(), 2000);
        QTest::qWait(60);
        QCoreApplication::processEvents();
        const QString infoPlacement =
            presenceDotPlacement(infoDot, infoAvatar, panel);
        QVERIFY2(infoPlacement.isEmpty(), qPrintable(infoPlacement));
        auto *infoDisc = infoDot->findChild<QQuickItem *>(
            QStringLiteral("presenceDotDisc"));
        QVERIFY(infoDisc);
        QCOMPARE(infoDisc->property("color").value<QColor>(),
                 themeColor(root, QStringLiteral("presenceOnline")));
        QCOMPARE(attached(infoDot, QStringLiteral("ToolTip.text")).toString(),
                 QStringLiteral("Online"));

        QVERIFY(root->setProperty("infoOpen", false));
        QTRY_COMPARE_WITH_TIMEOUT(infoDot->property("userId").toString(),
                                  QString(), 2000);
    }

    void aGroupRoomHeaderAvatarHasNoPresenceDot()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString generalId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, generalId,
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        window.resize(1000, 700);
        root->setSize(QSizeF(1000, 700));

        auto *dot = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderPresenceDot"));
        QVERIFY2(dot, "the header avatar has no presence dot slot");
        QObject *fake = fakePresenceService(engine);
        QVERIFY(fake);
        QVERIFY(injectPresence(dot, fake));
        QMetaObject::invokeMethod(fake, "reportEveryone",
                                  Q_ARG(QVariant, QStringLiteral("online")));
        QCoreApplication::processEvents();

        QCOMPARE(dot->property("userId").toString(), QString());
        QVERIFY2(!dot->isVisible(), "a group room shows a presence dot");
        QCOMPARE(watchedBy(fake), QStringList{});

        QVariantMap trio;
        trio.insert(QStringLiteral("name"), QStringLiteral("Trio"));
        trio.insert(QStringLiteral("isDirect"), true);
        trio.insert(QStringLiteral("directUserIds"),
                    QStringList{ QStringLiteral("@bob:mock.local"),
                                 QStringLiteral("@carol:mock.local") });
        trio.insert(QStringLiteral("identityColorKey"),
                    QStringLiteral("!trio:mock.local"));
        root->setProperty("currentRoom", trio);
        QCoreApplication::processEvents();
        QCOMPARE(dot->property("userId").toString(), QString());
        QVERIFY2(!dot->isVisible(), "a group DM shows a presence dot");
        QCOMPARE(watchedBy(fake), QStringList{});

        auto *panel = root->findChild<QQuickItem *>(QStringLiteral("roomInfoPanel"));
        QVERIFY(panel);
        QVERIFY(QMetaObject::invokeMethod(panel, "openForRoom",
                                          Q_ARG(QVariant, generalId)));
        QVERIFY(root->setProperty("infoOpen", true));
        QTRY_VERIFY_WITH_TIMEOUT(panel->isVisible() && panel->width() > 0, 2000);
        auto *infoDot = panel->findChild<QQuickItem *>(
            QStringLiteral("roomInfoPresenceDot"));
        QVERIFY2(infoDot, "Room Information's avatar has no presence dot slot");
        QVERIFY(injectPresence(infoDot, fake));
        QCoreApplication::processEvents();
        QCOMPARE(infoDot->property("userId").toString(), QString());
        QVERIFY2(!infoDot->isVisible(), "Room Information shows a group room's dot");
        QCOMPARE(watchedBy(fake), QStringList{});
    }

    void theHeaderNeverShowsARawRoomId()
    {
        AppController controller(AppController::MockBackend);
        const QString listed = loginAndRoomIdAt(controller, 0);
        QVERIFY(!listed.isEmpty());
        const QString unlisted = QStringLiteral("!xCR0notlistedyet:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, unlisted,
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        auto *title = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderTitle"));
        auto *avatar = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderAvatar"));
        QVERIFY(title && avatar);
        QTRY_VERIFY(!title->property("text").toString().isEmpty());
        QVERIFY2(!title->property("text").toString().startsWith(QLatin1Char('!')),
                 qPrintable(title->property("text").toString()));
        QVERIFY(!avatar->property("name").toString().startsWith(QLatin1Char('!')));

        controller.setCurrentRoomId(listed);
        const QString name = controller.roomList()->findRoom(listed)
                                 .value(QStringLiteral("name")).toString();
        QVERIFY(!name.isEmpty());
        QTRY_COMPARE(title->property("text").toString(), name);
    }

    void aDmHeaderAvatarAndNameOpenThePeersProfile()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString dmId = QStringLiteral("!dm-bob:mock.local");
        const QString bob = QStringLiteral("@bob:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, dmId,
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        window.resize(1000, 700);
        root->setSize(QSizeF(1000, 700));
        QCoreApplication::processEvents();

        auto *avatar = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderAvatar"));
        auto *title = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderTitle"));
        auto *identity = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderIdentity"));
        auto *columnTap = root->findChild<QObject *>(
            QStringLiteral("roomHeaderIdentityTap"));
        auto *profile = root->findChild<QObject *>(
            QStringLiteral("senderProfilePopover"));
        QVERIFY(avatar && title && identity && columnTap);
        QVERIFY2(profile, "the pane has no profile card");
        QTRY_VERIFY_WITH_TIMEOUT(avatar->isVisible() && avatar->width() > 0
                                 && title->isVisible() && title->width() > 0,
                                 2000);
        QVERIFY(!profile->property("opened").toBool());

        const auto centreOf = [](QQuickItem *item) {
            return item->mapToScene(QPointF(item->width() / 2.0,
                                            item->height() / 2.0)).toPoint();
        };
        const auto closeProfile = [&] {
            QMetaObject::invokeMethod(profile, "close");
            QTRY_VERIFY_WITH_TIMEOUT(!profile->property("visible").toBool(), 2000);
        };

        QCOMPARE(attached(avatar, QStringLiteral("Accessible.name")).toString(),
                 QStringLiteral("View profile"));
        QVERIFY(!attached(avatar, QStringLiteral("Accessible.ignored")).toBool());

        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centreOf(avatar));
        QTRY_VERIFY2_WITH_TIMEOUT(profile->property("opened").toBool(),
                                  "a tap on a DM's header avatar opened no profile",
                                  2000);
        QCOMPARE(profile->property("userId").toString(), bob);
        QVERIFY(!root->property("infoOpen").toBool());
        closeProfile();

        // The column's Room Information handler is off on the mock (no room
        // management); turned on here so a tap leaking through is counted.
        QVERIFY(QQmlProperty::write(columnTap, QStringLiteral("enabled"), true));
        const QMetaMethod tapped = signalNamed(columnTap, "tapped");
        QVERIFY(tapped.isValid());
        QSignalSpy columnTaps(columnTap, tapped);

        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centreOf(title));
        QTRY_VERIFY2_WITH_TIMEOUT(profile->property("opened").toBool(),
                                  "a tap on a DM's header name opened no profile",
                                  2000);
        QCOMPARE(profile->property("userId").toString(), bob);
        QCOMPARE(columnTaps.count(), 0);
        closeProfile();

        const QPoint belowName = identity->mapToScene(
            QPointF(2, identity->height() - 2)).toPoint();
        QVERIFY2(!title->contains(title->mapFromScene(belowName)),
                 "the control point lands on the name");
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, belowName);
        QTRY_COMPARE_WITH_TIMEOUT(columnTaps.count(), 1, 2000);
        QVERIFY(!profile->property("opened").toBool());

        QVariantMap trio;
        trio.insert(QStringLiteral("name"), QStringLiteral("Trio"));
        trio.insert(QStringLiteral("isDirect"), true);
        trio.insert(QStringLiteral("identityColorKey"),
                    QStringLiteral("!trio:mock.local"));
        root->setProperty("currentRoom", trio);
        QCoreApplication::processEvents();
        QVERIFY(attached(avatar, QStringLiteral("Accessible.ignored")).toBool());
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centreOf(avatar));
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, centreOf(title));
        QTRY_COMPARE_WITH_TIMEOUT(columnTaps.count(), 2, 2000);
        QTest::qWait(300);
        QVERIFY2(!profile->property("opened").toBool()
                     && !profile->property("visible").toBool(),
                 "a group room's header opened a profile");
    }

    void theWholeHeaderCardOpensRoomInformationWithOrWithoutATopic()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window,
                                          QStringLiteral("!general:example.org"),
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        window.resize(1000, 700);
        root->setSize(QSizeF(1000, 700));
        QCoreApplication::processEvents();

        auto *band = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderBand"));
        auto *identity = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderIdentity"));
        auto *title = root->findChild<QQuickItem *>(QStringLiteral("roomHeaderTitle"));
        auto *profile = root->findChild<QObject *>(
            QStringLiteral("senderProfilePopover"));
        QVERIFY(band && identity && title && profile);

        // The mock has no room management, so the handlers are off; forced on
        // so a tap is counted. Their objectNames are the contract.
        QObject *identityTap = root->findChild<QObject *>(
            QStringLiteral("roomHeaderIdentityTap"));
        QVERIFY(identityTap);
        QVERIFY(QQmlProperty::write(identityTap, QStringLiteral("enabled"), true));
        QSignalSpy identityTaps(identityTap, signalNamed(identityTap, "tapped"));
        const auto taps = [&] { return identityTaps.count(); };
        QStringList failures;

        struct Variant {
            const char *label; bool topic; bool encrypted; bool dm;
        };
        const Variant variants[] = {
            { "public group, topic", true, false, false },
            { "private group, no topic", false, true, false },
            { "private group, topic", true, true, false },
            { "public group, no topic", false, false, false },
            { "DM, no topic", false, true, true },
        };
        int clicks = 0;
        for (const Variant &v : variants) {
            QVariantMap room;
            room.insert(QStringLiteral("name"), QStringLiteral("Lightning Support"));
            room.insert(QStringLiteral("topic"),
                        v.topic ? QStringLiteral("Help with Lightning") : QString());
            room.insert(QStringLiteral("encrypted"), v.encrypted);
            room.insert(QStringLiteral("isDirect"), v.dm);
            room.insert(QStringLiteral("identityColorKey"),
                        v.dm ? QStringLiteral("@bob:mock.local")
                             : QStringLiteral("!support:mock.local"));
            root->setProperty("currentRoom", room);
            QTest::qWait(60);
            QCoreApplication::processEvents();
            QVERIFY(QMetaObject::invokeMethod(profile, "close"));

            const QRectF bandRect(band->mapToScene(QPointF(0, 0)),
                                  QSizeF(band->width(), band->height()));
            const double midY = bandRect.center().y();
            const QPointF idTop = identity->mapToScene(QPointF(0, 0));
            const double idX = idTop.x();
            const double titleRight = title->mapToScene(
                QPointF(title->width(), 0)).x();
            const double identityRight = idX + identity->width();
            QVERIFY2(identity->width() > 300,
                     qPrintable(QStringLiteral(
                         "fixture: the identity column has no width to probe "
                         "(%1: identity w=%2 h=%3, title w=%4)")
                         .arg(QLatin1String(v.label)).arg(identity->width())
                         .arg(identity->height()).arg(title->width())));

            struct Probe { const char *where; QPoint p; };
            const Probe probes[] = {
                { "top of the band over the name",
                  QPoint(int(idX + 10), int(bandRect.top() + 4)) },
                { "bottom of the band over the name",
                  QPoint(int(idX + 10), int(bandRect.bottom() - 4)) },
                { "just right of the name, on its line",
                  QPoint(int(titleRight + 8), int(midY)) },
                { "far right of the card, mid height",
                  QPoint(int(identityRight - 20), int(midY)) },
                { "far right of the card, top of the band",
                  QPoint(int(identityRight - 20), int(bandRect.top() + 4)) },
                { "far right of the card, bottom of the band",
                  QPoint(int(identityRight - 20), int(bandRect.bottom() - 4)) },
            };
            for (const Probe &pr : probes) {
                const int before = taps();
                QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, pr.p);
                QTest::qWait(30);
                ++clicks;
                if (taps() != before + 1)
                    failures << QStringLiteral("%1 / %2: %3 taps")
                                    .arg(QLatin1String(v.label),
                                         QLatin1String(pr.where))
                                    .arg(taps() - before);
                if (profile->property("opened").toBool())
                    failures << QStringLiteral("%1 / %2: opened a profile")
                                    .arg(QLatin1String(v.label),
                                         QLatin1String(pr.where));
            }

            const int before = taps();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                              title->mapToScene(QPointF(title->width() / 2.0,
                                                        title->height() / 2.0))
                                  .toPoint());
            QTest::qWait(60);
            ++clicks;
            if (v.dm) {
                if (!profile->property("opened").toBool()
                        || profile->property("userId").toString()
                               != QStringLiteral("@bob:mock.local"))
                    failures << QStringLiteral("%1 / name: no profile card")
                                    .arg(QLatin1String(v.label));
                if (taps() != before)
                    failures << QStringLiteral("%1 / name: also toggled the panel")
                                    .arg(QLatin1String(v.label));
            } else if (taps() != before + 1) {
                failures << QStringLiteral("%1 / name: %2 taps")
                                .arg(QLatin1String(v.label)).arg(taps() - before);
            }
            QMetaObject::invokeMethod(profile, "close");
        }
        QVERIFY2(clicks == 5 * 7, "fixture: not every probe was clicked");
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
    }

    void theRoomTitleOutranksTheHeaderIconRowAtEveryWidth()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window,
                                          QStringLiteral("!general:example.org"),
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        auto *identity = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderIdentity"));
        auto *actions = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderActions"));
        auto *spacer = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderSpacer"));
        auto *title = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderTitle"));
        QVERIFY(identity);
        QVERIFY(actions);
        QVERIFY(spacer);
        QVERIFY(title);
        auto *header = identity->parentItem();
        QVERIFY(header);

        QQmlExpression titlePxExpr(qmlContext(root), root,
                                   QStringLiteral(
                                       "AppTheme.scaled(AppTheme.textTitle)"));
        const double titlePx = titlePxExpr.evaluate().toDouble();
        QVERIFY2(titlePx > 0, "the title's scaled font size did not evaluate");
        const double inkFloor = 15.0 * 0.53 * titlePx;

        struct Variant { const char *label; bool topic; bool lock; };
        const Variant variants[] = {
            { "topic+lock", true, true },
            { "no topic, no lock", false, false },
        };
        const double kOneIconSlot = 34.0;
        QStringList failures;
        int measured = 0;
        int sawTruncated = 0;
        int sawStarved = 0;
        double worstInk = -1.0;

        for (const double scale : { 1.0, 1.4 }) {
        QQmlExpression setScale(qmlContext(root), root,
                                QStringLiteral("AppTheme.textScale = %1")
                                    .arg(scale));
        setScale.evaluate();
        QTest::qWait(60);
        QCoreApplication::processEvents();
        const double scaledTitlePx = titlePxExpr.evaluate().toDouble();
        const double scaledInkFloor = 15.0 * 0.53 * scaledTitlePx;

        for (const Variant &v : variants) {
            QVariantMap room;
            room.insert(QStringLiteral("name"),
                        QStringLiteral("Lightning development and release chat"));
            room.insert(QStringLiteral("topic"),
                        v.topic ? QStringLiteral("Where the work happens")
                                : QString());
            room.insert(QStringLiteral("encrypted"), v.lock);
            root->setProperty("currentRoom", room);
            QTest::qWait(60);
            QCoreApplication::processEvents();

            for (const int w : { 320, 360, 400, 460, 520, 600, 640, 760,
                                 900, 1200 }) {
                root->setSize(QSizeF(w, 700));
                QTest::qWait(60);
                QCoreApplication::processEvents();
                ++measured;

                const double ink = title->width();
                const double natural =
                    std::ceil(title->property("implicitWidth").toDouble());
                const bool truncated = title->property("truncated").toBool();
                if (truncated)
                    ++sawTruncated;
                if (worstInk < 0.0 || ink < worstInk)
                    worstInk = ink;

                const bool titleStarved =
                    ink + 0.5 < std::min(scaledInkFloor, natural);
                if (titleStarved && actions->width() > kOneIconSlot + 0.5)
                    failures.append(QStringLiteral(
                        "at text scale %10: %1 at pane %2 (header %3): the "
                        "room title is %4 px of ink where its own text wants "
                        "%5 and fifteen "
                        "characters are %6 — while the icon row still holds "
                        "%7 px, more than the one slot an overflow costs. "
                        "identity %8, spacer %9.")
                        .arg(QLatin1String(v.label)).arg(w)
                        .arg(header->width()).arg(ink).arg(natural)
                        .arg(int(scaledInkFloor)).arg(actions->width())
                        .arg(identity->width()).arg(spacer->width())
                        .arg(scale));
                if (titleStarved)
                    ++sawStarved;

                if (truncated && spacer->width() > 0.5)
                    failures.append(QStringLiteral(
                        "at text scale %9: %1 at pane %2 (header %3): the "
                        "room title elided at %4 px of a %5 px name while %6 "
                        "px of header sat EMPTY beside it (identity %7, "
                        "icons %8)")
                        .arg(QLatin1String(v.label)).arg(w)
                        .arg(header->width()).arg(ink).arg(natural)
                        .arg(spacer->width()).arg(identity->width())
                        .arg(actions->width()).arg(scale));
            }
        }

        }

        QQmlExpression resetScale(qmlContext(root), root,
                                  QStringLiteral("AppTheme.textScale = 1.0"));
        resetScale.evaluate();
        QCoreApplication::processEvents();

        qInfo("room header sweep: %d widths measured, %d elided, %d below the "
              "ink floor (each with the row already down to one control); "
              "title font %.0f px at scale 1.0, floor %.0f px, narrowest "
              "title %.0f px",
              measured, sawTruncated, sawStarved, titlePx, inkFloor, worstInk);
        QVERIFY2(sawTruncated > 0,
                 "no width in this sweep elided the room title, so neither "
                 "invariant was actually exercised");
        QVERIFY2(failures.isEmpty(),
                 qPrintable(failures.join(QStringLiteral("\n  "))));

        root->setSize(QSizeF(700, 700));
        QCoreApplication::processEvents();
    }

    void everyFoldedHeaderActionIsStillReachable()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window,
                                          QStringLiteral("!general:example.org"),
                                          {}, 0, 700, &timeline);
        QVERIFY(root);
        auto *actions = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderActions"));
        auto *overflow = root->findChild<QQuickItem *>(
            QStringLiteral("roomHeaderOverflowButton"));
        QVERIFY(actions);
        QVERIFY(overflow);

        QVariantMap room;
        room.insert(QStringLiteral("name"),
                    QStringLiteral("Lightning development and release chat"));
        root->setProperty("currentRoom", room);
        QTest::qWait(60);
        QCoreApplication::processEvents();

        root->setSize(QSizeF(1200, 700));
        QTest::qWait(60);
        QCoreApplication::processEvents();
        QCOMPARE(actions->property("foldedActions").toStringList().size(), 0);
        QVERIFY2(overflow->isVisible(),
                 "an open room has no More button on a wide header, so its "
                 "chat background and settings are unreachable from the header");
        {
            auto *wideMenu = root->findChild<QObject *>(
                QStringLiteral("roomHeaderOverflowMenu"));
            QVERIFY(wideMenu);
            QMetaObject::invokeMethod(wideMenu, "open");
            QTRY_VERIFY(wideMenu->property("opened").toBool());
            QCoreApplication::processEvents();
            auto *bgRow = wideMenu->findChild<QObject *>(
                QStringLiteral("roomHeaderChatBackground"));
            QVERIFY2(bgRow, "the More menu has no Chat background row");
            QVERIFY2(bgRow->property("visible").toBool(),
                     "the Chat background row is hidden in the More menu");
            auto *dialog = root->findChild<QObject *>(
                QStringLiteral("roomChatBackgroundDialog"));
            QVERIFY(dialog);
            QMetaObject::invokeMethod(bgRow, "triggered");
            QTRY_VERIFY2(dialog->property("opened").toBool(),
                         "Chat background… did not open the background dialog");
            QCOMPARE(dialog->property("scopeKind").toString(),
                     QStringLiteral("room"));
            QVERIFY(!dialog->property("roomId").toString().isEmpty());
            QMetaObject::invokeMethod(dialog, "close");
            QMetaObject::invokeMethod(wideMenu, "close");
            QCoreApplication::processEvents();
        }

        root->setSize(QSizeF(320, 700));
        QTest::qWait(60);
        QCoreApplication::processEvents();
        const QStringList folded =
            actions->property("foldedActions").toStringList();
        QVERIFY2(!folded.isEmpty(),
                 "the narrowest supported pane folded nothing, so this case "
                 "measured a header that was never under pressure");
        QVERIFY2(overflow->isVisible(),
                 "actions folded out of the row with no overflow button to "
                 "reach them through");

        auto *menu = root->findChild<QObject *>(
            QStringLiteral("roomHeaderOverflowMenu"));
        QVERIFY(menu);
        QMetaObject::invokeMethod(menu, "open");
        QTRY_VERIFY(menu->property("opened").toBool());
        QCoreApplication::processEvents();
        const QMap<QString, QString> rowFor = {
            { QStringLiteral("startVoiceCallButton"),
              QStringLiteral("overflowStartVoiceCall") },
            { QStringLiteral("pinnedMessagesButton"),
              QStringLiteral("overflowPinnedMessages") },
            { QStringLiteral("threadsViewButton"),
              QStringLiteral("overflowThreads") },
            { QStringLiteral("timelineSearchButton"),
              QStringLiteral("overflowSearch") },
            { QStringLiteral("memberPanelButton"),
              QStringLiteral("overflowMembers") },
            { QStringLiteral("roomInfoButton"),
              QStringLiteral("overflowRoomInfo") },
        };
        QStringList unreachable;
        for (auto it = rowFor.cbegin(); it != rowFor.cend(); ++it) {
            auto *button = root->findChild<QObject *>(it.key());
            QVERIFY2(button, qPrintable(it.key()));
            auto *row = menu->findChild<QObject *>(it.value());
            QVERIFY2(row, qPrintable(it.value()));
            const bool isFolded = button->property("folded").toBool();
            if (isFolded != row->property("visible").toBool())
                unreachable.append(QStringLiteral(
                    "%1 is %2 but its menu row is %3")
                    .arg(it.key(),
                         isFolded ? QStringLiteral("folded out of the row")
                                  : QStringLiteral("still an icon"),
                         row->property("visible").toBool()
                             ? QStringLiteral("shown") : QStringLiteral("hidden")));
            if (isFolded && row->property("text").toString().isEmpty())
                unreachable.append(QStringLiteral(
                    "%1 folded into a menu row with no label").arg(it.key()));
        }
        QVERIFY2(unreachable.isEmpty(),
                 qPrintable(unreachable.join(QStringLiteral("\n  "))));

        auto *searchRow = menu->findChild<QObject *>(
            QStringLiteral("overflowSearch"));
        QVERIFY(searchRow);
        if (searchRow->property("visible").toBool()) {
            const bool before = root->property("searchOpen").toBool();
            QMetaObject::invokeMethod(searchRow, "triggered");
            QCoreApplication::processEvents();
            QVERIFY2(root->property("searchOpen").toBool() != before,
                     "the overflow row for Search messages did not open the "
                     "search panel");
            if (root->property("searchOpen").toBool() != before)
                root->setProperty("searchOpen", before);
        }
        QMetaObject::invokeMethod(menu, "close");
        QCoreApplication::processEvents();

        root->setSize(QSizeF(700, 700));
        QCoreApplication::processEvents();
    }

    void readReceiptChipsRenderBoundedAndCollapseWhenEmpty()
    {
        const auto visualChildrenByName =
            [](QQuickItem *root, const QString &name) {
                QList<QQuickItem *> out;
                QList<QQuickItem *> stack{root};
                while (!stack.isEmpty()) {
                    QQuickItem *item = stack.takeLast();
                    const auto kids = item->childItems();
                    for (QQuickItem *k : kids) {
                        if (k->objectName() == name)
                            out.append(k);
                        stack.append(k);
                    }
                }
                return out;
            };

        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QString{});
        fixture.insert(QStringLiteral("eventId"), QString{});
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"),
                       QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("Receipt fixture body"));
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});

        QStringList warnings;
        const auto loadDelegate =
            [this, &controller, &warnings](
                QQmlApplicationEngine &engine,
                const QVariantMap &model, qreal width) -> QQuickItem * {
            connect(&engine, &QQmlEngine::warnings, this,
                    [&warnings](const QList<QQmlError> &errors) {
                        for (const auto &e : errors)
                            warnings << e.toString();
                    });
            engine.rootContext()->setContextProperty("app", &controller);
            engine.rootContext()->setContextProperty("model", model);
            QSignalSpy createdSpy(&engine,
                                  &QQmlApplicationEngine::objectCreated);
            engine.loadFromModule(QStringLiteral("MatrixClient"),
                                  QStringLiteral("MessageDelegate"));
            if (createdSpy.isEmpty()
                && !createdSpy.wait(kSignalTimeoutMs))
                return nullptr;
            auto *root = qobject_cast<QQuickItem *>(
                createdSpy.at(0).at(0).value<QObject *>());
            if (root == nullptr)
                return nullptr;
            root->setWidth(width);
            QCoreApplication::processEvents();
            return root;
        };

        {
            QQmlApplicationEngine engine;
            QVariantMap empty = fixture;
            empty.insert(QStringLiteral("readReceipts"), QVariantList{});
            QQuickItem *root = loadDelegate(engine, empty, 1400);
            QVERIFY(root != nullptr);
            QVERIFY(!root->findChild<QQuickItem *>(
                QStringLiteral("readReceiptStrip")));
            QVERIFY(visualChildrenByName(root,
                                         QStringLiteral("readReceiptChip"))
                        .isEmpty());
            QVERIFY(root->implicitHeight() > 0.0);
        }

        QVariantList receipts;
        const QStringList names = {
            QStringLiteral("Alice"), QStringLiteral("Bob"),
            QStringLiteral("Carol"), QStringLiteral("Dave"),
            QStringLiteral("Erin"), QStringLiteral("Frank"),
        };
        for (int i = 0; i < names.size(); ++i) {
            QVariantMap r;
            r.insert(QStringLiteral("userId"),
                     QStringLiteral("@u%1:mock.local").arg(i));
            r.insert(QStringLiteral("displayName"), names.at(i));
            r.insert(QStringLiteral("avatarMxc"), QString{});
            r.insert(QStringLiteral("tsMs"),
                     Q_INT64_C(1700000000000) - i * 1000);
            receipts.append(r);
        }
        {
            QQmlApplicationEngine engine;
            QVariantMap populated = fixture;
            populated.insert(QStringLiteral("readReceipts"), receipts);
            populated.insert(QStringLiteral("readReceiptsTotal"), 6);
            QQuickItem *root = loadDelegate(engine, populated, 1400);
            QVERIFY(root != nullptr);
            auto *strip = root->findChild<QQuickItem *>(
                QStringLiteral("readReceiptStrip"));
            QVERIFY(strip != nullptr);
            QVERIFY(strip->isVisible());
            QCOMPARE(visualChildrenByName(root,
                                          QStringLiteral("readReceiptChip"))
                         .size(),
                     3);
            auto *overflow = root->findChild<QQuickItem *>(
                QStringLiteral("readReceiptOverflow"));
            QVERIFY(overflow != nullptr);
            QVERIFY(overflow->isVisible());
            QCOMPARE(overflow->childItems().first()
                         ->property("text").toString(),
                     QStringLiteral("+3"));
            QCOMPARE(strip->property("summary").toString(),
                     QStringLiteral("Read by Alice, Bob and 4 others"));
            auto *presentation = root->findChild<QQuickItem *>(
                QStringLiteral("messagePresentationRow"));
            QVERIFY(presentation != nullptr);
            const qreal messageBottom = presentation->mapToScene(
                QPointF(0, presentation->height())).y();
            auto *chipRow = strip->findChild<QQuickItem *>(
                QStringLiteral("readReceiptRow"));
            QVERIFY(chipRow != nullptr);
            const qreal receiptBottom = chipRow->mapToScene(
                QPointF(0, chipRow->height())).y();
            QVERIFY2(qAbs(receiptBottom - messageBottom) < 1.5,
                     qPrintable(QStringLiteral(
                         "receiptBottom=%1 messageBottom=%2")
                         .arg(receiptBottom).arg(messageBottom)));
            const qreal stripRight =
                strip->mapToScene(QPointF(strip->width(), 0)).x();
            const qreal chipsRight =
                chipRow->mapToScene(QPointF(chipRow->width(), 0)).x();
            QVERIFY2(qAbs(chipsRight - stripRight) < 1.5,
                     qPrintable(QStringLiteral(
                         "chipsRight=%1 stripRight=%2")
                         .arg(chipsRight).arg(stripRight)));
        }

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void readReceiptChipsRideTheRightEdgeRail()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("itemId"), QString{});
        fixture.insert(QStringLiteral("eventId"), QString{});
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("reactions"), QVariantList{});
        QVariantList receipts;
        for (const auto &reader :
             { QStringLiteral("@carol:mock.local"),
               QStringLiteral("@dave:mock.local") }) {
            QVariantMap r;
            r.insert(QStringLiteral("userId"), reader);
            r.insert(QStringLiteral("displayName"),
                     reader.mid(1, reader.indexOf(QLatin1Char(':')) - 1));
            r.insert(QStringLiteral("avatarMxc"), QString{});
            receipts.append(r);
        }
        fixture.insert(QStringLiteral("readReceipts"), receipts);
        fixture.insert(QStringLiteral("readReceiptsTotal"), receipts.size());

        QStringList warnings;
        const auto loadDelegate =
            [this, &controller, &warnings](
                QQmlApplicationEngine &engine,
                const QVariantMap &model, qreal width) -> QQuickItem * {
            connect(&engine, &QQmlEngine::warnings, this,
                    [&warnings](const QList<QQmlError> &errors) {
                        for (const auto &e : errors)
                            warnings << e.toString();
                    });
            engine.rootContext()->setContextProperty("app", &controller);
            engine.rootContext()->setContextProperty("model", model);
            QSignalSpy createdSpy(&engine,
                                  &QQmlApplicationEngine::objectCreated);
            engine.loadFromModule(QStringLiteral("MatrixClient"),
                                  QStringLiteral("MessageDelegate"));
            if (createdSpy.isEmpty()
                && !createdSpy.wait(kSignalTimeoutMs))
                return nullptr;
            auto *root = qobject_cast<QQuickItem *>(
                createdSpy.at(0).at(0).value<QObject *>());
            if (root == nullptr)
                return nullptr;
            root->setWidth(width);
            QCoreApplication::processEvents();
            return root;
        };

        struct Shape {
            const char *sender;
            const char *name;
            const char *body;
        };
        const Shape shapes[] = {
            { "@bob:mock.local", "Bob", "Fr fr" },
            { "@bob:mock.local", "Bob",
              "https://www.example.com/a/very/long/unbroken/path/segment/"
              "that/cannot/wrap/anywhere/because/it/has/no/spaces/at/all" },
            { "@sponge:mock.local", "SpongeMan", "Fr fr" },
        };

        QVector<qreal> railOffsets;
        for (const Shape &shape : shapes) {
            QQmlApplicationEngine engine;
            QVariantMap model = fixture;
            model.insert(QStringLiteral("sender"),
                         QString::fromLatin1(shape.sender));
            model.insert(QStringLiteral("senderDisplayName"),
                         QString::fromLatin1(shape.name));
            model.insert(QStringLiteral("senderInitials"),
                         QString::fromLatin1(shape.name).left(1));
            model.insert(QStringLiteral("body"),
                         QString::fromLatin1(shape.body));
            QQuickItem *root = loadDelegate(engine, model, 1400);
            QVERIFY(root != nullptr);
            auto *strip = root->findChild<QQuickItem *>(
                QStringLiteral("readReceiptStrip"));
            QVERIFY(strip != nullptr);
            QVERIFY(strip->isVisible());
            auto *chipRow = strip->findChild<QQuickItem *>(
                QStringLiteral("readReceiptRow"));
            QVERIFY(chipRow != nullptr);

            const qreal stripRight =
                strip->mapToScene(QPointF(strip->width(), 0)).x();
            const qreal chipsRight =
                chipRow->mapToScene(QPointF(chipRow->width(), 0)).x();
            QVERIFY2(qAbs(chipsRight - stripRight) < 1.5,
                     qPrintable(QStringLiteral(
                         "chips must ride the right-edge rail: sender=%1 "
                         "chipsRight=%2 stripRight=%3")
                         .arg(QString::fromLatin1(shape.sender))
                         .arg(chipsRight).arg(stripRight)));
            QVERIFY(chipRow->x() >= root->property("avatarGutterWidth")
                                        .toReal() - 0.5);
            railOffsets.append(stripRight - chipsRight);
        }
        QCOMPARE(railOffsets.size(), 3);
        QVERIFY2(qAbs(railOffsets[0] - railOffsets[1]) < 1.0
                     && qAbs(railOffsets[1] - railOffsets[2]) < 1.0,
                 qPrintable(QStringLiteral(
                     "shapes disagree on the receipt rail: %1 / %2 / %3")
                     .arg(railOffsets[0]).arg(railOffsets[1])
                     .arg(railOffsets[2])));

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void readReceiptChipAvatarsLoadThroughRealProviderPath()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        // Enable the mock media bridge and register the readers' avatar bytes.
        const auto pngBytes = [](const QColor &color) {
            QImage image(64, 64, QImage::Format_ARGB32);
            image.fill(color);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return bytes;
        };
        const QString carolMxc = QStringLiteral("mxc://mock.local/carol-av");
        const QString daveMxc = QStringLiteral("mxc://mock.local/dave-av");
        mock->setSupportsMediaBridgeForTest(true);
        mock->setAvatarBytesForTest(carolMxc, pngBytes(QColor(200, 40, 40)),
                                    QStringLiteral("image/png"));
        mock->setAvatarBytesForTest(daveMxc, pngBytes(QColor(40, 40, 200)),
                                    QStringLiteral("image/png"));

        mock->setRoomMemberForTest(
            roomId, { QStringLiteral("@carol:mock.local"),
                      QStringLiteral("Carol"), carolMxc });

        controller.setCurrentRoomId(roomId);

        TimelineEvent readByCarol;
        readByCarol.eventId = QStringLiteral("$receipt-av-carol");
        readByCarol.itemId = QStringLiteral("uid-receipt-av-carol");
        readByCarol.roomId = roomId;
        readByCarol.sender = QStringLiteral("@bob:mock.local");
        readByCarol.senderDisplayName = QStringLiteral("Bob");
        readByCarol.body = QStringLiteral("read by carol");
        readByCarol.timestamp = QDateTime::currentDateTimeUtc();
        readByCarol.type = TimelineEvent::TextMessage;
        readByCarol.status = TimelineEvent::Sent;
        readByCarol.readBy = { { QStringLiteral("@carol:mock.local"),
                                 Q_INT64_C(1700000002000) } };
        readByCarol.readByTotal = 1;

        TimelineEvent readByDave = readByCarol;
        readByDave.eventId = QStringLiteral("$receipt-av-dave");
        readByDave.itemId = QStringLiteral("uid-receipt-av-dave");
        readByDave.body = QStringLiteral("read by dave");
        readByDave.timestamp = QDateTime::currentDateTimeUtc().addSecs(1);
        readByDave.readBy = { { QStringLiteral("@dave:mock.local"),
                                Q_INT64_C(1700000003000) } };
        readByDave.readByTotal = 1;

        mock->resetTimelineForTest(roomId, { readByCarol, readByDave },
0);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.addImageProvider(
            QStringLiteral("lightning-media"),
            new MediaImageProvider(controller.mediaBridge()));
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(1200, 900);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(),
            kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 2,
                                 kSignalTimeoutMs);
        QMetaObject::invokeMethod(timeline, "forceLayout");
        QCoreApplication::processEvents();

        const auto chipAvatar = [](QQuickItem *rowItem) -> QQuickItem * {
            const auto chips = findVisualChildren(
                rowItem, QStringLiteral("readReceiptChip"));
            if (chips.size() != 1)
                return nullptr;
            const auto images = findVisualChildren(
                chips.first(), QStringLiteral("avatarImage"));
            if (images.size() != 1)
                return nullptr;
            return images.first()->parentItem();
        };

        QQuickItem *carolRow = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (((carolRow = itemForSourceRow(timeline, 0)) != nullptr),
             carolRow != nullptr),
            kSignalTimeoutMs);
        QQuickItem *carolAvatar = chipAvatar(carolRow);
        QVERIFY2(carolAvatar != nullptr,
                 "carol's receipt chip has no Avatar with an avatarImage");
        QCOMPARE(carolAvatar->property("mxc").toString(), carolMxc);
        QTRY_COMPARE_WITH_TIMEOUT(
            carolAvatar->property("presentationState").toString(),
            QStringLiteral("ready"), kSignalTimeoutMs);
        {
            const auto images = findVisualChildren(
                carolAvatar, QStringLiteral("avatarImage"));
            QCOMPARE(images.size(), 1);
            const QString source =
                images.first()->property("source").toUrl().toString();
            QVERIFY2(source.startsWith(
                         QStringLiteral("image://lightning-media/")),
                     qPrintable(QStringLiteral(
                         "chip avatar source is not provider-backed: '%1'")
                         .arg(source)));
            QVERIFY(images.first()->isVisible());
        }

        QQuickItem *daveRow = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (((daveRow = itemForSourceRow(timeline, 1)) != nullptr),
             daveRow != nullptr),
            kSignalTimeoutMs);
        QQuickItem *daveAvatar = chipAvatar(daveRow);
        QVERIFY2(daveAvatar != nullptr,
                 "dave's receipt chip has no Avatar with an avatarImage");
        QCOMPARE(daveAvatar->property("mxc").toString(), QString{});
        QCOMPARE(daveAvatar->property("presentationState").toString(),
                 QStringLiteral("missing"));

        mock->setRoomMemberForTest(
            roomId, { QStringLiteral("@dave:mock.local"),
                      QStringLiteral("Dave"), daveMxc });

        QTRY_VERIFY_WITH_TIMEOUT(
            (daveAvatar = chipAvatar(daveRow)) != nullptr
                && daveAvatar->property("mxc").toString() == daveMxc,
            kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(
            daveAvatar->property("presentationState").toString(),
            QStringLiteral("ready"), kSignalTimeoutMs);

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void newMessagesDividerStaysWhilePinnedToBottom()
    {
        AppController controller(AppController::MockBackend);
        const QString roomId = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomId.isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        controller.setCurrentRoomId(roomId);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        const QDateTime base =
            QDateTime::currentDateTimeUtc().addSecs(-600);
        QList<TimelineEvent> events;
        for (int i = 0; i < 3; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$m%1").arg(i);
            e.itemId = QStringLiteral("uid-m%1").arg(i);
            e.roomId = roomId;
            e.sender = QStringLiteral("@alice:mock.local");
            e.body = QStringLiteral("message %1").arg(i);
            e.timestamp = base.addSecs(i * 60);
            events.append(e);
        }
        TimelineEvent marker;
        marker.itemId = QStringLiteral("uid-marker");
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        events.append(marker);
        TimelineEvent unread;
        unread.eventId = QStringLiteral("$unread");
        unread.itemId = QStringLiteral("uid-unread");
        unread.roomId = roomId;
        unread.sender = QStringLiteral("@bob:mock.local");
        unread.body = QStringLiteral("the unread one");
        unread.timestamp = base.addSecs(300);
        events.append(unread);
        mock->resetTimelineForTest(roomId, events,0);

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);
        QVERIFY(timeline->property("stickToBottom").toBool());

        QQuickItem *markerItem = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (((markerItem = itemForSourceRow(timeline, 3)) != nullptr),
             markerItem != nullptr),
            5000);
        auto *divider = markerItem->findChild<QQuickItem *>(
            QStringLiteral("unreadDivider"));
        QVERIFY(divider != nullptr);

        QTRY_VERIFY_WITH_TIMEOUT(divider->isVisible(), 2000);
        const qreal pinnedHeight = markerItem->implicitHeight();
        QVERIFY(pinnedHeight >= 28.0);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QTest::qWait(50);
        QVERIFY(divider->isVisible());
        QCOMPARE(markerItem->implicitHeight(), pinnedHeight);
        QVERIFY(timeline->setProperty("stickToBottom", true));
        QTest::qWait(50);
        QVERIFY(divider->isVisible());
        QCOMPARE(markerItem->implicitHeight(), pinnedHeight);

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void threadScrollMotionIsIsolatedFromRoomTimeline()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        auto *roomScroll = controller.timelineScroll();
        auto *threadScroll = controller.threadScroll();
        QVERIFY(roomScroll != nullptr);
        QVERIFY(threadScroll != nullptr);
        QVERIFY(roomScroll != threadScroll);

        controller.settings()->setTimelineWheelSpeed(2);
        QCOMPARE(roomScroll->wheelSpeed(), TimelineScrollController::VeryFast);
        QCOMPARE(threadScroll->wheelSpeed(),
                 TimelineScrollController::VeryFast);
        controller.settings()->setTimelineWheelSpeed(1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        QObject *root = createdSpy.at(0).at(0).value<QObject *>();
        QVERIFY(root != nullptr);

        const QString rootId = fixtureThreadRootId(controller);
        controller.thread()->openThread(QStringLiteral("!general:mock.local"),
                                        rootId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);

        threadScroll->wheelNotch(-120.0, 100.0, 0.0, 5000.0, 600.0);
        QVERIFY(threadScroll->motionActive());
        QVERIFY(!roomScroll->motionActive());

        roomScroll->wheelNotch(-120.0, 100.0, 0.0, 5000.0, 600.0);
        QVERIFY(roomScroll->motionActive());
        threadScroll->cancel();
        QVERIFY(roomScroll->motionActive());
        roomScroll->cancel();

        threadScroll->wheelNotch(-120.0, 100.0, 0.0, 5000.0, 600.0);
        QVERIFY(threadScroll->motionActive());
        controller.thread()->close();
        QTRY_COMPARE_WITH_TIMEOUT(threadScroll->motionActive(), false,
                                  kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void paginationPrependPreservesConcurrentScroll()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(60);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,2);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        const int anchorRow = 5;
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, anchorRow));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());

        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        controller.pagination()->requestNearTop();
        QCoreApplication::processEvents();

        const double capturedContentY = timeline->property("contentY").toDouble();
        const double anchorLastY =
            timeline->property("viewAnchorLastY").toDouble();
        constexpr double simulatedScrollDelta = 40.0;
        const double scrolledContentY = capturedContentY - simulatedScrollDelta;
        QVERIFY(timeline->setProperty("contentY", scrolledContentY));
        auto *settleTimer = timeline->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        QVERIFY(settleTimer != nullptr);
        GestureHold gesture(settleTimer);
        QVERIFY2(timeline->property("userScrollActive").toBool(),
                 "the simulated gesture must read as an active scroll "
                 "session, or the correction takes the wrong branch");

        QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
        QVERIFY2(completedSpy.constFirst().at(0).toInt() > 0,
                 "fixture assumption: the near-top page must insert rows");

        const int newRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(newRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (((anchorItem = itemForSourceRow(timeline, newRow)) != nullptr),
             anchorItem != nullptr),
            kSignalTimeoutMs);

        Q_UNUSED(anchorItem);
        Q_UNUSED(anchorLastY);
        double actual = 0;
        QTRY_VERIFY_WITH_TIMEOUT(
            (actual = timeline->property("contentY").toDouble(),
             qAbs(actual - scrolledContentY) < 0.5),
            kSignalTimeoutMs);
        QVERIFY2(qAbs(actual - scrolledContentY) < 0.5,
                 qPrintable(QStringLiteral(
                     "pagination prepend wrote over an active gesture's "
                     "position: actual=%1 scrolledContentY=%2 (a stale "
                     "absolute restore would move it toward %3)")
                     .arg(actual).arg(scrolledContentY)
                     .arg(capturedContentY)));
        QVERIFY2(timeline->property("userScrollActive").toBool(),
                 "the gesture must still read active at the assertion "
                 "point, or an idle restore — not the mechanism under "
                 "test — could be the reason nothing moved");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void paginationPrependPreservesWheelGlide()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(80);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,2);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 5));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        QVERIFY(!timeline->property("viewAnchorId").toString().isEmpty());

        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        controller.pagination()->requestNearTop();
        QCoreApplication::processEvents();

        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);
        const double glideStartY = timeline->property("contentY").toDouble();
        double minY = 0, maxY = 0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        for (int notch = 0; notch < 6; ++notch)
            scroll->wheelNotch(120.0,
                               timeline->property("contentY").toDouble(),
                               minY, maxY, timeline->height());
        QTRY_VERIFY_WITH_TIMEOUT(scroll->motionActive(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentY").toDouble() < glideStartY - 10.0,
            2000);
        QVERIFY(scroll->motionActive());
        const double beforeCompletionY =
            timeline->property("contentY").toDouble();

        QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
        QVERIFY2(completedSpy.constFirst().at(0).toInt() > 0,
                 "fixture assumption: the near-top page must insert rows");

        QVERIFY2(scroll->motionActive(),
                 "a real pagination completion killed the in-flight wheel "
                 "glide (the 'snaps me back partway' defect)");
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentY").toDouble()
                < beforeCompletionY - 20.0,
            kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!scroll->motionActive(), kSignalTimeoutMs);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void topEdgePrependKeepsReaderOnTheSameRowMidGesture()
    {
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
    }
    struct TraceGuard { ~TraceGuard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } traceGuard;

        AppController controller(AppController::MockBackend);
        QVERIFY2(controller.timelineScroll()->scrollTraceEnabled(),
                 "the diag counters are gated on this — without it the "
                 "snapshot below can only ever print zeros");
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(60);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,2);
        // A production-sized batch (20 rows): the default 3-row mock page
        // stays inside cacheBuffer (800) and the test would not discriminate.
        QList<TimelineEvent> chunk;
        for (int i = 0; i < 20; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral(
                "older backfilled message %1 — this body is deliberately "
                "long so the row wraps to several lines and the prepended "
                "batch displaces the reader's anchor well beyond the "
                "ListView cache buffer, reproducing the real top-edge "
                "geometry rather than a compact synthetic one").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            chunk.append(e);
        }
        mock->setPaginationChunkForTest(chunk);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY2(waitForRowsToStopArriving(controller),
                 "the proxy never finished revealing, so the anchor would be "
                 "captured on whichever row happened to be there");
        QString anchorId;
        QTRY_VERIFY2_WITH_TIMEOUT(
            ([&] {
                if (!positionAtTopEdge(timeline))
                    return false;
                QCoreApplication::processEvents();
                if (!QMetaObject::invokeMethod(timeline, "captureViewAnchor"))
                    return false;
                anchorId = timeline->property("viewAnchorId").toString();
                const int row = controller.timeline()->rowForStableId(anchorId);
                return !anchorId.isEmpty() && row >= 0
                    && row != controller.timeline()->rowCount() - 1;
            }()),
            "the anchor at the top edge stayed on the newest row, whose y "
            "is 0: the layout had not settled when it was captured",
            kSignalTimeoutMs);
        QVERIFY2(!anchorId.isEmpty(), "the fixture must yield a live anchor");

        const int rowBefore = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(rowBefore >= 0);
        QQuickItem *itemBefore = nullptr;
        QVERIFY(((itemBefore = itemForSourceRow(timeline, rowBefore)) != nullptr));
        QVERIFY(itemBefore != nullptr);
        const double offsetBefore =
            itemBefore->y() - timeline->property("contentY").toDouble();

        auto *settleTimer = timeline->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        QVERIFY(settleTimer != nullptr);
        GestureHold gesture(settleTimer);
        QVERIFY(timeline->property("userScrollActive").toBool());
        QVERIFY2(!timeline->property("moving").toBool(),
                 "this is the self-driven path, not a native drag");

        const double heightBefore =
            timeline->property("contentHeight").toDouble();
        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        controller.pagination()->requestNearTop();
        QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
        QVERIFY2(completedSpy.constFirst().at(0).toInt() > 0,
                 "fixture assumption: the near-top page must insert rows");

        const double cacheBufferPx = 800.0;
        double heightGrowth = 0;
        QTRY_VERIFY_WITH_TIMEOUT(
            (heightGrowth = timeline->property("contentHeight").toDouble()
                            - heightBefore,
             heightGrowth > cacheBufferPx + timeline->height()),
            kSignalTimeoutMs);
        QVERIFY2(heightGrowth > cacheBufferPx + timeline->height(),
                 qPrintable(QStringLiteral(
                     "fixture no longer displaces the anchor past the cache "
                     "buffer (growth %1 <= %2) — this test would pass on "
                     "broken code")
                     .arg(heightGrowth).arg(cacheBufferPx + timeline->height())));

        const int rowAfter = controller.timeline()->rowForStableId(anchorId);
        QVERIFY2(rowAfter > rowBefore,
                 "fixture assumption: the prepend must shift the row index");
        const AnchorSettle settle =
            anchorOffsetOnceItsRowExists(timeline, rowAfter, offsetBefore,
                                         anchorId);
        QVERIFY2(!settle.moved(offsetBefore),
                 qPrintable(QStringLiteral(
                     "a top-edge prepend moved the reader off their row "
                     "mid-gesture (the teleport cascade): %1")
                     .arg(settle.detail(offsetBefore))));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void nearTopControllerDrivenBatchesCompensateImmediatelyNotChained()
    {
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
    }
    struct TraceGuard { ~TraceGuard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } traceGuard;

        AppController controller(AppController::MockBackend);
        QVERIFY2(controller.timelineScroll()->scrollTraceEnabled(),
                 "the diag counters are gated on this — without it the "
                 "snapshot below can only ever print zeros");
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(60);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        constexpr int kBatches = 3;
        mock->resetTimelineForTest(roomId, events,
2 + kBatches
                                       + PaginationController::
                                             kMaxNearTopEmptyStrikes);
        QList<TimelineEvent> chunk;
        for (int i = 0; i < 20; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral(
                "older backfilled message %1 — deliberately long so the row "
                "wraps to several lines and each prepended batch displaces "
                "the reader's anchor well beyond the ListView cache buffer, "
                "reproducing the real top-edge geometry of a sustained "
                "near-top loading run rather than a compact synthetic one")
                .arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            chunk.append(e);
        }
        mock->setPaginationChunkForTest(chunk);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY2(waitForRowsToStopArriving(controller),
                 "the proxy never finished revealing, so the anchor would be "
                 "captured on whichever row happened to be there");
        QString anchorId;
        QTRY_VERIFY2_WITH_TIMEOUT(
            ([&] {
                if (!positionAtTopEdge(timeline))
                    return false;
                QCoreApplication::processEvents();
                if (!QMetaObject::invokeMethod(timeline, "captureViewAnchor"))
                    return false;
                anchorId = timeline->property("viewAnchorId").toString();
                const int row = controller.timeline()->rowForStableId(anchorId);
                return !anchorId.isEmpty() && row >= 0
                    && row != controller.timeline()->rowCount() - 1;
            }()),
            "the anchor at the top edge stayed on the newest row, whose y "
            "is 0: the layout had not settled when it was captured",
            kSignalTimeoutMs);
        QVERIFY2(!anchorId.isEmpty(), "the fixture must yield a live anchor");

        int rowBefore = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(rowBefore >= 0);
        QQuickItem *itemBefore = nullptr;
        QVERIFY(((itemBefore = itemForSourceRow(timeline, rowBefore)) != nullptr));
        QVERIFY(itemBefore != nullptr);
        double offsetBefore =
            itemBefore->y() - timeline->property("contentY").toDouble();

        auto *settleTimer = timeline->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        QVERIFY(settleTimer != nullptr);
        GestureHold gesture(settleTimer);
        QVERIFY(timeline->property("userScrollActive").toBool());
        QVERIFY2(!timeline->property("moving").toBool(),
                 "this is the self-driven path, not a native drag");
        QVERIFY2(timeline->property("nearTopArmed").toBool(),
                 "this test exercises the nearTopRunActive branch alone — "
                 "nearTopArmed must stay un-consumed throughout");

        const double cacheBufferPx = 800.0;
        for (int batch = 0; batch < kBatches; ++batch) {
            const double heightBeforeBatch =
                timeline->property("contentHeight").toDouble();
            QSignalSpy completedSpy(controller.pagination(),
                                   &PaginationController::paginationCompleted);
            controller.pagination()->requestNearTop();
            QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
            QVERIFY2(completedSpy.constFirst().at(0).toInt() > 0,
                     "fixture assumption: each near-top page must insert rows");
            QVERIFY2(!controller.pagination()->nearTopRunActive(),
                     "a productive batch must end the run immediately, not "
                     "leave a continuation scheduled");

            double heightGrowth = 0;
            QTRY_VERIFY_WITH_TIMEOUT(
                (heightGrowth = timeline->property("contentHeight").toDouble()
                                - heightBeforeBatch,
                 heightGrowth > cacheBufferPx + timeline->height()),
                kSignalTimeoutMs);
            QVERIFY2(heightGrowth > cacheBufferPx + timeline->height(),
                     qPrintable(QStringLiteral(
                         "batch %1: fixture no longer displaces the anchor "
                         "past the cache buffer (growth %2 <= %3) — this "
                         "test would pass on broken code")
                         .arg(batch).arg(heightGrowth)
                         .arg(cacheBufferPx + timeline->height())));

            const int rowAfter = controller.timeline()->rowForStableId(anchorId);
            QVERIFY2(rowAfter > rowBefore,
                     "fixture assumption: the prepend must shift the row index");

            const AnchorSettle settle =
                anchorOffsetOnceItsRowExists(timeline, rowAfter, offsetBefore,
                                         anchorId);
            QVERIFY2(!settle.moved(offsetBefore),
                     qPrintable(QStringLiteral(
                         "batch %1 of %2: a near-top prepend during a held "
                         "gesture moved the reader off their row: %3")
                         .arg(batch + 1).arg(kBatches)
                         .arg(settle.detail(offsetBefore))));
            const double offsetAfter = settle.offset;

            rowBefore = rowAfter;
            offsetBefore = offsetAfter;
        }
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void viewportFillRunCompensatesEveryBatchImmediately()
    {
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
    }
    struct TraceGuard { ~TraceGuard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } traceGuard;

        AppController controller(AppController::MockBackend);
        QVERIFY2(controller.timelineScroll()->scrollTraceEnabled(),
                 "the diag counters are gated on this — without it the "
                 "snapshot below can only ever print zeros");
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(60);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        constexpr int kBatches = 3;
        mock->resetTimelineForTest(roomId, events,
1 + kBatches);
        QList<TimelineEvent> chunk;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral(
                "older backfilled message %1 — deliberately long so the row "
                "wraps to several lines and each prepended batch displaces "
                "the reader's anchor well beyond the ListView cache buffer, "
                "reproducing the real top-edge geometry of a sustained "
                "near-top loading run rather than a compact synthetic one")
                .arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            chunk.append(e);
        }
        mock->setPaginationChunkForTest(chunk);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(QMetaObject::invokeMethod(timeline, "releasePendingRows"));
        QCoreApplication::processEvents();
        QVERIFY2(waitForRowsToStopArriving(controller),
                 "the proxy never finished revealing, so the anchor would be "
                 "captured on whichever row happened to be there");
        QVERIFY(positionAtSourceRow(timeline, 8));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY2(!anchorId.isEmpty(), "the fixture must yield a live anchor");
        QVERIFY2(!timeline->property("atYBeginning").toBool(),
                 "fixture assumption: positioned with headroom, not at the "
                 "exact top edge");

        int rowBefore = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(rowBefore >= 0);
        QQuickItem *itemBefore = nullptr;
        QVERIFY(((itemBefore = itemForSourceRow(timeline, rowBefore)) != nullptr));
        QVERIFY(itemBefore != nullptr);
        double offsetBefore =
            itemBefore->y() - timeline->property("contentY").toDouble();

        auto *settleTimer = timeline->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        QVERIFY(settleTimer != nullptr);
        GestureHold gesture(settleTimer);
        QVERIFY(timeline->property("userScrollActive").toBool());
        QVERIFY2(!timeline->property("moving").toBool(),
                 "this is the self-driven path, not a native drag");
        QVERIFY2(timeline->property("nearTopArmed").toBool(),
                 "never touched by this test — a ViewportFill dispatch "
                 "must not consume it");

        const double cacheBufferPx = 800.0;
        for (int batch = 0; batch < kBatches; ++batch) {
            const double heightBeforeBatch =
                timeline->property("contentHeight").toDouble();
            QSignalSpy completedSpy(controller.pagination(),
                                   &PaginationController::paginationCompleted);
            controller.pagination()->requestViewportFill();
            QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
            QVERIFY2(completedSpy.constFirst().at(0).toInt() > 0,
                     "fixture assumption: each fill page must insert rows");

            double heightGrowth = 0;
            QTRY_VERIFY_WITH_TIMEOUT(
                (heightGrowth = timeline->property("contentHeight").toDouble()
                                - heightBeforeBatch,
                 heightGrowth > cacheBufferPx + timeline->height()),
                kSignalTimeoutMs);
            QVERIFY2(heightGrowth > cacheBufferPx + timeline->height(),
                     qPrintable(QStringLiteral(
                         "batch %1: fixture no longer displaces the anchor "
                         "past the cache buffer (growth %2 <= %3) — this "
                         "test would pass on broken code")
                         .arg(batch).arg(heightGrowth)
                         .arg(cacheBufferPx + timeline->height())));

            const int rowAfter = controller.timeline()->rowForStableId(anchorId);
            QVERIFY2(rowAfter > rowBefore,
                     "fixture assumption: the prepend must shift the row "
                     "index");

            const AnchorSettle settle =
                anchorOffsetOnceItsRowExists(timeline, rowAfter, offsetBefore,
                                         anchorId);
            QVERIFY2(!settle.moved(offsetBefore),
                     qPrintable(QStringLiteral(
                         "batch %1 of %2: a prepend during a held gesture "
                         "moved the reader off their row before the next "
                         "batch was even requested: %3")
                         .arg(batch + 1).arg(kBatches)
                         .arg(settle.detail(offsetBefore))));
            const double offsetAfter = settle.offset;

            rowBefore = rowAfter;
            offsetBefore = offsetAfter;
        }
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void scrollBarHandleHeldAtTheTopKeepsLoadingHistory()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);

        QList<TimelineEvent> events;
        for (int i = 0; i < 40; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(90 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,8);
        QList<TimelineEvent> chunk;
        for (int i = 0; i < 20; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral("older backfilled message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            chunk.append(e);
        }
        mock->setPaginationChunkForTest(chunk);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        auto *bar = root->findChild<QQuickItem *>(
            QStringLiteral("timelineScrollBar"));
        QVERIFY(timeline != nullptr);
        QVERIFY(bar != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 40,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible(), kSignalTimeoutMs);

        const double size = bar->property("size").toDouble();
        const double position = bar->property("position").toDouble();
        const QPointF handleLocal(bar->width() / 2,
                                  (position + size / 2) * bar->height());
        const QPoint grab = bar->mapToScene(handleLocal).toPoint();
        const QPoint top = bar->mapToScene(QPointF(bar->width() / 2, 1)).toPoint();
        const int before = timeline->property("count").toInt();
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, grab);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(&window,
                             grab + (top - grab) * i / 10);
        QTest::qWait(50);
        QVERIFY2(bar->property("pressed").toBool(), "the handle was not grabbed");
        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("count").toInt() >= before + 60,
            qPrintable(QStringLiteral("only %1 of 60 rows loaded while the "
                                      "handle was held at the top")
                           .arg(timeline->property("count").toInt() - before)),
            12000);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, top);
    }

    void nearTopProximityIsMeasuredFromLoadedHistoryNotAbsoluteContentY()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,6);
        QList<TimelineEvent> chunk;
        for (int i = 0; i < 20; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral(
                "older backfilled message %1 — deliberately long enough to "
                "wrap to several lines so the prepended batch moves originY by "
                "far more than one near-top band width").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            chunk.append(e);
        }
        mock->setPaginationChunkForTest(chunk);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        const auto topmostY = [timeline] {
            const double maxY = timeline->property("originY").toDouble()
                + timeline->property("contentHeight").toDouble()
                + timeline->property("bottomMargin").toDouble()
                - timeline->height();
            const double minY = timeline->property("originY").toDouble()
                - timeline->property("topMargin").toDouble();
            return maxY < minY ? minY : maxY;
        };
        const auto bandWidth = [timeline] {
            return timeline->property("nearTopEnterDistance").toDouble();
        };
        const auto settledTopmostY = [&timeline, &topmostY] {
            double stable = topmostY();
            for (int attempt = 0; attempt < 30; ++attempt) {
                QCoreApplication::processEvents();
                const double next = topmostY();
                if (qAbs(next - stable) < 0.5)
                    return next;
                stable = next;
            }
            return stable;
        };
        const auto setContentYAtDistanceFromTop =
            [timeline, &settledTopmostY](double distance) {
            const double target = settledTopmostY() - distance;
            timeline->setProperty("contentY", target);
            QCoreApplication::processEvents();
        };

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtTopEdge(timeline));
        QCoreApplication::processEvents();
        const double topBefore = topmostY();

        QVERIFY(timeline->setProperty("nearTopArmed", true));
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        QVERIFY2(!timeline->property("nearTopArmed").toBool(),
                 "an approach to the top must consume the latch");

        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        controller.pagination()->requestNearTop(true);
        QTRY_VERIFY_WITH_TIMEOUT(!completedSpy.isEmpty(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(
            topmostY() - timeline->property("contentY").toDouble()
                > bandWidth(),
            kSignalTimeoutMs);
        QVERIFY2(waitForRowsToStopArriving(controller),
                 "the proxy never finished revealing, so the probes below "
                 "would be measured against a timeline still growing");

        const double contentY = timeline->property("contentY").toDouble();
        const double fromTop = topmostY() - contentY;
        qInfo("top-edge batch geometry: topmostY %g -> %g, contentY %g, "
              "distance %g, band %g",
              topBefore, topmostY(), contentY, fromTop, bandWidth());

        QVERIFY2(fromTop > bandWidth(),
                 qPrintable(QStringLiteral(
                     "a landed batch left the reader classified 'near the top' "
                     "(distance %1 <= band %2) — every further gesture would "
                     "re-arm and buy four more batches")
                     .arg(fromTop).arg(bandWidth())));
        QVariant reported;
        QVERIFY(QMetaObject::invokeMethod(timeline, "distanceFromTop",
                                          Q_RETURN_ARG(QVariant, reported)));
        const double reportedDistance = reported.toDouble();
        QVERIFY2(qAbs(reportedDistance - fromTop) < 1.0,
                 qPrintable(QStringLiteral(
                     "distanceFromTop() (%1) must agree with Qt's own geometry "
                     "(%2)").arg(reportedDistance).arg(fromTop)));

        mock->setPaginationDelayForTest(60000);
        QVERIFY2(!controller.pagination()->reachedStart(),
                 "premise: backfill must still be available");
        QVERIFY(positionAtTopEdge(timeline));
        setContentYAtDistanceFromTop(0.0);
        const double probeBase = 120;
        const double probeStep = 40;
        QVERIFY2(probeBase + probeStep < bandWidth(),
                 qPrintable(QStringLiteral(
                     "fixture: every probe must sit inside the band (max %1 vs "
                     "band %2)").arg(probeBase + probeStep).arg(bandWidth())));
        setContentYAtDistanceFromTop(probeBase);

        auto *settleTimer = timeline->findChild<QObject *>(
            QStringLiteral("scrollSettleTimer"));
        QVERIFY(settleTimer != nullptr);
        const auto notFollowingBottom = [timeline] {
            return !timeline->property("stickToBottom").toBool();
        };
        QVERIFY2(notFollowingBottom(),
                 "premise: these probes require stickToBottom == false");
        const auto armAndCheck = [timeline] {
            timeline->setProperty("nearTopArmed", true);
            QCoreApplication::processEvents();
            QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                      Q_ARG(QVariant, QVariant(true)));
            return !timeline->property("nearTopArmed").toBool();
        };

        QVERIFY(timeline->setProperty("nearTopRequestDistance",
                                      std::numeric_limits<double>::infinity()));
        QVERIFY2(armAndCheck(), "a fresh approach must dispatch");
        QVERIFY2(qAbs(timeline->property("nearTopRequestDistance").toDouble()
                     - probeBase) < 1.0,
                 qPrintable(QStringLiteral(
                     "recorded dispatch distance %1 does not match the driven "
                     "probe %2")
                     .arg(timeline->property("nearTopRequestDistance")
                              .toDouble())
                     .arg(probeBase)));

        setContentYAtDistanceFromTop(probeBase + probeStep);
        QVERIFY(QMetaObject::invokeMethod(settleTimer, "restart"));
        QTRY_VERIFY_WITH_TIMEOUT(!settleTimer->property("running").toBool(),
                                 kSignalTimeoutMs);
        QVERIFY2(timeline->property("nearTopArmed").toBool(),
                 "the settle re-arm is deliberately unconditional in the band");
        QVERIFY2(notFollowingBottom(),
                 "the settle re-pinned follow-latest; the refusal check below "
                 "would pass vacuously");
        QVERIFY2(!armAndCheck(),
                 "a DOWNWARD sample inside the band consumed the latch and "
                 "fetched a page — history loads while scrolling down");

        setContentYAtDistanceFromTop(probeBase - probeStep);
        QVERIFY2(armAndCheck(), "continued upward progress must still dispatch");
        QVERIFY2(qAbs(timeline->property("nearTopRequestDistance").toDouble()
                     - (probeBase - probeStep)) < 1.0,
                 qPrintable(QStringLiteral(
                     "recorded dispatch distance %1 does not match the driven "
                     "probe %2")
                     .arg(timeline->property("nearTopRequestDistance")
                              .toDouble())
                     .arg(probeBase - probeStep)));

        setContentYAtDistanceFromTop(0.0);
        QVariant atTopDistance;
        QVERIFY(QMetaObject::invokeMethod(timeline, "distanceFromTop",
                                          Q_RETURN_ARG(QVariant, atTopDistance)));
        QVERIFY2(atTopDistance.toDouble() <= 1.0,
                 qPrintable(QStringLiteral(
                     "premise: the probe must be pinned at the top (distance %1)")
                     .arg(atTopDistance.toDouble())));
        QVERIFY(timeline->setProperty("nearTopRequestDistance", 0.0));
        timeline->setProperty("contentY", topmostY());
        QVERIFY(timeline->setProperty("nearTopArmed", true));
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        QVERIFY2(!timeline->property("nearTopArmed").toBool(),
                 "a reader pinned at the exact top could not dispatch — history "
                 "is unreachable at the one place they most want it");

        const double entry = 200;
        const double deep = 40;
        const double backOff = 45;
        QVERIFY2(entry < bandWidth(),
                 "fixture: the entry probe must sit inside the band");
        QVERIFY(timeline->setProperty("nearTopRequestDistance",
                                      std::numeric_limits<double>::infinity()));
        setContentYAtDistanceFromTop(entry);
        QVERIFY2(armAndCheck(), "band entry must dispatch");

        setContentYAtDistanceFromTop(deep);
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        const double ratcheted =
            timeline->property("nearTopRequestDistance").toDouble();
        QVERIFY2(qAbs(ratcheted - deep) < 1.0,
                 qPrintable(QStringLiteral(
                     "the baseline did not ratchet to the closest approach "
                     "(%1, expected %2) — the region between the top and the "
                     "last dispatch stays unpaid, so a later downward sample "
                     "fetches").arg(ratcheted).arg(deep)));

        setContentYAtDistanceFromTop(backOff);
        QVERIFY2(!armAndCheck(),
                 "a downward sample inside the region already traversed "
                 "consumed the latch — 'scroll up near the top, then scroll "
                 "down a little' still loads history");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void oneApproachToTheTopDoesNotPaginateToTheStartOfTheRoom()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);

        QList<TimelineEvent> events;
        for (int i = 0; i < 60; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral(
                "history message %1 - deliberately long enough to wrap over "
                "several lines so the loaded room is much taller than the "
                "near-top exit band").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,6);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 60,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        const auto wheelMaxY = [timeline] {
            QVariant v;
            QMetaObject::invokeMethod(timeline, "wheelMaxY",
                                      Q_RETURN_ARG(QVariant, v));
            return v.toDouble();
        };
        const auto wheelMinY = [timeline] {
            QVariant v;
            QMetaObject::invokeMethod(timeline, "wheelMinY",
                                      Q_RETURN_ARG(QVariant, v));
            return v.toDouble();
        };

        const double exitBand =
            timeline->property("nearTopExitDistance").toDouble();
        QVERIFY2(wheelMaxY() - wheelMinY() > exitBand + 100.0,
                 qPrintable(QStringLiteral(
                     "fixture too short: scroll range %1 cannot express a "
                     "departure past the %2px exit band, so the re-arm half "
                     "of this test would assert nothing")
                     .arg(wheelMaxY() - wheelMinY()).arg(exitBand)));

        const int budget =
            timeline->property("nearTopApproachRowBudget").toInt();
        QVERIFY2(budget > 0, "the approach row budget must be a real bound");

        QVERIFY(timeline->setProperty("nearTopCheckScheduled", true));
        QVERIFY(timeline->setProperty("stickToBottom", false));

        int dispatches = 0;
        const int kPages = 40;
        const int kRowsPerPage = 20;
        for (int page = 0; page < kPages; ++page) {
            QVERIFY(positionAtTopEdge(timeline));
            QCoreApplication::processEvents();
            QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                              Q_ARG(QVariant, QVariant(true))));
            if (!timeline->property("nearTopArmed").toBool())
                ++dispatches;
            emit controller.pagination()->paginationCompleted(
                kRowsPerPage,false,false);
            QCoreApplication::processEvents();
        }

        QVERIFY2(dispatches > 0,
                 "the fixture never dispatched at all - it is not exercising "
                 "the near-top path and cannot fail on the old code either");
        const int allowedPages = budget / kRowsPerPage + 1;
        QVERIFY2(dispatches <= allowedPages,
                 qPrintable(QStringLiteral(
                     "one uninterrupted approach to the top dispatched %1 "
                     "pages (~%2 rows) against a budget of %3 rows - the "
                     "chain runs to the start of the room, which is the "
                     "reported 'it loads media slow and jumps' behaviour")
                     .arg(dispatches)
                     .arg(dispatches * kRowsPerPage)
                     .arg(budget)));
        QVERIFY2(timeline->property("nearTopRowsThisApproach").toInt() >= budget,
                 "the approach did not actually spend its budget, so the "
                 "bound above was not what stopped the chain");

        timeline->setProperty("contentY", wheelMaxY() - (exitBand + 60.0));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        QCOMPARE(timeline->property("nearTopRowsThisApproach").toInt(), 0);
        QVERIFY2(timeline->property("nearTopArmed").toBool(),
                 "leaving the exit band must re-arm the latch as before");

        QVERIFY(positionAtTopEdge(timeline));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(true))));
        QVERIFY2(!timeline->property("nearTopArmed").toBool(),
                 "a NEW approach after a real departure must dispatch again - "
                 "the budget bounds the automatic chain, it does not stop a "
                 "reader who keeps scrolling into history");

        QVERIFY(timeline->setProperty("stickToBottom", true));
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(false))));
        QCOMPARE(timeline->property("nearTopRowsThisApproach").toInt(), 0);

        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void aFilteredHistoryRunStillFillsTheViewport()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(1);

        QList<TimelineEvent> seed;
        {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("the one message that did load");
            e.timestamp = QDateTime::currentDateTimeUtc().addSecs(-60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            seed.append(e);
        }

        constexpr int kFilteredPages = 20;
        QList<TimelineEvent> realChunk;
        for (int i = 0; i < 25; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@carol:mock.local");
            e.senderDisplayName = QStringLiteral("Carol");
            e.body = QStringLiteral(
                "a real message from beyond the call-churn run, long enough "
                "that a page or two of them is taller than the viewport %1")
                .arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 + i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            realChunk.append(e);
        }
        mock->setPaginationChunkForTest(realChunk);
        mock->setFilteredPaginationPagesForTest(kFilteredPages);
        mock->resetTimelineForTest(roomId, seed,
kFilteredPages + 6);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);

        const qreal viewportHeight = timeline->property("height").toReal();
        QVERIFY2(viewportHeight > 0, "the fixture never laid the pane out");

        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentHeight").toReal() >= viewportHeight,
            20000);

        const qreal filled = timeline->property("contentHeight").toReal();
        QVERIFY2(filled >= viewportHeight,
                 qPrintable(QStringLiteral(
                     "the room opened with one message and %1 px of content "
                     "under a %2 px viewport, and the automatic fill stopped "
                     "there — the reader is left scrolling by hand through a "
                     "run of filtered history, which is the report")
                     .arg(filled).arg(viewportHeight)));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void anEndlessFilteredRunStillStopsTheAutomaticFill()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(1);

        QList<TimelineEvent> seed;
        {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("the one message that did load");
            e.timestamp = QDateTime::currentDateTimeUtc().addSecs(-60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            seed.append(e);
        }
        mock->setFilteredPaginationPagesForTest(5000);
        mock->resetTimelineForTest(roomId, seed,5000);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);

        QTRY_VERIFY_WITH_TIMEOUT(controller.pagination()->fillStopped(), 30000);
        QVERIFY2(controller.pagination()->fillStopped(),
                 "an unending filtered run must still stop the automatic fill");
    }

    void anApproachThatKeepsGettingEmptyPagesStillStops()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,6);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        const int requestBudget =
            timeline->property("nearTopApproachRequestBudget").toInt();
        QVERIFY2(requestBudget > 0, "the request budget must be a real bound");

        QVERIFY(timeline->setProperty("nearTopCheckScheduled", true));
        QVERIFY(timeline->setProperty("stickToBottom", false));

        int dispatches = 0;
        for (int page = 0; page < 40; ++page) {
            QVERIFY(positionAtTopEdge(timeline));
            QCoreApplication::processEvents();
            QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                              Q_ARG(QVariant, QVariant(true))));
            if (!timeline->property("nearTopArmed").toBool())
                ++dispatches;
            QVERIFY(timeline->setProperty("nearTopArmed", true));
            QCoreApplication::processEvents();
        }

        QVERIFY2(dispatches > 0, "the fixture never dispatched");
        QVERIFY2(dispatches <= requestBudget,
                 qPrintable(QStringLiteral(
                     "one approach issued %1 near-top requests against a "
                     "budget of %2, every one of them returning no rows — the "
                     "row budget cannot bound an empty page")
                     .arg(dispatches).arg(requestBudget)));

        QVERIFY(timeline->setProperty("stickToBottom", true));
        QVERIFY(QMetaObject::invokeMethod(timeline, "checkNearTopEdge",
                                          Q_ARG(QVariant, QVariant(false))));
        QCOMPARE(timeline->property("nearTopRequestsThisApproach").toInt(), 0);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void consecutivePaginationBatchesEachCompensateWithoutDoubleCounting()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        mock->setPaginationDelayForTest(30);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,3);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(!timeline->property("anchorStableId").isValid());

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int rowBefore = controller.timeline()->rowForStableId(anchorId);
        QQuickItem *itemBefore = nullptr;
        QVERIFY(((itemBefore = itemForSourceRow(timeline, rowBefore)) != nullptr));
        QVERIFY(itemBefore != nullptr);
        const double yBefore = itemBefore->y();
        const double contentYBefore = timeline->property("contentY").toDouble();

        QSignalSpy completedSpy(controller.pagination(),
                               &PaginationController::paginationCompleted);
        controller.pagination()->requestNearTop();
        QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, kSignalTimeoutMs);
        QCoreApplication::processEvents();
        controller.pagination()->requestNearTop();
        QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 2, kSignalTimeoutMs);
        QCoreApplication::processEvents();

        const int rowAfter = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(rowAfter >= 0);
        QQuickItem *itemAfter = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (((itemAfter = itemForSourceRow(timeline, rowAfter)) != nullptr),
             itemAfter != nullptr),
            kSignalTimeoutMs);
        const double expected = contentYBefore + (itemAfter->y() - yBefore);
        double actual = 0;
        QTRY_VERIFY_WITH_TIMEOUT(
            (actual = timeline->property("contentY").toDouble(),
             qAbs(actual - expected) < 1.0),
            kSignalTimeoutMs);
        QVERIFY2(qAbs(actual - expected) < 1.0,
                 qPrintable(QStringLiteral(
                     "two consecutive prepends were not each compensated "
                     "exactly once: actual=%1 expected=%2")
                     .arg(actual).arg(expected)));
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void maintainViewAnchorDefersGrowthDeltaMidGestureWithoutGlide()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();

        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(((anchorItem = itemForSourceRow(timeline, anchorRow)) != nullptr));
        QVERIFY(anchorItem != nullptr);
        const double realItemY = anchorItem->y();

        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QVERIFY2(!controller.timelineScroll()->motionActive(),
                 "the pixel path must never engage the wheel engine");

        constexpr double simulatedGrowth = 270.0;
        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realItemY - simulatedGrowth));
        const double beforeY = timeline->property("contentY").toDouble();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        const double afterY = timeline->property("contentY").toDouble();
        QVERIFY2(qAbs(afterY - beforeY) < 1.0,
                 qPrintable(QStringLiteral(
                     "growth above the anchor was written into contentY "
                     "during an active gesture, which the deliberate "
                     "no-write design forbids: before=%1 after=%2")
                     .arg(beforeY).arg(afterY)));
        QVERIFY2(!timeline->property("viewAnchorId").toString().isEmpty(),
                 "the branch must leave a resolved anchor behind, not a "
                 "stale/empty one, even though it performs no write");
        QVERIFY2(!controller.timelineScroll()->motionActive(),
                 "no glide was active — nothing should have been engaged");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void maintainViewAnchorTranslatesActiveGlideWhenGrowthLandsMidFlight()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 5));
        QCoreApplication::processEvents();

        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(((anchorItem = itemForSourceRow(timeline, anchorRow)) != nullptr));
        QVERIFY(anchorItem != nullptr);
        const double realItemY = anchorItem->y();

        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);
        const double glideStartY = timeline->property("contentY").toDouble();
        double minY = 0, maxY = 0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        for (int notch = 0; notch < 6; ++notch)
            scroll->wheelNotch(120.0,
                               timeline->property("contentY").toDouble(),
                               minY, maxY, timeline->height());
        QTRY_VERIFY_WITH_TIMEOUT(scroll->motionActive(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("contentY").toDouble() < glideStartY - 10.0,
            2000);
        QVERIFY(scroll->motionActive());

        const double targetBefore = scroll->targetYForTest();
        const double positionBefore = scroll->positionYForTest();
        const double remainingBefore = targetBefore - positionBefore;

        constexpr double simulatedGrowth = 300.0;
        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realItemY - simulatedGrowth));
        const double beforeY = timeline->property("contentY").toDouble();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QVERIFY2(scroll->motionActive(),
                 "an in-flight glide must survive a growth correction");
        const double afterY = timeline->property("contentY").toDouble();
        QVERIFY2(qAbs(afterY - beforeY) < 1.0,
                 qPrintable(QStringLiteral(
                     "contentY moved during an active glide, which the "
                     "deliberate no-write design forbids: before=%1 "
                     "after=%2")
                     .arg(beforeY).arg(afterY)));
        QVERIFY2(qAbs(scroll->targetYForTest() - targetBefore) < 1.0,
                 "the glide's coalesced target moved even though nothing "
                 "should have written to it");
        QVERIFY2(qAbs((scroll->targetYForTest() - scroll->positionYForTest())
                     - remainingBefore) < 1.0,
                 "growth correction changed the glide's remaining distance");
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void growthDeltaIsDeferredWhileFlickableOwnsTheDrag()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,1);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();

        QVERIFY(!controller.pagination()->busy());

        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(((anchorItem = itemForSourceRow(timeline, anchorRow)) != nullptr));
        QVERIFY(anchorItem != nullptr);
        const double realItemY = anchorItem->y();

        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier,
                          QPoint(360, 400));
        for (int step = 1; step <= 8; ++step) {
            QTest::mouseMove(&window, QPoint(360, 400 + step * 14));
            QCoreApplication::processEvents();
        }
        QVERIFY2(timeline->property("moving").toBool(),
                 "a native drag must set Flickable.moving");
        QVERIFY2(timeline->property("userScrollActive").toBool(),
                 "a drag is still a user scroll session");
        QVERIFY2(!timeline->property("selfDrivenScrollActive").toBool(),
                 "a drag is NOT a self-driven (Lightning-owned) scroll");

        constexpr double simulatedGrowth = 250.0;
        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realItemY - simulatedGrowth));
        const double beforeY = timeline->property("contentY").toDouble();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        const double afterY = timeline->property("contentY").toDouble();
        const double baselineAfter =
            timeline->property("viewAnchorLastY").toDouble();
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                            QPoint(360, 512));

        Q_UNUSED(baselineAfter);
        QCOMPARE(afterY, beforeY);
        QVERIFY2(!timeline->property("viewAnchorId").toString().isEmpty(),
                 "the drag path must leave a resolved anchor behind, not a "
                 "stale/empty one");
    }

    void scrollTraceDisabledAddsNoDiagnosticCost()
    {
        qunsetenv("LIGHTNING_SCROLL_TRACE");

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(!controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);
        QCOMPARE(timeline->property("scrollTrace").toBool(), false);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));

        LogCapture capture;

        const QPointF pos(320, 300);
        for (int attempt = 0; attempt < 50; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            if (timeline->property("userScrollActive").toBool())
                break;
            QTest::qWait(10);
        }
        QVERIFY(timeline->setProperty(
            "viewAnchorLastY",
            timeline->property("viewAnchorLastY").toDouble() - 270.0));
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QCOMPARE(timeline->property("diagActive").toBool(), false);
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagDisplacedAppliedSum").toDouble(), 0.0);
        QCOMPARE(timeline->property("diagDisplacedMaxAbsGrew").toDouble(), 0.0);
        QCOMPARE(timeline->property("diagDisplacedMaxAbsGrewRows").toInt(), 0);
        QCOMPARE(timeline->property(
                     "diagDisplacedMaxAbsGrewOriginShift").toDouble(),
                 0.0);
        QCOMPARE(timeline->property(
                     "diagDisplacedMaxAbsOriginShift").toDouble(),
                 0.0);
        QCOMPARE(timeline->property(
                     "diagDisplacedMaxAbsOriginShiftContentDelta").toDouble(),
                 0.0);
        QCOMPARE(timeline->property(
                     "diagDisplacedMaxAbsOriginShiftRows").toInt(),
                 0);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagMaterializedMaxAbsDelta").toDouble(), 0.0);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(), 0);
        QCOMPARE(timeline->property("diagPrependFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagPrependOriginShiftSum").toDouble(), 0.0);
        QCOMPARE(timeline->property("diagPrependMaxAbsOriginShift").toDouble(),
                 0.0);
        QCOMPARE(timeline->property("diagPrependMaxAbsOriginShiftRows").toInt(),
                 0);
        QCOMPARE(timeline->property(
                     "diagPrependMaxAbsOriginShiftContentDelta").toDouble(),
                 0.0);
        QCOMPARE(timeline->property(
                     "diagPrependMaxAbsOriginShiftPath").toString(),
                 QStringLiteral("none"));

        for (const QString &m : capture.messages()) {
            QVERIFY2(!m.contains(QStringLiteral("scroll-gesture")),
                     qPrintable(QStringLiteral(
                         "a scroll-gesture line was emitted with tracing "
                         "off: %1").arg(m)));
        }
    }

    void scrollTraceLineIncludesAllPerBranchFields()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QCoreApplication::processEvents();

        LogCapture capture;

        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");

        QTRY_VERIFY_WITH_TIMEOUT(
            !timeline->property("userScrollActive").toBool(), 3000);

        QString line;
        for (const QString &m : capture.messages()) {
            if (m.contains(QStringLiteral("scroll-gesture"))) {
                line = m;
                break;
            }
        }
        QVERIFY2(!line.isEmpty(), "no scroll-gesture line was emitted at settle");

        const QStringList expectedFields = {
            QStringLiteral("events="),
            QStringLiteral("pixel="),
            QStringLiteral("angle="),
            QStringLiteral("netY="),
            QStringLiteral("dContentH="),
            QStringLiteral("originY="),
            QStringLiteral("dOriginY="),
            QStringLiteral("noAnchorReturns="),
            QStringLiteral("stickToBottomReturns="),
            QStringLiteral("anchorCorrections="),
            QStringLiteral("growthCorrections="),
            QStringLiteral("displacedFirings="),
            QStringLiteral("displacedApplied="),
            QStringLiteral("displacedMaxAbsGrew="),
            QStringLiteral("displacedMaxAbsGrewRows="),
            QStringLiteral("displacedMaxAbsGrewOriginShift="),
            QStringLiteral("displacedMaxAbsOriginShift="),
            QStringLiteral("displacedMaxAbsOriginShiftDContentH="),
            QStringLiteral("displacedMaxAbsOriginShiftRows="),
            QStringLiteral("materializedFirings="),
            QStringLiteral("materializedMaxAbsDelta="),
            QStringLiteral("unresolvedId="),
            QStringLiteral("evictedNoInsert="),
            QStringLiteral("dragDeferrals="),
            QStringLiteral("prependFirings="),
            QStringLiteral("prependOriginShift="),
            QStringLiteral("prependMaxAbsOriginShift="),
            QStringLiteral("prependMaxAbsOriginShiftRows="),
            QStringLiteral("prependMaxAbsOriginShiftDContentH="),
            QStringLiteral("prependMaxAbsOriginShiftPath="),
            QStringLiteral("stick="),
            QStringLiteral("topDist="),
            QStringLiteral("nearTop="),
        };
        for (const QString &field : expectedFields) {
            QVERIFY2(line.contains(field),
                     qPrintable(QStringLiteral(
                         "scroll-gesture line is missing field %1: %2")
                         .arg(field, line)));
        }
    }

    void diagMaterializedCountersTrackTheSelfDrivenGrowthBranch()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,1);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();

        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(((anchorItem = itemForSourceRow(timeline, anchorRow)) != nullptr));
        QVERIFY(anchorItem != nullptr);
        const double realAnchorY = anchorItem->y() + anchorItem->height();

        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QVERIFY(timeline->property("diagActive").toBool());
        const int baseFirings =
            timeline->property("diagMaterializedFirings").toInt();
        const double baseApplied =
            timeline->property("diagMaterializedAppliedSum").toDouble();
        if (baseFirings != 0)
            qWarning("diag baseline not quiescent: firings=%d applied=%f "
                     "(load-timing; see comment)",
                     baseFirings, baseApplied);

        constexpr double simulatedGrowth = 270.0;
        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realAnchorY - simulatedGrowth));

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(),
                 baseFirings + 1);
        QCOMPARE(timeline->property("diagMaterializedAppliedSum").toDouble(),
                 baseApplied);
        const double maxAbs =
            qAbs(timeline->property("diagMaterializedMaxAbsDelta").toDouble());
        if (baseFirings == 0) {
            QVERIFY2(qAbs(maxAbs - simulatedGrowth) < 1.0,
                     "diagMaterializedMaxAbsDelta did not record the exact "
                     "driven magnitude");
        } else {
            QVERIFY2(maxAbs >= simulatedGrowth - 1.0,
                     "diagMaterializedMaxAbsDelta lost the driven magnitude");
        }
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(), 0);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void diagDragDeferralsCountsNativeDragEngagements()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,1);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY(!anchorId.isEmpty());
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);
        QQuickItem *anchorItem = nullptr;
        QVERIFY(((anchorItem = itemForSourceRow(timeline, anchorRow)) != nullptr));
        QVERIFY(anchorItem != nullptr);
        const double realItemY = anchorItem->y();

        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QVERIFY(timeline->property("diagActive").toBool());

        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier,
                          QPoint(360, 400));
        for (int step = 1; step <= 8; ++step) {
            QTest::mouseMove(&window, QPoint(360, 400 + step * 14));
            QCoreApplication::processEvents();
        }
        QVERIFY2(timeline->property("moving").toBool(),
                 "a native drag must set Flickable.moving");
        QVERIFY2(!timeline->property("selfDrivenScrollActive").toBool(),
                 "a drag is NOT a self-driven (Lightning-owned) scroll");

        const int baseFirings =
            timeline->property("diagMaterializedFirings").toInt();
        const int baseDragDeferrals =
            timeline->property("diagDragDeferrals").toInt();

        QVERIFY(timeline->setProperty("viewAnchorLastY",
                                      realItemY - 250.0));
        const double beforeY = timeline->property("contentY").toDouble();
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        const double afterY = timeline->property("contentY").toDouble();

        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                            QPoint(360, 512));

        QCOMPARE(afterY, beforeY);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(),
                 baseDragDeferrals + 1);
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(),
                 baseFirings + 1);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);
    }

    void diagUnresolvedIdFallbackCountsGenuinelyUnresolvableAnchor()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 30; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("history message %1").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(60 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,1);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 30,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();

        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);

        QVERIFY(timeline->setProperty(
            "viewAnchorId", QStringLiteral("$this-event-id-does-not-exist")));

        const int baseEvicted =
            timeline->property("diagEvictedNoInsertFallbacks").toInt();
        const int baseDisplaced =
            timeline->property("diagDisplacedFirings").toInt();
        const int baseMaterialized =
            timeline->property("diagMaterializedFirings").toInt();
        const int baseDragDeferrals =
            timeline->property("diagDragDeferrals").toInt();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 1);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(),
                 baseEvicted);
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(),
                 baseDisplaced);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(),
                 baseMaterialized);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(),
                 baseDragDeferrals);
    }

    void anchorDelegateSurvivesDistantScrollNeverEvictedFallback()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 60; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral(
                "message %1 — long enough to wrap across several lines so "
                "sixty rows vastly exceed any plausible delegate cache, the "
                "geometry that USED to evict the anchor delegate").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,1);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 60,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY2(!anchorId.isEmpty(), "the fixture must yield a live anchor");
        const int anchorRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(anchorRow >= 0);

        QVERIFY(positionAtSourceRow(timeline, 50));
        QCoreApplication::processEvents();
        QVERIFY2(itemForSourceRow(timeline, anchorRow) != nullptr,
                 "the un-virtualized Column must keep every loaded row's "
                 "delegate alive — eviction has been reintroduced");
        QVERIFY2(!timeline->property("moving").toBool(),
                 "programmatic positioning must not leave Flickable.moving");
        QVERIFY(timeline->setProperty("viewAnchorId", anchorId));

        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QVERIFY(timeline->setProperty("viewAnchorId", anchorId));
        QVERIFY2(itemForSourceRow(timeline, anchorRow) != nullptr,
                 "the anchor's delegate must be alive for this invariant");
        const int baseEvicted =
            timeline->property("diagEvictedNoInsertFallbacks").toInt();
        const int baseUnresolved =
            timeline->property("diagUnresolvedIdFallbacks").toInt();
        const int baseMaterialized =
            timeline->property("diagMaterializedFirings").toInt();
        const int baseDisplaced =
            timeline->property("diagDisplacedFirings").toInt();

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(),
                 baseEvicted);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(),
                 baseUnresolved);
        QVERIFY2(timeline->property("diagMaterializedFirings").toInt()
                     > baseMaterialized,
                 "the driven call must take the materialized branch");
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(),
                 baseDisplaced);
    }

    void displacedBranchDoesNotFireWhileAnchorDelegateAlive()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        const QString roomId = QStringLiteral("!general:mock.local");
        controller.setCurrentRoomId(roomId);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QList<TimelineEvent> events;
        for (int i = 0; i < 60; ++i) {
            TimelineEvent e;
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral(
                "message %1 — long enough to wrap so the geometry matches "
                "the displaced fixture this test inverts").arg(i);
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-(600 - i) * 60);
            e.type = TimelineEvent::TextMessage;
            e.status = TimelineEvent::Sent;
            events.append(e);
        }
        mock->resetTimelineForTest(roomId, events,1);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(760, 620);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();

        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 60,
                                 kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(),
                                 kSignalTimeoutMs);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(positionAtSourceRow(timeline, 15));
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        const QString anchorId = timeline->property("viewAnchorId").toString();
        QVERIFY2(!anchorId.isEmpty(), "the fixture must yield a live anchor");
        const int sourceRow = controller.timeline()->rowForStableId(anchorId);
        QVERIFY(sourceRow >= 0);
        QVariant viewRowOut;
        QVERIFY(QMetaObject::invokeMethod(
            timeline, "viewRowForStableId", Q_RETURN_ARG(QVariant, viewRowOut),
            Q_ARG(QVariant, anchorId)));
        const int anchorViewRow = viewRowOut.toInt();
        QVERIFY(anchorViewRow >= 0);
        QVERIFY2(itemForSourceRow(timeline, sourceRow) != nullptr,
                 "the anchor's delegate must be alive for this invariant");
        QVERIFY(!timeline->property("moving").toBool());
        QVERIFY(!timeline->property("userScrollActive").toBool());

        const int baseDisplaced =
            timeline->property("diagDisplacedFirings").toInt();
        const int basePrepend =
            timeline->property("diagPrependFirings").toInt();

        constexpr double injectedContentDelta = -3582.0;
        constexpr int injectedInsertedRows = 3;
        const double contentHeightNow =
            timeline->property("contentHeight").toDouble();
        QVERIFY(timeline->setProperty("viewAnchorRow",
                                      anchorViewRow - injectedInsertedRows));
        QVERIFY(timeline->setProperty("viewAnchorContentHeight",
                                      contentHeightNow
                                          - injectedContentDelta));

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));

        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(),
                 baseDisplaced);
        QCOMPARE(timeline->property("diagPrependFirings").toInt(),
                 basePrepend + 1);
        QCOMPARE(timeline->property(
                     "diagPrependMaxAbsOriginShiftPath").toString(),
                 QStringLiteral("idle"));
        QVERIFY(QMetaObject::invokeMethod(
            timeline, "viewRowForStableId", Q_RETURN_ARG(QVariant, viewRowOut),
            Q_ARG(QVariant, anchorId)));
        QCOMPARE(timeline->property("viewAnchorRow").toInt(),
                 viewRowOut.toInt());
    }

    void diagNoAnchorReturnsCountsTheEarlyReturn()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", true));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 1);

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 2);

        QCOMPARE(timeline->property("diagStickToBottomReturns").toInt(), 0);
        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagUnresolvedIdFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagEvictedNoInsertFallbacks").toInt(), 0);
        QCOMPARE(timeline->property("diagDragDeferrals").toInt(), 0);
        QCOMPARE(timeline->property("diagAnchorCorrections").toInt(), 0);
    }

    void diagStickToBottomReturnsCountsTheDroppedCorrection()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(QMetaObject::invokeMethod(timeline, "captureViewAnchor"));
        QVERIFY(!timeline->property("viewAnchorId").toString().isEmpty());
        QVERIFY(timeline->setProperty("stickToBottom", true));

        QCOMPARE(timeline->property("diagStickToBottomReturns").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagStickToBottomReturns").toInt(), 1);
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);

        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagStickToBottomReturns").toInt(), 2);
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);

        QCOMPARE(timeline->property("diagDisplacedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagMaterializedFirings").toInt(), 0);
        QCOMPARE(timeline->property("diagAnchorCorrections").toInt(), 0);
    }

    void diagOutcomeCountersSurviveAcrossAFlushBoundary()
    {
        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        struct Guard { ~Guard() { qunsetenv("LIGHTNING_SCROLL_TRACE"); } } guard;

        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QVERIFY(controller.timelineScroll()->scrollTraceEnabled());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        auto *timeline = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(timeline != nullptr);

        QQuickWindow window;
        window.resize(640, 480);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QVERIFY(timeline->setProperty("stickToBottom", true));
        QCOMPARE(timeline->property("diagActive").toBool(), false);
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 2);

        const QPointF pos(320, 300);
        bool opened = false;
        for (int attempt = 0; attempt < 50 && !opened; ++attempt) {
            QWheelEvent wheel(pos, window.mapToGlobal(pos.toPoint()),
                              QPoint(0, 24), QPoint(0, 0), Qt::NoButton,
                              Qt::NoModifier, Qt::ScrollUpdate,
false);
            QCoreApplication::sendEvent(&window, &wheel);
            QCoreApplication::processEvents();
            opened = timeline->property("userScrollActive").toBool();
            if (!opened)
                QTest::qWait(10);
        }
        QVERIFY2(opened, "a touchpad delta must open the scroll session");
        QCOMPARE(timeline->property("diagActive").toBool(), true);
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 2);

        LogCapture capture;
        QVERIFY(QMetaObject::invokeMethod(timeline, "diagFlushGesture"));

        QString line;
        for (const QString &m : capture.messages()) {
            if (m.contains(QStringLiteral("scroll-gesture"))) {
                line = m;
                break;
            }
        }
        QVERIFY2(!line.isEmpty(), "no scroll-gesture line was emitted");
        QVERIFY2(line.contains(QStringLiteral("noAnchorReturns=2")),
                 qPrintable(QStringLiteral(
                     "the pre-gesture outcome count was not carried into "
                     "the flushed line: %1").arg(line)));

        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(timeline, "maintainViewAnchor"));
        QCOMPARE(timeline->property("diagNoAnchorReturns").toInt(), 1);
        QCOMPARE(realWarnings(warnings), QStringList{});
    }

    void wheelMotionIntoHistoryAndBackReachesTheTrueLiveEdge()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);
        QQuickItem *root = paneRootOf(timeline);
        QVERIFY(root != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        const int skip = timeline->property("rowWindowSkip").toInt();
        QVERIFY2(skip > 0,
                 "no window was established, so the synthetic newest edge "
                 "this test is about does not exist here");

        const QPointF pos(window.width() / 2.0, window.height() / 2.0);
        for (int i = 0; i < 6000; ++i) {
            sendWheelNotch(window, pos, -120,false);
            if (timeline->property("stickToBottom").toBool())
                break;
            if (i % 10 == 9)
                QTest::qWait(16);
        }

        QCOMPARE(timeline->property("rowWindowSkip").toInt(), 0);
        QVERIFY2(timeline->property("diagWindowNewEndExtensions").toInt() > 0,
                 "the live edge was reached, but no newest-end extension ever "
                 "ran — the motion path is not what restored it");
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("stickToBottom").toBool(), 3000);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(pill != nullptr);
        QVERIFY2(!pill->isVisible(),
                 "the reader is at the true live edge and the jump pill is "
                 "still demanding a press");
    }

    void rowWindowWithASmallSkipStillClosesToTheLiveEdge()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);

        QQmlExpression setWindow(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ app.timelineView.setWindow(12, 330);"
                           " contentY = wheelMinY();"
                           " updateVisibleRowRange(); return true; })()"));
        QVERIFY(setWindow.evaluate().toBool());
        QVERIFY2(!setWindow.hasError(),
                 setWindow.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("rowWindowSkip").toInt(), 12);
        const int rows = timeline->property("count").toInt();
        QVERIFY2(qAbs(rows - 320) < 40,
                 qPrintable(QStringLiteral(
                     "fixture exposed %1 rows; the row term of the hysteresis "
                     "must also be under 40 or this proves nothing")
                                .arg(rows)));

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        QCOMPARE(timeline->property("rowWindowSkip").toInt(), 0);
    }

    void clampedNoOpWheelEventLeavesFollowLatestEngaged()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 3, QStringLiteral("short"),
                        QStringLiteral("tiny")),
0,600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        double minY = 0.0;
        double maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY2(qFuzzyCompare(minY + 1.0, maxY + 1.0),
                 qPrintable(QStringLiteral(
                     "fixture is scrollable (%1..%2); the clamped no-op this "
                     "test is about cannot occur").arg(minY).arg(maxY)));
        QVERIFY(timeline->setProperty("stickToBottom", true));

        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(pill != nullptr);

        const QPointF pos(window.width() / 2.0, window.height() / 2.0);
        for (bool inverted : { false, true }) {
            QVERIFY(timeline->setProperty("stickToBottom", true));
            sendWheelNotch(window, pos, 120, inverted);
            QVERIFY2(timeline->property("stickToBottom").toBool(),
                     inverted ? "an unappliable inverted wheel event "
                                "disengaged follow-latest"
                              : "an unappliable wheel event disengaged "
                                "follow-latest");
            QVERIFY(!pill->isVisible());
        }

        for (bool inverted : { false, true }) {
            QVERIFY(timeline->setProperty("stickToBottom", true));
            sendWheelNotch(window, pos, -120, inverted);
            QVERIFY(timeline->property("stickToBottom").toBool());
            QVERIFY(!pill->isVisible());
        }
    }

    void turningSmoothScrollingOffKeepsTheWheelDirection()
    {
        double moved[2] = { 0.0, 0.0 };
        const bool wanted[2] = { true, false };
        bool previous = true;
        for (int i = 0; i < 2; ++i) {
            AppController controller(AppController::MockBackend);
            QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
            const QString roomId = QStringLiteral("!general:mock.local");
            QQmlApplicationEngine engine;
            QQuickWindow window;
            QQuickItem *timeline = nullptr;
            QQuickItem *root = paneWithEvents(
                controller, engine, window, roomId,
                textFixture(roomId, 60, QStringLiteral("line"),
                            QStringLiteral("body")),
0,600, &timeline);
            QVERIFY(root != nullptr);
            QVERIFY(timeline != nullptr);
            if (i == 0)
                previous = controller.settings()->smoothScrolling();
            controller.settings()->setSmoothScrolling(wanted[i]);

            double minY = 0.0;
            double maxY = 0.0;
            QVERIFY(wheelBounds(timeline, &minY, &maxY));
            QVERIFY2(maxY > minY + 1.0, "fixture is not scrollable");

            const double start = (minY + maxY) / 2.0;
            QVERIFY(timeline->setProperty("contentY", start));
            QVERIFY(timeline->setProperty("stickToBottom", false));
            QTest::qWait(60);

            const QPointF pos(window.width() / 2.0, window.height() / 2.0);
            sendWheelNotch(window, pos, 120,false);
            QTest::qWait(400);
            moved[i] = timeline->property("contentY").toDouble() - start;

            controller.settings()->setSmoothScrolling(previous);
        }

        for (int i = 0; i < 2; ++i) {
            QVERIFY2(qAbs(moved[i]) > 1.0,
                     qPrintable(QStringLiteral("smooth=%1 moved nothing")
                                    .arg(wanted[i])));
        }
        QCOMPARE(moved[1] > 0 ? 1 : -1, moved[0] > 0 ? 1 : -1);
    }

    void thePillLandsTheReaderOnTheFirstUnreadMessage()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;

        QList<TimelineEvent> events =
            textFixture(roomId, 60, QStringLiteral("u"), QStringLiteral("body"));
        TimelineEvent marker;
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        events.insert(10, marker);

        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events,0,
600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        QCOMPARE(controller.timeline()->readMarkerRow(), 10);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        QVERIFY2(pill != nullptr, "the pill does not exist");
        QVERIFY2(pill->isVisible(),
                 "the marker is loaded and the pill is still hidden");

        QVERIFY(timeline->setProperty("stickToBottom", true));
        QMetaObject::invokeMethod(timeline, "goToLatest");
        QTest::qWait(120);
        const double before = timeline->property("contentY").toDouble();

        QMetaObject::invokeMethod(pill, "click");
        QVERIFY2(QTest::qWaitFor([&] {
                     return timeline->property("navigationPendingRow").toInt() < 0
                         && timeline->property("contentY").toDouble() > before + 1.0;
                 }, 4000),
                 "the pill did not move the reader toward the marker");

        QVERIFY(!timeline->property("stickToBottom").toBool());
        QCOMPARE(timeline->property("diagNavigationUnresolved").toInt(), 0);
        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("firstUnreadOnScreen").toBool(),
            qPrintable(QStringLiteral(
                "the pill landed with the divider off screen (marker view row "
                "%1, viewport rows %2..%3)")
                .arg(timeline->property("firstUnreadViewRow").toInt())
                .arg(timeline->property("viewportNewestRow").toInt())
                .arg(timeline->property("viewportOldestRow").toInt())),
            2000);
        QTRY_VERIFY_WITH_TIMEOUT(!pill->isVisible(), 2000);
    }

    void thePillIsOfferedWhenTheMarkerIsAboveTheLoadedRows()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QVERIFY2(controller.roomList()->findRoom(roomId)
                         .value(QStringLiteral("unreadCount")).toInt() > 0,
                 "the fixture room opens with unread messages");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 20, QStringLiteral("d"), QStringLiteral("body")),
3,600, &timeline);
        QVERIFY(root != nullptr);
        QCOMPARE(controller.timeline()->readMarkerRow(), -1);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        QVERIFY(pill != nullptr);
        QTRY_VERIFY2_WITH_TIMEOUT(pill->isVisible(),
                                  "a marker above the loaded rows offers no jump",
                                  2000);

        QMetaObject::invokeMethod(pill, "click");
        QTRY_VERIFY2_WITH_TIMEOUT(!pill->isVisible(),
                                  "the bounded hunt found nothing and the pill "
                                  "still promises a jump", 8000);
        QCOMPARE(controller.timeline()->readMarkerRow(), -1);
    }

    void aFailedFirstUnreadHuntSaysSo()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 20, QStringLiteral("n"), QStringLiteral("body")),
3,600, &timeline);
        QVERIFY(root != nullptr);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        auto *note = root->findChild<QQuickItem *>(
            QStringLiteral("firstUnreadFailedNote"));
        QVERIFY2(note != nullptr, "the failed-hunt note does not exist");
        QVERIFY(pill != nullptr);
        QVERIFY(!note->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(pill->isVisible(), 2000);
        QMetaObject::invokeMethod(pill, "click");
        QTRY_VERIFY2_WITH_TIMEOUT(note->isVisible(), "the pill vanished and nothing told the reader why", 8000);
        QVERIFY(!pill->isVisible());
        if (controller.pagination()->reachedStart()) {
            const QPointF left = note->parentItem()->mapToItem(
                root, QPointF(note->x(), 0));
            const qreal centre = root->width() / 2;
            QVERIFY2(left.x() > centre || left.x() + note->width() < centre,
                     "the failed-hunt note sits over the start label");
        }
    }

    void theFirstUnreadPillLeavesTheStartLabelAloneAtTheRoomStart()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QList<TimelineEvent> events =
            textFixture(roomId, 60, QStringLiteral("s"), QStringLiteral("body"));
        TimelineEvent marker;
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        events.insert(10, marker);
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events,1,
600, &timeline);
        QVERIFY(root != nullptr);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        QVERIFY(pill != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(pill->isVisible(), 2000);
        for (int i = 0; i < 4 && !controller.pagination()->reachedStart(); ++i) {
            controller.pagination()->requestNearTop();
            QTRY_VERIFY_WITH_TIMEOUT(!controller.pagination()->busy(), 5000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(controller.pagination()->reachedStart(), 5000);
        const QPointF pillMid = pill->parentItem()->mapToItem(
            root, QPointF(pill->x() + pill->width() / 2, 0));
        const qreal centre = root->width() / 2;
        QVERIFY2(qAbs(pillMid.x() - centre) > pill->width() / 2 + 40,
                 qPrintable(QStringLiteral("pill centre %1 is over the start "
                                           "label's centre %2")
                                .arg(pillMid.x()).arg(centre)));
    }

    void aReadMarkerThatGoesAwayTakesThePillWithIt()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QVERIFY2(controller.roomList()->findRoom(roomId)
                         .value(QStringLiteral("unreadCount")).toInt() > 0,
                 "the fixture room opens with unread messages");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;

        const QList<TimelineEvent> plain =
            textFixture(roomId, 60, QStringLiteral("g"), QStringLiteral("body"));
        QList<TimelineEvent> withMarker = plain;
        TimelineEvent marker;
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        withMarker.insert(5, marker);

        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          withMarker,3,
600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(controller.timeline()->readMarkerRow() >= 0);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        QVERIFY(pill != nullptr);
        QTRY_VERIFY2_WITH_TIMEOUT(pill->isVisible(),
                                  "the marker is loaded and the pill is hidden",
                                  2000);

        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->resetTimelineForTest(roomId, plain,3);
        QTRY_COMPARE_WITH_TIMEOUT(controller.timeline()->readMarkerRow(), -1,
                                  3000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.timeline()->eventCount() > 0,
                                 3000);
        QVERIFY2(!controller.pagination()->reachedStart(),
                 "the fixture reached the start, so it cannot tell the two "
                 "reasons for hiding the pill apart");

        QTRY_VERIFY2_WITH_TIMEOUT(!pill->isVisible(),
                                  "the marker went away and the pill still "
                                  "offers a jump to it", 2000);
        QTest::qWait(200);
        QVERIFY(!pill->isVisible());
    }

    void theDeepMarkerPillWithdrawsWhenTheFullyReadMarkerIsWritten()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QVERIFY2(controller.roomList()->findRoom(roomId)
                         .value(QStringLiteral("unreadCount")).toInt() > 0,
                 "the fixture room opens with unread messages");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 20, QStringLiteral("w"), QStringLiteral("body")),
3,600, &timeline);
        QVERIFY(root != nullptr);
        QCOMPARE(controller.timeline()->readMarkerRow(), -1);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        QVERIFY(pill != nullptr);
        QTRY_VERIFY2_WITH_TIMEOUT(pill->isVisible(),
                                  "a marker above the loaded rows offers no jump",
                                  2000);
        ReadReceiptCoordinator *receipts = controller.readReceipts();
        QVERIFY(receipts != nullptr);

        QVERIFY(QMetaObject::invokeMethod(
            receipts, "fullyReadMarkerMoved",
            Q_ARG(QString, QStringLiteral("!elsewhere:mock.local")),
            Q_ARG(QString, QStringLiteral("$x"))));
        QTest::qWait(50);
        QVERIFY(pill->isVisible());

        QVERIFY(QMetaObject::invokeMethod(
            receipts, "fullyReadMarkerMoved", Q_ARG(QString, roomId),
            Q_ARG(QString, QStringLiteral("$w-newest"))));
        QVERIFY2(!controller.pagination()->reachedStart(),
                 "the fixture reached the start, so it cannot tell the two "
                 "reasons for hiding the pill apart");
        QTRY_VERIFY2_WITH_TIMEOUT(!pill->isVisible(),
                                  "m.fully_read was written past everything "
                                  "and the pill still offers a jump", 2000);
    }

    void noReadMarkerMeansNoPill()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 20, QStringLiteral("q"), QStringLiteral("body")),
0,600, &timeline);
        QVERIFY(root != nullptr);
        QCOMPARE(controller.timeline()->readMarkerRow(), -1);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        QVERIFY(pill != nullptr);
        QVERIFY2(!pill->isVisible(),
                 qPrintable(QStringLiteral(
                     "the pill is offered with nothing to jump to "
                     "(openedWithUnread %1, moreHistory %2, reachedStart %3)")
                     .arg(root->property("openedWithUnread").toBool())
                     .arg(controller.pagination()->moreHistory())
                     .arg(controller.pagination()->reachedStart())));
    }

    void theFindBarSearchesTheLocalIndex()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;

        QList<TimelineEvent> events =
            textFixture(roomId, 12, QStringLiteral("s"), QStringLiteral("body"));
        events[3].body = QStringLiteral("the zephyrine protocol landed today");
        events[7].body = QStringLiteral("nothing to do with that word");
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events,0,
700, &timeline);
        QVERIFY(root != nullptr);

        auto *search = controller.messageSearch();
        QVERIFY(search != nullptr);
        QVERIFY2(search->localAvailable(),
                 "the backend reports no local index, so the bar cannot be "
                 "exercised at all");

        QMetaObject::invokeMethod(root, "openFind");
        QTest::qWait(60);
        auto *toggle = root->findChild<QQuickItem *>(
            QStringLiteral("findModeToggle"));
        QVERIFY2(toggle != nullptr, "the History segment is gone");
        QVERIFY2(toggle->isVisible(),
                 "History is not offered even though a local index exists");
        QMetaObject::invokeMethod(toggle, "activated",
                                  Q_ARG(QVariant, QStringLiteral("history")));
        QTest::qWait(60);
        QCOMPARE(root->property("findHistoryMode").toBool(), true);
        QCOMPARE(search->source(), QStringLiteral("local"));

        auto *field = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindField"));
        QVERIFY(field != nullptr);
        field->setProperty("text", QStringLiteral("zephyrine"));
        QVERIFY2(QTest::qWaitFor([&] {
                     return search->state() == QLatin1String("results");
                 }, 4000),
                 qPrintable(QStringLiteral("search state stuck at %1")
                                .arg(search->state())));
        QCOMPARE(search->rowCount({}), 1);
        QCOMPARE(search->rowAt(0).value(QStringLiteral("body")).toString(),
                 QStringLiteral("the zephyrine protocol landed today"));

        auto *coverage = root->findChild<QQuickItem *>(
            QStringLiteral("findLocalCoverage"));
        QVERIFY2(coverage != nullptr, "the coverage line is gone");
        QVERIFY(coverage->isVisible());

        field->setProperty("text", QStringLiteral("ze"));
        QVERIFY2(QTest::qWaitFor([&] {
                     return search->state() == QLatin1String("too_short");
                 }, 4000),
                 qPrintable(QStringLiteral("short query reported %1")
                                .arg(search->state())));
        QVERIFY(search->minLocalChars() >= 3);

        field->setProperty("text", QStringLiteral("borogoves"));
        QVERIFY(QTest::qWaitFor([&] {
            return search->state() == QLatin1String("no_results");
        }, 4000));
    }

    void theSearchPanelHasASourceToggle()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QList<TimelineEvent> events =
            textFixture(roomId, 12, QStringLiteral("s"), QStringLiteral("body"));
        events[3].body = QStringLiteral("the zephyrine protocol landed today");
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events,0,
700, &timeline);
        QVERIFY(root != nullptr);
        auto *search = controller.messageSearch();
        QVERIFY(search->localAvailable());

        search->setSource(QStringLiteral("server"));
        QCOMPARE(search->source(), QStringLiteral("server"));

        root->setProperty("searchOpen", true);
        QTest::qWait(80);
        QCOMPARE(search->source(), QStringLiteral("local"));

        auto *hint = root->findChild<QQuickItem *>(
            QStringLiteral("roomSearchHint"));
        QVERIFY2(hint != nullptr, "the panel has no source hint");
        const QString localHint = hint->property("text").toString();
        QVERIFY2(localHint.contains(QStringLiteral("own index")),
                 qPrintable(localHint));

        auto *toggle = root->findChild<QQuickItem *>(
            QStringLiteral("roomSearchSourceToggle"));
        QVERIFY2(toggle != nullptr, "the panel has no Indexed/Server toggle");

        search->setQuery(QStringLiteral("zephyrine"));
        QVERIFY2(QTest::qWaitFor([&] {
                     return search->state() == QLatin1String("results");
                 }, 4000),
                 qPrintable(search->state()));

        QSignalSpy states(search, &MessageSearchController::stateChanged);
        QMetaObject::invokeMethod(toggle, "activated",
                                  Q_ARG(QVariant, QStringLiteral("server")));
        QTest::qWait(60);
        QCOMPARE(search->source(), QStringLiteral("server"));
        QVERIFY2(states.count() > 0,
                 "switching source did not re-run the query");
        const QString serverHint = hint->property("text").toString();
        QVERIFY2(serverHint.contains(QStringLiteral("homeserver"))
                     && serverHint != localHint,
                 qPrintable(serverHint));

        QMetaObject::invokeMethod(toggle, "activated",
                                  Q_ARG(QVariant, QStringLiteral("local")));
        QTest::qWait(60);
        QCOMPARE(search->source(), QStringLiteral("local"));
        QCOMPARE(hint->property("text").toString(), localHint);
    }

    void indexingThisRoomReachesTheBackend()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 6, QStringLiteral("d"), QStringLiteral("body")),
0,700, &timeline);
        QVERIFY(root != nullptr);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QMetaObject::invokeMethod(root, "openFind");
        QTest::qWait(50);
        auto *toggle = root->findChild<QQuickItem *>(
            QStringLiteral("findModeToggle"));
        QVERIFY(toggle != nullptr);
        QMetaObject::invokeMethod(toggle, "activated",
                                  Q_ARG(QVariant, QStringLiteral("history")));
        QTest::qWait(50);

        auto *button = root->findChild<QQuickItem *>(
            QStringLiteral("findIndexRoomButton"));
        QVERIFY2(button != nullptr, "the index-this-room action is gone");
        QVERIFY(button->isVisible());
        const int before = mock->deepIndexedRooms.size();
        QMetaObject::invokeMethod(button, "click");
        QVERIFY2(QTest::qWaitFor([&] {
                     return mock->deepIndexedRooms.size() > before;
                 }, 3000),
                 "pressing Index this room asked the backend for nothing");
        QCOMPARE(mock->deepIndexedRooms.last(), roomId);
    }

    void freshShortRoomBecomesScrollableWithoutAResize()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.settings()->setShowRoomActivity(false);
        controller.pagination()->setMaxViewportFillRequests(24);

        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");

        QList<TimelineEvent> hidden;
        for (int i = 0; i < 3; ++i) {
            TimelineEvent e;
            e.roomId = roomId;
            e.type = TimelineEvent::StateChange;
            e.stateKind = QStringLiteral("membership");
            e.body = QStringLiteral("membership churn");
            e.timestamp =
                QDateTime::currentDateTimeUtc().addSecs(-9000 - i * 60);
            hidden.append(e);
        }
        mock->setPaginationChunkForTest(hidden);

        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 2, QStringLiteral("seed"),
                        QStringLiteral("seed"),7200),
24,420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        QTRY_VERIFY_WITH_TIMEOUT(controller.timeline()->rowCount() > 2, 4000);
        QVERIFY2(timeline->property("contentHeight").toReal()
                     < timeline->property("height").toReal(),
                 "the hidden page rendered after all; the deadlock this test "
                 "is about does not exist here");

        mock->setPaginationChunkForTest(textFixture(
            roomId, 4, QStringLiteral("older"),
            QStringLiteral("a deliberately long older message body so that "
                           "each page adds several wrapped lines of real "
                           "rendered height to the column"),
20000));

        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                double lo = 0.0;
                double hi = 0.0;
                return wheelBounds(timeline, &lo, &hi) && hi > lo;
            }(), 8000);
    }

    void rowsTheFillLandsAfterOpenAreInsideTheMediaBandWithoutAGesture()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");

        mock->setPaginationDelayForTest(5000);
        mock->setPaginationChunkForTest(textFixture(
            roomId, 150, QStringLiteral("older"),
            QStringLiteral("older history"),20000));

        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 2, QStringLiteral("seed"),
                        QStringLiteral("seed"),7200),
1,420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                const int count = timeline->property("count").toInt();
                return count > 0
                    && timeline->property("mediaBandLastRow").toInt()
                           == count - 1;
            }(), kSignalTimeoutMs);
        QVERIFY2(controller.pagination()->busy(),
                 "premise: the history page landed before the band settled");
        QVERIFY2(timeline->property("contentHeight").toReal()
                     < timeline->property("height").toReal(),
                 "premise: the seed rows must not fill the viewport");
        const int seededRows = timeline->property("count").toInt();

        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("count").toInt() > seededRows + 50, 15000);
        QVERIFY(waitForRowsToStopArriving(controller));
        QVERIFY2(timeline->property("stickToBottom").toBool(),
                 "premise: the reader never left the live edge");
        QVERIFY2(lastViewRowOnScreen(timeline) >= seededRows,
                 "premise: rows from the page must be on screen");

        QTRY_VERIFY2_WITH_TIMEOUT(
            onScreenRowsOutsideTheMediaBand(timeline).isEmpty(),
            qPrintable(onScreenRowsOutsideTheMediaBand(timeline)),
            kSignalTimeoutMs);

        const double far = timeline->property("contentY").toDouble()
            + 4.0 * qMax(timeline->property("height").toDouble(), 400.0);
        const int beyond = firstViewRowBeyond(timeline, far);
        QVERIFY2(beyond >= 0, "premise: the page must reach past four viewports");
        QVERIFY2(!itemAtViewRow(timeline, beyond)
                      ->property("mediaInBand").toBool(),
                 qPrintable(QStringLiteral(
                     "row %1, four viewports out, is inside the band [0..%2]: "
                     "the band no longer closes")
                     .arg(beyond)
                     .arg(timeline->property("mediaBandLastRow").toInt())));
    }

    void aBandWhoseOldestRowsAreHiddenStaysOpenForTheHistoryFill()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const bool activityBefore = controller.settings()->showRoomActivity();
        const auto restore = qScopeGuard([&] {
            controller.settings()->setShowRoomActivity(activityBefore);
        });
        controller.settings()->setShowRoomActivity(false);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QString roomId = QStringLiteral("!general:mock.local");

        QList<TimelineEvent> seed;
        for (int i = 0; i < 3; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$bandhidden%1").arg(i);
            e.itemId = QStringLiteral("uid-bandhidden%1").arg(i);
            e.roomId = roomId;
            e.type = TimelineEvent::StateChange;
            e.stateKind = QStringLiteral("membership");
            e.sender = QStringLiteral("@bob:mock.local");
            e.senderDisplayName = QStringLiteral("Bob");
            e.body = QStringLiteral("membership churn");
            e.timestamp = QDateTime::currentDateTimeUtc().addSecs(-9000 + i * 30);
            seed.append(e);
        }
        seed.append(textFixture(roomId, 2, QStringLiteral("bandseed"),
                                QStringLiteral("seed"),7200));

        QList<TimelineEvent> page =
            textFixture(roomId, 6, QStringLiteral("bandolder"),
                        QStringLiteral("older history"),30000);
        TimelineEvent image;
        image.eventId = QStringLiteral("$bandimage");
        image.itemId = QStringLiteral("uid-bandimage");
        image.roomId = roomId;
        image.type = TimelineEvent::Image;
        image.sender = QStringLiteral("@alice:mock.local");
        image.senderDisplayName = QStringLiteral("Alice");
        image.body = QStringLiteral("picture.png");
        image.mediaFilename = QStringLiteral("picture.png");
        image.mediaMimetype = QStringLiteral("image/png");
        image.mediaWidth = 320;
        image.mediaHeight = 200;
        image.mediaKey = QStringLiteral("mxc-band-image");
        image.timestamp = QDateTime::currentDateTimeUtc().addSecs(-25000);
        page.append(image);
        page.append(textFixture(roomId, 3, QStringLiteral("bandnewer"),
                                QStringLiteral("older history"),
24000));
        mock->setPaginationDelayForTest(5000);
        mock->setPaginationChunkForTest(page);

        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId, seed,
1,420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        QTRY_COMPARE_WITH_TIMEOUT(timeline->property("count").toInt(), 5,
                                  kSignalTimeoutMs);
        for (int row = 2; row < 5; ++row) {
            QQuickItem *item = itemAtViewRow(timeline, row);
            QVERIFY(item != nullptr);
            QVERIFY2(!item->isVisible() || item->height() <= 0,
                     "premise: the oldest rows must be hidden");
        }
        QVERIFY2(timeline->property("contentHeight").toReal()
                     < timeline->property("height").toReal(),
                 "premise: the seed must not fill the viewport");
        QVERIFY2(controller.pagination()->busy(),
                 "premise: the history page landed before the band settled");
        QTest::qWait(400);
        const bool openBefore = timeline->property("mediaBandOpen").toBool();

        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() >= 15,
                                 15000);
        QVERIFY(waitForRowsToStopArriving(controller));
        QQuickItem *imageRow = nullptr;
        const int count = timeline->property("count").toInt();
        for (int row = 0; row < count && !imageRow; ++row) {
            QQuickItem *item = itemAtViewRow(timeline, row);
            if (!item)
                continue;
            QQmlExpression isImage(qmlContext(item), item,
                                   QStringLiteral("model.isImage === true"));
            if (isImage.evaluate().toBool())
                imageRow = item;
        }
        QVERIFY2(imageRow != nullptr, "the picture row was never built");
        QTRY_VERIFY2_WITH_TIMEOUT(
            imageRow->property("mediaInBand").toBool(),
            qPrintable(QStringLiteral("the picture is outside the band "
                                      "[0..%1] without a gesture (band read "
                                      "%2 on the short content)")
                           .arg(timeline->property("mediaBandLastRow").toInt())
                           .arg(openBefore ? "open" : "closed")),
            kSignalTimeoutMs);
    }

    void rowHoverIsSuspendedWhileTheViewportMovesAndReturnsWhenItStops()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 60, QStringLiteral("hov"),
                        QStringLiteral("hover suspension probe")),
0,420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        auto *scroll = controller.timelineScroll();
        QVERIFY(scroll != nullptr);

        QString actionKey;
        QQuickItem *messageRow = onScreenMessageRow(timeline, &actionKey);
        QVERIFY2(messageRow != nullptr, "no message row on screen");
        auto *content = messageRow->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        QVERIFY(content != nullptr);
        const QPoint at = content
                              ->mapToScene(QPointF(content->width() / 2.0,
                                                   content->height() / 2.0))
                              .toPoint();
        auto *rowHover = messageRow->findChild<QObject *>(
            QStringLiteral("messageRowHover"));
        QVERIFY2(rowHover != nullptr, "the row has no messageRowHover handler");

        QTRY_VERIFY2_WITH_TIMEOUT(
            !timeline->property("rowHoverSuspended").toBool(),
            qPrintable(QStringLiteral("row hover stayed suspended at idle: ")
                       + rowHoverState(timeline, rowHover)),
            kSignalTimeoutMs);
        QTest::mouseMove(&window, at);
        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("hoveredActionsKey").toString() == actionKey,
            qPrintable(QStringLiteral("idle hover did not reach %1: ")
                           .arg(actionKey)
                       + rowHoverState(timeline, rowHover)),
            2000);
        QVERIFY(rowHover->property("hovered").toBool());

        QVERIFY(timeline->setProperty("stickToBottom", false));
        const qreal startY = timeline->property("contentY").toReal();
        QVERIFY(QMetaObject::invokeMethod(timeline, "beginWheelTo",
                                          Q_ARG(QVariant, startY + 400.0)));
        QVERIFY2(scroll->motionActive(), "premise: no glide started");

        int checks = 0;
        while (scroll->motionActive() && checks < 400) {
            QVERIFY2(timeline->property("hoveredActionsKey").toString()
                         .isEmpty(),
                     qPrintable(QStringLiteral(
                         "a row claimed the hover bar mid-glide (check %1)")
                                    .arg(checks)));
            QVERIFY2(!rowHover->property("hovered").toBool(),
                     "the row stayed hovered while the viewport moved");
            QTest::qWait(8);
            ++checks;
        }
        QVERIFY2(checks > 0, "premise: the glide ended before any check");
        QTRY_VERIFY_WITH_TIMEOUT(!scroll->motionActive(), 5000);
        QVERIFY2(timeline->property("contentY").toReal() > startY + 20,
                 "premise: the glide did not move the view");

        QQuickItem *underPointer = nullptr;
        const int rows = timeline->property("count").toInt();
        for (int r = 0; r < rows && !underPointer; ++r) {
            QQuickItem *item = itemAtViewRow(timeline, r);
            if (item && item->height() > 0
                && item->contains(item->mapFromScene(QPointF(at))))
                underPointer = item;
        }
        QVERIFY2(underPointer != nullptr, "premise: no row under the pointer");
        const QString keyUnder =
            underPointer->property("actionKey").toString();
        QVERIFY2(!keyUnder.isEmpty(),
                 "premise: the row under the pointer has no action bar");
        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("hoveredActionsKey").toString() == keyUnder,
            qPrintable(QStringLiteral("hover did not return to %1 after the "
                                      "glide: ")
                           .arg(keyUnder)
                       + rowHoverState(timeline, rowHover)),
            2000);
    }

    void quickMiddleClickLatchesAndTheExitClickNeverReachesARow()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 40, QStringLiteral("mid"),
                        QStringLiteral("middle click probe")),
0,420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        auto *scroller = root->findChild<QQuickItem *>(
            QStringLiteral("timelineMiddleClickScroller"));
        QVERIFY(scroller != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(scroller->isVisible(), 4000);
        auto *marker = scroller->findChild<QQuickItem *>(
            QStringLiteral("autoscrollAnchorMarker"));
        QVERIFY(marker != nullptr);

        QString actionKey;
        QQuickItem *messageRow = onScreenMessageRow(timeline, &actionKey);
        QVERIFY2(messageRow != nullptr, "no message row on screen");
        auto *content = messageRow->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        QVERIFY(content != nullptr);
        const QPoint at = content
                              ->mapToScene(QPointF(content->width() / 2.0,
                                                   content->height() / 2.0))
                              .toPoint();
        QVERIFY2(scroller->contains(scroller->mapFromScene(QPointF(at))),
                 "premise: the row is not under the scroller");
        auto menuOpen = [&] {
            return !messageRow->property("menuEventId").toString().isEmpty();
        };

        auto *rowHover = messageRow->findChild<QObject *>(
            QStringLiteral("messageRowHover"));
        QVERIFY2(rowHover != nullptr, "the row has no messageRowHover handler");
        QTRY_VERIFY2_WITH_TIMEOUT(
            !timeline->property("rowHoverSuspended").toBool(),
            qPrintable(QStringLiteral("row hover stayed suspended at idle: ")
                       + rowHoverState(timeline, rowHover)),
            kSignalTimeoutMs);
        QTest::mouseMove(&window, at + QPoint(0, 3));
        QTest::mouseMove(&window, at);
        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("hoveredActionsKey").toString() == actionKey,
            qPrintable(QStringLiteral("idle hover did not reach %1: ")
                           .arg(actionKey)
                       + rowHoverState(timeline, rowHover)),
            2000);

        QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY2(scroller->property("latched").toBool(),
                 "a quick middle click did not latch autoscroll");
        QVERIFY2(marker->isVisible(), "a latched autoscroll shows no marker");
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "a left click did not end the latched autoscroll");
        QVERIFY(!marker->isVisible());

        QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY(scroller->property("latched").toBool());
        QTest::mouseClick(&window, Qt::RightButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY(!scroller->property("active").toBool());
        QVERIFY2(!menuOpen(),
                 "the right click that ended the latch opened the row's menu");

        QTest::mouseClick(&window, Qt::RightButton, Qt::NoModifier, at);
        QTRY_VERIFY2_WITH_TIMEOUT(menuOpen(), "idle right click opened no menu",
                                  2000);
    }

    void aLatchedTimelineAutoscrollEndsOnEscapeNavigationAndRoomSwitch()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 60, QStringLiteral("latch"),
                        QStringLiteral("latch probe")),
0,420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        auto *scroller = root->findChild<QQuickItem *>(
            QStringLiteral("timelineMiddleClickScroller"));
        QVERIFY(scroller != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(scroller->isVisible(), 4000);

        const QPointF centre(scroller->width() / 2.0,
                             scroller->height() / 2.0);
        const QPoint anchor = scroller->mapToScene(centre).toPoint();
        auto latch = [&] {
            QTest::mouseClick(&window, Qt::MiddleButton, Qt::NoModifier,
                              anchor);
            QCoreApplication::processEvents();
            return scroller->property("latched").toBool();
        };

        QVERIFY(latch());
        const qreal start = timeline->property("contentY").toReal();
        QTest::mouseMove(&window,
                         scroller->mapToScene(centre + QPointF(0, -120))
                             .toPoint());
        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("contentY").toReal() > start + 20,
            "moving up did not scroll toward older messages", 4000);

        scroller->setProperty("maxSpeed", 0);
        auto *jump = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(jump != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(jump->isVisible() && jump->opacity() > 0.99,
                                 4000);
        const QPoint onJump =
            jump->mapToScene(QPointF(jump->width() / 2.0,
                                     jump->height() / 2.0)).toPoint();
        QTest::mouseMove(&window, onJump);
        QVERIFY(scroller->property("latched").toBool());
        const qreal away = timeline->property("contentY").toReal();
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, onJump);
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "a click on a pill did not end the latch");
        QTest::qWait(400);
        QVERIFY2(qAbs(timeline->property("contentY").toReal() - away) < 1.0,
                 "the click that ended the latch reached Jump to latest");
        QVERIFY(jump->isVisible());
        QTest::mouseMove(&window, anchor);

        QTest::keyClick(&window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "Escape did not end a latched autoscroll");

        QVERIFY(latch());
        QVERIFY(QMetaObject::invokeMethod(timeline, "goToLatest"));
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "jump to latest did not end a latched autoscroll");

        QTest::qWait(600);
        QVERIFY(latch());
        QTest::qWait(100);
        QVERIFY(scroller->property("latched").toBool());
        controller.setCurrentRoomId(QStringLiteral("!devs:mock.local"));
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "a room switch did not end a latched autoscroll");
    }

    void aMiddleClickOnAMessageLinkStartsNothing()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QList<TimelineEvent> events = textFixture(
            roomId, 40, QStringLiteral("lnk"), QStringLiteral("link probe"));
        for (TimelineEvent &e : events) {
            e.body = QStringLiteral("https://example.org/probe");
            e.formattedBody = QStringLiteral(
                "<a href=\"https://example.org/probe\">"
                "https://example.org/probe</a>");
        }
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events,0,
420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        auto *scroller = root->findChild<QQuickItem *>(
            QStringLiteral("timelineMiddleClickScroller"));
        QVERIFY(scroller != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(scroller->isVisible(), 4000);

        QString actionKey;
        QQuickItem *messageRow = onScreenMessageRow(timeline, &actionKey);
        QVERIFY2(messageRow != nullptr, "no message row on screen");
        auto *body = messageRow->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(body != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(body->width() > 20, 2000);
        QPointF onLink(-1, -1);
        const qreal y = qMin(body->height() / 2.0, 10.0);
        for (qreal x = 2; x < body->width() && onLink.x() < 0; x += 4) {
            QString href;
            if (QMetaObject::invokeMethod(body, "linkAt",
                                          Q_RETURN_ARG(QString, href),
                                          Q_ARG(qreal, x), Q_ARG(qreal, y))
                && !href.isEmpty())
                onLink = QPointF(x + 4, y);
        }
        QVERIFY2(onLink.x() >= 0, "premise: the message body has no link");
        const QPoint at = body->mapToScene(onLink).toPoint();
        QVERIFY(scroller->contains(scroller->mapFromScene(QPointF(at))));

        QTest::mousePress(&window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "a middle press on a link started autoscroll");
        QTest::mouseRelease(&window, Qt::MiddleButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("latched").toBool(),
                 "a middle click on a link latched autoscroll");
    }

    void activeMiddleDragIsCancelledByEscapeRoomSwitchAndNavigation()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 60, QStringLiteral("drag"),
                        QStringLiteral("drag probe")),
0,420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        auto *scroller = root->findChild<QQuickItem *>(
            QStringLiteral("timelineMiddleClickScroller"));
        QVERIFY(scroller != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(scroller->isVisible(), 4000);

        const QPointF centre(scroller->width() / 2.0,
                             scroller->height() / 2.0);
        const QPoint anchor = scroller->mapToScene(centre).toPoint();
        const QPoint dragged =
            scroller->mapToScene(centre + QPointF(0, 90)).toPoint();

        auto beginDrag = [&] {
            QTest::mousePress(&window, Qt::MiddleButton, Qt::NoModifier,
                              anchor);
            QTest::mouseMove(&window, dragged);
            QCoreApplication::processEvents();
            return scroller->property("active").toBool();
        };
        auto endDrag = [&] {
            QTest::mouseRelease(&window, Qt::MiddleButton, Qt::NoModifier,
                                dragged);
            QCoreApplication::processEvents();
        };

        QVERIFY(beginDrag());
        QTest::keyClick(&window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "Escape did not end an active autoscroll");
        endDrag();

        QVERIFY(beginDrag());
        QVERIFY(QMetaObject::invokeMethod(timeline, "goToLatest"));
        QCoreApplication::processEvents();
        QVERIFY2(!scroller->property("active").toBool(),
                 "jump to latest did not end an active autoscroll");
        endDrag();

        QVERIFY(beginDrag());
        controller.setCurrentRoomId(QStringLiteral("!devs:mock.local"));
        QCoreApplication::processEvents();
        QVERIFY(!scroller->property("active").toBool());
        endDrag();
    }

    void tonePopupOwnsRowInteractionSoNoActionBarShows()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 12, QStringLiteral("tone"),
                        QStringLiteral("tone probe")),
0,420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("count").toInt() > 0, 4000);

        QQuickItem *messageRow = nullptr;
        QString actionKey;
        for (int row = 0; row < 8 && actionKey.isEmpty(); ++row) {
            QVariant rowVar;
            if (!QMetaObject::invokeMethod(timeline, "itemAtViewRow",
                                           Q_RETURN_ARG(QVariant, rowVar),
                                           Q_ARG(QVariant, QVariant(row))))
                continue;
            auto *candidate = rowVar.value<QQuickItem *>();
            if (!candidate)
                continue;
            const QString key = candidate->property("actionKey").toString();
            if (key.isEmpty())
                continue;
            messageRow = candidate;
            actionKey = key;
        }
        QVERIFY2(messageRow != nullptr, "no real message row was instantiated");

        QVERIFY(timeline->setProperty("pinnedActionsKey", actionKey));
        QTRY_VERIFY_WITH_TIMEOUT(
            messageRow->property("actionsVisible").toBool(), 2000);

        auto *picker = root->findChild<QObject *>(
            QStringLiteral("sharedReactionPicker"));
        QVERIFY(picker != nullptr);
        auto *tonePopup =
            picker->findChild<QObject *>(QStringLiteral("emojiTonePopup"));
        QVERIFY(tonePopup != nullptr);

        QQmlExpression openPicker(
            qmlContext(timeline), timeline,
            QStringLiteral("openReactionPicker('$tone3', Qt.point(10, 10))"));
        openPicker.evaluate();
        QVERIFY2(!openPicker.hasError(),
                 openPicker.error().toString().toUtf8().constData());
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("transientInteractionOwner").toString(),
            QStringLiteral("picker"), 2000);

        QVariantList variants;
        variants.append(QVariantMap{
            { QStringLiteral("emoji"), QStringLiteral("\xF0\x9F\x91\x8D") },
            { QStringLiteral("name"), QStringLiteral("thumbs up") },
            { QStringLiteral("tone"), 1 },
        });
        QVERIFY(tonePopup->setProperty("variants", variants));
        QVERIFY(QMetaObject::invokeMethod(tonePopup, "open"));
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("transientInteractionOwner").toString(),
            QStringLiteral("tone"), 2000);

        QCOMPARE(timeline->property("pinnedActionsKey").toString(), QString());
        QCOMPARE(timeline->property("hoveredActionsKey").toString(), QString());
        QVERIFY2(!messageRow->property("actionsVisible").toBool(),
                 "a row still showed its action bar under the tone popup");

        QVERIFY(timeline->setProperty("hoveredActionsKey", actionKey));
        QCOMPARE(timeline->property("hoveredActionsKey").toString(), QString());
        QVERIFY(!messageRow->property("actionsVisible").toBool());

        QVERIFY(QMetaObject::invokeMethod(tonePopup, "close"));
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("transientInteractionOwner").toString(),
            QStringLiteral("picker"), 2000);

        QVERIFY(QMetaObject::invokeMethod(picker, "close"));
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("transientInteractionOwner").toString(),
            QString(), 2000);
        QVERIFY(timeline->setProperty("hoveredActionsKey", actionKey));
        QCOMPARE(timeline->property("hoveredActionsKey").toString(), actionKey);
    }

    void replyNavigationToAnUnexposedTargetActuallyPositionsTheView()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        QVERIFY2(timeline->property("rowWindowSkip").toInt() > 0,
                 "no window established");

        const QString targetId = QStringLiteral("$win50");
        QQmlExpression exposed(
            qmlContext(timeline), timeline,
            QStringLiteral("viewRowForStableId('") + targetId
                + QStringLiteral("')"));
        QCOMPARE(exposed.evaluate().toInt(), -1);

        controller.pagination()->setHighlightDurationForTest(120000);
        controller.pagination()->jumpToEvent(targetId);
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("navigationHighlightEventId").toString(),
            targetId, 10000);

        QQmlExpression onScreen(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = r >= 0 ? itemAtViewRow(r) : null;"
                           " if (!it) return 1e9;"
                           " return (contentY + height)"
                           "        - (it.y + it.height); })('")
                + targetId + QStringLiteral("')"));
        double offset = 1e9;
        const bool landed = QTest::qWaitFor([&] {
            const QVariant v = onScreen.evaluate();
            if (onScreen.hasError())
                return false;
            offset = v.toDouble();
            return offset >= 0.0
                   && offset <= timeline->property("height").toReal();
        }, 5000);
        const double height = timeline->property("height").toReal();
        QVERIFY2(landed, qPrintable(QStringLiteral(
                     "reply target never reached the viewport "
                     "(offset %1, viewport %2, landings %3, unresolved %4)")
                     .arg(offset).arg(height)
                     .arg(timeline->property("diagNavigationLandings").toInt())
                     .arg(timeline->property("diagNavigationUnresolved")
                              .toInt())
                     + QStringLiteral(" abandoned %1 pendingRow %2 pendingId %3"
                                      " ticks %4 count %5 laidOut %6 winSkip %7")
                           .arg(timeline->property("diagNavigationAbandoned").toInt())
                           .arg(timeline->property("navigationPendingRow").toInt())
                           .arg(timeline->property("navigationPendingId").toString())
                           .arg(timeline->property("navigationTotalAttempts").toInt())
                           .arg(timeline->property("count").toInt())
                           .arg(timeline->property("layoutRowsAtLastPass").toInt())
                           .arg(timeline->property("rowWindowSkip").toInt())));
        QVERIFY2(offset > 1.0 && offset < height - 1.0,
                 qPrintable(QStringLiteral(
                     "reply target landed flush against a viewport edge "
                     "(offset %1 of %2)").arg(offset).arg(height)));
        QCOMPARE(timeline->property("diagNavigationUnresolved").toInt(), 0);
        QCOMPARE(timeline->property("diagNavigationLandings").toInt(), 1);
    }

    void aWheelNotchAbandonsAJumpThatHasNotLandedYet()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);

        QQmlExpression exposed(
            qmlContext(timeline), timeline,
            QStringLiteral("viewRowForStableId('$win50')"));
        QCOMPARE(exposed.evaluate().toInt(), -1);

        QQmlExpression arm(
            qmlContext(timeline), timeline,
            QStringLiteral("beginNavigationLanding(50, 0, false)"));
        arm.evaluate();
        QVERIFY2(!arm.hasError(),
                 arm.error().toString().toUtf8().constData());
        QVERIFY2(!timeline->property("navigationPendingId").toString()
                      .isEmpty(),
                 "the landing did not stay pending, so this case would pass "
                 "vacuously");
        const qreal parked = timeline->property("contentY").toReal();

        sendWheelNotch(window, QPointF(350, 200), 120, false);

        QCOMPARE(timeline->property("navigationPendingId").toString(),
                 QString());
        QCOMPARE(timeline->property("navigationPendingRow").toInt(), -1);
        QCOMPARE(timeline->property("diagNavigationAbandoned").toInt(), 1);

        QTest::qWait(400);
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("diagNavigationLandings").toInt(), 0);
        QVERIFY2(!qFuzzyCompare(timeline->property("contentY").toReal() + 1.0,
                                parked + 1.0),
                 "the wheel notch did not move the view, so a teleport back "
                 "to `parked` could not be distinguished from doing nothing");
    }

    void inputBetweenALocateAndItsQueuedLandingWins()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);
        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);

        const int abandoned0 =
            timeline->property("diagNavigationAbandoned").toInt();
        QVERIFY(QMetaObject::invokeMethod(
            controller.pagination(), "targetLocated",
            Q_ARG(int, 50), Q_ARG(qreal, 0.0), Q_ARG(bool, true)));
        sendWheelNotch(window, QPointF(350, 200), 120, false);
        QTest::qWait(400);
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("navigationPendingId").toString(),
                 QString());
        QCOMPARE(timeline->property("diagNavigationLandings").toInt(), 0);
        QVERIFY2(timeline->property("diagNavigationAbandoned").toInt()
                     > abandoned0,
                 "the obsolete landing was neither landed nor counted");

        QVERIFY(QMetaObject::invokeMethod(
            controller.pagination(), "targetLocated",
            Q_ARG(int, 50), Q_ARG(qreal, 0.0), Q_ARG(bool, true)));
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("diagNavigationLandings").toInt(), 1, 5000);
    }

    void aScrollBarPressWithoutMovementAbandonsAPendingLanding()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);
        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        QQuickItem *pane = timeline;
        while (pane->parentItem() && !pane->findChild<QQuickItem *>(
                   QStringLiteral("timelineScrollBar")))
            pane = pane->parentItem();
        auto *bar = pane->findChild<QQuickItem *>(
            QStringLiteral("timelineScrollBar"));
        QVERIFY(bar != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible(), 5000);

        QQmlExpression arm(qmlContext(timeline), timeline,
                           QStringLiteral("beginNavigationLanding(50, 0, true)"));
        arm.evaluate();
        QVERIFY2(!arm.hasError(), arm.error().toString().toUtf8().constData());
        QVERIFY2(!timeline->property("navigationPendingId").toString().isEmpty(),
                 "the landing did not stay pending, so this case would pass "
                 "vacuously");

        const double size = bar->property("size").toDouble();
        const double position = bar->property("position").toDouble();
        const QPoint grab = bar->mapToScene(
            QPointF(bar->width() / 2, (position + size / 2) * bar->height()))
                                .toPoint();
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, grab);
        QTRY_VERIFY(bar->property("pressed").toBool());
        QCOMPARE(timeline->property("navigationPendingId").toString(),
                 QString());
        QTest::qWait(300);
        QCOMPARE(timeline->property("diagNavigationLandings").toInt(), 0);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, grab);
    }

    void theScrollRecorderReportsAnUnattributedSnapToTheOldestEdgeOnly()
    {
        AppController controller(AppController::MockBackend);
        controller.timelineScroll()->setScrollRecorderEnabledForTest(true);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);
        QVERIFY(controller.timelineScroll()->recordedScrollCount() > 0);
        const int before = timeline->property("diagScrollIncidents").toInt();

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        QVERIFY2(timeline->property("diagScrollIncidents").toInt() == before,
                 qPrintable(QStringLiteral("a row-window trim was reported: ")
                            + controller.timelineScroll()
                                  ->lastScrollIncidentDump()));

        QQmlExpression park(qmlContext(timeline), timeline,
                            QStringLiteral("contentY = wheelMinY()"));
        park.evaluate();
        QQmlExpression snap(qmlContext(timeline), timeline,
                            QStringLiteral("contentY = wheelMaxY()"));
        snap.evaluate();
        QVERIFY2(!snap.hasError(), snap.error().toString().toUtf8().constData());
        QVERIFY2(timeline->property("diagScrollIncidents").toInt() == before + 1,
                 qPrintable(QStringLiteral("incidents %1 -> %2, y %3 min %4 max %5 h %6")
                     .arg(before)
                     .arg(timeline->property("diagScrollIncidents").toInt())
                     .arg(timeline->property("contentY").toReal())
                     .arg(QQmlExpression(qmlContext(timeline), timeline,
                              QStringLiteral("wheelMinY()")).evaluate().toReal())
                     .arg(QQmlExpression(qmlContext(timeline), timeline,
                              QStringLiteral("wheelMaxY()")).evaluate().toReal())
                     .arg(timeline->property("height").toReal())));
        const QString dump =
            controller.timelineScroll()->lastScrollIncidentDump();
        QVERIFY2(dump.contains(QStringLiteral("w=unattributed")),
                 qPrintable(dump));
        QVERIFY(!dump.contains(QStringLiteral("$win")));
    }

    void aLocatedTargetKeepsItsIdWhenRowsShiftBeforeTheLanding()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);
        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QCoreApplication::processEvents();

        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock);
        const QString roomId = controller.currentRoomId();
        QList<TimelineEvent> shifted;
        const QDateTime base = QDateTime::currentDateTimeUtc().addSecs(-80000);
        for (int i = 0; i < 25; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$older%1").arg(i);
            e.itemId = QStringLiteral("uid-older%1").arg(i);
            e.sender = QStringLiteral("@alice:mock.local");
            e.body = QStringLiteral("older %1").arg(i);
            e.timestamp = base.addSecs(i * 30);
            shifted.append(e);
        }
        const QDateTime base2 = QDateTime::currentDateTimeUtc().addSecs(-40000);
        for (int i = 0; i < 900; ++i) {
            TimelineEvent e;
            e.eventId = QStringLiteral("$win%1").arg(i);
            e.itemId = QStringLiteral("uid-win%1").arg(i);
            e.sender = QStringLiteral("@alice:mock.local");
            e.senderDisplayName = QStringLiteral("Alice");
            e.body = QStringLiteral("window probe %1").arg(i);
            e.timestamp = base2.addSecs(i * 30);
            shifted.append(e);
        }
        const int row = controller.timeline()->rowForStableId(
            QStringLiteral("$win50"));
        QVERIFY(row >= 0);
        QVERIFY(QMetaObject::invokeMethod(
            controller.pagination(), "targetLocated",
            Q_ARG(int, row), Q_ARG(qreal, 0.0), Q_ARG(bool, true)));
        mock->resetTimelineForTest(roomId, shifted, 0);
        QVERIFY(controller.timeline()->rowForStableId(
                    QStringLiteral("$win50")) != row);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("diagNavigationLandings").toInt() > 0
                || timeline->property("diagNavigationUnresolved").toInt() > 0,
            10000);
        QCOMPARE(timeline->property("diagNavigationUnresolved").toInt(), 0);
        QQmlExpression onScreen(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = r >= 0 ? itemAtViewRow(r) : null;"
                           " if (!it) return false;"
                           " var top = it.y - contentY;"
                           " return top >= 0 && top < height; })"
                           "('$win50')"));
        QVERIFY2(onScreen.evaluate().toBool(),
                 "the jump landed away from the located message");
    }

    void theContinuationTimestampStaysInsideItsGutter_data()
    {
        QTest::addColumn<double>("scale");
        QTest::newRow("100") << 1.0;
        QTest::newRow("130") << 1.3;
        QTest::newRow("140") << 1.4;
        QTest::newRow("150") << 1.5;
    }

    void theContinuationTimestampStaysInsideItsGutter()
    {
        QFETCH(double, scale);
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 60, 0.0, 420);
        QVERIFY(timeline != nullptr);
        QQmlExpression setScale(
            qmlContext(timeline), timeline,
            QStringLiteral("AppTheme.textScale = %1").arg(scale));
        setScale.evaluate();
        QVERIFY2(!setScale.hasError(),
                 setScale.error().toString().toUtf8().constData());
        QTest::qWait(200);

        QQuickItem *label = nullptr;
        QQuickItem *rowItem = nullptr;
        const std::function<QQuickItem *(QQuickItem *)> find =
            [&](QQuickItem *it) -> QQuickItem * {
            if (it->objectName() == QLatin1String("continuationTimestamp"))
                return it;
            for (QQuickItem *c : it->childItems())
                if (QQuickItem *f = find(c))
                    return f;
            return nullptr;
        };
        for (int y = 40; y < window.height() - 20 && !label; y += 18) {
            QTest::mouseMove(&window, QPoint(300, y));
            QTest::qWait(30);
            label = find(window.contentItem());
        }
        QVERIFY2(label != nullptr,
                 "no continuation timestamp appeared on hover, so the "
                 "geometry below would pass vacuously");
        Q_UNUSED(rowItem);
        QVERIFY2(label->property("contentWidth").toReal()
                     <= label->width() + 0.5,
                 qPrintable(QStringLiteral(
                                "timestamp text %1 wider than its slot %2")
                                .arg(label->property("contentWidth").toReal())
                                .arg(label->width())));
        const QPointF topLeft = label->mapToScene(QPointF(0, 0));
        const qreal textLeft = topLeft.x() + label->width()
            - label->property("contentWidth").toReal();
        QVERIFY2(textLeft >= 0.0, qPrintable(QStringLiteral(
            "timestamp drawn at x=%1, outside the row").arg(textLeft)));
        QVERIFY(label->width() > 10.0);
    }

    void aLandingWhoseViewNeverStopsChangingGivesUpInsteadOfWaitingForever()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.55);
        QVERIFY(timeline != nullptr);

        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);

        QQmlExpression arm(
            qmlContext(timeline), timeline,
            QStringLiteral("beginNavigationLanding(50, 0, false)"));
        arm.evaluate();
        QVERIFY2(!arm.hasError(),
                 arm.error().toString().toUtf8().constData());
        QVERIFY(!timeline->property("navigationPendingId").toString().isEmpty());

        QQmlExpression retry(qmlContext(timeline), timeline,
                             QStringLiteral("tryLandNavigationTarget()"));
        for (int i = 0; i < 400; ++i) {
            timeline->setProperty("layoutRowsAtLastPass", i + 1);
            retry.evaluate();
            QVERIFY2(!retry.hasError(),
                     retry.error().toString().toUtf8().constData());
            if (timeline->property("navigationPendingId").toString().isEmpty())
                break;
        }

        QCOMPARE(timeline->property("navigationPendingId").toString(),
                 QString());
        QCOMPARE(timeline->property("navigationPendingRow").toInt(), -1);
        QCOMPARE(timeline->property("diagNavigationUnresolved").toInt(), 1);
        QCOMPARE(timeline->property("diagNavigationLandings").toInt(), 0);
    }

    void aJumpToTheNewestMessageKeepsFollowingSoTheRoomIsRead()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        ReadReceiptCoordinator *receipts = controller.readReceipts();
        QVERIFY(receipts != nullptr);
        QObject::disconnect(qGuiApp,
                            SIGNAL(applicationStateChanged(Qt::ApplicationState)),
                            &controller, nullptr);
        QObject::disconnect(qGuiApp, SIGNAL(focusWindowChanged(QWindow*)),
                            &controller, nullptr);
        receipts->setWindowActive(false);
        receipts->setDebounceMs(10);
        QSignalSpy sent(receipts, &ReadReceiptCoordinator::receiptSent);

        QList<TimelineEvent> events =
            textFixture(roomId, 30, QStringLiteral("n"), QStringLiteral("body"));
        TimelineEvent marker;
        marker.itemId = QStringLiteral("uid-n-marker");
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        events.append(marker);
        TimelineEvent latest;
        latest.eventId = QStringLiteral("$n-latest");
        latest.itemId = QStringLiteral("uid-n-latest");
        latest.roomId = roomId;
        latest.sender = QStringLiteral("@bob:mock.local");
        latest.senderDisplayName = QStringLiteral("Bob");
        latest.body = QStringLiteral("the new one");
        latest.timestamp = events.at(29).timestamp.addSecs(30);
        latest.type = TimelineEvent::TextMessage;
        latest.status = TimelineEvent::Sent;
        events.append(latest);

        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events,0,
600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        QVERIFY(timeline->property("stickToBottom").toBool());
        QVERIFY2(!receipts->windowActive() && sent.isEmpty(),
                 "a receipt went out before the jump, so this case cannot "
                 "tell a landing that keeps following from one that does not");

        controller.pagination()->jumpToEvent(QStringLiteral("$n-latest"));
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("diagNavigationLandings").toInt(), 1, 5000);
        QTest::qWait(300);

        double minY = 0.0;
        double maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        const double contentY = timeline->property("contentY").toDouble();
        QVERIFY2(timeline->property("stickToBottom").toBool(),
                 qPrintable(QStringLiteral(
                     "the landing at the live edge left follow-latest off "
                     "(contentY %1, live edge %2)").arg(contentY).arg(minY)));
        QVERIFY(receipts->nearBottom());
        QQmlExpression below(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(id){ var r = viewRowForStableId(id);"
                           " var it = r >= 0 ? itemAtViewRow(r) : null;"
                           " return it ? contentY - it.y : 1e9; })"
                           "('$n-latest')"));
        const double hidden = below.evaluate().toDouble();
        QVERIFY2(!below.hasError(),
                 below.error().toString().toUtf8().constData());
        QVERIFY2(hidden <= 0.5,
                 qPrintable(QStringLiteral(
                     "the new message is %1 px under the bottom edge")
                     .arg(hidden)));

        receipts->setWindowActive(true);
        QTRY_VERIFY_WITH_TIMEOUT(!sent.isEmpty(), 3000);
        QCOMPARE(sent.last().at(1).toString(), QStringLiteral("$n-latest"));
    }

    void teardownDuringAPendingResetSettleLogsNoWarning()
    {
        AppController controller(AppController::MockBackend);
        const QString roomA = loginAndRoomIdAt(controller,0);
        QVERIFY(!roomA.isEmpty());
        QVERIFY(controller.roomList()->rowCount() > 1);
        const QString roomB = controller.roomList()
            ->data(controller.roomList()->index(1, 0),
                   RoomListModel::RoomIdRole)
            .toString();
        QVERIFY(!roomB.isEmpty());
        QVERIFY(roomA != roomB);

        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, roomA,
                                          textFixture(roomA, 5, "a", "hello"),
0,
400, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);

        LogCapture capture;
        controller.setCurrentRoomId(roomB);

        delete root;
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        QTest::qWait(50);
        QCoreApplication::processEvents();

        for (const QString &line : capture.messages()) {
            QVERIFY2(!line.contains(QStringLiteral("is not a function")),
                     qPrintable(line));
        }
    }

    void canonicalParentIsOfferedWithoutOpeningTheInfoPanel()
    {
        struct ParentClient : MockMatrixClient {
            QVariantMap parentSpace{
                {QStringLiteral("space_id"), QStringLiteral("!parent:example.org")},
                {QStringLiteral("name"), QStringLiteral("<b>Team</b>")},
                {QStringLiteral("via"), QStringList{QStringLiteral("example.org")}},
                {QStringLiteral("joined"), false}};
            quint64 nextOp = 0;
            QString joinedTarget;
            QStringList joinedVia;
            bool supportsRoomManagement() const override { return true; }
            bool supportsRoomDiscovery() const override { return true; }
            quint64 requestRoomMembers(const QString &roomId) override
            {
                const quint64 op = ++nextOp;
                const QVariantMap snapshot{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("canonicalSpaceParent"), parentSpace}};
                QMetaObject::invokeMethod(this, [this, op, roomId, snapshot] {
                    Q_EMIT roomMembersReceived(op, roomId, snapshot);
                }, Qt::QueuedConnection);
                return op;
            }
            quint64 joinRoomByIdOrAlias(const QString &target,
                                       const QStringList &via) override
            {
                joinedTarget = target;
                joinedVia = via;
                return ++nextOp;
            }
        } managed;
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller, 0).isEmpty());
        controller.settings()->setSecretStore(nullptr);
        const QString bannerUser = QStringLiteral("@banner:example.org");
        controller.settings()->saveSession(QStringLiteral("https://example.org"),
                                           bannerUser, QStringLiteral("BANNERDEV"),
                                           QStringLiteral("banner-token"));
        controller.settings()->setActiveAccountUserId(bannerUser);
        auto *original = controller.findChild<MockMatrixClient *>();
        QVERIFY(original);
        const auto restore = qScopeGuard([&] {
            controller.roomInfo()->setClient(original);
            controller.discovery()->setClient(original);
            controller.settings()->clearSessionForAccount(bannerUser);
        });
        controller.roomInfo()->setClient(&managed);
        controller.discovery()->setClient(&managed);
        const QString general = QStringLiteral("!general:mock.local");
        const QString dm = QStringLiteral("!dm-bob:mock.local");
        controller.setCurrentRoomId(general);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *root = qobject_cast<QQuickItem *>(engine.rootObjects().first());
        QVERIFY(root);
        auto *banner = root->findChild<QQuickItem *>(QStringLiteral("spaceParentBanner"));
        QVERIFY(banner);
        QTRY_COMPARE(controller.roomInfo()->roomId(), general);
        QTRY_VERIFY(banner->isVisible());
        QVERIFY(!root->property("infoOpen").toBool());
        auto *label = root->findChild<QQuickItem *>(QStringLiteral("spaceParentBannerText"));
        QVERIFY(label);
        QCOMPARE(label->property("text").toString(),
                 QStringLiteral("This room is in <b>Team</b>. Joining adds the Space to your "
                                "sidebar; you can leave at any time."));
        auto *infoIcon = root->findChild<QQuickItem *>(QStringLiteral("spaceParentInfoIcon"));
        QVERIFY(infoIcon);
        const QString tip =
            infoIcon->property("tipText").toString();
        QVERIFY(tip.contains(QStringLiteral("does not join you to any of its other rooms")));
        QVERIFY(tip.contains(QStringLiteral("Other members of the Space can see that you joined")));
        QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));

        managed.parentSpace.insert(QStringLiteral("joined"), true);
        controller.roomInfo()->refreshMembers();
        QTRY_VERIFY(!banner->isVisible());
        managed.parentSpace.insert(QStringLiteral("joined"), false);
        controller.setCurrentRoomId(dm);
        QTRY_VERIFY(banner->isVisible());
        auto *dismiss = root->findChild<QQuickItem *>(QStringLiteral("spaceParentDismissButton"));
        QVERIFY(dismiss);
        QVERIFY(QMetaObject::invokeMethod(dismiss, "clicked"));
        QTRY_VERIFY(!banner->isVisible());
        QCOMPARE(controller.settings()->dismissedSpaceBanners(),
                 QStringList{QStringLiteral("!parent:example.org")});
        controller.setCurrentRoomId(general);
        QTRY_COMPARE(controller.roomInfo()->roomId(), general);
        QTRY_VERIFY(!banner->isVisible());
        controller.setCurrentRoomId(dm);
        QTRY_VERIFY(!banner->isVisible());

        controller.settings()->clearSessionForAccount(bannerUser);
        controller.settings()->saveSession(QStringLiteral("https://example.org"),
                                           bannerUser, QStringLiteral("BANNERDEV2"),
                                           QStringLiteral("banner-token-2"));
        controller.settings()->setActiveAccountUserId(bannerUser);
        QVERIFY(controller.settings()->dismissedSpaceBanners().isEmpty());
        controller.setCurrentRoomId(general);
        QTRY_VERIFY(banner->isVisible());

        auto *join = root->findChild<QQuickItem *>(QStringLiteral("spaceParentJoinButton"));
        QVERIFY(join);
        QVERIFY(QMetaObject::invokeMethod(join, "clicked"));
        QCOMPARE(managed.joinedTarget, QStringLiteral("!parent:example.org"));
        QCOMPARE(managed.joinedVia, QStringList{QStringLiteral("example.org")});
        QVERIFY(!join->isEnabled());
        QCOMPARE(join->property("text").toString(), QStringLiteral("Joining…"));
        Q_EMIT controller.discovery()->joinFailed(QStringLiteral("!parent:example.org"),
                                                  QStringLiteral("nope"));
        QTRY_VERIFY(label->property("text").toString().contains(QStringLiteral("nope")));
        QCOMPARE(join->property("text").toString(), QStringLiteral("Join space"));
        managed.parentSpace.clear();
        controller.roomInfo()->refreshMembers();
        QTRY_VERIFY(!banner->isVisible());
        QVERIFY(controller.roomInfo()->canonicalSpaceParent().isEmpty());
    }

    void findBarOffersAllRoomsWhenNothingMatches()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors) warnings << e.toString();
                });
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(900, 700);
        root->setParentItem(window.contentItem());
        root->setWidth(window.width());
        root->setHeight(window.height());
        window.show();
        QCoreApplication::processEvents();

        QSignalSpy allRooms(root, SIGNAL(searchAllRoomsRequested(QString)));
        QVERIFY(allRooms.isValid());
        QVERIFY(QMetaObject::invokeMethod(root, "openFind"));
        QCoreApplication::processEvents();
        auto *field = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindField"));
        QVERIFY(field != nullptr);
        auto *row = root->findChild<QQuickItem *>(
            QStringLiteral("findNoMatchesRow"));
        QVERIFY2(row != nullptr, "the no-matches row does not exist");
        QVERIFY(!row->isVisible());

        QVERIFY(field->setProperty("text",
                                   QStringLiteral("zzzqqq-no-such-text")));
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(row->isVisible(), kSignalTimeoutMs);
        auto *label = root->findChild<QQuickItem *>(
            QStringLiteral("findNoMatchesLabel"));
        QVERIFY(label != nullptr);
        QVERIFY(label->isVisible());
        QCOMPARE(label->property("text").toString(),
                 QStringLiteral("No matches in loaded messages"));
        auto *button = root->findChild<QQuickItem *>(
            QStringLiteral("findSearchAllRoomsButton"));
        QVERIFY(button != nullptr);
        QVERIFY(button->isVisible());
        QCOMPARE(button->property("text").toString(),
                 QStringLiteral("Search all rooms for 'zzzqqq-no-such-text'"));

        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QCOMPARE(allRooms.count(), 1);
        QCOMPARE(allRooms.at(0).at(0).toString(),
                 QStringLiteral("zzzqqq-no-such-text"));
        QCOMPARE(root->property("findOpen").toBool(), false);
    }

    void memberPanelStaysOpenAcrossRoomSwitches()
    {
        struct ManagedClient : MockMatrixClient {
            bool supportsRoomManagement() const override { return true; }
        };
        ManagedClient managed;
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        auto *original = controller.findChild<MockMatrixClient *>();
        QVERIFY(original != nullptr);
        struct Restore {
            AppController &c;
            MockMatrixClient *original;
            ~Restore()
            {
                c.settings()->setProperty("memberPanelOpen", false);
                c.roomInfo()->setClient(original);
            }
        } restore{controller, original};
        controller.roomInfo()->setClient(&managed);
        QVERIFY(controller.roomInfo()->supported());

        const QString general = QStringLiteral("!general:mock.local");
        const QString dm = QStringLiteral("!dm-bob:mock.local");
        QVERIFY2(controller.settings()->property("memberPanelOpen").isValid(),
                 "no memberPanelOpen setting");
        QCOMPARE(controller.settings()->property("memberPanelOpen").toBool(),
                 false);
        controller.setCurrentRoomId(general);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);
        QQuickWindow window;
        window.resize(1000, 700);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        QCoreApplication::processEvents();
        auto *panel = root->findChild<QQuickItem *>(
            QStringLiteral("roomInfoPanel"));
        QVERIFY(panel != nullptr);

        controller.setCurrentRoomId(dm);
        QCoreApplication::processEvents();
        QCOMPARE(root->property("infoOpen").toBool(), false);
        controller.setCurrentRoomId(general);

        QVERIFY(QMetaObject::invokeMethod(root, "toggleMemberPanel"));
        QCOMPARE(root->property("infoOpen").toBool(), true);
        QCOMPARE(panel->property("section").toString(),
                 QStringLiteral("people"));
        QCOMPARE(controller.settings()->property("memberPanelOpen").toBool(),
                 true);

        controller.setCurrentRoomId(dm);
        QTRY_VERIFY_WITH_TIMEOUT(root->property("infoOpen").toBool(),
                                 kSignalTimeoutMs);
        QCOMPARE(panel->property("section").toString(),
                 QStringLiteral("people"));
        QCOMPARE(controller.roomInfo()->roomId(), dm);
        QCOMPARE(controller.settings()->property("memberPanelOpen").toBool(),
                 true);

        QVERIFY(QMetaObject::invokeMethod(root, "toggleMemberPanel"));
        QCOMPARE(root->property("infoOpen").toBool(), false);
        QCOMPARE(controller.settings()->property("memberPanelOpen").toBool(),
                 false);
        controller.setCurrentRoomId(general);
        QCoreApplication::processEvents();
        QCOMPARE(root->property("infoOpen").toBool(), false);
    }

    void theFirstUnreadPillFollowsWhereTheMarkerIs()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;

        QList<TimelineEvent> events =
            textFixture(roomId, 60, QStringLiteral("s"), QStringLiteral("body"));
        TimelineEvent marker;
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        events.insert(58, marker);
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events,0,
600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        QCOMPARE(controller.timeline()->readMarkerRow(), 58);
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        auto *icon = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadIcon"));
        QVERIFY(pill != nullptr);
        QVERIFY2(icon != nullptr, "the pill's chevron has no objectName");
        QVERIFY(timeline->property("stickToBottom").toBool());

        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("firstUnreadOnScreen").toBool(),
            "fixture: the marker is not on screen at the live edge", 2000);
        QVERIFY2(!pill->isVisible(),
                 "the divider is on screen and the pill still offers a jump "
                 "to it");

        double minY = 0.0;
        double maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY2(maxY > minY + 600.0,
                 "fixture is too short to scroll the marker off screen");
        QVERIFY(QMetaObject::invokeMethod(timeline, "goToEarliestLoaded"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("firstUnreadBelow").toBool(),
            "the marker did not leave the bottom of the viewport", 2000);
        QVERIFY(!timeline->property("stickToBottom").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(pill->isVisible(), 2000);
        QCOMPARE(icon->property("name").toString(),
                 QStringLiteral("expand_more"));

        QVERIFY(QMetaObject::invokeMethod(timeline, "goToLatest"));
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("stickToBottom").toBool(),
                                 3000);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        const QList<TimelineEvent> live =
            textFixture(roomId, 40, QStringLiteral("slive"),
                        QStringLiteral("live"),1300);
        for (const TimelineEvent &e : live)
            mock->appendEventForTest(roomId, e);
        QTRY_VERIFY2_WITH_TIMEOUT(
            !timeline->property("firstUnreadOnScreen").toBool()
                && !timeline->property("firstUnreadBelow").toBool(),
            "fixture: the live messages did not push the marker above the "
            "viewport", 3000);
        QVERIFY(timeline->property("stickToBottom").toBool());
        QTest::qWait(100);
        QVERIFY2(!pill->isVisible(),
                 "the reader passed the divider and is at the live edge, and "
                 "the pill still offers to go back to it");
    }

    void theFirstUnreadPillReachesAMarkerTheRowWindowWithholds()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QList<TimelineEvent> events = textFixture(
            roomId, 900, QStringLiteral("rw"), QStringLiteral("window body"));
        TimelineEvent marker;
        marker.roomId = roomId;
        marker.type = TimelineEvent::ReadMarker;
        events.insert(880, marker);
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events,0,
420, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        QCOMPARE(controller.timeline()->readMarkerRow(), 880);
        QTRY_COMPARE_WITH_TIMEOUT(timeline->property("count").toInt(), 901,
                                  10000);
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("presentationReady").toBool(), 5000);

        QVERIFY(timeline->setProperty("stickToBottom", false));
        double minY = 0.0;
        double maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY(timeline->setProperty("contentY", maxY * 0.6));
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            !timeline->property("viewportMotionActive").toBool(), 5000);
        QQmlExpression idle(qmlContext(timeline), timeline, QStringLiteral(
            "!app.pagination || !app.pagination.busy"));
        QTRY_VERIFY_WITH_TIMEOUT(idle.evaluate().toBool(), 5000);
        QQmlExpression apply(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ updateVisibleRowRange();"
                           " applyRowWindow(); updateVisibleRowRange();"
                           " return rowWindowSkip; })()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        QQmlExpression settled(qmlContext(timeline), timeline, QStringLiteral(
            "(function(){ updateVisibleRowRange(); return rowWindowSkip; })()"));
        const int skip = settled.evaluate().toInt();
        QQmlExpression diag(qmlContext(timeline), timeline, QStringLiteral(
            "'ready=' + presentationReady + ' motion=' + viewportMotionActive"
            " + ' busy=' + (app.pagination ? app.pagination.busy : 'n')"
            " + ' first=' + visibleFirstRow + ' last=' + visibleLastRow"
            " + ' runway=' + windowRunwayRows + ' min=' + windowMinRows"
            " + ' count=' + count + ' cy=' + contentY + ' tv=' + !!app.timelineView"));
        QVERIFY2(skip > 0, qPrintable(diag.evaluate().toString()));
        QVERIFY2(timeline->property("firstUnreadViewRow").toInt() < 0,
                 "fixture: the marker is not among the withheld newest rows");

        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        QVERIFY(pill != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(pill->isVisible(), 2000);
        const int unresolvedBefore =
            timeline->property("diagNavigationUnresolved").toInt();
        QMetaObject::invokeMethod(pill, "click");
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("navigationPendingRow").toInt() < 0, 5000);
        QCOMPARE(timeline->property("diagNavigationUnresolved").toInt(),
                 unresolvedBefore);
        QTRY_VERIFY2_WITH_TIMEOUT(
            timeline->property("firstUnreadOnScreen").toBool(),
            "the pill did not bring the divider on screen", 3000);
    }

    void theJumpPillStaysWhileTheWindowWithholdsTheNewestRows()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.6);
        QVERIFY(timeline != nullptr);
        QQuickItem *root = paneRootOf(timeline);
        QVERIFY(root != nullptr);
        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        QVERIFY2(timeline->property("rowWindowSkip").toInt() > 0,
                 "no window was established, so the case is unreachable here");
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(pill != nullptr);
        QVERIFY(pill->isVisible());

        QVERIFY(timeline->setProperty("stickToBottom", true));
        QCoreApplication::processEvents();
        QVERIFY(timeline->property("rowWindowSkip").toInt() > 0);
        QVERIFY2(pill->isVisible(),
                 "the window still withholds the newest rows and the jump "
                 "pill hid on follow-latest alone");
    }

    void aFindMatchBehindTheRowWindowLandsOnScreen()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline =
            deepHistoryPane(controller, engine, window, 900, 0.6);
        QVERIFY(timeline != nullptr);
        QQuickItem *root = paneRootOf(timeline);
        QVERIFY(root != nullptr);
        QQmlExpression apply(qmlContext(timeline), timeline,
                             QStringLiteral("applyRowWindow()"));
        apply.evaluate();
        QVERIFY2(!apply.hasError(),
                 apply.error().toString().toUtf8().constData());
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("pendingWindowSkip").toInt() < 0, 10000);
        const int skip = timeline->property("rowWindowSkip").toInt();
        QVERIFY2(skip > 100,
                 qPrintable(QStringLiteral(
                     "window skip %1 is too small to put a match far enough "
                     "from the live edge").arg(skip)));

        const int target = 899 - skip / 2;
        QVERIFY(target >= 100);
        const QString eventId = QStringLiteral("$win%1").arg(target);
        QVERIFY(QMetaObject::invokeMethod(root, "openFind"));
        auto *field = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindField"));
        QVERIFY(field != nullptr);
        QVERIFY(field->setProperty(
            "text", QStringLiteral("window probe %1").arg(target)));
        QCOMPARE(controller.timeline()->property("searchResultCount").toInt(),
                 1);
        QCOMPARE(controller.timeline()->property("searchCurrentEventId")
                     .toString(), eventId);

        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("navigationPendingRow").toInt() < 0, 5000);
        QCOMPARE(timeline->property("diagNavigationUnresolved").toInt(), 0);
        QCOMPARE(timeline->property("rowWindowSkip").toInt(), 0);
        QQmlExpression where(
            qmlContext(timeline), timeline,
            QStringLiteral("(function(){ updateVisibleRowRange();"
                           " return [viewRowForStableId('%1'),"
                           " viewportNewestRow, viewportOldestRow]; })()")
                .arg(eventId));
        QVariantList at;
        const bool onScreen = QTest::qWaitFor([&] {
            at = where.evaluate().toList();
            return at.size() == 3 && at.value(0).toInt() >= 0
                && at.value(0).toInt() >= at.value(1).toInt()
                && at.value(0).toInt() <= at.value(2).toInt();
        }, 2000);
        QVERIFY2(!where.hasError(),
                 where.error().toString().toUtf8().constData());
        QVERIFY2(onScreen,
                 qPrintable(QStringLiteral(
                     "the match (view row %1) is not on screen (rows %2..%3)")
                     .arg(at.value(0).toInt()).arg(at.value(1).toInt())
                     .arg(at.value(2).toInt())));
    }

    void aMatchCountChangeDoesNotPullTheReaderBackToTheMatch()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QList<TimelineEvent> events = textFixture(
            roomId, 80, QStringLiteral("rs"), QStringLiteral("plain text"));
        events[10].body = QStringLiteral("needle one");
        events[40].body = QStringLiteral("needle two");
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                          events,0,
600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        QVERIFY(QMetaObject::invokeMethod(root, "openFind"));
        auto *field = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindField"));
        QVERIFY(field != nullptr);
        QVERIFY(field->setProperty("text", QStringLiteral("needle")));
        QCOMPARE(controller.timeline()->property("searchResultCount").toInt(),
                 2);
        QCOMPARE(controller.timeline()->property("searchCurrentEventId")
                     .toString(), QStringLiteral("$rs40"));
        QTRY_VERIFY_WITH_TIMEOUT(
            timeline->property("navigationPendingRow").toInt() < 0, 3000);

        QVERIFY(QMetaObject::invokeMethod(timeline, "goToEarliestLoaded"));
        QTest::qWait(100);
        QQmlExpression top(qmlContext(timeline), timeline,
                           QStringLiteral("eventIdAtViewRow("
                                          "viewRowAtPhysicalTop())"));
        const QString topBefore = top.evaluate().toString();
        QVERIFY2(!top.hasError(), top.error().toString().toUtf8().constData());
        QVERIFY(!topBefore.isEmpty());
        QVERIFY2(topBefore != QStringLiteral("$rs40"),
                 "fixture: the oldest rows are the match's rows");

        TimelineEvent live =
            textFixture(roomId, 1, QStringLiteral("rslive"),
                        QStringLiteral("needle three"),100)
                .first();
        mock->appendEventForTest(roomId, live);
        QTRY_COMPARE_WITH_TIMEOUT(
            controller.timeline()->property("searchResultCount").toInt(), 3,
            2000);
        QCOMPARE(controller.timeline()->property("searchCurrentEventId")
                     .toString(), QStringLiteral("$rs40"));
        QTest::qWait(300);
        const auto rowOf = [](const QString &id) {
            return id.mid(QStringLiteral("$rs").size()).toInt();
        };
        const QString topAfter = top.evaluate().toString();
        QVERIFY2(topAfter.startsWith(QStringLiteral("$rs"))
                     && std::abs(rowOf(topAfter) - rowOf(topBefore)) <= 5,
                 qPrintable(QStringLiteral("the reader was moved from %1 to %2")
                                .arg(topBefore, topAfter)));

        QVERIFY(QMetaObject::invokeMethod(root, "stepFind",
                                          Q_ARG(QVariant, true)));
        QCOMPARE(controller.timeline()->property("searchCurrentEventId")
                     .toString(), live.eventId);
        QTRY_VERIFY_WITH_TIMEOUT(
            top.evaluate().toString() != topBefore, 3000);
    }

    void whitespaceIsNotAFindQuery()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 20, QStringLiteral("ws"), QStringLiteral("body")),
0,600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(QMetaObject::invokeMethod(root, "openFind"));
        auto *field = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindField"));
        auto *counter = root->findChild<QQuickItem *>(
            QStringLiteral("timelineFindCount"));
        QVERIFY(field != nullptr);
        QVERIFY(counter != nullptr);

        QVERIFY(field->setProperty("text", QStringLiteral("   ")));
        QCoreApplication::processEvents();
        QCOMPARE(controller.timeline()->property("searchResultCount").toInt(),
                 0);
        QCOMPARE(counter->property("text").toString(), QString());

        QVERIFY(field->setProperty("text", QStringLiteral("zzzq-none")));
        QCoreApplication::processEvents();
        QCOMPARE(counter->property("text").toString(),
                 QStringLiteral("No matches"));
    }

    void contextViewJumpToLatestSettlesTheLiveTimeline()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 80, QStringLiteral("cx"), QStringLiteral("body")),
0,600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        double minY = 0.0;
        double maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY2(maxY > minY + 600.0, "fixture is not scrollable");
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(timeline->setProperty("contentY", maxY));
        QCoreApplication::processEvents();
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToLatestButton"));
        QVERIFY(pill != nullptr);
        QVERIFY(pill->isVisible());

        ContextController *context = controller.eventContext();
        QVERIFY(context != nullptr);
        QVERIFY(!context->active());
        QVERIFY(QMetaObject::invokeMethod(context, "latestRequested"));
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("stickToBottom").toBool(),
                                 2000);
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(timeline->property("contentY").toDouble() - minY) < 1.0, 2000);
        QVERIFY(!pill->isVisible());
    }

    void sendingYourOwnMessageReturnsToTheNewest()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 80, QStringLiteral("own"), QStringLiteral("body")),
0,600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        double minY = 0.0;
        double maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY2(maxY > minY + 600.0, "fixture is not scrollable");
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(timeline->setProperty("contentY", maxY));
        QCoreApplication::processEvents();

        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        int before = controller.timeline()->rowCount();
        mock->sendTextMessage(roomId, QStringLiteral("from elsewhere"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.timeline()->rowCount(), before + 1,
                                  kSignalTimeoutMs);
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);

        before = controller.timeline()->rowCount();
        controller.composer()->setText(QStringLiteral("my own message"));
        controller.composer()->send();
        QTRY_COMPARE_WITH_TIMEOUT(controller.timeline()->rowCount(), before + 1,
                                  kSignalTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("stickToBottom").toBool(),
                                 2000);
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(timeline->property("contentY").toDouble() - minY) < 1.0, 2000);
    }

    void aReplySentFromHistoryKeepsTheReaderThere()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 80, QStringLiteral("own"), QStringLiteral("body")),
0,600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        double minY = 0.0;
        double maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY2(maxY > minY + 600.0, "fixture is not scrollable");
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(timeline->setProperty("contentY", maxY));
        QCoreApplication::processEvents();

        QVariant target;
        QVERIFY(QMetaObject::invokeMethod(timeline, "eventIdAtViewRow",
                                          Q_RETURN_ARG(QVariant, target),
                                          Q_ARG(QVariant, 70)));
        QVERIFY(!target.toString().isEmpty());
        const int before = controller.timeline()->rowCount();
        controller.composer()->beginReply(target.toString(),
                                          QStringLiteral("@own:mock.local"),
                                          QStringLiteral("body"));
        controller.composer()->setText(QStringLiteral("a reply"));
        controller.composer()->send();
        QTRY_COMPARE_WITH_TIMEOUT(controller.timeline()->rowCount(), before + 1,
                                  kSignalTimeoutMs);
        QTest::qWait(600);
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY2(timeline->property("contentY").toDouble() > minY + 300.0,
                 "a reply sent from history jumped to the newest");
    }

    void turningOffTheSendJumpKeepsThePlaceOnAPlainSend()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        QObject *settings = controller.settings();
        QVERIFY(settings->setProperty("jumpToNewestOnSend", false));
        const auto restore = qScopeGuard(
            [settings] { settings->setProperty("jumpToNewestOnSend", true); });
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 80, QStringLiteral("own"), QStringLiteral("body")),
0,600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        double minY = 0.0;
        double maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY2(maxY > minY + 600.0, "fixture is not scrollable");
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(timeline->setProperty("contentY", maxY));
        QCoreApplication::processEvents();

        const int before = controller.timeline()->rowCount();
        controller.composer()->setText(QStringLiteral("sent while paging"));
        controller.composer()->send();
        QTRY_COMPARE_WITH_TIMEOUT(controller.timeline()->rowCount(), before + 1,
                                  kSignalTimeoutMs);
        QTest::qWait(600);
        QCOMPARE(timeline->property("stickToBottom").toBool(), false);
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY2(timeline->property("contentY").toDouble() > minY + 300.0,
                 "a send with the jump turned off went to the newest");
    }

    void theLoadingChipSitsBelowTheFirstUnreadPill()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 30, QStringLiteral("chip"), QStringLiteral("body")),
0,600, &timeline);
        QVERIFY(root != nullptr);
        auto *chip = root->findChild<QQuickItem *>(
            QStringLiteral("paginationHeader"));
        auto *pill = root->findChild<QQuickItem *>(
            QStringLiteral("jumpToFirstUnreadButton"));
        QVERIFY(chip != nullptr);
        QVERIFY(pill != nullptr);
        pill->setProperty("visible", true);
        QTRY_VERIFY(pill->isVisible());
        QTRY_VERIFY2(chip->mapToScene(QPointF(0, 0)).y()
                         >= pill->mapToScene(QPointF(0, pill->height())).y(),
                     "the loading chip shares the pill's place");
        pill->setProperty("visible", false);
        QTRY_COMPARE(chip->y(), 0.0);
    }

    void theActionBarOfACutRowStaysInsideTheTimeline()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 40, QStringLiteral("bar"),
                        QStringLiteral("one\ntwo\nthree\nfour\nfive")),
0,600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        QVERIFY(timeline->setProperty("stickToBottom", false));
        const qreal viewTop = timeline->mapRectToScene(
            QRectF(0, 0, timeline->width(), timeline->height())).top();

        QQuickItem *loader = nullptr;
        QQuickItem *row = nullptr;
        double minY = 0.0;
        double maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        for (double y = minY + 100; y < maxY && !loader; y += 5) {
            timeline->setProperty("contentY", y);
            QCoreApplication::processEvents();
            QList<QQuickItem *> loaders;
            QList<QQuickItem *> stack{ timeline };
            while (!stack.isEmpty()) {
                QQuickItem *it = stack.takeLast();
                if (it->objectName()
                    == QLatin1String("messageActionBarLoader"))
                    loaders.append(it);
                stack.append(it->childItems());
            }
            for (QQuickItem *candidate : std::as_const(loaders)) {
                QQuickItem *host = candidate->parentItem();
                if (!host || !host->isVisible() || host->height() < 60)
                    continue;
                const qreal over = viewTop - host->mapRectToScene(
                    QRectF(0, 0, host->width(), host->height())).top();
                if (over >= 10 && over <= 20) {
                    loader = candidate;
                    row = host;
                    break;
                }
            }
        }
        QVERIFY2(loader, "no row stood over the top of the timeline");
        QQuickItem *delegate = row;
        while (delegate && !delegate->property("actionKey").isValid())
            delegate = delegate->parentItem();
        QVERIFY(delegate != nullptr);
        QVERIFY(timeline->setProperty("pinnedActionsKey",
                                      delegate->property("actionKey")));
        QTRY_VERIFY(loader->isVisible());
        QTRY_VERIFY(loader->height() > 0);
        QTRY_VERIFY2(loader->mapRectToScene(
                         QRectF(0, 0, loader->width(), loader->height())).top()
                         >= viewTop,
                     "the action bar stands over the top of the timeline");
    }

    void contextLoadNewerAtTheEndRestoresTheLivePane_data()
    {
        QTest::addColumn<int>("messageCount");
        QTest::newRow("content-fits") << 1;
        QTest::newRow("live-pane-left-in-history") << 80;
    }

    void contextLoadNewerAtTheEndRestoresTheLivePane()
    {
        QFETCH(int, messageCount);
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        auto events = textFixture(roomId, messageCount,
                                  QStringLiteral("end"), QStringLiteral("body"));
        const QString target = events.last().eventId;
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                         events, 0, 600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline != nullptr);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setEventContextEnabled(true);
        double minY = 0.0, maxY = 0.0;
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QVERIFY(timeline->setProperty("stickToBottom", false));
        QVERIFY(timeline->setProperty("contentY", maxY));
        auto *context = controller.eventContext();
        QVERIFY(context->open(roomId, target));
        auto *overlay = root->findChild<QQuickItem *>(QStringLiteral("contextView"));
        auto *composer = root->findChild<QQuickItem *>(QStringLiteral("messageComposer"));
        QVERIFY(overlay != nullptr);
        QVERIFY(composer != nullptr);
        QTRY_VERIFY(overlay->isVisible());
        QVERIFY(composer->isVisible());
        auto *list = overlay->findChild<QQuickItem *>(QStringLiteral("contextList"));
        QVERIFY(list != nullptr);
        QTRY_VERIFY(list->property("count").toInt() > 0);
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QQuickItem *load = nullptr;
        QTRY_VERIFY((load = overlay->findChild<QQuickItem *>(
                         QStringLiteral("contextLoadNewer"))) != nullptr);
        QTRY_VERIFY(load->isVisible() && load->isEnabled());
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                          load->mapToScene(QPointF(load->width() / 2,
                                                  load->height() / 2)).toPoint());
        QTRY_COMPARE(context->state(), ContextController::Closed);
        QVERIFY(!overlay->isVisible());
        QVERIFY(composer->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(timeline->property("stickToBottom").toBool(), 2000);
        QVERIFY(wheelBounds(timeline, &minY, &maxY));
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(timeline->property("contentY").toDouble() - minY) < 1.0, 2000);
        QCOMPARE(mock->eventContextPaginateCount(), 1);
    }

    void contextViewRowActionsTargetTheJumpedToRow()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        auto events = textFixture(roomId, 120, QStringLiteral("act"),
                                  QStringLiteral("body"));
        const QString target = events.at(20).eventId;
        events[20].sender = mock->currentUserId();
        events[20].senderDisplayName = QStringLiteral("Me");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                         events, 0, 600, &timeline);
        QVERIFY(root != nullptr);
        QVERIFY(timeline->property("stickToBottom").toBool());
        mock->setEventContextEnabled(true);
        mock->setEventContextRadius(40);
        auto *context = controller.eventContext();
        QVERIFY(context->open(roomId, target));
        auto *overlay = root->findChild<QQuickItem *>(QStringLiteral("contextView"));
        auto *composer = root->findChild<QQuickItem *>(QStringLiteral("messageComposer"));
        QVERIFY(overlay != nullptr);
        QVERIFY(composer != nullptr);
        auto *list = overlay->findChild<QQuickItem *>(QStringLiteral("contextList"));
        QVERIFY(list != nullptr);
        QTRY_VERIFY(overlay->property("targetPositioned").toBool());
        QTest::qWait(200);
        QVERIFY(composer->isVisible());

        QQuickItem *row = nullptr;
        QVERIFY(QMetaObject::invokeMethod(list, "itemAtIndex",
                                          Q_RETURN_ARG(QQuickItem *, row),
                                          Q_ARG(int, context->targetRow())));
        QVERIFY(row != nullptr);
        const auto liveMenuOwners = [&] {
            QStringList ids;
            for (auto *item : timeline->findChildren<QQuickItem *>(
                     QStringLiteral("messageDelegateRoot"))) {
                const QString id = item->property("menuEventId").toString();
                if (!id.isEmpty())
                    ids << id;
            }
            return ids;
        };
        const auto openMenu = [&] {
            row->setProperty("menuEventId", QString());
            QTest::mouseClick(&window, Qt::RightButton, Qt::NoModifier,
                              row->mapToScene(QPointF(120, row->height() / 2))
                                  .toPoint());
            QTRY_COMPARE(row->property("menuEventId").toString(), target);
            QCOMPARE(liveMenuOwners(), QStringList{});
        };
        const auto trigger = [&](const char *name) {
            auto *item = row->findChild<QObject *>(QLatin1String(name));
            QVERIFY2(item != nullptr, name);
            QVERIFY(item->property("enabled").toBool());
            QVERIFY(QMetaObject::invokeMethod(item, "triggered"));
        };

        openMenu();
        QTest::keyClick(&window, Qt::Key_R);
        QTRY_COMPARE(controller.composer()->replyingToEventId(), target);
        QVERIFY(context->active());
        controller.composer()->cancelReplyOrEdit();

        openMenu();
        trigger("forwardMessageMenuItem");
        QCOMPARE(controller.forward()->sourceEventId(), target);
        controller.forward()->cancel();

        QSignalSpy reactions(mock, &MatrixClient::reactionsChanged);
        context->model()->toggleReaction(target, QStringLiteral("👍"));
        QCOMPARE(reactions.count(), 1);
        QCOMPARE(reactions.at(0).at(0).toString(), roomId);
        QCOMPARE(reactions.at(0).at(1).toString(), target);

        openMenu();
        auto *bulk = row->findChild<QObject *>(
            QStringLiteral("selectMessagesToDeleteMenuItem"));
        QVERIFY(bulk != nullptr);
        QVERIFY(!bulk->property("visible").toBool());

        QSignalSpy redactions(mock, &MatrixClient::redactionFinished);
        trigger("deleteMessageMenuItem");
        QObject *accept = nullptr;
        QTRY_VERIFY((accept = row->findChild<QObject *>(
                         QStringLiteral("messageDestructiveConfirmAccept"))) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(accept, "clicked"));
        QTRY_COMPARE(redactions.count(), 1);
        QCOMPARE(redactions.at(0).at(1).toString(), target);
        QVERIFY(redactions.at(0).at(2).toBool());
        QTRY_VERIFY(context->model()->data(
            context->model()->index(context->targetRow()),
            TimelineModel::RedactedRole).toBool());
    }

    void sendingFromTheContextViewReturnsToTheNewestUnlessReplying()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        const auto events = textFixture(roomId, 120, QStringLiteral("snd"),
                                        QStringLiteral("body"));
        const QString target = events.at(20).eventId;
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                         events, 0, 600, &timeline);
        QVERIFY(root != nullptr);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setEventContextEnabled(true);
        auto *context = controller.eventContext();
        QVERIFY(context->open(roomId, target));
        QTRY_COMPARE(context->state(), ContextController::Ready);

        controller.composer()->beginReply(target, QStringLiteral("Alice"),
                                          QStringLiteral("body 20"), QString(),
                                          QString());
        controller.composer()->setText(QStringLiteral("a reply"));
        controller.composer()->send();
        QTest::qWait(100);
        QVERIFY(context->active());

        controller.composer()->setText(QStringLiteral("a plain message"));
        controller.composer()->send();
        QTRY_COMPARE(context->state(), ContextController::Closed);
    }

    void composerPagingKeysDriveTheContextView()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        const auto events = textFixture(roomId, 120, QStringLiteral("key"),
                                        QStringLiteral("body"));
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                         events, 0, 600, &timeline);
        QVERIFY(root != nullptr);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setEventContextEnabled(true);
        mock->setEventContextRadius(40);
        auto *context = controller.eventContext();
        QVERIFY(context->open(roomId, events.at(60).eventId));
        auto *overlay = root->findChild<QQuickItem *>(QStringLiteral("contextView"));
        auto *composer = root->findChild<QQuickItem *>(QStringLiteral("messageComposer"));
        QVERIFY(overlay != nullptr);
        QVERIFY(composer != nullptr);
        auto *list = overlay->findChild<QQuickItem *>(QStringLiteral("contextList"));
        QVERIFY(list != nullptr);
        QTRY_VERIFY(overlay->property("targetPositioned").toBool());
        QTest::qWait(200);
        const double liveY = timeline->property("contentY").toDouble();
        const double before = list->property("contentY").toDouble();

        QVERIFY(QMetaObject::invokeMethod(composer, "timelineKeyRequested",
                                          Q_ARG(QString, QStringLiteral("timeline.pageUp"))));
        QTRY_VERIFY(list->property("contentY").toDouble() < before - 50.0);
        QCOMPARE(timeline->property("contentY").toDouble(), liveY);

        QVERIFY(QMetaObject::invokeMethod(composer, "timelineKeyRequested",
                                          Q_ARG(QString, QStringLiteral("timeline.jumpToEnd"))));
        QTRY_COMPARE(context->state(), ContextController::Closed);
    }

    void anOlderPageLandingInTheContextViewLeavesTheReaderInPlace_data()
    {
        QTest::addColumn<bool>("readingFirstRow");
        QTest::newRow("mid-list") << false;
        QTest::newRow("first-row") << true;
    }

    void anOlderPageLandingInTheContextViewLeavesTheReaderInPlace()
    {
        QFETCH(bool, readingFirstRow);
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        const auto events = textFixture(roomId, 120, QStringLiteral("ctx"),
                                        QStringLiteral("body"));
        const QString target = events.at(80).eventId;
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                         events, 0, 600, &timeline);
        QVERIFY(root != nullptr);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setEventContextEnabled(true);
        mock->setEventContextRadius(40);
        auto *context = controller.eventContext();
        QVERIFY(context->open(roomId, target));
        auto *overlay = root->findChild<QQuickItem *>(QStringLiteral("contextView"));
        QVERIFY(overlay != nullptr);
        auto *list = overlay->findChild<QQuickItem *>(QStringLiteral("contextList"));
        QVERIFY(list != nullptr);
        QTRY_VERIFY(list->property("count").toInt() > 60);
        QTRY_VERIFY(overlay->property("targetPositioned").toBool());
        QTest::qWait(200);

        const auto itemAt = [&](int row) {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(list, "itemAtIndex",
                                      Q_RETURN_ARG(QQuickItem *, item),
                                      Q_ARG(int, row));
            return item;
        };
        const auto contentY = [&] { return list->property("contentY").toReal(); };
        int readerRow = 0;
        if (readingFirstRow) {
            QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtBeginning"));
        } else {
            const qreal x = list->property("contentX").toReal() + list->width() / 2;
            QVERIFY(QMetaObject::invokeMethod(list, "indexAt",
                                              Q_RETURN_ARG(int, readerRow),
                                              Q_ARG(qreal, x),
                                              Q_ARG(qreal, contentY() + 1)));
            QVERIFY2(readerRow >= 20,
                     qPrintable(QStringLiteral("reader not mid-list: row %1")
                                    .arg(readerRow)));
        }
        QTest::qWait(200);
        QQuickItem *before = itemAt(readerRow);
        QVERIFY(before != nullptr);
        const QString readerId = context->model()->stableIdAt(readerRow);
        const double readerOffset = before->y() - contentY();
        QVERIFY(readerOffset >= -before->height() && readerOffset < list->height());

        // One push_front per event, delivered in one burst, as an SDK page lands.
        const QString contextId = MatrixClient::contextTimelineId(roomId, target);
        constexpr int kPage = 20;
        for (int i = 0; i < kPage; ++i) {
            TimelineEvent e = events.first();
            e.eventId = QStringLiteral("$front%1").arg(i);
            e.itemId = QStringLiteral("uid-front%1").arg(i);
            e.roomId = contextId;
            Q_EMIT mock->eventsPrepended(contextId, { e });
        }
        QTest::qWait(300);

        const int row = context->model()->rowForStableId(readerId);
        QCOMPARE(row, readerRow + kPage);
        QQuickItem *item = itemAt(row);
        QVERIFY2(item != nullptr, "the reader's row was scrolled out of the view");
        const double offset = item->y() - contentY();
        if (readingFirstRow) {
            QVERIFY2(offset >= -1.0 && offset <= readerOffset + 1.0,
                     qPrintable(QStringLiteral("first row %1 px below the top, was %2")
                                    .arg(offset).arg(readerOffset)));
        } else {
            QVERIFY2(qAbs(offset - readerOffset) <= 1.0,
                     qPrintable(QStringLiteral("reader moved %1 px")
                                    .arg(offset - readerOffset)));
        }
    }

    void aContextViewBarStaysDismissedUntilTheNextJump()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        auto events = textFixture(roomId, 40, QStringLiteral("bar"),
                                  QStringLiteral("body"));
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                         events, 0, 600, &timeline);
        QVERIFY(root != nullptr);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setEventContextEnabled(true);
        auto *context = controller.eventContext();
        QVERIFY(context->open(roomId, events.at(10).eventId));
        auto *overlay = root->findChild<QQuickItem *>(QStringLiteral("contextView"));
        QVERIFY(overlay != nullptr);
        QTRY_VERIFY(overlay->isVisible());
        auto *bar = overlay->findChild<QQuickItem *>(QStringLiteral("contextTopBar"));
        auto *dismiss = overlay->findChild<QQuickItem *>(
            QStringLiteral("contextBarDismiss"));
        QVERIFY(bar != nullptr);
        QVERIFY(dismiss != nullptr);
        QTRY_VERIFY(bar->isVisible() && dismiss->width() > 0);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                          dismiss->mapToScene(QPointF(dismiss->width() / 2,
                                                      dismiss->height() / 2)).toPoint());
        QTRY_VERIFY(!bar->isVisible());
        QVERIFY(overlay->isVisible());
        QCoreApplication::processEvents();
        QVERIFY(!bar->isVisible());

        QVERIFY(context->open(roomId, events.at(20).eventId));
        QTRY_VERIFY(bar->isVisible());
    }

    void aContextViewScrollsAsFarPerNotchAsTheTimelineAndLoadsOlderRows()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        auto events = textFixture(roomId, 60, QStringLiteral("wheel"),
                                  QStringLiteral("body"));
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(controller, engine, window, roomId,
                                         events, 0, 400, &timeline);
        QVERIFY(root != nullptr);
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        mock->setEventContextEnabled(true);
        mock->setEventContextRadius(6);
        auto *context = controller.eventContext();
        QVERIFY(context->open(roomId, events.at(30).eventId));
        auto *overlay = root->findChild<QQuickItem *>(QStringLiteral("contextView"));
        QVERIFY(overlay != nullptr);
        QTRY_VERIFY(overlay->isVisible());
        auto *list = overlay->findChild<QQuickItem *>(QStringLiteral("contextList"));
        QVERIFY(list != nullptr);
        QTRY_VERIFY(list->property("count").toInt() > 0);
        QTRY_VERIFY(list->isVisible());
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QCoreApplication::processEvents();
        const double originY = list->property("originY").toDouble();
        const double before = list->property("contentY").toDouble();
        const double extent = before - originY;
        QVERIFY2(extent > 50.0, "the fixture leaves no room to scroll");
        const double per = controller.timelineScroll()->notchDistance(list->height());
        QVERIFY2(per > 40.0, "a notch here is not longer than Qt's default");
        sendWheelNotch(window, list->mapToScene(
            QPointF(list->width() / 2, list->height() / 2)), 120, false);
        QTRY_VERIFY2(before - list->property("contentY").toDouble()
                         >= qMin(per, extent) - 1.0,
                     "a notch moved the context view less than the timeline's");
        QTRY_VERIFY2(mock->eventContextPaginateCount() >= 1,
                     "scrolling towards the top asked for no older rows");
    }

    void aPendingInviteShowsTheInviteCardInsteadOfTheComposer()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString roomId = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *timeline = nullptr;
        QQuickItem *root = paneWithEvents(
            controller, engine, window, roomId,
            textFixture(roomId, 20, QStringLiteral("iv"), QStringLiteral("body")),
0,600, &timeline);
        QVERIFY(root != nullptr);
        auto *card = root->findChild<QQuickItem *>(
            QStringLiteral("invitePreviewCard"));
        auto *composer = root->findChild<QQuickItem *>(
            QStringLiteral("messageComposer"));
        QVERIFY2(card != nullptr, "there is no invite card");
        QVERIFY(composer != nullptr);
        QVERIFY2(!card->isVisible(), "a joined room shows the invite card");
        QVERIFY(composer->isVisible());

        QVariantMap room = root->property("currentRoom").toMap();
        room.insert(QStringLiteral("membership"), QStringLiteral("invited"));
        room.insert(QStringLiteral("inviter"), QStringLiteral("Sam <b>x</b>"));
        QVERIFY(root->setProperty("currentRoom", room));
        QCoreApplication::processEvents();
        QVERIFY(root->property("invitePreview").toBool());
        QVERIFY2(card->isVisible(), "an invite shows no invite card");
        QVERIFY2(!composer->isVisible(),
                 "an invite still offers a composer that cannot send");

        auto *inviter = root->findChild<QQuickItem *>(
            QStringLiteral("invitePreviewInviter"));
        auto *accept = root->findChild<QQuickItem *>(
            QStringLiteral("invitePreviewAccept"));
        auto *decline = root->findChild<QQuickItem *>(
            QStringLiteral("invitePreviewDecline"));
        QVERIFY(inviter != nullptr);
        QVERIFY(accept != nullptr);
        QVERIFY(decline != nullptr);
        QCOMPARE(inviter->property("text").toString(),
                 QStringLiteral("Sam <b>x</b> invited you to join"));
        QCOMPARE(inviter->property("textFormat").toInt(),
                 static_cast<int>(Qt::PlainText));
        QVERIFY(accept->isVisible() && accept->isEnabled());
        QVERIFY(decline->isVisible() && decline->isEnabled());

        room.insert(QStringLiteral("invitePending"), true);
        QVERIFY(root->setProperty("currentRoom", room));
        QCoreApplication::processEvents();
        QVERIFY(!accept->isEnabled());
        QVERIFY(!decline->isEnabled());

        room.insert(QStringLiteral("membership"), QStringLiteral("joined"));
        room.insert(QStringLiteral("invitePending"), false);
        QVERIFY(root->setProperty("currentRoom", room));
        QCoreApplication::processEvents();
        QVERIFY(!card->isVisible());
        QVERIFY(composer->isVisible());
    }

    static QQuickItem *spaceHomeRoot(AppController &controller,
                                     QQmlApplicationEngine &engine,
                                     QQuickWindow &window,
                                     const QString &spaceId)
    {
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return nullptr;
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!root)
            return nullptr;
        window.resize(900, 700);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        (void)QTest::qWaitForWindowExposed(&window, 2000);
        controller.openSpaceHome(spaceId);
        if (!QTest::qWaitFor([&] {
                auto *pane = root->findChild<QQuickItem *>(
                    QStringLiteral("spaceHomePane"));
                return pane && pane->isVisible();
            }, 3000))
            return nullptr;
        return root;
    }

    void spaceHomeOffersNoChildPlacementWithoutThePower()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString spaceId = QStringLiteral("!space-team:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *root = spaceHomeRoot(controller, engine, window, spaceId);
        QVERIFY2(root != nullptr, "Space Home did not open");
        // Fixture: the mock's member snapshot grants no child management.
        QVERIFY(!controller.roomInfo()->property("canManageSpaceChildren")
                     .toBool());
        auto *add = root->findChild<QQuickItem *>(
            QStringLiteral("spaceAddRoomButton"));
        auto *create = root->findChild<QQuickItem *>(
            QStringLiteral("spaceCreateRoomButton"));
        QVERIFY(add != nullptr);
        QVERIFY(create != nullptr);
        QTest::qWait(50);
        QVERIFY2(!add->isVisible(),
                 "Add existing room is offered without the power to add a "
                 "child");
        QVERIFY2(!create->isVisible(),
                 "Create room here is offered without the power to place the "
                 "room");
    }

    void spaceHomeClearsTheWaitingNoticeOnceTheChildIsListed()
    {
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString spaceId = QStringLiteral("!space-team:mock.local");
        const QString child = QStringLiteral("!general:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *root = spaceHomeRoot(controller, engine, window, spaceId);
        QVERIFY2(root != nullptr, "Space Home did not open");
        auto *notice = root->findChild<QQuickItem *>(
            QStringLiteral("spaceHomeNotice"));
        QVERIFY(notice != nullptr);
        QVERIFY(!notice->isVisible());
        QVERIFY2(controller.spaces()->includesRoom(spaceId, child),
                 "fixture: the Space does not list the room");

        controller.spaces()->addRoomToSpace(spaceId, child);
        QCoreApplication::processEvents();
        QVERIFY2(!notice->isVisible(),
                 qPrintable(QStringLiteral("the Space lists the room and the "
                                           "notice still reads \"%1\"")
                                .arg(notice->property("text").toString())));

        QVERIFY(QMetaObject::invokeMethod(
            controller.spaces(), "childAddFinished", Q_ARG(QString, spaceId),
            Q_ARG(QString, QStringLiteral("!nope:mock.local")),
            Q_ARG(bool, false)));
        QCoreApplication::processEvents();
        QVERIFY(notice->isVisible());
        QCOMPARE(notice->property("text").toString(),
                 QStringLiteral("The room could not be added to this Space."));
    }

    void leavingSpaceHomeLeavesNoQueuedCallIntoIt()
    {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("is not a function|invalid context")));
        AppController controller(AppController::MockBackend);
        QVERIFY(!loginAndRoomIdAt(controller,0).isEmpty());
        const QString spaceId = QStringLiteral("!space-team:mock.local");
        QQmlApplicationEngine engine;
        QQuickWindow window;
        QQuickItem *root = spaceHomeRoot(controller, engine, window, spaceId);
        QVERIFY2(root != nullptr, "Space Home did not open");

        QVERIFY(QMetaObject::invokeMethod(controller.discovery(),
                                          "spaceChildrenChanged",
                                          Q_ARG(QString, spaceId)));
        controller.setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QTest::qWait(150);
        QTRY_VERIFY_WITH_TIMEOUT(
            root->findChild<QQuickItem *>(QStringLiteral("spaceHomePane"))
                == nullptr, 2000);
    }
    void signingOutLeavesNoQueuedFollowLatestCall()
    {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("is not a function|invalid context|"
                           "delayed function evaluation")));
        AppController controller(AppController::MockBackend);
        const QString room = loginAndRoomIdAt(controller,0);
        QVERIFY(!room.isEmpty());
        QQmlApplicationEngine engine;
        QQuickWindow window;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("TimelinePane"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            QFAIL("pane did not load");
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root);
        window.resize(900, 700);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(window.width(), window.height()));
        window.show();
        controller.setCurrentRoomId(room);
        QTRY_VERIFY_WITH_TIMEOUT(controller.timeline()->rowCount() > 0, 3000);
        QTest::qWait(300);

        controller.setCurrentRoomId(QString());
        controller.setCurrentRoomId(room);
        delete root;
        QTest::qWait(200);
    }

    void theCallStageFitsTheNarrowPane_data()
    {
        QTest::addColumn<int>("width");
        QTest::newRow("narrower than the controls") << 260;
        QTest::newRow("640 window pane") << 320;
        QTest::newRow("760 window pane") << 380;
    }

    void theCallStageFitsTheNarrowPane()
    {
        QFETCH(int, width);
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        QQuickWindow window;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("CallStage"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            QFAIL("call stage did not load");
        auto *stage = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(stage);
        window.resize(width, 420);
        stage->setParentItem(window.contentItem());
        stage->setSize(QSizeF(width, 400));
        window.show();
        auto *controls = stage->findChild<QQuickItem *>(
            QStringLiteral("callStageControls"));
        QVERIFY2(controls, "the call controls were not loaded");
        QVERIFY(controls->setProperty("previewMode", true));
        QTest::qWait(300);
        auto *row = stage->findChild<QQuickItem *>(
            QStringLiteral("callHeaderRow"));
        QVERIFY(row);
        const QPointF end = row->mapToItem(stage, QPointF(row->width(), 0));
        QVERIFY2(end.x() <= width + 0.5,
                 qPrintable(QStringLiteral("header row ends at %1 in a %2 px stage")
                                .arg(end.x()).arg(width)));
        const auto kids = row->parentItem()->childItems();
        for (QQuickItem *kid : kids) {
            if (!kid->isVisible() || kid->width() <= 0)
                continue;
            const QPointF e = kid->mapToItem(stage, QPointF(kid->width(), 0));
            QVERIFY2(e.x() <= width + 0.5,
                     qPrintable(QStringLiteral("%1 ends at %2 in a %3 px stage")
                                    .arg(kid->objectName()).arg(e.x()).arg(width)));
        }
    }

};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    TimelinePaneQmlTest testObject;
    return QTest::qExec(&testObject, argc, argv);
}

#include "TimelinePaneQmlTest.moc"
