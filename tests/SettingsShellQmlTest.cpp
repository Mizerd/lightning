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

#include <cmath>

#include <QColor>
#include <QFile>
#include <QFont>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "app/CustomThemeStore.h"
#include "app/ModerationController.h"
#include "profile/ProfileBioManager.h"
#include "app/PinnedMessagesController.h"
#include "app/RoomInfoController.h"
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"
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

// WCAG 2.1 relative luminance and contrast, on the sRGB values a token
// carries. Kept local rather than shared with ThemeTokensTest: that suite
// reads qml/AppTheme.qml as TEXT, and what is asserted here is the colour a
// live control actually resolved under a live theme.
double relativeLuminance(const QColor &c)
{
    auto channel = [](double v) {
        v /= 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green())
           + 0.0722 * channel(c.blue());
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

// CIE L* of a rendered colour. The right measure for two FILLS, where a
// contrast RATIO flatters a pair that the eye reads as one slab: #2E3440 on
// #3B4252 is 1.24:1 and clearly two surfaces, while #D2E5D6 on #D4E6D8 is
// 1.01:1 and is not. ThemeTokensTest carries the same helper over the
// literals; this one works on the colour a live item actually resolved.
double lstarOf(const QColor &c)
{
    const double y = relativeLuminance(c);
    return y <= 0.008856 ? y * 903.3 : 116.0 * std::cbrt(y) - 16.0;
}

} // namespace

class SettingsShellQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
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

    // EVERY item of that name, not the first. Repeater delegates all share
    // one objectName, and `findChild` stops at the first hit -- which is how
    // a sweep over a list silently becomes a check on its first row.
    static void collectItems(QQuickItem *parent, const QString &name,
                             QList<QQuickItem *> &out)
    {
        if (!parent)
            return;
        if (parent->objectName() == name)
            out.append(parent);
        const auto children = parent->childItems();
        for (QQuickItem *child : children)
            collectItems(child, name, out);
    }

    QList<QQuickItem *> items(const char *name) const
    {
        QList<QQuickItem *> out;
        collectItems(m_window->contentItem(), QLatin1String(name), out);
        return out;
    }

    // A Popup is a QObject, NOT a QQuickItem, so neither findChild<QQuickItem*>
    // nor a childItems() walk can ever reach one by name -- its popupItem is
    // reparented onto the overlay and carries no objectName of its own.
    QObject *popup(const char *name) const
    {
        return m_window->findChild<QObject *>(QLatin1String(name));
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
        // AND THE DATA HOME, which was missing and is not the same thing.
        // The org/app names below scope QSettings, but AppDataPaths does not
        // read them — it composes its own root — so anything this suite
        // writes through that path landed in the REAL user data directory,
        // under the account slug `alice_mock.local` that the mock login
        // produces. Found on 2026-09-12: one interrupted run of
        // starredGifsSettingsRowReflectsStoreAndClearAllEmptiesIt left a
        // starred GIF on disk, and every run after it failed that case's
        // opening "0 image(s), 0 B" comparison — a suite that had made
        // itself permanently red with no code change anywhere.
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
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

    // A RADIO YOU CANNOT SEE IS NOT A RADIO — AND IT ONLY FAILED IN THE
    // LIGHT PALETTES.
    //
    // The resting ring on the four featured cards was AppTheme.stormTextFaint,
    // which routes to each palette's DISABLED ink. Measured on a real screen
    // against cardFoot (stormCanvas) in all eleven themes: 1.60:1 in
    // Lightning Light, 1.76 in Warm, 2.27 in Moss Light — against the 3:1
    // WCAG 1.4.11 asks of a component boundary — while Storm sat at 4.41 and
    // every dark theme passed, which is exactly why nobody saw it. A token
    // that is fine on eight palettes and invisible on three is the shape this
    // project keeps shipping, so the assertion runs over ALL ELEVEN rather
    // than over the one the suite happens to be in.
    //
    // The ring is read off the LIVE control (border.color on the real
    // Rectangle under the real theme), never off a token name: a test that
    // read AppTheme.stormTextMuted would pass while the QML still asked for
    // stormTextFaint.
    //
    // UNFIXED TREE: fails on Lightning Light at 1.60:1.
    void theUnselectedThemeCardRadioRingClearsThreeToOneOnEveryTheme()
    {
        const int restore = m_controller->settings()->theme();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        // Every preset except System (0), which is not a palette.
        const int themes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        double worst = 100.0;
        QString worstWhere;
        int checked = 0;
        for (int id : themes) {
            m_controller->settings()->setTheme(
                static_cast<SettingsManager::Theme>(id));
            QCoreApplication::processEvents();
            // Any featured card that is NOT the active theme, so the ring
            // is in its resting state and not the accent fill.
            const int cardId = (id == 9) ? 8 : 9;
            auto *radio = item(qPrintable(
                QStringLiteral("themeCardRadio_%1").arg(cardId)));
            QVERIFY2(radio, qPrintable(QStringLiteral(
                         "no themeCardRadio_%1 under theme %2")
                             .arg(cardId).arg(id)));
            QQmlExpression expr(qmlContext(radio), radio,
                                QStringLiteral("border.color"));
            const QColor ring = expr.evaluate().value<QColor>();
            QVERIFY(ring.isValid());
            const QColor foot = themeColor("stormCanvas");
            QVERIFY(foot.isValid());
            const double ratio = contrastRatio(ring, foot);
            if (ratio < worst) {
                worst = ratio;
                worstWhere = QStringLiteral("theme %1: ring %2 on %3")
                                 .arg(id).arg(ring.name(), foot.name());
            }
            ++checked;
            QVERIFY2(ratio >= 3.0,
                     qPrintable(QStringLiteral(
                         "theme %1: the resting theme-card radio ring is "
                         "%2 on %3 = %4:1, below the 3:1 a control boundary "
                         "needs")
                             .arg(id)
                             .arg(ring.name(), foot.name())
                             .arg(ratio, 0, 'f', 2)));
        }
        // Assert the COUNT of what actually varied, never the count of loop
        // iterations: a palette that never applied would otherwise be graded
        // against the previous one eleven times over.
        QCOMPARE(checked, 11);
        qInfo("theme-card radio ring: worst %.2f:1 (%s)",
              worst, qPrintable(worstWhere));

        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(restore));
        QCoreApplication::processEvents();
    }

    // ── A CARD THE COLOUR OF THE PAGE IS NOT A CARD ─────────────────────
    //
    // SettingsCard painted stormCanvas over a page painted stormDeep. Under
    // Storm those are two literals (_stoCanvas #121655 on _stoDeep #02051D);
    // under every other theme BOTH route to the palette's `background`, so
    // the card was the page and only its 1px border was left. Measured on a
    // real rendered window before the fix, card against page: Storm 1.22:1
    // and the other ten 1.00:1 EXACTLY. On Moss Light the border is #D2E2D6
    // on #D2E5D6 (1.02:1) as well, so the whole Privacy page read as one
    // flat green slab.
    //
    // Both fills are read off LIVE items — the card's own background
    // Rectangle and the screen's ground Rectangle — never off token names,
    // which would pass on the unfixed tree because the tokens themselves
    // were fine; what was wrong was which of them the card asked for.
    //
    // 5.0 dL* is a floor, not a target: measured after the fix the worst
    // real palette is Nordic at 6.3 and the rest run 8.8 to 17.4, so this
    // catches a NEW palette (or a re-routed token) that flattens the plane
    // without relitigating the existing ones. A lightness separation rather
    // than a contrast ratio, for the reason lstarOf() gives.
    //
    // UNFIXED TREE: fails on the first non-Storm theme at 0.0 dL*.
    void theSettingsCardIsVisibleAgainstThePageOnEveryTheme()
    {
        const int restore = m_controller->settings()->theme();
        // Appearance, not Privacy: every SettingsCard in the file is built
        // whatever the section, so the section does not change what is
        // measured — but the cases after this one click Appearance controls,
        // and leaving the screen somewhere else makes their clicks land on a
        // hidden item. A test must hand the next one the state it found.
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        auto *card = item("settingsCardSurface");
        auto *ground = item("settingsPageGround");
        QVERIFY2(card, "no live SettingsCard background in the settings screen");
        QVERIFY2(ground, "no live settings page ground rectangle");

        const int themes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        double worst = 1000.0;
        QString worstWhere;
        int checked = 0;
        for (int id : themes) {
            m_controller->settings()->setTheme(
                static_cast<SettingsManager::Theme>(id));
            QCoreApplication::processEvents();
            const QColor fill = card->property("color").value<QColor>();
            const QColor page = ground->property("color").value<QColor>();
            QVERIFY(fill.isValid() && page.isValid());
            const double sep = qAbs(lstarOf(fill) - lstarOf(page));
            if (sep < worst) {
                worst = sep;
                worstWhere = QStringLiteral("theme %1: %2 on %3")
                                 .arg(id).arg(fill.name(), page.name());
            }
            ++checked;
            QVERIFY2(sep >= 5.0,
                     qPrintable(QStringLiteral(
                         "theme %1: the settings card is %2 on a page of %3 "
                         "— only %4 dL* (%5:1) apart. A card that is the "
                         "colour of the page behind it is not a card, and "
                         "card grouping is the information architecture of "
                         "these pages")
                             .arg(id)
                             .arg(fill.name(), page.name())
                             .arg(sep, 0, 'f', 1)
                             .arg(contrastRatio(fill, page), 0, 'f', 2)));
        }
        // Assert the COUNT of what actually varied, never the count of loop
        // iterations.
        QCOMPARE(checked, 11);
        qInfo("settings card vs page: worst %.1f dL* (%s)",
              worst, qPrintable(worstWhere));

        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(restore));
        QCoreApplication::processEvents();
    }

    // ── AND NEITHER IS A SELECTION PILL THAT IS NEVER DRAWN ─────────────
    //
    // Same family, same root: the selected nav row filled stormSelection,
    // which is _stoSelection under Storm and the palette's `hover` under
    // every other theme — a tint designed to sit on `surface`, not on the
    // page the nav column paints. Measured on screen before the fix, fill
    // against the column: Lightning Light 1.01:1 (0.4 dL*), Moss Light
    // 1.01:1 (0.4), Warm 1.03:1 (1.0). The selected section was signalled by
    // the bolt caret and a bold label alone.
    //
    // 4.0 dL* is the floor because Moss Light is genuinely the hardest case
    // even after the fix — `selectedHover` puts it at 5.5 where the next
    // worst is Lightning Light at 10.5 and the dark themes run 18.7 to 37.0.
    // Moss Light's `selected` would have been 3.4 and `hover` 0.4, so the
    // floor is set where it separates the fix from both of the tokens that
    // do not work rather than where it flatters the result.
    //
    // UNFIXED TREE: fails on Lightning Light at 0.4 dL*.
    void theSelectedNavRowHasAVisibleFillOnEveryTheme()
    {
        const int restore = m_controller->settings()->theme();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        auto *row = item("settingsNavRow_appearance");
        auto *fill = item("settingsNavRowFill_appearance");
        auto *column = item("settingsNavColumn");
        QVERIFY2(row && fill && column, "the appearance nav row is not live");
        // Guard the premise: an unhighlighted row paints "transparent", and
        // a transparent sample would read as pure black and PASS on every
        // light theme while testing nothing at all.
        QVERIFY2(row->property("highlighted").toBool(),
                 "the appearance nav row is not the highlighted one");

        const int themes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        double worst = 1000.0;
        QString worstWhere;
        int checked = 0;
        for (int id : themes) {
            m_controller->settings()->setTheme(
                static_cast<SettingsManager::Theme>(id));
            QCoreApplication::processEvents();
            const QColor pill = fill->property("color").value<QColor>();
            const QColor nav = column->property("color").value<QColor>();
            QVERIFY(pill.isValid() && nav.isValid());
            QVERIFY2(pill.alpha() == 255,
                     "the selected pill must be opaque; a translucent sample "
                     "is not the colour the reader sees");
            const double sep = qAbs(lstarOf(pill) - lstarOf(nav));
            if (sep < worst) {
                worst = sep;
                worstWhere = QStringLiteral("theme %1: %2 on %3")
                                 .arg(id).arg(pill.name(), nav.name());
            }
            ++checked;
            QVERIFY2(sep >= 4.0,
                     qPrintable(QStringLiteral(
                         "theme %1: the selected settings nav row fills %2 "
                         "on a column of %3 — only %4 dL* (%5:1) apart, so "
                         "the selection pill is not drawn at all")
                             .arg(id)
                             .arg(pill.name(), nav.name())
                             .arg(sep, 0, 'f', 1)
                             .arg(contrastRatio(pill, nav), 0, 'f', 2)));
        }
        QCOMPARE(checked, 11);
        qInfo("selected nav row vs column: worst %.1f dL* (%s)",
              worst, qPrintable(worstWhere));

        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(restore));
        QCoreApplication::processEvents();
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
        QTRY_VERIFY2(strip->isVisible() && strip->height() > 0,
                     "the single strip must be shown while it fits");
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
        // It LEAVES the layout rather than collapsing to zero height: a
        // zero-height row whose maximum was bound to its own implicit height
        // is what made the panel's ColumnLayout loop.
        QTRY_VERIFY2(!strip->isVisible(),
                     "the single strip must leave the layout when wrapped");
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
        QTRY_VERIFY(strip->isVisible());
        QTRY_VERIFY(!item("roomInfoTabsRow0"));
        QVERIFY(timeline->setProperty("infoOpen", false));
        mock->mockSupportsPinnedMessages = false;
        Q_EMIT m_controller->pinned()->supportedChanged();
        m_controller->roomInfo()->setRoomId(QString());
    }

    // ── Opening the panel at a wrap width must not tear the layout mid-pass ─
    //
    // Two core dumps on 2026-09-06, both on the (i) click: the tab strip's
    // `overflowing` was settled by the same layout pass that then rewrote
    // the wrapped-rows Repeater's model, rebuilding delegate trees from
    // inside the layout engine's iteration (see RoomInfoPanel.qml's note on
    // tabsWrap for the stack). The previous case opens WIDE and narrows
    // later, which never makes the wrap decision on the panel's first pass.
    // This one does what the reader did: open at a width that already needs
    // the wrap, several times, then sweep across the threshold both ways.
    //
    // HONEST SCOPE: this does NOT reproduce the crash — offscreen drives no
    // continuous frames, so the layout settles instead of re-running every
    // frame (it passes on the unfixed tree, under ASan too). It is a
    // behaviour gate: opening straight into the wrapped state must build
    // the two rows and survive the threshold in both directions.
    void roomInfoOpeningAtAWrapWidthDoesNotTearTheLayoutMidPass()
    {
        auto *timeline = timelinePane();
        QVERIFY(timeline);
        m_controller->showMain();
        QCoreApplication::processEvents();
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->mockSupportsPinnedMessages = true;
        Q_EMIT m_controller->pinned()->supportedChanged();
        m_controller->roomInfo()->setRoomId(QStringLiteral("!general:mock.local"));
        m_controller->pinned()->setRoomId(QStringLiteral("!general:mock.local"));
        auto *strip = item("roomInfoTabs");
        QVERIFY(strip);
        QTRY_COMPARE(strip->property("model").toList().size(), 5);

        // Stored narrow BEFORE the open, as for a reader whose panel was
        // last dragged narrow: the wrap decision lands on the panel's very
        // first layout pass.
        m_controller->settings()->setSidePanelWidth(260);
        for (int round = 0; round < 6; ++round) {
            QVERIFY(timeline->setProperty("infoOpen", true));
            QTRY_VERIFY2(item("roomInfoTabsRow0") != nullptr,
                         qPrintable(QStringLiteral("round %1: wrapped rows absent")
                                        .arg(round)));
            QTRY_VERIFY(!strip->isVisible());
            QVERIFY(timeline->setProperty("infoOpen", false));
            QTest::qWait(20);
        }

        // Across the threshold and back, a frame per step.
        QVERIFY(timeline->setProperty("infoOpen", true));
        QTRY_VERIFY(item("roomInfoTabsRow0") != nullptr);
        for (int w = 260; w <= 700; w += 10) {
            m_controller->settings()->setSidePanelWidth(w);
            QTest::qWait(16);
        }
        QTRY_VERIFY(!item("roomInfoTabsRow0"));
        for (int w = 700; w >= 260; w -= 10) {
            m_controller->settings()->setSidePanelWidth(w);
            QTest::qWait(16);
        }
        QTRY_VERIFY(item("roomInfoTabsRow0") != nullptr);
        auto *row0 = item("roomInfoTabsRow0");
        auto *row1 = item("roomInfoTabsRow1");
        QVERIFY(row0 && row1);
        const int all = strip->property("model").toList().size();
        QCOMPARE(row0->property("model").toList().size()
                     + row1->property("model").toList().size(), all);

        m_controller->settings()->setSidePanelWidth(700);
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

    // The panel is hidden with the rest of the chat shell while Settings is
    // up, and COMES BACK with its section when Settings closes — reported:
    // "if I go to settings and back to the room it closes the preview". It
    // used to stay closed by design (99c9e12); the maintainer reversed that.
    void openSettingsFromRoomInfoHidesThePanelAndRestoresIt()
    {
        auto *timeline = timelinePane();
        QVERIFY(timeline);
        // Simulate the member/info panel at the state level (its content is
        // a Rust-backend surface; the state machine is what matters here).
        QVERIFY(timeline->setProperty("infoOpen", true));
        auto *panel = item("roomInfoPanel");
        QVERIFY(panel);
        QVERIFY(panel->setProperty("section", QStringLiteral("media")));
        QCOMPARE(timeline->property("rightPanelState").toString(),
                 QStringLiteral("info"));

        m_controller->showSettings();
        QCoreApplication::processEvents();
        QCOMPARE(timeline->property("infoOpen").toBool(), false);

        m_controller->showMain();
        QCoreApplication::processEvents();
        QTRY_VERIFY2(timeline->property("infoOpen").toBool(),
                     "the panel must come back after Settings");
        QCOMPARE(panel->property("section").toString(), QStringLiteral("media"));
        // Close it so the rest of this case sees the old expectations.
        QVERIFY(timeline->setProperty("infoOpen", false));
        QCoreApplication::processEvents();
        m_controller->showSettings();
        QCoreApplication::processEvents();
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
            "account", "appearance", "notifications", "sound",
            "privacy", "sessions", "labs", "about",
        };
        for (const char *key : sections) {
            auto *row = item(qPrintable(QStringLiteral("settingsNavRow_%1")
                                             .arg(QLatin1String(key))));
            QVERIFY2(row, key);
        }
    }

    // THE QUICK SWITCHER'S SECTION LIST IS A HAND-KEPT COPY, and on
    // 2026-09-12 it was found three behind: `shortcuts` and `updates` had
    // never been in it and `sound` had just arrived, so the one surface whose
    // entire job is "type a name, land on it" could not reach three of the
    // ten sections. Nothing could have caught that — the switcher builds its
    // rows from a local array and every row it does build works.
    //
    // A SOURCE scan, not a loaded-engine one, deliberately: the engine here
    // loads the COMPILED module and the two lists are two literals in two
    // files. Both halves assert a non-zero count first, because a scan whose
    // pattern stops matching passes vacuously and would then agree with
    // anything.
    void theQuickSwitcherOffersEverySettingsSectionTheNavHas()
    {
        const QString qmlDir = QStringLiteral(QML_DIR);

        auto read = [&](const QString &name) {
            QFile file(qmlDir + QLatin1Char('/') + name);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                return QString();
            return QString::fromUtf8(file.readAll());
        };

        const QString settings = read(QStringLiteral("SettingsScreen.qml"));
        const QString switcher = read(QStringLiteral("QuickSwitcher.qml"));
        QVERIFY2(!settings.isEmpty(), "SettingsScreen.qml unreadable");
        QVERIFY2(!switcher.isEmpty(), "QuickSwitcher.qml unreadable");

        // The nav rows. `property string sectionKey: ""` declares it, so the
        // pattern requires a non-empty value and skips the declaration.
        QSet<QString> navKeys;
        QRegularExpression navRe(
            QStringLiteral("sectionKey:\\s*\"([A-Za-z]+)\""));
        auto navIt = navRe.globalMatch(settings);
        while (navIt.hasNext())
            navKeys.insert(navIt.next().captured(1));

        // Only the sectionDefs array, not every `key:` in the file.
        const int start = switcher.indexOf(QStringLiteral("var sectionDefs"));
        QVERIFY2(start >= 0, "QuickSwitcher.qml has no sectionDefs array");
        const int end = switcher.indexOf(QStringLiteral("]"), start);
        QVERIFY2(end > start, "sectionDefs array is not closed");
        const QString defs = switcher.mid(start, end - start);

        QSet<QString> switcherKeys;
        QRegularExpression defRe(
            QStringLiteral("key:\\s*\"([A-Za-z]+)\""));
        auto defIt = defRe.globalMatch(defs);
        while (defIt.hasNext())
            switcherKeys.insert(defIt.next().captured(1));

        // THE FLOOR IS THE NAV-ROW COUNT, not a comfortable margin under it.
        // The two sets only have to be EQUAL below, so a floor of 8 against
        // ten real sections would let two sections vanish from BOTH files and
        // still pass — which is the same vacuity the floor exists to prevent.
        // Raise this with the sections; `SettingsScreen.qml`'s
        // `sectionTitle()` is the list.
        static constexpr int kSections = 10;
        QVERIFY2(navKeys.size() >= kSections,
                 qPrintable(QStringLiteral("only %1 nav rows matched, expected "
                                           "at least %2 — either the scan has "
                                           "stopped working or a section was "
                                           "dropped")
                                .arg(navKeys.size())
                                .arg(kSections)));
        QVERIFY2(switcherKeys.size() >= kSections,
                 qPrintable(QStringLiteral("only %1 switcher rows matched, "
                                           "expected at least %2")
                                .arg(switcherKeys.size())
                                .arg(kSections)));

        const QSet<QString> missing = navKeys - switcherKeys;
        const QSet<QString> extra = switcherKeys - navKeys;
        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral("the quick switcher cannot reach: %1")
                                .arg(QStringList(missing.cbegin(),
                                                 missing.cend())
                                         .join(QStringLiteral(", ")))));
        QVERIFY2(extra.isEmpty(),
                 qPrintable(QStringLiteral("the quick switcher offers sections "
                                           "Settings has no nav row for: %1")
                                .arg(QStringList(extra.cbegin(), extra.cend())
                                         .join(QStringLiteral(", ")))));
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

    // A SEARCH THAT MATCHES NOTHING MUST NOT LEAVE SETTINGS WITH NO
    // SETTINGS IN IT.
    //
    // Reported by measurement, not by reading: type "zzqqxx" in the search
    // field, press Escape, press Ctrl+, — Settings reopens showing "No
    // matching settings" over an EMPTY nav column. Not Account, not
    // Appearance, not even About, which is otherwise always there. Two
    // causes, one per half of this case:
    //
    //   * every SettingsNavRow is gated on its section having a match, and
    //     with zero matches every gate is false at once; and
    //   * the screen is a warm Loader kept alive between opens, so the text
    //     in the field outlives the screen that was closed on top of it.
    //
    // The only way out was the small clear button inside the field.
    //
    // UNFIXED TREE: fails on the first QTRY_VERIFY (the nav is hidden while
    // the query matches nothing) and again after the reopen (the query is
    // still in the field).
    void aQueryThatMatchesNothingNeverLeavesTheNavEmpty()
    {
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        auto *search = item("settingsSearchField");
        auto *accountNav = item("settingsNavRow_account");
        auto *aboutNav = item("settingsNavRow_about");
        auto *resultsPanel = item("settingsSearchResults");
        QVERIFY(search && accountNav && aboutNav && resultsPanel);
        search->setProperty("text", QString());
        QCoreApplication::processEvents();

        search->setProperty("text", QStringLiteral("zzqqxx"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(resultsPanel->isVisible());
        auto *noResults = item("settingsSearchNoResults");
        QVERIFY(noResults);
        QVERIFY(noResults->isVisible());
        QVERIFY2(accountNav->isVisible(),
                 "a query matching nothing hid the whole nav column");
        QVERIFY2(aboutNav->isVisible(),
                 "a query matching nothing hid even About");

        // And the stale query does not survive the screen being closed.
        m_controller->showMain();
        QCoreApplication::processEvents();
        QTRY_COMPARE(search->property("text").toString(), QString());

        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(accountNav->isVisible());
        QVERIFY2(!resultsPanel->isVisible(),
                 "Settings reopened still filtered by the previous query");
    }

    // THE CALL DEVICES LIVE IN "Sound & video" AND ARE NOT LEFT BEHIND.
    //
    // UNFIXED TREE: fails on the first assertion — there was no "sound"
    // section at all, and the microphone, output and camera pickers were a
    // sub-heading at the BOTTOM of Notifications, which is not a place anyone
    // looks for a microphone.
    //
    // Both halves matter. The first proves the pickers ARRIVED; the second
    // proves they LEFT, because a move that quietly became a copy gives the
    // application two places to change one device and no way to tell which
    // one the user is looking at. Driven through real section switches on the
    // real screen rather than read off the source, so a pane that exists but
    // never becomes visible still fails.
    void theCallDevicesLiveInTheSoundSectionAndLeaveNotifications()
    {
        m_controller->showSettingsSection(QStringLiteral("sound"));
        QCoreApplication::processEvents();

        auto *devices = item("callDeviceSettings");
        QVERIFY2(devices, "there is no callDeviceSettings anywhere in Settings");
        QTRY_VERIFY2(devices->isVisible(),
                     "the call device pickers are not shown by the sound "
                     "section");
        // Enumeration is lazy on purpose (Qt Multimedia costs real time on a
        // PipeWire desktop); the section being on screen is what arms it.
        QVERIFY2(devices->property("activated").toBool(),
                 "the device pickers were never activated, so they enumerate "
                 "nothing and render three empty combo boxes");

        // The media playback level had no home in Settings at all before this
        // — the only way to change it was to find a media card and drag its
        // hover popup.
        auto *mediaLevel = item("mediaVolumeSettingSlider");
        QVERIFY2(mediaLevel, "the sound section has no media playback level");
        QVERIFY(mediaLevel->isVisible());

        m_controller->showSettingsSection(QStringLiteral("notifications"));
        QCoreApplication::processEvents();
        QTRY_VERIFY2(!devices->isVisible(),
                     "the call device pickers are still shown by Notifications "
                     "— the move left a copy behind");

        // The ringer deliberately STAYED: it is gated on the desktop
        // notification switch and sits beside the notification sound, so
        // Notifications is its real home rather than a leftover.
        auto *ring = item("ringForCallsCheck");
        QVERIFY(ring);
        QTRY_VERIFY(ring->isVisible());

        // DELIBERATELY LEFT OPEN. Cases in this file run in declaration
        // order and several of the later ones click a nav row without
        // opening Settings first — they inherit it from whatever ran before.
        // Closing it here put starredGifsSettingsRowReflectsStoreAnd… on a
        // hidden screen, whose items map to window coordinates outside the
        // window ("Mouse event at 421, 1716 occurs outside target window").
    }

    // The section is reachable by SEARCH, not only by finding its nav row.
    // A settings page nobody can search is a settings page people ask about
    // in chat instead, and the words below are the ones they type.
    void searchingForAMicrophoneFindsTheSoundSection()
    {
        m_controller->showSettings();
        QCoreApplication::processEvents();
        auto *search = item("settingsSearchField");
        QVERIFY(search);

        for (const char *term : { "mic", "microphone", "input", "output",
                                  "speaker", "volume" }) {
            search->setProperty("text", QString::fromLatin1(term));
            QCoreApplication::processEvents();
            auto *nav = item("settingsNavRow_sound");
            QVERIFY2(nav, "the sound section has no nav row");
            QTRY_VERIFY2(nav->isVisible(),
                         qPrintable(QStringLiteral(
                                        "searching for \"%1\" does not narrow "
                                        "to the sound section")
                                        .arg(QLatin1String(term))));
        }

        // Cleared, and Settings left OPEN — see the note above.
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
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

    // A REFUSED "Stop ignoring" USED TO SAY NOTHING AT ALL.
    //
    // ModerationController reports every ignore/unignore outcome on
    // ignoreActionFinished, and until this round its ONLY consumer in the
    // whole tree was MemberProfilePopover — which filters on its own userId
    // and is not open when this button is pressed. So a server refusal, a
    // rate limit or a dead connection left the row exactly where it was with
    // no explanation, which reads as a dead button. The list is bound to
    // `ignoredUsers` and only changes on success, so it was never dishonest;
    // it was silent, and silence about a write the user asked for is its own
    // defect (§6: never report a cleanup as successful when it removed
    // nothing — and never report nothing at all).
    //
    // Driven through the real signal rather than a source scan, so this fails
    // if the Connections block is removed, mis-named, or wired to a property
    // the label does not read.
    void aRefusedStopIgnoringIsShownInTheIgnoredUsersCard()
    {
        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();

        auto *card = item("ignoredUsersCard");
        QVERIFY2(card, "the ignored-users card is not in the tree");
        auto *notice = item("ignoredUsersWriteError");
        QVERIFY2(notice,
                 "the ignored-users card has no place to report a refused "
                 "write, so a failed 'Stop ignoring' is silent");
        QVERIFY(!notice->isVisible());

        // The card records who it asked about when the button is pressed;
        // stand in for that press, then deliver the refusal the controller
        // would have emitted.
        const QString target = QStringLiteral("@spam:example.org");
        card->setProperty("unignoreUserId", target);
        auto *moderation = m_controller->moderation();
        QVERIFY(moderation);
        const QString refusal =
            QStringLiteral("The server refused this action.");
        QVERIFY(QMetaObject::invokeMethod(
            moderation, "ignoreActionFinished", Qt::DirectConnection,
            Q_ARG(QString, target), Q_ARG(bool, false), Q_ARG(bool, false),
            Q_ARG(QString, refusal)));
        QCoreApplication::processEvents();

        QVERIFY2(card->property("unignoreError").toString() == refusal,
                 "the ignored-users card did not take the refusal from "
                 "ignoreActionFinished, so a refused 'Stop ignoring' is "
                 "still silent");
        QVERIFY2(QTest::qWaitFor([notice] { return notice->isVisible(); },
                                 3000),
                 "the ignored-users card holds the refusal but never shows "
                 "it");
        QCOMPARE(notice->property("text").toString(), refusal);

        // A different user's outcome — the profile popover's own ignore
        // button, say — must not paint an error into this card.
        card->setProperty("unignoreError", QString());
        card->setProperty("unignoreUserId", target);
        QVERIFY(QMetaObject::invokeMethod(
            moderation, "ignoreActionFinished", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("@someone-else:example.org")),
            Q_ARG(bool, true), Q_ARG(bool, false), Q_ARG(QString, refusal)));
        QCoreApplication::processEvents();
        QCOMPARE(card->property("unignoreError").toString(), QString());

        // And a success clears the notice rather than leaving a stale one.
        card->setProperty("unignoreError", refusal);
        QVERIFY(QMetaObject::invokeMethod(
            moderation, "ignoreActionFinished", Qt::DirectConnection,
            Q_ARG(QString, target), Q_ARG(bool, false), Q_ARG(bool, true),
            Q_ARG(QString, QStringLiteral("no longer ignored"))));
        QCoreApplication::processEvents();
        QCOMPARE(card->property("unignoreError").toString(), QString());

        // Restore the shell for the cases that follow.
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        m_controller->showMain();
        QTRY_VERIFY_WITH_TIMEOUT(
            item("spacesRail") && item("spacesRail")->isVisible(), 3000);
    }

    // ── THE PAGE NAMED THE WRONG DEFAULT, AND THE WRONG ONE WAS THE
    // PRIVATE ONE ───────────────────────────────────────────────────────
    //
    // The help text under the notification-preview combo read "Sender only
    // (the default) never shows message text in notifications", while
    // SettingsManager::notificationPreview() has returned 0 = Sender and
    // message since 8e4977d1 (2026-08-22). So the app told a reader that
    // their desktop was NOT showing message bodies at a moment when it was:
    // a promise about disclosure that it did not keep.
    //
    // The default is read from a SCRATCH SettingsManager with its own empty
    // store, not from the live one this suite has been writing to, and the
    // mode NAMES are read off the live combo's own model — so the case
    // asserts the page against the code rather than against a string a
    // future round can move out from under it. The negative half matters as
    // much as the positive one: the old sentence named a mode that was not
    // the default, and only "does not claim the wrong one" catches that.
    //
    // UNFIXED TREE: fails with `"Sender only" is described as the default,
    // but the default is 0 = "Sender and message"`.
    void theNotificationHelpNamesTheModeThatIsActuallyTheDefault()
    {
        m_controller->showSettingsSection(QStringLiteral("notifications"));
        QCoreApplication::processEvents();

        auto *combo = item("notificationPreviewCombo");
        auto *help = item("notificationPreviewHelp");
        QVERIFY2(combo && help, "the notification preview row is not live");
        const QVariantList modes = combo->property("model").toList();
        QCOMPARE(modes.size(), 3);

        // A store nothing has ever written, so the getter answers with its
        // own documented default rather than with this suite's history.
        const QString appName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(
            QStringLiteral("settings-shell-qml-default-probe"));
        int defaultMode = -1;
        {
            SettingsManager probe;
            defaultMode = probe.notificationPreview();
        }
        QCoreApplication::setApplicationName(appName);
        QVERIFY(defaultMode >= 0 && defaultMode < modes.size());

        const QString text = help->property("text").toString();
        QVERIFY2(!text.isEmpty(), "the preview help text is empty");
        int claimed = 0;
        for (int i = 0; i < modes.size(); ++i) {
            const QString name = modes.at(i).toString();
            const bool claims =
                text.contains(name + QStringLiteral(" is the default"))
                || text.contains(name + QStringLiteral(" (the default)"));
            if (claims)
                ++claimed;
            if (i == defaultMode)
                continue;
            QVERIFY2(!claims,
                     qPrintable(QStringLiteral(
                         "\"%1\" is described as the default, but the "
                         "default is %2 = \"%3\" — the page is telling the "
                         "reader it discloses less than it does")
                             .arg(name).arg(defaultMode)
                             .arg(modes.at(defaultMode).toString())));
        }
        QVERIFY2(claimed == 1,
                 qPrintable(QStringLiteral(
                     "%1 of the three preview modes are named as the "
                     "default; exactly one must be, and it must be %2")
                         .arg(claimed)
                         .arg(modes.at(defaultMode).toString())));
    }

    // ── EVERY SEARCH ENTRY MUST POINT AT SOMETHING THAT EXISTS ──────────
    //
    // The anchors are what makes a result click go anywhere, and a typo in
    // one is invisible: the row still highlights, still reads as a button,
    // and silently does nothing — which is the defect they were added to
    // fix. So the whole index is resolved against the LIVE pane, all
    // seventy, and the COUNT is asserted rather than "no failures seen"
    // (a loop over an index that failed to load passes vacuously).
    //
    // UNFIXED TREE: there are no anchors at all, so this fails on the first
    // entry.
    void everySearchIndexEntryResolvesItsAnchorToALiveControl()
    {
        auto *screen = item("settingsScreenRoot");
        auto *flick = item("settingsContentFlick");
        QVERIFY2(screen && flick, "the settings screen root is not live");
        auto *pane = flick->property("contentItem").value<QQuickItem *>();
        QVERIFY(pane);

        const QVariantList index = screen->property("searchIndex").toList();
        QVERIFY2(index.size() >= 60,
                 qPrintable(QStringLiteral("only %1 index entries were read")
                                .arg(index.size())));
        QStringList bad;
        int resolved = 0;
        for (const QVariant &v : index) {
            const QVariantMap e = v.toMap();
            const QString title = e.value(QStringLiteral("title")).toString();
            const QString anchor = e.value(QStringLiteral("anchor")).toString();
            if (anchor.isEmpty()) {
                bad.append(title + QStringLiteral(" -> (none)"));
                continue;
            }
            QQuickItem *hit = findItem(m_window->contentItem(), anchor);
            if (!hit) {
                bad.append(title + QStringLiteral(" -> \"") + anchor
                           + QStringLiteral("\" (no such item)"));
                continue;
            }
            // And it must live in the CONTENT PANE. An anchor that resolved
            // to something in the nav column or a dialog would scroll the
            // page to a position that means nothing.
            bool inPane = false;
            for (QQuickItem *p = hit; p; p = p->parentItem()) {
                if (p == pane) {
                    inPane = true;
                    break;
                }
            }
            if (!inPane) {
                bad.append(title + QStringLiteral(" -> \"") + anchor
                           + QStringLiteral("\" (outside the content pane)"));
                continue;
            }
            ++resolved;
        }
        QVERIFY2(bad.isEmpty(),
                 qPrintable(QStringLiteral(
                     "search entries whose anchor names no live control in "
                     "the settings content pane: %1")
                         .arg(bad.join(QStringLiteral("; ")))));
        // Assert the COUNT of what actually resolved, never the count of
        // loop iterations.
        QCOMPARE(resolved, index.size());
    }

    // ── CLICKING A RESULT FOR THE SECTION YOU ARE ON DID NOTHING ────────
    //
    // The tap did `root.section = entry.section`, and when that IS the
    // current section Qt emits no change, so nothing happened at all.
    // Measured on a real window: searched "rail depth" from Appearance,
    // clicked the result, and the 1440x1280 content region came back
    // BYTE-IDENTICAL — 0 differing pixels. It is the common case, not the
    // corner one: Appearance is the landing section and supplies 26 of the
    // 70 entries.
    //
    // Asserted on the SCROLL and on the halo, not on "the section is still
    // appearance", which was already true on the broken tree. The click is
    // a real mouse press on the result's TITLE — not on the row centre,
    // where the rail-depth entry's own inline segmented control lives.
    //
    // UNFIXED TREE: fails on `contentY moved`, at 0.
    void aSearchResultInTheSectionYouAreAlreadyOnStillTakesYouToTheControl()
    {
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        auto *search = item("settingsSearchField");
        auto *flick = item("settingsContentFlick");
        QVERIFY(search && flick);
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
        QTRY_COMPARE(flick->property("contentY").toReal(), 0.0);

        search->setProperty("text", QStringLiteral("rail depth"));
        QCoreApplication::processEvents();
        auto *title = item("settingsSearchResultTitle_0");
        QVERIFY2(title, "no first search result for \"rail depth\"");

        auto *control = item("spacesRailDepthControl");
        QVERIFY(control);
        const qreal target =
            control->mapToItem(
                flick->property("contentItem").value<QQuickItem *>(),
                QPointF(0, 0)).y();
        QVERIFY2(target > flick->height(),
                 qPrintable(QStringLiteral(
                     "the fixture is not exercising the defect: the rail "
                     "depth control is already on screen at y=%1 in a %2 "
                     "tall viewport")
                         .arg(target).arg(flick->height())));

        auto *halo = item("settingsSearchRevealHalo");
        QVERIFY(halo);
        clickItem(title);
        for (int i = 0; i < 100; ++i) {
            if (flick->property("contentY").toReal() > 0.0
                && halo->opacity() > 0.5)
                break;
            QTest::qWait(10);
        }
        // Read everything, THEN put the query back, THEN assert: a case
        // that fails here must not hand the next one a filtered nav.
        const qreal contentY = flick->property("contentY").toReal();
        const qreal haloOpacity = halo->opacity();
        const qreal haloY = halo->y();
        search->setProperty("text", QString());
        QCoreApplication::processEvents();

        QVERIFY2(contentY > 0.0,
                 "clicking a result for the section you are already on left "
                 "the page exactly where it was");
        QVERIFY2(target >= contentY && target <= contentY + flick->height(),
                 qPrintable(QStringLiteral(
                     "the control is at %1 and the viewport shows %2..%3")
                         .arg(target).arg(contentY)
                         .arg(contentY + flick->height())));
        // And the click is acknowledged even when it had nowhere to scroll:
        // the halo rings the control it named.
        QVERIFY2(haloOpacity > 0.5,
                 "the reveal halo never lit, so a click on a control already "
                 "on screen still has no feedback");
        QVERIFY2(qAbs(haloY - target) < 12.0,
                 qPrintable(QStringLiteral(
                     "the halo is at y=%1 and the control it names at y=%2")
                         .arg(haloY).arg(target)));
    }

    // ── AND A RESULT IN ANOTHER SECTION LANDED AT THE TOP OF IT ─────────
    //
    // `onSectionChanged` sets contentY = 0, so a breadcrumb naming a
    // sub-group dropped the reader at the top of a very long page and left
    // them to find the control themselves.
    //
    // UNFIXED TREE: fails with contentY 0 and the control far below the
    // viewport.
    void aSearchResultInAnotherSectionLandsOnTheControlNotTheTopOfThePage()
    {
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        auto *search = item("settingsSearchField");
        auto *flick = item("settingsContentFlick");
        QVERIFY(search && flick);
        search->setProperty("text", QStringLiteral("ignored users"));
        QCoreApplication::processEvents();
        auto *title = item("settingsSearchResultTitle_0");
        QVERIFY2(title, "no first search result for \"ignored users\"");

        auto *screen = item("settingsScreenRoot");
        auto *card = item("ignoredUsersCard");
        auto *content = flick->property("contentItem").value<QQuickItem *>();
        QVERIFY(screen && card && content);

        clickItem(title);
        for (int i = 0; i < 100; ++i) {
            if (screen->property("section").toString()
                    == QLatin1String("privacy")
                && flick->property("contentY").toReal() > 0.0)
                break;
            QTest::qWait(10);
        }
        const QString section = screen->property("section").toString();
        const qreal contentY = flick->property("contentY").toReal();
        const qreal top = card->mapToItem(content, QPointF(0, 0)).y();
        const qreal viewH = flick->height();
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        QCOMPARE(section, QStringLiteral("privacy"));
        QVERIFY2(contentY > 0.0,
                 "a cross-section result still landed at contentY 0");
        QVERIFY2(top >= contentY && top <= contentY + viewH,
                 qPrintable(QStringLiteral(
                     "the ignored-users card is at %1 and the viewport shows "
                     "%2..%3").arg(top).arg(contentY).arg(contentY + viewH)));
    }

    // ── AND THE HOVER ON THOSE ROWS IS fea70c63's DEFECT, ONE SCREEN
    // AWAY ──────────────────────────────────────────────────────────────
    //
    // A search result sits in the same nav column as the section rows, over
    // the same ground, and painted its hover in the same stormSelection —
    // the palette's `hover`, a tint designed to sit on `surface` and not on
    // a page. Measured against that column before the fix: Lightning Light
    // 0.40 dL*, Moss Light 0.45, Warm 0.99. There is no bolt caret and no
    // bold label on a result row, so on the three light themes the row had
    // NO hover state at all.
    //
    // The fill is read off the live Rectangle with a real pointer over it,
    // never off a token name — the tokens were fine, the question is which
    // one the row asks for. 4.0 dL* is the floor the nav pill uses, and the
    // worst real palette after the fix is Moss Light at 5.49; `selected`
    // would be 3.4 there and `hover` 0.45, so the floor separates the fix
    // from both tokens that do not work.
    //
    // UNFIXED TREE: fails on Lightning Light at 0.40 dL*.
    void theSearchResultHoverIsVisibleOnEveryTheme()
    {
        const int restore = m_controller->settings()->theme();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        auto *search = item("settingsSearchField");
        QVERIFY(search);
        search->setProperty("text", QStringLiteral("room activity"));
        QCoreApplication::processEvents();

        auto *row = item("settingsSearchResult_0");
        auto *column = item("settingsNavColumn");
        QVERIFY2(row && column, "the first search result is not live");

        // Away first, then on: an unconditional move event, whatever the
        // previous case left the pointer sitting on.
        QTest::mouseMove(m_window, QPoint(m_window->width() - 4, 4));
        QCoreApplication::processEvents();
        const QPointF centre = row->mapToScene(
            QPointF(row->width() / 2, row->height() / 2));
        QTest::mouseMove(m_window, centre.toPoint());
        QCoreApplication::processEvents();
        // Guard the premise the way the nav case does: an unhovered row
        // paints "transparent", which samples as pure black and would PASS
        // on every light theme while testing nothing.
        QTRY_VERIFY2(
            row->property("color").value<QColor>().alpha() == 255,
            "the pointer never reached the result row, so its resting "
            "transparent fill is what would have been measured");

        const int themes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        double worst = 1000.0;
        QString worstWhere;
        QStringList flat;
        int checked = 0;
        for (int id : themes) {
            m_controller->settings()->setTheme(
                static_cast<SettingsManager::Theme>(id));
            QCoreApplication::processEvents();
            const QColor fill = row->property("color").value<QColor>();
            const QColor nav = column->property("color").value<QColor>();
            QVERIFY(fill.isValid() && nav.isValid());
            const double sep = qAbs(lstarOf(fill) - lstarOf(nav));
            if (sep < worst) {
                worst = sep;
                worstWhere = QStringLiteral("theme %1: %2 on %3")
                                 .arg(id).arg(fill.name(), nav.name());
            }
            ++checked;
            if (sep < 4.0)
                flat.append(QStringLiteral(
                    "theme %1: a hovered settings-search result is %2 on a "
                    "column of %3 — only %4 dL* (%5:1) apart, so the row "
                    "gives no feedback that it is the one under the pointer")
                        .arg(id)
                        .arg(fill.name(), nav.name())
                        .arg(sep, 0, 'f', 2)
                        .arg(contrastRatio(fill, nav), 0, 'f', 2));
        }
        // Put the shell back BEFORE asserting: a case that fails mid-loop
        // must not hand the next one a stray theme, a live query and a
        // pointer parked on a row.
        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(restore));
        search->setProperty("text", QString());
        QTest::mouseMove(m_window, QPoint(m_window->width() - 4, 4));
        QCoreApplication::processEvents();

        QVERIFY2(flat.isEmpty(),
                 qPrintable(flat.join(QStringLiteral("\n  "))));
        QCOMPARE(checked, 11);
        qInfo("search result hover vs nav: worst %.2f dL* (%s)",
              worst, qPrintable(worstWhere));
    }

    // ── THE ACCOUNT PAGE DID NOT FIT THE WINDOW THE APP ITSELF ALLOWS ───
    //
    // Main.qml declares minimumWidth 640. At 640x420 the Account page was
    // clipped with no horizontal scrollbar — `contentFlick` sets no
    // contentWidth and clips — so the "+" custom swatch, "Use theme
    // colour" (the ONLY way to clear a custom name colour) and the
    // display-name "Edit" button were off-screen and unreachable, and the
    // name-colour help paragraph was cut mid-word at the window edge.
    //
    // One cause, three symptoms: a RowLayout of nine swatches, the custom
    // slot, a spacer and a button has an unshrinkable ~470 px minimum, and
    // that minimum propagates up through the ColumnLayout the wrapping help
    // Label sizes itself to. The whole column, not just the row, was wider
    // than the card.
    //
    // Geometric, on real delegates, in x — a source scan cannot see this
    // and neither can a screenshot at the default size. Everything is
    // measured against the CARD, not the window, because a control inside
    // the window but hanging out of its own card is still wrong.
    //
    // UNFIXED TREE: fails on the swatch row, ~470 px wide inside a ~330 px
    // card.
    void theAccountPageFitsTheApplicationsOwnMinimumWindow()
    {
        const int w = m_window->width();
        const int h = m_window->height();
        m_controller->showSettingsSection(QStringLiteral("account"));
        QCoreApplication::processEvents();
        m_window->setWidth(640);
        m_window->setHeight(420);
        QCoreApplication::processEvents();
        QTRY_COMPARE(m_window->width(), 640);

        auto *card = item("accountIdentityCard");
        QVERIFY2(card, "the account identity card is not live");
        QTRY_VERIFY(card->width() > 0 && card->width() < 640);

        struct Probe { const char *name; const char *what; };
        const Probe probes[] = {
            { "nameColorSwatchFlow", "the nine name-colour swatches" },
            { "nameColorCustomSwatch", "the custom-colour \"+\" slot" },
            { "clearNameColorButton", "\"Use theme colour\"" },
            { "editDisplayNameButton", "the display-name Edit button" },
            { "nameColorHelpText", "the name-colour help paragraph" },
        };
        const qreal cardRight =
            card->mapToScene(QPointF(card->width(), 0)).x();
        QStringList clipped;
        int checked = 0;
        for (const Probe &p : probes) {
            auto *probe = item(p.name);
            QVERIFY2(probe, p.name);
            if (!probe->isVisible())
                continue;   // hidden by a backend capability, not clipped
            const qreal right =
                probe->mapToScene(QPointF(probe->width(), 0)).x();
            const qreal left = probe->mapToScene(QPointF(0, 0)).x();
            ++checked;
            if (right > cardRight + 1.0)
                clipped.append(QStringLiteral(
                    "at 640x420 %1 runs to x=%2, past its own card's right "
                    "edge at %3 — and the page has no horizontal scrollbar, "
                    "so it cannot be reached")
                        .arg(QLatin1String(p.what)).arg(right).arg(cardRight));
            if (right > m_window->width() + 1.0 || left < -1.0)
                clipped.append(QStringLiteral(
                    "at 640x420 %1 spans x=%2..%3 in a 640 px window")
                        .arg(QLatin1String(p.what)).arg(left).arg(right));
        }

        // Restore before asserting — a failure here must not leave the next
        // case running in a 640x420 window.
        m_window->setWidth(w);
        m_window->setHeight(h);
        QCoreApplication::processEvents();
        QTRY_COMPARE(m_window->width(), w);
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        QVERIFY2(clipped.isEmpty(),
                 qPrintable(clipped.join(QStringLiteral("\n  "))));
        QVERIFY2(checked >= 4,
                 qPrintable(QStringLiteral(
                     "only %1 of the five Account controls were on screen to "
                     "measure").arg(checked)));
    }

    // ── TWO SHORTCUTS THAT RENDER AS THE SAME STRING ────────────────────
    //
    // The action-name column was the only cell with fillWidth, so it took
    // the entire shortfall while the 132 px keycap and the Change button
    // kept theirs, and it ELIDED. At 640x520 "Open the quick switcher" and
    // "Open the quick switcher in command mode" both read "Open the …", so
    // there was no way to tell which shortcut the Change button beside them
    // was about to rebind.
    //
    // Asserted as "nothing is shortened", not as "these two differ": four
    // rows begin "Show or hide", three "Mark the", four "Open the", and a
    // case that pins one pair would pass while the other nine collide. The
    // second half proves the first is not vacuous — if `truncated` ever
    // stops reporting, the distinctness check still bites.
    //
    // UNFIXED TREE: fails with `"Open the quick switcher" is shortened`.
    void noShortcutNameIsShortenedIntoAnotherShortcutsName()
    {
        const int w = m_window->width();
        const int h = m_window->height();
        m_controller->showSettingsSection(QStringLiteral("shortcuts"));
        QCoreApplication::processEvents();
        m_window->setWidth(640);
        m_window->setHeight(520);
        QCoreApplication::processEvents();
        QTRY_COMPARE(m_window->width(), 640);

        auto *registry = m_controller->shortcuts();
        QVERIFY(registry);
        const int rows = registry->rowCount();
        QVERIFY(rows > 10);

        QSet<QString> seen;
        QStringList cut;
        int checked = 0;
        for (int r = 0; r < rows; ++r) {
            const QString id =
                registry->data(registry->index(r, 0),
                               ShortcutRegistry::IdRole).toString();
            QVERIFY(!id.isEmpty());
            auto *label = item(qPrintable(QStringLiteral("shortcutName_%1")
                                              .arg(id)));
            if (!label)
                continue;
            ++checked;
            const QString text = label->property("text").toString();
            if (label->property("truncated").toBool())
                cut.append(QStringLiteral(
                    "at 640x520 the name of \"%1\" is shortened to fit a "
                    "%2 px column, and a shortened action name is not an "
                    "action name — it is what made \"Open the quick "
                    "switcher\" and \"…in command mode\" the same row")
                        .arg(text).arg(label->width()));
            if (seen.contains(text))
                cut.append(QStringLiteral(
                    "two shortcut rows both render as \"%1\"").arg(text));
            seen.insert(text);
        }

        m_window->setWidth(w);
        m_window->setHeight(h);
        QCoreApplication::processEvents();
        QTRY_COMPARE(m_window->width(), w);
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        QVERIFY2(cut.isEmpty(), qPrintable(cut.join(QStringLiteral("\n  "))));
        QVERIFY2(checked >= rows,
                 qPrintable(QStringLiteral(
                     "only %1 of %2 registry rows have a live name label")
                         .arg(checked).arg(rows)));
    }

    // ── PLACE THE INK, NOT THE BOX ──────────────────────────────────────
    //
    // The rail-depth segmented control sat about 8 px right of the label it
    // belongs to: every Label in that card starts its ink at spacing4 from
    // the card's content edge, while a SegmentedControl segment is
    // `text + 24`, so its first glyph is 12 px inside its own left edge.
    // Measured in Lightning Dark: label x=300, help paragraph x=301, the
    // checkbox above x=303, the control x=309.
    //
    // The comparison is between real INK positions on live delegates, so it
    // keeps holding if SegmentedControl's padding ever changes — which is
    // the whole reason not to assert the 12 in the QML.
    //
    // UNFIXED TREE: fails at ~8 px out.
    void theRailDepthControlLinesUpWithItsOwnLabel()
    {
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        auto *label = item("spacesRailDepthLabel");
        auto *segment = item("spacesRailDepthControl_0");
        QVERIFY2(label && segment, "the rail-depth row is not live");
        ensureVisible(segment);
        auto *ink = segment->property("contentItem").value<QQuickItem *>();
        QVERIFY2(ink, "the segment has no content item to measure");
        QTRY_VERIFY(ink->width() > 0 && label->width() > 0);

        const qreal labelInk = label->mapToScene(QPointF(0, 0)).x();
        // The segment centres its label, so its first glyph is half the
        // slack in from the content item's own left edge.
        const qreal segInk =
            ink->mapToScene(QPointF(0, 0)).x()
            + (ink->width() - ink->implicitWidth()) / 2.0;
        QVERIFY2(qAbs(segInk - labelInk) <= 1.0,
                 qPrintable(QStringLiteral(
                     "\"Spaces rail depth\" starts its ink at x=%1 and its "
                     "own control starts at x=%2 — %3 px out of line with "
                     "the label it belongs to")
                         .arg(labelInk).arg(segInk)
                         .arg(qAbs(segInk - labelInk), 0, 'f', 1)));
    }

    // ── ONE PARAGRAPH SET SOLID AMONG PARAGRAPHS THAT ARE NOT ───────────
    //
    // The "Message search index" description omitted lineHeight, so twelve
    // lines of body copy rendered at Qt's default 17 px leading against
    // 25-26 px for every other paragraph on the same page. Measured
    // baseline-to-baseline on a real window.
    //
    // Compared against a NEIGHBOUR rather than against a constant: what was
    // wrong is that it disagreed with the page around it.
    //
    // UNFIXED TREE: fails with the index paragraph at lineHeight 1.0.
    void theSearchIndexHelpParagraphLeadsLikeItsNeighbours()
    {
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();
        auto *odd = item("searchIndexHelpText");
        auto *neighbour = item("collapseEmbedsHint");
        QVERIFY2(odd, "the message-search-index help text is not live");
        if (!neighbour)
            neighbour = item("nameColorHelpText");
        QVERIFY2(neighbour, "no neighbouring help paragraph to compare with");
        const int mode = odd->property("lineHeightMode").toInt();
        const int wantMode = neighbour->property("lineHeightMode").toInt();
        const qreal lead = odd->property("lineHeight").toReal();
        const qreal wantLead = neighbour->property("lineHeight").toReal();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        QCOMPARE(mode, wantMode);
        QVERIFY2(qFuzzyCompare(lead, wantLead),
                 qPrintable(QStringLiteral(
                     "the message-search-index paragraph leads at %1 where "
                     "its neighbours lead at %2").arg(lead).arg(wantLead)));
    }

    // ── EVERY PARAGRAPH ON THE PAGE LEADS THE SAME, AND THE COUNT SAYS SO ─
    //
    // The case above fixed ONE paragraph. An audit then found twenty-four
    // more wrapping Labels in this file with no `lineHeight` at all, set
    // solid at Qt's default ~17 px among paragraphs leading at 25-26. Fixing
    // them one at a time is how the next one gets missed, so this is a
    // SWEEP — and it asserts the COUNT it swept, because §16's standing
    // lesson is that a check which can come back silently short is itself
    // the defect, not its symptom. A pattern that stops matching would
    // otherwise pass over an empty set.
    //
    // THE INCLUSION RULE, which the sweep implements literally:
    //
    //   A Label in SettingsScreen.qml is BODY COPY when it sets `wrapMode`
    //   — i.e. it can produce more than one line — UNLESS it opts out by
    //   being (a) clamped to one line with `maximumLineCount: 1`, (b) a
    //   monospace value readout (`font.family: AppTheme.monoFont`), or
    //   (c) marked `// not-body-copy:` with a reason.
    //
    // `wrapMode` is not an arbitrary choice of criterion: AppTheme's own
    // token comment beside `lineHeightBody` already defines the contract as
    // "every WRAPPING text item; single-line chrome keeps the default". The
    // three carve-outs are the categories that are NOT body copy — a chip
    // or badge is a single line, a monospace value must not be re-led, and
    // anything else has to say so out loud instead of drifting off quietly.
    //
    // Source text, not live items, and deliberately: two thirds of these
    // paragraphs are behind a `visible:` binding, another section, or a
    // backend capability the mock does not report, so a runtime sweep could
    // only ever reach a subset — and a subset cannot support a count. The
    // live half of the evidence is the case below, which measures a
    // newly-swept Label's real rendered leading on a real delegate.
    //
    // UNFIXED TREE: fails listing 24 paragraphs set solid.
    void everyWrappingParagraphInSettingsSetsBodyLeading()
    {
        QFile file(QStringLiteral(QML_DIR) + QStringLiteral("/SettingsScreen.qml"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 "SettingsScreen.qml is not readable");
        const QStringList lines =
            QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));

        // Braces, with string literals and line comments removed first: a
        // `//` inside a sentence and a `{` inside a translated string both
        // shift the depth otherwise.
        static const QRegularExpression dq(QStringLiteral("\"(\\\\.|[^\"\\\\])*\""));
        static const QRegularExpression sq(QStringLiteral("'(\\\\.|[^'\\\\])*'"));
        static const QRegularExpression lc(QStringLiteral("//.*$"));
        QStringList bare;
        bare.reserve(lines.size());
        for (const QString &raw : lines) {
            QString s = raw;
            s.remove(dq);
            s.remove(sq);
            s.remove(lc);
            bare.append(s);
        }

        static const QRegularExpression labelOpen(
            QStringLiteral("^\\s*Label\\s*\\{\\s*$"));
        static const QRegularExpression prop(
            QStringLiteral("^\\s*([A-Za-z_][A-Za-z0-9_.]*)\\s*:\\s*(.*)$"));

        int wrapping = 0;
        int excluded = 0;
        int bodyCopy = 0;
        QStringList solid;

        for (int i = 0; i < lines.size(); ++i) {
            if (!labelOpen.match(lines.at(i)).hasMatch())
                continue;
            // The block's own properties are the ones at depth 1; anything
            // deeper belongs to a nested MouseArea, ToolTip or Rectangle.
            QHash<QString, QString> own;
            bool optOut = false;
            int depth = 0;
            int end = i;
            for (int j = i; j < lines.size(); ++j) {
                if (depth == 1) {
                    const auto m = prop.match(lines.at(j));
                    if (m.hasMatch())
                        own.insert(m.captured(1), m.captured(2).trimmed());
                    if (lines.at(j).contains(QLatin1String("// not-body-copy:")))
                        optOut = true;
                }
                const QString &b = bare.at(j);
                depth += b.count(QLatin1Char('{')) - b.count(QLatin1Char('}'));
                if (depth == 0 && j > i) {
                    end = j;
                    break;
                }
            }
            QVERIFY2(end > i, qPrintable(QStringLiteral(
                "unterminated Label block at SettingsScreen.qml:%1")
                    .arg(i + 1)));

            const QString wrap = own.value(QStringLiteral("wrapMode"));
            if (wrap.isEmpty() || wrap == QLatin1String("Text.NoWrap"))
                continue;
            ++wrapping;

            if (optOut
                || own.value(QStringLiteral("maximumLineCount")) == QLatin1String("1")
                || own.value(QStringLiteral("font.family"))
                       == QLatin1String("AppTheme.monoFont")) {
                ++excluded;
                continue;
            }
            ++bodyCopy;

            if (own.value(QStringLiteral("lineHeight"))
                    != QLatin1String("AppTheme.lineHeightBody")
                || own.value(QStringLiteral("lineHeightMode"))
                       != QLatin1String("Text.ProportionalHeight")) {
                solid.append(QStringLiteral(
                    "SettingsScreen.qml:%1 wraps but does not set body "
                    "leading (lineHeight=%2 lineHeightMode=%3)")
                        .arg(i + 1)
                        .arg(own.value(QStringLiteral("lineHeight"),
                                       QStringLiteral("<absent>")))
                        .arg(own.value(QStringLiteral("lineHeightMode"),
                                       QStringLiteral("<absent>"))));
            }
        }

        // THE COUNT, BEFORE THE VERDICT. A sweep whose pattern rots reports
        // "nothing wrong" with a straight face; these three numbers are what
        // make that impossible. The floors are floors, not the current
        // values, so ordinary editing does not make this case red — but
        // losing two thirds of the file to a parser change does.
        QVERIFY2(wrapping >= 110,
                 qPrintable(QStringLiteral(
                     "the sweep found only %1 wrapping Labels in a file that "
                     "has had 112 since 2026-09-20 — the parser has stopped "
                     "matching and every verdict below it is vacuous")
                         .arg(wrapping)));
        QCOMPARE(bodyCopy + excluded, wrapping);
        QVERIFY2(bodyCopy >= 109,
                 qPrintable(QStringLiteral(
                     "only %1 of %2 wrapping Labels were graded as body copy; "
                     "%3 opted out, which is more carve-outs than this file "
                     "has ever needed").arg(bodyCopy).arg(wrapping)
                         .arg(excluded)));
        QVERIFY2(solid.isEmpty(),
                 qPrintable(QStringLiteral("%1 of %2 body paragraphs are set "
                                           "solid:\n  %3")
                                .arg(solid.size()).arg(bodyCopy)
                                .arg(solid.join(QStringLiteral("\n  ")))));
    }

    // ── AND ONE OF THEM, MEASURED WHERE IT IS ACTUALLY DRAWN ────────────
    //
    // The sweep above is source text. This is the other half: a Label the
    // sweep just fixed, on a live delegate, measured in PIXELS. The number
    // compared is `contentHeight / lineCount / font.pixelSize` — the
    // rendered line box as a multiple of the type size — because the two
    // Labels in this card are at DIFFERENT sizes (textBody 14 and textMeta
    // 12), so their absolute line boxes must differ while their leading
    // must not. It fails on the unfixed tree for the reason the page looked
    // wrong, not because a property is spelled differently.
    //
    // The numbers are ratios of the TYPE SIZE, not multipliers of
    // `lineHeightBody`: a 12 px line's natural box is ~17 px, so solid text
    // measures ~1.42x and body leading measures 1.5 x 1.42 = ~2.13x. The
    // non-vacuity floor is 1.8 because it has to fall BETWEEN those two —
    // 1.35 would have been cleared by a reference that was itself solid.
    //
    // UNFIXED TREE: fails at 1.4286x against 2.125x (measured 2026-09-20).
    void theIndexedMessageCountLeadsLikeTheParagraphInItsOwnCard()
    {
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();
        auto *swept = item("searchIndexStatsLabel");
        auto *reference = item("searchIndexHelpText");
        QVERIFY2(swept, "the indexed-message count is not live");
        QVERIFY2(reference, "the search-index help paragraph is not live");
        QTRY_VERIFY(swept->width() > 0 && reference->width() > 0);
        QTRY_VERIFY(swept->property("lineCount").toInt() > 0
                    && reference->property("lineCount").toInt() > 0);

        auto leading = [](QQuickItem *label) {
            const int lines = label->property("lineCount").toInt();
            const int size = label->property("font").value<QFont>().pixelSize();
            if (lines <= 0 || size <= 0)
                return 0.0;
            return label->property("contentHeight").toReal() / lines / size;
        };
        const qreal sweptLead = leading(swept);
        const qreal wantLead = leading(reference);

        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        // Not vacuous: the reference must itself be at body leading, or two
        // solid paragraphs would agree with each other and pass.
        QVERIFY2(wantLead >= 1.8,
                 qPrintable(QStringLiteral(
                     "the reference paragraph draws at %1x its type size, so "
                     "it is not at body leading and cannot be a reference")
                         .arg(wantLead)));
        QVERIFY2(qAbs(sweptLead - wantLead) <= 0.06,
                 qPrintable(QStringLiteral(
                     "the indexed-message count draws at %1x its type size "
                     "where the paragraph above it draws %2x — the same "
                     "defect the case above swept out of this file")
                         .arg(sweptLead).arg(wantLead)));
    }

    // ── A GRADE WITH NO PAIR ON IT IS NOT A GRADE ───────────────────────
    //
    // The theme editor's readability column is pinned at 304 px on any
    // window 1380 or wider, and every row put a whole sentence and a
    // numeric column on ONE line. So the numbers were intact and the thing
    // they were about was not: "The Spaces rail against the room list" read
    // "The Spaces rail against the …", "Main text on the conversation …",
    // and a reader could not tell which pair was being graded — which is
    // the panel's entire job.
    //
    // BOTH FAMILIES, on real delegates, at the width the report came from:
    // the picker's live readout (a role is open) and the findings list
    // (nothing is open, and three inks are forced onto the background so
    // the list has rows at all). `truncated` is Qt's own answer to "did
    // this elide" — not a guess from a string length — so it measures the
    // laid-out text rather than the source.
    //
    // The rows are still a CONSTANT height, which is what the report row's
    // own comment demands: `noQmlWarnings` below and `qml-component-load`
    // are what hold the no-layout-loop half of that.
    //
    // UNFIXED TREE: fails naming every row that is shortened.
    void everyReadabilityRowSaysWhichPairItIsGrading()
    {
        const int w = m_window->width();
        const int h = m_window->height();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        m_window->setWidth(1920);
        m_window->setHeight(1000);
        QCoreApplication::processEvents();
        QTRY_COMPARE(m_window->width(), 1920);

        auto *loader = item("themeEditorLoader");
        QVERIFY2(loader, "the theme editor loader is not live");
        loader->setProperty("active", true);
        QCoreApplication::processEvents();
        QTRY_VERIFY(popup("themeEditorDialog"));
        QObject *dialog = popup("themeEditorDialog");
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto *store = m_controller->customTheme();
        QVERIFY(store);

        QStringList cut;
        QSet<QString> phrases;
        int checkedPicker = 0;
        int checkedReport = 0;

        auto sweep = [&](const char *objectName, const char *where) {
            const auto labels = items(objectName);
            int seen = 0;
            for (QQuickItem *label : labels) {
                if (!label->isVisible() || label->width() <= 0.0)
                    continue;
                ++seen;
                const QString text = label->property("text").toString();
                phrases.insert(text);
                if (label->property("truncated").toBool())
                    cut.append(QStringLiteral(
                        "in a %1x%2 px box the %3 row reads \"%4\" — the "
                        "sentence naming the pair being graded is cut, so "
                        "the number beside it is about nothing the reader "
                        "can see (it laid out %5 line(s) in %6 px, and a "
                        "box one pixel short of two lines shows one)")
                            .arg(label->width())
                            .arg(label->height())
                            .arg(QLatin1String(where))
                            .arg(text)
                            .arg(label->property("lineCount").toInt())
                            .arg(label->property("contentHeight").toReal()));
            }
            return seen;
        };

        // 1) The live readout, one role at a time. `rail` carries the row
        //    the report named — "The Spaces rail against the room list" —
        //    and the three ink roles carry the longest sentences there are.
        const char *roles[] = { "rail", "textPrimary", "textSecondary",
                                "textMuted" };
        for (const char *role : roles) {
            QMetaObject::invokeMethod(
                dialog, "beginEdit",
                Q_ARG(QVariant, QVariant(QLatin1String(role))),
                Q_ARG(QVariant, QVariant(QLatin1String(role))));
            QCoreApplication::processEvents();
            QTRY_VERIFY(!items("themeRoleCheckLabel").isEmpty());
            checkedPicker += sweep("themeRoleCheckLabel", "live readout");
        }

        // 2) The findings list. It is empty on a clean palette by design
        //    (`everyReadabilityCheckPassesOnEveryShippedPreset`), so three
        //    inks are painted onto the background colour: ratio 1.0, which
        //    fails the three longest ink checks in the table.
        dialog->setProperty("editingRole", QString());
        dialog->setProperty("reportOpen", true);
        QCoreApplication::processEvents();
        for (const char *role : { "background", "textPrimary", "textSecondary",
                                  "textMuted" })
            store->setColor(QLatin1String(role), QStringLiteral("#808080"));
        QTRY_VERIFY(dialog->property("readabilityProblems").toInt() >= 3);
        QCoreApplication::processEvents();
        checkedReport = sweep("themeReadabilityRowLabel", "findings list");

        // WHAT THE TALLER ROW COSTS, recorded rather than asserted: the
        // number of findings visible at once is a judgement Rokas owns, and
        // pinning it here would make an ordinary copy edit red. The rows
        // being IDENTICAL is the part that is a contract -- that is what
        // "still a constant" means, and a row that grew from its own text
        // would break it.
        QList<qreal> rowHeights;
        for (QQuickItem *row : items("themeReadabilityRow")) {
            if (row->isVisible())
                rowHeights.append(row->height());
        }
        if (auto *scroll = item("themeReadabilityScroll");
            scroll && !rowHeights.isEmpty()) {
            qInfo("readability findings: %.0f px viewport, %.0f px rows -> "
                  "%d visible at once",
                  scroll->height(), rowHeights.first(),
                  int(scroll->height() / (rowHeights.first() + 2.0)));
        }
        QStringList ragged;
        for (qreal height : rowHeights) {
            if (!qFuzzyCompare(height, rowHeights.first()))
                ragged.append(QStringLiteral("%1 px among %2 px rows")
                                  .arg(height).arg(rowHeights.first()));
        }

        // Restore before asserting: a failure must not leave the editor
        // open, the window at 1920, or a grey palette on disk.
        store->resetAll();
        loader->setProperty("active", false);
        QCoreApplication::processEvents();
        m_window->setWidth(w);
        m_window->setHeight(h);
        QCoreApplication::processEvents();
        QTRY_COMPARE(m_window->width(), w);
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        QVERIFY2(checkedPicker >= 8,
                 qPrintable(QStringLiteral(
                     "only %1 live-readout rows were on screen to measure "
                     "across four roles").arg(checkedPicker)));
        QVERIFY2(checkedReport >= 3,
                 qPrintable(QStringLiteral(
                     "only %1 findings rows were on screen to measure")
                         .arg(checkedReport)));
        // The sweep is only as good as the sentences it saw: the two
        // longest phrases in the table are the ones that were cut, and a
        // pass that never rendered them would prove nothing.
        QVERIFY2(phrases.contains(QStringLiteral(
                     "The Spaces rail against the room list")),
                 "the row the report named was never rendered");
        QVERIFY2(phrases.contains(QStringLiteral(
                     "Secondary text on the conversation background")),
                 "the longest sentence in the table was never rendered");
        QVERIFY2(ragged.isEmpty(),
                 qPrintable(QStringLiteral(
                     "the findings rows are no longer one height, so a row "
                     "has started deriving its height from its own text: %1")
                         .arg(ragged.join(QStringLiteral(", ")))));
        QVERIFY2(cut.isEmpty(), qPrintable(cut.join(QStringLiteral("\n  "))));
    }

    void noQmlWarnings()
    {
        QCOMPARE(m_warnings, QStringList{});
    }

    // ---- the selected nav row's caret sits INSIDE the row ----

    void theNavCaretDoesNotStraddleTheSelectionCorner()
    {
        // REPORTED FROM A REAL DESKTOP, 2026-09-13: the bolt caret looked
        // broken. It was anchored at `leftMargin: -2`, deliberately
        // overhanging the row -- and against a radiusTile background that put
        // an 11px glyph across the pill's ROUNDED CORNER, half on the
        // selection fill and half on the panel behind it. An overhang only
        // reads as a caret if it clears the curve, which at this radius and
        // this glyph size it never did.
        //
        // Geometry, not a source scan: this loads the real component and
        // measures the caret against its own row, so it fails on the old
        // negative margin for the actual reason rather than on a spelling.
        // Self-sufficient: earlier cases leave a search term in the field,
        // and a narrowed nav hides every row that does not match.
        m_controller->showSettings();
        QCoreApplication::processEvents();
        if (auto *search = item("settingsSearchField")) {
            search->setProperty("text", QString());
            QCoreApplication::processEvents();
        }

        auto *nav = item("settingsNavRow_appearance");
        QVERIFY(nav);
        QTRY_VERIFY(nav->isVisible());
        QVERIFY2(nav->property("highlighted").toBool(),
                 "Appearance is the section Settings opens on, so its row is "
                 "the selected one and the only one showing a caret");

        auto *caret = nav->findChild<QQuickItem *>(QStringLiteral("settingsNavCaret"));
        QVERIFY2(caret, "the selected nav row has no caret");
        QTRY_VERIFY(caret->isVisible());

        QVERIFY2(caret->x() >= 0.0,
                 qPrintable(QStringLiteral(
                     "the caret starts at x=%1, outside its own row, so it is "
                     "drawn across the selection pill's rounded corner")
                     .arg(caret->x())));
        const qreal contentInset = nav->property("leftPadding").toReal();
        QVERIFY2(caret->x() + caret->width() <= contentInset,
                 qPrintable(QStringLiteral(
                     "the caret ends at x=%1 but content starts at %2, so the "
                     "caret and the row's icon overlap")
                     .arg(caret->x() + caret->width()).arg(contentInset)));
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    SettingsShellQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "SettingsShellQmlTest.moc"
