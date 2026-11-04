#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/FontManager.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"

namespace {
constexpr int kSignalTimeoutMs = 10'000;
}

// Drives the real Interface and Code font combos in Settings -> Appearance,
// wired to a real FontManager the way main.cpp wires it.
class FontPickerFilterQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
    AppController *m_controller = nullptr;
    FontManager *m_fonts = nullptr;
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
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    struct Combo
    {
        QQuickItem *combo = nullptr;
        QObject *popup = nullptr;
        QQuickItem *field = nullptr;
        QQuickItem *list = nullptr;
    };

    Combo combo(const char *name) const
    {
        Combo c;
        c.combo = item(name);
        if (!c.combo)
            return c;
        c.popup = c.combo->property("popup").value<QObject *>();
        auto *content = c.popup
            ? c.popup->property("contentItem").value<QQuickItem *>() : nullptr;
        c.field = findItem(content, QStringLiteral("comboFilterField"));
        c.list = findItem(content, QStringLiteral("comboFilteredList"));
        return c;
    }

    void ensureVisible(QQuickItem *target)
    {
        QQuickItem *flick = target->parentItem();
        while (flick && !flick->inherits("QQuickFlickable"))
            flick = flick->parentItem();
        if (!flick)
            return;
        auto *content = flick->property("contentItem").value<QQuickItem *>();
        const qreal top = target->mapToItem(content, QPointF(0, 0)).y();
        const qreal maxY = qMax(0.0, flick->property("contentHeight").toReal()
                                         - flick->height());
        flick->setProperty("contentY", qBound(0.0, top - 120, maxY));
        QCoreApplication::processEvents();
    }

    bool open(const Combo &c)
    {
        const auto centre = [&] {
            return c.combo->mapToScene(
                QPointF(c.combo->width() / 2, c.combo->height() / 2)).toPoint();
        };
        // The page above keeps growing while it lays out, so scroll until the
        // combo stays inside the window.
        const bool onScreen = QTest::qWaitFor([&] {
            ensureVisible(c.combo);
            const QPoint p = centre();
            return p.y() > 0 && p.y() < m_window->height() - 40;
        }, 5000);
        if (!onScreen)
            return false;
        const QPoint at = centre();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, at);
        return QTest::qWaitFor([&] {
            return c.popup->property("visible").toBool() && c.field->hasActiveFocus();
        }, 3000);
    }

    void type(const QString &text)
    {
        for (const QChar ch : text)
            QTest::keyClick(m_window, ch.toLatin1());
    }

    // A family other than the current one whose whole name, typed, matches
    // only itself, and that contains a space when one is available so the
    // Space key is exercised.
    static QString pickTarget(const QStringList &families, const QString &current)
    {
        QString fallback;
        for (const QString &f : families) {
            if (f == current || f.isEmpty())
                continue;
            bool ascii = true;
            for (const QChar ch : f)
                ascii = ascii && ch.unicode() >= 0x20 && ch.unicode() < 0x7f;
            if (!ascii)
                continue;
            int matches = 0;
            for (const QString &g : families)
                matches += g.contains(f, Qt::CaseInsensitive) ? 1 : 0;
            if (matches != 1)
                continue;
            if (f.contains(QLatin1Char(' ')))
                return f;
            if (fallback.isEmpty())
                fallback = f;
        }
        return fallback;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("font-picker-filter-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_fonts = new FontManager(m_controller->settings());
        m_engine = new QQmlApplicationEngine;
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings) {
                        const QString text = w.toString();
                        if (!text.contains(QLatin1String("Host mock.local not found")))
                            m_warnings.append(text);
                    }
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), m_controller);
        m_engine->rootContext()->setContextProperty(QStringLiteral("fonts"), m_fonts);
        QSignalSpy created(m_engine, &QQmlApplicationEngine::objectCreated);
        m_engine->loadFromModule(QStringLiteral("MatrixClient"), QStringLiteral("Main"));
        if (created.isEmpty())
            QVERIFY(created.wait(kSignalTimeoutMs));
        m_window = qobject_cast<QQuickWindow *>(created.at(0).at(0).value<QObject *>());
        QVERIFY(m_window);
        m_window->resize(1374, 944);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));

        QSignalSpy login(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(login.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());

        m_controller->showSettings();
        m_controller->showSettingsSection(QStringLiteral("appearance"));
        QTRY_VERIFY(item("systemUiFontCombo") && item("systemUiFontCombo")->isVisible());
    }

    void cleanupTestCase()
    {
        delete m_engine;
        delete m_fonts;
        delete m_controller;
    }

    void bothFontCombosOfferASearchField()
    {
        for (const char *name : {"systemUiFontCombo", "monoFontCombo"}) {
            const Combo c = combo(name);
            QVERIFY2(c.combo && c.popup && c.field && c.list, name);
            QVERIFY2(c.combo->property("filterable").toBool(), name);
            QVERIFY(open(c));
            QCOMPARE(c.field->property("placeholderText").toString(),
                     QStringLiteral("Search fonts"));
            QCOMPARE(c.list->property("count").toInt(), c.combo->property("count").toInt());
            QTest::keyClick(m_window, Qt::Key_Escape);
            QTRY_VERIFY(!c.popup->property("visible").toBool());
        }
    }

    void typingAFamilyNamePicksItAndSavesIt()
    {
        const Combo c = combo("systemUiFontCombo");
        QVERIFY(c.combo);
        const QString before = m_fonts->storedUiFamily();
        const QString target = pickTarget(m_fonts->uiFamilies(), before);
        if (target.isEmpty())
            QSKIP("no installed UI family with a unique ASCII name");

        QVERIFY(open(c));
        type(target.toLower());
        QTRY_COMPARE(c.list->property("count").toInt(), 1);
        QCOMPARE(c.field->property("text").toString(), target.toLower());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(!c.popup->property("visible").toBool());

        QTRY_COMPARE(m_fonts->storedUiFamily(), target);
        QCOMPARE(m_controller->settings()->uiFont(), target);
        QCOMPARE(c.combo->property("currentText").toString(), target);
        m_fonts->setUiFamily(before);
    }

    void escapeAfterTypingLeavesTheFontAlone()
    {
        const Combo c = combo("systemUiFontCombo");
        QVERIFY(c.combo);
        const QString before = m_fonts->storedUiFamily();
        const int index = c.combo->property("currentIndex").toInt();
        QVERIFY(open(c));
        type(QStringLiteral("a"));
        QTRY_VERIFY(c.list->property("count").toInt() > 0);
        QTest::keyClick(m_window, Qt::Key_Down);
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!c.popup->property("visible").toBool());
        QCOMPARE(m_fonts->storedUiFamily(), before);
        QCOMPARE(c.combo->property("currentIndex").toInt(), index);
    }

    void theCodeFontComboFiltersMonospaceFamilies()
    {
        const Combo c = combo("monoFontCombo");
        QVERIFY(c.combo);
        const QString before = m_fonts->storedMonospaceFamily();
        const QString target = pickTarget(m_fonts->monospaceFamilies(), before);
        if (target.isEmpty())
            QSKIP("no installed monospace family with a unique ASCII name");

        QVERIFY(open(c));
        type(target);
        QTRY_COMPARE(c.list->property("count").toInt(), 1);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(!c.popup->property("visible").toBool());
        QTRY_COMPARE(m_fonts->storedMonospaceFamily(), target);
        QCOMPARE(m_controller->settings()->monoFont(), target);
        m_fonts->setMonospaceFamily(before);
    }

    void noQmlWarnings()
    {
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(FontPickerFilterQmlTest)
#include "FontPickerFilterQmlTest.moc"
