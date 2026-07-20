// In-shell Settings proof (correction spec §3): opening Settings replaces
// ONLY the timeline region — the spaces rail and room list stay visible and
// instantiated, the rail gear shows the accent-chip state, the header reads
// "Settings — Appearance" with a bare close X, the three featured theme
// cards paint their fixed preview palettes regardless of the active theme,
// clicking a card switches the theme instantly, the match-system row and
// message-layout chips drive the real settings backend, and closing restores
// the chat view. Loads the production MainScreen against the mock backend
// with a zero-QML-warning contract.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

#include "app/AppController.h"
#include "app/SettingsManager.h"

namespace {

QColor sampleAvg(const QImage &img, const QRect &r)
{
    qint64 red = 0, green = 0, blue = 0, n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        for (int x = r.left(); x <= r.right(); ++x) {
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

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 1280
    height: 800
    visible: true
    color: AppTheme.background

    // Mirror Main.qml's production bindings: the selected theme and text
    // scale drive the AppTheme singleton.
    Binding {
        target: AppTheme
        property: "mode"
        value: app.settings ? app.settings.theme : 0
    }
    Binding {
        target: AppTheme
        property: "textScale"
        value: app.settings ? app.settings.textScale / 100 : 1
    }

    Rectangle { objectName: "tokAccentSoft"; visible: false; color: AppTheme.accentSoft }
    Rectangle { objectName: "tokAccent"; visible: false; color: AppTheme.accent }

    MainScreen {
        objectName: "mainScreen"
        anchors.fill: parent
    }
}
)QML";

} // namespace

class SettingsShellQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
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
        if (auto *hit = m_root->findChild<QQuickItem *>(QLatin1String(name)))
            return hit;
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    QColor token(const char *name) const
    {
        auto *it = m_root->findChild<QQuickItem *>(QLatin1String(name));
        return it ? it->property("color").value<QColor>() : QColor();
    }

    void clickItem(QQuickItem *target)
    {
        const QPointF center = target->mapToScene(
            QPointF(target->width() / 2, target->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          center.toPoint());
        QCoreApplication::processEvents();
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
        m_engine = new QQmlEngine(this);
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("settingsshell.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QCoreApplication::processEvents();

        // Sign the mock account in so the shell behaves as in production
        // (the rail gear and Settings are logged-in surfaces).
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QTRY_VERIFY(m_controller->loggedIn());
        QCoreApplication::processEvents();
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void settingsOpensInsideShellNotOverIt()
    {
        auto *timeline = item("timelinePane");
        auto *loader = m_root->findChild<QQuickItem *>(
            QStringLiteral("settingsPaneLoader"));
        QVERIFY(timeline && loader);
        QVERIFY(timeline->isVisible());
        QVERIFY(!loader->property("active").toBool());

        m_controller->showSettings();
        QCoreApplication::processEvents();

        // Rail and room list survive; only the timeline region swaps.
        QVERIFY(item("spacesRail"));
        QVERIFY(item("spacesRail")->isVisible());
        QVERIFY(item("roomsPanel"));
        QVERIFY(item("roomsPanel")->isVisible());
        QVERIFY(!timeline->isVisible());
        QVERIFY(loader->property("active").toBool());
        // The timeline stays instantiated so closing restores it exactly.
        QVERIFY(timeline);
    }

    void headerReadsSettingsAppearanceWithBareClose()
    {
        auto *title = item("settingsHeaderTitle");
        QVERIFY(title);
        QCOMPARE(title->property("text").toString(),
                 QStringLiteral("Settings — Appearance"));
        auto *close = item("settingsCloseButton");
        QVERIFY(close);
        QCOMPARE(close->width(), 34.0);
        QCOMPARE(close->height(), 34.0);
        QCOMPARE(close->property("radius").toInt(), 8);
        QCOMPARE(close->property("fill").toBool(), false);
        QCOMPARE(close->property("active").toBool(), false);
    }

    void railGearShowsAccentChipWhileOpen()
    {
        auto *gear = item("railSettingsButton");
        QVERIFY(gear);
        QVERIFY(gear->property("active").toBool());
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
            // Sample below the mini bars, inside the frame area.
            const QPointF framePoint = preview->mapToScene(
                QPointF(preview->width() - 14, preview->height() - 8));
            QVERIFY2(channelDelta(sampleAvg(img,
                          QRect(int(framePoint.x()), int(framePoint.y()), 2, 2)),
                          e.frame) <= kTolerance, e.preview);
            auto *accentBar = item(e.accentBar);
            QVERIFY2(accentBar, e.accentBar);
            const QPointF accentPoint = accentBar->mapToScene(
                QPointF(accentBar->width() / 2, accentBar->height() / 2));
            const QColor sampled = sampleAvg(img,
                QRect(int(accentPoint.x()), int(accentPoint.y()) - 1, 2, 2));
            QVERIFY2(channelDelta(sampled, e.accent) <= kTolerance,
                     qPrintable(QStringLiteral("%1 w=%2 sampled=%3 expected=%4")
                                    .arg(QLatin1String(e.accentBar))
                                    .arg(accentBar->width())
                                    .arg(sampled.name(), e.accent.name())));
        }
    }

    void clickingThemeCardSwitchesInstantly()
    {
        const QColor before = token("tokAccent");
        auto *tealCard = item("featuredThemeCard_10");
        QVERIFY(tealCard);
        clickItem(tealCard);
        QCOMPARE(int(m_controller->settings()->theme()), 10);
        // The AppTheme singleton follows immediately — no apply button.
        QTRY_VERIFY(token("tokAccent") != before);

        auto *mossCard = item("featuredThemeCard_8");
        QVERIFY(mossCard);
        clickItem(mossCard);
        QCOMPARE(int(m_controller->settings()->theme()), 8);
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

    void messageLayoutChipsDriveTheBackend()
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

    void textScaleSliderTracksAndScalesText()
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

    void closeRestoresChatAndGearDeactivates()
    {
        auto *close = item("settingsCloseButton");
        QVERIFY(close);
        clickItem(close);
        QCOMPARE(int(m_controller->currentScreen()),
                 int(AppController::MainScreen));
        auto *timeline = item("timelinePane");
        QVERIFY(timeline);
        QTRY_VERIFY(timeline->isVisible());
        auto *gear = item("railSettingsButton");
        QVERIFY(gear);
        QVERIFY(!gear->property("active").toBool());
        // Clicking the gear again reopens; a second click returns to chat.
        clickItem(gear);
        QCOMPARE(int(m_controller->currentScreen()),
                 int(AppController::SettingsScreen));
        clickItem(gear);
        QCOMPARE(int(m_controller->currentScreen()),
                 int(AppController::MainScreen));
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
