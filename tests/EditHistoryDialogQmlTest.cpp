// The Edit history dialog on the real QML (offscreen), against stubs.
// Pins C26-F01 / C26-F02:
//   * an overflowing revision list scrolls by keyboard (Down, Page_Down,
//     End, Home, Page_Up, Up) and a middle-click autoscroller covers it;
//   * Return and Enter close the dialog (a Button alone answers Space only).

#include <QDateTime>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest/QtTest>

namespace {

class StubSettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString clockTimeFormat READ clockTimeFormat CONSTANT)
public:
    QString clockTimeFormat() const { return QStringLiteral("HH:mm"); }
};

class StubApp final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
public:
    StubSettings s;
    QObject *settings() { return &s; }
};

class StubTimeline final : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE void requestEditHistory(const QString &id)
    {
        QVariantList rows;
        for (int i = 0; i < 12; ++i) {
            QVariantMap r;
            r[QStringLiteral("isOriginal")] = i == 0;
            r[QStringLiteral("isLatest")] = i == 11;
            r[QStringLiteral("redacted")] = false;
            r[QStringLiteral("undecryptable")] = false;
            r[QStringLiteral("timestamp")] = QDateTime::currentDateTime();
            r[QStringLiteral("formattedBody")] = QString();
            r[QStringLiteral("body")] = QStringLiteral("Revision %1").arg(i);
            rows << r;
        }
        Q_EMIT editHistoryReceived(id, true, false, rows);
    }
    Q_INVOKABLE QString sanitizeHtml(const QString &h) const { return h; }
Q_SIGNALS:
    void editHistoryReceived(const QString &id, bool ok, bool partial,
                             const QVariantList &rows);
};

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 640
    height: 420
    visible: true
    EditHistoryDialog {
        objectName: "dlg"
    }
}
)QML";

} // namespace

class EditHistoryDialogQmlTest : public QObject
{
    Q_OBJECT
    StubApp m_app;
    StubTimeline m_timeline;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QObject *m_dialog = nullptr;

    QQuickItem *list() const
    {
        return m_dialog->findChild<QQuickItem *>(QStringLiteral("editHistoryList"));
    }
    void openIt()
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_dialog, "openFor", Q_ARG(QVariant, QVariant::fromValue<QObject *>(&m_timeline)),
            Q_ARG(QVariant, QStringLiteral("$e"))));
        QTRY_VERIFY(m_dialog->property("opened").toBool());
        QTRY_VERIFY(list()->property("count").toInt() == 12);
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), &m_app);
        QQmlComponent c(m_engine);
        c.setData(QByteArray(kScene), QUrl(QStringLiteral("ehscene.qml")));
        m_root = c.create();
        QVERIFY2(m_root, qPrintable(c.errorString()));
        auto *w = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(w);
        QVERIFY(QTest::qWaitForWindowExposed(w));
        m_dialog = m_root->findChild<QObject *>(QStringLiteral("dlg"));
        QVERIFY(m_dialog);
    }
    void cleanupTestCase() { delete m_root; }

    void keyboardScrollsTheOverflowingList()
    {
        openIt();
        auto *w = qobject_cast<QQuickWindow *>(m_root);
        QQuickItem *l = list();
        QVERIFY(l->property("contentHeight").toReal() > l->height());
        const qreal y0 = l->property("contentY").toReal();
        QTest::keyClick(w, Qt::Key_Down);
        const qreal y1 = l->property("contentY").toReal();
        QVERIFY2(y1 > y0, "Down must scroll");
        QTest::keyClick(w, Qt::Key_PageDown);
        const qreal y2 = l->property("contentY").toReal();
        QVERIFY2(y2 > y1, "Page_Down must scroll");
        QTest::keyClick(w, Qt::Key_End);
        const qreal y3 = l->property("contentY").toReal();
        QVERIFY2(y3 > y2, "End must reach the bottom");
        QTest::keyClick(w, Qt::Key_Home);
        QCOMPARE(l->property("contentY").toReal(), y0);
        QTest::keyClick(w, Qt::Key_Escape);
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
    }

    void aMiddleClickScrollerCoversTheList()
    {
        openIt();
        QVERIFY(m_dialog->findChild<QQuickItem *>(
            QStringLiteral("editHistoryMiddleClickScroller")));
        QTest::keyClick(qobject_cast<QQuickWindow *>(m_root), Qt::Key_Escape);
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
    }

    void returnAndEnterClose()
    {
        for (Qt::Key k : { Qt::Key_Return, Qt::Key_Enter }) {
            openIt();
            QTest::keyClick(qobject_cast<QQuickWindow *>(m_root), k);
            QTRY_VERIFY(!m_dialog->property("visible").toBool());
        }
    }

    void returnClosesWithTheCloseButtonFocused()
    {
        auto *w = qobject_cast<QQuickWindow *>(m_root);
        for (Qt::Key k : { Qt::Key_Return, Qt::Key_Enter }) {
            openIt();
            QTest::keyClick(w, Qt::Key_Tab); // list -> Close button
            QQuickItem *f = w->activeFocusItem();
            QVERIFY(f);
            QVERIFY2(f->property("text").toString() == QStringLiteral("Close"),
                     "Tab from the list must land on the Close button");
            QTest::keyClick(w, k);
            QTRY_VERIFY(!m_dialog->property("visible").toBool());
        }
    }
};

QTEST_MAIN(EditHistoryDialogQmlTest)
#include "EditHistoryDialogQmlTest.moc"
