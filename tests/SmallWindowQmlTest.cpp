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
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "auth/AuthManager.h"

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
        m_window->setHeight(420);
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
        QCOMPARE(m_window->minimumHeight(), 420);
    }

    void everySettingsSectionStaysInsideItsViewportAtTheFloor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x420") << QSize(640, 420);
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
        m_window->resize(640, 420);
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
        m_window->resize(640, 420);
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

    // The find bar was one row of seven controls in a card 288 px wide at the
    // floor: the field was squeezed away and Close sat outside the card.
    void theFindBarKeepsEveryControlInsideItsCardAtTheFloor_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("640x420") << QSize(640, 420);
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
};

QTEST_MAIN(SmallWindowQmlTest)
#include "SmallWindowQmlTest.moc"
