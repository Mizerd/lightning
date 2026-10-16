// MSC3381 poll text is plain text from the sender (break-it B002/B039/B137).
//
// The poll card's question and answer labels used Qt's default AutoText, so a
// remote poll whose question carried `<img src=...>` made every viewer fetch
// that URL the moment the row rendered (an IP leak to a third party, no click
// needed), `<a>` became a live link whose target differed from its label and
// `<h1>` a huge heading. The Create poll dialog's "as sent" preview did the
// same, and wrapped a long unbroken word differently from the timeline.
//
// These load the real components and watch the engine's network access: a
// control Text with the same markup proves the harness can see a fetch, so
// "no request" from the poll labels is evidence, not silence.
#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlNetworkAccessManagerFactory>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"

namespace {
constexpr int kSignalTimeoutMs = 5000;

// Every URL any engine-owned QNetworkAccessManager was asked for. The pixmap
// reader runs on its own thread, so the list is guarded.
struct RequestLog {
    QMutex mutex;
    QStringList urls;
    void add(const QUrl &url)
    {
        QMutexLocker lock(&mutex);
        urls.append(url.toString());
    }
    bool any(const QString &needle)
    {
        QMutexLocker lock(&mutex);
        for (const QString &u : std::as_const(urls))
            if (u.contains(needle))
                return true;
        return false;
    }
};

// Records the request and answers it locally (an empty data: URL), so nothing
// leaves the machine whatever the code under test does.
class RecordingNam : public QNetworkAccessManager
{
public:
    RecordingNam(RequestLog *log, QObject *parent)
        : QNetworkAccessManager(parent), m_log(log) {}

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request,
                                 QIODevice *outgoing) override
    {
        m_log->add(request.url());
        return QNetworkAccessManager::createRequest(
            op, QNetworkRequest(QUrl(QStringLiteral("data:,"))), outgoing);
    }

private:
    RequestLog *m_log;
};

class RecordingNamFactory : public QQmlNetworkAccessManagerFactory
{
public:
    explicit RecordingNamFactory(RequestLog *log) : m_log(log) {}
    QNetworkAccessManager *create(QObject *parent) override
    {
        return new RecordingNam(m_log, parent);
    }

private:
    RequestLog *m_log;
};

const QString kTrackerQuestion = QStringLiteral(
    "track <img src=\"http://poll-question.invalid/tracker.png\"> "
    "<a href=\"https://evil.example/phish\">click me</a> <h1>HUGE</h1>");
const QString kTrackerAnswer = QStringLiteral(
    "a <img src=\"http://poll-answer.invalid/ans.png\"> <b>bold</b>");

// Text.PlainText is Qt::PlainText (0); Text.Wrap is
// QTextOption::WrapAtWordBoundaryOrAnywhere (4).
constexpr int kPlainText = 0;
constexpr int kWrap = 4;

QQuickItem *findVisual(QQuickItem *parent, const QString &name)
{
    if (!parent)
        return nullptr;
    for (QQuickItem *child : parent->childItems()) {
        if (child->objectName() == name)
            return child;
        if (QQuickItem *hit = findVisual(child, name))
            return hit;
    }
    return nullptr;
}

void findAllVisual(QQuickItem *parent, const QString &name,
                   QList<QQuickItem *> &out)
{
    if (!parent)
        return;
    for (QQuickItem *child : parent->childItems()) {
        if (child->objectName() == name)
            out.append(child);
        findAllVisual(child, name, out);
    }
}

void settle(int ms = 300)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}
} // namespace

class PollTextQmlTest : public QObject
{
    Q_OBJECT

    static QVariantMap pollFixture()
    {
        QVariantMap f;
        f.insert(QStringLiteral("isVirtual"), false);
        f.insert(QStringLiteral("isStateActivity"), false);
        f.insert(QStringLiteral("isCallEvent"), false);
        f.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        f.insert(QStringLiteral("showSenderIdentity"), true);
        f.insert(QStringLiteral("eventId"), QStringLiteral("$poll"));
        f.insert(QStringLiteral("itemId"), QStringLiteral("poll-item"));
        f.insert(QStringLiteral("sender"), QStringLiteral("@bk068:mock.local"));
        f.insert(QStringLiteral("senderDisplayName"), QStringLiteral("bk068"));
        f.insert(QStringLiteral("senderInitials"), QStringLiteral("B"));
        f.insert(QStringLiteral("body"), QStringLiteral("poll fallback"));
        f.insert(QStringLiteral("eventType"), 0);
        f.insert(QStringLiteral("status"), 0);
        f.insert(QStringLiteral("isOwn"), false);
        f.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc());
        f.insert(QStringLiteral("redacted"), false);
        f.insert(QStringLiteral("edited"), false);
        f.insert(QStringLiteral("isEncrypted"), false);
        f.insert(QStringLiteral("isDecrypted"), true);
        f.insert(QStringLiteral("undecryptable"), false);
        f.insert(QStringLiteral("errorKind"), QString{});
        f.insert(QStringLiteral("isImage"), false);
        f.insert(QStringLiteral("isFile"), false);
        f.insert(QStringLiteral("isVideo"), false);
        f.insert(QStringLiteral("isAudio"), false);
        f.insert(QStringLiteral("isSticker"), false);
        f.insert(QStringLiteral("mediaIsVoice"), false);
        f.insert(QStringLiteral("mediaDurationMs"), 0);
        f.insert(QStringLiteral("mediaWidth"), 0);
        f.insert(QStringLiteral("mediaHeight"), 0);
        f.insert(QStringLiteral("mediaSize"), 0);
        f.insert(QStringLiteral("mediaSourceAvailable"), false);
        f.insert(QStringLiteral("mediaMimetype"), QString{});
        f.insert(QStringLiteral("reactions"), QVariantList{});
        f.insert(QStringLiteral("replyToEventId"), QString{});
        f.insert(QStringLiteral("isThreadRoot"), false);
        f.insert(QStringLiteral("mentionsMe"), false);
        f.insert(QStringLiteral("mentionsRoom"), false);
        f.insert(QStringLiteral("isLocalEcho"), false);
        f.insert(QStringLiteral("readReceipts"), QVariantList{});
        f.insert(QStringLiteral("readReceiptsTotal"), 0);
        f.insert(QStringLiteral("sentReceipt"), false);
        // The poll itself.
        f.insert(QStringLiteral("isPoll"), true);
        f.insert(QStringLiteral("pollQuestion"), kTrackerQuestion);
        f.insert(QStringLiteral("pollKind"), QStringLiteral("disclosed"));
        f.insert(QStringLiteral("pollEnded"), false);
        f.insert(QStringLiteral("pollMaxSelections"), 1);
        f.insert(QStringLiteral("pollTotalVoters"), 1);
        f.insert(QStringLiteral("canEndPoll"), false);
        QVariantMap a;
        a.insert(QStringLiteral("id"), QStringLiteral("a1"));
        a.insert(QStringLiteral("text"), kTrackerAnswer);
        a.insert(QStringLiteral("count"), 1);
        a.insert(QStringLiteral("byMe"), false);
        QVariantMap b;
        b.insert(QStringLiteral("id"), QStringLiteral("a2"));
        b.insert(QStringLiteral("text"),
                 QStringLiteral("<a href=\"https://evil.example/\">lnk</a>"));
        b.insert(QStringLiteral("count"), 0);
        b.insert(QStringLiteral("byMe"), false);
        f.insert(QStringLiteral("pollAnswers"), QVariantList{a, b});
        return f;
    }

    /// The control: the same markup in a default (AutoText) Text through the
    /// same engine does fetch. Without it a broken harness would pass.
    static bool controlFetches(QQmlEngine *engine, RequestLog &log,
                               QQuickItem *parentItem)
    {
        QQmlComponent comp(engine);
        comp.setData("import QtQuick\nText { text: 'x <img "
                     "src=\"http://control.invalid/c.png\">' }",
                     QUrl());
        std::unique_ptr<QObject> obj(comp.create());
        auto *item = qobject_cast<QQuickItem *>(obj.get());
        if (!item)
            return false;
        item->setParentItem(parentItem);
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < kSignalTimeoutMs) {
            if (log.any(QStringLiteral("control.invalid")))
                return true;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return false;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("poll-text-qml-test"));
    }

    // A remote poll's question and answers render as literal text: PlainText
    // on every label, and no request for the <img> they carry.
    void aRemotePollsMarkupIsLiteralAndFetchesNothing()
    {
        AppController app(AppController::MockBackend);
        RequestLog log;
        RecordingNamFactory factory(&log);

        QQmlApplicationEngine engine;
        engine.setNetworkAccessManagerFactory(&factory);
        engine.rootContext()->setContextProperty("app", &app);
        engine.rootContext()->setContextProperty("model", pollFixture());
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("MessageDelegate"));
        QVERIFY(!created.isEmpty() || created.wait(kSignalTimeoutMs));
        auto *root = qobject_cast<QQuickItem *>(
            created.at(0).at(0).value<QObject *>());
        QVERIFY2(root, "MessageDelegate did not load");
        QQuickWindow window;
        window.resize(720, 600);
        root->setParentItem(window.contentItem());
        root->setWidth(640);
        window.show();
        settle();

        QQuickItem *question = findVisual(root, QStringLiteral("pollQuestion"));
        QVERIFY2(question, "no pollQuestion label: the poll card did not load");
        QCOMPARE(question->property("textFormat").toInt(), kPlainText);
        // Literal: the label holds the markup and lays it out as characters.
        QCOMPARE(question->property("text").toString(), kTrackerQuestion);

        QList<QQuickItem *> answers;
        findAllVisual(root, QStringLiteral("pollAnswerText"), answers);
        QCOMPARE(answers.size(), 2);
        for (QQuickItem *answer : std::as_const(answers))
            QCOMPARE(answer->property("textFormat").toInt(), kPlainText);

        QVERIFY2(controlFetches(&engine, log, window.contentItem()),
                 "the harness never saw the control Text fetch its <img>; "
                 "a missing request below would prove nothing");
        settle();
        QVERIFY2(!log.any(QStringLiteral("poll-question.invalid")),
                 "the poll question fetched its <img> (tracker request)");
        QVERIFY2(!log.any(QStringLiteral("poll-answer.invalid")),
                 "a poll answer fetched its <img> (tracker request)");
    }

    // The Create poll dialog's "as sent" preview: plain text like the card,
    // and Text.Wrap, so a long unbroken word breaks inside the preview
    // instead of running off its edge (it was Text.WordWrap).
    void theCreatePollPreviewDrawsWhatTheTimelineWill()
    {
        AppController app(AppController::MockBackend);
        RequestLog log;
        RecordingNamFactory factory(&log);

        QQmlEngine engine;
        engine.setNetworkAccessManagerFactory(&factory);
        engine.rootContext()->setContextProperty("app", &app);
        QQmlComponent comp(&engine);
        comp.setData("import QtQuick\nimport QtQuick.Controls\n"
                     "import MatrixClient\n"
                     "ApplicationWindow { width: 900; height: 760;"
                     " visible: true; property alias dialog: d;"
                     " CreatePollDialog { id: d } }",
                     QUrl());
        std::unique_ptr<QObject> windowObj(comp.create());
        QVERIFY2(windowObj, qPrintable(comp.errorString()));
        auto *dialog = windowObj->property("dialog").value<QObject *>();
        QVERIFY(dialog);
        QMetaObject::invokeMethod(dialog, "openDialog");
        settle();

        auto *contentItem =
            dialog->property("contentItem").value<QQuickItem *>();
        QVERIFY(contentItem);
        QQuickItem *field =
            findVisual(contentItem, QStringLiteral("pollQuestionField"));
        QVERIFY2(field, "no pollQuestionField in the dialog");
        QVERIFY(field->setProperty("text", kTrackerQuestion));
        QQmlExpression setAnswers(
            engine.contextForObject(dialog), dialog,
            QStringLiteral("answerModel.setProperty(0, 'answerText', '%1');"
                           "answerModel.setProperty(1, 'answerText', 'b');"
                           "bumpAnswerRevision()")
                .arg(QString(kTrackerAnswer).replace(QLatin1Char('\''),
                                                     QStringLiteral("\\'"))));
        setAnswers.evaluate();
        QVERIFY2(!setAnswers.hasError(),
                 qPrintable(setAnswers.error().toString()));
        settle();

        QQuickItem *preview =
            findVisual(contentItem, QStringLiteral("pollPreviewQuestion"));
        QVERIFY2(preview, "no pollPreviewQuestion in the dialog");
        QCOMPARE(preview->property("textFormat").toInt(), kPlainText);
        QCOMPARE(preview->property("wrapMode").toInt(), kWrap);

        QList<QQuickItem *> answers;
        findAllVisual(contentItem, QStringLiteral("pollPreviewAnswer"), answers);
        QVERIFY2(!answers.isEmpty(), "no pollPreviewAnswer rows");
        for (QQuickItem *answer : std::as_const(answers))
            QCOMPARE(answer->property("textFormat").toInt(), kPlainText);

        QVERIFY2(controlFetches(&engine, log, contentItem),
                 "the harness never saw the control Text fetch its <img>");
        settle();
        QVERIFY2(!log.any(QStringLiteral("poll-question.invalid")),
                 "the preview question fetched its <img>");
        QVERIFY2(!log.any(QStringLiteral("poll-answer.invalid")),
                 "the preview answer fetched its <img>");

        // 300 unbroken characters: with Text.Wrap the line breaks inside the
        // label; with WordWrap it overflowed the preview card.
        QVERIFY(field->setProperty("text", QString(300, QLatin1Char('A'))));
        settle(100);
        const qreal contentWidth = preview->property("contentWidth").toReal();
        QVERIFY2(preview->width() > 0, "the preview question measured empty");
        QVERIFY2(contentWidth <= preview->width() + 1.0,
                 qPrintable(QStringLiteral(
                     "a long unbroken question is %1px wide in a %2px "
                     "preview: it runs off the card instead of wrapping")
                     .arg(contentWidth).arg(preview->width())));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(PollTextQmlTest)
#include "PollTextQmlTest.moc"
