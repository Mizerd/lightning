// Full-view Settings against the production Main window: opening Settings
// hides the whole chat shell and any right-side panel and fills the content
// area; closing restores the shell and the selected room. The Appearance
// controls (featured theme cards, match-system, message layout, text size)
// are exercised in place, including no horizontal clipping at 1374x944.

#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>
#include <functional>

#include <QClipboard>
#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QGuiApplication>
#include <QImage>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QStyleHints>

#include "app/AppController.h"
#include "app/CustomThemeStore.h"
#include "app/ModerationController.h"
#include "models/MessageSearchController.h"
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

    // A language change re-evaluates every qsTr() model and ComboBox resets
    // currentIndex to 0 without `activated`; a combo bound to a stored setting
    // then showed row 0 (read receipts, notification preview). AppComboBox
    // restores the index itself. Asserts the COUNT of combos checked, so a
    // renamed objectName cannot turn this into a vacuous pass.
    void comboBoxesKeepTheirStoredValueAcrossARetranslation()
    {
        auto *settings = m_controller->settings();
        // Read receipts have two modes since 2026-10-18; Private is the last.
        settings->setReadReceiptMode(1);
        settings->setNotificationPreview(2);
        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();
        struct Case { const char *name; int expected; };
        const Case privacy[] = { { "readReceiptModeCombo", 1 } };
        int checked = 0;
        for (const Case &c : privacy) {
            auto *combo = item(c.name);
            QVERIFY2(combo, c.name);
            QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentIndex").toInt(),
                                      c.expected, 3000);
        }
        m_controller->showSettingsSection(QStringLiteral("notifications"));
        QCoreApplication::processEvents();
        const Case notif[] = { { "notificationPreviewCombo", 2 } };
        for (const Case &c : notif) {
            auto *combo = item(c.name);
            QVERIFY2(combo, c.name);
            QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentIndex").toInt(),
                                      c.expected, 3000);
        }
        // What setLanguage does through main.cpp's retranslateRequested.
        m_engine->retranslate();
        QCoreApplication::processEvents();
        const Case retranslated[] = { { "readReceiptModeCombo", 1 },
                                      { "notificationPreviewCombo", 2 } };
        for (const Case &c : retranslated) {
            auto *combo = item(c.name);
            QVERIFY2(combo, c.name);
            QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentIndex").toInt(),
                                      c.expected, 3000);
            ++checked;
        }
        QCOMPARE(checked, 2);
        settings->setReadReceiptMode(0);
        settings->setNotificationPreview(0);
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        m_controller->showMain();
    }

    // Escape peels the search query before it leaves Settings (B094).
    void settingsEscapePeelsTheSearchQueryBeforeLeaving()
    {
        m_controller->showSettings();
        QCoreApplication::processEvents();
        auto *field = item("settingsSearchField");
        QVERIFY(field);
        field->setProperty("text", QStringLiteral("zz"));
        QCoreApplication::processEvents();
        QTest::keyClick(m_window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QCOMPARE(field->property("text").toString(), QString());
        QCOMPARE(int(m_controller->currentScreen()),
                 int(AppController::SettingsScreen));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_COMPARE_WITH_TIMEOUT(int(m_controller->currentScreen()),
                                  int(AppController::MainScreen), 3000);
        m_controller->showMain();
    }

    // "Preload short videos": the switch, slider and number field live in the
    // Media card, show the stored values on first open, and stay in sync
    // both ways.
    void videoPrefetchRowLoadsAndSliderAndFieldStayInSync()
    {
        auto *settings = m_controller->settings();
        settings->setVideoPrefetchEnabled(true);
        settings->setVideoPrefetchMaxMb(40);
        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();
        auto *check = item("videoPrefetchCheck");
        auto *slider = item("videoPrefetchSlider");
        auto *field = item("videoPrefetchField");
        QVERIFY(check && slider && field);
        QTRY_COMPARE_WITH_TIMEOUT(check->property("checked").toBool(), true,
                                  3000);
        QTRY_COMPARE_WITH_TIMEOUT(slider->property("value").toInt(), 40, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(field->property("text").toString(),
                                  QStringLiteral("40"), 3000);
        QCOMPARE(slider->property("from").toInt(), 1);
        QCOMPARE(slider->property("to").toInt(), 100);
        // Setting -> slider and field.
        settings->setVideoPrefetchMaxMb(75);
        QTRY_COMPARE_WITH_TIMEOUT(slider->property("value").toInt(), 75, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(field->property("text").toString(),
                                  QStringLiteral("75"), 3000);
        // Field -> setting; an out-of-range number lands in range and the
        // field shows what was applied.
        field->setProperty("text", QStringLiteral("500"));
        QMetaObject::invokeMethod(field, "editingFinished");
        QCOMPARE(settings->videoPrefetchMaxMb(), 100);
        QTRY_COMPARE_WITH_TIMEOUT(field->property("text").toString(),
                                  QStringLiteral("100"), 3000);
        // An empty field reverts to the stored value.
        field->setProperty("text", QString());
        QMetaObject::invokeMethod(field, "editingFinished");
        QCOMPARE(settings->videoPrefetchMaxMb(), 100);
        QTRY_COMPARE_WITH_TIMEOUT(field->property("text").toString(),
                                  QStringLiteral("100"), 3000);
        // Off hides the limit controls.
        settings->setVideoPrefetchEnabled(false);
        QTRY_COMPARE_WITH_TIMEOUT(check->property("checked").toBool(), false,
                                  3000);
        QTRY_VERIFY_WITH_TIMEOUT(!item("videoPrefetchLimitRow")->isVisible(),
                                 3000);
        settings->setVideoPrefetchEnabled(true);
        settings->setVideoPrefetchMaxMb(SettingsManager::videoPrefetchDefaultMb());
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        m_controller->showMain();
        QTRY_VERIFY_WITH_TIMEOUT(
            item("spacesRail") && item("spacesRail")->isVisible(), 3000);
    }

    // 2026-10-01, a COSMIC desktop with no Secret Service: "Keep downloaded
    // media on this device" stayed ticked while nothing at all was kept, and
    // the only word of it was a sentence in a long grey paragraph. The line
    // under the box says what THIS session keeps, from the client.
    void keepMediaCheckboxSaysWhatThisSessionKeeps()
    {
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();
        auto *line = item("keepMediaOnDeviceState");
        QVERIFY2(line, "no state line beside the keep-media checkbox");
        // Nothing tells (the mock's default): no claim either way.
        QVERIFY(!line->isVisible());

        const struct {
            const char *state;
            const char *says;
        } cases[] = {
            {"kept", "including media from encrypted rooms"},
            {"keptExceptEncrypted", "except media from encrypted rooms"},
            {"notKeptNewSignIn", "just signed in"},
            {"keptExceptEncryptedPendingUpload", "finishes uploading"},
            {"notKeptKeyring", "locked or unavailable, or its key could not be read"},
            {"notKeptStore", "could not be opened"},
        };
        int shown = 0;
        for (const auto &c : cases) {
            mock->mockMediaKeepState = QLatin1String(c.state);
            Q_EMIT mock->mediaKeepStateChanged();
            QTRY_VERIFY2(line->property("text").toString().contains(
                             QLatin1String(c.says)),
                         qPrintable(QStringLiteral("%1: \"%2\"")
                                        .arg(QLatin1String(c.state),
                                             line->property("text").toString())));
            QVERIFY2(line->isVisible(), c.state);
            // Only the keyring-less state names the keyring as the reason.
            QCOMPARE(line->property("text").toString().contains(
                         QLatin1String("keyring holds the key")),
                     qstrcmp(c.state, "keptExceptEncrypted") == 0);
            ++shown;
        }
        QCOMPARE(shown, 6);

        // Keeping switched off: the line has nothing to report.
        m_controller->settings()->setKeepMediaOnDevice(false);
        QTRY_VERIFY(!line->isVisible());
        m_controller->settings()->setKeepMediaOnDevice(true);
        QTRY_VERIFY(line->isVisible());

        mock->mockMediaKeepState.clear();
        Q_EMIT mock->mediaKeepStateChanged();
        QTRY_VERIFY(!line->isVisible());
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
        QCOMPARE(slider->property("from").toInt(), 75);
        QCOMPARE(slider->property("to").toInt(), 150);
        m_controller->settings()->setTextScale(120);
        QCoreApplication::processEvents();
        QCOMPARE(slider->property("value").toInt(), 120);

        // The slider thumbs stay white across the whole range (past ~115% they
        // flipped to a dark ink and read as disabled).
        m_controller->settings()->setTextScale(150);
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

    // Ctrl+, from the chat shell (Settings not yet open) must open Settings
    // AND land keyboard focus in the search field, not just show the screen.
    // Before the fix, MainScreen's Shortcut called app.showSettings() alone;
    // only SettingsScreen's OWN Shortcut (gated to when it is already
    // visible, exercised by the next test) focused the field, so the very
    // first Ctrl+, from the chat shell opened Settings with focus nowhere
    // useful — Main.qml's loader only forces focus on its own root item.
    void ctrlCommaFromTheChatShellOpensSettingsAndFocusesSearch()
    {
        m_controller->showMain();
        QCoreApplication::processEvents();
        QTRY_VERIFY(item("spacesRail") && item("spacesRail")->isVisible());
        auto *settingsLoader = m_window->findChild<QQuickItem *>(
            QStringLiteral("settingsViewLoader"));
        QVERIFY(settingsLoader);
        QVERIFY(!settingsLoader->isVisible());

        QTest::keyClick(m_window, Qt::Key_Comma, Qt::ControlModifier);

        QTRY_VERIFY(settingsLoader->isVisible());
        auto *search = item("settingsSearchField");
        QVERIFY(search);
        QTRY_VERIFY(search->hasActiveFocus());
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

    // Notifications carries the choice of whose sound a notification makes,
    // its volume and the two Test buttons, bound to the real settings.
    void notificationsOffersLightningsChimeAndItsVolume()
    {
        m_controller->showSettingsSection(QStringLiteral("notifications"));
        QCoreApplication::processEvents();

        auto *combo = item("notificationSoundSourceCombo");
        QVERIFY2(combo, "Notifications has no sound style picker");
        QTRY_VERIFY(combo->isVisible());
        QCOMPARE(combo->property("currentIndex").toInt(), 0); // Lightning
        QCOMPARE(combo->property("count").toInt(), 2);

        auto *volume = item("notificationSoundVolumeSlider");
        QVERIFY2(volume, "Notifications has no notification volume slider");
        QCOMPARE(volume->property("value").toInt(),
                 SettingsManager::kDefaultNotificationSoundVolume);
        QVERIFY(item("notificationSoundPreviewButton"));
        QVERIFY(item("notificationMentionPreviewButton"));

        // The picker follows the setting, and the volume slider is only
        // live while Lightning's own chime is the one that plays.
        m_controller->settings()->setNotificationSoundSource(1);
        QTRY_COMPARE(combo->property("currentIndex").toInt(), 1);
        QTRY_VERIFY(!volume->property("enabled").toBool());
        m_controller->settings()->setNotificationSoundSource(0);
        QTRY_VERIFY(volume->property("enabled").toBool());

        // Test buttons press without a sink (offscreen: no audio) and without
        // a warning.
        clickItem(item("notificationSoundPreviewButton"));
        clickItem(item("notificationMentionPreviewButton"));
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

    // Chat 2026-10-22: the navigation listed sub-sections with no sign that
    // they fold. A section with sub-sections carries a chevron, down while its
    // list is open and right while it is folded. Since the master prompt's
    // Part 6 (3.2, UI-021) the chevron is its own control: IT folds the list,
    // and a repeated click on the row label only selects the page.
    void aSectionWithSubSectionsShowsAChevronAndFolds()
    {
        openSection(QStringLiteral("appearance"));
        auto *row = item("settingsNavRow_account");
        QVERIFY(row);
        clickItem(row);
        QCoreApplication::processEvents();
        auto *chevron = item("settingsNavChevron_account");
        auto *chevronButton = item("settingsNavChevronButton_account");
        QVERIFY2(chevron && chevronButton, "the Account row has no disclosure chevron");
        QVERIFY(chevronButton->isVisible());
        QVERIFY2(chevronButton->width() >= 24 && chevronButton->height() >= 24,
                 "the chevron's pointer target is smaller than 24px");
        QCOMPARE(chevron->property("name").toString(),
                 QStringLiteral("expand_more"));
        auto *outline = outlineEntry(QStringLiteral("account"), 0);
        QVERIFY2(outline && outline->isVisible(),
                 "the open section's sub-sections are not showing");
        // Sub-sections sit to the right of the root row's label.
        auto *rowLabelProbe = item("settingsNavRowFill_account");
        QVERIFY(rowLabelProbe);
        QVERIFY(outline->mapToItem(rowLabelProbe, QPointF(0, 0)).x() > 0);

        // A second click on the label keeps the list open (UI-021).
        clickItem(row);
        QCOMPARE(chevron->property("name").toString(),
                 QStringLiteral("expand_more"));
        QVERIFY(outlineEntry(QStringLiteral("account"), 0)->isVisible());

        clickItem(chevronButton);
        QCOMPARE(chevron->property("name").toString(),
                 QStringLiteral("chevron_right"));
        QVERIFY(!outlineEntry(QStringLiteral("account"), 0)->isVisible());
        // Folding did not change the page.
        QCOMPARE(item("settingsScreenRoot")->property("section").toString(),
                 QStringLiteral("account"));

        clickItem(chevronButton);
        QCOMPARE(chevron->property("name").toString(),
                 QStringLiteral("expand_more"));
        QVERIFY(outlineEntry(QStringLiteral("account"), 0)->isVisible());
    }

    // UI-017/018: branches expand independently of the selected page. The
    // outline used to show for the selected section only, so selecting
    // Privacy closed Appearance's list.
    void severalBranchesStayOpenAndAChevronDoesNotChangeThePage()
    {
        openSection(QStringLiteral("appearance"));
        auto *screen = item("settingsScreenRoot");
        QVERIFY(screen);
        callScreen("setOutlineExpanded", QStringLiteral("appearance"), true);
        callScreen("setOutlineExpanded", QStringLiteral("privacy"), false);
        QCoreApplication::processEvents();

        clickItem(item("settingsNavRow_privacy"));
        QCOMPARE(screen->property("section").toString(), QStringLiteral("privacy"));
        QVERIFY2(outlineEntry(QStringLiteral("appearance"), 0)->isVisible(),
                 "selecting Privacy closed Appearance's sub-sections");
        QVERIFY(outlineEntry(QStringLiteral("privacy"), 0)->isVisible());

        // A chevron on another branch changes only that branch.
        callScreen("setOutlineExpanded", QStringLiteral("notifications"), false);
        QCoreApplication::processEvents();
        clickItem(item("settingsNavChevronButton_notifications"));
        QCOMPARE(screen->property("section").toString(), QStringLiteral("privacy"));
        QVERIFY(outlineEntry(QStringLiteral("notifications"), 0)->isVisible());
        QVERIFY(outlineEntry(QStringLiteral("privacy"), 0)->isVisible());
        QVERIFY(outlineEntry(QStringLiteral("appearance"), 0)->isVisible());
        // Only the selected page's list marks a heading as current.
        auto *other = item("settingsNavOutline_notifications");
        QVERIFY(other);
        QCOMPARE(other->property("activeIndex").toInt(), -1);

        // A search hides the tree; clearing it brings back the same set.
        auto *search = item("settingsSearchField");
        search->setProperty("text", QStringLiteral("speaker"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(!outlineEntry(QStringLiteral("appearance"), 0)->isVisible());
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
        QTRY_VERIFY(outlineEntry(QStringLiteral("appearance"), 0)->isVisible());
        QVERIFY(outlineEntry(QStringLiteral("notifications"), 0)->isVisible());
        QVERIFY(outlineEntry(QStringLiteral("privacy"), 0)->isVisible());
    }

    // UI-019/020: a sub-section under another open branch selects that page
    // and reveals the heading once its content exists; of two rapid clicks in
    // different branches, the last one wins.
    void aSubSectionOfAnotherBranchOpensItsPageAndTheLastClickWins()
    {
        openSection(QStringLiteral("appearance"));
        auto *screen = item("settingsScreenRoot");
        callScreen("setOutlineExpanded", QStringLiteral("privacy"), true);
        callScreen("setOutlineExpanded", QStringLiteral("notifications"), true);
        QCoreApplication::processEvents();

        const QVariantList privacy =
            callScreen("outlineEntries", QStringLiteral("privacy")).toList();
        const QVariantList notifications =
            callScreen("outlineEntries", QStringLiteral("notifications")).toList();
        QVERIFY(privacy.size() > 2 && notifications.size() > 2);

        clickItem(outlineEntry(QStringLiteral("privacy"), 2));
        QCOMPARE(screen->property("section").toString(), QStringLiteral("privacy"));
        const QStringList privacyAnchors = privacy.at(2).toMap()
            .value(QStringLiteral("anchors")).toStringList();
        QVERIFY2(privacyAnchors.contains(
                     screen->property("searchRevealAnchor").toString()),
                 "the heading under the inactive branch was not revealed");
        QTRY_COMPARE_WITH_TIMEOUT(screen->property("outlineActive").toInt(), 2, 2000);

        // Two clicks in quick succession, different branches.
        clickItem(outlineEntry(QStringLiteral("appearance"), 3));
        clickItem(outlineEntry(QStringLiteral("notifications"), 2));
        QCOMPARE(screen->property("section").toString(),
                 QStringLiteral("notifications"));
        const QStringList notificationAnchors = notifications.at(2).toMap()
            .value(QStringLiteral("anchors")).toStringList();
        QTest::qWait(300);   // past the reveal settle (12 frames at most)
        QVERIFY2(notificationAnchors.contains(
                     screen->property("searchRevealAnchor").toString()),
                 "a stale reveal request won");
        QTRY_COMPARE_WITH_TIMEOUT(screen->property("outlineActive").toInt(), 2, 2000);
        // All three branches are still open.
        QVERIFY(outlineEntry(QStringLiteral("appearance"), 0)->isVisible());
        QVERIFY(outlineEntry(QStringLiteral("privacy"), 0)->isVisible());
    }

    // UI-024: Right opens a folded branch and Left folds it, on the row; Left
    // on a sub-section returns to its row. Neither selects a page.
    void arrowKeysFoldAndUnfoldABranch()
    {
        openSection(QStringLiteral("appearance"));
        auto *screen = item("settingsScreenRoot");
        callScreen("setOutlineExpanded", QStringLiteral("privacy"), false);
        QCoreApplication::processEvents();
        auto *row = item("settingsNavRow_privacy");
        row->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(row->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Right);
        QVERIFY(outlineEntry(QStringLiteral("privacy"), 0)->isVisible());
        QCOMPARE(screen->property("section").toString(), QStringLiteral("appearance"));
        auto *entry = outlineEntry(QStringLiteral("privacy"), 1);
        entry->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(m_window, Qt::Key_Left);
        QVERIFY2(row->hasActiveFocus(), "Left on a sub-section did not return to its row");
        QTest::keyClick(m_window, Qt::Key_Left);
        QVERIFY(!outlineEntry(QStringLiteral("privacy"), 0)->isVisible());
        QCOMPARE(screen->property("section").toString(), QStringLiteral("appearance"));
    }

    // Part 6, 3.3: the sub-section labels are navigation, one step larger than
    // the hint text they used to share, and their rows grow with the text.
    void subSectionLabelsUseTheirOwnLargerToken()
    {
        openSection(QStringLiteral("appearance"));
        auto *entry = outlineEntry(QStringLiteral("appearance"), 0);
        QVERIFY(entry);
        auto *label = entry->property("contentItem").value<QQuickItem *>();
        QVERIFY(label);
        const int size = label->property("font").value<QFont>().pixelSize();
        QCOMPARE(size, themeValue(QStringLiteral("settingsNavSubsectionSize")).toInt());
        QVERIFY2(size > themeValue(QStringLiteral("settingsHintSize")).toInt(),
                 "the sub-section labels are no larger than the hint text");
        QVERIFY(entry->height() >= label->property("implicitHeight").toReal());
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
            // Roles split out of a shared token: each must resolve, in the
            // editor's palette, to what the live token paints.
            QStringLiteral("warning"),         QStringLiteral("popoverSurface"),
            QStringLiteral("focusRing"),       QStringLiteral("scrollbarHandle"),
            QStringLiteral("reactionSelectedBackground"),
            QStringLiteral("presenceOnline"),
            QStringLiteral("dangerTint"),      QStringLiteral("messageHighlight"),
            QStringLiteral("textSelection"),   QStringLiteral("paletteHighlight"),
            QStringLiteral("roomSelected"),    QStringLiteral("roomHover"),
            QStringLiteral("channelSelected"), QStringLiteral("channelHover"),
            QStringLiteral("menuHighlight"),   QStringLiteral("buttonGhostHover"),
            QStringLiteral("timestampInk"),    QStringLiteral("placeholderInk"),
            QStringLiteral("settingsPage"),    QStringLiteral("settingsNav"),
        };
        // And every editable role, the per-element ones included: on every
        // preset, the colour the editor shows for an element is the one the
        // app paints it with (its pre-split token, Storm's own included).
        QStringList all = keys;
        for (const QString &role : CustomThemeStore::editableRoles()) {
            const QString key = m_controller->customTheme()->paletteKeyForRole(role);
            if (!all.contains(key))
                all << key;
        }
        QVERIFY2(all.size() > 120, qPrintable(QString::number(all.size())));
        const int original = int(m_controller->settings()->theme());
        for (int id = 1; id <= 11; ++id) {
            m_controller->settings()->setTheme(SettingsManager::Theme(id));
            QCoreApplication::processEvents();
            for (const QString &key : all) {
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
    // Settings option controls are sized for reading and for the pointer:
    // option text at the body size, every checkbox, switch and combo at least
    // AppTheme.settingsControlMinHeight tall. Walks the live items of each
    // section; counts what it measured so an empty walk cannot pass.
    static void collectControls(QQuickItem *parent, QList<QQuickItem *> &out)
    {
        if (!parent || !parent->isVisible())
            return;
        const QByteArray cls = parent->metaObject()->className();
        const auto *mo = parent->metaObject();
        if (mo->inherits(&QQuickItem::staticMetaObject)
            && (cls.startsWith("AppSwitch")
                || cls.startsWith("QQuickCheckBox")
                || cls.startsWith("QQuickComboBox")
                || cls.startsWith("AppComboBox")))
            out.append(parent);
        const auto children = parent->childItems();
        for (QQuickItem *child : children)
            collectControls(child, out);
    }

    static qreal fontPx(const QFont &f)
    {
        return f.pixelSize() > 0 ? f.pixelSize() : f.pointSizeF() * 96.0 / 72.0;
    }

    void settingsOptionControlsMeetBodySizeAndHitSize()
    {
        const qreal body = QQmlExpression(
            qmlContext(m_window), m_window,
            QStringLiteral("AppTheme.settingsOptionSize")).evaluate().toReal();
        const qreal minHit = QQmlExpression(
            qmlContext(m_window), m_window,
            QStringLiteral("AppTheme.settingsControlMinHeight")).evaluate().toReal();
        const qreal bodyRef = QQmlExpression(
            qmlContext(m_window), m_window,
            QStringLiteral("AppTheme.scaled(AppTheme.textBody)")).evaluate().toReal();
        QVERIFY2(body >= bodyRef && minHit >= 28,
                 "the settings tokens are missing or below the reading size");
        int measured = 0;
        QStringList bad;
        const QStringList sections = {QStringLiteral("account"),
            QStringLiteral("appearance"), QStringLiteral("notifications"),
            QStringLiteral("privacy"), QStringLiteral("sound"),
            QStringLiteral("shortcuts")};
        for (const QString &section : sections) {
            m_controller->showSettingsSection(section);
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();
            auto *root = item("settingsScreenRoot");
            QVERIFY(root);
            QList<QQuickItem *> controls;
            collectControls(root, controls);
            for (QQuickItem *c : std::as_const(controls)) {
                if (c->width() <= 0 || c->height() <= 0)
                    continue;
                ++measured;
                const QByteArray cls = c->metaObject()->className();
                const QString id = QStringLiteral("%1/%2 %3")
                    .arg(section, QString::fromLatin1(cls), c->objectName());
                const bool isSwitch = cls.startsWith("AppSwitch");
                qreal hit = c->height();
                if (isSwitch) {
                    const auto hp = c->property("hitHeight");
                    if (hp.isValid())
                        hit = hp.toReal();
                } else {
                    // The label and the indicator share the row.
                    const QFont f = c->property("font").value<QFont>();
                    if (fontPx(f) + 0.5 < body)
                        bad << QStringLiteral("%1 font %2px < %3px")
                                   .arg(id).arg(fontPx(f)).arg(body);
                }
                if (hit + 0.5 < minHit)
                    bad << QStringLiteral("%1 hit height %2 < %3")
                               .arg(id).arg(hit).arg(minHit);
            }
        }
        qWarning().noquote() << "measured" << measured << bad.size();
        QVERIFY2(measured >= 20, qPrintable(QStringLiteral("only %1 controls measured").arg(measured)));
        QVERIFY2(bad.isEmpty(), qPrintable(bad.join(QLatin1Char('\n'))));
    }

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

    // "Index all rooms" is found by searching Settings, its entry names a
    // live control, the card discloses the cost and the plaintext-at-rest
    // consequence, and the control follows the pass: start, pause, resume.
    // Old code: no entry and no control, so "index all" found nothing.
    void indexAllRoomsIsFindableInSettingsSearchAndFollowsThePass()
    {
        m_controller->showSettings();
        QCoreApplication::processEvents();
        auto *screen = item("settingsScreenRoot");
        auto *search = item("settingsSearchField");
        QVERIFY(screen && search);
        search->setProperty("text", QStringLiteral("index encrypted"));
        QCoreApplication::processEvents();
        bool found = false;
        const QVariantList hits =
            screen->property("matchedSearchResults").toList();
        for (const QVariant &hit : hits) {
            found = found
                || hit.toMap().value(QStringLiteral("anchor")).toString()
                       == QStringLiteral("indexAllRoomsButton");
        }
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
        QVERIFY2(found, "searching Settings for \"index encrypted\" does not find "
                        "Index encrypted rooms");

        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();
        auto *button = item("indexAllRoomsButton");
        auto *help = item("indexAllHelpText");
        QVERIFY2(button && help, "the index-all control is not live");
        const QString disclosure = help->property("text").toString();
        QVERIFY2(disclosure.contains(QStringLiteral("every encrypted room"))
                     && disclosure.contains(QStringLiteral("bandwidth"))
                     && disclosure.contains(QStringLiteral("decrypted"))
                     && disclosure.contains(QStringLiteral("cannot be decrypted")),
                 "the index-all disclosure lost its cost, privacy or "
                 "undecryptable-history sentence");
        QCOMPARE(button->property("text").toString(),
                 QStringLiteral("Index encrypted rooms"));

        auto *search_ = m_controller->messageSearch();
        search_->indexAllRooms();
        QTRY_COMPARE(button->property("text").toString(), QStringLiteral("Pause"));
        search_->pauseIndexAll();
        QTRY_COMPARE(button->property("text").toString(), QStringLiteral("Resume"));
        auto *stop = item("indexAllCancelButton");
        QVERIFY(stop && stop->isVisible());
        search_->cancelIndexAll();
        QTRY_COMPARE(button->property("text").toString(),
                     QStringLiteral("Index encrypted rooms"));
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
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

    // The words people type for the settings Rokas could not find each land
    // on the control itself. Before 2026-10-07 "gradient" found only Depth
    // (the automatic gradient, not the way to make one), "microphone test"
    // found nothing (the query was matched as one phrase), "noise" led only
    // to Labs and "downloads" to a checkbox rather than the folder.
    void everyFindabilityQueryLandsOnItsControl()
    {
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        auto *screen = item("settingsScreenRoot");
        auto *search = item("settingsSearchField");
        auto *flick = item("settingsContentFlick");
        QVERIFY(screen && search && flick);
        auto *pane = flick->property("contentItem").value<QQuickItem *>();
        QVERIFY(pane);

        struct Case {
            const char *query;
            const char *anchor;
        };
        static const Case cases[] = {
            { "background", "chatBackgroundSettingsSection" },
            { "wallpaper", "chatBackgroundSettingsSection" },
            { "gradient", "customThemeGradientButton" },
            { "depth", "surfaceDepthControl" },
            { "noise", "soundNoiseSuppressionRow" },
            { "microphone test", "microphoneTestSection" },
            // Word order is the reader's, not the index's.
            { "test microphone", "microphoneTestSection" },
            { "downloads", "downloadFolderLabel" },
            { "recovery key", "recoveryInputField" },
        };
        QStringList missing;
        int checked = 0;
        for (const Case &c : cases) {
            search->setProperty("text", QString::fromLatin1(c.query));
            QCoreApplication::processEvents();
            const QVariantList hits =
                screen->property("matchedSearchResults").toList();
            bool found = false;
            for (const QVariant &hit : hits) {
                found = found
                    || hit.toMap().value(QStringLiteral("anchor")).toString()
                           == QLatin1String(c.anchor);
            }
            // The anchor must also be a real control in the content pane, or
            // the result would land nowhere.
            QQuickItem *target = findItem(pane, QLatin1String(c.anchor));
            if (!found || !target)
                missing.append(QStringLiteral("\"%1\" -> %2 (%3 results%4)")
                                   .arg(QLatin1String(c.query),
                                        QLatin1String(c.anchor))
                                   .arg(hits.size())
                                   .arg(target ? QString()
                                               : QStringLiteral(", no such "
                                                                "control")));
            ++checked;
        }
        search->setProperty("text", QString());
        QCoreApplication::processEvents();

        QCOMPARE(checked, int(sizeof(cases) / sizeof(cases[0])));
        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral("settings search does not lead to: %1")
                                .arg(missing.join(QStringLiteral("; ")))));
    }

    // Appearance is about twenty groups long; its contents chips take the
    // reader to each group from the top of the page, in page order.
    // (The page-contents chips became the sidebar outline; see
    // theOutlineReachesEveryGroupOfEachPageAndFollowsTheScroll.)

    // The chat background belongs with the theme, not a screen and a half
    // down below the layout pickers where nobody found it.
    void theChatBackgroundFollowsTheThemeAndPrecedesTheLayoutPickers()
    {
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        auto *custom = item("customThemeRow");
        auto *background = item("chatBackgroundSettingsSection");
        auto *layouts = item("roomNavigationLayoutCards");
        QVERIFY(custom && background && layouts);
        const qreal customTop = contentPosY(custom);
        const qreal backgroundTop = contentPosY(background);
        const qreal layoutsTop = contentPosY(layouts);
        QVERIFY2(backgroundTop > customTop && backgroundTop < layoutsTop,
                 qPrintable(QStringLiteral(
                     "custom theme at %1, chat background at %2, layout "
                     "pickers at %3")
                         .arg(customTop).arg(backgroundTop).arg(layoutsTop)));
    }

    // The Settings copy of the background editor is framed against the page
    // on every theme. It used the panel `border`, which on Moss Light is the
    // page's own colour: the preview had no edge and its button no outline.
    void theChatBackgroundPreviewIsFramedOnEveryTheme()
    {
        const int restore = m_controller->settings()->theme();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        auto *editor = item("chatBackgroundDefaultEditor");
        auto *ground = item("settingsPageGround");
        QVERIFY(editor && ground);
        QQuickItem *frame = findItem(editor, QStringLiteral("chatBackgroundPreview"));
        QVERIFY2(frame, "the Settings background editor has no preview frame");

        const int themes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        QStringList faint;
        double worst = 1000.0;
        int checked = 0;
        for (int id : themes) {
            m_controller->settings()->setTheme(
                static_cast<SettingsManager::Theme>(id));
            QCoreApplication::processEvents();
            const QColor edge = QQmlProperty(frame, QStringLiteral("border.color"))
                                    .read().value<QColor>();
            const QColor page = ground->property("color").value<QColor>();
            QVERIFY(edge.isValid() && page.isValid());
            const double sep = qAbs(lstarOf(edge) - lstarOf(page));
            worst = qMin(worst, sep);
            ++checked;
            // The editor's frame: borderStrong clears 15 dL* on every
            // palette; the panel border it replaced managed 0.8 on Moss
            // Light, 4.6 on Warm and 5.2 on Lightning Light.
            if (sep < 10.0)
                faint.append(QStringLiteral("theme %1: edge %2 on a page of %3, "
                                            "%4 dL*")
                                 .arg(id)
                                 .arg(edge.name(), page.name())
                                 .arg(sep, 0, 'f', 1));
        }
        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(restore));
        QCoreApplication::processEvents();

        QCOMPARE(checked, 11);
        qInfo("chat background preview edge vs page: worst %.1f dL*", worst);
        QVERIFY2(faint.isEmpty(), qPrintable(faint.join(QStringLiteral("\n  "))));
    }

    // "Add a gradient" opens the editor on a large surface with its Fill
    // choice ABOVE the colour picker; one click makes a gradient that can be
    // seen, and a gradient colour is edited with the picker itself.
    //
    // Fails on the old tree three ways: no customThemeGradientButton; the
    // gradient controls sat below the picker; and the seed was
    // Qt.darker(base, 1.12), which on Storm's #02051D is #02041A — the same
    // colour to the eye, so turning a gradient on visibly did nothing.
    void addAGradientOpensTheFillChoiceAndMakesAVisibleGradient()
    {
        const int w = m_window->width();
        const int h = m_window->height();
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        store->resetAll();
        store->resetGradient(QStringLiteral("background"));
        // A near-black ground: where a timid seed is invisible.
        store->setBaseTheme(11);
        QCoreApplication::processEvents();

        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        auto *button = item("customThemeGradientButton");
        auto *loader = item("themeEditorLoader");
        QVERIFY2(button, "Appearance offers no way to add a gradient");
        QVERIFY(loader);
        clickItem(button);
        QTRY_VERIFY(popup("themeEditorDialog"));
        QObject *dialog = popup("themeEditorDialog");
        QTRY_VERIFY(dialog->property("opened").toBool());

        QString role, seedA, seedB, stopAfterPick, flatAfterPick, caption;
        qreal fillY = -1.0, pickerFieldY = -1.0;
        double seedSep = -1.0;
        int stopWhileSelected = -2, stopAfterDeselect = -2;
        bool fillVisible = false;

        role = dialog->property("editingRole").toString();
        auto *fillHeading = item("gradientFillHeading");
        auto *picker = item("themeColorPicker");
        QQuickItem *hexField = picker
            ? findItem(picker, QStringLiteral("colorHexField")) : nullptr;
        if (fillHeading && hexField) {
            fillVisible = fillHeading->isVisible();
            fillY = fillHeading->mapToScene(QPointF(0, 0)).y();
            pickerFieldY = hexField->mapToScene(QPointF(0, 0)).y();
        }

        if (auto *linear = item("gradientLinearButton")) {
            clickItem(linear);
            const QVariantMap spec = store->gradients()
                .value(QStringLiteral("background")).toMap();
            const QStringList stops =
                spec.value(QStringLiteral("stops")).toStringList();
            if (stops.size() >= 2) {
                seedA = stops.first();
                seedB = stops.last();
                seedSep = qAbs(store->lightness(seedA) - store->lightness(seedB));
            }
        }

        // Pick the end colour, then drive the picker as a drag would.
        if (auto *swatch = item("gradientStopSwatch_1")) {
            clickItem(swatch);
            stopWhileSelected = dialog->property("editingStop").toInt();
            if (auto *capLabel = item("colorPickerBodyCaption"))
                caption = capLabel->property("text").toString();
            if (picker)
                QMetaObject::invokeMethod(
                    picker, "applyColor",
                    Q_ARG(QVariant, QVariant::fromValue(QColor(QStringLiteral("#FF0000")))));
            QCoreApplication::processEvents();
            const QStringList stops = store->gradients()
                .value(QStringLiteral("background")).toMap()
                .value(QStringLiteral("stops")).toStringList();
            stopAfterPick = stops.value(1);
            flatAfterPick = store->colors()
                .value(QStringLiteral("background")).toString();
            // Clicking it again hands the picker back to the solid colour.
            clickItem(swatch);
            stopAfterDeselect = dialog->property("editingStop").toInt();
        }

        // Restore before asserting.
        store->resetGradient(QStringLiteral("background"));
        store->resetAll();
        store->setBaseTheme(restoreBase);
        loader->setProperty("active", false);
        QCoreApplication::processEvents();
        m_window->setWidth(w);
        m_window->setHeight(h);
        QCoreApplication::processEvents();

        QCOMPARE(role, QStringLiteral("background"));
        QVERIFY2(fillVisible, "the Fill choice is not shown for the background");
        QVERIFY2(fillY >= 0.0 && fillY < pickerFieldY,
                 qPrintable(QStringLiteral("the Fill choice is at y=%1, below "
                                           "the picker's hex field at y=%2")
                                .arg(fillY).arg(pickerFieldY)));
        QVERIFY2(seedSep >= 5.0,
                 qPrintable(QStringLiteral("turning a gradient on made %1 -> %2, "
                                           "%3 dL* apart: nothing visible changes")
                                .arg(seedA, seedB).arg(seedSep, 0, 'f', 1)));
        QCOMPARE(stopWhileSelected, 1);
        QVERIFY2(!caption.isEmpty(),
                 "the picker does not say it is changing a gradient colour");
        QCOMPARE(stopAfterPick, QStringLiteral("#FF0000"));
        QVERIFY2(flatAfterPick.isEmpty(),
                 qPrintable(QStringLiteral("editing a gradient colour also "
                                           "recoloured the solid background "
                                           "(%1)").arg(flatAfterPick)));
        QCOMPARE(stopAfterDeselect, -1);
    }

    // ---- theme editor: gradients, the gradient bar, the preview --------------

    // Opens the theme editor on the custom theme and returns the dialog. The
    // caller restores the store and closes it with closeThemeEditor().
    QObject *openThemeEditor()
    {
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        if (!QTest::qWaitFor([this] { return item("themeEditorLoader") != nullptr; },
                             10000))
            return nullptr;
        auto *loader = item("themeEditorLoader");
        loader->setProperty("active", true);
        QTest::qWait(50);
        QObject *dialog = popup("themeEditorDialog");
        if (!dialog)
            return nullptr;
        if (!QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); },
                             5000))
            return nullptr;
        return dialog;
    }

    void closeThemeEditor()
    {
        if (auto *loader = item("themeEditorLoader"))
            loader->setProperty("active", false);
        QCoreApplication::processEvents();
    }

    void resetThemeFixture(int base)
    {
        auto *store = m_controller->customTheme();
        store->resetAll();
        for (const char *role : { "background", "sidebar", "rail", "surface",
                                  "settingsPage", "settingsNav", "timelineBackground",
                                  "roomList", "sidePanel" })
            store->resetGradient(QLatin1String(role));
        store->setBaseTheme(base);
        QCoreApplication::processEvents();
    }

    QVariantMap storedGradient(const char *role) const
    {
        return m_controller->customTheme()->gradients()
            .value(QLatin1String(role)).toMap();
    }

    static QVariantMap linearSpec(const QStringList &stops)
    {
        return { { QStringLiteral("type"), QStringLiteral("linear") },
                 { QStringLiteral("angle"), 180 },
                 { QStringLiteral("stops"), stops } };
    }

    static void collectRegions(QQuickItem *parent, QList<QQuickItem *> &out)
    {
        if (!parent)
            return;
        if (parent->property("isThemeRegion").toBool())
            out.append(parent);
        for (QQuickItem *child : parent->childItems())
            collectRegions(child, out);
    }

    QList<QQuickItem *> previewRegions() const
    {
        QList<QQuickItem *> out;
        collectRegions(item("themePreviewDemo"), out);
        return out;
    }

    // The first Region painting `role` that can be reached.
    QQuickItem *previewRegionFor(const QString &role) const
    {
        for (QQuickItem *r : previewRegions()) {
            if (r->property("role").toString() == role && r->isVisible()
                    && r->width() > 0 && r->height() > 0)
                return r;
        }
        return nullptr;
    }

    // Reported live as "when a gradient is set, editing the colours does not
    // work": the picker opened on the role's flat colour, which a gradient
    // paints over, so every edit was stored and none was seen. A role with a
    // gradient now opens the picker on a gradient colour, and so does a role
    // that has just been given one.
    //
    // Fails on the old tree: editingStop was -1 after beginEdit, the pick
    // landed in colours() and the gradient's stops did not move.
    void editingARoleThatHasAGradientRecoloursTheGradient()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QVERIFY(store->setGradient(
            QStringLiteral("background"),
            linearSpec({ QStringLiteral("#102030"), QStringLiteral("#203040") })));

        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(
            dialog, "beginEdit", Q_ARG(QVariant, QStringLiteral("background")),
            Q_ARG(QVariant, QStringLiteral("Conversation background"))));
        QCoreApplication::processEvents();
        const int stopOnOpen = dialog->property("editingStop").toInt();

        auto *picker = item("themeColorPicker");
        QVERIFY(picker);
        QMetaObject::invokeMethod(
            picker, "applyColor",
            Q_ARG(QVariant, QVariant::fromValue(QColor(QStringLiteral("#FF0000")))));
        QCoreApplication::processEvents();
        const QStringList stops =
            storedGradient("background").value(QStringLiteral("stops")).toStringList();
        const bool flatWritten = store->colors().contains(QStringLiteral("background"));
        // What the preview is handed to paint.
        const QVariantMap painted = dialog->property("previewGradients").toMap()
            .value(QStringLiteral("background")).toMap();
        const QString paintedFirst =
            painted.value(QStringLiteral("stops")).toStringList().value(0);

        // A role given its first gradient while the picker is open on it.
        QVERIFY(QMetaObject::invokeMethod(
            dialog, "beginEdit", Q_ARG(QVariant, QStringLiteral("sidebar")),
            Q_ARG(QVariant, QStringLiteral("Room list"))));
        QCoreApplication::processEvents();
        const int stopOnPlainRole = dialog->property("editingStop").toInt();
        if (auto *linear = item("gradientLinearButton"))
            clickItem(linear);
        const int stopAfterMakingOne = dialog->property("editingStop").toInt();
        QMetaObject::invokeMethod(
            picker, "applyColor",
            Q_ARG(QVariant, QVariant::fromValue(QColor(QStringLiteral("#00FF00")))));
        QCoreApplication::processEvents();
        const QString sidebarFirst = storedGradient("sidebar")
            .value(QStringLiteral("stops")).toStringList().value(0);
        const bool sidebarFlatWritten =
            store->colors().contains(QStringLiteral("sidebar"));

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QCOMPARE(stopOnOpen, 0);
        QCOMPARE(stops.value(0), QStringLiteral("#FF0000"));
        QCOMPARE(stops.value(1), QStringLiteral("#203040"));
        QVERIFY2(!flatWritten,
                 "the pick recoloured the solid colour hidden under the gradient");
        QCOMPARE(paintedFirst, QStringLiteral("#FF0000"));
        QCOMPARE(stopOnPlainRole, -1);
        QCOMPARE(stopAfterMakingOne, 0);
        QCOMPARE(sidebarFirst, QStringLiteral("#00FF00"));
        QVERIFY(!sidebarFlatWritten);
    }

    // The hex field of a gradient colour followed the stop until one bad entry
    // was typed: the handler assigned `text`, which destroys a binding, and
    // afterwards the field showed the typed text for ever (picker edits, a
    // preset, the bar).
    void aGradientColourFieldKeepsFollowingTheStopAfterABadEntry()
    {
        auto *store = m_controller->customTheme();
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QVERIFY(store->setGradient(
            QStringLiteral("background"),
            linearSpec({ QStringLiteral("#102030"), QStringLiteral("#203040") })));
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        QMetaObject::invokeMethod(
            dialog, "beginEdit", Q_ARG(QVariant, QStringLiteral("background")),
            Q_ARG(QVariant, QStringLiteral("Conversation background")));
        QCoreApplication::processEvents();

        const auto fields = items("gradientStopField");
        QString before, afterBad, afterPick;
        if (fields.size() >= 2) {
            QQuickItem *field = fields.at(0);
            before = field->property("text").toString();
            field->setProperty("text", QStringLiteral("zzzz"));
            QMetaObject::invokeMethod(field, "editingFinished");
            QCoreApplication::processEvents();
            afterBad = field->property("text").toString();
            QMetaObject::invokeMethod(
                item("themeColorPicker"), "applyColor",
                Q_ARG(QVariant, QVariant::fromValue(QColor(QStringLiteral("#00FF00")))));
            QCoreApplication::processEvents();
            afterPick = field->property("text").toString();
        }
        const int fieldCount = fields.size();

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QCOMPARE(fieldCount, 2);
        QCOMPARE(before, QStringLiteral("#102030"));
        QCOMPARE(afterBad, QStringLiteral("#102030"));
        QCOMPARE(afterPick, QStringLiteral("#00FF00"));
    }

    // The gradient bar: click to add, drag to move, arrow keys to nudge,
    // Delete to remove, never fewer than two or more than three stops.
    void theGradientBarAddsMovesAndRemovesStops()
    {
        auto *store = m_controller->customTheme();
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QVERIFY(store->setGradient(
            QStringLiteral("background"),
            linearSpec({ QStringLiteral("#102030"), QStringLiteral("#304050") })));
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        QMetaObject::invokeMethod(
            dialog, "beginEdit", Q_ARG(QVariant, QStringLiteral("background")),
            Q_ARG(QVariant, QStringLiteral("Conversation background")));
        QCoreApplication::processEvents();

        QQuickItem *bar = item("gradientBar");
        QQuickItem *track = item("gradientBarTrack");
        QVERIFY2(bar && track, "the gradient has no bar");
        ensureVisible(bar);
        const qreal handleW = bar->property("handleWidth").toReal();
        const qreal inner = bar->property("inner").toReal();
        auto sceneX = [&](qreal position) {
            return track->mapToScene(QPointF(handleW / 2 + position * inner,
                                             track->height() / 2));
        };

        // 1. Click the bar at 30%: a third stop, in the colour already there.
        const QPointF at30 = sceneX(0.3);
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, at30.toPoint());
        QCoreApplication::processEvents();
        QVariantMap g = storedGradient("background");
        const QStringList stopsAfterAdd = g.value(QStringLiteral("stops")).toStringList();
        const QVariantList posAfterAdd = g.value(QStringLiteral("positions")).toList();
        const int selectedAfterAdd = dialog->property("editingStop").toInt();

        // 2. Drag that handle right by 35% of the bar (30% -> 65%).
        QQuickItem *handle = item("gradientBarHandle_1");
        QVERIFY(handle);
        const QPointF start = handle->mapToScene(
            QPointF(handle->width() / 2, handle->height() / 2));
        QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        const qreal dx = 0.35 * inner;
        for (int step = 1; step <= 5; ++step)
            QTest::mouseMove(m_window, (start + QPointF(dx * step / 5, 0)).toPoint());
        QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier,
                            (start + QPointF(dx, 0)).toPoint());
        QCoreApplication::processEvents();
        g = storedGradient("background");
        const double afterDrag = g.value(QStringLiteral("positions")).toList()
                                     .value(1).toDouble();

        // 3. Keyboard: Right nudges 2%, Shift+Right 10%, End stops at the
        // neighbour and cannot pass it.
        bar->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_Right);
        const double afterRight = storedGradient("background")
            .value(QStringLiteral("positions")).toList().value(1).toDouble();
        QTest::keyClick(m_window, Qt::Key_Right, Qt::ShiftModifier);
        const double afterShift = storedGradient("background")
            .value(QStringLiteral("positions")).toList().value(1).toDouble();
        QTest::keyClick(m_window, Qt::Key_End);
        const double afterEnd = storedGradient("background")
            .value(QStringLiteral("positions")).toList().value(1).toDouble();
        QTest::keyClick(m_window, Qt::Key_Home);
        const double afterHome = storedGradient("background")
            .value(QStringLiteral("positions")).toList().value(1).toDouble();
        const QVariantMap beforeLast = storedGradient("background");

        // 4. A third stop is the most there is: clicking a full bar selects
        // the nearest handle and adds nothing.
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          sceneX(0.9).toPoint());
        QCoreApplication::processEvents();
        const int stopCountFull =
            storedGradient("background").value(QStringLiteral("stops")).toList().size();

        // 5. Delete removes the selected stop; at two it does nothing.
        bar->forceActiveFocus();
        QTest::keyClick(m_window, Qt::Key_Up); // from the last stop to the middle one
        QTest::keyClick(m_window, Qt::Key_Delete);
        const QVariantMap afterDelete = storedGradient("background");
        QTest::keyClick(m_window, Qt::Key_Delete);
        const QVariantMap afterSecondDelete = storedGradient("background");

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QCOMPARE(stopsAfterAdd.size(), 3);
        QCOMPARE(posAfterAdd.size(), 3);
        QVERIFY(qAbs(posAfterAdd.at(1).toDouble() - 0.3) < 0.02);
        QCOMPARE(selectedAfterAdd, 1);
        // Adding changed nothing visible: the new stop is the blend there.
        QVERIFY2(m_controller->customTheme()->lightness(stopsAfterAdd.at(1)) > 0,
                 "the new stop has no colour");
        QVERIFY2(qAbs(afterDrag - 0.65) < 0.04,
                 qPrintable(QStringLiteral("dragging 35%% moved the stop to %1")
                                .arg(afterDrag)));
        QVERIFY(qAbs(afterRight - (afterDrag + 0.02)) < 0.006);
        QVERIFY(qAbs(afterShift - (afterRight + 0.10)) < 0.006);
        QCOMPARE(afterEnd, 1.0);
        QCOMPARE(afterHome, 0.0);
        QCOMPARE(stopCountFull, 3);
        QCOMPARE(afterDelete.value(QStringLiteral("stops")).toList().size(), 2);
        QVERIFY2(!afterDelete.contains(QStringLiteral("positions")),
                 "two evenly spaced stops must be stored as the old format");
        QCOMPARE(afterSecondDelete, afterDelete);
    }

    // Every themed element of the sample window is a click target of its own:
    // clicking the centre of each Region reports exactly that Region's role
    // (so none is covered by a sibling that names another), and hovering it
    // outlines it. Every editable role has a Region, and every ink that
    // shares a box with a fill (a badge's count, the label on a button, a
    // chip's text, a border) has one of its own.
    void everyElementOfThePreviewIsItsOwnClickTarget()
    {
        auto *store = m_controller->customTheme();
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        // Channels draws rows the default layout does not; the test covers
        // the layout the user has.
        QQuickItem *preview = item("themePreviewDemo");
        QVERIFY(preview);

        const QList<QQuickItem *> regions = previewRegions();
        QSet<QString> roles;
        for (QQuickItem *r : regions) {
            if (r->isVisible())
                roles.insert(r->property("role").toString());
        }
        // A role is in the preview through a Region of its own, or (a group
        // whose every element has its own role) through its elements.
        QHash<QString, QStringList> children;
        for (const QVariant &r : store->roles()) {
            const QVariantMap m = r.toMap();
            const QString parent = m.value(QStringLiteral("follows")).toString();
            if (!parent.isEmpty())
                children[parent] << m.value(QStringLiteral("key")).toString();
        }
        std::function<bool(const QString &, int)> shown =
            [&](const QString &key, int depth) {
            if (roles.contains(key))
                return true;
            for (const QString &child : children.value(key)) {
                if (depth < 6 && shown(child, depth + 1))
                    return true;
            }
            return false;
        };
        QStringList missing;
        for (const QVariant &r : store->roles()) {
            const QString key = r.toMap().value(QStringLiteral("key")).toString();
            // Room rows are Channels-only or room-list-only, and the preview
            // follows the user's layout; both are covered by one pass each.
            if (!shown(key, 0))
                missing.append(key);
        }

        // One element, one role: a role painted by several visible Regions
        // must be one element drawn several times (`repeated`), never two
        // different elements that would light up together. Before the split
        // the room header and the people panel were both "surface".
        QHash<QString, QList<QQuickItem *>> byRole;
        for (QQuickItem *r : regions) {
            if (r->isVisible() && r->width() > 0 && r->height() > 0)
                byRole[r->property("role").toString()].append(r);
        }
        QStringList shared;
        for (auto it = byRole.constBegin(); it != byRole.constEnd(); ++it) {
            if (it.value().size() < 2)
                continue;
            for (QQuickItem *r : it.value()) {
                if (!r->property("repeated").toBool()) {
                    shared.append(QStringLiteral("%1 (%2 targets)")
                                      .arg(it.key()).arg(it.value().size()));
                    break;
                }
            }
        }
        // Every target names a leaf: nothing currently follows it, so editing
        // it recolours that element alone.
        QStringList notLeaf;
        for (auto it = byRole.constBegin(); it != byRole.constEnd(); ++it) {
            QVariant deps;
            QMetaObject::invokeMethod(dialog, "dependentsOf",
                                      Q_RETURN_ARG(QVariant, deps),
                                      Q_ARG(QVariant, it.key()));
            if (!deps.toList().isEmpty())
                notLeaf.append(it.key() + QStringLiteral(" -> ")
                               + deps.toStringList().join(QStringLiteral("/")));
        }

        // Which Region a click at `scene` reaches: the topmost one, children
        // before their parent and later siblings before earlier ones (the
        // order Qt delivers a press in), z honoured.
        std::function<QQuickItem *(QQuickItem *, const QPointF &)> topmost =
            [&](QQuickItem *parent, const QPointF &scene) -> QQuickItem * {
            QList<QQuickItem *> kids = parent->childItems();
            std::stable_sort(kids.begin(), kids.end(),
                             [](QQuickItem *a, QQuickItem *b) { return a->z() < b->z(); });
            for (int i = kids.size() - 1; i >= 0; --i) {
                QQuickItem *k = kids.at(i);
                if (!k->isVisible() || !k->isEnabled() || k->opacity() <= 0.0)
                    continue;
                if (QQuickItem *hit = topmost(k, scene))
                    return hit;
            }
            if (parent->property("isThemeRegion").toBool()
                    && parent->contains(parent->mapFromScene(scene)))
                return parent;
            return nullptr;
        };

        // 1. Every Region can be reached: some point of it is not covered by
        // another Region (a text sitting on a fill covers the middle of the
        // fill and nothing else; a Region wholly under another is dead).
        // 2. Hovering and clicking that point reports and outlines exactly
        // this Region's role.
        QSignalSpy spy(preview, SIGNAL(regionActivated(QString, QVariant)));
        QStringList unreachable;
        QStringList wrong;
        QStringList notLit;
        QSet<QString> alsoLit;
        int clicked = 0;
        const QRectF stage(0, 0, preview->width(), preview->height());
        for (QQuickItem *r : regions) {
            if (!r->isVisible() || r->width() <= 0 || r->height() <= 0)
                continue;
            const QString role = r->property("role").toString();
            // The reachable sample nearest the middle: that is where a user
            // points, and a point on the very rim of a padded target is not
            // delivered to it by Qt (outside its parent's bounds).
            QPointF found;
            bool reachable = false;
            qreal best = 1e18;
            for (int iy = 0; iy < 9; ++iy) {
                for (int ix = 0; ix < 9; ++ix) {
                    const QPointF local((ix + 0.5) * r->width() / 9,
                                        (iy + 0.5) * r->height() / 9);
                    // Whole pixels: that is all a pointer event carries.
                    const QPointF scene(r->mapToScene(local).toPoint());
                    if (!stage.contains(preview->mapFromScene(scene)))
                        continue;
                    if (topmost(m_window->contentItem(), scene) != r)
                        continue;
                    const QPointF d = local - QPointF(r->width() / 2, r->height() / 2);
                    const qreal dist = d.x() * d.x() + d.y() * d.y();
                    if (dist < best) {
                        best = dist;
                        reachable = true;
                        found = scene;
                    }
                }
            }
            if (!reachable) {
                unreachable.append(role);
                continue;
            }
            QTest::mouseMove(m_window, found.toPoint() + QPoint(2, 1));
            QTest::mouseMove(m_window, found.toPoint());
            QCoreApplication::processEvents();
            const qreal lit = r->property("emphasis").toReal();
            spy.clear();
            QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, found.toPoint());
            QCoreApplication::processEvents();
            ++clicked;
            if (spy.isEmpty()) {
                wrong.append(role + QStringLiteral(" (no signal)"));
                continue;
            }
            const QString got = spy.last().at(0).toString();
            if (got != role)
                wrong.append(role + QStringLiteral(" -> ") + got);
            else if (lit < 0.5)
                notLit.append(role);
            if (dialog->property("editingRole").toString() != got)
                wrong.append(role + QStringLiteral(" did not open its editor"));
            // What the click outlined: this element (and its repeats) only.
            // Moved off the preview first, so the pointer's own 0.5 is gone.
            QTest::mouseMove(m_window, QPoint(1, 1));
            QCoreApplication::processEvents();
            for (QQuickItem *other : regions) {
                if (!other->isVisible()
                        || other->property("emphasis").toReal() < 1.0)
                    continue;
                if (other->property("role").toString() != got)
                    alsoLit.insert(got + QStringLiteral(" also outlines ")
                                   + other->property("role").toString());
            }
        }

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral("editable roles with no place in the "
                                           "preview: %1").arg(missing.join(", "))));
        QVERIFY2(clicked >= 90,
                 qPrintable(QStringLiteral("only %1 regions could be clicked").arg(clicked)));
        // The element roles themselves: far more distinct targets than the
        // 2026-10-06 preview's ~50 shared roles.
        QVERIFY2(byRole.size() >= 90,
                 qPrintable(QStringLiteral("only %1 distinct roles have a target")
                                .arg(byRole.size())));
        QVERIFY2(unreachable.isEmpty(),
                 qPrintable(QStringLiteral("covered by another target: %1")
                                .arg(unreachable.join(", "))));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QStringLiteral("\n  "))));
        QVERIFY2(notLit.isEmpty(),
                 qPrintable(QStringLiteral("hover did not outline: %1")
                                .arg(notLit.join(", "))));
        QVERIFY2(shared.isEmpty(),
                 qPrintable(QStringLiteral("two different elements share a role: %1")
                                .arg(shared.join(", "))));
        QVERIFY2(notLeaf.isEmpty(),
                 qPrintable(QStringLiteral("a target recolours other elements too: %1")
                                .arg(notLeaf.join(", "))));
        QVERIFY2(alsoLit.isEmpty(),
                 qPrintable(QStringLiteral("a click outlined more than its element:\n  %1")
                                .arg(QStringList(alsoLit.values()).join(QStringLiteral("\n  ")))));
    }

    // With no custom theme yet, ONE click on "Create a theme" opens the
    // editor and it stays open. Reported live as needing a second click; NOT
    // reproduced by this case, which passes on the unfixed tree too.
    // The whole resolved palette of theme `id`, read in one evaluation:
    // key -> colour.
    QHash<QString, QColor> resolvedPaletteOf(int id) const
    {
        QQmlExpression expr(
            qmlContext(m_window), m_window,
            QStringLiteral("(function () { var p = AppTheme.paletteForTheme(%1);"
                           " var out = []; for (var k in p)"
                           " out.push(k + '=' + String(p[k])); return out.join(';') })()")
                .arg(id));
        const QString flat = expr.evaluate().toString();
        QHash<QString, QColor> out;
        for (const QString &pair : flat.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const int eq = pair.indexOf(QLatin1Char('='));
            if (eq > 0)
                out.insert(pair.left(eq), QColor::fromString(pair.mid(eq + 1)));
        }
        return out;
    }

    bool baseDefines(int base, const QString &key) const
    {
        QQmlExpression expr(qmlContext(m_window), m_window,
                            QStringLiteral("AppTheme.rawPaletteForTheme(%1)['%2'] !== undefined")
                                .arg(base).arg(key));
        return expr.evaluate().toBool();
    }

    // Every element role resolves, by default, to the group it follows: in
    // the palette the editor and the preview paint (paletteForTheme(12)) and
    // in the live token the app paints with, on a custom theme over each of
    // the eleven presets. So splitting a shared role changed no theme, and
    // the editor's "follows <group>" is the truth. Counted, so a short sweep
    // fails. (A role the base preset sets itself, such as `rail`, is its own
    // colour on that base and is skipped, as the editor's links skip it.)
    //
    // Fails on the old tree: it has no element roles, so the count is far
    // below the floor.
    void everyElementRoleFollowsItsGroupOnEveryBase()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        const int restoreTheme = int(m_controller->settings()->theme());
        QObject *backdrops = m_controller->backdrops();
        const QVariant restoreDepth = backdrops ? backdrops->property("surfaceDepth")
                                                : QVariant();
        if (backdrops)
            backdrops->setProperty("surfaceDepth", 0);

        // role -> group, as palette keys.
        QList<QPair<QString, QString>> pairs;
        for (const QVariant &v : store->roles()) {
            const QVariantMap m = v.toMap();
            const QString parent = m.value(QStringLiteral("follows")).toString();
            if (parent.isEmpty())
                continue;
            pairs.append({ store->paletteKeyForRole(m.value(QStringLiteral("key")).toString()),
                           store->paletteKeyForRole(parent) });
        }
        QVERIFY2(pairs.size() >= 80,
                 qPrintable(QStringLiteral("only %1 roles follow a group").arg(pairs.size())));

        QStringList wrong;
        int compared = 0;
        int bases = 0;
        for (int base = 1; base <= 11; ++base) {
            resetThemeFixture(base);
            m_controller->settings()->setTheme(SettingsManager::CustomTheme);
            QCoreApplication::processEvents();
            ++bases;
            const QHash<QString, QColor> pal = resolvedPaletteOf(12);
            for (const auto &[element, group] : pairs) {
                if (baseDefines(base, element))
                    continue;
                // Four presets set the online dot under its older key.
                if (element == QLatin1String("presenceOnline")
                        && baseDefines(base, QStringLiteral("online")))
                    continue;
                // The room list's mention badge follows the mention colour
                // THROUGH the badge contrast fit (mentionBadgeFill, badges
                // 2026-10-20): equal wherever the fit leaves it alone.
                const QString through =
                    element == QLatin1String("roomMentionBadge")
                            && group == QLatin1String("mentionBadge")
                        ? QStringLiteral("mentionBadgeFill") : group;
                const QColor e = pal.value(element);
                const QColor g = pal.value(through);
                if (!e.isValid() || e != g)
                    wrong << QStringLiteral("base %1: palette %2 %3 != %4 %5")
                                 .arg(base).arg(element, e.name(QColor::HexArgb),
                                                group, g.name(QColor::HexArgb));
                const QColor le = themeColor(element.toUtf8().constData());
                const QColor lg = themeColor(through.toUtf8().constData());
                if (!le.isValid() || le != lg)
                    wrong << QStringLiteral("base %1: live %2 %3 != %4 %5")
                                 .arg(base).arg(element, le.name(QColor::HexArgb),
                                                group, lg.name(QColor::HexArgb));
                compared += 2;
            }
        }

        m_controller->settings()->setTheme(SettingsManager::Theme(restoreTheme));
        resetThemeFixture(restoreBase);
        if (backdrops)
            backdrops->setProperty("surfaceDepth", restoreDepth);
        QCoreApplication::processEvents();

        QCOMPARE(bases, 11);
        // ~80 elements, both sides, eleven bases, less the few a base sets.
        QVERIFY2(compared >= 11 * 2 * 75,
                 qPrintable(QStringLiteral("only %1 comparisons were made").arg(compared)));
        QVERIFY2(wrong.isEmpty(),
                 qPrintable(QStringLiteral("%1 of %2 comparisons differ:\n  %3")
                                .arg(wrong.size()).arg(compared)
                                .arg(wrong.mid(0, 40).join(QStringLiteral("\n  ")))));
    }

    // ---- theme editor UX (2026-10-13): levels, links, readability ----------

    // Effective visibility: an item is on screen only if it and every ancestor
    // are visible (QQuickItem::isVisible is already the effective value).
    static bool shown(const QQuickItem *it)
    {
        return it && it->isVisible() && it->width() > 0 && it->height() > 0;
    }

    // The editor opens at Simple on a theme with nothing changed, and each
    // level reveals more of the same editor: Simple shows presets and four
    // base colours, Groups the colours others follow, Elements every part.
    // Fails on the old tree: there was one flat list and no level.
    void theEditorStartsSimpleAndEachLevelRevealsMore()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);

        const int startLevel = dialog->property("level").toInt();
        const bool simpleShown = shown(item("themeSimplePanel"));
        const bool listAtSimple = shown(item("themeRole_sidebar"));
        const bool baseCards = shown(item("themeSimpleBases"));
        const bool switchShown = shown(item("themeLevelSwitch"));

        QMetaObject::invokeMethod(dialog, "setLevel", Q_ARG(QVariant, 1));
        QCoreApplication::processEvents();
        const bool simpleAtGroups = shown(item("themeSimplePanel"));
        const bool groupAtGroups = shown(item("themeRole_sidebar"));
        const bool elementAtGroups = shown(item("themeRole_roomList"));
        // A group says how much it carries.
        const bool sizeShown = shown(item("themeRoleMembers_sidebar"));

        QMetaObject::invokeMethod(dialog, "setLevel", Q_ARG(QVariant, 2));
        QCoreApplication::processEvents();
        const bool elementAtElements = shown(item("themeRole_roomList"));

        // A search looks at everything, whatever the level.
        QMetaObject::invokeMethod(dialog, "setLevel", Q_ARG(QVariant, 1));
        dialog->setProperty("roleFilter", QStringLiteral("room list"));
        QCoreApplication::processEvents();
        const bool searchFindsElement = shown(item("themeRole_roomList"));
        dialog->setProperty("roleFilter", QString());

        // A theme that has been worked on does not start at Simple.
        closeThemeEditor();
        store->setColor(QStringLiteral("sidebar"), QStringLiteral("#123456"));
        dialog = openThemeEditor();
        QVERIFY(dialog);
        const int workedLevel = dialog->property("level").toInt();

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QCOMPARE(startLevel, 0);
        QVERIFY2(switchShown, "the Simple / Groups / Elements switch is missing");
        QVERIFY2(simpleShown, "Simple does not show its panel");
        QVERIFY2(!listAtSimple, "Simple still shows the full role list");
        QVERIFY2(baseCards, "Simple offers no presets to start from");
        QVERIFY2(!simpleAtGroups, "the Simple panel stays behind at Groups");
        QVERIFY2(groupAtGroups, "Groups does not list the groups");
        QVERIFY2(!elementAtGroups, "Groups lists the elements as well");
        QVERIFY2(sizeShown, "a group does not say how many elements it carries");
        QVERIFY2(elementAtElements, "Elements does not list every part");
        QVERIFY2(searchFindsElement, "a search cannot reach an element at Groups");
        QCOMPARE(workedLevel, 2);
    }

    // Unlinking keeps what is on screen and ends the following; its toast
    // undoes it. Fails on the old tree: there was no Unlink.
    void unlinkingKeepsTheColourAndStopsFollowing()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        const QString element = QStringLiteral("roomHeader");
        const QString group = QStringLiteral("sidebar");
        const QString key = store->paletteKeyForRole(element);
        QVERIFY(QMetaObject::invokeMethod(
            dialog, "beginEdit", Q_ARG(QVariant, element), Q_ARG(QVariant, element)));
        QCoreApplication::processEvents();

        const QString parentBefore = dialog->property("editingParent").toString();
        const QColor shownBefore = resolvedPaletteOf(12).value(key);
        const bool canUnlink = dialog->property("editingCanUnlink").toBool();
        QVERIFY(QMetaObject::invokeMethod(dialog, "unlinkEditing"));
        QCoreApplication::processEvents();

        const QColor shownAfter = resolvedPaletteOf(12).value(key);
        const bool stored = store->colors().contains(element);
        const bool toastShown = shown(item("themeToast"));

        // The toast's Undo puts the link back (it takes back the LATEST step,
        // so it is pressed before anything else is edited), and Redo unlinks
        // again.
        if (auto *undo = item("themeToastAction"))
            clickItem(undo);
        QCoreApplication::processEvents();
        const bool storedAfterUndo = store->colors().contains(element);
        const QString parentAfterUndo = dialog->property("editingParent").toString();
        QVERIFY(QMetaObject::invokeMethod(dialog, "redoEdit"));
        QCoreApplication::processEvents();

        // The group moves; the unlinked element stays.
        QVERIFY(store->setColor(group, QStringLiteral("#123456")));
        QCoreApplication::processEvents();
        const QColor elementAfterGroup = resolvedPaletteOf(12).value(key);
        const QColor siblingAfterGroup =
            resolvedPaletteOf(12).value(store->paletteKeyForRole(QStringLiteral("roomList")));
        const QString parentAfter = dialog->property("editingParent").toString();

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QCOMPARE(parentBefore, group);
        QVERIFY(canUnlink);
        QVERIFY2(stored, "unlinking stored no colour of its own");
        QCOMPARE(shownAfter, shownBefore);
        QVERIFY2(toastShown, "unlinking said nothing and offered no Undo");
        QCOMPARE(elementAfterGroup, shownBefore);
        QCOMPARE(siblingAfterGroup, QColor(QStringLiteral("#123456")));
        QVERIFY2(parentAfter.isEmpty(), "an unlinked element still reports a parent");
        QVERIFY2(!storedAfterUndo, "Undo did not restore the link");
        QCOMPARE(parentAfterUndo, group);
    }

    // Master prompt Part 6, 4 (UI-001/003/005/016): "Select elements to link"
    // turns row and preview clicks into a selection, two or more are linked
    // under a name with the first one's colour, the rows say they are
    // linked, the picker says its edit changes the whole group, and "Edit
    // this element only" takes one out. The editor had no such flow.
    void theEditorLinksSelectedElementsIntoAColourGroup()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        auto cleanup = qScopeGuard([&] {
            closeThemeEditor();
            resetThemeFixture(restoreBase);
        });

        auto *select = item("themeSelectRolesButton");
        QVERIFY(select);
        clickItem(select);
        QTRY_VERIFY(dialog->property("selectingRoles").toBool());
        // A preview click selects instead of opening the picker.
        QVERIFY(QMetaObject::invokeMethod(dialog, "openFromPreview",
                                          Q_ARG(QVariant, QStringLiteral("accent")),
                                          Q_ARG(QVariant, QVariantList())));
        QCOMPARE(dialog->property("editingRole").toString(), QString());
        auto *linkRow = item("themeRole_link");
        QVERIFY(linkRow);
        clickItem(linkRow);
        QCOMPARE(dialog->property("selectedRoles").toStringList(),
                 (QStringList{ QStringLiteral("accent"), QStringLiteral("link") }));
        auto *mark = item("themeRoleSelect_link");
        QVERIFY(mark && mark->isVisible());
        const QString startColour = dialog->property("selectionStartColour").toString();
        QVERIFY(!startColour.isEmpty());

        auto *name = item("themeGroupNameField");
        auto *create = item("themeGroupCreateButton");
        QVERIFY(name && create);
        name->setProperty("text", QStringLiteral("Links"));
        QVERIFY(create->isEnabled());
        clickItem(create);
        QTRY_COMPARE(store->colorGroups().size(), 1);
        QVERIFY(!dialog->property("selectingRoles").toBool());
        const QVariantMap group = store->colorGroups().first().toMap();
        QCOMPARE(group.value(QStringLiteral("name")).toString(), QStringLiteral("Links"));
        QCOMPARE(group.value(QStringLiteral("color")).toString(), startColour.toUpper());
        // Leaving selection mode returns to the editor's opening level
        // (Simple), where the element list is not shown; open Elements.
        QVERIFY(QMetaObject::invokeMethod(dialog, "setLevel", Q_ARG(QVariant, 2)));
        QCoreApplication::processEvents();
        auto *badge = item("themeRoleGroup_accent");
        QVERIFY(badge);
        QTRY_VERIFY(badge->property("visible").toBool());
        QVERIFY(badge->property("text").toString().contains(QStringLiteral("Links")));

        // The picker on a member states the group scope and edits the group.
        QVERIFY(QMetaObject::invokeMethod(dialog, "beginEdit",
                                          Q_ARG(QVariant, QStringLiteral("link")),
                                          Q_ARG(QVariant, QStringLiteral("link"))));
        QCoreApplication::processEvents();
        auto *scope = item("themeEditingGroupScope");
        QVERIFY(scope);
        QTRY_VERIFY(scope->isVisible());
        QVERIFY(store->setColor(QStringLiteral("link"), QStringLiteral("#13579B")));
        QCOMPARE(store->colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#13579B"));
        auto *detach = item("themeDetachFromGroupButton");
        QVERIFY(detach);
        clickItem(detach);
        QTRY_VERIFY(store->colorGroups().isEmpty());
        QCOMPARE(store->colors().value(QStringLiteral("link")).toString(),
                 QStringLiteral("#13579B"));
        QTRY_VERIFY(!scope->isVisible());
    }

    // Opening a group shows how many of its elements keep their own colour,
    // and "Include them" gives them the group's. Fails on the old tree.
    void aGroupCanIncludeTheElementsThatKeepTheirOwnColour()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QVERIFY(store->setColor(QStringLiteral("roomHeader"),
                                QStringLiteral("#AA3355")));
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(
            dialog, "beginEdit", Q_ARG(QVariant, QStringLiteral("sidebar")),
            Q_ARG(QVariant, QStringLiteral("Side columns"))));
        QCoreApplication::processEvents();
        const int keepBefore = dialog->property("editingKeepOwn").toInt();
        const bool keepRowShown = shown(item("themeRoleKeepOwn"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "includeOwnMembers"));
        QCoreApplication::processEvents();
        const bool headerStillOwn = store->colors().contains(QStringLiteral("roomHeader"));
        const int keepAfter = dialog->property("editingKeepOwn").toInt();

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QCOMPARE(keepBefore, 1);
        QVERIFY2(keepRowShown, "the group does not say that an element keeps its own colour");
        QVERIFY2(!headerStillOwn, "Include them left the element's own colour");
        QCOMPARE(keepAfter, 0);
    }

    // "Make readable" moves the open colour to one that passes, and the
    // preview dots the roles that fail. Fails on the old tree: neither
    // existed.
    void makeReadableClearsWhatFailedAndThePreviewDotsIt()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        // Body text almost the colour of the ground it sits on.
        QVERIFY(store->setColor(QStringLiteral("background"), QStringLiteral("#202020")));
        QVERIFY(store->setColor(QStringLiteral("textPrimary"), QStringLiteral("#262626")));
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(
            dialog, "beginEdit", Q_ARG(QVariant, QStringLiteral("textPrimary")),
            Q_ARG(QVariant, QStringLiteral("Text"))));

        // The audit is throttled; wait for it to see the failure.
        QTRY_VERIFY(dialog->property("problemRoleSet").toMap()
                        .contains(QStringLiteral("textPrimary")));
        int dots = 0;
        for (QQuickItem *dot : items("themeProblemDot")) {
            if (shown(dot))
                ++dots;
        }
        const bool buttonShown = shown(item("themeMakeReadable"));
        const auto failing = [&]() {
            int n = 0;
            const QVariantList rows = store->auditForRole(
                dialog->property("previewPalette").toMap(),
                QStringLiteral("textPrimary"));
            for (const QVariant &v : rows) {
                if (!v.toMap().value(QStringLiteral("passes")).toBool())
                    ++n;
            }
            return n;
        };
        const int failingBefore = failing();
        const double lightBefore = store->lightness(QStringLiteral("#262626"));

        // makeReadable(role) takes its role optionally; an invoke must pass
        // the argument, and an empty one means the open role.
        QVERIFY(QMetaObject::invokeMethod(dialog, "makeReadable",
                                          Q_ARG(QVariant, QString())));
        QCoreApplication::processEvents();
        const int failingAfter = failing();
        const QString now = store->colors().value(QStringLiteral("textPrimary")).toString();

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QVERIFY2(dots > 0, "the preview does not dot a role that is hard to read");
        QVERIFY2(buttonShown, "no Make readable beside a failing pair");
        QVERIFY(failingBefore > 0);
        QVERIFY2(failingAfter < failingBefore,
                 qPrintable(QStringLiteral("Make readable left %1 of %2 failing")
                                .arg(failingAfter).arg(failingBefore)));
        QVERIFY(store->lightness(now) > lightBefore);
    }

    // Searching "changed" lists what this theme sets itself, and "unreadable"
    // what the audit flags. Fails on the old tree: both words matched nothing.
    void searchKnowsChangedAndUnreadable()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QVERIFY(store->setColor(QStringLiteral("sidebar"), QStringLiteral("#123456")));
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);

        const auto listed = [&](const QString &word, const QString &role) {
            dialog->setProperty("roleFilter", word);
            QCoreApplication::processEvents();
            const QVariantList groups = dialog->property("roleGroups").toList();
            for (const QVariant &g : groups) {
                for (const QVariant &it : g.toMap().value(QStringLiteral("items")).toList()) {
                    if (it.toMap().value(QStringLiteral("key")).toString() == role)
                        return true;
                }
            }
            return false;
        };
        const bool changedFinds = listed(QStringLiteral("changed"), QStringLiteral("sidebar"));
        const bool changedSkips = !listed(QStringLiteral("changed"), QStringLiteral("rail"));

        QVERIFY(store->setColor(QStringLiteral("background"), QStringLiteral("#202020")));
        QVERIFY(store->setColor(QStringLiteral("textPrimary"), QStringLiteral("#262626")));
        QTRY_VERIFY(dialog->property("problemRoleSet").toMap()
                        .contains(QStringLiteral("textPrimary")));
        const bool hardFinds = listed(QStringLiteral("unreadable"), QStringLiteral("textPrimary"));
        dialog->setProperty("roleFilter", QString());

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QVERIFY2(changedFinds, "\"changed\" does not list a colour this theme sets");
        QVERIFY2(changedSkips, "\"changed\" lists a colour the theme leaves alone");
        QVERIFY2(hardFinds, "\"unreadable\" does not list a flagged role");
    }

    // ---- theme editor UX, second pass (2026-10-14) --------------------------

    // Choosing a preset while the theme has edits asks Keep / Replace / Cancel;
    // with nothing changed it just applies. Keep leaves the edits on the new
    // base, Replace drops them as one undoable step, Cancel changes nothing.
    // Fails on the old tree: the preset applied at once and nothing asked.
    void choosingAPresetWithEditsAsksKeepOrReplace()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        const int other = restoreBase == 10 ? 9 : 10;

        // Nothing of the user's own: no question.
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestBase", Q_ARG(QVariant, other)));
        QCoreApplication::processEvents();
        const bool appliedAtOnce = store->baseTheme() == other
                                   && dialog->property("pendingBase").toInt() < 0;

        // With an edit it asks, and changes nothing yet.
        QVERIFY(store->setColor(QStringLiteral("accent"), QStringLiteral("#FF3366")));
        store->sealUndoStep();
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestBase", Q_ARG(QVariant, 11)));
        QCoreApplication::processEvents();
        const bool asked = shown(item("themePresetPrompt"))
                           && dialog->property("pendingBase").toInt() == 11;
        const bool unchangedWhileAsking = store->baseTheme() == other;

        // Escape peels the question before anything else.
        QVERIFY(dialog->property("hasTransientState").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "dismissTransient"));
        QCoreApplication::processEvents();
        const bool cancelled = dialog->property("pendingBase").toInt() < 0
                               && store->baseTheme() == other
                               && !shown(item("themePresetPrompt"));

        // Keep my changes: the base moves, the edit stays.
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestBase", Q_ARG(QVariant, 11)));
        QCoreApplication::processEvents();
        clickItem(item("themePresetKeepButton"));
        const bool kept = store->baseTheme() == 11
                          && store->colors().contains(QStringLiteral("accent"));

        // Replace: the edit goes, and one Undo brings back both halves.
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestBase", Q_ARG(QVariant, other)));
        QCoreApplication::processEvents();
        clickItem(item("themePresetReplaceButton"));
        const bool replaced = store->baseTheme() == other
                              && !store->colors().contains(QStringLiteral("accent"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "undoEdit"));
        const bool undoneAsOne = store->baseTheme() == 11
                                 && store->colors().contains(QStringLiteral("accent"));

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QVERIFY2(appliedAtOnce, "a preset on an untouched theme asked a question");
        QVERIFY2(asked, "a preset on an edited theme did not ask");
        QVERIFY2(unchangedWhileAsking, "the preset applied before it was answered");
        QVERIFY2(cancelled, "Cancel / Escape did not leave the theme alone");
        QVERIFY2(kept, "Keep my changes lost the edit or did not move the base");
        QVERIFY2(replaced, "Replace kept the edit or did not move the base");
        QVERIFY2(undoneAsOne, "Replace is not one undoable step");
    }


    // The readability report is a popover under the header's pill, not a
    // permanent column: closed it takes no room, open it lists every finding,
    // a finding's Fix moves that colour, a finding's row opens the element and
    // closes the popover, and Escape closes it before anything else.
    // Fails on the old tree: the report filled the right-hand column whenever
    // nothing was open and there was no popover to click open.
    void theReadabilityReportIsAPopoverUnderThePill()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QVERIFY(store->setColor(QStringLiteral("background"), QStringLiteral("#202020")));
        QVERIFY(store->setColor(QStringLiteral("textPrimary"), QStringLiteral("#262626")));
        // The empty-state hint is the wide layout's; a narrow window gives the
        // preview that room instead (pickerPanelVisible).
        const int w = m_window->width();
        const int h = m_window->height();
        m_window->setWidth(1920);
        m_window->setHeight(1000);
        QCoreApplication::processEvents();
        QTRY_COMPARE(m_window->width(), 1920);
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("readabilityProblems").toInt() > 0);

        const bool pillShown = shown(item("themeReadabilityBadge"));
        const bool closedAtFirst = !shown(item("themeReadabilityScroll"));
        QTRY_VERIFY(!dialog->property("compact").toBool());
        QTRY_VERIFY(shown(item("themeInspectorEmpty")));
        const bool hintInColumn = shown(item("themeInspectorEmpty"));

        clickItem(item("themeReadabilityBadge"));
        QTRY_VERIFY(dialog->property("reportOpen").toBool());
        // Every ThemePopover (menu, share, readability) has a panel of this
        // name; the one under the pill is the one that is on screen.
        const auto anyPanelShown = [&] {
            for (QQuickItem *p : items("themePopoverPanel")) {
                if (shown(p))
                    return true;
            }
            return false;
        };
        QTRY_VERIFY(anyPanelShown());
        const bool panelShown = anyPanelShown();
        int rows = 0;
        for (QQuickItem *row : items("themeReadabilityRow")) {
            if (shown(row))
                ++rows;
        }

        // Fix moves the colour of the first finding's element, popover stays.
        const int problemsBefore = dialog->property("readabilityProblems").toInt();
        const QString before = store->colors().value(QStringLiteral("textPrimary")).toString();
        QQuickItem *fix = nullptr;
        for (QQuickItem *f : items("themeReadabilityFix")) {
            if (shown(f)) {
                fix = f;
                break;
            }
        }
        QVERIFY2(fix, "no finding offers Fix");
        clickItem(fix);
        QTRY_VERIFY(dialog->property("readabilityProblems").toInt() < problemsBefore);
        const bool fixMoved =
            store->colors().value(QStringLiteral("textPrimary")).toString() != before
            || store->colors().value(QStringLiteral("background")).toString()
                   != QStringLiteral("#202020");
        const bool stillOpenAfterFix = dialog->property("reportOpen").toBool();

        // Escape closes the popover first.
        QVERIFY(QMetaObject::invokeMethod(
            dialog, "beginEdit", Q_ARG(QVariant, QStringLiteral("rail")),
            Q_ARG(QVariant, QStringLiteral("Rail"))));
        dialog->setProperty("reportOpen", true);
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(dialog, "peelEscape"));
        QCoreApplication::processEvents();
        const bool escapeClosedPopover = !dialog->property("reportOpen").toBool()
            && dialog->property("editingRole").toString() == QStringLiteral("rail");

        m_window->setWidth(w);
        m_window->setHeight(h);
        QCoreApplication::processEvents();
        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QVERIFY2(pillShown, "the readability pill is not in the header");
        QVERIFY2(closedAtFirst, "the report is on screen without being asked for");
        QVERIFY2(hintInColumn, "the right-hand column lost its empty state");
        QVERIFY2(panelShown, "clicking the pill opened nothing");
        QVERIFY2(rows >= 1, "the popover lists no finding");
        QVERIFY2(fixMoved, "Fix changed no colour");
        QVERIFY2(stillOpenAfterFix, "Fix closed the popover");
        QVERIFY2(escapeClosedPopover, "Escape did not close the popover first");
    }


    // Puts the theme collection back after a test that created or imported
    // themes: deletes every theme that was not there before and reselects the
    // one that was active.
    static void restoreThemes(CustomThemeStore *store, const QStringList &ids,
                              const QString &active)
    {
        const QVariantList now = store->themes();
        for (const QVariant &t : now) {
            const QString id = t.toMap().value(QStringLiteral("id")).toString();
            if (!ids.contains(id))
                store->deleteTheme(id);
        }
        if (!active.isEmpty())
            store->setActiveThemeId(active);
    }
    static QStringList themeIds(CustomThemeStore *store)
    {
        QStringList out;
        for (const QVariant &t : store->themes())
            out << t.toMap().value(QStringLiteral("id")).toString();
        return out;
    }

    // The themes (chips, name, New / Duplicate / Share / Import / Delete) are
    // one button and a menu in the top bar, and the left column no longer
    // carries them. Fails on the old tree: the chips, the name field and the
    // Manage drawer were in the left column and there was no menu button.
    void theThemeMenuLivesInTheTopBar()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        const QStringList idsBefore = themeIds(store);
        const QString activeBefore = store->activeThemeId();
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);

        QQuickItem *button = item("themeMenuButton");
        const bool buttonShown = shown(button);
        // Closed: nothing of it takes room, and the old column has none of it.
        const bool menuHidden = !shown(item("themeMenuPanel"))
                                && !shown(item("customThemeNameField"));
        const bool manageGone = item("themeManageToggleButton") == nullptr;

        clickItem(button);
        QTRY_VERIFY(dialog->property("menuOpen").toBool());
        QTRY_VERIFY(shown(item("themeMenuPanel")));
        const bool chipShown = shown(item(qPrintable(QStringLiteral("customThemeChip_") + activeBefore)));
        const bool actionsShown = shown(item("customThemeNewButton"))
            && shown(item("customThemeDuplicateButton"))
            && shown(item("customThemeShareButton"))
            && shown(item("customThemeImportButton"))
            && shown(item("customThemeDeleteButton"));

        // New adds a theme and selects it; the button shows its name.
        clickItem(item("customThemeNewButton"));
        const bool added = store->themes().size() == idsBefore.size() + 1
                           && store->activeThemeId() != activeBefore;
        // Switching back is a click on its row.
        const QString created = store->activeThemeId();
        clickItem(item(qPrintable(QStringLiteral("customThemeChip_") + activeBefore)));
        const bool switched = store->activeThemeId() == activeBefore;

        // Delete asks first, then removes the theme that is active.
        store->setActiveThemeId(created);
        QCoreApplication::processEvents();
        clickItem(item("customThemeDeleteButton"));
        const bool asked = dialog->property("confirmingDelete").toBool()
                           && shown(item("customThemeDeleteConfirmButton"))
                           && store->themes().size() == idsBefore.size() + 1;
        clickItem(item("customThemeDeleteConfirmButton"));
        const bool deleted = store->themes().size() == idsBefore.size();

        // Escape closes the menu before anything else.
        const bool openBeforeEscape = dialog->property("menuOpen").toBool();
        QVERIFY(QMetaObject::invokeMethod(dialog, "peelEscape"));
        QCoreApplication::processEvents();
        const bool escapeClosed = !dialog->property("menuOpen").toBool()
                                  && dialog->property("opened").toBool();

        closeThemeEditor();
        restoreThemes(store, idsBefore, activeBefore);
        resetThemeFixture(restoreBase);

        QVERIFY2(buttonShown, "the top bar has no theme menu button");
        QVERIFY2(menuHidden, "the theme menu is on screen without being asked for");
        QVERIFY2(manageGone, "the old Manage toggle is still in the left column");
        QVERIFY2(chipShown, "the menu does not list the current theme");
        QVERIFY2(actionsShown, "the menu is missing one of New / Duplicate / Share / Import / Delete");
        QVERIFY2(added, "New did not add and select a theme");
        QVERIFY2(switched, "a theme row did not switch themes");
        QVERIFY2(asked, "Delete did not ask first");
        QVERIFY2(deleted, "Delete did not remove the theme");
        QVERIFY(openBeforeEscape);
        QVERIFY2(escapeClosed, "Escape did not close the menu first");
    }

    // Share / Import is a popover that PREVIEWS before it applies, offers
    // "Import as new" and "Replace current", and a dropped file or code lands
    // in the same preview. Fails on the old tree: a pasted code was imported at
    // once from a hidden field in the left column.
    void importShowsAPreviewBeforeAnythingIsApplied()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        const QStringList idsBefore = themeIds(store);
        const QString activeBefore = store->activeThemeId();
        const QString nameBefore = store->name();
        QVERIFY(store->setColor(QStringLiteral("sidebar"), QStringLiteral("#123456")));
        QVERIFY(store->setColor(QStringLiteral("accent"), QStringLiteral("#FF8800")));
        const QString code = store->exportTheme(activeBefore);
        QVERIFY(!code.isEmpty());
        store->resetAll();
        store->sealUndoStep();
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);

        // A dropped code opens the Import tab with the preview, applying nothing.
        QVERIFY(QMetaObject::invokeMethod(dialog, "receiveThemeText",
                                          Q_ARG(QVariant, code)));
        QTRY_VERIFY(shown(item("themeImportPreview")));
        const bool openedOnImport = dialog->property("shareOpen").toBool()
            && dialog->property("shareMode").toString() == QStringLiteral("import");
        const QVariantMap preview = dialog->property("importPreview").toMap();
        const bool previewRight = preview.value(QStringLiteral("ok")).toBool()
            && preview.value(QStringLiteral("colourCount")).toInt() == 2
            && preview.value(QStringLiteral("swatch")).toMap()
                   .value(QStringLiteral("sidebar")).value<QColor>()
                   == QColor(QStringLiteral("#123456"));
        const bool swatchShown = shown(item("themeImportSwatch"));
        const bool readabilityLine = shown(item("themeImportReadability"));
        const bool nothingApplied = store->themes().size() == idsBefore.size()
                                    && store->overrideCount() == 0;

        // Not a theme: the store's reason, no card, no buttons.
        dialog->setProperty("importText", QStringLiteral("not a theme"));
        QCoreApplication::processEvents();
        const bool errorShown = shown(item("themeImportError"))
                                && !shown(item("themeImportPreview"))
                                && !shown(item("themeImportAsNewButton"));
        dialog->setProperty("importText", code);
        QCoreApplication::processEvents();

        // Replace current: the colours come, the name stays, Undo puts it back.
        QTRY_VERIFY(shown(item("themeImportReplaceButton")));
        clickItem(item("themeImportReplaceButton"));
        QTRY_VERIFY(!dialog->property("shareOpen").toBool());
        const bool replaced =
            store->colors().value(QStringLiteral("sidebar")).toString() == QStringLiteral("#123456")
            && store->name() == nameBefore
            && store->themes().size() == idsBefore.size();
        QVERIFY(QMetaObject::invokeMethod(dialog, "undoEdit"));
        const bool undone = store->overrideCount() == 0;

        // Import as new: a second theme appears and is selected.
        QVERIFY(QMetaObject::invokeMethod(dialog, "receiveThemeText",
                                          Q_ARG(QVariant, code)));
        QTRY_VERIFY(shown(item("themeImportAsNewButton")));
        clickItem(item("themeImportAsNewButton"));
        QTRY_VERIFY(store->themes().size() == idsBefore.size() + 1);
        const bool importedNew = store->activeThemeId() != activeBefore
            && store->colors().value(QStringLiteral("accent")).toString() == QStringLiteral("#FF8800");

        closeThemeEditor();
        restoreThemes(store, idsBefore, activeBefore);
        resetThemeFixture(restoreBase);

        QVERIFY2(openedOnImport, "a dropped code did not open the Import tab");
        QVERIFY2(previewRight, "the preview does not describe the incoming theme");
        QVERIFY2(swatchShown, "no small rendered preview of the incoming theme");
        QVERIFY2(readabilityLine, "the preview says nothing about readability");
        QVERIFY2(nothingApplied, "previewing changed the collection or the theme");
        QVERIFY2(errorShown, "a bad paste is not refused in the popover");
        QVERIFY2(replaced, "Replace current did not replace the colours and keep the name");
        QVERIFY2(undone, "Replace current is not one undo step");
        QVERIFY2(importedNew, "Import as new did not add and select the theme");
    }


    // The sample window can be driven from the keyboard: one Tab stop, then
    // Tab or the arrows walk the regions in reading order with a visible ring,
    // Enter opens the element in the inspector, Escape hands focus back to the
    // list; every region has an accessible name. Fails on the old tree: a
    // region was a MouseArea and nothing in the preview took focus.
    void thePreviewCanBeWalkedAndOpenedFromTheKeyboard()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        resetThemeFixture(11);
        QVERIFY(store->setColor(QStringLiteral("roomHeader"), QStringLiteral("#334455")));
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);
        QQuickItem *preview = item("themePreviewDemo");
        QQuickItem *entry = item("themePreviewKeyboardEntry");
        QVERIFY(preview && entry);
        const auto role = [&] { return preview->property("keyboardRole").toString(); };

        // The regions come in reading order: top to bottom in bands, a
        // container before what it holds.
        QVariant ordered;
        QVERIFY(QMetaObject::invokeMethod(preview, "orderedRegions",
                                          Q_RETURN_ARG(QVariant, ordered)));
        const QVariantList regions = ordered.toList();
        QVERIFY2(regions.size() > 40, "the keyboard order holds almost no regions");
        qreal lastBand = -1;
        bool inOrder = true;
        for (const QVariant &v : regions) {
            auto *r = qobject_cast<QQuickItem *>(v.value<QObject *>());
            if (!r) {
                inOrder = false;
                break;
            }
            const qreal band = qRound(r->mapToItem(preview, QPointF(0, 0)).y() / 10.0);
            if (band < lastBand)
                inOrder = false;
            lastBand = band;
        }

        // The one Tab stop hands focus to a region.
        entry->forceActiveFocus();
        QTRY_VERIFY(!role().isEmpty());
        const QString first = role();
        const bool ringShown = !items("themeKeyboardRing").isEmpty();

        // Arrow keys move on and back; Home and End reach the ends.
        QTest::keyClick(m_window, Qt::Key_Right);
        const QString second = role();
        QTest::keyClick(m_window, Qt::Key_Left);
        const QString backAgain = role();
        QTest::keyClick(m_window, Qt::Key_End);
        const QString last = role();
        QTest::keyClick(m_window, Qt::Key_Down);
        const QString stoppedAtEnd = role();
        QTest::keyClick(m_window, Qt::Key_Home);
        const QString home = role();
        QTest::keyClick(m_window, Qt::Key_Tab);
        const QString tabbed = role();

        // Enter opens the element in the inspector.
        dialog->setProperty("editingRole", QString());
        QTest::keyClick(m_window, Qt::Key_Return);
        QCoreApplication::processEvents();
        const QString opened = dialog->property("editingRole").toString();

        // Escape hands focus back to the list's search field.
        QTest::keyClick(m_window, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QQuickItem *filter = item("themeRoleFilterField");
        const bool focusInList = filter && filter->property("activeFocus").toBool();
        const bool editorStillOpen = dialog->property("opened").toBool();

        // Every region names itself and says where its colour comes from.
        QFile src(QStringLiteral(QML_DIR) + QStringLiteral("/ThemePreviewDemo.qml"));
        QVERIFY(src.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(src.readAll());
        const int regionAt = text.indexOf(QStringLiteral("component Region: MouseArea"));
        const int regionEnd = text.indexOf(QStringLiteral("component GradientLayer"));
        const QString regionSrc = text.mid(regionAt, regionEnd - regionAt);
        QVariant stateVar;
        QVERIFY(QMetaObject::invokeMethod(dialog, "describeRoleState",
                                          Q_RETURN_ARG(QVariant, stateVar),
                                          Q_ARG(QVariant, QStringLiteral("roomHeader"))));
        QVariant linkedVar;
        QVERIFY(QMetaObject::invokeMethod(dialog, "describeRoleState",
                                          Q_RETURN_ARG(QVariant, linkedVar),
                                          Q_ARG(QVariant, QStringLiteral("roomList"))));

        closeThemeEditor();
        resetThemeFixture(restoreBase);

        QVERIFY2(inOrder, "the keyboard order is not top to bottom");
        QVERIFY2(!first.isEmpty(), "the Tab stop did not land on a region");
        QVERIFY2(ringShown, "the focused region draws no keyboard ring");
        QVERIFY2(second != first, "Right did not move to the next region");
        QCOMPARE(backAgain, first);
        QVERIFY(!last.isEmpty() && last != first);
        QCOMPARE(stoppedAtEnd, last);
        QCOMPARE(home, first);
        QVERIFY2(tabbed != first, "Tab did not move to the next region");
        QVERIFY2(!opened.isEmpty() && opened == tabbed,
                 "Enter did not open the focused element in the inspector");
        QVERIFY2(focusInList, "Escape did not return focus to the list");
        QVERIFY2(editorStillOpen, "Escape from the preview closed the editor");
        QVERIFY2(regionSrc.contains(QStringLiteral("Accessible.name"))
                     && regionSrc.contains(QStringLiteral("Accessible.role"))
                     && regionSrc.contains(QStringLiteral("Accessible.description")),
                 "a Region does not name itself for assistive technology");
        const QString stateText = stateVar.toString();
        const QString linkedText = linkedVar.toString();
        QVERIFY2(!stateText.isEmpty(), "an own colour is not described as such");
        QVERIFY2(!linkedText.isEmpty() && linkedText != stateText,
                 "a linked element does not say what it follows");
    }


    // Giving one element a colour changes that element and nothing else: in
    // the resolved palette (every other key keeps its colour), in the live
    // token the app reads, and in the preview (no Region of another role
    // changes colour). Run for every element role.
    //
    // Fails on the old tree: setColor refuses the element roles, so none
    // changes; and the room header shared `surface` with the people panel.
    void anElementOverrideChangesOnlyThatElement()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        const int restoreBase = store->baseTheme();
        const int restoreTheme = int(m_controller->settings()->theme());
        resetThemeFixture(11);
        m_controller->settings()->setTheme(SettingsManager::CustomTheme);
        QCoreApplication::processEvents();
        QObject *dialog = openThemeEditor();
        QVERIFY(dialog);

        const auto regionColour = [](QQuickItem *r) {
            QQuickItem *p = r->parentItem();
            const QVariant c = p ? p->property("color") : QVariant();
            return c.isValid() ? c.value<QColor>() : QColor();
        };
        const QList<QQuickItem *> regions = previewRegions();
        QVERIFY(regions.size() > 90);

        // Leaves only: a role others follow (`accentSoft`, `unreadBadge`) is
        // a group, and recolouring its elements is its job.
        QStringList elements;
        for (const QVariant &v : store->roles()) {
            const QVariantMap m = v.toMap();
            const QString key = m.value(QStringLiteral("key")).toString();
            if (!m.value(QStringLiteral("follows")).toString().isEmpty()
                    && store->groupMembers(key).isEmpty())
                elements << key;
        }
        QVERIFY(elements.size() >= 80);
        const auto liveTokens = [&]() {
            QHash<QString, QColor> out;
            for (const QString &e : elements) {
                const QString k = store->paletteKeyForRole(e);
                out.insert(k, themeColor(k.toUtf8().constData()));
            }
            return out;
        };

        const QColor mark(QStringLiteral("#FE01FD"));
        QStringList leaked;
        QStringList notApplied;
        int previewChanged = 0;
        int checked = 0;
        for (const QString &role : elements) {
            const QString key = store->paletteKeyForRole(role);
            const QHash<QString, QColor> before = resolvedPaletteOf(12);
            const QHash<QString, QColor> liveBefore = liveTokens();
            QHash<QQuickItem *, QColor> regionBefore;
            for (QQuickItem *r : regions)
                regionBefore.insert(r, regionColour(r));

            if (!store->setColor(role, mark.name())) {
                notApplied << role + QStringLiteral(" (refused)");
                continue;
            }
            QCoreApplication::processEvents();
            ++checked;

            const QHash<QString, QColor> after = resolvedPaletteOf(12);
            if (after.value(key) != mark)
                notApplied << role + QStringLiteral(" (palette)");
            if (themeColor(key.toUtf8().constData()) != mark)
                notApplied << role + QStringLiteral(" (live token)");
            // The badge tokens are DERIVED (badges, 2026-10-20): the rings and
            // the Channels mark are fitted against the surfaces a dot sits
            // on, and the count/mention fills and inks against the badge
            // colours they come from. Recolouring either refits them by
            // design; they are not other elements.
            static const QStringList surfaceFitted = {
                QStringLiteral("unreadDotRing"), QStringLiteral("mentionDotRing"),
                QStringLiteral("railUnreadDotRing"), QStringLiteral("channelUnreadMark"),
                QStringLiteral("unreadDot"), QStringLiteral("unreadCountFill"),
                QStringLiteral("unreadCountInk"), QStringLiteral("mentionBadgeFill"),
                QStringLiteral("mentionBadgeInk"),
            };
            for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
                if (it.key() != key && after.value(it.key()) != it.value()
                        && !surfaceFitted.contains(it.key()))
                    leaked << role + QStringLiteral(" also changed palette ") + it.key();
            }
            const QHash<QString, QColor> liveAfter = liveTokens();
            for (auto it = liveBefore.constBegin(); it != liveBefore.constEnd(); ++it) {
                if (it.key() != key && liveAfter.value(it.key()) != it.value())
                    leaked << role + QStringLiteral(" also changed live ") + it.key();
            }
            bool own = false;
            for (QQuickItem *r : regions) {
                const QColor now = regionColour(r);
                if (now == regionBefore.value(r))
                    continue;
                if (r->property("role").toString() == role)
                    own = true;
                else
                    leaked << role + QStringLiteral(" also changed the preview's ")
                                  + r->property("role").toString();
            }
            if (own)
                ++previewChanged;
            store->resetColor(role);
            QCoreApplication::processEvents();
        }

        closeThemeEditor();
        m_controller->settings()->setTheme(SettingsManager::Theme(restoreTheme));
        resetThemeFixture(restoreBase);

        QCOMPARE(checked, elements.size());
        QVERIFY2(notApplied.isEmpty(), qPrintable(notApplied.join(QStringLiteral(", "))));
        leaked.removeDuplicates();
        QVERIFY2(leaked.isEmpty(),
                 qPrintable(QStringLiteral("an element override reached other "
                                           "elements:\n  %1")
                                .arg(leaked.mid(0, 40).join(QStringLiteral("\n  ")))));
        // Most elements paint their own fill or ink; lines drawn as a
        // border and the identity-derived ones are seen in the palette only.
        QVERIFY2(previewChanged >= 60,
                 qPrintable(QStringLiteral("only %1 elements changed in the preview")
                                .arg(previewChanged)));
    }

    void createAThemeOpensTheEditorOnTheFirstClick()
    {
        auto *store = m_controller->customTheme();
        QVERIFY(store);
        store->discard();
        QCoreApplication::processEvents();
        QVERIFY(!store->exists());

        // As in the app: Settings is built in the background (asynchronous
        // Loader) before it is first shown.
        auto *settingsLoader = item("settingsViewLoader");
        QVERIFY(settingsLoader);
        QTRY_VERIFY_WITH_TIMEOUT(
            settingsLoader->property("item").value<QObject *>(), 10000);

        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        auto *button = item("customThemeEditButton");
        auto *loader = item("themeEditorLoader");
        QVERIFY(button);
        QVERIFY(loader);
        QCOMPARE(button->property("text").toString(),
                 QStringLiteral("Create a theme"));
        QVERIFY(!loader->property("active").toBool());

        // A real pointer arrives first and hovers the button.
        ensureVisible(button);
        const QPoint centre = button->mapToScene(
            QPointF(button->width() / 2, button->height() / 2)).toPoint();
        QTest::mouseMove(m_window, centre - QPoint(40, 0));
        QTest::qWait(50);
        QTest::mouseMove(m_window, centre);
        QTest::qWait(50);
        clickItem(button);
        QTRY_VERIFY(popup("themeEditorDialog"));
        QPointer<QObject> dialog = popup("themeEditorDialog");
        QTRY_VERIFY(dialog && dialog->property("opened").toBool());
        // Still open once everything the open set off has settled.
        QTest::qWait(300);
        const bool stillOpen = dialog && dialog->property("opened").toBool();
        const bool created = store->exists();

        if (dialog)
            QMetaObject::invokeMethod(dialog.data(), "close");
        QTRY_VERIFY(!loader->property("active").toBool());

        QVERIFY2(stillOpen, "the first click's editor closed again");
        QVERIFY(created);
    }

    // "grad" in the editor's role filter lists exactly the surfaces that can
    // take a gradient.
    void theThemeEditorFilterFindsTheGradientSurfaces()
    {
        auto *store = m_controller->customTheme();
        auto *loader = item("themeEditorLoader");
        QVERIFY(store && loader);
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        loader->setProperty("active", true);
        QTRY_VERIFY(popup("themeEditorDialog"));
        QObject *dialog = popup("themeEditorDialog");
        QTRY_VERIFY(dialog->property("opened").toBool());

        dialog->setProperty("roleFilter", QStringLiteral("grad"));
        QCoreApplication::processEvents();
        QSet<QString> listed;
        for (const QVariant &g : dialog->property("roleGroups").toList()) {
            for (const QVariant &r : g.toMap().value(QStringLiteral("items")).toList())
                listed.insert(r.toMap().value(QStringLiteral("key")).toString());
        }
        QSet<QString> expected;
        for (const QVariant &r : store->roles()) {
            const QString key = r.toMap().value(QStringLiteral("key")).toString();
            if (store->isGradientRole(key))
                expected.insert(key);
        }
        dialog->setProperty("roleFilter", QString());
        loader->setProperty("active", false);
        QCoreApplication::processEvents();

        QVERIFY2(expected.size() >= 4,
                 qPrintable(QStringLiteral("only %1 gradient roles to look for")
                                .arg(expected.size())));
        QCOMPARE(listed, expected);
    }

    // ───────────── Settings structure (2026-10-13) ─────────────

    // The first item with this name that is actually showing: Repeater
    // delegates repeat across the per-section outlines, and only the open
    // section's is visible.
    QQuickItem *visibleItem(const QString &name) const
    {
        QList<QQuickItem *> all;
        collectItems(m_window->contentItem(), name, all);
        for (QQuickItem *it : all) {
            if (it->isVisible() && it->width() > 0 && it->height() > 0)
                return it;
        }
        return nullptr;
    }

    // One branch's sub-section entry: several branches can be open at once
    // and their entries share objectNames.
    QQuickItem *outlineEntry(const QString &section, int index) const
    {
        auto *outline = findItem(m_window->contentItem(),
                                 QStringLiteral("settingsNavOutline_") + section);
        return outline ? findItem(outline, QStringLiteral("settingsNavOutlineEntry_%1")
                                               .arg(index))
                       : nullptr;
    }

    QVariant callScreen(const char *method, const QVariant &a = QVariant(),
                        const QVariant &b = QVariant())
    {
        auto *screen = item("settingsScreenRoot");
        QVariant ret;
        bool ok = false;
        if (b.isValid())
            ok = QMetaObject::invokeMethod(screen, method,
                                           Q_RETURN_ARG(QVariant, ret),
                                           Q_ARG(QVariant, a), Q_ARG(QVariant, b));
        else if (a.isValid())
            ok = QMetaObject::invokeMethod(screen, method,
                                           Q_RETURN_ARG(QVariant, ret),
                                           Q_ARG(QVariant, a));
        else
            ok = QMetaObject::invokeMethod(screen, method,
                                           Q_RETURN_ARG(QVariant, ret));
        if (!ok)
            qWarning("could not call %s on the settings screen", method);
        return ret;
    }

    void openSection(const QString &section)
    {
        m_controller->showSettings();
        if (auto *search = item("settingsSearchField"))
            search->setProperty("text", QString());
        m_controller->showSettingsSection(section);
        // Branches stay open across page changes since the multi-open nav;
        // each case starts from the opened page's branch alone and the nav
        // at its top, so one case's expanded tree cannot move the next
        // case's targets (the copy-link click missed after the outline
        // sweep had opened and scrolled every branch).
        if (auto *screen = item("settingsScreenRoot"))
            screen->setProperty("expandedOutlines",
                                QVariantMap{ { section, true } });
        if (auto *nav = item("settingsNavFlick"))
            nav->setProperty("contentY", 0.0);
        QCoreApplication::processEvents();
        if (auto *flick = item("settingsContentFlick"))
            flick->setProperty("contentY", 0.0);
        QCoreApplication::processEvents();
    }

    QVariant themeValue(const QString &expression) const
    {
        QQmlExpression expr(qmlContext(m_window), m_window,
                            QStringLiteral("AppTheme.") + expression);
        return expr.evaluate();
    }

    // Each page's "On this page" outline lists headings that exist, in page
    // order; a click scrolls to one and marks it; scrolling to one marks it.
    void theOutlineReachesEveryGroupOfEachPageAndFollowsTheScroll_data()
    {
        QTest::addColumn<QString>("section");
        QTest::addColumn<int>("atLeast");
        QTest::newRow("appearance") << "appearance" << 11;
        QTest::newRow("privacy") << "privacy" << 5;
        QTest::newRow("notifications") << "notifications" << 4;
        QTest::newRow("sound") << "sound" << 4;
        QTest::newRow("account") << "account" << 2;
    }
    void theOutlineReachesEveryGroupOfEachPageAndFollowsTheScroll()
    {
        QFETCH(QString, section);
        QFETCH(int, atLeast);
        openSection(section);
        auto *screen = item("settingsScreenRoot");
        auto *flick = item("settingsContentFlick");
        QVERIFY(screen && flick);
        auto *content = flick->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);

        const QVariantList entries =
            callScreen("outlineEntries", section).toList();
        QVERIFY2(entries.size() >= atLeast,
                 qPrintable(QStringLiteral("%1 has only %2 outline entries")
                                .arg(section).arg(entries.size())));

        QStringList wrong;
        qreal lastTop = -1.0;
        int reached = 0;
        for (int i = 0; i < entries.size(); ++i) {
            const QStringList anchors = entries.at(i).toMap()
                .value(QStringLiteral("anchors")).toStringList();
            QQuickItem *target = nullptr;
            for (const QString &a : anchors) {
                QQuickItem *t = findItem(content, a);
                if (t && t->isVisible()) {
                    target = t;
                    break;
                }
            }
            if (!target)
                continue;   // a card this backend does not show

            flick->setProperty("contentY", 0.0);
            QCoreApplication::processEvents();
            auto *button = outlineEntry(section, i);
            QVERIFY2(button, qPrintable(QStringLiteral(
                "%1 outline entry %2 is not showing").arg(section).arg(i)));
            const qreal top = target->mapToItem(content, QPointF(0, 0)).y();
            clickItem(button);
            for (int t = 0; t < 60; ++t) {
                const qreal y = flick->property("contentY").toReal();
                if (top >= y && top <= y + flick->height())
                    break;
                QTest::qWait(10);
            }
            const qreal y = flick->property("contentY").toReal();
            if (top < y || top > y + flick->height())
                wrong.append(QStringLiteral("%1 -> %2 at %3, viewport %4..%5")
                                 .arg(entries.at(i).toMap()
                                          .value(QStringLiteral("label")).toString(),
                                      anchors.first())
                                 .arg(top).arg(y).arg(y + flick->height()));
            if (top < lastTop)
                wrong.append(QStringLiteral("%1 comes before the entry ahead of it")
                                 .arg(anchors.first()));
            lastTop = top;
            QTRY_COMPARE_WITH_TIMEOUT(screen->property("outlineActive").toInt(),
                                      i, 2000);
            ++reached;
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QStringLiteral("\n  "))));
        QVERIFY2(reached >= atLeast - 1,
                 qPrintable(QStringLiteral("only %1 entries reachable").arg(reached)));

        // A click's reveal keeps re-asserting its position for up to twelve
        // frames (searchRevealSettle), so a jump made while it is still
        // running is undone by it: the last click's settle put contentY back
        // at the end of the page under the first "scroll" below. Let it finish
        // before scrolling by hand.
        {
            qreal last = -1.0;
            int steady = 0;
            for (int t = 0; t < 200 && steady < 8; ++t) {
                QTest::qWait(25);
                const qreal now = flick->property("contentY").toReal();
                steady = (qAbs(now - last) < 0.5) ? steady + 1 : 0;
                last = now;
            }
            QVERIFY2(steady >= 8, "the page never stopped moving after the last click");
        }

        // Scrolling, not clicking, moves the mark. Skip entries the page
        // cannot scroll far enough to put at the top.
        const qreal maxY = qMax(0.0, flick->property("contentHeight").toReal()
                                         - flick->height());
        int followed = 0;
        for (int i = 0; i < entries.size(); ++i) {
            const QStringList anchors = entries.at(i).toMap()
                .value(QStringLiteral("anchors")).toStringList();
            QQuickItem *target = nullptr;
            for (const QString &a : anchors) {
                QQuickItem *t = findItem(content, a);
                if (t && t->isVisible()) {
                    target = t;
                    break;
                }
            }
            if (!target)
                continue;
            const qreal want = target->mapToItem(content, QPointF(0, 0)).y() - 40;
            if (want > maxY - 4 || want < 0)
                continue;
            flick->setProperty("contentY", 0.0);
            QCoreApplication::processEvents();
            flick->setProperty("contentY", want);
            QTRY_COMPARE_WITH_TIMEOUT(screen->property("outlineActive").toInt(),
                                      i, 2000);
            ++followed;
        }
        // A page that scrolls must let at least one entry be followed. A page
        // that (nearly) fits its viewport cannot: the mock account page is
        // 868 px in an 849 px viewport (maxY 19), with Password (Rust only)
        // hidden. It used to pass only because an earlier case left the
        // name-colour picker open (510 px), which Settings now closes when it
        // is hidden (break-it B138).
        if (maxY >= 60)
            QVERIFY2(followed >= 1, "no outline entry could be followed by scrolling");
        flick->setProperty("contentY", 0.0);
        QCoreApplication::processEvents();
    }

    // The tray controls are hidden where there is no tray, so a search must
    // not offer them there (the jump would land on the page top).
    void trayEntriesFollowTrayAvailability()
    {
        openSection(QStringLiteral("appearance"));
        auto *screen = item("settingsScreenRoot");
        auto *search = item("settingsSearchField");
        QVERIFY(screen && search);
        search->setProperty("text", QStringLiteral("tray"));
        QCoreApplication::processEvents();
        int trayHits = 0;
        const QVariantList hits = screen->property("matchedSearchResults").toList();
        for (const QVariant &hit : hits) {
            const QString a = hit.toMap().value(QStringLiteral("anchor")).toString();
            if (a == QStringLiteral("closeToTrayCheck")
                || a == QStringLiteral("startInTrayCheck"))
                ++trayHits;
        }
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
        QCOMPARE(trayHits, m_controller->trayAvailable() ? 2 : 0);
    }

    // "call keys" found nothing though the control is labelled "Use call keys
    // from any window". Old code: no such index entry, so no hit. The entry
    // is Linux-only (the card is `visible: Qt.platform.os === "linux"`), so
    // elsewhere it must not be offered.
    void callKeysIsFindableInSettingsSearch()
    {
        openSection(QStringLiteral("shortcuts"));
        auto *screen = item("settingsScreenRoot");
        auto *search = item("settingsSearchField");
        QVERIFY(screen && search);
        search->setProperty("text", QStringLiteral("call keys"));
        QCoreApplication::processEvents();
        bool found = false;
        const QVariantList hits = screen->property("matchedSearchResults").toList();
        for (const QVariant &hit : hits) {
            found = found
                || hit.toMap().value(QStringLiteral("anchor")).toString()
                       == QStringLiteral("globalShortcutsCheck");
        }
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
#ifdef Q_OS_LINUX
        QVERIFY2(found, "searching Settings for \"call keys\" finds nothing");
        // The hit must land on a real control.
        QVERIFY2(item("globalShortcutsCheck"),
                 "the call-keys search anchor is not a live control");
#else
        QVERIFY2(!found, "the Linux-only call-keys entry is offered elsewhere");
#endif
    }

    // German "Datenschutz & Sicherheit" was cut to "Datenschutz & Sicherh…"
    // in the 260 px nav column (live 0.10.2 capture). Old code: the label is
    // wider than its slot at 260, so implicitWidth > width.
    void theLongestGermanNavLabelFitsItsRow()
    {
        openSection(QStringLiteral("privacy"));
        auto *column = item("settingsNavColumn");
        auto *row = item("settingsNavRow_privacy");
        QVERIFY(column && row);
        QCOMPARE(column->width(), 300.0);
        QQuickItem *label = nullptr;
        const auto children =
            row->property("contentItem").value<QQuickItem *>()->childItems();
        for (QQuickItem *child : children) {
            // The label, not the disclosure chevron or the alert dot.
            if (child->property("elide").isValid()
                && !child->objectName().startsWith(QLatin1String("settingsNav")))
                label = child;
        }
        QVERIFY2(label, "the nav row has no elided label");
        const QString original = label->property("text").toString();
        label->setProperty("text", QStringLiteral("Datenschutz & Sicherheit"));
        QCoreApplication::processEvents();
        const qreal need = label->property("implicitWidth").toReal();
        const qreal have = label->width();
        label->setProperty("text", original);
        QVERIFY2(need <= have,
                 qPrintable(QStringLiteral("label needs %1 px, row gives %2")
                                .arg(need).arg(have)));
    }

    // Dates follow the chosen language: bindings re-run on clockFormatChanged,
    // so a language switch must announce it. Old code: no connect, no signal.
    void switchingLanguageAnnouncesTheClockFormat()
    {
        auto *loc = m_controller->localization();
        const QString before = loc->language();
        const QString other = before == QStringLiteral("es")
            ? QStringLiteral("system") : QStringLiteral("es");
        QSignalSpy clock(m_controller->settings(),
                         &SettingsManager::clockFormatChanged);
        loc->setLanguage(other);
        const int count = clock.count();
        loc->setLanguage(before);
        QVERIFY2(count >= 1, "a language switch did not announce clockFormatChanged");
    }

    void theNavIsGroupedUserInterfaceBehaviorAbout()
    {
        openSection(QStringLiteral("appearance"));
        auto *nav = item("settingsNavColumn");
        QVERIFY(nav);
        const auto yOf = [&](const char *name) {
            QQuickItem *it = item(name);
            [&] { QVERIFY2(it, name); }();
            return it ? it->mapToItem(nav, QPointF(0, 0)).y() : -1.0;
        };
        const qreal user = yOf("settingsNavGroup_user");
        const qreal iface = yOf("settingsNavGroup_interface");
        const qreal behavior = yOf("settingsNavGroup_behavior");
        const qreal about = yOf("settingsNavGroup_about");
        QVERIFY(user >= 0 && user < iface && iface < behavior && behavior < about);
        QCOMPARE(item("settingsNavGroup_user")->property("text").toString(),
                 QStringLiteral("User"));
        QCOMPARE(item("settingsNavGroup_interface")->property("text").toString(),
                 QStringLiteral("Interface"));
        QCOMPARE(item("settingsNavGroup_behavior")->property("text").toString(),
                 QStringLiteral("Behavior"));
        QCOMPARE(item("settingsNavGroup_about")->property("text").toString(),
                 QStringLiteral("About"));

        const auto inside = [&](const char *row, qreal from, qreal to) {
            const qreal y = yOf(row);
            QVERIFY2(y > from && y < to, row);
        };
        inside("settingsNavRow_account", user, iface);
        inside("settingsNavRow_privacy", user, iface);
        inside("settingsNavRow_sessions", user, iface);
        inside("settingsNavRow_appearance", iface, behavior);
        inside("settingsNavRow_shortcuts", iface, behavior);
        inside("settingsNavRow_notifications", behavior, about);
        inside("settingsNavRow_sound", behavior, about);
        inside("settingsNavRow_labs", behavior, about);
        inside("settingsNavRow_updates", about, 1e9);
        inside("settingsNavRow_about", about, 1e9);
    }

    void aSearchHidesTheGroupHeadingsOfSectionsItRulesOut()
    {
        openSection(QStringLiteral("appearance"));
        auto *search = item("settingsSearchField");
        QVERIFY(search);
        // Not "microphone": the Message box buttons setting (Appearance, in
        // the Interface group) is findable by it, because the composer has a
        // voice-message microphone button. "speaker" is only the Sound page.
        search->setProperty("text", QStringLiteral("speaker"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(!item("settingsNavGroup_user")->isVisible());
        QVERIFY(!item("settingsNavGroup_interface")->isVisible());
        QVERIFY(item("settingsNavGroup_behavior")->isVisible());
        // The outline gives way to the results.
        QVERIFY(!visibleItem(QStringLiteral("settingsNavOutlineEntry_0")));
        search->setProperty("text", QString());
        QCoreApplication::processEvents();
        QTRY_VERIFY(item("settingsNavGroup_user")->isVisible());
    }

    void aSettingLinkRoundTripsAndOpensItsSetting()
    {
        openSection(QStringLiteral("appearance"));
        auto *screen = item("settingsScreenRoot");
        QCOMPARE(callScreen("settingsLink", QStringLiteral("privacy"),
                            QStringLiteral("readReceiptModeCombo")).toString(),
                 QStringLiteral("lightning://settings/privacy?focus="
                                "readReceiptModeCombo"));

        // Every indexed setting yields a link that parses back to itself.
        const QVariantList index = screen->property("searchIndex").toList();
        QVERIFY(index.size() >= 60);
        int checked = 0;
        for (const QVariant &v : index) {
            const QVariantMap e = v.toMap();
            const QString section = e.value(QStringLiteral("section")).toString();
            const QString anchor = e.value(QStringLiteral("anchor")).toString();
            const QString link = callScreen("settingsLink", section, anchor).toString();
            const QVariantMap parsed = callScreen("parseSettingsLink", link).toMap();
            QVERIFY2(parsed.value(QStringLiteral("section")).toString() == section
                         && parsed.value(QStringLiteral("anchor")).toString() == anchor,
                     qPrintable(link));
            ++checked;
        }
        QCOMPARE(checked, index.size());

        // Things that are not links to a setting.
        for (const QString &bad : {
                 QStringLiteral("https://example.org/settings/privacy"),
                 QStringLiteral("lightning://settings/nowhere?focus=x"),
                 QStringLiteral("lightning://settings/privacy?focus=a%20b"),
                 QStringLiteral("lightning://settings/privacy?focus=%E0%A4%A"),
                 QStringLiteral(""),
             }) {
            QVERIFY2(callScreen("parseSettingsLink", bad).isNull()
                         || !callScreen("parseSettingsLink", bad).isValid()
                         || callScreen("parseSettingsLink", bad).toMap().isEmpty(),
                     qPrintable(bad));
            QVERIFY2(!callScreen("openSettingsLink", bad).toBool(), qPrintable(bad));
        }
        // A section alone is a link to the section.
        QVERIFY(callScreen("openSettingsLink",
                           QStringLiteral("lightning://settings/sound")).toBool());
        QCOMPARE(screen->property("section").toString(), QStringLiteral("sound"));

        // Pasting one into the search box opens it and empties the box.
        openSection(QStringLiteral("appearance"));
        auto *search = item("settingsSearchField");
        search->setProperty("text",
            QStringLiteral("lightning://settings/privacy?focus=readReceiptModeCombo"));
        QTRY_COMPARE(screen->property("section").toString(), QStringLiteral("privacy"));
        QCOMPARE(screen->property("searchRevealAnchor").toString(),
                 QStringLiteral("readReceiptModeCombo"));
        QTRY_COMPARE(search->property("text").toString(), QString());
        QCoreApplication::processEvents();
        auto *flick = item("settingsContentFlick");
        auto *content = flick->property("contentItem").value<QQuickItem *>();
        QQuickItem *target = findItem(content, QStringLiteral("readReceiptModeCombo"));
        QVERIFY(target);
        QTRY_VERIFY(target->mapToItem(content, QPointF(0, 0)).y()
                    >= flick->property("contentY").toReal() - 1);
    }

    void aSearchResultAndARightClickCopyTheSettingsLink()
    {
        openSection(QStringLiteral("appearance"));
        auto *clipboard = QGuiApplication::clipboard();
        clipboard->clear();

        auto *search = item("settingsSearchField");
        search->setProperty("text", QStringLiteral("reduce motion"));
        QCoreApplication::processEvents();
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = visibleItem(
            QStringLiteral("settingsSearchResultCopyLink_0"))) != nullptr);
        // Click where the button really is: inside the nav viewport once the
        // column has laid out the results, not at a pre-layout position.
        auto *nav = item("settingsNavFlick");
        QVERIFY(nav);
        QTRY_VERIFY([&] {
            const QPointF p = button->mapToItem(nav, QPointF(0, 0));
            return p.y() >= 0 && p.y() + button->height() <= nav->height();
        }());
        clickItem(button);
        QCOMPARE(clipboard->text(),
                 QStringLiteral("lightning://settings/appearance?focus=reducedMotionCheck"));
        search->setProperty("text", QString());
        QCoreApplication::processEvents();

        // Right-click the setting itself.
        clipboard->clear();
        openSection(QStringLiteral("appearance"));
        auto *check = item("reducedMotionCheck");
        QVERIFY(check);
        ensureVisible(check);
        const QPointF at = check->mapToScene(QPointF(check->width() / 2,
                                                     check->height() / 2));
        QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier, at.toPoint());
        QCoreApplication::processEvents();
        QQuickItem *copy = nullptr;
        QTRY_VERIFY((copy = visibleItem(QStringLiteral("settingLinkMenuCopy"))) != nullptr);
        clickItem(copy);
        QTRY_COMPARE(clipboard->text(),
                     QStringLiteral("lightning://settings/appearance?focus=reducedMotionCheck"));
    }

    // A disabled dependent row names the setting it needs, takes no layout
    // space, and goes away when the setting is on.
    void aDisabledRowSaysWhichSettingItNeeds()
    {
        openSection(QStringLiteral("appearance"));
        auto *settings = m_controller->settings();
        const bool before = settings->showRoomActivity();
        auto *membership = item("showMembershipEventsCheck");
        auto *below = item("collapseEmbedsCheck");
        QVERIFY(membership && below);
        ensureVisible(membership);

        settings->setShowRoomActivity(true);
        QCoreApplication::processEvents();
        auto *badge = findItem(membership, QStringLiteral("settingsNeedsBadge"));
        QVERIFY2(badge, "the dependent row carries no Needs badge");
        QVERIFY(!badge->isVisible());
        const qreal belowY = contentPosY(below);

        settings->setShowRoomActivity(false);
        QCoreApplication::processEvents();
        QTRY_VERIFY(badge->isVisible());
        QVERIFY(!membership->isEnabled());
        QVERIFY2(badge->property("needs").toString()
                     == QStringLiteral("Show room activity"),
                 qPrintable(badge->property("needs").toString()));
        // Beside the row, not under it: nothing below moves.
        QCOMPARE(contentPosY(below), belowY);
        // Inside the card, clear of the control.
        QVERIFY(badge->x() >= membership->width());
        auto *flick = item("settingsContentFlick");
        QVERIFY(badge->mapToItem(flick, QPointF(badge->width(), 0)).x()
                <= flick->width());

        settings->setShowRoomActivity(before);
        QCoreApplication::processEvents();
        QTRY_VERIFY(!badge->isVisible());
    }

    void callSoundRowsNameTheirParentToo()
    {
        openSection(QStringLiteral("sound"));
        auto *settings = m_controller->settings();
        const bool before = settings->callSoundsEnabled();
        auto *row = item("callSoundsPresenceCheck");
        QVERIFY(row);
        auto *badge = findItem(row, QStringLiteral("settingsNeedsBadge"));
        QVERIFY(badge);
        settings->setCallSoundsEnabled(false);
        QTRY_VERIFY(badge->isVisible());
        QCOMPARE(badge->property("needs").toString(),
                 QStringLiteral("Play sounds during calls"));
        settings->setCallSoundsEnabled(true);
        QTRY_VERIFY(!badge->isVisible());
        settings->setCallSoundsEnabled(before);
    }

    void riskyTogglesCarryAShortPrivacyHintUnderThem()
    {
        openSection(QStringLiteral("privacy"));
        struct Pair { const char *check; const char *hint; const char *next; };
        const Pair pairs[] = {
            { "autoPreviewCheck", "autoPreviewHint", "encryptedPreviewCheck" },
            { "encryptedPreviewCheck", "encryptedPreviewHint", "inlineLinkMediaCheck" },
            { "gifProviderCombo", "gifProviderHint", nullptr },
        };
        for (const Pair &p : pairs) {
            auto *check = item(p.check);
            auto *hint = item(p.hint);
            QVERIFY2(check && hint, p.hint);
            QVERIFY2(!hint->property("text").toString().isEmpty(), p.hint);
            const qreal checkBottom = contentPosY(check) + check->height();
            QVERIFY2(contentPosY(hint) >= checkBottom - 1, p.hint);
            QVERIFY2(contentPosY(hint) - checkBottom < 24, p.hint);
            if (p.next) {
                QVERIFY2(contentPosY(item(p.next)) >= contentPosY(hint) + hint->height() - 1,
                         p.hint);
            }
        }
    }

    void aTypedPercentageDrivesTheSliderAndClamps()
    {
        openSection(QStringLiteral("appearance"));
        auto *settings = m_controller->settings();
        settings->setTextScale(100);
        QCoreApplication::processEvents();
        auto *slider = item("textScaleSlider");
        auto *box = item("textScalePercentBox");
        QVERIFY(slider && box);
        QCOMPARE(slider->property("from").toInt(), 75);
        QCOMPARE(slider->property("to").toInt(), 150);
        QCOMPARE(box->property("text").toString(), QStringLiteral("100"));

        const auto type = [&](const QString &text) {
            ensureVisible(box);
            const QPointF c = box->mapToScene(QPointF(box->width() / 2,
                                                      box->height() / 2));
            QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, c.toPoint());
            QVERIFY(box->hasActiveFocus());
            QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
            for (const QChar ch : text)
                QTest::keyClick(m_window, ch.toLatin1());
            QTest::keyClick(m_window, Qt::Key_Return);
            QCoreApplication::processEvents();
        };
        type(QStringLiteral("107"));
        QCOMPARE(settings->textScale(), 107);
        QCOMPARE(box->property("text").toString(), QStringLiteral("107"));
        type(QStringLiteral("400"));
        QCOMPARE(settings->textScale(), 150);
        type(QStringLiteral("10"));
        QCOMPARE(settings->textScale(), 75);
        // Nothing typed leaves the stored value alone and shows it.
        type(QString());
        QCOMPARE(settings->textScale(), 75);
        QCOMPARE(box->property("text").toString(), QStringLiteral("75"));
        // Interface zoom takes the same box.
        auto *zoomBox = item("interfaceZoomPercentBox");
        QVERIFY(zoomBox);
        QCOMPARE(zoomBox->property("text").toString(),
                 QString::number(settings->interfaceZoom()));
        settings->setTextScale(100);
    }

    // Dragging only moves the number; the setting (which relayouts the window)
    // is written on release.
    void aSliderDragCommitsOnRelease()
    {
        openSection(QStringLiteral("appearance"));
        auto *settings = m_controller->settings();
        settings->setTextScale(100);
        QCoreApplication::processEvents();
        auto *slider = item("textScaleSlider");
        auto *box = item("textScalePercentBox");
        QVERIFY(slider && box);
        ensureVisible(slider);
        auto *handle = slider->property("handle").value<QQuickItem *>();
        QVERIFY(handle);
        const QPointF from = handle->mapToScene(QPointF(handle->width() / 2,
                                                        handle->height() / 2));
        QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
        QTest::mouseMove(m_window, from.toPoint() + QPoint(40, 0));
        QTest::mouseMove(m_window, from.toPoint() + QPoint(80, 0));
        QCoreApplication::processEvents();
        QVERIFY(slider->property("pressed").toBool());
        const int draft = qRound(slider->property("value").toReal());
        QVERIFY2(draft > 100, qPrintable(QString::number(draft)));
        QCOMPARE(box->property("text").toString(), QString::number(draft));
        QCOMPARE(settings->textScale(), 100);   // not yet
        QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier,
                            (from.toPoint() + QPoint(80, 0)));
        QCoreApplication::processEvents();
        QCOMPARE(settings->textScale(), draft);
        settings->setTextScale(100);
    }

    void theAccessibilitySettingsReachTheThemeAndTheLinks()
    {
        openSection(QStringLiteral("appearance"));
        auto *settings = m_controller->settings();

        // Increase contrast: secondary and muted inks move toward the main
        // ink, borders strengthen, and turning it off restores them.
        settings->setHighContrast(false);
        QCoreApplication::processEvents();
        const QColor mutedBefore = themeValue(QStringLiteral("textMuted")).value<QColor>();
        const QColor borderBefore = themeValue(QStringLiteral("border")).value<QColor>();
        const QColor primary = themeValue(QStringLiteral("textPrimary")).value<QColor>();
        auto *contrastCheck = item("highContrastCheck");
        QVERIFY(contrastCheck);
        clickItem(contrastCheck);
        QVERIFY(settings->highContrast());
        QCoreApplication::processEvents();
        const QColor mutedAfter = themeValue(QStringLiteral("textMuted")).value<QColor>();
        const QColor borderAfter = themeValue(QStringLiteral("border")).value<QColor>();
        const auto distance = [](const QColor &a, const QColor &b) {
            return std::abs(a.redF() - b.redF()) + std::abs(a.greenF() - b.greenF())
                 + std::abs(a.blueF() - b.blueF());
        };
        QVERIFY2(distance(mutedAfter, primary) < distance(mutedBefore, primary),
                 "muted text did not move toward the primary ink");
        QVERIFY2(distance(borderAfter, primary) < distance(borderBefore, primary),
                 "borders did not strengthen");
        clickItem(contrastCheck);
        QVERIFY(!settings->highContrast());
        QCoreApplication::processEvents();
        QCOMPARE(themeValue(QStringLiteral("textMuted")).value<QColor>(), mutedBefore);
        QCOMPARE(themeValue(QStringLiteral("border")).value<QColor>(), borderBefore);

        // Always underline links.
        settings->setUnderlineLinks(false);
        QCoreApplication::processEvents();
        QCOMPARE(themeValue(QStringLiteral("underlineLinks")).toBool(), false);
        clickItem(item("underlineLinksCheck"));
        QVERIFY(settings->underlineLinks());
        QCoreApplication::processEvents();
        QCOMPARE(themeValue(QStringLiteral("underlineLinks")).toBool(), true);
        clickItem(item("underlineLinksCheck"));
        QVERIFY(!settings->underlineLinks());

        // Name colour readability: Strong 4.5, Weak 3, Off 1 and the colours
        // follow.
        QStringList users;
        for (int i = 0; i < 9; ++i)
            users << QStringLiteral("@user%1:mock.local").arg(i);
        const auto colours = [&]() {
            QList<QColor> out;
            for (const QString &u : users)
                out << themeValue(QStringLiteral("userColor(\"%1\")").arg(u))
                           .value<QColor>();
            return out;
        };
        clickItem(item("nameColorCorrectionControl_0"));
        QCOMPARE(settings->nameColorCorrection(), 0);
        QCoreApplication::processEvents();
        QCOMPARE(themeValue(QStringLiteral("nameColorMinContrast")).toDouble(), 4.5);
        const QList<QColor> strong = colours();
        clickItem(item("nameColorCorrectionControl_1"));
        QCOMPARE(settings->nameColorCorrection(), 1);
        QCoreApplication::processEvents();
        QCOMPARE(themeValue(QStringLiteral("nameColorMinContrast")).toDouble(), 3.0);
        const QList<QColor> weak = colours();
        clickItem(item("nameColorCorrectionControl_2"));
        QCOMPARE(settings->nameColorCorrection(), 2);
        QCoreApplication::processEvents();
        QCOMPARE(themeValue(QStringLiteral("nameColorMinContrast")).toDouble(), 1.0);
        const QList<QColor> off = colours();
        int differsWeak = 0, differsOff = 0;
        for (int i = 0; i < strong.size(); ++i) {
            differsWeak += strong.at(i) != weak.at(i);
            differsOff += strong.at(i) != off.at(i);
        }
        QVERIFY2(differsOff > 0,
                 "turning the correction off changed no sender colour");
        QVERIFY2(differsWeak + differsOff > 0, "Weak and Off equal Strong everywhere");
        settings->setNameColorCorrection(0);
        QCoreApplication::processEvents();
        QCOMPARE(colours(), strong);
    }

    void messageSpacingIsASettingTheThemeFollows()
    {
        openSection(QStringLiteral("appearance"));
        auto *settings = m_controller->settings();
        QCOMPARE(settings->messageSpacing(), 1);
        for (int mode : {0, 2, 1}) {
            clickItem(item(qPrintable(
                QStringLiteral("messageSpacingControl_%1").arg(mode))));
            QCOMPARE(settings->messageSpacing(), mode);
            QCoreApplication::processEvents();
            QCOMPARE(themeValue(QStringLiteral("messageSpacing")).toInt(), mode);
        }
        // Out of range reads back as Cozy, not the nearest end.
        settings->setMessageSpacing(7);
        QCOMPARE(settings->messageSpacing(), 1);
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
    // How many distinct pictures the editor's caret region shows over about a
    // second. A blinking caret shows at least two (drawn, absent); a steady one
    // shows one. Reads pixels, because the flash time on QStyleHints only says
    // what was REQUESTED, not that any editor stopped blinking.
    int distinctCaretFrames(QQuickItem *editor)
    {
        const QRectF cursor = editor->property("cursorRectangle").toRectF();
        const QPoint at = editor->mapToScene(cursor.topLeft()).toPoint();
        const QRect area(at, QSize(qMax(2, int(cursor.width()) + 2),
                                   qMax(4, int(cursor.height()))));
        // Let focus, the focus ring and layout settle first: a frame that
        // changes once for those reasons is not a blink (seen under load).
        QTest::qWait(500);
        QSet<QByteArray> seen;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 1000) {
            const QImage frame = m_window->grabWindow().copy(area);
            seen.insert(QByteArray(reinterpret_cast<const char *>(frame.constBits()),
                                   frame.sizeInBytes()));
            QTest::qWait(30);
        }
        return seen.size();
    }

    void cursorBlinkPreferenceAppliesImmediatelyAndPersists()
    {
        auto *settings = m_controller->settings();
        auto *hints = QGuiApplication::styleHints();
        const bool original = settings->disableCursorBlink();
        const int originalTime = hints->cursorFlashTime();
        const QString originalMode = settings->composerMode();
        const auto restore = qScopeGuard([&] {
            settings->setDisableCursorBlink(original);
            settings->setComposerMode(originalMode);
            hints->setCursorFlashTime(originalTime);
        });
        settings->setDisableCursorBlink(false);
        hints->setCursorFlashTime(-1);
        const int systemTime = hints->cursorFlashTime();
        openSection(QStringLiteral("appearance"));
        auto *check = item("disableCursorBlinkCheck");
        QVERIFY(check);
        QVERIFY(!check->property("checked").toBool());
        clickItem(check);
        QTRY_VERIFY(settings->disableCursorBlink());
        QCOMPARE(hints->cursorFlashTime(), 0);
        SettingsManager saved;
        QVERIFY(saved.disableCursorBlink());
        m_controller->showMain();
        for (const auto &mode : { QStringLiteral("markdown"), QStringLiteral("rich") }) {
            settings->setComposerMode(mode);
            auto *editor = item(mode == QStringLiteral("rich")
                                    ? "composerRichInput" : "composerInput");
            QVERIFY(editor);
            editor->forceActiveFocus();
            QTRY_VERIFY(editor->hasActiveFocus());
            QCOMPARE(hints->cursorFlashTime(), 0);
            // The editor itself stopped blinking, not only the hint.
            QCOMPARE(distinctCaretFrames(editor), 1);
            // Control: the same editor blinks once a flash time is back.
            // Without it a steady picture could just be a caret that never
            // draws, and the assertion above would pass on anything.
            hints->setCursorFlashTime(200);
            editor->setProperty("focus", false);
            editor->forceActiveFocus();
            QTRY_VERIFY(editor->hasActiveFocus());
            QVERIFY2(distinctCaretFrames(editor) >= 2,
                     "the control editor never blinks, so the steadiness "
                     "assertion above proves nothing");
            hints->setCursorFlashTime(0);
        }
        openSection(QStringLiteral("appearance"));
        clickItem(check);
        QTRY_VERIFY(!settings->disableCursorBlink());
        QCOMPARE(hints->cursorFlashTime(), systemTime);
        SettingsManager restored;
        QVERIFY(!restored.disableCursorBlink());
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    SettingsShellQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "SettingsShellQmlTest.moc"
