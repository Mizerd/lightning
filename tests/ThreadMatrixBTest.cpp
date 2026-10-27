// T38: an input method owns keys while a thread editor has uncommitted text.
// This source contract checks both QML editors because the regression was a
// missing guard in their separate Keys.onPressed handlers. Removing either
// guard makes this test fail against the pre-fix ThreadPanel.qml.

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QtTest/QtTest>

class ThreadMatrixBTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void preeditHasPriorityInBothThreadEditors()
    {
        QFile file(QStringLiteral(QML_DIR "/ThreadPanel.qml"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 "ThreadPanel.qml must be readable");
        const QString source = QString::fromUtf8(file.readAll());

        const auto verifyEditor = [&](const QString &editorId,
                                      const QString &guard,
                                      const QString &firstAction) {
            const int editor = source.indexOf(editorId);
            QVERIFY2(editor >= 0, qPrintable(editorId));
            const int handler = source.indexOf(QStringLiteral("Keys.onPressed: (event) => {"),
                                               editor);
            QVERIFY2(handler >= 0, qPrintable(editorId));
            const int guardAt = source.indexOf(guard, handler);
            const int actionAt = source.indexOf(firstAction, handler);
            QVERIFY2(guardAt > handler && actionAt > guardAt,
                     qPrintable(editorId + QStringLiteral(": preedit must precede key actions")));
            const QString prefix = source.mid(guardAt, actionAt - guardAt);
            QVERIFY2(QRegularExpression(
                         QRegularExpression::escape(guard)
                         + QStringLiteral("\\s+return\\b"))
                         .match(prefix).hasMatch(),
                     qPrintable(editorId + QStringLiteral(": preedit must return without accepting the key")));
        };

        verifyEditor(QStringLiteral("id: threadRichInput"),
                     QStringLiteral("if (threadRichInput.preeditText !== \"\")"),
                     QStringLiteral("threadRichAutocorrect.handleKey(event)"));
        verifyEditor(QStringLiteral("id: threadComposerInput"),
                     QStringLiteral("if (threadComposerInput.preeditText !== \"\")"),
                     QStringLiteral("threadPlainAutocorrect.handleKey(event)"));
    }
};

QTEST_GUILESS_MAIN(ThreadMatrixBTest)
#include "ThreadMatrixBTest.moc"
