#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/MessageComposer.h"
#include "models/VimKeymap.h"

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 900
    height: 420
    visible: true
    MessageComposerBar {
        objectName: "composerBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
)QML";

const QString kRoom = QStringLiteral("!general:mock.local");

} // namespace

class ComposerVimQmlTest : public QObject
{
    Q_OBJECT

    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QStringList m_warnings;

    QQuickItem *item(const char *name) const
    {
        return m_root->findChild<QQuickItem *>(QLatin1String(name));
    }
    QQuickItem *input() const { return item("composerInput"); }
    VimKeymap *vim() const { return m_controller->composer()->vim(); }
    QString text() const { return input()->property("text").toString(); }
    int cursor() const { return input()->property("cursorPosition").toInt(); }

    void type(const QString &keys)
    {
        for (const QChar c : keys) {
            if (c == u'$')
                QTest::keyClick(m_window, Qt::Key_Dollar, Qt::ShiftModifier);
            else if (c.isUpper())
                QTest::keyClick(m_window, c.toLatin1(), Qt::ShiftModifier);
            else
                QTest::keyClick(m_window, c.toLatin1());
        }
    }

    void focusFresh(const QString &content = QString())
    {
        m_controller->setCurrentRoomId(kRoom);
        vim()->enterInsert();
        m_controller->composer()->cancelReplyOrEdit();
        input()->setProperty("text", content);
        input()->setProperty("cursorPosition", content.size());
        QMetaObject::invokeMethod(input(), "forceActiveFocus");
        QTRY_VERIFY(input()->hasActiveFocus());
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_controller = new AppController(AppController::MockBackend);
        m_controller->settings()->setVimComposer(false);
        m_engine = new QQmlEngine(this);
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("vimscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QVERIFY(input());
    }

    void cleanup()
    {
        m_controller->settings()->setVimComposer(false);
    }

    void cleanupTestCase()
    {
        m_controller->settings()->setVimComposer(false);
        delete m_root;
        delete m_controller;
    }

    void offByDefaultEscapeKeepsTyping()
    {
        QVERIFY(!m_controller->settings()->vimComposer());
        focusFresh();
        auto *badge = item("composerVimMode");
        QVERIFY(badge);
        QVERIFY(!badge->isVisible());
        type(QStringLiteral("ab"));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY(!vim()->normal());
        type(QStringLiteral("x"));
        QCOMPARE(text(), QStringLiteral("abx"));
    }

    void escapeTogglesModesAndNormalDoesNotType()
    {
        m_controller->settings()->setVimComposer(true);
        focusFresh();
        auto *badge = item("composerVimMode");
        auto *label = item("composerVimModeLabel");
        QVERIFY(badge && label);
        QTRY_VERIFY(badge->isVisible());
        QCOMPARE(label->property("text").toString(), QStringLiteral("INSERT"));
        QVERIFY(input()->inputMethodQuery(Qt::ImEnabled).toBool());

        type(QStringLiteral("hello world"));
        QCOMPARE(text(), QStringLiteral("hello world"));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY(vim()->normal());
        QCOMPARE(label->property("text").toString(), QStringLiteral("NORMAL"));
        QCOMPARE(cursor(), 10);
        QVERIFY(input()->property("readOnly").toBool());
        QVERIFY(!input()->inputMethodQuery(Qt::ImEnabled).toBool());

        type(QStringLiteral("qzZQ;,."));
        QTest::keyClick(m_window, Qt::Key_Slash);
        QCOMPARE(text(), QStringLiteral("hello world"));
        QVERIFY(vim()->normal());

        type(QStringLiteral("2"));
        QCOMPARE(label->property("text").toString(), QStringLiteral("NORMAL 2"));
        type(QStringLiteral("b"));
        QCOMPARE(cursor(), 0);
        type(QStringLiteral("dw"));
        QCOMPARE(text(), QStringLiteral("world"));
        type(QStringLiteral("$x"));
        QCOMPARE(text(), QStringLiteral("worl"));
        QCOMPARE(cursor(), 3);

        type(QStringLiteral("u"));
        QCOMPARE(text(), QStringLiteral("world"));
        type(QStringLiteral("u"));
        QCOMPARE(text(), QStringLiteral("hello world"));
        QTest::keyClick(m_window, Qt::Key_R, Qt::ControlModifier);
        QCOMPARE(text(), QStringLiteral("world"));
        QVERIFY(vim()->normal());

        type(QStringLiteral("A"));
        QVERIFY(!vim()->normal());
        QCOMPARE(label->property("text").toString(), QStringLiteral("INSERT"));
        QVERIFY(input()->inputMethodQuery(Qt::ImEnabled).toBool());
        type(QStringLiteral("!"));
        QCOMPARE(text(), QStringLiteral("world!"));

        QTest::keyClick(m_window, Qt::Key_Escape);
        type(QStringLiteral("0yyp"));
        QCOMPARE(text(), QStringLiteral("world!\nworld!"));
        type(QStringLiteral("ggOtop"));
        QCOMPARE(text(), QStringLiteral("top\nworld!\nworld!"));
        QTest::keyClick(m_window, Qt::Key_Escape);
        type(QStringLiteral("jdd"));
        QCOMPARE(text(), QStringLiteral("top\nworld!"));
        type(QStringLiteral("ggcwnew"));
        QCOMPARE(text(), QStringLiteral("new\nworld!"));
        QVERIFY(m_warnings.isEmpty());
    }

    void enterInNormalModeSends()
    {
        m_controller->settings()->setVimComposer(true);
        focusFresh();
        auto *mock = m_controller->findChild<MockMatrixClient *>();
        QVERIFY(mock);
        type(QStringLiteral("ship it"));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY(vim()->normal());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(mock->lastSentBodyForTest(), QStringLiteral("ship it"));
        QTRY_COMPARE(text(), QString());
        QVERIFY(!vim()->normal());
        QVERIFY(input()->hasActiveFocus());
    }

    void escapeCancelsAReplyBeforeEnteringNormal()
    {
        m_controller->settings()->setVimComposer(true);
        focusFresh(QStringLiteral("draft"));
        MessageComposer *composer = m_controller->composer();
        composer->beginReply(QStringLiteral("$event:mock.local"),
                             QStringLiteral("@alice:mock.local"),
                             QStringLiteral("hi"));
        QVERIFY(composer->isReplying());
        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY(!composer->isReplying());
        QVERIFY(!vim()->normal());
        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY(vim()->normal());

        composer->beginReply(QStringLiteral("$event:mock.local"),
                             QStringLiteral("@alice:mock.local"),
                             QStringLiteral("hi"));
        type(QStringLiteral("2d"));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY(vim()->pending().isEmpty());
        QVERIFY(composer->isReplying());
        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY(!composer->isReplying());
        QVERIFY(vim()->normal());
        QCOMPARE(text(), QStringLiteral("draft"));
    }

    void disablingTheSettingReturnsToInsert()
    {
        m_controller->settings()->setVimComposer(true);
        focusFresh(QStringLiteral("abc"));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QVERIFY(vim()->normal());
        m_controller->settings()->setVimComposer(false);
        QVERIFY(!vim()->normal());
        QVERIFY(!input()->property("readOnly").toBool());
        type(QStringLiteral("d"));
        QVERIFY(text().contains(u'd'));
    }
};

QTEST_MAIN(ComposerVimQmlTest)
#include "ComposerVimQmlTest.moc"
