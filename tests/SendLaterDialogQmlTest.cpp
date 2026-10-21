// SendLaterDialog, driven through the keyboard on the real dialog:
//
//   * Return in the date field, the time field, or on the focused Schedule
//     button schedules the message (it used to do nothing: only a click or
//     Space did);
//   * Return in a pending entry's text editor commits the edit AND closes the
//     editor. updateText emits pendingChanged, which rebuilds the delegate the
//     handler runs from, so the statement after it threw
//     "ReferenceError: root is not defined" and the editor stayed open;
//   * "Pick date & time" opens prefilled with a FUTURE instant (the date came
//     from now and the time from now + 1 h, so after 23:00 it was in the
//     past and the dialog opened already showing its error).
//
// A focused scene around the real AppController (mock backend), so the real
// ScheduledSendController is the thing under the dialog.

#include <QtTest/QtTest>

#include <QDateTime>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "models/ScheduledSendController.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true
    color: AppTheme.background

    SendLaterDialog {
        objectName: "sendLaterDialog"
    }
}
)QML";

} // namespace

class SendLaterDialogQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QObject *m_dialog = nullptr;
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

    ScheduledSendController *scheduler() const
    {
        return qobject_cast<ScheduledSendController *>(
            m_controller->scheduledSends());
    }

    static QVariantMap message(const QString &body)
    {
        QVariantMap m;
        m.insert(QStringLiteral("roomId"), QStringLiteral("!sendlater:test"));
        m.insert(QStringLiteral("body"), body);
        return m;
    }

    void openFor(const QVariantMap &snapshot)
    {
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "openFor",
                                          Q_ARG(QVariant, QVariant(snapshot))));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        QCoreApplication::processEvents();
    }

    void closeDialog()
    {
        QMetaObject::invokeMethod(m_dialog, "close");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        QCoreApplication::processEvents();
    }

    void clearPending()
    {
        const QVariantList all = scheduler()->property("pending").toList();
        for (const QVariant &v : all)
            scheduler()->cancel(v.toMap().value(QStringLiteral("id")).toString());
    }

    // Opens the dialog in "custom" mode with a valid future date and time.
    void openCustom(const QString &body)
    {
        openFor(message(body));
        m_dialog->setProperty("choice", QStringLiteral("custom"));
        const QDateTime later = QDateTime::currentDateTime().addDays(3);
        item("sendLaterDate")->setProperty(
            "text", later.toString(QStringLiteral("yyyy-MM-dd")));
        item("sendLaterTime")->setProperty(
            "text", later.toString(QStringLiteral("HH:mm")));
        QTRY_VERIFY(!m_dialog->property("customInvalid").toBool());
    }

    int pendingCount() const
    {
        return scheduler()->property("pending").toList().size();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("send-later-dialog-qml-test"));
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
                          QUrl(QStringLiteral("sendlaterscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QCoreApplication::processEvents();
        m_dialog = m_root->findChild<QObject *>(QStringLiteral("sendLaterDialog"));
        QVERIFY(m_dialog);
        QVERIFY(scheduler());
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void init()
    {
        m_warnings.clear();
        clearPending();
    }

    void cleanup()
    {
        if (m_dialog && m_dialog->property("visible").toBool())
            closeDialog();
        clearPending();
    }

    void returnInTheDateFieldSchedules()
    {
        openCustom(QStringLiteral("date field"));
        auto *field = item("sendLaterDate");
        field->forceActiveFocus();
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(pendingCount(), 1);
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        QCOMPARE(m_warnings, QStringList{});
    }

    void returnInTheTimeFieldSchedules()
    {
        openCustom(QStringLiteral("time field"));
        auto *field = item("sendLaterTime");
        field->forceActiveFocus();
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(pendingCount(), 1);
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
    }

    void returnOnTheFocusedScheduleButtonSchedules()
    {
        openFor(message(QStringLiteral("button")));
        auto *button = item("sendLaterConfirm");
        QVERIFY(button);
        button->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(button->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(pendingCount(), 1);
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
    }

    void returnInAnInvalidCustomTimeSchedulesNothing()
    {
        openFor(message(QStringLiteral("invalid")));
        m_dialog->setProperty("choice", QStringLiteral("custom"));
        item("sendLaterDate")->setProperty("text", QStringLiteral("2001-01-01"));
        item("sendLaterTime")->setProperty("text", QStringLiteral("00:00"));
        QTRY_VERIFY(m_dialog->property("customInvalid").toBool());
        auto *field = item("sendLaterTime");
        field->forceActiveFocus();
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTest::qWait(100);
        QCOMPARE(pendingCount(), 0);
        QVERIFY(m_dialog->property("visible").toBool());
    }

    void returnInThePendingEditorCommitsAndClosesIt()
    {
        const QString id = scheduler()->schedule(
            message(QStringLiteral("original")),
            QDateTime::currentMSecsSinceEpoch() + 3600 * 1000);
        QVERIFY(!id.isEmpty());
        openFor(message(QStringLiteral("composed")));
        auto *toggle = item("sendLaterEditToggle");
        QVERIFY(toggle);
        QMetaObject::invokeMethod(toggle, "click");
        QTRY_COMPARE(m_dialog->property("editingId").toString(), id);
        auto *edit = item("sendLaterEditField");
        QVERIFY(edit);
        QTRY_VERIFY(edit->isVisible());
        edit->forceActiveFocus();
        QTRY_VERIFY(edit->hasActiveFocus());
        edit->setProperty("text", QStringLiteral("edited text"));
        m_warnings.clear();
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_dialog->property("editingId").toString(), QString());
        const QVariantList pending = scheduler()->property("pending").toList();
        QCOMPARE(pending.size(), 1);
        QCOMPARE(pending.first().toMap().value(QStringLiteral("body")).toString(),
                 QStringLiteral("edited text"));
        for (const QString &w : std::as_const(m_warnings))
            QVERIFY2(!w.contains(QLatin1String("not defined")), qPrintable(w));
    }

    void theDoneButtonCommitsAndClosesTheEditor()
    {
        const QString id = scheduler()->schedule(
            message(QStringLiteral("before")),
            QDateTime::currentMSecsSinceEpoch() + 3600 * 1000);
        QVERIFY(!id.isEmpty());
        openFor(message(QStringLiteral("composed")));
        auto *toggle = item("sendLaterEditToggle");
        QMetaObject::invokeMethod(toggle, "click");
        QTRY_COMPARE(m_dialog->property("editingId").toString(), id);
        item("sendLaterEditField")->setProperty("text", QStringLiteral("after"));
        QMetaObject::invokeMethod(toggle, "click");
        QTRY_COMPARE(m_dialog->property("editingId").toString(), QString());
        QCOMPARE(scheduler()->property("pending").toList().first().toMap()
                     .value(QStringLiteral("body")).toString(),
                 QStringLiteral("after"));
    }

    // Date and time come from one instant. Only a clock inside the last hour
    // of a local day tells the old code apart (it took the date from now and
    // the time from now + 1 h), so this asserts the property itself: the
    // prefill is about one hour ahead, to the minute.
    void thePrefillIsOneHourAheadAsASingleInstant()
    {
        openFor(message(QStringLiteral("prefill")));
        const QString d = item("sendLaterDate")->property("text").toString();
        const QString t = item("sendLaterTime")->property("text").toString();
        const QDateTime prefill = QDateTime::fromString(
            d + QLatin1Char(' ') + t, QStringLiteral("yyyy-MM-dd HH:mm"));
        QVERIFY(prefill.isValid());
        const qint64 deltaSecs =
            QDateTime::currentDateTime().secsTo(prefill);
        QVERIFY2(deltaSecs > 3600 - 120 && deltaSecs <= 3600 + 60,
                 qPrintable(QString::number(deltaSecs)));
        m_dialog->setProperty("choice", QStringLiteral("custom"));
        QVERIFY(!m_dialog->property("customInvalid").toBool());
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    SendLaterDialogQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "SendLaterDialogQmlTest.moc"
