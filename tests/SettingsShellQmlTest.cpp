// Full-view Settings against the production Main window: opening Settings
// hides the whole chat shell and any right-side panel and fills the content
// area; closing restores the shell and the selected room. The Appearance
// controls (featured theme cards, match-system, message layout, text size)
// are exercised in place, including no horizontal clipping at 1374x944.

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

// WCAG 2.1 relative luminance and contrast of a colour a live control
// resolved (ThemeTokensTest works on the QML literals instead).
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

// CIE L* of a rendered colour: the right measure for two fills, where a
// contrast ratio can call two clearly distinct surfaces nearly equal.
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

    // Every item with that name: Repeater delegates share one objectName and
    // findChild stops at the first.
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

    // A Popup is a QObject, not a QQuickItem; its popupItem is reparented to
    // the overlay without an objectName, so look it up as a QObject.
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

    // Scroll `target` into its nearest Flickable's viewport only if it is
    // outside it (scrolling a visible control would move positions other
    // tests assert). A click outside the window is dropped by Qt.
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

    // Y within the nearest Flickable's contentItem, which does not change on
    // scroll. Reflow guards measure this, since a scroll is not a reflow.
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
        // Also isolate the data home: AppDataPaths composes its own root and
        // ignores the QSettings org/app names, so writes would otherwise land
        // in the real user data directory and leak between runs.
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
                        // Mock media URLs use an unresolvable host, and rows in
                        // the viewport activate their media; drop only that
                        // exact DNS warning.
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
        // The window size from the original screenshot.
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

    // The GIF settings combos show persisted non-default values on the first
    // open (creation-time indexOfValue bindings showed defaults). Runs first so
    // Settings instantiates fresh with the values already stored.
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
        // Restore the shared shell state for the tests that follow (fresh
        // Settings on the default section, chat shell beneath).
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

        // The whole chat shell disappears and Settings fills the content area.
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
        // Every featured theme card fits in the content area, and the
        // appearance column never overflows horizontally.
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
        // Each featured card reads AppTheme.paletteForTheme(id). Compare
        // against the raw per-theme underscore literals that function returns,
        // not the routed aliases (e.g. stormDeep), which only hold Storm's
        // value while Storm is active.
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
        // The selection glow (3 px, outside the card) and the focus ring
        // (2 px at -6..-4) render in full, not clipped by the card.
        m_controller->settings()->setTheme(SettingsManager::IndigoNightTheme);
        QCoreApplication::processEvents();
        auto *card = item("featuredThemeCard_9");
        QVERIFY(card);
        QVERIFY(!card->clip());

        const QImage selected = m_window->grabWindow();
        QVERIFY(!selected.isNull());
        // Mid-height, ~1.5 px outside the right edge, inside the glow band.
        // Storm's glow is bolt at 18% over stormDeep; expect that blend.
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
        // Sample one column inside the 2 px focus band.
        const QPointF focusPoint =
            card->mapToScene(QPointF(card->width() + 4.5, card->height() / 2));
        // Settings focus rings use bolt under Storm.
        QVERIFY2(channelDelta(sampleAvg(focused,
                      QRect(int(focusPoint.x()), int(focusPoint.y()) - 1, 1, 2)),
                      themeColor("bolt")) <= kTolerance,
                 "focus ring invisible outside the card edge");
    }

    // The unselected theme card's radio ring clears 3:1 (WCAG 1.4.11) against
    // the card foot on every theme. Read off the live control under each
    // live theme, not off a token name.
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
            // A featured card that is not the active theme, so the ring is
            // in its resting state.
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
            // The live foot fill, not a token name.
            auto *foot = item(qPrintable(
                QStringLiteral("themeCardFoot_%1").arg(cardId)));
            QVERIFY2(foot, qPrintable(QStringLiteral(
                         "no themeCardFoot_%1 under theme %2")
                             .arg(cardId).arg(id)));
            const QColor footColor = foot->property("color").value<QColor>();
            QVERIFY(footColor.isValid());
            const double ratio = contrastRatio(ring, footColor);
            if (ratio < worst) {
                worst = ratio;
                worstWhere = QStringLiteral("theme %1: ring %2 on %3")
                                 .arg(id).arg(ring.name(), footColor.name());
            }
            ++checked;
            QVERIFY2(ratio >= 3.0,
                     qPrintable(QStringLiteral(
                         "theme %1: the resting theme-card radio ring is "
                         "%2 on %3 = %4:1, below the 3:1 a control boundary "
                         "needs")
                             .arg(id)
                             .arg(ring.name(), footColor.name())
                             .arg(ratio, 0, 'f', 2)));
        }
        // Assert how many palettes were actually checked, not loop iterations.
        QCOMPARE(checked, 11);
        qInfo("theme-card radio ring: worst %.2f:1 (%s)",
              worst, qPrintable(worstWhere));

        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(restore));
        QCoreApplication::processEvents();
    }

    // A SettingsCard is visible against the page on every theme: its fill
    // must differ from the page by at least 5 dL* (read off the live card and
    // ground Rectangles). Under non-Storm themes the old tokens both resolved
    // to the palette background.
    void theSettingsCardIsVisibleAgainstThePageOnEveryTheme()
    {
        const int restore = m_controller->settings()->theme();
        // Appearance: the section does not change what is measured, but later
        // cases click Appearance controls, so leave the screen there.
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
        // Assert how many palettes were actually checked.
        QCOMPARE(checked, 11);
        qInfo("settings card vs page: worst %.1f dL* (%s)",
              worst, qPrintable(worstWhere));

        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(restore));
        QCoreApplication::processEvents();
    }

    // The featured theme cards (bespoke, not SettingsCards) are visible
    // against the page too: the body must be a different plane from the page,
    // and the resting edge must clear 3:1, since it draws the silhouette
    // across the preview half. Read off live items.
    void theFeaturedThemeCardIsVisibleAgainstThePageOnEveryTheme()
    {
        const int restore = m_controller->settings()->theme();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        auto *ground = item("settingsPageGround");
        QVERIFY2(ground, "no live settings page ground rectangle");
        // All four cards, not just the first Repeater delegate.
        const int cardIds[] = { 9, 8, 10, 11 };

        const int themes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        double worstSep = 1000.0;
        double worstEdge = 1000.0;
        QString worstSepWhere;
        QString worstEdgeWhere;
        int measured = 0;
        QSet<QRgb> distinctPages;
        for (int id : themes) {
            m_controller->settings()->setTheme(
                static_cast<SettingsManager::Theme>(id));
            QCoreApplication::processEvents();
            const QColor page = ground->property("color").value<QColor>();
            QVERIFY(page.isValid());
            distinctPages.insert(page.rgb());
            for (int cardId : cardIds) {
                auto *card = item(qPrintable(
                    QStringLiteral("featuredThemeCard_%1").arg(cardId)));
                auto *outline = item(qPrintable(
                    QStringLiteral("themeCardOutline_%1").arg(cardId)));
                QVERIFY2(card, qPrintable(QStringLiteral(
                             "no featuredThemeCard_%1 under theme %2")
                                 .arg(cardId).arg(id)));
                QVERIFY2(outline, qPrintable(QStringLiteral(
                             "no themeCardOutline_%1 under theme %2")
                                 .arg(cardId).arg(id)));
            // Resting edge only: a live card carries the bolt edge instead.
                if (card->property("cardIsLive").toBool())
                    continue;

                const QColor fill = card->property("color").value<QColor>();
                QVERIFY(fill.isValid());
                const double sep = qAbs(lstarOf(fill) - lstarOf(page));
                if (sep < worstSep) {
                    worstSep = sep;
                    worstSepWhere = QStringLiteral("theme %1 card %2: %3 on %4")
                                        .arg(id).arg(cardId)
                                        .arg(fill.name(), page.name());
                }
                QVERIFY2(sep >= 5.0,
                         qPrintable(QStringLiteral(
                             "theme %1: the featured theme card %2 is %3 on a "
                             "page of %4 — only %5 dL* (%6:1) apart. A card "
                             "painted in the colour of the page behind it is "
                             "not a card")
                                 .arg(id).arg(cardId)
                                 .arg(fill.name(), page.name())
                                 .arg(sep, 0, 'f', 1)
                                 .arg(contrastRatio(fill, page), 0, 'f', 2)));

                QQmlExpression expr(qmlContext(outline), outline,
                                    QStringLiteral("border.color"));
                const QColor edge = expr.evaluate().value<QColor>();
                QVERIFY(edge.isValid());
                const double edgeRatio = contrastRatio(edge, page);
                if (edgeRatio < worstEdge) {
                    worstEdge = edgeRatio;
                    worstEdgeWhere =
                        QStringLiteral("theme %1 card %2: %3 on %4")
                            .arg(id).arg(cardId).arg(edge.name(), page.name());
                }
                QVERIFY2(edgeRatio >= 3.0,
                         qPrintable(QStringLiteral(
                             "theme %1: the resting edge of theme card %2 is "
                             "%3 on the page %4 = %5:1, below the 3:1 a "
                             "component boundary needs — and this edge is the "
                             "only thing bounding the preview half, which "
                             "paints an arbitrary palette")
                                 .arg(id).arg(cardId)
                                 .arg(edge.name(), page.name())
                                 .arg(edgeRatio, 0, 'f', 2)));
                ++measured;
            }
        }
        // Assert the counts: eleven distinct page colours (not one palette
        // measured eleven times)...
        QCOMPARE(distinctPages.size(), 11);
        // ...and 40 cards measured: 44 minus the one live card per featured
        // theme.
        QCOMPARE(measured, 40);
        qInfo("featured theme card: worst %.1f dL* (%s), worst edge %.2f:1 (%s)",
              worstSep, qPrintable(worstSepWhere),
              worstEdge, qPrintable(worstEdgeWhere));

        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(restore));
        QCoreApplication::processEvents();
    }

    // With match-system on (theme 0), the card whose theme is actually in
    // effect is marked as live (bolt edge and ring) without being reported as
    // chosen: no filled radio, `Accessible.checked` false, and the state in
    // its accessible name.
    void theThemeInEffectIsMarkedWhenMatchSystemIsOn()
    {
        const int restore = m_controller->settings()->theme();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(0));
        QCoreApplication::processEvents();

        // Read the effective theme from the singleton rather than assuming the
        // platform's scheme.
        QQmlExpression effExpr(qmlContext(m_window), m_window,
                               QStringLiteral("AppTheme.effectiveTheme"));
        const int effective = effExpr.evaluate().toInt();
        QVERIFY2(effective == 8 || effective == 9,
                 qPrintable(QStringLiteral("match-system resolved to %1, "
                                           "which is not a featured card")
                                .arg(effective)));

        const int cardIds[] = { 9, 8, 10, 11 };
        int live = 0;
        int chosen = 0;
        for (int cardId : cardIds) {
            auto *card = item(qPrintable(
                QStringLiteral("featuredThemeCard_%1").arg(cardId)));
            QVERIFY(card);
            const bool isLive = card->property("cardIsLive").toBool();
            const bool isChosen = card->property("selectedTheme").toBool();
            if (isLive)
                ++live;
            if (isChosen)
                ++chosen;
            QVERIFY2(isLive == (cardId == effective),
                     qPrintable(QStringLiteral(
                         "match-system resolved to theme %1, and card %2 "
                         "reports live=%3 — with no theme chosen, exactly the "
                         "card whose theme is running must be marked and no "
                         "other")
                            .arg(effective).arg(cardId)
                            .arg(isLive ? QStringLiteral("true")
                                        : QStringLiteral("false"))));
            // Nothing was chosen: match-system is on.
            QVERIFY2(!isChosen,
                     qPrintable(QStringLiteral("card %1 reports a user "
                                               "choice while match-system is "
                                               "on").arg(cardId)));

            auto *radio = item(qPrintable(
                QStringLiteral("themeCardRadio_%1").arg(cardId)));
            QVERIFY(radio);
            const QColor ringFill =
                radio->property("color").value<QColor>();
            QQmlExpression ringExpr(qmlContext(radio), radio,
                                    QStringLiteral("border.color"));
            const QColor ring = ringExpr.evaluate().value<QColor>();
            const QColor bolt = themeColor("bolt");
            if (isLive) {
                // The ring says "running"; only a filled radio would say
                // "chosen".
                QCOMPARE(ring, bolt);
                QVERIFY2(ringFill.alpha() == 0,
                         qPrintable(QStringLiteral(
                             "card %1 draws a FILLED radio for a theme the "
                             "system chose, not the user").arg(cardId)));
            } else {
                QVERIFY2(ring != bolt,
                         qPrintable(QStringLiteral(
                             "card %1 is not in effect and not chosen, yet "
                             "carries the bolt ring").arg(cardId)));
            }
        }
        QCOMPARE(live, 1);
        QCOMPARE(chosen, 0);

        // An attached property is not a QObject property, so read it through
        // a QQmlExpression.
        auto attached = [](QQuickItem *it, const char *what) {
            QQmlExpression expr(qmlContext(it), it,
                                QStringLiteral("Accessible.%1")
                                    .arg(QLatin1String(what)));
            return expr.evaluate();
        };
        auto *liveCard = item(qPrintable(
            QStringLiteral("featuredThemeCard_%1").arg(effective)));
        auto *otherCard = item(qPrintable(
            QStringLiteral("featuredThemeCard_%1").arg(effective == 8 ? 9 : 8)));
        QVERIFY(liveCard && otherCard);
        const QString liveName = attached(liveCard, "name").toString();
        const QString otherName = attached(otherCard, "name").toString();
        QVERIFY2(!liveName.isEmpty() && !otherName.isEmpty(),
                 "the theme cards report no accessible name at all");
        QVERIFY2(liveName != otherName,
                 qPrintable(QStringLiteral("accessible name %1 vs %2")
                                .arg(liveName, otherName)));
        QVERIFY2(liveName.length() > otherName.length(),
                 qPrintable(QStringLiteral(
                     "the in-effect card reads %1, which carries no more "
                     "state than a plain card name").arg(liveName)));
        QVERIFY2(!attached(liveCard, "checked").toBool(),
                 "a theme the system chose must not report itself checked");

        // A card the user did choose fills its radio and reports checked.
        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(10));
        QCoreApplication::processEvents();
        auto *teal = item("featuredThemeCard_10");
        auto *tealRadio = item("themeCardRadio_10");
        QVERIFY(teal && tealRadio);
        QVERIFY(teal->property("selectedTheme").toBool());
        QVERIFY(teal->property("cardIsLive").toBool());
        QVERIFY2(!teal->property("inEffect").toBool(),
                 "an explicitly chosen theme is not in effect BY the system");
        QCOMPARE(tealRadio->property("color").value<QColor>(),
                 themeColor("bolt"));
        QVERIFY(attached(teal, "checked").toBool());
        QCOMPARE(attached(teal, "name").toString(),
                 QStringLiteral("Deep Teal"));

        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(restore));
        QCoreApplication::processEvents();
    }
    // The selected nav row's fill is visible against the nav column on every
    // theme (at least 4 dL*; Moss Light is the hardest at ~5.5).
    void theSelectedNavRowHasAVisibleFillOnEveryTheme()
    {
        const int restore = m_controller->settings()->theme();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();

        auto *row = item("settingsNavRow_appearance");
        auto *fill = item("settingsNavRowFill_appearance");
        auto *column = item("settingsNavColumn");
        QVERIFY2(row && fill && column, "the appearance nav row is not live");
        // Guard the premise: an unhighlighted row is transparent, which would
        // sample as black and pass on light themes.
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

        // Storm switches instantly like every other featured card.
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
        // Indigo Night sorts first among the featured cards, and "More themes"
        // never repeats a featured card.
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
        // "More themes" lists only the 7 non-featured presets. Moss (8) is
        // featured, so check a non-featured preset (Lightning Light) instead.
        QVERIFY2(!item("miniThemeCard_11"),
                 "Storm must not also render in the MORE THEMES row");
        QVERIFY2(item("miniThemeCard_1"),
                 "the MORE THEMES row must still list the non-featured presets");
    }

    void interactingWithOrdinaryRowsNeverReflowsContentBelow()
    {
        // Pressing, focusing or toggling an ordinary settings control must not
        // move content below it: focus rings and glows are overlays and
        // implicit heights are constant. The intentional disclosure expanders
        // are not covered.
        m_controller->settings()->setTheme(SettingsManager::IndigoNightTheme);
        QCoreApplication::processEvents();

        // The message-layout control sits below the theme cards and the
        // match-system row; toggling or focusing those must not move it.
        auto *anchor = item("messageLayoutControl");
        QVERIFY(anchor);
        // Content-relative: clickItem() may scroll, which is not a reflow.
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

        // Toggling "Show room activity" must not move the wheel-speed combo
        // below it.
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

        // The slider thumbs stay white across the whole range (past ~115% they
        // flipped to a dark ink and read as disabled).
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

    // Room info tabs wrap into two rows when the panel is too narrow, driven
    // by the real layout at real widths rather than by setting `tabsWrap`.
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

        // Narrow: the strip collapses and two rows carry every tab. Four tabs
        // fit the 260 px floor, so enable pins and re-open the room to get the
        // Pinned tab (five tabs).
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        mock->mockSupportsPinnedMessages = true;
        // `supported` is read live; the binding learns of the change through
        // this signal.
        Q_EMIT m_controller->pinned()->supportedChanged();
        // The Pinned tab also needs the panel showing the room the pin
        // controller tracks (openForRoom() does this in production).
        m_controller->roomInfo()->setRoomId(QStringLiteral("!general:mock.local"));
        m_controller->pinned()->setRoomId(QStringLiteral("!general:mock.local"));
        QTRY_COMPARE(strip->property("model").toList().size(), 5);
        m_controller->settings()->setSidePanelWidth(260);
        QTRY_COMPARE(panel->width(), 260.0);
        QTRY_VERIFY2(strip->property("overflowing").toBool(),
                     qPrintable(QStringLiteral("natural %1 px in %2 px, %3 tabs")
                                    .arg(strip->implicitWidth()).arg(strip->width())
                                    .arg(strip->property("model").toList().size())));
        // The strip leaves the layout rather than collapsing to zero height,
        // which made the panel's ColumnLayout loop.
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
        // Neither row may itself overflow the panel.
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

    // Opening the panel at a width that already needs the wrap, repeatedly,
    // then crossing the threshold both ways. Rewriting the rows' model during
    // the layout pass crashed in production; offscreen does not reproduce the
    // crash, so this is a behaviour gate for opening straight into the wrap.
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

        // Stored narrow before the open, so the wrap decision lands on the
        // panel's first layout pass.
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

    // Settings is built once and kept: the same item survives a close and
    // serves the next open, and a section requested while it is alive still
    // lands.
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
        // On re-open, focus lands in the screen, not on the composer.
        QTRY_VERIFY(first->property("activeFocus").toBool());

        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        QCOMPARE(first->property("section").toString(),
                 QStringLiteral("appearance"));
        m_controller->showMain();
        QCoreApplication::processEvents();
    }

    // Escape still closes the info panel after Settings has been opened: the
    // kept-alive screen's Escape Shortcut must be disabled while hidden, or
    // two enabled Shortcuts on one sequence make Qt fire neither.
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

    // A custom name colour (the tenth swatch opens a picker) reaches the
    // server once, on Apply, as a colour that matches none of the nine slots.
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
        // The picker's own signal, as a drag raises it.
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

    // The info panel hides with the chat shell while Settings is up and comes
    // back with its section when Settings closes.
    void openSettingsFromRoomInfoHidesThePanelAndRestoresIt()
    {
        auto *timeline = timelinePane();
        QVERIFY(timeline);
        // Simulate the info panel at the state level (its content is a
        // Rust-backend surface).
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

    // The Settings header is exactly as tall as the room header band it
    // replaces on screen, read off the live room header.

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

    // The quick switcher offers every Settings section the nav has. Both are
    // literal lists in two files, so this is a source scan; each half asserts
    // a non-zero count first so a broken pattern cannot pass.
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

        // Nav rows; the pattern requires a non-empty value to skip the
        // property declaration.
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

        // The floor is the real section count, so sections cannot vanish from
        // both files unnoticed. Keep in step with SettingsScreen.qml's
        // `sectionTitle()`.
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

        // "room activity" matches only Appearance's "Show room activity", so
        // the nav narrows to that section.
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

        // The inline control binds the same SettingsManager property as the
        // Appearance control.
        auto *inlineToggle = item("settingsSearchInlineShowRoomActivity_0");
        QVERIFY(inlineToggle);
        QVERIFY(inlineToggle->isVisible());
        QMetaObject::invokeMethod(inlineToggle, "toggled");
        QCOMPARE(m_controller->settings()->showRoomActivity(), !activityBefore);
        // Restore for later tests.
        QMetaObject::invokeMethod(inlineToggle, "toggled");
        QCOMPARE(m_controller->settings()->showRoomActivity(), activityBefore);

        // Clearing the search restores the full nav.
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
        QTRY_VERIFY(!resultsPanel->isVisible());
        QTRY_VERIFY(accountNav->isVisible());
    }

    // A search matching nothing never leaves Settings with an empty nav, and
    // the query does not survive closing the (kept-alive) screen.
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

        // The stale query does not survive closing the screen.
        m_controller->showMain();
        QCoreApplication::processEvents();
        QTRY_COMPARE(search->property("text").toString(), QString());

        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(accountNav->isVisible());
        QVERIFY2(!resultsPanel->isVisible(),
                 "Settings reopened still filtered by the previous query");
    }

    // The call device pickers live in "Sound & video" and are no longer in
    // Notifications, driven through real section switches.
    void theCallDevicesLiveInTheSoundSectionAndLeaveNotifications()
    {
        m_controller->showSettingsSection(QStringLiteral("sound"));
        QCoreApplication::processEvents();

        auto *devices = item("callDeviceSettings");
        QVERIFY2(devices, "there is no callDeviceSettings anywhere in Settings");
        QTRY_VERIFY2(devices->isVisible(),
                     "the call device pickers are not shown by the sound "
                     "section");
        // Enumeration is lazy (Qt Multimedia is slow on PipeWire); showing the
        // section arms it.
        QVERIFY2(devices->property("activated").toBool(),
                 "the device pickers were never activated, so they enumerate "
                 "nothing and render three empty combo boxes");

        // The media playback level has a home in Settings.
        auto *mediaLevel = item("mediaVolumeSettingSlider");
        QVERIFY2(mediaLevel, "the sound section has no media playback level");
        QVERIFY(mediaLevel->isVisible());

        m_controller->showSettingsSection(QStringLiteral("notifications"));
        QCoreApplication::processEvents();
        QTRY_VERIFY2(!devices->isVisible(),
                     "the call device pickers are still shown by Notifications "
                     "— the move left a copy behind");

        // The ringer stays in Notifications, beside the notification sound it
        // is gated with.
        auto *ring = item("ringForCallsCheck");
        QVERIFY(ring);
        QTRY_VERIFY(ring->isVisible());

        // Settings is left open on purpose: later cases click nav rows without
        // opening Settings and inherit it from the case before.
    }

    // The sound section is reachable by search with the words people type.
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

        // Cleared, and Settings left open (see above).
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
    }

    // The starred-GIF store has its own count/size row and a confirmed Clear
    // All, exercised with real GifStarredStore state and a real Dialog.
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

        // The row binds live to the store's count/totalBytes.
        QCOMPARE(summary->property("text").toString(),
                 QStringLiteral("1 image(s), 10 B — kept on this device only and removed when you sign out of this account."));
        QVERIFY(clearButton->property("enabled").toBool());

        clickItem(clearButton);
        QCoreApplication::processEvents();
        auto *confirmDialog = m_window->findChild<QObject *>(
            QStringLiteral("starredGifsClearConfirm"));
        QVERIFY(confirmDialog);
        QVERIFY(confirmDialog->property("visible").toBool());

        // accept() runs the same onAccepted path as a real "Yes" click,
        // independent of the popup's screen position.
        QMetaObject::invokeMethod(confirmDialog, "accept");
        QCoreApplication::processEvents();

        QCOMPARE(store->count(), 0);
        QCOMPARE(summary->property("text").toString(),
                 QStringLiteral("0 image(s), 0 B — kept on this device only and removed when you sign out of this account."));
        QVERIFY(!clearButton->property("enabled").toBool());
    }

    // The by-id palette resolver (used for preview cards and the custom-theme
    // editor's preview) agrees with the live semantic aliases for every theme
    // and key.
    void previewPaletteMatchesLiveTokens()
    {
        // Left: key in paletteForTheme(). Right: the live AppTheme alias it
        // must equal. Paired so a key cannot be added to one side only.
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

    // Settings > Account offers a profile picture and a bio editor, checked by
    // navigating to the section (a source scan cannot tell reachability).
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

        // The editor follows the stored bio through a binding; an imperative
        // write to `text` would destroy it.
        QCOMPARE(bioField->property("text").toString(),
                 m_controller->bio() ? m_controller->bio()->ownBio()
                                     : QString());

        // Restore the shell for the cases that follow.
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
    }

    // A refused "Stop ignoring" is shown in the Ignored users card
    // (ModerationController::ignoreActionFinished), driven through the real
    // signal so a mis-wired Connections block fails.
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

        // Stand in for the button press (the card records who it asked
        // about), then deliver the controller's refusal.
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

        // Another user's outcome (e.g. from the profile popover) must not
        // paint an error into this card.
        card->setProperty("unignoreError", QString());
        card->setProperty("unignoreUserId", target);
        QVERIFY(QMetaObject::invokeMethod(
            moderation, "ignoreActionFinished", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("@someone-else:example.org")),
            Q_ARG(bool, true), Q_ARG(bool, false), Q_ARG(QString, refusal)));
        QCoreApplication::processEvents();
        QCOMPARE(card->property("unignoreError").toString(), QString());

        // A success clears the notice.
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

    // The notification-preview help names the mode that is actually the
    // default. The default comes from a scratch SettingsManager with an empty
    // store and the mode names from the live combo, and the text must not
    // call any other mode the default.
    void theNotificationHelpNamesTheModeThatIsActuallyTheDefault()
    {
        m_controller->showSettingsSection(QStringLiteral("notifications"));
        QCoreApplication::processEvents();

        auto *combo = item("notificationPreviewCombo");
        auto *help = item("notificationPreviewHelp");
        QVERIFY2(combo && help, "the notification preview row is not live");
        const QVariantList modes = combo->property("model").toList();
        QCOMPARE(modes.size(), 3);

        // A never-written store, so the getter returns its own default.
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

    // Every settings search index entry resolves its anchor to a live control
    // in the content pane; a typo would leave a result that does nothing.
    // Asserts the resolved count.
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
            // It must be in the content pane, not the nav column or a dialog.
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
        // Assert the count actually resolved.
        QCOMPARE(resolved, index.size());
    }

    // Clicking a result in the section you are already on still scrolls to
    // the control and lights the halo (setting the same section emits no
    // change). Clicks the result's title, not the row centre, where an inline
    // control may sit.
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
        // Read everything, restore the query, then assert, so a failure does
        // not leave a filtered nav.
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
        // The halo acknowledges the click even with nothing to scroll.
        QVERIFY2(haloOpacity > 0.5,
                 "the reveal halo never lit, so a click on a control already "
                 "on screen still has no feedback");
        QVERIFY2(qAbs(haloY - target) < 12.0,
                 qPrintable(QStringLiteral(
                     "the halo is at y=%1 and the control it names at y=%2")
                         .arg(haloY).arg(target)));
    }

    // A result in another section lands on the control, not at the top of the
    // page (`onSectionChanged` resets contentY).
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

    // A search result row's hover fill is visible against the nav column on
    // every theme (at least 4 dL*, as for the nav pill), read off the live
    // Rectangle under a real pointer.
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

        // Move away and back, so a move event is delivered whatever the
        // previous case left.
        QTest::mouseMove(m_window, QPoint(m_window->width() - 4, 4));
        QCoreApplication::processEvents();
        const QPointF centre = row->mapToScene(
            QPointF(row->width() / 2, row->height() / 2));
        QTest::mouseMove(m_window, centre.toPoint());
        QCoreApplication::processEvents();
        // Guard the premise: an unhovered row is transparent and would sample
        // as black.
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
        // Restore before asserting, so a failure does not leak a theme, a
        // query or a hovered pointer.
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

    // The Account page fits the app's own minimum window (640 px wide): every
    // control stays inside its card, measured in x on real delegates. A
    // RowLayout of swatches had an unshrinkable minimum that widened the whole
    // column.
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

        // Restore the window size before asserting.
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

    // No shortcut name is elided at 640x520: several names share long
    // prefixes, so elision makes rows indistinguishable. The distinctness
    // check keeps the case meaningful if `truncated` stops reporting.
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

    // The rail-depth segmented control's ink lines up with its label's ink
    // (a segment's first glyph is inset by its padding), compared on live
    // delegates so padding changes do not matter.
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
        // The segment centres its label, so the first glyph is half the slack
        // in from its content item's left edge.
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

    // The search-index help paragraph uses the same leading as its
    // neighbours, compared against a neighbour rather than a constant.
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

    // Every wrapping paragraph in SettingsScreen.qml sets body leading. A
    // Label is body copy when it sets `wrapMode`, unless it opts out with
    // `maximumLineCount: 1`, a monospace font, or a `// not-body-copy:` marker
    // with a reason. A source sweep, because many paragraphs are unreachable
    // at runtime in the mock; it asserts the counts it swept. The next case
    // measures rendered leading on a live delegate.
    void everyWrappingParagraphInSettingsSetsBodyLeading()
    {
        QFile file(QStringLiteral(QML_DIR) + QStringLiteral("/SettingsScreen.qml"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 "SettingsScreen.qml is not readable");
        const QStringList lines =
            QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));

        // Brace depth with string literals and line comments removed first.
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
            // The block's own properties are at depth 1; deeper ones belong to
            // nested items.
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

        // Assert the counts before the verdict, so a broken pattern cannot
        // pass. Floors, not exact values.
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

    // A swept Label's rendered leading matches the paragraph in its card,
    // compared as `contentHeight / lineCount / pixelSize` since the two use
    // different font sizes. Solid text measures ~1.42x and body leading
    // ~2.13x, so the non-vacuity floor is 1.8.
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

        // Not vacuous: the reference must itself be at body leading.
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

    // Every theme-editor readability row shows its full pair description at
    // a 1380 px window (a 304 px column), in both the live readout
    // and the findings list, using Qt's `truncated`. Rows stay a constant
    // height.
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

        // 1) The live readout, one role at a time; `rail` and the ink roles
        //    carry the longest sentences.
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

        // 2) The findings list, empty on a clean palette; paint three inks onto
        //    the background (ratio 1.0) so it has rows.
        dialog->setProperty("editingRole", QString());
        dialog->setProperty("reportOpen", true);
        QCoreApplication::processEvents();
        for (const char *role : { "background", "textPrimary", "textSecondary",
                                  "textMuted" })
            store->setColor(QLatin1String(role), QStringLiteral("#808080"));
        QTRY_VERIFY(dialog->property("readabilityProblems").toInt() >= 3);
        QCoreApplication::processEvents();
        checkedReport = sweep("themeReadabilityRowLabel", "findings list");

        // Row heights are recorded, not asserted, except that they are all
        // identical (a constant row height).
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

        // Restore before asserting: no open editor, 1920 window or grey
        // palette may leak.
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
        // The two longest phrases must have been rendered, or the sweep proves
        // nothing.
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

    // The selected nav row's caret sits inside the row.

    void theNavCaretDoesNotStraddleTheSelectionCorner()
    {
        // A negative margin put the caret across the pill's rounded corner.
        // Measured against its own row on the real component. Opens Settings
        // itself: earlier cases may leave a search term narrowing the nav.
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
