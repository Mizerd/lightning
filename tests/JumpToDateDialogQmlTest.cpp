// The Jump to date dialog, loaded for real (offscreen) against a stub `app`.
// Pins (C32-F01, C32-F02):
//   * Return on the focused Jump button submits, Return on Cancel closes;
//   * Return in the date field submits (and an invalid date does not);
//   * the date field and the dialog carry an accessible name, and the field
//     has no inputMask (which would hide the placeholder format hint).

#include <QAccessible>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest/QtTest>

namespace {

class StubApp final : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE double jumpToDate(double ms)
    {
        ++calls;
        lastMs = ms;
        return 7;
    }
    int calls = 0;
    double lastMs = 0;
Q_SIGNALS:
    void jumpToDateFinished(double opId, bool ok, const QString &category);
};

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 900
    height: 700
    visible: true
    property string dialogName: dlg.Accessible.name
    property int dialogRole: dlg.Accessible.role
    JumpToDateDialog {
        id: dlg
        objectName: "dialogUnderTest"
        parent: Overlay.overlay
    }
}
)QML";

} // namespace

class JumpToDateDialogQmlTest : public QObject
{
    Q_OBJECT
    StubApp m_app;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QObject *m_dialog = nullptr;
    QQuickWindow *m_window = nullptr;

    QQuickItem *item(const char *name) const
    {
        return m_dialog->findChild<QQuickItem *>(QLatin1String(name));
    }
    void open()
    {
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "openDialog"));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        m_app.calls = 0;
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), &m_app);
        QQmlComponent c(m_engine);
        c.setData(QByteArray(kScene), QUrl(QStringLiteral("jumpscene.qml")));
        m_root = c.create();
        QVERIFY2(m_root, qPrintable(c.errorString()));
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        m_dialog = m_root->findChild<QObject *>(QStringLiteral("dialogUnderTest"));
        QVERIFY(m_dialog);
    }
    void cleanupTestCase() { delete m_root; }

    void returnOnTheFocusedJumpButtonSubmits()
    {
        open();
        QQuickItem *jump = item("jumpToDateConfirm");
        QVERIFY(jump);
        jump->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(jump->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QCOMPARE(m_app.calls, 1);
    }

    void returnOnCancelClosesWithoutSubmitting()
    {
        open();
        QQuickItem *cancel = item("jumpToDateCancel");
        QVERIFY(cancel);
        cancel->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(cancel->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        QCOMPARE(m_app.calls, 0);
    }

    void returnInTheDateFieldSubmitsAValidDateOnly()
    {
        open();
        m_dialog->setProperty("choice", QStringLiteral("custom"));
        QQuickItem *field = item("jumpToDateField");
        QVERIFY(field);
        field->setProperty("text", QStringLiteral("2026-01-05"));
        field->forceActiveFocus();
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QCOMPARE(m_app.calls, 1);

        open();
        m_dialog->setProperty("choice", QStringLiteral("custom"));
        field->setProperty("text", QStringLiteral("2026-02-31"));
        field->forceActiveFocus();
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QCOMPARE(m_app.calls, 0);
    }

    void theFieldAndTheDialogAreNamedAndTheHintCanShow()
    {
        open();
        m_dialog->setProperty("choice", QStringLiteral("custom"));
        QQuickItem *field = item("jumpToDateField");
        QVERIFY(field);
        QVERIFY(field->property("inputMask").toString().isEmpty());
        QAccessibleInterface *fi = QAccessible::queryAccessibleInterface(field);
        QVERIFY(fi);
        // Equality, not "non-empty": a TextField may fall back to its
        // placeholder ("yyyy-mm-dd") for the name.
        QCOMPARE(fi->text(QAccessible::Name), QStringLiteral("Pick a date"));
        // A Popup is not an item, so read the attached properties in QML.
        QCOMPARE(m_root->property("dialogName").toString(), QStringLiteral("Jump to date"));
        QCOMPARE(m_root->property("dialogRole").toInt(), int(QAccessible::Dialog));
    }
};

QTEST_MAIN(JumpToDateDialogQmlTest)
#include "JumpToDateDialogQmlTest.moc"
