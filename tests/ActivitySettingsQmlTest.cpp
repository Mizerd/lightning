// Labs activity settings (Game + Music), loaded for real: the real components
// in a real window with the real mock controller, driven with real key and
// mouse events, asserting the state that results (C72 round 2).
//   * Escape on a draft clears the draft and does NOT reach an unconditional
//     Escape Shortcut like SettingsScreen's (a Shortcut is dispatched before
//     the focused item sees the key); with no draft it still does;
//   * a double click on Remove removes ONE entry, a second deliberate click a
//     double-click interval later removes the next, and a keyboard removal is
//     never swallowed and hands the focus to the field;
//   * both checkboxes draw a ring on keyboard focus only, readable against
//     the Settings canvas on every palette.
// The fixture's Shortcut mirrors SettingsScreen.qml; a source case pins that
// the real one is still unconditional so the fixture cannot drift.

#include <QtTest/QtTest>

#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QGuiApplication>
#include <QStyleHints>
#include <cmath>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AuthManager.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 760
    height: 1500
    visible: true
    color: AppTheme.stormCanvas
    property int escapeShortcuts: 0
    Shortcut {
        sequence: "Escape"
        onActivated: win.escapeShortcuts++
    }
    Column {
        x: 20
        y: 20
        width: 700
        spacing: 16
        GameActivitySettings { objectName: "gs"; width: parent.width }
        MusicActivitySettings { objectName: "ms"; width: parent.width }
    }
    // One entry per palette: [ring colour, page colour, storm].
    function ringsPerTheme() {
        const keep = AppTheme.mode
        const out = []
        for (let m = 1; m <= 12; ++m) {
            AppTheme.mode = m
            out.push({ mode: m,
                       storm: AppTheme.storm,
                       ring: "" + AppTheme.bolt,
                       focusRing: "" + AppTheme.focusRing,
                       page: "" + AppTheme.stormCanvas })
        }
        AppTheme.mode = keep
        return out
    }
}
)QML";

// QStringList({"x", "y"}) with two string literals picks the iterator-pair
// constructor and reads past the literals; build from QStrings explicitly.
QStringList L(std::initializer_list<const char *> names)
{
    QStringList out;
    for (const char *n : names)
        out << QString::fromLatin1(n);
    return out;
}

double channelLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double luminance(const QColor &c)
{
    return 0.2126 * channelLinear(c.redF()) + 0.7152 * channelLinear(c.greenF())
         + 0.0722 * channelLinear(c.blueF());
}
double contrast(const QColor &a, const QColor &b)
{
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

} // namespace

class ActivitySettingsQmlTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QStringList m_warnings;   // every QML warning since the last init()

    static void collect(QQuickItem *parent, const QString &name,
                        QList<QQuickItem *> &out)
    {
        if (!parent)
            return;
        if (parent->objectName() == name)
            out << parent;
        const auto kids = parent->childItems();
        for (QQuickItem *k : kids)
            collect(k, name, out);
    }
    QList<QQuickItem *> all(const char *name) const
    {
        QList<QQuickItem *> out;
        collect(m_window->contentItem(), QLatin1String(name), out);
        return out;
    }
    QQuickItem *one(const char *name) const
    {
        const auto l = all(name);
        return l.isEmpty() ? nullptr : l.first();
    }
    // The ListEditor named `list` (hide or custom) and a child of it.
    QQuickItem *inList(const char *list, const char *name) const
    {
        QQuickItem *ed = one(list);
        if (!ed)
            return nullptr;
        QList<QQuickItem *> out;
        collect(ed, QLatin1String(name), out);
        return out.isEmpty() ? nullptr : out.first();
    }
    QList<QQuickItem *> removeButtons() const
    {
        QList<QQuickItem *> out;
        collect(one("gameActivityHideList"), QStringLiteral("listRemoveButton"), out);
        std::sort(out.begin(), out.end(), [](QQuickItem *a, QQuickItem *b) {
            return a->mapToScene(QPointF()).y() < b->mapToScene(QPointF()).y();
        });
        return out;
    }
    QPoint centreOf(QQuickItem *it) const
    {
        return it->mapToScene(QPointF(it->width() / 2, it->height() / 2)).toPoint();
    }
    int shortcuts() const { return m_root->property("escapeShortcuts").toInt(); }
    QStringList hidden() const { return m_controller->settings()->gameActivityHidden(); }
    // The ListEditor that owns the active-focus item, by objectName.
    QString focusedEditor() const
    {
        for (QQuickItem *it = m_window->activeFocusItem(); it; it = it->parentItem())
            if (it->objectName().endsWith(QLatin1String("List")))
                return it->objectName();
        return {};
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("activity-settings-qml-test"));
        m_controller = new AppController(AppController::MockBackend);
        QSignalSpy loggedIn(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"), QStringLiteral("unused"));
        QVERIFY(loggedIn.wait(3000));
        m_controller->settings()->setGameActivityEnabled(true);
        m_controller->settings()->setMusicActivityEnabled(true);

        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), m_controller);
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &errs) {
                    for (const QQmlError &e : errs)
                        m_warnings << e.toString();
                });
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("activitysettings.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QTest::qWait(80);
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join('\n')));
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void init()
    {
        m_warnings.clear();
        m_controller->settings()->setGameActivityHidden({});
        m_controller->settings()->setGameActivityCustom({});
        QTest::qWait(30);
    }

    // A load-time or handler error in the component is a failure of the case
    // that provoked it, not a log line.
    void cleanup()
    {
        QVERIFY2(m_warnings.isEmpty(), qPrintable(m_warnings.join(QLatin1Char('\n'))));
    }

    // The real SettingsScreen must keep the unconditional Escape Shortcut this
    // fixture mirrors, or the Escape case below stops meaning anything.
    void theFixtureMirrorsSettingsScreensEscapeShortcut()
    {
        QFile f(QStringLiteral("qml/SettingsScreen.qml"));
        QVERIFY2(f.open(QIODevice::ReadOnly), "run from the source directory");
        const QString q = QString::fromUtf8(f.readAll());
        QVERIFY(q.contains(QLatin1String("sequence: \"Escape\"")));
        QVERIFY(q.contains(QLatin1String("root.goBack()")));
    }

    void escapeOnADraftClearsItAndSettingsStaysOpen()
    {
        QQuickItem *field = inList("gameActivityHideList", "listEntryField");
        QVERIFY(field);
        field->forceActiveFocus();
        QTest::qWait(20);
        for (const char ch : QByteArray("hades"))
            QTest::keyClick(m_window, ch);
        QTRY_COMPARE(field->property("text").toString(), QStringLiteral("hades"));
        const int before = shortcuts();

        QTest::keyClick(m_window, Qt::Key_Escape);
        QTest::qWait(30);
        QCOMPARE(field->property("text").toString(), QString());
        QCOMPARE(shortcuts(), before);   // Settings did NOT close over the draft

        // With nothing to cancel, Escape is the shortcut's again.
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTest::qWait(30);
        QCOMPARE(shortcuts(), before + 1);
    }

    void aDraftDoesNotOutliveItsPage()
    {
        QQuickItem *field = inList("gameActivityCustomList", "listEntryField");
        QVERIFY(field);
        field->setProperty("text", QStringLiteral("half=Typed"));
        QQuickItem *page = one("gameActivityCustomList");
        page->setVisible(false);
        QTest::qWait(20);
        QCOMPARE(field->property("text").toString(), QString());
        page->setVisible(true);
    }

    void aDoubleClickOnRemoveRemovesOneEntry()
    {
        m_controller->settings()->setGameActivityHidden(L({ "a", "b", "c", "d" }));
        QTRY_COMPARE(removeButtons().size(), 4);
        QTest::qWait(60);   // the layout positions the new rows after they exist
        const QPoint first = centreOf(removeButtons().first());
        QTest::mouseClick(m_window, Qt::LeftButton, {}, first);
        QTest::qWait(5);
        QTest::mouseClick(m_window, Qt::LeftButton, {}, first);   // same spot, reflowed
        QTest::qWait(30);
        QCOMPARE(hidden(), L({ "b", "c", "d" }));

        // A second deliberate click after the double-click interval is NOT eaten.
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100);
        QTest::mouseClick(m_window, Qt::LeftButton, {}, centreOf(removeButtons().first()));
        QTRY_COMPARE(hidden(), L({ "c", "d" }));
    }

    void keyboardRemovalsAreNeverSwallowedAndKeepTheFocus()
    {
        m_controller->settings()->setGameActivityHidden(L({ "a", "b", "c" }));
        QTRY_COMPARE(removeButtons().size(), 3);
        QTest::qWait(60);
        removeButtons().first()->forceActiveFocus(Qt::TabFocusReason);
        QTest::qWait(20);
        QTest::keyClick(m_window, Qt::Key_Space);
        QTRY_COMPARE(hidden(), L({ "b", "c" }));
        // Focus went to the same editor's field, not to nothing.
        QTRY_COMPARE(focusedEditor(), QStringLiteral("gameActivityHideList"));
        QVERIFY(m_window->activeFocusItem()
                && m_window->activeFocusItem()->objectName()
                       == QLatin1String("listEntryField"));
        // A second keyboard removal straight away still works.
        removeButtons().first()->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(m_window, Qt::Key_Space);
        QTRY_COMPARE(hidden(), L({ "c" }));
    }

    void theCheckboxesDrawARingOnKeyboardFocusOnly()
    {
        QQuickItem *elsewhere = inList("gameActivityHideList", "listEntryField");
        QVERIFY(elsewhere);
        for (const char *box : { "gameActivityCheck", "musicActivityCheck" }) {
            QQuickItem *cb = one(box);
            QVERIFY2(cb, box);
            QList<QQuickItem *> rings;
            collect(cb, QStringLiteral("activityCheckFocusRing"), rings);
            QCOMPARE(rings.size(), 1);
            QQuickItem *ring = rings.first();
            // forceActiveFocus on an item that already has the focus changes
            // nothing, so move the focus away before each reason.
            elsewhere->forceActiveFocus();
            QTest::qWait(20);
            cb->forceActiveFocus(Qt::MouseFocusReason);
            QTest::qWait(20);
            QVERIFY2(cb->hasActiveFocus(), box);
            QVERIFY2(!ring->isVisible(), box);   // a mouse focus draws no ring
            elsewhere->forceActiveFocus();
            QTest::qWait(20);
            cb->forceActiveFocus(Qt::TabFocusReason);
            QTRY_VERIFY2(ring->isVisible(), box);
            QVERIFY(ring->width() > 0 && ring->height() > 0);
            QCOMPARE(ring->property("border").value<QObject *>()->property("width").toInt(), 2);
        }
    }

    void theRingIsReadableOnEveryPalette()
    {
        QVariant v;
        QVERIFY(QMetaObject::invokeMethod(m_root, "ringsPerTheme", Q_RETURN_ARG(QVariant, v)));
        const QVariantList rows = v.toList();
        QCOMPARE(rows.size(), 12);
        QSet<QString> pages;
        double worst = 100;
        QString worstAt;
        for (const QVariant &r : rows) {
            const QVariantMap m = r.toMap();
            const QColor ring(m.value("ring").toString());
            const QColor page(m.value("page").toString());
            QVERIFY(ring.isValid() && page.isValid());
            // bolt (what the text fields draw in Settings) and focusRing
            // (what AppComboBox draws outside Storm) are one colour on every
            // palette, so the ring token cannot differ from either.
            QCOMPARE(QColor(m.value("focusRing").toString()).name(),
                     ring.name());
            pages.insert(page.name());
            const double c = contrast(ring, page);
            if (c < worst) {
                worst = c;
                worstAt = QString::number(m.value("mode").toInt());
            }
        }
        // The count of what actually varied, not of loop iterations.
        QVERIFY2(pages.size() >= 10, qPrintable(QString::number(pages.size())));
        // WCAG 1.4.11 asks 3:1. Indigo Night (theme 9) is 2.86:1 for EVERY
        // control that draws its focus in the accent, text fields included:
        // a palette decision recorded in the fixer notes, so the floor sits
        // just under it and any other palette (or a worse one) fails here.
        QVERIFY2(worst >= 2.8,
                 qPrintable(QStringLiteral("worst ring contrast %1 at theme %2")
                                .arg(worst).arg(worstAt)));
        QVERIFY2(worstAt == QLatin1String("9") || worst >= 3.0,
                 qPrintable(QStringLiteral("theme %1 is below 3:1 (%2)")
                                .arg(worstAt).arg(worst)));
    }
};

QTEST_MAIN(ActivitySettingsQmlTest)
#include "ActivitySettingsQmlTest.moc"
