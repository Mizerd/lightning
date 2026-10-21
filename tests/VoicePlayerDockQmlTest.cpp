// The docked "now playing" strip (design-media U1) on the real TimelinePane:
// it is laid out in a band of its own under the room header, so the timeline
// viewport (and with it every row, its hover toolbar and its receipts) starts
// below it and nothing is covered. Checked in all three message layouts, at a
// wide and a narrow pane, and against the newest visible row's own geometry;
// and the band goes away with the clip.

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "media/VoicePlaybackController.h"
#include "models/RoomListModel.h"
#include "models/TimelineModel.h"

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {
constexpr int kSignalTimeoutMs = 5000;

// A silent 16-bit mono PCM WAV of `seconds`.
bool writeSilentWav(const QString &path, int seconds)
{
    const quint32 rate = 8000;
    const quint32 dataBytes = rate * 2 * quint32(seconds);
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s.writeRawData("RIFF", 4);
    s << quint32(36 + dataBytes);
    s.writeRawData("WAVE", 4);
    s.writeRawData("fmt ", 4);
    s << quint32(16) << quint16(1) << quint16(1) << rate << (rate * 2)
      << quint16(2) << quint16(16);
    s.writeRawData("data", 4);
    s << dataBytes;
    out.append(QByteArray(int(dataBytes), '\0'));
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
}

// The item's bounds in the scene, whatever its transform: the timeline view
// is a rotated Flickable, so its own (0, 0) is its bottom-right corner.
QRectF sceneRect(QQuickItem *item)
{
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

// Every visible item under `root` whose objectName is `name`.
void collect(QQuickItem *root, const QString &name, QList<QQuickItem *> &out)
{
    if (!root)
        return;
    if (root->objectName() == name && root->isVisible())
        out.append(root);
    const auto kids = root->childItems();
    for (QQuickItem *kid : kids)
        collect(kid, name, out);
}
} // namespace

class VoicePlayerDockQmlTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;

    static QString loginAndFirstRoom(AppController &controller)
    {
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                  QStringLiteral("alice"),
                                  QStringLiteral("unused"));
        if (!loginSpy.wait(kSignalTimeoutMs))
            return {};
        for (int i = 0; i < 50 && controller.roomList()->rowCount() < 1; ++i)
            QTest::qWait(20);
        if (controller.roomList()->rowCount() < 1)
            return {};
        return controller.roomList()
            ->data(controller.roomList()->index(0, 0),
                   RoomListModel::RoomIdRole)
            .toString();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", m_home.filePath(QStringLiteral("c")).toUtf8());
        qputenv("XDG_DATA_HOME", m_home.filePath(QStringLiteral("d")).toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("voice-player-dock-test"));
    }

    void theDockedStripCoversNoRow_data()
    {
        QTest::addColumn<int>("layout");
        QTest::addColumn<int>("width");
        for (int layout : { 0, 1, 2 }) {
            for (int width : { 1000, 520 }) {
                QTest::addRow("layout%d-%dpx", layout, width) << layout << width;
            }
        }
    }
    void theDockedStripCoversNoRow()
    {
        QFETCH(int, layout);
        QFETCH(int, width);
        AppController controller(AppController::MockBackend);
        controller.settings()->setMessageLayout(layout);
        QVERIFY(controller.settings()->voicePlayerDocked());
        const QString roomId = loginAndFirstRoom(controller);
        QVERIFY(!roomId.isEmpty());
        controller.setCurrentRoomId(roomId);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQuickWindow window;
        window.resize(width, 700);
        QQmlComponent component(&engine);
        component.loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("TimelinePane"));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> pane(component.create(engine.rootContext()));
        auto *root = qobject_cast<QQuickItem *>(pane.get());
        QVERIFY(root);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(width, 700));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY(controller.timeline()->rowCount() > 3);

        auto *strip = root->findChild<QQuickItem *>(
            QStringLiteral("voicePlayerDock"));
        auto *view = root->findChild<QQuickItem *>(
            QStringLiteral("timelineListView"));
        QVERIFY(strip && view);
        QVERIFY(!strip->isVisible());
        const QRectF viewBefore = sceneRect(view);

        // A clip from this room: the strip shows, docked under the header.
        auto *vp = controller.voicePlayback();
        vp->play(QStringLiteral("$clip"), roomId, QStringLiteral("mk-none"),
                 { { QStringLiteral("senderName"), QStringLiteral("Alice") },
                   { QStringLiteral("isVoice"), true } });
        QVERIFY(vp->active());
        QTRY_VERIFY(strip->isVisible() && strip->height() > 20);
        // Laid out (the column polishes on the next frame).
        QTRY_VERIFY(strip->width() > 0
                    && sceneRect(view).height() < viewBefore.height());
        QTRY_VERIFY(sceneRect(view).top() >= sceneRect(strip).bottom() - 0.5);
        const QRectF stripRect = sceneRect(strip);
        const QRectF viewRect = sceneRect(view);
        // The view is shorter by the strip, not covered by it.
        QVERIFY2(qAbs((viewBefore.height() - viewRect.height())
                      - stripRect.height()) < 1.5,
                 qPrintable(QStringLiteral("view %1 -> %2, strip %3")
                                .arg(viewBefore.height())
                                .arg(viewRect.height())
                                .arg(stripRect.height())));
        QVERIFY2(view->clip(), "rows could paint above the viewport");
        // The strip lies inside the pane, across the room column.
        QVERIFY(stripRect.left() >= -0.5 && stripRect.right() <= width + 0.5);

        // No visible row (nor its action bar) starts inside the strip.
        QList<QQuickItem *> rows;
        collect(view, QStringLiteral("messageDelegateRoot"), rows);
        int onScreen = 0;
        for (QQuickItem *row : std::as_const(rows)) {
            const QRectF r = sceneRect(row);
            const QRectF visible = r.intersected(viewRect);
            if (visible.isEmpty())
                continue;
            ++onScreen;
            QVERIFY2(!visible.intersects(stripRect),
                     qPrintable(QStringLiteral("a row at %1..%2 under the "
                                               "strip %3..%4")
                                    .arg(visible.top()).arg(visible.bottom())
                                    .arg(stripRect.top())
                                    .arg(stripRect.bottom())));
        }
        QVERIFY2(onScreen > 0, "no row was on screen to check");

        // The strip's controls are inside it, and its scrubber's hit band
        // never reaches the timeline.
        auto *seek = strip->findChild<QQuickItem *>(
            QStringLiteral("voicePlayerBarSeek"));
        QVERIFY(seek);
        QVERIFY(sceneRect(seek).bottom() <= stripRect.bottom() + 0.5);

        // Stopped: the band goes and the view gets its height back.
        vp->stop();
        QTRY_VERIFY(!strip->isVisible());
        QTRY_COMPARE(sceneRect(view).height(), viewBefore.height());
    }


    // Review round 1 (M1): the window's "focus the player" shortcut lands on
    // the docked strip; Space right after it must play or pause, not follow
    // the "go to the message" link (it opened the clip's room).
    void spaceOnTheFocusedStripPlaysOrPauses()
    {
        QTemporaryDir media;
        QVERIFY(media.isValid());
        const QString clip = media.filePath(QStringLiteral("clip.wav"));
        QVERIFY(writeSilentWav(clip, 6));
        AppController controller(AppController::MockBackend);
        QVERIFY(controller.settings()->voicePlayerDocked());
        const QString roomId = loginAndFirstRoom(controller);
        QVERIFY(!roomId.isEmpty());
        controller.setCurrentRoomId(roomId);
        auto *vp = controller.voicePlayback();
        const QString url = QUrl::fromLocalFile(clip).toString();
        vp->setSourceResolverForTest([url](const QString &) { return url; });

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQuickWindow window;
        window.resize(1000, 700);
        QQmlComponent component(&engine);
        component.loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("TimelinePane"));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> pane(component.create(engine.rootContext()));
        auto *root = qobject_cast<QQuickItem *>(pane.get());
        QVERIFY(root);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(1000, 700));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&window));

        // A clip from another room, so following the link would switch.
        vp->play(QStringLiteral("$clip"), QStringLiteral("!elsewhere:x"),
                 QStringLiteral("mk-clip"),
                 { { QStringLiteral("senderName"), QStringLiteral("Alice") },
                   { QStringLiteral("isVoice"), true } });
        QTRY_VERIFY_WITH_TIMEOUT(vp->playing(), 5000);
        auto *strip = root->findChild<QQuickItem *>(
            QStringLiteral("voicePlayerDock"));
        QVERIFY(strip);
        QTRY_VERIFY(strip->isVisible());
        QVERIFY(QMetaObject::invokeMethod(root, "focusVoiceDock"));
        auto *playPause = strip->findChild<QQuickItem *>(
            QStringLiteral("voicePlayerBarPlayPause"));
        QVERIFY(playPause);
        QTRY_VERIFY2(playPause->hasActiveFocus(),
                     "the shortcut's focus did not land on play/pause");
        QTest::keyClick(&window, Qt::Key_Space);
        QTRY_VERIFY2(!vp->playing(), "Space did not pause the clip");
        QCOMPARE(controller.currentRoomId(), roomId);
        // And the link itself, focused by Tab, takes Space as play/pause too.
        auto *link = strip->findChild<QQuickItem *>(
            QStringLiteral("voicePlayerBarSource"));
        QVERIFY(link);
        link->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(&window, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(vp->playing(), 5000);
        QCOMPARE(controller.currentRoomId(), roomId);
        vp->stop();
    }
};

QTEST_MAIN(VoicePlayerDockQmlTest)
#include "VoicePlayerDockQmlTest.moc"
