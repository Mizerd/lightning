
#include <QDirIterator>
#include <QFile>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QtTest>

#include <memory>

namespace {

const char kScene[] = R"QML(
import QtQuick
import QtQuick.Window
import MatrixClient

Window {
    width: 420
    height: 240
    visible: true
    property int appClicks: 0
    property int iconClicks: 0
    property int guardedClicks: 0
    property int guardedRefusals: 0
    property int optOutClicks: 0
    property int optOutOwnReturns: 0
    Column {
        spacing: 4
        AppButton { objectName: "app"; text: "App"; onClicked: appClicks += 1 }
        IconButton { objectName: "icon"; iconName: "close"; onClicked: iconClicks += 1 }
        AppButton {
            objectName: "guarded"
            text: "Leave"
            kind: "danger"
            keysNeedVisualFocus: true
            onClicked: guardedClicks += 1
            onKeyRefusedWithoutRing: guardedRefusals += 1
        }
        AppButton {
            objectName: "optOut"
            text: "Own"
            activateOnReturn: false
            onClicked: optOutClicks += 1
            Keys.onReturnPressed: (event) => { optOutOwnReturns += 1; event.accepted = true }
        }
    }
}
)QML";

void pressKey(QQuickWindow *window, Qt::Key key, bool autoRepeat)
{
    const QString text = key == Qt::Key_Return || key == Qt::Key_Enter
        ? QStringLiteral("\r") : QString();
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text, autoRepeat);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text, autoRepeat);
    QCoreApplication::sendEvent(window, &release);
}

}

class ButtonKeyActivationQmlTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    std::unique_ptr<QObject> m_root;
    QQuickWindow *m_window = nullptr;

    QQuickItem *item(const char *name)
    {
        return m_root->findChild<QQuickItem *>(QLatin1String(name));
    }
    int count(const char *name) { return m_root->property(name).toInt(); }

private Q_SLOTS:
    void initTestCase()
    {
        QQmlComponent component(&m_engine);
        component.setData(kScene, QUrl(QStringLiteral("buttonkeys.qml")));
        m_root.reset(component.create());
        QVERIFY2(m_root, qPrintable(component.errorString()));
        m_window = qobject_cast<QQuickWindow *>(m_root.get());
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        m_window->requestActivate();
    }

    void oneReturnOrEnterIsOneClick_data()
    {
        QTest::addColumn<QByteArray>("button");
        QTest::addColumn<QByteArray>("counter");
        QTest::addColumn<int>("key");
        QTest::newRow("AppButton Return") << QByteArray("app") << QByteArray("appClicks") << int(Qt::Key_Return);
        QTest::newRow("AppButton Enter") << QByteArray("app") << QByteArray("appClicks") << int(Qt::Key_Enter);
        QTest::newRow("IconButton Return") << QByteArray("icon") << QByteArray("iconClicks") << int(Qt::Key_Return);
        QTest::newRow("IconButton Enter") << QByteArray("icon") << QByteArray("iconClicks") << int(Qt::Key_Enter);
    }
    void oneReturnOrEnterIsOneClick()
    {
        QFETCH(QByteArray, button);
        QFETCH(QByteArray, counter);
        QFETCH(int, key);
        QQuickItem *b = item(button.constData());
        QVERIFY(b);
        b->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(b->hasActiveFocus());
        const int before = count(counter.constData());
        pressKey(m_window, Qt::Key(key), false);
        QCOMPARE(count(counter.constData()), before + 1);
    }

    void aHeldReturnDoesNotRepeat()
    {
        QQuickItem *b = item("app");
        b->forceActiveFocus(Qt::TabFocusReason);
        const int before = count("appClicks");
        pressKey(m_window, Qt::Key_Return, false);
        for (int i = 0; i < 5; ++i)
            pressKey(m_window, Qt::Key_Return, true);
        QCOMPARE(count("appClicks"), before + 1);
    }

    void aDisabledOrHiddenButtonDoesNothing()
    {
        QQuickItem *b = item("app");
        b->forceActiveFocus(Qt::TabFocusReason);
        const int before = count("appClicks");
        b->setEnabled(false);
        pressKey(m_window, Qt::Key_Return, false);
        b->setEnabled(true);
        QCOMPARE(count("appClicks"), before);
    }

    void aGuardedButtonAnswersOnlyWithItsRing()
    {
        QQuickItem *b = item("guarded");
        b->forceActiveFocus(Qt::OtherFocusReason);
        QVERIFY(b->hasActiveFocus());
        QVERIFY(!b->property("visualFocus").toBool());
        pressKey(m_window, Qt::Key_Return, false);
        pressKey(m_window, Qt::Key_Enter, false);
        QCOMPARE(count("guardedClicks"), 0);
        QCOMPARE(count("guardedRefusals"), 2);
        item("app")->forceActiveFocus(Qt::TabFocusReason);
        b->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(b->property("visualFocus").toBool());
        pressKey(m_window, Qt::Key_Return, false);
        QCOMPARE(count("guardedClicks"), 1);
    }

    void anOptOutLeavesReturnToTheInstance()
    {
        QQuickItem *b = item("optOut");
        b->forceActiveFocus(Qt::TabFocusReason);
        pressKey(m_window, Qt::Key_Return, false);
        QCOMPARE(count("optOutOwnReturns"), 1);
        QCOMPARE(count("optOutClicks"), 0);
    }

    void noInstanceDeclaresItsOwnReturnHandler()
    {
        static const QRegularExpression comment(QStringLiteral("//[^\\n]*"));
        static const QRegularExpression rootType(
            QStringLiteral("(^|\\n)(AppButton|IconButton)\\s*\\{"));
        QStringList types = { QStringLiteral("AppButton"), QStringLiteral("IconButton") };
        {
            QDirIterator dit(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml"),
                             { QStringLiteral("*.qml") }, QDir::Files);
            while (dit.hasNext()) {
                const QString path = dit.next();
                QFile f(path);
                QVERIFY(f.open(QIODevice::ReadOnly));
                const QString src = QString::fromUtf8(f.readAll()).replace(comment, QString());
                if (rootType.match(src).hasMatch())
                    types << QFileInfo(path).completeBaseName();
            }
        }
        QVERIFY2(types.size() > 2, "no derived button type found (MediaVolumeControl is one)");
        const QRegularExpression open(
            QStringLiteral("\\b(%1)\\s*\\{").arg(types.join(QLatin1Char('|'))));
        static const QRegularExpression handler(
            QStringLiteral("(^|[^.\\w])Keys\\.on(Return|Enter)Pressed\\s*:"));
        static const QRegularExpression pressedReturn(
            QStringLiteral("Keys\\.onPressed[\\s\\S]*Qt\\.Key_(Return|Enter)\\b"));
        QStringList offenders;
        int files = 0;
        int instances = 0;
        QDirIterator it(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml"),
                        { QStringLiteral("*.qml") }, QDir::Files);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString src = QString::fromUtf8(f.readAll());
            src.replace(comment, QString());
            ++files;
            const bool typeFile = types.contains(QFileInfo(path).completeBaseName());
            auto m = open.globalMatch(src);
            while (m.hasNext()) {
                const auto match = m.next();
                ++instances;
                int depth = 1;
                qsizetype i = match.capturedEnd();
                QString flat;
                const qsizetype bodyStart = i;
                for (; i < src.size() && depth > 0; ++i) {
                    const QChar c = src.at(i);
                    if (c == QLatin1Char('{'))
                        ++depth;
                    else if (c == QLatin1Char('}'))
                        --depth;
                    else if (depth == 1)
                        flat += c;
                }
                if (typeFile)
                    continue;
                const QString full = src.mid(bodyStart, i - bodyStart);
                if ((handler.match(flat).hasMatch() || pressedReturn.match(full).hasMatch())
                    && !flat.contains(QLatin1String("activateOnReturn: false"))) {
                    offenders << QStringLiteral("%1:%2")
                                     .arg(QFileInfo(path).fileName())
                                     .arg(src.left(match.capturedStart()).count(QLatin1Char('\n')) + 1);
                }
            }
        }
        QVERIFY2(files > 100, "qml/ was not found");
        QVERIFY2(instances > 200, "the scan found implausibly few button instances");
        QVERIFY2(offenders.isEmpty(), qPrintable(offenders.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(ButtonKeyActivationQmlTest)
#include "ButtonKeyActivationQmlTest.moc"
