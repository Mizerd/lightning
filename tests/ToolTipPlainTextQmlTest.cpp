// Tooltips never render someone else's text as markup (breaker-swarm
// integration, MASTER-2 H1 / MASTER-3 H3).
//
// Qt Quick Controls serves every ATTACHED ToolTip.text from one shared
// instance per engine, and the Basic style's contentItem is a Text in
// AutoText: a room name like `Room <img src="http://host/x.png">` was rendered
// as StyledText and the image fetched, straight from the sender's host, the
// moment the tooltip showed. Main.qml's SharedToolTipGuard makes the shared
// instance plain text; an explicit ToolTip { } has its own contentItem and
// must be a PlainToolTip (or declare plain text) when its text is not ours.
//
// Fails without the guard: the shared contentItem stays AutoText and the
// listener below receives the image request.

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
        // Nothing fetched the <img>.
        QTest::qWait(600);
        QVERIFY2(!listener.hasPendingConnections(),
                 "a tooltip fetched an image named in a room name");
    }

    // Main.qml keeps the guard.
    void theApplicationWindowCarriesTheGuard()
    {
        QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/Main.qml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString main = QString::fromUtf8(f.readAll());
        QVERIFY(main.contains(QRegularExpression(QStringLiteral("\\bSharedToolTipGuard\\s*\\{"))));
    }

    // Every explicit ToolTip { } whose text is not a literal qsTr() draws it
    // as plain text (PlainToolTip, or its own PlainText content).
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
        // Our own words only (qsTr with arguments we compute), each listed by
        // file AND the start of its text binding, so a new foreign-text
        // ToolTip in the same file is still caught (recheck N4).
        const QStringList allowed = {
            QStringLiteral("AccountMenu.qml text: root.activeTrustCompleted < 0 || trustChip.full"),
            QStringLiteral("AudioPlayerCard.qml text: playButton.actionLabel"),
            QStringLiteral("MessageDelegate.qml text: qsTr("),
        };
        QStringList unexpected;
        int allowedSeen = 0;
        for (const QString &o : std::as_const(offenders)) {
            // "File.qml:LINE text: ..." -> "File.qml text: ..."
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
        // Each allowance still matches exactly one block: a stale entry is
        // removed, not left to excuse something else.
        QCOMPARE(allowedSeen, int(allowed.size()));
        QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(ToolTipPlainTextQmlTest)
#include "ToolTipPlainTextQmlTest.moc"
