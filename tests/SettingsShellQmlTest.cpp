// Full-view Settings proof against the production Main window: opening
// Settings hides the ENTIRE chat shell (spaces rail, room list, timeline,
// composer) and any right-side panel, and fills the application content
// area; entering it from an open Room Information / People / Thread state
// clears that state; closing restores the chat shell and the selected room
// with the right panel remaining None. The Appearance controls (featured
// theme cards with fixed palettes, instant switching, match-system,
// message-layout, text-size) are exercised inside the real full-view
// settings, including the 1374x944 no-horizontal-clipping contract.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
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

constexpr int kTolerance = 8;
constexpr int kSignalTimeoutMs = 5000;

} // namespace

class SettingsShellQmlTest : public QObject
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

    QColor themeColor(const char *token) const
    {
        QQmlExpression expr(qmlContext(m_window),
                            m_window,
                            QStringLiteral("AppTheme.%1")
                                .arg(QLatin1String(token)));
        return expr.evaluate().value<QColor>();
    }

    void clickItem(QQuickItem *target)
    {
        const QPointF center = target->mapToScene(
            QPointF(target->width() / 2, target->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          center.toPoint());
        QCoreApplication::processEvents();
    }

    QQuickItem *timelinePane() const { return item("timelinePane"); }

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
            QStringLiteral("settings-shell-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlApplicationEngine;
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings)
                        m_warnings.append(w.toString());
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
        // The screenshot geometry from the runtime evidence.
        m_window->setWidth(1374);
        m_window->setHeight(944);
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

    void settingsTakesOverTheFullContentArea()
    {
        auto *rail = item("spacesRail");
        auto *rooms = item("roomsPanel");
        auto *timeline = timelinePane();
        QVERIFY(rail && rooms && timeline);
        QVERIFY(rail->isVisible());
        QVERIFY(rooms->isVisible());
        QVERIFY(timeline->isVisible());

        m_controller->showSettings();
        QCoreApplication::processEvents();

        // The entire chat shell disappears — rail, room list, timeline,
        // composer — and Settings fills the content area.
        QVERIFY(!rail->isVisible());
        QVERIFY(!rooms->isVisible());
        QVERIFY(!timeline->isVisible());
        auto *composer = item("composerCard");
        QVERIFY(!composer || !composer->isVisible());
        auto *settingsLoader = m_window->findChild<QQuickItem *>(
            QStringLiteral("settingsViewLoader"));
        QVERIFY(settingsLoader);
        QVERIFY(settingsLoader->isVisible());
        QCOMPARE(settingsLoader->width(),
                 m_window->contentItem()->width());
        QVERIFY(settingsLoader->height()
                >= m_window->contentItem()->height() - 40);
        QVERIFY(item("settingsHeaderTitle"));
    }

    void settingsHasNoHorizontalClippingAt1374()
    {
        // All three featured theme cards are fully inside the content
        // area, and the appearance column never overflows horizontally.
        const qreal windowWidth = m_window->contentItem()->width();
        for (int id : { 8, 9, 10 }) {
            auto *card = item(qPrintable(
                QStringLiteral("featuredThemeCard_%1").arg(id)));
            QVERIFY2(card, qPrintable(QString::number(id)));
            QVERIFY(card->isVisible());
            const QPointF right =
                card->mapToScene(QPointF(card->width(), 0));
            QVERIFY2(right.x() <= windowWidth + 0.5,
                     qPrintable(QStringLiteral("card %1 clipped: %2 > %3")
                                    .arg(id)
                                    .arg(right.x())
                                    .arg(windowWidth)));
        }
        // The text-size slider row stays inside the viewport too.
        auto *slider = item("textScaleSlider");
        QVERIFY(slider);
        const QPointF sliderRight =
            slider->mapToScene(QPointF(slider->width(), 0));
        QVERIFY(sliderRight.x() <= windowWidth + 0.5);
    }

    void featuredThemeCardsPaintFixedPalettes()
    {
        struct Expect {
            const char *preview;
            const char *accentBar;
            QColor frame;
            QColor accent;
        };
        const Expect expected[] = {
            { "themeCardPreview_8", "themeCardAccentBar_8",
              QColor("#f7f7f5"), QColor("#12a67f") },
            { "themeCardPreview_9", "themeCardAccentBar_9",
              QColor("#101016"), QColor("#7c7ff2") },
            { "themeCardPreview_10", "themeCardAccentBar_10",
              QColor("#0e1416"), QColor("#27c2ad") },
        };
        const QImage img = m_window->grabWindow();
        QVERIFY(!img.isNull());
        for (const auto &e : expected) {
            auto *preview = item(e.preview);
            QVERIFY2(preview, e.preview);
            const QPointF framePoint = preview->mapToScene(
                QPointF(preview->width() - 14, preview->height() - 8));
            QVERIFY2(channelDelta(sampleAvg(img,
                          QRect(int(framePoint.x()), int(framePoint.y()), 2, 2)),
                          e.frame) <= kTolerance, e.preview);
            auto *accentBar = item(e.accentBar);
            QVERIFY2(accentBar, e.accentBar);
            const QPointF accentPoint = accentBar->mapToScene(
                QPointF(accentBar->width() / 2, accentBar->height() / 2));
            QVERIFY2(channelDelta(sampleAvg(img,
                          QRect(int(accentPoint.x()), int(accentPoint.y()) - 1,
                                2, 2)),
                          e.accent) <= kTolerance, e.accentBar);
        }
    }

    void selectedThemeCardRingIsNotClipped()
    {
        // The selection glow (3px accent-soft, drawn outside the card) and
        // the keyboard focus ring (2px at -6..-4) must render in full. The
        // old clip:true rectangular scissor shaved both to corner crescents
        // plus a one-device-pixel sliver protruding into the card gap — the
        // live "line sticking out beside Indigo Night" defect.
        m_controller->settings()->setTheme(SettingsManager::IndigoNightTheme);
        QCoreApplication::processEvents();
        auto *card = item("featuredThemeCard_9");
        QVERIFY(card);
        QVERIFY(!card->clip());

        const QImage selected = m_window->grabWindow();
        QVERIFY(!selected.isNull());
        // Mid-height, ~1.5px outside the right edge: inside the glow band.
        const QPointF ringPoint =
            card->mapToScene(QPointF(card->width() + 1.5, card->height() / 2));
        QVERIFY2(channelDelta(sampleAvg(selected,
                      QRect(int(ringPoint.x()), int(ringPoint.y()) - 1, 2, 2)),
                      themeColor("accentSoft")) <= kTolerance,
                 "selection glow missing outside the card edge");

        // Keyboard focus ring: 2px band at -6..-4 from the card edge.
        card->forceActiveFocus();
        QTRY_VERIFY(card->hasActiveFocus());
        const QImage focused = m_window->grabWindow();
        // The focus band is only 2px wide (-6..-4); sample a single column
        // squarely inside it.
        const QPointF focusPoint =
            card->mapToScene(QPointF(card->width() + 4.5, card->height() / 2));
        QVERIFY2(channelDelta(sampleAvg(focused,
                      QRect(int(focusPoint.x()), int(focusPoint.y()) - 1, 1, 2)),
                      themeColor("focusRing")) <= kTolerance,
                 "focus ring invisible outside the card edge");
    }

    void clickingThemeCardSwitchesInstantly()
    {
        const QColor before = themeColor("accent");
        auto *tealCard = item("featuredThemeCard_10");
        QVERIFY(tealCard);
        clickItem(tealCard);
        QCOMPARE(int(m_controller->settings()->theme()), 10);
        QTRY_VERIFY(themeColor("accent") != before);

        auto *mossCard = item("featuredThemeCard_8");
        QVERIFY(mossCard);
        clickItem(mossCard);
        QCOMPARE(int(m_controller->settings()->theme()), 8);
        m_controller->settings()->setTheme(SettingsManager::IndigoNightTheme);
        QCoreApplication::processEvents();
    }

    void matchSystemRowTogglesSystemTheme()
    {
        auto *row = item("matchSystemSwitch");
        QVERIFY(row);
        clickItem(row);
        QCOMPARE(int(m_controller->settings()->theme()), 0);
        clickItem(row);
        QVERIFY(int(m_controller->settings()->theme()) != 0);
        m_controller->settings()->setTheme(SettingsManager::IndigoNightTheme);
        QCoreApplication::processEvents();
    }

    void messageLayoutSegmentsDriveTheBackend()
    {
        auto *compact = item("messageLayoutControl_2");
        QVERIFY(compact);
        clickItem(compact);
        QCOMPARE(m_controller->settings()->messageLayout(), 2);
        auto *modern = item("messageLayoutControl_0");
        QVERIFY(modern);
        clickItem(modern);
        QCOMPARE(m_controller->settings()->messageLayout(), 0);
    }

    void textScaleSliderTracksTheBackend()
    {
        auto *slider = item("textScaleSlider");
        QVERIFY(slider);
        QCOMPARE(slider->property("from").toInt(), 90);
        QCOMPARE(slider->property("to").toInt(), 140);
        m_controller->settings()->setTextScale(120);
        QCoreApplication::processEvents();
        QCOMPARE(slider->property("value").toInt(), 120);
        m_controller->settings()->setTextScale(100);
        QCoreApplication::processEvents();
        QCOMPARE(slider->property("value").toInt(), 100);
    }

    void closingSettingsRestoresChatWithNoRightPanel()
    {
        auto *close = item("settingsCloseButton");
        QVERIFY(close);
        clickItem(close);
        QCOMPARE(int(m_controller->currentScreen()),
                 int(AppController::MainScreen));
        QTRY_VERIFY(item("spacesRail")->isVisible());
        QVERIFY(item("roomsPanel")->isVisible());
        auto *timeline = timelinePane();
        QVERIFY(timeline);
        QTRY_VERIFY(timeline->isVisible());
        QCOMPARE(m_controller->currentRoomId(),
                 QStringLiteral("!general:mock.local"));
        QCOMPARE(timeline->property("rightPanelState").toString(),
                 QStringLiteral("none"));
    }

    void openSettingsFromRoomInfoClearsThePanel()
    {
        auto *timeline = timelinePane();
        QVERIFY(timeline);
        // Simulate the member/info panel at the state level (its content is
        // a Rust-backend surface; the state machine is what matters here).
        QVERIFY(timeline->setProperty("infoOpen", true));
        QCOMPARE(timeline->property("rightPanelState").toString(),
                 QStringLiteral("info"));

        m_controller->showSettings();
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("infoOpen").toBool(), false);

        m_controller->showMain();
        QCoreApplication::processEvents();
        // Exiting Settings does NOT restore the panel.
        QCOMPARE(timeline->property("rightPanelState").toString(),
                 QStringLiteral("none"));
    }

    void openSettingsFromThreadClearsTheThread()
    {
        const QString rootId = fixtureThreadRootId();
        QVERIFY(!rootId.isEmpty());
        m_controller->thread()->openThread(
            QStringLiteral("!general:mock.local"), rootId);
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->thread()->state(),
                                  ThreadController::Ready, kSignalTimeoutMs);
        auto *timeline = timelinePane();
        QVERIFY(timeline);
        QTRY_COMPARE_WITH_TIMEOUT(
            timeline->property("rightPanelState").toString(),
            QStringLiteral("thread"), kSignalTimeoutMs);

        m_controller->showSettings();
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->thread()->state(),
                                  ThreadController::Closed, kSignalTimeoutMs);

        m_controller->showMain();
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("rightPanelState").toString(),
                 QStringLiteral("none"));
    }

    // ── SPEC 1v: 44px header, "Compact" label, search + inline controls ────

    void headerUses44pxTitleBarTreatment()
    {
        m_controller->showSettings();
        QCoreApplication::processEvents();
        auto *bar = item("settingsHeaderBar");
        QVERIFY(bar);
        QCOMPARE(bar->height(), 44.0);
        QVERIFY(item("settingsHeaderTitle"));
    }

    void messageLayoutOffersCompactNotIrc()
    {
        auto *compact = item("messageLayoutControl_2");
        QVERIFY(compact);
        QCOMPARE(compact->property("segLabel").toString(),
                 QStringLiteral("Compact"));
    }

    void navRowsExistPerSectionWithIcons()
    {
        static const char *sections[] = {
            "account", "appearance", "notifications",
            "privacy", "sessions", "labs", "about",
        };
        for (const char *key : sections) {
            auto *row = item(qPrintable(QStringLiteral("settingsNavRow_%1")
                                             .arg(QLatin1String(key))));
            QVERIFY2(row, key);
        }
    }

    void ctrlCommaFocusesSearchThenFiltersNavAndBindsInlineControl()
    {
        auto *search = item("settingsSearchField");
        QVERIFY(search);
        auto *resultsPanel = item("settingsSearchResults");
        QVERIFY(resultsPanel);
        QVERIFY(!resultsPanel->isVisible());

        search->setProperty("text", QString());
        QCoreApplication::processEvents();

        QTest::keyClick(m_window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(search->hasActiveFocus());

        // "room activity" matches exactly one entry (Appearance's "Show
        // room activity") — the nav narrows to that one section.
        const bool activityBefore = m_controller->settings()->showRoomActivity();
        search->setProperty("text", QStringLiteral("room activity"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(resultsPanel->isVisible());

        auto *appearanceNav = item("settingsNavRow_appearance");
        auto *accountNav = item("settingsNavRow_account");
        QVERIFY(appearanceNav && accountNav);
        QTRY_VERIFY(appearanceNav->isVisible());
        QVERIFY(!accountNav->isVisible());

        auto *resultRow = item("settingsSearchResult_0");
        QVERIFY(resultRow);

        // The inline control is the SAME SettingsManager property the real
        // Appearance-pane control binds — flipping it here must flip the
        // backend directly.
        auto *inlineToggle = item("settingsSearchInlineShowRoomActivity_0");
        QVERIFY(inlineToggle);
        QVERIFY(inlineToggle->isVisible());
        QMetaObject::invokeMethod(inlineToggle, "toggled");
        QCOMPARE(m_controller->settings()->showRoomActivity(), !activityBefore);
        // Restore so later tests are not affected by ordering.
        QMetaObject::invokeMethod(inlineToggle, "toggled");
        QCOMPARE(m_controller->settings()->showRoomActivity(), activityBefore);

        // Clearing the search restores the full nav.
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
        QTRY_VERIFY(!resultsPanel->isVisible());
        QTRY_VERIFY(accountNav->isVisible());
    }

    void noQmlWarnings()
    {
        QCOMPARE(m_warnings, QStringList{});
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    SettingsShellQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "SettingsShellQmlTest.moc"
