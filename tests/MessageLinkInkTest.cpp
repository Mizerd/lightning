#include <QFile>
#include <QJSEngine>
#include <QtTest>

class MessageLinkInkTest : public QObject
{
    Q_OBJECT

    static QString source()
    {
        QFile f(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                           : QString();
    }

    static QString functionText(const QString &qml, const QString &name)
    {
        const qsizetype at = qml.indexOf(QStringLiteral("function ") + name + QLatin1Char('('));
        if (at < 0)
            return {};
        qsizetype i = qml.indexOf(QLatin1Char('{'), at);
        int depth = 0;
        for (qsizetype j = i; j < qml.size(); ++j) {
            if (qml[j] == QLatin1Char('{')) ++depth;
            else if (qml[j] == QLatin1Char('}') && --depth == 0)
                return qml.mid(at, j - at + 1);
        }
        return {};
    }

    QJSValue build(QJSEngine &e, bool onOwnBubble)
    {
        QString fn = functionText(source(), QStringLiteral("inkLinksForOwnBubble"));
        if (fn.isEmpty())
            return {};
        e.evaluate(QStringLiteral("var AppTheme = { link: '#2E7D32', ownBubbleText: '#FFFFFF' };"
                                  "var drawsOnOwnBubble = %1;").arg(onOwnBubble ? "true" : "false"));
        return e.evaluate(QLatin1Char('(') + fn.replace(QStringLiteral("function inkLinksForOwnBubble"),
                                                        QStringLiteral("function")) + QLatin1Char(')'));
    }

private slots:
    void linksOnAnOwnBubbleTakeTheBubbleInkAndStayUnderlined()
    {
        QJSEngine e;
        QJSValue f = build(e, true);
        QVERIFY2(f.isCallable(), "inkLinksForOwnBubble() is missing from MessageDelegate.qml");
        const QString html = QStringLiteral(
            "see <a href=\"https://x.org\" style=\"color:#2e7d32\">x.org</a> end");
        const QString out = f.call({html}).toString();
        QVERIFY2(!out.contains(QStringLiteral("#2e7d32")), qPrintable(out));
        QVERIFY2(out.contains(QStringLiteral("style=\"color:#ffffff; text-decoration:underline\"")),
                 qPrintable(out));
    }

    void mentionPillsOnAnOwnBubbleKeepWeightAndTakeTheBubbleInk()
    {
        QJSEngine e;
        QJSValue f = build(e, true);
        QVERIFY(f.isCallable());
        const QString html = QStringLiteral(
            "<a href=\"mention:@a:b\" style=\"color:#2e7d32;font-weight:600;"
            "text-decoration:none\">@a</a>");
        const QString out = f.call({html}).toString();
        QCOMPARE(out, QStringLiteral(
            "<a href=\"mention:@a:b\" style=\"color:#ffffff;font-weight:600;"
            "text-decoration:none\">@a</a>"));
    }

    void inkOtherThanTheLinkInkIsUntouched()
    {
        QJSEngine e;
        QJSValue f = build(e, true);
        QVERIFY(f.isCallable());
        const QString html = QStringLiteral("<span style=\"color:#112233;font-weight:600\">x</span>");
        QCOMPARE(f.call({html}).toString(), html);
    }

    void otherRowsAreUntouched()
    {
        QJSEngine e;
        QJSValue f = build(e, false);
        QVERIFY(f.isCallable());
        const QString html = QStringLiteral("<a href=\"https://x.org\" style=\"color:#2e7d32\">x</a>");
        QCOMPARE(f.call({html}).toString(), html);
    }

    void bothBodyPathsApplyIt()
    {
        const QString qml = source();
        QCOMPARE(qml.count(QStringLiteral("root.inkLinksForOwnBubble(")), 2);
    }
};

QTEST_GUILESS_MAIN(MessageLinkInkTest)
#include "MessageLinkInkTest.moc"
