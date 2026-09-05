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

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "profile/ProfileBioManager.h"
#include "app/PinnedMessagesController.h"
#include "app/RoomInfoController.h"
#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "auth/AuthManager.h"
#include "gif/GifSearchController.h"
#include "gif/GifStarredStore.h"
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

    // Bring `target` inside its nearest Flickable ancestor's viewport, but
    // ONLY when it is actually outside it — scrolling an already-visible
    // control would move the page under tests that assert positions.
    //
    // Settings pages grew taller in the 2026-08-21 UI round, and a click at
    // an item's scene centre then landed OUTSIDE the window. Qt reports that
    // as "Mouse event at X, Y occurs outside target window" and drops it, so
    // the suite failed on a control that works perfectly — the click simply
    // never arrived. A user scrolls before clicking; so does this.
    void ensureVisible(QQuickItem *target)
    {
        QQuickItem *flick = target->parentItem();
        while (flick && !flick->inherits("QQuickFlickable"))
            flick = flick->parentItem();
        if (!flick)
            return;
        auto *content = flick->property("contentItem").value<QQuickItem *>();
        if (!content)
            return;
        const qreal viewH = flick->height();
        const qreal top = target->mapToItem(content, QPointF(0, 0)).y();
        const qreal bottom = top + target->height();
        const qreal contentY = flick->property("contentY").toReal();
        if (top >= contentY && bottom <= contentY + viewH)
            return; // already fully visible
        const qreal contentH = flick->property("contentHeight").toReal();
        const qreal want = qBound(0.0, top - viewH / 2 + target->height() / 2,
                                  qMax(0.0, contentH - viewH));
        flick->setProperty("contentY", want);
        QCoreApplication::processEvents();
    }

    // Y within the nearest Flickable's contentItem — i.e. the position that
    // does NOT change when the page scrolls. Reflow guards must measure this
    // rather than a scene coordinate, or a scroll (which is not a reflow)
    // reads as content having moved.
    qreal contentPosY(QQuickItem *target) const
    {
        QQuickItem *flick = target->parentItem();
        while (flick && !flick->inherits("QQuickFlickable"))
            flick = flick->parentItem();
        auto *content = flick
            ? flick->property("contentItem").value<QQuickItem *>() : nullptr;
        return content ? target->mapToItem(content, QPointF(0, 0)).y()
                       : target->mapToScene(QPointF(0, 0)).y();
    }

    void clickItem(QQuickItem *target)
    {
        ensureVisible(target);
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
                    for (const auto &w : warnings) {
                        const QString text = w.toString();
                        // The mock backend hands out media URLs on a host that
                        // does not resolve, and timeline rows now activate
                        // their media whenever they are genuinely inside the
                        // viewport — including in an offscreen run, where the
                        // previous virtualized view never instantiated them at
                        // all. That is a DNS failure in the fixture, not a QML
                        // defect, and it must not mask real warnings: only
                        // this exact unreachable-host message is dropped.
                        if (text.contains(QLatin1String(
                                "QQuickImage: Host mock.local not found")))
                            continue;
                        m_warnings.append(text);
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

    // 2026-08-18 tester report #2: "GIF settings reset every close/launch"
    // (Win11). The persisted VALUES were fine; the suspicion is the
    // combos' creation-time indexOfValue binding showing defaults. This
    // case runs FIRST among the slots so the Settings screen instantiates
    // fresh with non-default values already stored — exactly the relaunch
    // shape the tester saw.
    void gifSettingsCombosDisplayPersistedValuesOnFirstOpen()
    {
        m_controller->settings()->setGifAutoplay(2);        // Never
        m_controller->settings()->setGifSafeSearch(0);      // G — strict
        m_controller->settings()->setGifPreferredProvider(
            QStringLiteral("klipy"));
        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();
        auto *autoplay = item("gifAutoplayCombo");
        auto *rating = item("gifRatingCombo");
        auto *provider = item("gifProviderCombo");
        QVERIFY(autoplay && rating && provider);
        QTRY_COMPARE_WITH_TIMEOUT(
            autoplay->property("currentValue").toInt(), 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(
            rating->property("currentValue").toInt(), 0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(
            provider->property("currentValue").toString(),
            QStringLiteral("klipy"), 3000);
        // Restore the shared shell state for the section-sensitive tests
        // that follow (they expect a fresh Settings open on the default
        // section with the chat shell visible beneath).
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        m_controller->showMain();
        QTRY_VERIFY_WITH_TIMEOUT(
            item("spacesRail") && item("spacesRail")->isVisible(), 3000);
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
        for (int id : { 11, 8, 9, 10 }) {
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

    void featuredThemeCardsPaintTheirRealPalettes()
    {
        // Every featured card now reads AppTheme.paletteForTheme(id) — no
        // card carries a colour of its own. Assert against the SAME raw
        // per-theme literals that function returns, so this test moves with
        // a palette retune instead of pinning yesterday's copy of it.
        //
        // Read the underscore literals, never the routed aliases
        // (AppTheme.stormDeep/bolt are `storm ? _sto* : <active theme>`, so
        // they only equal Storm's value while Storm is active — sampling
        // those would compare each card against whatever theme the test
        // happens to run under).
        //
        // History: cards 8/9/10 used to hold hand-copied hex literals and
        // had drifted far enough that Indigo Night and Deep Teal previewed
        // a room list lighter than their canvas while both real themes ship
        // it darker. The literals — and this test's copies of them — are
        // gone; drift is now structurally impossible.
        struct Expect {
            const char *preview;
            const char *accentBar;
            QColor frame;
            QColor accent;
        };
        const Expect expected[] = {
            { "themeCardPreview_8", "themeCardAccentBar_8",
              themeColor("_mosBg"), themeColor("_mosAccent") },
            { "themeCardPreview_9", "themeCardAccentBar_9",
              themeColor("_indBg"), themeColor("_indAccent") },
            { "themeCardPreview_10", "themeCardAccentBar_10",
              themeColor("_teaBg"), themeColor("_teaAccent") },
            { "themeCardPreview_11", "themeCardAccentBar_11",
              themeColor("_stoDeep"), themeColor("_stoBolt") },
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
        // Storm: the glow is bolt at 18% alpha compositing over the
        // stormDeep content backdrop — compute that blend as the expected
        // sample instead of the old themed accentSoft.
        const QColor glowInk = themeColor("bolt");
        const QColor glowBase = themeColor("stormDeep");
        const QColor glowBlend(
            int(0.18 * glowInk.red() + 0.82 * glowBase.red()),
            int(0.18 * glowInk.green() + 0.82 * glowBase.green()),
            int(0.18 * glowInk.blue() + 0.82 * glowBase.blue()));
        const QPointF ringPoint =
            card->mapToScene(QPointF(card->width() + 1.5, card->height() / 2));
        QVERIFY2(channelDelta(sampleAvg(selected,
                      QRect(int(ringPoint.x()), int(ringPoint.y()) - 1, 2, 2)),
                      glowBlend) <= kTolerance,
                 "selection glow missing outside the card edge");

        // Keyboard focus ring: 2px band at -6..-4 from the card edge.
        card->forceActiveFocus();
        QTRY_VERIFY(card->hasActiveFocus());
        const QImage focused = m_window->grabWindow();
        // The focus band is only 2px wide (-6..-4); sample a single column
        // squarely inside it.
        const QPointF focusPoint =
            card->mapToScene(QPointF(card->width() + 4.5, card->height() / 2));
        // Storm: focus rings in Settings ink bolt.
        QVERIFY2(channelDelta(sampleAvg(focused,
                      QRect(int(focusPoint.x()), int(focusPoint.y()) - 1, 1, 2)),
                      themeColor("bolt")) <= kTolerance,
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

        // Storm (11) — the brand card, first/primary in the featured row —
        // switches instantly like every other featured card.
        auto *stormCard = item("featuredThemeCard_11");
        QVERIFY(stormCard);
        clickItem(stormCard);
        QCOMPARE(int(m_controller->settings()->theme()), 11);
        QTRY_COMPARE(themeColor("accent"), themeColor("bolt"));

        auto *mossCard = item("featuredThemeCard_8");
        QVERIFY(mossCard);
        clickItem(mossCard);
        QCOMPARE(int(m_controller->settings()->theme()), 8);
        m_controller->settings()->setTheme(SettingsManager::IndigoNightTheme);
        QCoreApplication::processEvents();
    }

    void indigoNightLeadsAndTheMiniRowNeverDuplicatesAFeaturedCard()
    {
        // Indigo Night is the flagship as of 2026-08-25 (maintainer's call),
        // so it sorts first among the featured cards. Storm led until then;
        // it is still featured and still the shell's own chrome, and the
        // secondary "MORE THEMES" row must never render any featured card a
        // second time.
        auto *flow = item("featuredThemeFlow");
        QVERIFY(flow);
        auto *indigoCard = item("featuredThemeCard_9");
        auto *mossCard = item("featuredThemeCard_8");
        auto *stormCard = item("featuredThemeCard_11");
        QVERIFY(indigoCard && mossCard && stormCard);
        QVERIFY2(indigoCard->x() <= mossCard->x()
                     && indigoCard->y() <= mossCard->y(),
                 "Indigo Night must sort before Moss Light in the featured row");
        QVERIFY2(stormCard->y() > indigoCard->y()
                     || (stormCard->y() == indigoCard->y()
                         && stormCard->x() > indigoCard->x()),
                 "Storm must sort after Indigo Night in the featured row");
        // "MORE THEMES" only lists the 7 non-featured presets — Storm is
        // never duplicated there. Moss (8) is the WRONG sanity check here:
        // it is itself one of the four featured cards (8/9/10/11), so the
        // filter correctly excludes it too — asserting its presence would
        // fail by design, not prove anything about Storm. Lightning Light
        // (1) is a genuinely non-featured preset and must still be listed.
        QVERIFY2(!item("miniThemeCard_11"),
                 "Storm must not also render in the MORE THEMES row");
        QVERIFY2(item("miniThemeCard_1"),
                 "the MORE THEMES row must still list the non-featured presets");
    }

    void interactingWithOrdinaryRowsNeverReflowsContentBelow()
    {
        // v0.6.5 (C8): pressing, focusing, or toggling an ORDINARY settings
        // row/control must never drag content below it down. An exhaustive
        // static read of the whole SettingsScreen.qml file (the file that
        // motivated this test) found no reproducible hover/press/focus-
        // driven reflow anywhere in the current code — every focus ring and
        // selection glow is drawn as an absolute overlay
        // (anchors.fill + negative anchors.margins), never a Layout
        // sibling, and every control's implicitHeight is a hard constant.
        // This guard exists to keep it that way. It deliberately does NOT
        // cover the three INTENTIONAL disclosure expanders (recovery
        // diagnostics, Danger Zone Show/Hide, session verification reveal)
        // — their whole job is to grow the content below them.
        m_controller->settings()->setTheme(SettingsManager::IndigoNightTheme);
        QCoreApplication::processEvents();

        // Anchor: the message-layout control sits below the featured/mini
        // theme cards AND the match-system row. Neither toggling the
        // match-system switch nor keyboard-focusing a theme card may move
        // it even one pixel.
        auto *anchor = item("messageLayoutControl");
        QVERIFY(anchor);
        // Scroll-invariant: clickItem() may scroll a control into view,
        // and a scroll is not a reflow.
        const qreal anchorY = contentPosY(anchor);

        auto *matchSwitch = item("matchSystemSwitch");
        QVERIFY(matchSwitch);
        clickItem(matchSwitch);
        QCOMPARE(contentPosY(anchor), anchorY);
        clickItem(matchSwitch); // toggle back off "match system"
        QCoreApplication::processEvents();
        QCOMPARE(contentPosY(anchor), anchorY);

        auto *mossCard = item("featuredThemeCard_8");
        QVERIFY(mossCard);
        mossCard->forceActiveFocus();
        QTRY_VERIFY(mossCard->hasActiveFocus());
        QCOMPARE(contentPosY(anchor), anchorY);

        // The Timeline card: "Show room activity" sits directly above the
        // wheel-speed combo. Toggling the checkbox must not move the combo.
        auto *wheelCombo = item("timelineWheelSpeedCombo");
        QVERIFY(wheelCombo);
        const qreal comboY = contentPosY(wheelCombo);
        auto *activityCheck = item("showRoomActivityCheck");
        QVERIFY(activityCheck);
        clickItem(activityCheck);
        QCOMPARE(contentPosY(wheelCombo), comboY);
        clickItem(activityCheck); // restore
        QCoreApplication::processEvents();
        QCOMPARE(contentPosY(wheelCombo), comboY);

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

        // 2026-08-15 report: past ~115% both thumbs flipped to boltInk
        // (near-black navy) and read as disabled. The thumb rides the
        // fill's boundary, never sits on it — it stays white across the
        // whole range. This fails on the pre-fix visualPosition > 0.5
        // ternary.
        m_controller->settings()->setTextScale(140);
        QCoreApplication::processEvents();
        auto *handle = slider->property("handle").value<QQuickItem *>();
        QVERIFY(handle);
        QCOMPARE(handle->property("color").value<QColor>(),
                 QColor(QStringLiteral("#FFFFFF")));
        auto *zoom = item("interfaceZoomSlider");
        QVERIFY(zoom);
        m_controller->settings()->setInterfaceZoom(150);
        QCoreApplication::processEvents();
        auto *zoomHandle = zoom->property("handle").value<QQuickItem *>();
        QVERIFY(zoomHandle);
        QCOMPARE(zoomHandle->property("color").value<QColor>(),
                 QColor(QStringLiteral("#FFFFFF")));
        m_controller->settings()->setInterfaceZoom(100);

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

    // ── Room info tabs wrap instead of running off the panel ──────────────
    //
    // Reported with a screenshot: six tabs in a narrow panel "go off screen,
    // they should wrap to another line or something". The single strip is
    // kept while it fits and a two-row pair takes over when it overflows —
    // driven by the REAL layout at two real widths, not by poking the policy
    // property, because a wrap that never triggers looks exactly like one
    // that does in a test that sets `tabsWrap` by hand.
    void roomInfoTabsWrapIntoTwoRowsWhenThePanelIsNarrow()
    {
        auto *timeline = timelinePane();
        QVERIFY(timeline);
        m_controller->showMain();
        QCoreApplication::processEvents();
        auto *panel = item("roomInfoPanel");
        QVERIFY(panel);
        auto *strip = item("roomInfoTabs");
        QVERIFY(strip);

        // Wide: one row, the pair absent.
        m_controller->settings()->setSidePanelWidth(700);
        QVERIFY(timeline->setProperty("infoOpen", true));
        QTRY_VERIFY(panel->isVisible());
        QTRY_VERIFY2(panel->width() > 500,
                     qPrintable(QString::number(panel->width())));
        QTRY_VERIFY(!strip->property("overflowing").toBool());
        QTRY_VERIFY2(strip->height() > 0,
                     "the single strip must keep its height while it fits");
        QVERIFY(!item("roomInfoTabsRow0"));

        // Narrow: the strip collapses and two rows carry every tab. The
        // fixture's four tabs (236 px) fit the 260 px floor, so give it the
        // Pinned tab the real backend always offers — five tabs is the
        // reported shape — by turning pins on and re-opening the room so
        // `pinnedAvailable` re-reads it.
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->mockSupportsPinnedMessages = true;
        // `supported` is read live off the client; the binding learns of a
        // change through this signal, which is what the backend announces.
        Q_EMIT m_controller->pinned()->supportedChanged();
        // The Pinned tab also needs the panel to be showing the room the pin
        // controller tracks — what openForRoom() sets when the (i) button
        // opens the panel, done here by hand because the mock shell opened
        // it at the state level above.
        m_controller->roomInfo()->setRoomId(QStringLiteral("!general:mock.local"));
        m_controller->pinned()->setRoomId(QStringLiteral("!general:mock.local"));
        QTRY_COMPARE(strip->property("model").toList().size(), 5);
        m_controller->settings()->setSidePanelWidth(260);
        QTRY_COMPARE(panel->width(), 260.0);
        QTRY_VERIFY2(strip->property("overflowing").toBool(),
                     qPrintable(QStringLiteral("natural %1 px in %2 px, %3 tabs")
                                    .arg(strip->implicitWidth()).arg(strip->width())
                                    .arg(strip->property("model").toList().size())));
        QTRY_COMPARE(strip->height(), 0.0);
        auto *row0 = item("roomInfoTabsRow0");
        auto *row1 = item("roomInfoTabsRow1");
        QVERIFY2(row0 && row1, "the wrapped pair was not built");
        QTRY_VERIFY2(row0->height() > 0 && row1->height() > 0,
                     qPrintable(QStringLiteral("rows %1 / %2 px")
                                    .arg(row0->height()).arg(row1->height())));
        QVERIFY(row0->isVisible() && row1->isVisible());
        QVERIFY(row0->width() > 0);
        const int all = strip->property("model").toList().size();
        const int split = row0->property("model").toList().size()
                          + row1->property("model").toList().size();
        QCOMPARE(split, all);
        QVERIFY(row0->property("model").toList().size() >= 2);
        // Neither half may itself overflow the panel, or nothing was gained.
        QVERIFY(!row0->property("overflowing").toBool());
        QVERIFY(!row1->property("overflowing").toBool());

        // Wide again: the single strip comes back and the pair goes.
        m_controller->settings()->setSidePanelWidth(700);
        QTRY_VERIFY(strip->height() > 0);
        QTRY_VERIFY(!item("roomInfoTabsRow0"));
        QVERIFY(timeline->setProperty("infoOpen", false));
        mock->mockSupportsPinnedMessages = false;
        Q_EMIT m_controller->pinned()->supportedChanged();
        m_controller->roomInfo()->setRoomId(QString());
    }

    // ── Settings is built once and kept ──────────────────────────────────
    //
    // Reported: "when i open settings it takes like a second to open the
    // menu". The loader used to follow the current screen, rebuilding the
    // whole screen on every open. The pinned mechanism is identity: the SAME
    // item survives a close and serves the next open, hidden in between —
    // and a section requested while it is alive still lands, which used to
    // ride on Component.onCompleted alone.
    void settingsStaysBuiltBetweenOpensAndStillLandsOnTheRequestedSection()
    {
        auto *loader = item("settingsViewLoader");
        QVERIFY(loader);
        m_controller->showSettings();
        QCoreApplication::processEvents();
        QTRY_VERIFY(loader->isVisible());
        QObject *first = loader->property("item").value<QObject *>();
        QVERIFY(first);

        m_controller->showMain();
        QCoreApplication::processEvents();
        QTRY_VERIFY(!loader->isVisible());
        QCOMPARE(loader->property("item").value<QObject *>(), first);

        m_controller->showSettingsSection(QStringLiteral("sessions"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(loader->isVisible());
        QCOMPARE(loader->property("item").value<QObject *>(), first);
        QCOMPARE(first->property("section").toString(),
                 QStringLiteral("sessions"));
        // Focus lands in the screen on a re-open, not left on the composer.
        QTRY_VERIFY(first->property("activeFocus").toBool());

        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        QCOMPARE(first->property("section").toString(),
                 QStringLiteral("appearance"));
        m_controller->showMain();
        QCoreApplication::processEvents();
    }

    // Found in review of the kept-alive screen: its window-level Escape
    // Shortcut used to die with the screen. Kept alive and hidden, it stayed
    // armed — and two enabled Shortcuts on one sequence make Qt fire
    // NEITHER, so Escape stopped closing the info panel on the main screen.
    // Driven with a real key on the window, after a real open and close.
    void escapeStillClosesTheInfoPanelAfterSettingsHasBeenOpened()
    {
        m_controller->showSettings();
        QCoreApplication::processEvents();
        m_controller->showMain();
        QCoreApplication::processEvents();
        auto *timeline = timelinePane();
        QVERIFY(timeline);
        QVERIFY(timeline->setProperty("infoOpen", true));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QTRY_VERIFY2(!timeline->property("infoOpen").toBool(),
                     "Escape no longer reaches the timeline's own shortcut");
        QCOMPARE(int(m_controller->currentScreen()),
                 int(AppController::MainScreen));
    }

    // "add an option for custom display name color, not just a few premade
    // options": a tenth swatch opens the colour picker, and Apply is the one
    // server write — the picker reports every drag step. The written value
    // must be a colour that is none of the nine slots.
    void aCustomNameColourReachesTheServerOnApply()
    {
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        m_controller->showSettingsSection(QStringLiteral("account"));
        QCoreApplication::processEvents();
        auto *swatch = item("nameColorCustomSwatch");
        QVERIFY2(swatch, "the custom swatch is missing beside the nine");
        auto *picker = item("nameColorPicker");
        QVERIFY(picker);
        QVERIFY(!picker->isVisible());
        clickItem(swatch);
        QTRY_VERIFY(picker->isVisible());
        // The picker's own signal, as a drag would raise it.
        QVERIFY(QMetaObject::invokeMethod(picker, "picked",
                                          Q_ARG(QColor, QColor(0x12, 0x34, 0x56))));
        QCoreApplication::processEvents();
        QCOMPARE(picker->property("draft").toString(), QStringLiteral("#123456"));
        QVERIFY2(mock->mockNameColors.value(mock->currentUserId()).isEmpty(),
                 "nothing may be written before Apply");
        auto *apply = item("applyNameColorButton");
        QVERIFY(apply);
        QTRY_VERIFY(apply->isEnabled());
        clickItem(apply);
        QTRY_COMPARE(mock->mockNameColors.value(mock->currentUserId()),
                     QStringLiteral("#123456"));
        // Leave the shell on the main screen for the cases that follow.
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        m_controller->showMain();
        QCoreApplication::processEvents();
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

    // ── SPEC 1v: header, "Compact" label, search + inline controls ────────
    //
    // The header was a 44 px title bar; the room header band it replaces on
    // screen is AppTheme.headerBandHeight (60). Reported: "the top part where
    // it says Lightning is thicker than the one in settings … so opening
    // settings feels more smooth and less changy". The invariant is EQUALITY
    // with the band it swaps in for, read off the live room header rather
    // than a literal, so the two cannot drift apart again.

    void headerIsAsTallAsTheRoomHeaderBandItReplaces()
    {
        auto *band = item("roomHeaderBand");
        QVERIFY(band);
        m_controller->showSettings();
        QCoreApplication::processEvents();
        auto *bar = item("settingsHeaderBar");
        QVERIFY(bar);
        QVERIFY(bar->height() >= 44.0);
        QCOMPARE(bar->height(), band->height());
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

    // v0.6.6 (review HIGH-2): the client-local starred-GIF store gets its
    // own visible count/size row (never folded into Favorites/Recents,
    // which hold no actual file bytes) and a confirmed destructive Clear
    // All — this is real GifStarredStore state, real QML bindings, and a
    // real Dialog, not a source-scan pin.
    void starredGifsSettingsRowReflectsStoreAndClearAllEmptiesIt()
    {
        auto *navRow = item("settingsNavRow_privacy");
        QVERIFY(navRow);
        clickItem(navRow);
        QCoreApplication::processEvents();

        auto *summary = item("starredGifsSummaryLabel");
        auto *clearButton = item("clearStarredGifsButton");
        QVERIFY(summary);
        QVERIFY(clearButton);
        QCOMPARE(summary->property("text").toString(),
                 QStringLiteral("0 image(s), 0 B — kept on this device only and removed when you sign out of this account."));
        QVERIFY(!clearButton->property("enabled").toBool());

        auto *store = m_controller->gif()->starredStore();
        const QByteArray gif = QByteArray("GIF89a\x10\x00\x10\x00", 10);
        store->starBytes(QStringLiteral("mk-settings-test"), gif);
        QCoreApplication::processEvents();

        // The row is a live binding off the store's own count/totalBytes
        // properties — no manual refresh needed.
        QCOMPARE(summary->property("text").toString(),
                 QStringLiteral("1 image(s), 10 B — kept on this device only and removed when you sign out of this account."));
        QVERIFY(clearButton->property("enabled").toBool());

        clickItem(clearButton);
        QCoreApplication::processEvents();
        auto *confirmDialog = m_window->findChild<QObject *>(
            QStringLiteral("starredGifsClearConfirm"));
        QVERIFY(confirmDialog);
        QVERIFY(confirmDialog->property("visible").toBool());

        // accept() drives the exact same onAccepted path a real "Yes" click
        // would, without depending on the modal popup's screen position.
        QMetaObject::invokeMethod(confirmDialog, "accept");
        QCoreApplication::processEvents();

        QCOMPARE(store->count(), 0);
        QCOMPARE(summary->property("text").toString(),
                 QStringLiteral("0 image(s), 0 B — kept on this device only and removed when you sign out of this account."));
        QVERIFY(!clearButton->property("enabled").toBool());
    }

    // The by-id palette resolver and the live semantic aliases must agree,
    // for every theme and every key. They drifted once already — the
    // resolver fell back to a translucent accent where the aliases fall back
    // to `selected` / `borderStrong`, so a Settings preview card painted
    // chrome the running theme never renders — and now the custom-theme
    // editor's whole preview is painted from the resolver, which makes a
    // second divergence a whole fake window rather than one card.
    void previewPaletteMatchesLiveTokens()
    {
        // Left: key in paletteForTheme(). Right: the live AppTheme alias it
        // must equal. They are spelled the same on purpose; the pair list
        // exists so a key can never be added to one side alone.
        const QStringList keys = {
            QStringLiteral("background"),      QStringLiteral("rail"),
            QStringLiteral("sidebar"),         QStringLiteral("surface"),
            QStringLiteral("cardElevated"),    QStringLiteral("hover"),
            QStringLiteral("selected"),        QStringLiteral("selectedHover"),
            QStringLiteral("selectedText"),    QStringLiteral("border"),
            QStringLiteral("borderStrong"),    QStringLiteral("inputBackground"),
            QStringLiteral("codeBlock"),       QStringLiteral("accent"),
            QStringLiteral("accentHover"),     QStringLiteral("accentPressed"),
            QStringLiteral("accentText"),      QStringLiteral("accentSoft"),
            QStringLiteral("accentBorder"),    QStringLiteral("link"),
            QStringLiteral("textPrimary"),     QStringLiteral("textSecondary"),
            QStringLiteral("textMuted"),       QStringLiteral("textDisabled"),
            QStringLiteral("icon"),            QStringLiteral("sectionLabelColor"),
            QStringLiteral("ownBubble"),       QStringLiteral("ownBubbleText"),
            QStringLiteral("otherBubble"),     QStringLiteral("otherBubbleText"),
            QStringLiteral("embedSurface"),    QStringLiteral("embedBorder"),
            QStringLiteral("reactionBackground"),
            QStringLiteral("reactionBorder"),  QStringLiteral("reactionInk"),
            QStringLiteral("unreadBadge"),     QStringLiteral("mentionHighlight"),
            QStringLiteral("mentionBadge"),    QStringLiteral("success"),
            QStringLiteral("danger"),
        };
        const int original = int(m_controller->settings()->theme());
        for (int id = 1; id <= 11; ++id) {
            m_controller->settings()->setTheme(SettingsManager::Theme(id));
            QCoreApplication::processEvents();
            for (const QString &key : keys) {
                QQmlExpression expr(
                    qmlContext(m_window), m_window,
                    QStringLiteral("AppTheme.paletteForTheme(%1).%2")
                        .arg(id).arg(key));
                const QVariant raw = expr.evaluate();
                const QString where =
                    QStringLiteral("theme %1, role %2").arg(id).arg(key);
                QVERIFY2(!expr.hasError(), qPrintable(where));
                QVERIFY2(raw.isValid() && !raw.isNull(),
                         qPrintable(QStringLiteral("%1 missing from "
                                                   "paletteForTheme").arg(where)));
                const QColor resolved = raw.value<QColor>();
                const QColor live = themeColor(key.toUtf8().constData());
                QVERIFY2(resolved.isValid(), qPrintable(where));
                QVERIFY2(live.isValid(), qPrintable(where));
                QVERIFY2(resolved == live,
                         qPrintable(QStringLiteral(
                             "%1: resolver %2 != live token %3")
                                        .arg(where, resolved.name(),
                                             live.name())));
            }
        }
        m_controller->settings()->setTheme(SettingsManager::Theme(original));
        QCoreApplication::processEvents();
    }

    // THE DEFECT THIS PINS: Settings > Account carried a Profile BANNER
    // editor and no way at all to change your profile PICTURE or to write
    // your own bio. You could read everyone else's bio and edit nobody's,
    // including your own, because the own-avatar path did not exist end to
    // end (no FFI, no client method, no controller command) and the bio
    // manager shipped with no editor anywhere.
    //
    // It navigates to the section rather than reading the .qml as text: a
    // source scan cannot tell whether a control is REACHABLE, and these
    // blocks sit inside the account section's own loader.
    void accountSectionOffersAPictureAndABioEditor()
    {
        m_controller->showSettingsSection(QStringLiteral("account"));
        QCoreApplication::processEvents();

        QQuickItem *avatarSection = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (avatarSection = item("ownAvatarSection")) != nullptr, 3000);
        QVERIFY2(item("chooseOwnAvatarButton") != nullptr,
                 "no way to change your own profile picture");
        QVERIFY2(item("ownAvatarPreview") != nullptr,
                 "the picture control shows no preview of what is set");

        QVERIFY2(item("ownBioSection") != nullptr,
                 "no way to write your own bio");
        auto *bioField = item("ownBioField");
        QVERIFY2(bioField != nullptr, "the bio section has no editor");
        QVERIFY2(item("saveOwnBioButton") != nullptr,
                 "the bio can be typed but never saved");

        // The editor must FOLLOW the stored value rather than being written
        // imperatively — an imperative assignment to `text` would destroy
        // that binding and leave the box showing a bio the account no
        // longer has.
        QCOMPARE(bioField->property("text").toString(),
                 m_controller->bio() ? m_controller->bio()->ownBio()
                                     : QString());

        // Restore the shell for the cases that follow.
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
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
