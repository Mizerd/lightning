
#include <QDirIterator>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTcpServer>
#include <QtTest>

#include <memory>

class ToolTipPlainTextQmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aMarkupRoomNameInAnAttachedTooltipIsPlainText()
    {
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost));
        const QString name = QStringLiteral(
            "Room <img src=\"http://127.0.0.1:%1/tooltip.png\"> <b>name</b>")
                                 .arg(listener.serverPort());

        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQuick
            import QtQuick.Controls
            import QtQuick.Window
            import MatrixClient
            Window {
                width: 400; height: 200; visible: true
                property string roomName: ""
                SharedToolTipGuard { }
                Item {
                    objectName: "row"
                    width: 200; height: 40
                    ToolTip.text: roomName
                    ToolTip.visible: roomName.length > 0
                    ToolTip.delay: 0
                }
            }
        )QML", QUrl(QStringLiteral("tooltipprobe.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        root->setProperty("roomName", name);

        auto *row = root->findChild<QQuickItem *>(QStringLiteral("row"));
        QVERIFY(row);
        QQmlExpression tipExpr(qmlContext(row), row, QStringLiteral("ToolTip.toolTip"));
        auto *tip = tipExpr.evaluate().value<QObject *>();
        QVERIFY(tip);
        QTRY_VERIFY(tip->property("visible").toBool());
        auto *content = tip->property("contentItem").value<QObject *>();
        QVERIFY(content);
        QCOMPARE(content->property("textFormat").toInt(), int(Qt::PlainText));
        QCOMPARE(content->property("text").toString(), name);
        QTest::qWait(600);
        QVERIFY2(!listener.hasPendingConnections(),
                 "a tooltip fetched an image named in a room name");
    }

    void aLongRoomNameWrapsInsideTheTooltipCap()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQuick
            import QtQuick.Controls
            import QtQuick.Window
            import MatrixClient
            Window {
                width: 1400; height: 300; visible: true
                SharedToolTipGuard { }
                Item {
                    id: row
                    objectName: "row"
                    width: 200; height: 40
                }
                // ToolTip.show() is the path that resets the popup's width.
                function showTip(text) { row.ToolTip.show(text) }
                function hideTip() { row.ToolTip.hide() }
            }
        )QML", QUrl(QStringLiteral("tooltipwrap.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const QString name = QString(120, QLatin1Char('w')).replace(
            5, 1, QLatin1Char(' ')).replace(11, 1, QLatin1Char(' '))
            .append(QStringLiteral(" the extremely long room name number one two three four five six seven eight nine ten"));
        auto *row = root->findChild<QQuickItem *>(QStringLiteral("row"));
        QVERIFY(row);
        QQmlExpression tipExpr(qmlContext(row), row, QStringLiteral("ToolTip.toolTip"));
        auto *tip = tipExpr.evaluate().value<QObject *>();
        QVERIFY(tip);
        for (int pass = 0; pass < 3; ++pass) {
            if (pass > 0) {
                QMetaObject::invokeMethod(root.get(), "hideTip");
                QTRY_VERIFY(!tip->property("visible").toBool());
            }
            QMetaObject::invokeMethod(root.get(), "showTip", Q_ARG(QVariant, name));
            QTRY_VERIFY(tip->property("visible").toBool());
            QTRY_VERIFY2(tip->property("width").toReal() <= 460,
                         qPrintable(QStringLiteral("pass %1 width %2").arg(pass)
                                        .arg(tip->property("width").toReal())));
            auto *content = tip->property("contentItem").value<QObject *>();
            QVERIFY(content);
            QTRY_VERIFY2(content->property("lineCount").toInt() > 1,
                         qPrintable(QStringLiteral("pass %1").arg(pass)));
        }
    }

    void theApplicationWindowCarriesTheGuard()
    {
        QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/Main.qml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString main = QString::fromUtf8(f.readAll());
        QVERIFY(main.contains(QRegularExpression(QStringLiteral("\\bSharedToolTipGuard\\s*\\{"))));
    }

    void everyExplicitToolTipWithForeignTextIsPlain()
    {
        static const QRegularExpression open(
            QStringLiteral("(?<![\\w.])ToolTip\\s*\\{"));
        static const QRegularExpression textBinding(
            QStringLiteral("(^|\\n)\\s*text\\s*:\\s*([^\\n]*)"));
        static const QRegularExpression literal(
            QStringLiteral("^qsTr\\(\"[^\"]*\"\\)\\s*$"));
        static const QRegularExpression comment(QStringLiteral("//[^\\n]*"));
        QStringList offenders;
        int blocks = 0;
        QDirIterator it(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml"),
                        { QStringLiteral("*.qml") }, QDir::Files);
        while (it.hasNext()) {
            const QString path = it.next();
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString src = QString::fromUtf8(f.readAll());
            src.replace(comment, QString());
            auto m = open.globalMatch(src);
            while (m.hasNext()) {
                const auto match = m.next();
                ++blocks;
                int depth = 1;
                qsizetype i = match.capturedEnd();
                const qsizetype start = i;
                for (; i < src.size() && depth > 0; ++i) {
                    if (src.at(i) == QLatin1Char('{'))
                        ++depth;
                    else if (src.at(i) == QLatin1Char('}'))
                        --depth;
                }
                const QString body = src.mid(start, i - start);
                const auto t = textBinding.match(body);
                const QString text = t.hasMatch() ? t.captured(2).trimmed() : QString();
                if (text.isEmpty() || literal.match(text).hasMatch())
                    continue;
                if (body.contains(QLatin1String("Text.PlainText")))
                    continue;
                offenders << QStringLiteral("%1:%2 text: %3")
                                 .arg(QFileInfo(path).fileName())
                                 .arg(src.left(match.capturedStart()).count(QLatin1Char('\n')) + 1)
                                 .arg(text.left(60));
            }
        }
        QVERIFY2(blocks > 5, "the scan found implausibly few ToolTip blocks");
        const QStringList allowed = {
            QStringLiteral("AccountMenu.qml text: root.activeTrustCompleted < 0 || trustChip.full"),
            QStringLiteral("AudioPlayerCard.qml text: playButton.actionLabel"),
            QStringLiteral("MessageDelegate.qml text: qsTr("),
        };
        QStringList unexpected;
        int allowedSeen = 0;
        for (const QString &o : std::as_const(offenders)) {
            const qsizetype colon = o.indexOf(QLatin1Char(':'));
            const qsizetype space = o.indexOf(QLatin1Char(' '), colon);
            const QString key = o.left(colon) + o.mid(space);
            bool ok = false;
            for (const QString &a : allowed)
                ok = ok || key.startsWith(a);
            if (ok)
                ++allowedSeen;
            else
                unexpected << o;
        }
        QCOMPARE(allowedSeen, int(allowed.size()));
        QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(ToolTipPlainTextQmlTest)
#include "ToolTipPlainTextQmlTest.moc"
