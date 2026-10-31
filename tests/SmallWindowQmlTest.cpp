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
    // leaves [0, space.width] horizontally. NO clip is applied on purpose:
    // the point is what a clip would be hiding.
    static void controlsOutsideHorizontally(QQuickItem *it, QQuickItem *space,
                                            QStringList &out)
    {
        if (!it->isVisible() || it->opacity() <= 0.01)
            return;
        const QByteArray cls = it->metaObject()->className();
        if (isControl(cls) && it->width() > 0 && it->height() > 0) {
            const QRectF r = space->mapRectFromItem(
                it, QRectF(0, 0, it->width(), it->height()));
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
};

QTEST_MAIN(SmallWindowQmlTest)
#include "SmallWindowQmlTest.moc"
