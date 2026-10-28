// Offscreen acceptance walkthrough of the production Main window (mock
// backend, bundled fonts) at the reference and narrower sizes. Saves PNG
// snapshots and asserts geometry and sampled pixels: the composer card tracks
// each theme's raised surface; the thread panel takes the shared "Side panel
// width" setting beside the visible timeline, follows it and its own resize
// grab, and closing it frees that space; a Space Home lists its channels
// before its people; Settings is a full view (rail, room list, timeline and
// composer hidden); and a larger text scale does not break the shell.

#include <QtTest/QtTest>

#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "models/TimelineModel.h"
#include "threads/ThreadController.h"

namespace {

constexpr int kSignalTimeoutMs = 5000;
constexpr int kTolerance = 8;

QColor sampleAvg(const QImage &img, const QRect &r)
{
    qint64 red = 0, green = 0, blue = 0, n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        for (int x = r.left(); x <= r.right(); ++x) {
            if (x < 0 || y < 0 || x >= img.width() || y >= img.height())
                continue;
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

QString snapshotDir()
{
    const QByteArray env = qgetenv("LIGHTNING_SNAPSHOT_DIR");
    const QString dir = env.isEmpty()
        ? QDir::temp().filePath(QStringLiteral("lightning-design-snapshots"))
        : QString::fromLocal8Bit(env);
    QDir().mkpath(dir);
    return dir;
}

// The production Main window is loaded directly — its own bindings drive
// AppTheme from the settings backend exactly as in the shipped app.

} // namespace

class DesignAcceptanceTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
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

    QQuickItem *item(const char *name) const
    {
        if (auto *hit = m_window->findChild<QQuickItem *>(QLatin1String(name)))
            return hit;
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    QColor token(const char *name) const
    {
        QQmlExpression expr(qmlContext(m_window), m_window,
                            QStringLiteral("AppTheme.%1")
                                .arg(QLatin1String(name)));
        return expr.evaluate().value<QColor>();
    }

    QImage grabAndSave(const QString &name)
    {
        QCoreApplication::processEvents();
        const QImage img = m_window->grabWindow();
        if (!img.isNull())
            img.save(QDir(snapshotDir()).filePath(name + QStringLiteral(".png")));
        return img;
    }

    // The thread panel's side-by-side width for a pane this wide: the shared
    // "Side panel width" setting, clamped as TimelinePane clamps it (floor 260,
    // and the 320px timeline plus the 1px divider always fit).
    int expectedThreadPanelWidth(qreal paneWidth) const
    {
        return qMax(260, qMin(m_controller->settings()->sidePanelWidth(),
                              int(paneWidth) - 320 - 1));
    }

    // Resizes the window until the timeline pane is exactly `target` wide
    // (the shell around it is not a fixed width at every window size).
    bool setPaneWidth(QQuickItem *pane, int target)
    {
        for (int i = 0; i < 4 && int(pane->width()) != target; ++i) {
            m_window->setWidth(m_window->width() + target - int(pane->width()));
            QCoreApplication::processEvents();
            QTest::qWait(30);
        }
        return int(pane->width()) == target;
    }

    static QQuickItem *flickableAncestor(QQuickItem *item)
    {
        for (QQuickItem *p = item ? item->parentItem() : nullptr; p;
             p = p->parentItem()) {
            if (p->inherits("QQuickFlickable"))
                return p;
        }
        return nullptr;
    }

    // Puts the shared setting back however a case ends, so a mid-case
    // failure cannot change the geometry later cases measure.
    auto restoreSidePanelWidth()
    {
        return qScopeGuard([this] {
            m_controller->settings()->setSidePanelWidth(320);
        });
    }

    QString fixtureThreadRootId() const
    {
        auto *timeline = m_controller->timeline();
        for (int row = 0; row < timeline->rowCount(); ++row) {
            const QString rootId = timeline
                ->data(timeline->index(row, 0),
                       TimelineModel::ThreadRootIdRole)
                .toString();
            if (!rootId.isEmpty())
                return rootId;
        }
        return {};
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("design-acceptance-test"));
        QSettings().clear();

        // The bundled UI fonts, registered as main.cpp does. Each must register
        // and report the family Settings offers, or the font option would
        // silently fall back on user machines.
        const QList<QPair<QString, QString>> bundled = {
            { QStringLiteral("Manrope[wght].ttf"), QStringLiteral("Manrope") },
            { QStringLiteral("JetBrainsMono[wght].ttf"),
              QStringLiteral("JetBrains Mono") },
            { QStringLiteral("Inter[wght].ttf"), QStringLiteral("Inter") },
            { QStringLiteral("IBMPlexSans[wght].ttf"),
              QStringLiteral("IBM Plex Sans") },
            { QStringLiteral("SourceSans3[wght].ttf"),
              QStringLiteral("Source Sans 3") },
            { QStringLiteral("PlusJakartaSans[wght].ttf"),
              QStringLiteral("Plus Jakarta Sans") },
            // Brand face for the trust surface, not a selectable body face; a
            // stripped build must still fail loudly.
            { QStringLiteral("SpaceGrotesk[wght].ttf"),
              QStringLiteral("Space Grotesk") },
        };
        for (const auto &font : bundled) {
            const int id = QFontDatabase::addApplicationFont(
                QStringLiteral(":/qt/qml/MatrixClient/data/fonts/")
                + font.first);
            QVERIFY2(id >= 0, qPrintable(font.first));
            QVERIFY2(QFontDatabase::applicationFontFamilies(id)
                         .contains(font.second),
                     qPrintable(font.second));
        }
        QFontDatabase::addApplicationFont(QStringLiteral(
            ":/qt/qml/MatrixClient/data/fonts/MaterialSymbolsRounded-subset.ttf"));

        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlApplicationEngine;
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings) {
                        // Mock media URLs point at the fake mock.local host; the
                        // resulting DNS failure is fixture noise. Everything
                        // else fails the run.
                        if (w.toString().contains(
                                QLatin1String("Host mock.local not found")))
                            continue;
                        m_warnings.append(w.toString());
                    }
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QSignalSpy createdSpy(m_engine,
                              &QQmlApplicationEngine::objectCreated);
        m_engine->loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Main"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        m_window = qobject_cast<QQuickWindow *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(m_window);
        m_window->setWidth(1600);
        m_window->setHeight(1000);
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

    void mainChatRendersAcrossDesignThemes()
    {
        // Open the collapsible toolbar so its raised surface is sampled.
        if (auto *composer = item("messageComposer"))
            composer->setProperty("toolbarExpanded", true);
        QCoreApplication::processEvents();
        struct Case { int theme; const char *name; };
        const Case cases[] = {
            { 8, "design-main-moss-light" },
            { 9, "design-main-indigo-night" },
            { 10, "design-main-deep-teal" },
            // Storm (11). token() reads AppTheme live, so samples resolve
            // against Storm's palette.
            { 11, "design-main-storm" },
        };
        for (const auto &c : cases) {
            m_controller->settings()->setTheme(
                static_cast<SettingsManager::Theme>(c.theme));
            const QImage img = grabAndSave(QLatin1String(c.name));
            QVERIFY(!img.isNull());
            // Composer card: the toolbar row's empty right side, the theme's
            // raised surface.
            auto *card = item("composerCard");
            auto *toolbar = item("composerToolbarRow");
            QVERIFY(card && toolbar);
            const QPointF p = toolbar->mapToScene(
                QPointF(toolbar->width() - 24, toolbar->height() / 2));
            QVERIFY2(channelDelta(sampleAvg(img, QRect(int(p.x()), int(p.y()) - 1, 3, 3)),
                                  token("surface")) <= kTolerance,
                     c.name);
            // Rail tier on the far left.
            QVERIFY2(channelDelta(sampleAvg(img, QRect(4, 300, 3, 3)),
                                  token("rail")) <= kTolerance,
                     c.name);
        }
        m_controller->settings()->setTheme(SettingsManager::IndigoNightTheme);
        QCoreApplication::processEvents();
    }

    // The wordmark bolt hugs the "Lightning" label: not a tile to its left,
    // and not pinned to the room-list column's far edge by a layout that
    // distributes surplus between non-fill children (see RoomsPanel.qml).
    // Uses the real Main.qml shell.
    void wordmarkBoltHugsTheLabelNotTheColumnEdge()
    {
        auto *label = item("workspaceLabel");
        auto *bolt = item("workspaceBoltMark");
        auto *rooms = item("roomsPanel");
        QVERIFY(label && bolt && rooms);

        const qreal labelRight =
            label->mapToItem(rooms, QPointF(label->width(), 0)).x();
        const qreal boltLeft = bolt->mapToItem(rooms, QPointF(0, 0)).x();
        const qreal boltRight =
            bolt->mapToItem(rooms, QPointF(bolt->width(), 0)).x();

        // The bolt's left edge sits just past the label's right edge plus
        // headerRow spacing (6px).
        QVERIFY2(boltLeft >= labelRight && boltLeft <= labelRight + 20,
                 qPrintable(QStringLiteral(
                     "bolt.x=%1 not within a few px of label's right edge=%2")
                                .arg(boltLeft).arg(labelRight)));
        // Not flush with the column's far-right edge.
        QVERIFY2(boltRight < rooms->width() - 4,
                 qPrintable(QStringLiteral(
                     "bolt right edge=%1 flush with column edge=%2")
                                .arg(boltRight).arg(rooms->width())));
    }

    // Support chat 2026-10-20: "the thread-pane settings don't work and it's
    // hardcoded". The panel was a literal 340px that ignored Settings >
    // Appearance > Side panel width, which only Room Information read. It
    // takes that setting now, follows a change to it, has the same resize
    // grab as Room Information, and the chat-wide drop area stops exactly at
    // its divider, all from one width.
    void threadPanelFollowsTheSidePanelWidthBesideVisibleTimeline()
    {
        // The width, the side-by-side threshold and the drop area all derive
        // from one property: none of them may feed back into another.
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("[Bb]inding loop")));
        QCOMPARE(m_controller->settings()->sidePanelWidth(), 320);
        const auto restoreWidth = restoreSidePanelWidth();
        const auto closeThread = qScopeGuard([this] {
            m_controller->thread()->close();
        });
        const QString rootId = fixtureThreadRootId();
        QVERIFY(!rootId.isEmpty());
        m_controller->thread()->openThread(
            QStringLiteral("!general:mock.local"), rootId);
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        QCoreApplication::processEvents();

        auto *panel = item("threadPanel");
        auto *roomColumn = item("roomColumn");
        auto *composer = item("composerCard");
        auto *pane = item("timelinePane");
        auto *dropArea = item("chatDropArea");
        QVERIFY(panel && roomColumn && composer && pane && dropArea);
        QTRY_COMPARE_WITH_TIMEOUT(panel->isVisible(), true, kSignalTimeoutMs);
        // Fixture: the pane is wide enough for the widest width used below,
        // or "follows the setting" would be measuring the clamp.
        QVERIFY2(pane->width() >= 480 + 320 + 1,
                 qPrintable(QStringLiteral("pane only %1px wide")
                                .arg(pane->width())));

        // The default setting (320), not a literal of the panel's own;
        // timeline and composer stay visible and interactive beside it.
        QTRY_COMPARE_WITH_TIMEOUT(
            panel->width(), qreal(expectedThreadPanelWidth(pane->width())),
            kSignalTimeoutMs);
        QCOMPARE(panel->width(), 320.0);
        QVERIFY(roomColumn->isVisible());
        QVERIFY(composer->isVisible());

        // A changed setting moves the panel, and the drop area with it.
        m_controller->settings()->setSidePanelWidth(420);
        QTRY_COMPARE_WITH_TIMEOUT(panel->width(), 420.0, kSignalTimeoutMs);
        QVERIFY(roomColumn->isVisible());
        auto *handle = item("sidePanelResizeHandle");
        QVERIFY2(handle, "the thread panel's divider is not a resize grab");
        QVERIFY(handle->isVisible());
        const qreal panelLeft = panel->mapToScene(QPointF(0, 0)).x();
        const qreal handleLeft = handle->mapToScene(QPointF(0, 0)).x();
        const qreal dropRight =
            dropArea->mapToScene(QPointF(dropArea->width(), 0)).x();
        // Divider immediately left of the panel; the drop area ends where the
        // divider begins: it covers none of the thread surface and leaves no
        // gap where a drop would be lost.
        QCOMPARE(handleLeft + handle->width(), panelLeft);
        QVERIFY2(qAbs(dropRight - handleLeft) < 0.5,
                 qPrintable(QStringLiteral("drop area ends at %1, divider at "
                                           "%2, panel at %3")
                                .arg(dropRight).arg(handleLeft)
                                .arg(panelLeft)));
        // The panel's right edge is the pane's: nothing else took the space.
        QCOMPARE(panel->mapToScene(QPointF(panel->width(), 0)).x(),
                 pane->mapToScene(QPointF(pane->width(), 0)).x());

        // The divider is a grab: dragging it left by 40px widens the panel
        // through the same setting Room Information's grab writes.
        // On the divider's own pixel: half a pixel in would round onto the
        // panel's first column.
        const QPoint press = handle->mapToScene(
            QPointF(0, handle->height() / 2)).toPoint();
        QTest::mousePress(m_window, Qt::LeftButton, {}, press);
        for (int dx = 5; dx <= 40; dx += 5)
            QTest::mouseMove(m_window, press - QPoint(dx, 0), 10);
        QTest::mouseRelease(m_window, Qt::LeftButton, {},
                            press - QPoint(40, 0), 10);
        QTRY_VERIFY2_WITH_TIMEOUT(
            qAbs(m_controller->settings()->sidePanelWidth() - 460) <= 2,
            qPrintable(QStringLiteral("setting %1 after a 40px drag from 420")
                           .arg(m_controller->settings()->sidePanelWidth())),
            kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(
            panel->width(),
            qreal(m_controller->settings()->sidePanelWidth()),
            kSignalTimeoutMs);

        m_controller->settings()->setSidePanelWidth(320);
        QTRY_COMPARE_WITH_TIMEOUT(panel->width(), 320.0, kSignalTimeoutMs);
        // Mini composer and send button render inside the panel.
        auto *miniComposer = item("threadMiniComposer");
        auto *threadSend = item("threadSendButton");
        QVERIFY(miniComposer && threadSend);
        QCOMPARE(qobject_cast<QQuickItem *>(threadSend)->width(), 28.0);
        auto *composerRow = item("threadComposerRow");
        QVERIFY(composerRow);
        const qreal originalHeight = miniComposer->height();
        miniComposer->setHeight(originalHeight + 80);
        QCoreApplication::processEvents();
        QVERIFY(qAbs(composerRow->mapToItem(miniComposer,
                    QPointF(0, composerRow->height())).y()
                     - (miniComposer->height() - 8)) < 1.0);
        miniComposer->setHeight(originalHeight);
        QCoreApplication::processEvents();

        const QImage img = grabAndSave(QStringLiteral("design-thread-panel"));
        QVERIFY(!img.isNull());
        // Panel surface: below the header, at the panel's left edge.
        const QPointF p = panel->mapToScene(QPointF(8, 70));
        QVERIFY(channelDelta(sampleAvg(img, QRect(int(p.x()), int(p.y()), 3, 3)),
                             token("sidebar")) <= kTolerance);

        // Closing with the panel's X collapses the right side completely.
        auto *closeButton = item("threadCloseButton");
        QVERIFY(closeButton);
        QMetaObject::invokeMethod(closeButton, "click");
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(
            pane->property("rightPanelState").toString(),
            QStringLiteral("none"), kSignalTimeoutMs);
        QTRY_VERIFY(!panel->isVisible());
        QVERIFY(roomColumn->isVisible());
        // Geometry, not just booleans: the panel's width returns to the
        // timeline column.
        auto *roomColumnItem = qobject_cast<QQuickItem *>(roomColumn);
        QVERIFY(roomColumnItem);
        QTRY_VERIFY(roomColumnItem->width() >= pane->width() - 1.0);
    }

    void settingsIsAFullApplicationView()
    {
        m_controller->showSettings();
        QCoreApplication::processEvents();
        auto *rail = item("spacesRail");
        auto *rooms = item("roomsPanel");
        auto *timeline = item("timelinePane");
        QVERIFY(rail && rooms && timeline);
        // Full application view: the whole chat shell is hidden.
        QVERIFY(!rail->isVisible());
        QVERIFY(!rooms->isVisible());
        QVERIFY(!timeline->isVisible());
        QVERIFY(item("settingsHeaderTitle"));
        auto *settingsLoader = m_window->findChild<QQuickItem *>(
            QStringLiteral("settingsViewLoader"));
        QVERIFY(settingsLoader);
        QCOMPARE(settingsLoader->width(), m_window->contentItem()->width());
        const QImage img = grabAndSave(QStringLiteral("design-settings"));
        QVERIFY(!img.isNull());

        // Increased text scale renders the same shell without breaking it.
        m_controller->settings()->setTextScale(140);
        QCoreApplication::processEvents();
        const QImage scaled = grabAndSave(
            QStringLiteral("design-settings-scale140"));
        QVERIFY(!scaled.isNull());
        auto *slider = item("textScaleSlider");
        QVERIFY(slider);
        QCOMPARE(slider->property("value").toInt(), 140);
        m_controller->settings()->setTextScale(100);

        m_controller->showMain();
        QCoreApplication::processEvents();
        QTRY_VERIFY(timeline->isVisible());
    }

    void narrowerSupportedSizesKeepTheShellCoherent()
    {
        m_window->setWidth(1280);
        m_window->setHeight(800);
        QCoreApplication::processEvents();
        const QString rootId = fixtureThreadRootId();
        QVERIFY(!rootId.isEmpty());
        m_controller->thread()->openThread(
            QStringLiteral("!general:mock.local"), rootId);
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        QCoreApplication::processEvents();
        auto *panel = item("threadPanel");
        auto *roomColumn = item("roomColumn");
        auto *pane = item("timelinePane");
        auto *handle = item("sidePanelResizeHandle");
        auto *dropArea = item("chatDropArea");
        QVERIFY(panel && roomColumn && pane && handle && dropArea);
        const auto restoreWidth = restoreSidePanelWidth();
        const auto restoreWindow = qScopeGuard([this] {
            m_controller->thread()->close();
            m_window->setWidth(1600);
            m_window->setHeight(1000);
        });
        // At 1280 wide the pane still fits the panel beside the 320px
        // timeline: the thread stays a side panel at the setting's width and
        // the timeline stays visible.
        QTRY_COMPARE_WITH_TIMEOUT(
            panel->width(), qreal(expectedThreadPanelWidth(pane->width())),
            kSignalTimeoutMs);
        QCOMPARE(panel->width(), 320.0);
        QVERIFY(roomColumn->isVisible());
        QVERIFY(!grabAndSave(QStringLiteral("design-1280-thread")).isNull());

        // The cap: the widest setting would leave the timeline under its
        // 320px here, so the panel takes exactly what the timeline and the
        // 1px divider leave, and the drop area still ends at the divider.
        // Every number derives from the pane's real width.
        m_controller->settings()->setSidePanelWidth(640);
        const qreal paneWidth = pane->width();
        QVERIFY2(paneWidth - 321 < 640 && paneWidth - 321 > 260,
                 qPrintable(QStringLiteral("fixture: a %1px pane does not "
                                           "exercise the cap")
                                .arg(paneWidth)));
        QTRY_COMPARE_WITH_TIMEOUT(panel->width(), paneWidth - 321,
                                  kSignalTimeoutMs);
        QVERIFY(roomColumn->isVisible());
        QCOMPARE(roomColumn->width(), 320.0);
        QVERIFY(handle->isVisible());
        const qreal handleLeft = handle->mapToScene(QPointF(0, 0)).x();
        QCOMPARE(handleLeft + handle->width(),
                 panel->mapToScene(QPointF(0, 0)).x());
        QCOMPARE(dropArea->mapToScene(QPointF(dropArea->width(), 0)).x(),
                 handleLeft);

        // The floor and the breakpoint, in the real shell: a 581px pane is
        // the narrowest that keeps the 260px floor beside the 320px timeline;
        // at 580 the thread takes the whole pane.
        m_controller->settings()->setSidePanelWidth(320);
        QVERIFY2(setPaneWidth(pane, 581),
                 qPrintable(QStringLiteral("could not size the pane to 581 "
                                           "(it is %1)").arg(pane->width())));
        QTRY_COMPARE_WITH_TIMEOUT(panel->width(), 260.0, kSignalTimeoutMs);
        QVERIFY(roomColumn->isVisible());
        QCOMPARE(roomColumn->width(), 320.0);
        QVERIFY(handle->isVisible());
        // At the 260px floor the thread root's header gives way in order:
        // the timestamp first, then the "Open in room" label, the author's
        // name last. The name used to be the one fill child, so it went
        // first, to nothing (measured 0px wide at this width).
        QTest::qWait(50);
        auto *senderName = item("threadRootSenderName");
        QVERIFY(senderName && senderName->isVisible());
        QVERIFY2(!senderName->property("truncated").toBool()
                     && senderName->width() >= senderName->implicitWidth(),
                 qPrintable(QStringLiteral("the thread root's author is %1px "
                                           "of %2px at the 260px floor")
                                .arg(senderName->width())
                                .arg(senderName->implicitWidth())));
        auto *rootDate = item("threadRootDate");
        auto *openInRoom = item("threadOpenInRoomButton");
        QVERIFY(rootDate && openInRoom);
        QVERIFY2(!rootDate->isVisible(),
                 "the timestamp did not give way before the author's name");
        QVERIFY(openInRoom->isVisible());

        // A name too long for the labelled button: "Open in room" goes
        // icon-only before the name is cut. ThreadPanel.rootData is assigned
        // (refreshRoot), not bound, so the fixture can rename the author;
        // the guard puts the real record back.
        const QVariant originalRoot = panel->property("rootData");
        const auto restoreRoot = qScopeGuard([panel, originalRoot] {
            panel->setProperty("rootData", originalRoot);
        });
        QVariantMap renamed = originalRoot.toMap();
        // 82px at this size: wider than the labelled button leaves (about
        // 66px), narrower than the icon-only one does (about 125px).
        renamed.insert(QStringLiteral("senderDisplayName"),
                       QStringLiteral("Christopher"));
        QVERIFY(panel->setProperty("rootData", renamed));
        QTRY_COMPARE_WITH_TIMEOUT(senderName->property("text").toString(),
                                  QStringLiteral("Christopher"),
                                  kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(openInRoom->property("text").toString(),
                                  QString(), kSignalTimeoutMs);
        QCOMPARE(openInRoom->property("iconName").toString(),
                 QStringLiteral("arrow_forward"));
        QVERIFY(openInRoom->isVisible());
        QVERIFY(!rootDate->isVisible());
        QTest::qWait(50);
        QVERIFY2(!senderName->property("truncated").toBool()
                     && senderName->width() >= senderName->implicitWidth(),
                 qPrintable(QStringLiteral("with the button icon-only the "
                                           "author is still %1px of %2px")
                                .arg(senderName->width())
                                .arg(senderName->implicitWidth())));
        const auto attached = [openInRoom](const char *expression) {
            QQmlExpression expr(qmlContext(openInRoom), openInRoom,
                                QLatin1String(expression));
            return expr.evaluate();
        };
        // An icon alone still has a name, for screen readers and as a
        // tooltip, and the tooltip comes on KEYBOARD focus too, not only
        // under a pointer (none is over it here).
        QCOMPARE(attached("Accessible.name").toString(),
                 QStringLiteral("Open in room"));
        QCOMPARE(attached("ToolTip.text").toString(),
                 QStringLiteral("Open in room"));
        QVERIFY(!attached("ToolTip.visible").toBool());
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(m_window));
        openInRoom->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(openInRoom->property("visualFocus").toBool(),
                                 kSignalTimeoutMs);
        QTRY_VERIFY2_WITH_TIMEOUT(
            attached("ToolTip.visible").toBool(),
            "the icon-only Open in room button names itself on hover only",
            kSignalTimeoutMs);
        openInRoom->setFocus(false);
        QVERIFY2(setPaneWidth(pane, 580),
                 qPrintable(QStringLiteral("could not size the pane to 580 "
                                           "(it is %1)").arg(pane->width())));
        QTRY_VERIFY_WITH_TIMEOUT(!roomColumn->isVisible(), kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(panel->width(), 580.0, kSignalTimeoutMs);
        QVERIFY(!handle->isVisible());
        QCOMPARE(dropArea->width(), 0.0);
        m_window->setWidth(1280);
        QCoreApplication::processEvents();

        m_controller->thread()->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);

        // 960×600: composer still inside the timeline, send visible.
        m_window->setWidth(960);
        m_window->setHeight(600);
        QCoreApplication::processEvents();
        auto *send = item("composerSendButton");
        auto *card = item("composerCard");
        QVERIFY(send && card);
        QVERIFY(send->isVisible());
        const QPointF sendRight = send->mapToScene(QPointF(send->width(), 0));
        QVERIFY(sendRight.x() <= 960.0);
        QVERIFY(!grabAndSave(QStringLiteral("design-960-chat")).isNull());

        m_window->setWidth(1600);
        m_window->setHeight(1000);
        QCoreApplication::processEvents();
    }

    // Support chat 2026-10-20: "In a space's lobby, the 'people in this
    // space' should be displayed ... at the end, rather than before the
    // channels". The card follows the room member panel's remembered flag, so
    // anyone who keeps a member list open got a roster above every lobby's
    // channels. Geometric, on the real pane: the card is laid out below the
    // whole lobby, and the button that toggles it stays in the header. Being
    // at the end, the card can open below the fold: an explicit click on
    // "People" brings it into view, the remembered flag never scrolls.
    void spaceHomeListsChannelsBeforePeople()
    {
        auto *settings = m_controller->settings();
        const bool wasOpen = settings->memberPanelOpen();
        const auto restore = qScopeGuard([this, settings, wasOpen] {
            settings->setMemberPanelOpen(wasOpen);
            m_window->setWidth(1600);
            m_window->setHeight(1000);
            m_controller->setCurrentRoomId(
                QStringLiteral("!general:mock.local"));
            QCoreApplication::processEvents();
        });
        settings->setMemberPanelOpen(false);
        // Short enough that the card, after the lobby, opens below the fold.
        m_window->setHeight(560);
        QCoreApplication::processEvents();
        const QString spaceId = QStringLiteral("!space-team:mock.local");
        m_controller->openSpaceHome(spaceId);
        QTRY_VERIFY_WITH_TIMEOUT(item("spaceHomePane")
                                     && item("spaceHomePane")->isVisible(),
                                 kSignalTimeoutMs);
        auto *card = item("spacePeopleCard");
        auto *lobby = item("spaceLobby");
        auto *button = item("spacePeopleButton");
        QVERIFY(card && lobby && button);
        QTRY_VERIFY_WITH_TIMEOUT(button->isVisible(), kSignalTimeoutMs);
        QVERIFY(!card->isVisible());
        QQuickItem *flick = flickableAncestor(card);
        QVERIFY2(flick, "Space Home no longer scrolls in a Flickable");
        const auto viewTop = [flick] {
            return flick->mapToScene(QPointF(0, 0)).y();
        };
        const auto viewBottom = [flick] {
            return flick->mapToScene(QPointF(0, flick->height())).y();
        };
        const auto cardTop = [card] {
            return card->mapToScene(QPointF(0, 0)).y();
        };

        // An explicit click on People, the card's FIRST showing in this
        // Space Home (never laid out before): it opens and comes into view.
        QMetaObject::invokeMethod(button, "click");
        QTRY_VERIFY_WITH_TIMEOUT(card->isVisible() && card->height() > 0
                                     && lobby->height() > 0,
                                 kSignalTimeoutMs);
        // Laid out after the lobby first: a card never shown before sits at
        // y 0 until the column places it, which would satisfy the fold check
        // below by accident.
        QTRY_VERIFY2_WITH_TIMEOUT(
            cardTop() >= lobby->mapToScene(QPointF(0, lobby->height())).y(),
            qPrintable(QStringLiteral("the people card (y=%1) was never laid "
                                      "out after the lobby (bottom y=%2)")
                           .arg(cardTop())
                           .arg(lobby->mapToScene(QPointF(0, lobby->height())).y())),
            kSignalTimeoutMs);
        // A card no taller than half the view is shown whole.
        QVERIFY(card->height() <= flick->height() / 2);
        QTRY_VERIFY2_WITH_TIMEOUT(
            cardTop() + card->height() <= viewBottom(),
            qPrintable(QStringLiteral("after clicking People the card ends "
                                      "at y=%1, the view at y=%2")
                           .arg(cardTop() + card->height())
                           .arg(viewBottom())),
            kSignalTimeoutMs);
        QVERIFY(cardTop() >= viewTop());
        const qreal lobbyTop = lobby->mapToScene(QPointF(0, 0)).y();
        const qreal lobbyBottom =
            lobby->mapToScene(QPointF(0, lobby->height())).y();
        const qreal buttonBottom =
            button->mapToScene(QPointF(0, button->height())).y();
        QVERIFY2(cardTop() >= lobbyBottom,
                 qPrintable(QStringLiteral("people card at y=%1, lobby spans "
                                           "y=%2..%3")
                                .arg(cardTop()).arg(lobbyTop)
                                .arg(lobbyBottom)));
        QVERIFY2(buttonBottom <= lobbyTop,
                 "the People button left the header above the channels");
        QVERIFY(!grabAndSave(QStringLiteral("design-space-home-people"))
                     .isNull());

        // The remembered flag (as restored at startup) shows the card and
        // never scrolls, though the card is then cut by the fold.
        QMetaObject::invokeMethod(button, "click");
        QTRY_VERIFY_WITH_TIMEOUT(!card->isVisible(), kSignalTimeoutMs);
        QVERIFY(flick->setProperty("contentY", 0.0));
        settings->setMemberPanelOpen(true);
        QTRY_VERIFY_WITH_TIMEOUT(card->isVisible(), kSignalTimeoutMs);
        QTest::qWait(100);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
        QVERIFY2(cardTop() + card->height() > viewBottom(),
                 qPrintable(QStringLiteral("fixture: the card (y=%1..%2) is "
                                           "not cut by the fold (y=%3), so "
                                           "neither half of this case means "
                                           "anything")
                                .arg(cardTop()).arg(cardTop() + card->height())
                                .arg(viewBottom())));
    }

    // Support chat 2026-10-20 follow-ups: the search panel was a fixed 360px
    // with its own 700px threshold, and the chat-wide drop area assumed a
    // 320px Room information panel while that panel follows the setting, so
    // with a wider setting a drop on Room information's left part became a
    // composer attachment. Both now read the shared side-panel geometry.
    void searchAndRoomInfoShareTheSidePanelWidth()
    {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("[Bb]inding loop")));
        auto *settings = m_controller->settings();
        const bool memberPanelWasOpen = settings->memberPanelOpen();
        const auto restoreWidth = restoreSidePanelWidth();
        auto *pane = item("timelinePane");
        auto *roomColumn = item("roomColumn");
        auto *dropArea = item("chatDropArea");
        QVERIFY(pane && roomColumn && dropArea);
        const auto closePanels = qScopeGuard([pane, settings,
                                              memberPanelWasOpen] {
            pane->setProperty("searchOpen", false);
            pane->setProperty("infoOpen", false);
            settings->setMemberPanelOpen(memberPanelWasOpen);
        });
        QCOMPARE(m_controller->currentRoomId(),
                 QStringLiteral("!general:mock.local"));
        QVERIFY2(pane->width() >= 500 + 420,
                 qPrintable(QStringLiteral("fixture: pane only %1px wide")
                                .arg(pane->width())));

        // Search: the setting's width (320 by default, not 360), beside the
        // timeline, behind the shared divider and grab.
        QVERIFY(QMetaObject::invokeMethod(pane, "toggleSearchPanel"));
        auto *search = item("roomSearchPanel");
        auto *handle = item("sidePanelResizeHandle");
        QVERIFY(search && handle);
        QTRY_VERIFY_WITH_TIMEOUT(search->isVisible(), kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(search->width(), 320.0, kSignalTimeoutMs);
        settings->setSidePanelWidth(420);
        QTRY_COMPARE_WITH_TIMEOUT(search->width(), 420.0, kSignalTimeoutMs);
        QVERIFY(roomColumn->isVisible());
        QVERIFY(handle->isVisible());
        const qreal searchLeft = search->mapToScene(QPointF(0, 0)).x();
        const qreal handleLeft = handle->mapToScene(QPointF(0, 0)).x();
        QCOMPARE(handleLeft + handle->width(), searchLeft);
        QCOMPARE(dropArea->mapToScene(QPointF(dropArea->width(), 0)).x(),
                 handleLeft);
        QCOMPARE(search->mapToScene(QPointF(search->width(), 0)).x(),
                 pane->mapToScene(QPointF(pane->width(), 0)).x());
        QVERIFY(QMetaObject::invokeMethod(pane, "toggleSearchPanel"));
        QTRY_VERIFY_WITH_TIMEOUT(!search->isVisible(), kSignalTimeoutMs);

        // Room information at a setting wider than the 320 the drop area
        // used to assume: the drop area ends at Room information's divider.
        settings->setSidePanelWidth(500);
        // Opened directly: the mock backend has no room management, so the
        // header's toggle refuses (TimelinePaneQmlTest does the same).
        QVERIFY(pane->setProperty("infoOpen", true));
        auto *info = item("roomInfoPanel");
        auto *infoHandle = item("roomInfoResizeHandle");
        QVERIFY(info && infoHandle);
        QTRY_VERIFY_WITH_TIMEOUT(info->isVisible(), kSignalTimeoutMs);
        const qreal expectedInfo =
            qMax(260.0, qMin(500.0, pane->width() - 420));
        QTRY_COMPARE_WITH_TIMEOUT(info->width(), expectedInfo,
                                  kSignalTimeoutMs);
        const qreal infoHandleLeft =
            infoHandle->mapToScene(QPointF(0, 0)).x();
        QCOMPARE(infoHandleLeft + infoHandle->width(),
                 info->mapToScene(QPointF(0, 0)).x());
        QVERIFY2(qAbs(dropArea->mapToScene(QPointF(dropArea->width(), 0)).x()
                      - infoHandleLeft) < 0.5,
                 qPrintable(QStringLiteral("drop area ends at %1, Room "
                                           "information's divider at %2")
                                .arg(dropArea->mapToScene(
                                         QPointF(dropArea->width(), 0)).x())
                                .arg(infoHandleLeft)));
    }

    void noQmlWarningsAcrossTheWalkthrough()
    {
        QCOMPARE(m_warnings, QStringList{});
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    DesignAcceptanceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "DesignAcceptanceTest.moc"
