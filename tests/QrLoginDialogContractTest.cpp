// Contract scan for qml/QrLoginDialog.qml (swarm C49-F02, C49-F03).
// Source-text only: it proves the wiring is present, not how it renders.

#include <QFile>
#include <QtTest/QtTest>

class QrLoginDialogContractTest : public QObject
{
    Q_OBJECT
    static QString source()
    {
        QFile f(QStringLiteral(QML_DIR "/QrLoginDialog.qml"));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

private Q_SLOTS:
    void dialogAndInputsAreNamed()
    {
        const QString s = source();
        QVERIFY(!s.isEmpty());
        QVERIFY(s.contains(QStringLiteral("Accessible.role: Accessible.Dialog")));
        // Dialog name plus the two inputs: three Accessible.name entries.
        QCOMPARE(s.count(QStringLiteral("Accessible.name:")), 3);
    }

    void staleErrorIsHiddenOnEdit()
    {
        const QString s = source();
        QVERIFY(s.contains(QStringLiteral("onTextChanged: root.errorStale = true")));
        QVERIFY(s.contains(QStringLiteral("function onStateChanged() { root.errorStale = false }")));
        QVERIFY(s.contains(QStringLiteral("errorText.length > 0 && !root.errorStale")));
    }
};

QTEST_APPLESS_MAIN(QrLoginDialogContractTest)
#include "QrLoginDialogContractTest.moc"
