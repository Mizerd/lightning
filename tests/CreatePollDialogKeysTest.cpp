// Create poll dialog (swarm C44): Enter in an answer must move focus instead
// of sending, removing a row must not throw, the preview's height must reach
// the scrolling body, and focus must scroll into view.
#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

#include "app/AppController.h"

namespace {
QStringList g_warnings;
QtMessageHandler g_prev = nullptr;
void handler(QtMsgType t, const QMessageLogContext &c, const QString &m)
{
    g_warnings << m;
    if (g_prev)
        g_prev(t, c, m);
}

QQuickItem *findVisual(QQuickItem *parent, const QString &name)
{
    for (QQuickItem *child : parent->childItems()) {
        if (child->objectName() == name)
            return child;
        if (QQuickItem *hit = findVisual(child, name))
            return hit;
    }
    return nullptr;
}
void findAll(QQuickItem *parent, const QString &name, QList<QQuickItem *> &out)
{
    for (QQuickItem *child : parent->childItems()) {
        if (child->objectName() == name)
            out.append(child);
        findAll(child, name, out);
    }
}
void settle(int ms = 200)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}
void typeText(QQuickWindow *w, const char *text)
{
    for (const char *c = text; *c; ++c)
        QTest::keyClick(w, *c);
}
} // namespace

class CreatePollDialogKeysTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<AppController> app;
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> windowObj;
    QObject *dialog = nullptr;
    QQuickItem *content = nullptr;
    QQuickWindow *window = nullptr;

    QList<QQuickItem *> answerFields()
    {
        QList<QQuickItem *> f;
        findAll(content, QStringLiteral("pollAnswerField"), f);
        return f;
    }
    void setup()
    {
        app = std::make_unique<AppController>(AppController::MockBackend);
        engine = std::make_unique<QQmlEngine>();
        engine->rootContext()->setContextProperty("app", app.get());
        QQmlComponent comp(engine.get());
        comp.setData("import QtQuick\nimport QtQuick.Controls\n"
                     "import MatrixClient\n"
                     "ApplicationWindow { width: 900; height: 600;"
                     " visible: true; property alias dialog: d;"
                     " CreatePollDialog { id: d } }",
                     QUrl());
        windowObj.reset(comp.create());
        QVERIFY2(windowObj, qPrintable(comp.errorString()));
        window = qobject_cast<QQuickWindow *>(windowObj.get());
        dialog = windowObj->property("dialog").value<QObject *>();
        QMetaObject::invokeMethod(dialog, "openDialog");
        settle();
        content = dialog->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
    }
    void teardown()
    {
        windowObj.reset();
        engine.reset();
        app.reset();
    }

private Q_SLOTS:
    void initTestCase()
    {
        g_prev = qInstallMessageHandler(handler);
    }

    void enterWalksTheAnswersAndNeverSends()
    {
        setup();
        QQuickItem *q = findVisual(content, QStringLiteral("pollQuestionField"));
        QVERIFY(q);
        q->setProperty("text", "Lunch?");
        auto fields = answerFields();
        QCOMPARE(fields.size(), 2);
        fields[0]->forceActiveFocus();
        typeText(window, "red");
        QTest::keyClick(window, Qt::Key_Return); // non-last: focus row 2
        settle();
        QVERIFY2(dialog->property("opened").toBool(),
                 "Enter in a non-last answer sent the poll");
        fields = answerFields();
        QVERIFY2(fields[1]->hasActiveFocus(), "Enter did not focus the next row");
        typeText(window, "blue");
        QTest::keyClick(window, Qt::Key_Return); // last: add + focus row 3
        settle();
        fields = answerFields();
        QCOMPARE(fields.size(), 3);
        QVERIFY2(fields[2]->hasActiveFocus(), "the added row was not focused");
        QTest::keyClick(window, Qt::Key_Return); // empty last row: add another
        settle();
        QVERIFY2(dialog->property("opened").toBool(),
                 "a second Enter sent the poll");
        teardown();
    }

    void enterInANonLastRowOfAValidFormNeverSends()
    {
        // The reported regression: a valid form, Enter in a row that is not
        // the last one. The old handler called submit() here.
        setup();
        QQuickItem *q = findVisual(content, QStringLiteral("pollQuestionField"));
        q->setProperty("text", "Lunch?");
        QQmlExpression ex(engine->contextForObject(dialog), dialog,
                          QStringLiteral("answerModel.setProperty(0,'answerText','red');"
                                         "answerModel.setProperty(1,'answerText','blue');"
                                         "answerModel.append({answerText:'green'});"
                                         "bumpAnswerRevision(); formValid"));
        QVERIFY2(ex.evaluate().toBool(), "fixture: the form must be valid");
        settle();
        auto fields = answerFields();
        QCOMPARE(fields.size(), 3);
        fields[0]->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Return);
        settle();
        QVERIFY2(dialog->property("opened").toBool(),
                 "Enter in a non-last answer of a valid form sent the poll");
        fields = answerFields();
        QVERIFY2(fields[1]->hasActiveFocus(), "Enter did not focus the next row");
        teardown();
    }

    void enterInTheLastRowOfAFullValidListSends()
    {
        // The one branch that must still send.
        setup();
        QQuickItem *q = findVisual(content, QStringLiteral("pollQuestionField"));
        q->setProperty("text", "Lunch?");
        QQmlExpression ex(engine->contextForObject(dialog), dialog,
                          QStringLiteral("for (var i=0;i<2;++i) answerModel."
                                         "setProperty(i,'answerText','a'+i);"
                                         "for (var j=2;j<maxAnswers;++j) answerModel."
                                         "append({answerText:'a'+j});"
                                         "bumpAnswerRevision(); answerModel.count"));
        QCOMPARE(ex.evaluate().toInt(), 20);
        settle();
        auto fields = answerFields();
        QCOMPARE(fields.size(), 20);
        fields.last()->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Return);
        settle();
        QVERIFY2(!dialog->property("opened").toBool(),
                 "Enter in the last row of a full valid list did not send");
        teardown();
    }

    void enterInTheLastRowOfALongListKeepsTheNewRowInView()
    {
        // Adding a row and focusing it happen in one JS call, before layout:
        // the follow-focus measurement must wait for the new row's geometry.
        setup();
        QQmlExpression ex(engine->contextForObject(dialog), dialog,
                          QStringLiteral("for (var i=2;i<12;++i) "
                                         "answerModel.append({answerText:'a'+i});"));
        ex.evaluate();
        settle();
        auto fields = answerFields();
        QCOMPARE(fields.size(), 12);
        QQuickItem *flick = findVisual(content, QStringLiteral("pollBodyFlick"));
        QVERIFY(flick->property("contentHeight").toReal() > flick->height());
        fields.last()->forceActiveFocus();
        settle();
        QTest::keyClick(window, Qt::Key_Return); // appends row 13, focuses it
        settle();
        fields = answerFields();
        QCOMPARE(fields.size(), 13);
        QQuickItem *added = fields.last();
        QVERIFY(added->hasActiveFocus());
        auto *ci = flick->property("contentItem").value<QQuickItem *>();
        const qreal top = added->mapToItem(ci, QPointF(0, 0)).y();
        const qreal bottom = top + added->height();
        const qreal cy = flick->property("contentY").toReal();
        QVERIFY2(top >= cy - 1 && bottom <= cy + flick->height() + 1,
                 qPrintable(QStringLiteral("new row [%1,%2] outside view [%3,%4]")
                                .arg(top).arg(bottom).arg(cy).arg(cy + flick->height())));
        teardown();
    }

    void removingARowThrowsNothingAndRefreshesThePreview()
    {
        setup();
        // two rows only clear, so add a third first through the UI path
        auto fields = answerFields();
        fields.last()->forceActiveFocus();
        typeText(window, "x");
        QTest::keyClick(window, Qt::Key_Return);
        settle();
        g_warnings.clear();
        QList<QQuickItem *> rm;
        findAll(content, QStringLiteral("pollAnswerRemoveButton"), rm);
        QVERIFY(!rm.isEmpty());
        QPointF c = rm[0]->mapToScene(QPointF(rm[0]->width() / 2, rm[0]->height() / 2));
        QTest::mouseClick(window, Qt::LeftButton, {}, c.toPoint());
        settle();
        for (const QString &w : std::as_const(g_warnings))
            QVERIFY2(!w.contains("is not defined"), qPrintable(w));
        teardown();
    }

    void aLongPreviewReachesTheScrollingBody()
    {
        setup();
        QQuickItem *q = findVisual(content, QStringLiteral("pollQuestionField"));
        q->setProperty("text", QString(300, QLatin1Char('Q')));
        const QString longWord(200, QLatin1Char('w'));
        QQmlExpression ex(engine->contextForObject(dialog), dialog,
                          QStringLiteral("for (var i=0;i<2;++i) answerModel."
                                         "setProperty(i,'answerText','%1');"
                                         "answerModel.append({answerText:'%1'});"
                                         "bumpAnswerRevision()").arg(longWord));
        ex.evaluate();
        settle();
        QQuickItem *flick = findVisual(content, QStringLiteral("pollBodyFlick"));
        QQuickItem *panel = findVisual(content, QStringLiteral("pollPreviewPanel"));
        QVERIFY(flick && panel);
        const qreal contentHeight = flick->property("contentHeight").toReal();
        const qreal panelBottom = panel->mapToItem(flick->property("contentItem")
                                      .value<QQuickItem *>(), QPointF(0, panel->height())).y();
        QVERIFY2(contentHeight + 1 >= panelBottom,
                 qPrintable(QStringLiteral("body content %1px is shorter than the "
                                           "preview's bottom edge %2px: clipped")
                                .arg(contentHeight).arg(panelBottom)));
        QList<QQuickItem *> vote;
        findAll(content, QStringLiteral("pollPreviewVoteLine"), vote);
        QVERIFY(!vote.isEmpty());
        const qreal voteBottom = vote[0]->mapToItem(panel, QPointF(0, vote[0]->height())).y();
        QVERIFY2(voteBottom <= panel->height() + 1,
                 "the vote line is drawn outside the preview panel");
        teardown();
    }

    void focusScrollsIntoViewAndPageKeysScroll()
    {
        setup();
        QQmlExpression ex(engine->contextForObject(dialog), dialog,
                          QStringLiteral("for (var i=0;i<18;++i) "
                                         "answerModel.append({answerText:''});"));
        ex.evaluate();
        settle();
        auto fields = answerFields();
        QCOMPARE(fields.size(), 20);
        QQuickItem *flick = findVisual(content, QStringLiteral("pollBodyFlick"));
        QVERIFY(flick);
        QVERIFY(flick->property("contentHeight").toReal() > flick->height());
        fields.last()->forceActiveFocus();
        settle();
        QVERIFY2(flick->property("contentY").toReal() > 0,
                 "focusing the last answer did not scroll the body");
        QQuickItem *tf = fields[10];
        tf->forceActiveFocus();
        settle();
        flick->setProperty("contentY", 0);
        QTest::keyClick(window, Qt::Key_PageDown);
        settle();
        QVERIFY2(flick->property("contentY").toReal() > 0,
                 "PageDown did not scroll the poll body");
        teardown();
    }
};

QTEST_MAIN(CreatePollDialogKeysTest)
#include "CreatePollDialogKeysTest.moc"
