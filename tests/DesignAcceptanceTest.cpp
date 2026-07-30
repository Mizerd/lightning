// Offscreen acceptance walkthrough for the runtime-correction pass: drives
// the PRODUCTION Main window (mock backend, bundled fonts) at the reference
// and narrower sizes, saves PNG snapshots as artifacts, and asserts rendered
// geometry and sampled pixels: the composer card tracks each design theme's
// raised surface, the open thread panel is exactly 340px beside the visible
// timeline and closing it collapses the right side to none, Settings is a
// FULL application view (rail, room list, timeline, composer all hidden),
// and an increased text scale renders without breaking the shell.

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

        // The bundled UI fonts, exactly as main.cpp registers them, so the
        // snapshots render production type — including the v0.7 selectable
        // families. Every registration must succeed and announce the exact
        // family Settings offers, or the font option would silently fall
        // back on user machines.
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
            // v0.6.5: brand face for the trust surface (SPEC 1r) — not a
            // selectable body face, but a stripped build must still fail
            // loudly rather than silently substituting.
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
                        // The mock backend seeds media rows with URLs on the
                        // fake mock.local host; the resulting offline DNS
                        // failure from QQuickImage is mock-data noise, not a
                        // QML defect. Everything else fails the run.
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
        // The formatting toolbar is collapsible; open it so its raised
        // surface is sampled below.
        if (auto *composer = item("messageComposer"))
            composer->setProperty("toolbarExpanded", true);
        QCoreApplication::processEvents();
        struct Case { int theme; const char *name; };
        const Case cases[] = {
            { 8, "design-main-moss-light" },
            { 9, "design-main-indigo-night" },
            { 10, "design-main-deep-teal" },
        };
        for (const auto &c : cases) {
            m_controller->settings()->setTheme(
                static_cast<SettingsManager::Theme>(c.theme));
            const QImage img = grabAndSave(QLatin1String(c.name));
            QVERIFY(!img.isNull());
            // Composer card: sample the toolbar row's empty right side —
            // the theme's raised surface.
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

    void threadPanelRenders340BesideVisibleTimeline()
    {
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
        QVERIFY(panel && roomColumn && composer);
        QTRY_COMPARE_WITH_TIMEOUT(panel->isVisible(), true, kSignalTimeoutMs);
        // The rendered panel is exactly 340px; the timeline and composer
        // stay visible and interactive beside it.
        QTRY_COMPARE_WITH_TIMEOUT(panel->width(), 340.0, kSignalTimeoutMs);
        QVERIFY(roomColumn->isVisible());
        QVERIFY(composer->isVisible());
        // Mini composer and send button render inside the panel.
        auto *miniComposer = item("threadMiniComposer");
        auto *threadSend = item("threadSendButton");
        QVERIFY(miniComposer && threadSend);
        QCOMPARE(qobject_cast<QQuickItem *>(threadSend)->width(), 28.0);

        const QImage img = grabAndSave(QStringLiteral("design-thread-panel"));
        QVERIFY(!img.isNull());
        // Panel surface: sample below the header, left edge of the panel.
        const QPointF p = panel->mapToScene(QPointF(8, 70));
        QVERIFY(channelDelta(sampleAvg(img, QRect(int(p.x()), int(p.y()), 3, 3)),
                             token("sidebar")) <= kTolerance);

        // Closing with the panel's X collapses the right side completely:
        // no Room Information, no member panel, timeline expands.
        auto *closeButton = item("threadCloseButton");
        QVERIFY(closeButton);
        QMetaObject::invokeMethod(closeButton, "click");
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);
        QQuickItem *pane = item("timelinePane");
        QVERIFY(pane);
        QTRY_COMPARE_WITH_TIMEOUT(
            pane->property("rightPanelState").toString(),
            QStringLiteral("none"), kSignalTimeoutMs);
        QTRY_VERIFY(!panel->isVisible());
        QVERIFY(roomColumn->isVisible());
        // Geometry, not just booleans: the released 340px goes back to the
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
        QVERIFY(panel && roomColumn);
        // 1280 wide leaves the pane comfortably over the 660px boundary:
        // the thread stays a 340px side panel, the timeline stays visible.
        QTRY_COMPARE_WITH_TIMEOUT(panel->width(), 340.0, kSignalTimeoutMs);
        QVERIFY(roomColumn->isVisible());
        QVERIFY(!grabAndSave(QStringLiteral("design-1280-thread")).isNull());
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
