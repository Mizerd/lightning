// Small windows: users keep Lightning in a quarter of the screen. The real
// production window (Main.qml on the mock backend) is driven at the sizes a
// quarter screen gives, and every control that matters must lie inside the
// surface that is meant to hold it:
//
//   * each Settings section's controls stay inside the content viewport
//     horizontally (a section may scroll DOWN, never sideways);
//   * the pickers that anchor to the composer (GIF, emoji, stickers) keep
//     every control inside their own panel, and the panel inside the window;
//   * the window's own floor (Main.qml minimumWidth/minimumHeight) is the
//     size those cases are measured at.
//
// Offscreen geometry only: it is not a physical GUI pass.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QScopedPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "calls/CallController.h"
#include "calls/CallMediaBackend.h"
#include "calls/CallParticipantModel.h"
#include "calls/CallStageState.h"
#include "calls/OneToOneDm.h"
#include "calls/SfuCallController.h"
#include "matrix/MockMatrixClient.h"

namespace {
constexpr int kSignalTimeoutMs = 5000;

bool isControl(const QByteArray &cls)
{
    return cls.contains("Button") || cls.contains("TextField")
        || cls.contains("ComboBox") || cls.contains("CheckBox")
        || cls.contains("Switch") || cls.contains("Slider")
        || cls.contains("TextArea");
}
} // namespace

class SmallWindowQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    AppController *m_controller = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;

    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        if (parent->objectName() == name)
            return parent;
        for (QQuickItem *child : parent->childItems())
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        return nullptr;
    }

    QQuickItem *item(const char *name) const
    {
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    // Every visible control under `it` whose box, in `space`'s coordinates,
    // leaves [0, space.width] horizontally. Plain clips are NOT applied on
    // purpose (the point is what a clip would be hiding); a nested scroller is.
    static void controlsOutsideHorizontally(QQuickItem *it, QQuickItem *space,
                                            QStringList &out)
    {
        if (!it->isVisible() || it->opacity() <= 0.01)
            return;
        const QByteArray cls = it->metaObject()->className();
        if (isControl(cls) && it->width() > 0 && it->height() > 0) {
            QRectF r = space->mapRectFromItem(
                it, QRectF(0, 0, it->width(), it->height()));
            // A control inside a nested scroller (the emoji category rail) is
            // reached by scrolling, so only what that scroller shows counts.
            for (QQuickItem *a = it->parentItem(); a && a != space;
                 a = a->parentItem())
                if (a->inherits("QQuickFlickable"))
                    r = r.intersected(space->mapRectFromItem(
                        a, QRectF(0, 0, a->width(), a->height())));
            if (r.isEmpty())
                return;
            if (r.left() < -1.0 || r.right() > space->width() + 1.0)
                out << QStringLiteral("%1 %2 [%3..%4 of %5]")
                           .arg(QString::fromLatin1(cls), it->objectName())
                           .arg(qRound(r.left())).arg(qRound(r.right()))
                           .arg(qRound(space->width()));
        }
        for (QQuickItem *c : it->childItems())
            controlsOutsideHorizontally(c, space, out);
    }

    // Every visible control under `it` (after what a nested scroller shows)
    // that leaves `box`, in scene coordinates. Both axes: a dialog whose body
    // is not scrollable loses its bottom controls past the box.
    static void controlsOutsideBox(QQuickItem *it, const QRectF &box,
                                   QStringList &out)
    {
        if (!it->isVisible() || it->opacity() <= 0.01)
            return;
        const QByteArray cls = it->metaObject()->className();
        if (isControl(cls) && it->width() > 0 && it->height() > 0) {
            QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
            for (QQuickItem *a = it->parentItem(); a; a = a->parentItem())
                if (a->inherits("QQuickFlickable"))
                    r = r.intersected(a->mapRectToScene(
                        QRectF(0, 0, a->width(), a->height())));
            if (!r.isEmpty() && !box.adjusted(-1, -1, 1, 1).contains(r))
                out << QStringLiteral("%1 %2 [%3,%4 %5x%6]")
                           .arg(QString::fromLatin1(cls), it->objectName())
                           .arg(qRound(r.x())).arg(qRound(r.y()))
                           .arg(qRound(r.width())).arg(qRound(r.height()));
        }
        for (QQuickItem *c : it->childItems())
            controlsOutsideBox(c, box, out);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("small-window-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlApplicationEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QSignalSpy createdSpy(m_engine, &QQmlApplicationEngine::objectCreated);
        m_engine->loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Main"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        m_window = qobject_cast<QQuickWindow *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(m_window);
        m_window->setWidth(640);
        m_window->setHeight(360);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));

        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
        QCoreApplication::processEvents();
    }

    void cleanupTestCase()
    {
        delete m_engine;
        delete m_controller;
    }

    // The application's floor is what this file measures against.
    void theWindowFloorIsTheSizeTheCasesAssume()
    {
        QCOMPARE(m_window->minimumWidth(), 640);
        QCOMPARE(m_window->minimumHeight(), 360);
    }

    void everySettingsSectionStaysInsideItsViewportAtTheFloor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x360") << QSize(640, 360);
        QTest::newRow("854x480") << QSize(854, 480);
    }

    void everySettingsSectionStaysInsideItsViewportAtTheFloor()
    {
        QFETCH(QSize, size);
        m_window->resize(size);
        m_controller->showSettings();
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QStringList failures;
        const QStringList sections = {
            QStringLiteral("account"),       QStringLiteral("privacy"),
            QStringLiteral("sessions"),      QStringLiteral("appearance"),
            QStringLiteral("layout"),        QStringLiteral("chat"),
            QStringLiteral("notifications"), QStringLiteral("shortcuts"),
            QStringLiteral("sound"),         QStringLiteral("labs"),
            QStringLiteral("updates"),       QStringLiteral("about") };
        int measured = 0;
        for (const QString &section : sections) {
            m_controller->showSettingsSection(section);
            QCoreApplication::processEvents();
            QTest::qWait(250);
            QQuickItem *flick = item("settingsContentFlick");
            QVERIFY2(flick, "settings content viewport not found");
            auto *content = flick->property("contentItem").value<QQuickItem *>();
            QVERIFY(content);
            QStringList outside;
            controlsOutsideHorizontally(content, flick, outside);
            ++measured;
            for (const QString &o : outside)
                failures << section + QLatin1String(": ") + o;
        }
        QCOMPARE(measured, sections.size());
        QVERIFY2(failures.isEmpty(),
                 qPrintable(QStringLiteral("%1x%2 -> ").arg(size.width())
                                .arg(size.height())
                            + failures.join(QStringLiteral("; "))));
        m_controller->showMain();
        QCoreApplication::processEvents();
    }

    // The composer's pickers are anchored to a card that is only ~300 px wide
    // at the floor. Their panel may be narrower than their content wants; the
    // content must adapt, not run past the panel (the GIF picker's tabs and
    // its third column of results hung off the window edge).
    void thePickersKeepEveryControlInsideTheirPanelAndTheWindow_data()
    {
        QTest::addColumn<QString>("opener");
        QTest::addColumn<QString>("popupName");
        QTest::newRow("gif") << "openGifPicker" << "composerGifPicker";
        QTest::newRow("emoji") << "openEmojiPicker" << "composerEmojiPicker";
        QTest::newRow("sticker") << "openStickerPicker" << "composerStickerPicker";
    }

    void thePickersKeepEveryControlInsideTheirPanelAndTheWindow()
    {
        QFETCH(QString, opener);
        QFETCH(QString, popupName);
        m_controller->showMain();
        m_window->resize(640, 360);
        QCoreApplication::processEvents();
        QTest::qWait(200);
        QQuickItem *composer = item("messageComposer");
        QVERIFY2(composer, "composer not found");
        if (opener == QLatin1String("openEmojiPicker"))
            QVERIFY(QMetaObject::invokeMethod(composer, "openEmojiPicker",
                                              Q_ARG(QVariant, QVariant(false))));
        else
            QVERIFY(QMetaObject::invokeMethod(composer, opener.toLatin1()));
        QTest::qWait(400);
        QObject *popup = m_window->findChild<QObject *>(popupName);
        QVERIFY2(popup, qPrintable(popupName + QLatin1String(" not found")));
        QTRY_VERIFY2(popup->property("opened").toBool(), "picker did not open");
        auto *panel = popup->property("contentItem").value<QQuickItem *>();
        QVERIFY(panel);
        QTest::qWait(300);
        // The panel inside the window.
        const QRectF pr = panel->mapRectToScene(
            QRectF(0, 0, panel->width(), panel->height()));
        QVERIFY2(QRectF(0, 0, m_window->width(), m_window->height())
                     .adjusted(-1, -1, 1, 1).contains(pr),
                 qPrintable(popupName + QStringLiteral(" panel is outside the "
                            "window: %1,%2 %3x%4").arg(pr.x()).arg(pr.y())
                            .arg(pr.width()).arg(pr.height())));
        QStringList outside;
        controlsOutsideHorizontally(panel, panel, outside);
        QVERIFY2(outside.isEmpty(),
                 qPrintable(popupName + QLatin1String(": ")
                            + outside.join(QStringLiteral("; "))));
        QMetaObject::invokeMethod(popup, "close");
        QTest::qWait(100);
    }

    // During a call the stage takes a share of the column. In a short window
    // that share must leave room for the composer: the largest panel the pane
    // will ever request still keeps header + call controls + composer + a
    // sliver of timeline inside the pane (the composer used to be pushed off
    // the bottom of the window).
    void theCallPanelNeverPushesTheComposerOutOfAShortWindow()
    {
        m_controller->showMain();
        m_window->resize(640, 360);
        QCoreApplication::processEvents();
        QTest::qWait(200);
        QQuickItem *pane = item("timelinePane");
        QQuickItem *header = item("roomHeaderBand");
        QQuickItem *callBar = item("timelineCallHeaderBar");
        QQuickItem *composer = item("messageComposer");
        QVERIFY(pane && header && callBar && composer);
        QVariant tallest;
        QVERIFY(QMetaObject::invokeMethod(pane, "clampCallPanelHeight",
                                          Q_RETURN_ARG(QVariant, tallest),
                                          Q_ARG(QVariant, QVariant(100000))));
        const qreal used = header->height() + callBar->height() + 9
                           + tallest.toReal() + composer->height();
        QVERIFY2(used <= pane->height(),
                 qPrintable(QStringLiteral("header %1 + call bar %2 + panel %3 "
                                           "+ composer %4 = %5 > pane %6")
                                .arg(header->height()).arg(callBar->height())
                                .arg(tallest.toReal()).arg(composer->height())
                                .arg(used).arg(pane->height())));
    }

    // The stage's header row holds the call controls, and beside a spotlight
    // the participant faces too. Neither cell's rect shows the fault on its
    // own: the row squeezes the controls' CELL and the bar inside it keeps its
    // width, so what is measured is the bar's own buttons. The faces must still
    // be drawn somewhere.
    void theCallControlsNeverDrawOverTheParticipantFaces_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("collapsed");
        QTest::newRow("640x720") << QSize(640, 720) << false;
        QTest::newRow("640x360") << QSize(640, 360) << false;
        QTest::newRow("854x480") << QSize(854, 480) << false;
        QTest::newRow("1280x720") << QSize(1280, 720) << false;
        QTest::newRow("960x540") << QSize(960, 540) << false;
        QTest::newRow("1000x600") << QSize(1000, 600) << false;
        QTest::newRow("1000x600 collapsed") << QSize(1000, 600) << true;
        QTest::newRow("640x720 collapsed") << QSize(640, 720) << true;
    }

    void theCallControlsNeverDrawOverTheParticipantFaces()
    {
#ifndef HAVE_LIGHTNING_WEBRTC
        QSKIP("built without the SFU media engine");
#else
        QFETCH(QSize, size);
        QFETCH(bool, collapsed);
        const QString room = QStringLiteral("!general:mock.local");
        m_controller->showMain();
        m_controller->setCurrentRoomId(room);
        m_window->resize(size);
        SfuCallController *call = m_controller->groupCall();
        call->setMembershipForTest(room, QString());
        call->setCallStateForTest(SfuCallController::State::Connected);
        Q_EMIT call->stateChanged();
        QVariantList people;
        for (int i = 0; i < 6; ++i) {
            QVariantMap person;
            person.insert(QStringLiteral("identity"),
                          QStringLiteral("person%1").arg(i));
            person.insert(QStringLiteral("sid"), QStringLiteral("PA_%1").arg(i));
            if (i == 0) {
                QVariantMap track;
                track.insert(QStringLiteral("source"),
                             QStringLiteral("screen_share"));
                track.insert(QStringLiteral("sid"), QStringLiteral("TR_share"));
                track.insert(QStringLiteral("muted"), false);
                person.insert(QStringLiteral("tracks"), QVariantList{track});
            }
            people << person;
        }
        call->ingestParticipantsForTest(people);
        call->stageState()->restoreShare(QStringLiteral("TR_share"));
        // Straight into the model: these people have no Matrix membership, so
        // the controller's own rebuild keeps only the local row.
        QList<CallParticipantRow> rows;
        for (int i = 0; i < 6; ++i) {
            CallParticipantRow person;
            person.identity = QStringLiteral("person%1").arg(i);
            person.sid = QStringLiteral("PA_%1").arg(i);
            person.displayName = person.identity;
            rows << person;
        }
        call->participantModel()->applyParticipants(rows);
        QTRY_VERIFY(item("callHeaderRow") != nullptr);
        QQuickItem *stage = item("timelineCallStage");
        QVERIFY(stage);
        QObject *pane = item("timelinePane");
        QVERIFY(pane);
        pane->setProperty("callPanelCollapsed", collapsed);
        QTest::qWait(250);

        QQuickItem *row = item("callHeaderRow");
        QQuickItem *controls = item("callStageControls");
        QQuickItem *bar = findItem(controls, QStringLiteral("callHeaderBarRow"));
        QVERIFY(row && controls && bar);
        QVERIFY2(collapsed
                     || stage->property("effectiveLayout").toString()
                            == QLatin1String("spotlight"),
                 "no spotlight, so the faces are not in play");
        const auto shown = [this](const char *name) {
            QQuickItem *it = item(name);
            // All it asked for: a host squeezed down shows one face of six.
            return it && it->isVisible() && it->width() > 0
                && it->width() + 1 >= it->implicitWidth();
        };
        const bool facesDrawn = shown("callHeaderBubblesHost")
            || shown("callStrip") || shown("callStripBubblesHost");
        // The stage, not the row: a row wider than its stage reports nothing.
        const QRectF rowBox = stage->mapRectToScene(
            QRectF(0, 0, stage->width(), stage->height()));
        QStringList faults;
        const auto box = [](QQuickItem *it) {
            return it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
        };
        QList<QPair<QString, QRectF>> cells;
        for (QQuickItem *c : row->childItems()) {
            if (!c->isVisible() || c->width() <= 0 || c->height() <= 0
                || c == controls->parentItem())
                continue;
            const QString name = c->objectName().isEmpty()
                ? QString::fromLatin1(c->metaObject()->className())
                : c->objectName();
            cells.append({name, box(c)});
        }
        // What the controls really occupy, not the cell they were given.
        for (QQuickItem *c : bar->childItems())
            if (c->isVisible() && c->width() > 0 && c->height() > 0) {
                const QRectF r = box(c);
                cells.append({c->objectName().isEmpty()
                                  ? QStringLiteral("control")
                                  : c->objectName(), r});
            }
        for (int i = 0; i < cells.size(); ++i) {
            const QRectF a = cells.at(i).second;
            if (a.left() < rowBox.left() - 1 || a.right() > rowBox.right() + 1)
                faults << QStringLiteral("%1 [%2..%3] leaves the stage [%4..%5]")
                              .arg(cells.at(i).first).arg(a.left())
                              .arg(a.right()).arg(rowBox.left())
                              .arg(rowBox.right());
            for (int j = i + 1; j < cells.size(); ++j) {
                const QRectF overlap = a.intersected(cells.at(j).second);
                if (overlap.width() > 1 && overlap.height() > 1)
                    faults << QStringLiteral("%1 overlaps %2 by %3 px")
                                  .arg(cells.at(i).first, cells.at(j).first)
                                  .arg(overlap.width());
            }
        }
        pane->setProperty("callPanelCollapsed", false);
        call->setCallStateForTest(SfuCallController::State::Idle);
        Q_EMIT call->stateChanged();
        QVERIFY2(faults.isEmpty(), qPrintable(faults.join(QStringLiteral("; "))));
        // Collapsed, the narrowest strip has room for the controls alone;
        // expanding it shows everyone.
        QVERIFY2(collapsed || facesDrawn, "the faces went nowhere");
#endif
    }

    // The chevron beside the call button is the only way to a video call and
    // the call type; it folds out of a narrow header with that button.
    void theCallTypeMenuIsReachableWhenTheCallButtonFolds()
    {
        // The mock has no call lane until a backend exists; then a 1:1 DM has.
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
            bool supportsVideo() const override { return true; }
        };
        Engine engine;
        m_controller->calls()->setMediaBackend(&engine);
        const auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        QString dm;
        for (const RoomInfo &room : mock->rooms())
            if (!calls::oneToOneDmPeer(room, true).isEmpty()) {
                dm = room.id;
                break;
            }
        QVERIFY(!dm.isEmpty());
        m_controller->showMain();
        m_controller->setCurrentRoomId(dm);
        m_window->resize(640, 360);
        QTest::qWait(150);
        QObject *button = m_window->findChild<QObject *>(
            QStringLiteral("startVoiceCallButton"));
        QVERIFY(button);
        QVERIFY2(button->property("available").toBool(),
                 "this room offers no call, so nothing can fold");
        QVERIFY2(button->property("folded").toBool(),
                 "the floor did not fold the call button: nothing measured");
        QQuickItem *overflow = item("roomHeaderOverflowButton");
        QVERIFY(overflow);
        QObject *menu = m_window->findChild<QObject *>(
            QStringLiteral("roomHeaderOverflowMenu"));
        QVERIFY(menu);
        QMetaObject::invokeMethod(menu, "open");
        QTRY_VERIFY(menu->property("opened").toBool());
        QObject *row =
            menu->findChild<QObject *>(QStringLiteral("overflowCallOptions"));
        QVERIFY(row);
        QVERIFY(row->property("visible").toBool());
        QMetaObject::invokeMethod(row, "triggered");
        QMetaObject::invokeMethod(menu, "close");
        QObject *format = m_window->findChild<QObject *>(
            QStringLiteral("callFormatMenu"));
        QVERIFY(format);
        QTRY_VERIFY(format->property("opened").toBool());
        const qreal x = format->property("x").toReal();
        const qreal width = format->property("width").toReal();
        const QRectF placed = overflow->mapRectToScene(QRectF(
            x, format->property("y").toReal(), width,
            format->property("height").toReal()));
        QVERIFY2(placed.left() >= 0 && placed.right() <= m_window->width()
                     && placed.top() >= 0
                     && placed.bottom() <= m_window->height(),
                 qPrintable(QStringLiteral("the call menu sits at %1,%2 %3x%4")
                                .arg(placed.x()).arg(placed.y())
                                .arg(placed.width()).arg(placed.height())));
        QMetaObject::invokeMethod(format, "close");
        QTRY_VERIFY(!format->property("visible").toBool());
        m_controller->calls()->setMediaBackend(nullptr);
        m_controller->setCurrentRoomId(QStringLiteral("!general:mock.local"));
    }

    // The find bar was one row of seven controls in a card 288 px wide at the
    // floor: the field was squeezed away and Close sat outside the card.
    void theFindBarKeepsEveryControlInsideItsCardAtTheFloor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x360") << QSize(640, 360);
        QTest::newRow("854x480") << QSize(854, 480);
    }

    void theFindBarKeepsEveryControlInsideItsCardAtTheFloor()
    {
        QFETCH(QSize, size);
        m_controller->showMain();
        m_window->resize(size);
        QCoreApplication::processEvents();
        QTest::qWait(200);
        QQuickItem *pane = item("timelinePane");
        QVERIFY(pane);
        QVERIFY(QMetaObject::invokeMethod(pane, "openFind"));
        QTest::qWait(300);
        QQuickItem *bar = item("timelineFindBar");
        QVERIFY(bar && bar->isVisible());
        QStringList outside;
        controlsOutsideHorizontally(bar, bar, outside);
        QVERIFY2(outside.isEmpty(), qPrintable(outside.join(QStringLiteral("; "))));
        QQuickItem *field = item("timelineFindField");
        QVERIFY(field);
        QVERIFY2(field->width() >= 100,
                 qPrintable(QStringLiteral("the find field is %1 px wide")
                                .arg(field->width())));
        QMetaObject::invokeMethod(pane, "closeFind");
        QTest::qWait(100);
    }

    // Every main dialog, opened in the floor-sized window: its box must be
    // inside the window. A dialog taller than the window loses its footer
    // buttons below the edge, where nothing can reach them.
    void everyMainDialogFitsTheWindowAtTheFloor_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("open");
        const auto row = [](const char *type, const char *open) {
            QTest::newRow(type) << QString::fromLatin1(type)
                                << QString::fromLatin1(open);
        };
        row("JumpToDateDialog", "openDialog()");
        row("SendLaterDialog", "openFor({})");
        row("LeaveRoomConfirmDialog",
            "openFor('!r:mock.local', 'Room', 12, 'public', false)");
        row("ExportRoomDialog", "openDialog()");
        row("DiscoverJoinDialog", "openDialog('browse')");
        row("ForwardSelectionDialog", "openDialog()");
        row("QrLoginDialog", "openDialog()");
        row("InvitePeopleDialog", "openFor('!general:mock.local')");
        row("CreatePollDialog", "openDialog()");
        row("NewConversationDialog", "openDialog('room', {})");
        row("RoomUpgradeDialog", "openFor('!general:mock.local')");
        row("SpaceSettingsDialog", "openFor('!space:mock.local')");
        row("AddWidgetDialog", "openDialog()");
        row("MessageSearchDialog", "openDialog('')");
        row("StatusDialog", "openForEdit()");
        row("ChatBackgroundDialog", "openForDefault()");
        row("PolicyListDialog", "openFor('!r:mock.local', 'Room')");
    }

    void everyMainDialogFitsTheWindowAtTheFloor()
    {
        QFETCH(QString, type);
        QFETCH(QString, open);
        m_controller->showMain();
        m_window->resize(640, 360);
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QQmlComponent component(m_engine);
        component.setData(QStringLiteral(
            "import QtQuick\nimport QtQuick.Controls\nimport MatrixClient\n"
            "Item { anchors.fill: parent\n  %1 { id: dlg; objectName: 'dlg' }\n}\n")
                .arg(type).toUtf8(), QUrl(QStringLiteral("dialog-under-test.qml")));
        QScopedPointer<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *host = qobject_cast<QQuickItem *>(owner.data());
        QVERIFY(host);
        host->setParentItem(m_window->contentItem());
        host->setWidth(m_window->width());
        host->setHeight(m_window->height());
        QQmlExpression expr(qmlContext(host), host,
                            QStringLiteral("dlg.%1").arg(open));
        expr.evaluate();
        QVERIFY2(!expr.hasError(), qPrintable(expr.error().toString()));
        QObject *dlg = host->findChild<QObject *>(QStringLiteral("dlg"));
        QVERIFY(dlg);
        QTRY_VERIFY_WITH_TIMEOUT(dlg->property("opened").toBool(), 1500);
        QTest::qWait(350);
        const qreal x = dlg->property("x").toReal();
        const qreal y = dlg->property("y").toReal();
        const qreal w = dlg->property("width").toReal();
        const qreal h = dlg->property("height").toReal();
        const QRectF box(x, y, w, h);
        const QRectF win(0, 0, m_window->width(), m_window->height());
        QStringList stray;
        for (const char *part : { "contentItem", "footer", "header" }) {
            auto *it = dlg->property(part).value<QQuickItem *>();
            if (it)
                controlsOutsideBox(it, QRectF(x, y, w, h), stray);
        }
        QMetaObject::invokeMethod(dlg, "close");
        QTest::qWait(150);
        QVERIFY2(stray.isEmpty(),
                 qPrintable(type + QLatin1String(" has controls outside its "
                            "box: ") + stray.join(QStringLiteral("; "))));
        QVERIFY2(win.adjusted(-1, -1, 1, 1).contains(box),
                 qPrintable(QStringLiteral("%1 is %2,%3 %4x%5 in a %6x%7 window")
                                .arg(type).arg(x).arg(y).arg(w).arg(h)
                                .arg(win.width()).arg(win.height())));
    }

    // The image viewer's own chrome (toolbar, arrows, close, thumbnail strip)
    // in a window a quarter of a screen: every control inside the window.
    void theImageViewerChromeStaysInsideTheWindowAtTheFloor()
    {
        m_controller->showMain();
        m_window->resize(640, 360);
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QObject *viewer = m_window->findChild<QObject *>(
            QStringLiteral("imageViewerOverlay"));
        QVERIFY2(viewer, "image viewer not found");
        QVariantList entries;
        for (int i = 0; i < 6; ++i) {
            QVariantMap e;
            e.insert(QStringLiteral("row"), i);
            e.insert(QStringLiteral("mediaKey"), QStringLiteral("mock-%1").arg(i));
            e.insert(QStringLiteral("filename"), QStringLiteral("photo-%1.png").arg(i));
            e.insert(QStringLiteral("sender"), QStringLiteral("Alice"));
            e.insert(QStringLiteral("mime"), QStringLiteral("image/png"));
            e.insert(QStringLiteral("httpUrl"), QString());
            e.insert(QStringLiteral("isImage"), true);
            e.insert(QStringLiteral("isVideo"), false);
            e.insert(QStringLiteral("isVisual"), true);
            e.insert(QStringLiteral("thumbAvailable"), false);
            e.insert(QStringLiteral("size"), 1234);
            e.insert(QStringLiteral("timestamp"), 1.7e12 + i);
            entries.append(e);
        }
        QVERIFY(QMetaObject::invokeMethod(viewer, "openAt",
                                          Q_ARG(QVariant, entries),
                                          Q_ARG(QVariant, 2)));
        QTest::qWait(500);
        // Not vacuous: the chrome under test must really be on screen.
        for (const char *name : { "viewerCloseButton", "viewerToolbar" }) {
            QQuickItem *chrome = item(name);
            QVERIFY2(chrome && chrome->isVisible() && chrome->width() > 0,
                     name);
        }
        QStringList stray;
        controlsOutsideBox(m_window->contentItem(),
                           QRectF(0, 0, m_window->width(), m_window->height()),
                           stray);
        // Only the viewer's controls are of interest here.
        QStringList viewerOnly;
        for (const QString &s : stray)
            if (s.contains(QLatin1String("viewer"), Qt::CaseInsensitive))
                viewerOnly << s;
        QMetaObject::invokeMethod(viewer, "close");
        QTest::qWait(150);
        QVERIFY2(viewerOnly.isEmpty(), qPrintable(viewerOnly.join(QStringLiteral("; "))));
    }

    // Room Information beside the room, or over it when the pane is too
    // narrow for both: either way its controls sit inside the window.
    void theRoomInfoPanelStaysInsideTheWindowAtTheFloor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x360") << QSize(640, 360);
        QTest::newRow("854x480") << QSize(854, 480);
        QTest::newRow("960x540") << QSize(960, 540);
    }

    void theRoomInfoPanelStaysInsideTheWindowAtTheFloor()
    {
        QFETCH(QSize, size);
        m_controller->showMain();
        m_window->resize(size);
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QQuickItem *pane = item("timelinePane");
        QVERIFY(pane);
        QVERIFY(pane->setProperty("infoOpen", true));
        QTest::qWait(500);
        QStringList stray;
        controlsOutsideBox(m_window->contentItem(),
                           QRectF(0, 0, m_window->width(), m_window->height()),
                           stray);
        // Opening Room Information must always show it, beside the room or
        // (in a pane too narrow for both) over it. It used to vanish below a
        // 700 px pane: the header button did nothing in a quarter-screen window.
        QQuickItem *panel = item("roomInfoPanel");
        const bool shown = panel && panel->isVisible() && panel->width() > 100;
        const qreal paneW = pane->width();
        const qreal panelW = panel ? panel->width() : 0;
        QQuickItem *roomColumn = item("roomColumn");
        const bool roomHidden = roomColumn && !roomColumn->isVisible();
        // A file dropped on the panel is not for a composer nobody can see.
        QQuickItem *drop = item("chatDropArea");
        QVERIFY(drop);
        const qreal dropW = drop->width();
        pane->setProperty("infoOpen", false);
        QTest::qWait(150);
        QVERIFY(!roomHidden || dropW == 0);
        QVERIFY2(shown, qPrintable(QStringLiteral("Room Information is not "
                                                  "shown at %1x%2")
                                       .arg(size.width()).arg(size.height())));
        if (paneW < 700)
            QVERIFY2(roomHidden && qAbs(panelW - paneW) < 1.0,
                     "a narrow pane should hand itself to Room Information");
        QVERIFY2(stray.isEmpty(), qPrintable(stray.join(QStringLiteral("; "))));
    }

    // The sign-in screen at the floor: nothing the user must press lies
    // outside the window.
    void theLoginScreenStaysInsideTheWindowAtTheFloor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x360") << QSize(640, 360);
        QTest::newRow("854x480") << QSize(854, 480);
    }

    void theLoginScreenStaysInsideTheWindowAtTheFloor()
    {
        QFETCH(QSize, size);
        m_window->resize(size);
        m_controller->showLogin();
        QCoreApplication::processEvents();
        QTest::qWait(400);
        QStringList stray;
        controlsOutsideBox(m_window->contentItem(),
                           QRectF(0, 0, m_window->width(), m_window->height()),
                           stray);
        const QSize sz = size;
        const bool ok = m_controller->showMain();
        QTest::qWait(200);
        QVERIFY2(stray.isEmpty(),
                 qPrintable(QStringLiteral("%1x%2: ").arg(sz.width()).arg(sz.height())
                            + stray.join(QStringLiteral("; "))));
        QVERIFY(ok);
    }

    // The account switcher grows from the rail's bottom tile. Its quick rows
    // and the Add / Settings / Sign out footer must stay on screen in a short
    // window (the footer used to be cut by the window edge).
    void theAccountSwitcherFooterStaysInsideTheWindowAtTheFloor()
    {
        m_controller->showMain();
        m_window->resize(640, 360);
        QCoreApplication::processEvents();
        QTest::qWait(150);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(
            "import QtQuick\nimport QtQuick.Controls\nimport MatrixClient\n"
            "Item { anchors.fill: parent\n"
            "  AccountMenu { id: dlg; objectName: 'dlg'; x: 86; y: parent.height - 60; margins: 12 }\n}\n"),
            QUrl(QStringLiteral("account-menu-under-test.qml")));
        QScopedPointer<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *host = qobject_cast<QQuickItem *>(owner.data());
        QVERIFY(host);
        host->setParentItem(m_window->contentItem());
        host->setWidth(m_window->width());
        host->setHeight(m_window->height());
        QObject *menu = host->findChild<QObject *>(QStringLiteral("dlg"));
        QVERIFY(menu);
        QVERIFY(QMetaObject::invokeMethod(menu, "open"));
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("opened").toBool(), 1500);
        QTest::qWait(400);
        auto *content = menu->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        const QRectF win(0, 0, m_window->width(), m_window->height());
        const QRectF cr = content->mapRectToScene(
            QRectF(0, 0, content->width(), content->height()));
        QStringList stray;
        controlsOutsideBox(content, win, stray);
        QMetaObject::invokeMethod(menu, "close");
        QTest::qWait(150);
        QVERIFY2(win.adjusted(-1, -1, 1, 1).contains(cr),
                 qPrintable(QStringLiteral("switcher content is %1,%2 %3x%4 in a "
                                           "%5x%6 window").arg(cr.x()).arg(cr.y())
                                .arg(cr.width()).arg(cr.height())
                                .arg(win.width()).arg(win.height())));
        QVERIFY2(stray.isEmpty(), qPrintable(stray.join(QStringLiteral("; "))));
    }
};

QTEST_MAIN(SmallWindowQmlTest)
#include "SmallWindowQmlTest.moc"
