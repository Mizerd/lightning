
#include <QFile>
#include <QRegularExpression>
#include <QtTest/QtTest>

namespace {
QString readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}
}

class SearchKeyboardContractTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void searchDialogResultsListIsFocusableAndEnterOpensTheCurrentRow()
    {
        const QString s = readAll(QStringLiteral(QML_DIR "/MessageSearchDialog.qml"));
        QVERIFY(!s.isEmpty());
        const int list = s.indexOf(QStringLiteral("id: resultsList"));
        QVERIFY(list > 0);
        const QString tail = s.mid(list);
        QVERIFY(tail.contains(QStringLiteral("activeFocusOnTab: true")));
        QVERIFY(tail.contains(QStringLiteral("Keys.onReturnPressed")));
        QVERIFY(tail.contains(QStringLiteral("Keys.onEnterPressed")));
        QVERIFY(tail.contains(QStringLiteral("Qt.Key_PageDown")));
        QVERIFY(tail.contains(QStringLiteral("Qt.Key_Home")));
        QVERIFY(tail.contains(QStringLiteral("ListView.isCurrentItem")));
    }

    void contextViewTakesFocusAndScrollsByKey()
    {
        const QString s = readAll(QStringLiteral(QML_DIR "/ContextView.qml"));
        QVERIFY(!s.isEmpty());
        QVERIFY(s.contains(QStringLiteral("onVisibleChanged")));
        QVERIFY(s.contains(QStringLiteral("list.forceActiveFocus")));
        QVERIFY(s.contains(QStringLiteral("ContextController.Ready)\n            list.forceActiveFocus")));
        QVERIFY(s.contains(QStringLiteral("function onStateChanged()")));
        const int list = s.indexOf(QStringLiteral("id: list"));
        QVERIFY(list > 0);
        const QString tail = s.mid(list);
        QVERIFY(tail.contains(QStringLiteral("activeFocusOnTab: true")));
        for (const char *key : { "Qt.Key_PageDown", "Qt.Key_PageUp",
                                 "Qt.Key_Home", "Qt.Key_End" })
            QVERIFY2(tail.contains(QLatin1String(key)), key);
    }
};

QTEST_MAIN(SearchKeyboardContractTest)
#include "SearchKeyboardContractTest.moc"
