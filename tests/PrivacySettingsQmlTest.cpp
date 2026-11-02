
#include <QtTest/QtTest>

#include <cmath>

#include <QColor>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/ModerationController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"

namespace {

constexpr int kSignalTimeoutMs = 5000;

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

}

class PrivacySettingsQmlTest : public QObject
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
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        }
        return nullptr;
    }

    QQuickItem *item(const char *name) const
    {
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    static void collectByText(QQuickItem *parent, const QString &text,
                              QList<QQuickItem *> &out)
    {
        if (!parent)
            return;
        if (parent->property("text").toString() == text
            && parent->inherits("QQuickAbstractButton")) {
            out.append(parent);
        }
        const auto children = parent->childItems();
        for (QQuickItem *child : children)
            collectByText(child, text, out);
    }

    QList<QQuickItem *> stopButtons() const
    {
        QList<QQuickItem *> all;
        collectByText(item("ignoredUsersCard"),
                      QStringLiteral("Stop ignoring"), all);
        QList<QQuickItem *> out;
        for (QQuickItem *b : all) {
            if (b->isVisible())
                out.append(b);
        }
        return out;
    }

    static void countVisibleNamed(QQuickItem *parent, const QString &name,
                                  int &n)
    {
        if (!parent)
            return;
        if (parent->objectName() == name && parent->isVisible())
            ++n;
        const auto kids = parent->childItems();
        for (QQuickItem *k : kids)
            countVisibleNamed(k, name, n);
    }

    int goneLines() const
    {
        int n = 0;
        countVisibleNamed(item("ignoredUsersCard"),
                          QStringLiteral("ignoredRowGone"), n);
        return n;
    }

    static QColor cardColorOf(QQuickItem *inner)
    {
        for (QQuickItem *p = inner ? inner->parentItem() : nullptr; p;
             p = p->parentItem()) {
            const auto kids = p->childItems();
            for (QQuickItem *k : kids) {
                if (k->objectName() == QLatin1String("settingsCardSurface"))
                    return k->property("color").value<QColor>();
            }
        }
        return QColor();
    }

    void activateWindow() const
    {
        m_window->requestActivate();
        QTRY_VERIFY_WITH_TIMEOUT(m_window->isActive(), 3000);
    }

    static QQuickItem *ringOf(QQuickItem *check)
    {
        return check ? findItem(check, QStringLiteral("checkFocusRing"))
                     : nullptr;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("privacy-settings-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlApplicationEngine;
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
        m_window->setWidth(1374);
        m_window->setHeight(944);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));

        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());
        QCoreApplication::processEvents();
    }

    void cleanupTestCase()
    {
        delete m_engine;
        delete m_controller;
    }

    void aDoubleClickOnStopIgnoringRemovesExactlyOneUser()
    {
        auto *moderation = m_controller->moderation();
        QVERIFY(moderation);
        const QStringList ids = { QStringLiteral("@aa:example.org"),
                                  QStringLiteral("@bb:example.org"),
                                  QStringLiteral("@cc:example.org") };
        for (const QString &id : ids) {
            QTRY_VERIFY_WITH_TIMEOUT(moderation->supported(), 3000);
            QTRY_VERIFY_WITH_TIMEOUT(!moderation->busy(), 3000);
            moderation->ignoreUser(id);
            QTRY_VERIFY_WITH_TIMEOUT(
                moderation->ignoredUsers().contains(id), 3000);
        }
        QCOMPARE(moderation->ignoredUsers().size(), 3);

        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(stopButtons().size(), 3, 3000);

        QQuickItem *first = stopButtons().at(0);
        const QPoint where = first->mapToScene(
            QPointF(first->width() / 2, first->height() / 2)).toPoint();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, where);
        QTRY_VERIFY_WITH_TIMEOUT(!moderation->ignoredUsers().contains(ids[0]),
                                 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!moderation->busy(), 3000);
        QCoreApplication::processEvents();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, where);
        QTest::qWait(150);
        QCoreApplication::processEvents();

        QVERIFY2(moderation->ignoredUsers().contains(ids[1])
                     && moderation->ignoredUsers().contains(ids[2]),
                 qPrintable(QStringLiteral(
                     "a double-click on 'Stop ignoring' also unignored the "
                     "next user; still ignored: %1")
                     .arg(moderation->ignoredUsers().join(QLatin1Char(',')))));
        QCOMPARE(moderation->ignoredUsers().size(), 2);

        QCOMPARE(stopButtons().size(), 2);
        QCOMPARE(goneLines(), 1);

        moderation->unignoreUser(ids[1]);
        QTRY_VERIFY_WITH_TIMEOUT(!moderation->busy(), 3000);
        moderation->unignoreUser(ids[2]);
        QTRY_VERIFY_WITH_TIMEOUT(!moderation->busy(), 3000);
    }

    void unignoringTheLastUserLeavesTheEmptyStateAfterReopening()
    {
        auto *moderation = m_controller->moderation();
        QVERIFY(moderation);
        const QString id = QStringLiteral("@only:example.org");
        QTRY_VERIFY_WITH_TIMEOUT(!moderation->busy(), 3000);
        QVERIFY(m_controller->showMain());
        QCoreApplication::processEvents();
        moderation->ignoreUser(id);
        QTRY_VERIFY_WITH_TIMEOUT(moderation->ignoredUsers().contains(id), 3000);
        QCOMPARE(moderation->ignoredUsers().size(), 1);

        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QTRY_COMPARE_WITH_TIMEOUT(stopButtons().size(), 1, 3000);
        QQuickItem *btn = stopButtons().at(0);
        const QPoint where = btn->mapToScene(
            QPointF(btn->width() / 2, btn->height() / 2)).toPoint();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, where);
        QTRY_VERIFY_WITH_TIMEOUT(moderation->ignoredUsers().isEmpty(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!moderation->busy(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(goneLines(), 1, 3000);
        QQuickItem *empty = item("ignoredEmptyState");
        QVERIFY2(empty, "the empty-state label is not named");
        QVERIFY(empty->isVisible());

        QVERIFY(m_controller->showMain());
        QCoreApplication::processEvents();
        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(goneLines(), 0, 3000);
        QVERIFY(item("ignoredEmptyState")->isVisible());
        QCOMPARE(stopButtons().size(), 0);

        moderation->ignoreUser(id);
        QTRY_VERIFY_WITH_TIMEOUT(moderation->ignoredUsers().contains(id), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(stopButtons().size(), 1, 3000);
        btn = stopButtons().at(0);
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          btn->mapToScene(QPointF(btn->width() / 2,
                                                  btn->height() / 2)).toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(moderation->ignoredUsers().isEmpty(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(goneLines(), 1, 3000);
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QCoreApplication::processEvents();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(goneLines(), 0, 3000);
    }

    void theCheckboxFocusRingClearsThreeToOneOnEveryTheme()
    {
        const int restore = m_controller->settings()->theme();
        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        QCoreApplication::processEvents();

        const char *names[] = { "sendTypingCheck", "sharePresenceCheck",
                                "strictDeviceTrustCheck" };
        activateWindow();
        int focusTested = 0;

        for (const char *name : names) {
            auto *check = item(name);
            QVERIFY2(check, name);
            QQuickItem *ring = ringOf(check);
            QVERIFY2(ring, qPrintable(QStringLiteral(
                "%1 has no focus ring of its own").arg(QLatin1String(name))));
            QVERIFY(!ring->isVisible());
            // The presence card is hidden on a backend without presence (the
            // mock): nothing there can take focus, but its ring ink is still
            // checked below.
            if (!check->isVisible())
                continue;
            ++focusTested;
            check->forceActiveFocus(Qt::TabFocusReason);
            QCoreApplication::processEvents();
            QVERIFY2(check->property("visualFocus").toBool()
                         && ring->isVisible(),
                     qPrintable(QStringLiteral(
                         "%1 shows no ring while keyboard-focused")
                         .arg(QLatin1String(name))));
            check->setFocus(false);
        }

        QVERIFY2(focusTested >= 2, "fewer than two checkboxes were focusable");
        int checked = 0;
        for (int id = 1; id <= 11; ++id) {
            m_controller->settings()->setTheme(
                static_cast<SettingsManager::Theme>(id));
            QCoreApplication::processEvents();
            for (const char *name : names) {
                QQuickItem *ring = ringOf(item(name));
                QVERIFY(ring);
                const QColor fill = cardColorOf(item(name));
                QVERIFY2(fill.isValid(), name);
                const QColor ink =
                    QQmlProperty(ring, QStringLiteral("border.color"))
                        .read().value<QColor>();
                QVERIFY(ink.isValid());
                const double ratio = contrastRatio(ink, fill);
                QVERIFY2(ratio >= 3.0,
                         qPrintable(QStringLiteral(
                             "theme %1: the %2 focus ring %3 on the card %4 "
                             "is %5:1, under 3:1")
                             .arg(id).arg(QLatin1String(name), ink.name(),
                                          fill.name())
                             .arg(ratio, 0, 'f', 2)));
                qInfo("theme %d %s ring %s card %s ratio %.2f", id, name,
                      qPrintable(ink.name()), qPrintable(fill.name()), ratio);
            }
            ++checked;
        }
        QCOMPARE(checked, 11);

        m_controller->settings()->setTheme(
            static_cast<SettingsManager::Theme>(restore));
        QCoreApplication::processEvents();
    }

    void theDeviceTrustLabelWrapsInsideTheMinimumWindow()
    {
        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("privacy"));
        m_window->setWidth(640);
        m_window->setHeight(420);
        QTest::qWait(100);
        QCoreApplication::processEvents();

        auto *label = item("strictDeviceTrustLabel");
        QVERIFY2(label, "the Device trust checkbox has no wrapping label");
        const qreal right =
            label->mapToScene(QPointF(label->width(), 0)).x();
        QVERIFY2(right <= m_window->width(),
                 qPrintable(QStringLiteral(
                     "the Device trust label ends at x=%1 in a %2 px window")
                     .arg(right).arg(m_window->width())));
        QVERIFY2(label->property("lineCount").toInt() >= 1,
                 "the Device trust label has no text");
        QVERIFY2(!label->property("truncated").toBool(),
                 "the Device trust label is cut off at 640 px");

        m_window->setWidth(1374);
        m_window->setHeight(944);
        QCoreApplication::processEvents();
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    PrivacySettingsQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "PrivacySettingsQmlTest.moc"
