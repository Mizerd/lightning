// The spell underline, proven against the PRODUCTION composer rather than
// against the policy behind it.
//
// This suite exists because of a lesson this repository has recorded three
// times: a test that invokes a policy function directly proves nothing about
// whether production ever reaches it. The row window shipped as a permanent
// no-op with the policy covered six ways; the rail's drop gesture passed
// fifteen model cases through two successive broken rules. So the case below
// loads the real MessageComposerBar.qml, puts a real misspelling in the real
// TextArea, and asks the real item tree what got drawn and where.
//
// What is deliberately NOT here: whether a real dictionary agrees. That is
// `--spell-status`, which runs the shipped binary against the machine's own
// checker; a fake backend is installed here so the geometry is the only
// variable.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>

#include <memory>

#include "app/AppController.h"
#include "text/SpellBackend.h"
#include "text/SpellChecker.h"

namespace {

class FakeBackend : public SpellBackend
{
public:
    bool isCorrect(const QString &word) const override
    {
        return word.compare(QStringLiteral("hello"), Qt::CaseInsensitive) == 0
            || word.compare(QStringLiteral("world"), Qt::CaseInsensitive) == 0;
    }
    QStringList suggest(const QString &) const override
    {
        return { QStringLiteral("world") };
    }
    void addToPersonalDictionary(const QString &) override {}
    QString language() const override { return QStringLiteral("en_US"); }
    QString name() const override { return QStringLiteral("fake"); }
};

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 900
    height: 420
    visible: true
    color: AppTheme.background

    MessageComposerBar {
        objectName: "composerBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
)QML";

} // namespace

class ComposerSpellQmlTest : public QObject
{
    Q_OBJECT

private:
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QStringList m_warnings;

    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        if (parent->objectName() == name)
            return parent;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        }
        return nullptr;
    }

    QQuickItem *item(const QString &name) const
    {
        return findItem(m_window->contentItem(), name);
    }

    // Repeater delegates live only in the VISUAL tree — findChild cannot see
    // them, which this repository has proven with a constant objectName that
    // never appeared in a full findChildren dump. Walk childItems().
    QList<QQuickItem *> underlines() const
    {
        QList<QQuickItem *> out;
        QQuickItem *input = item(QStringLiteral("composerInput"));
        if (!input)
            return out;
        const auto children = input->childItems();
        for (QQuickItem *child : children) {
            if (child->objectName() == QStringLiteral("composerSpellUnderline"))
                out << child;
        }
        return out;
    }

    // Puts `text` in the real field with the caret out of the way, then runs
    // the composer's own refresh and lets the delegates be created.
    void typeAndSettle(const QString &text, int cursor = 0)
    {
        QQuickItem *input = item(QStringLiteral("composerInput"));
        QVERIFY(input);
        input->setProperty("text", text);
        input->setProperty("cursorPosition", cursor);
        QMetaObject::invokeMethod(item(QStringLiteral("composerBar")),
                                  "refreshSpellUnderlines");
        QCoreApplication::processEvents();
    }

private slots:
    void initTestCase()
    {
        m_controller = new AppController(AppController::MockBackend);
        // A dictionary that knows "hello" and "world" and nothing else, so
        // every case below has exactly one predictable misspelling.
        auto *checker = qobject_cast<SpellChecker *>(
            m_controller->property("spell").value<QObject *>());
        QVERIFY2(checker, "app.spell is not a SpellChecker");
        checker->setBackendForTest(std::make_unique<FakeBackend>());
        QVERIFY(checker->available());

        m_engine = new QQmlEngine(this);
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("spellscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QCoreApplication::processEvents();
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void theComposerLoadsWithNoQmlWarnings()
    {
        QCOMPARE(m_warnings, QStringList{});
    }

    void aMisspelledWordGetsExactlyOneUnderlineUnderItself()
    {
        typeAndSettle(QStringLiteral("hello wrold"));
        const QList<QQuickItem *> marks = underlines();
        QCOMPARE(marks.size(), 1);

        // The mark must sit under THAT word: same left edge as the word's
        // first character, and as wide as the word. Nothing here is a
        // constant — it is read back out of the same TextArea that drew it.
        QQuickItem *input = item(QStringLiteral("composerInput"));
        QRectF head;
        QRectF tail;
        QMetaObject::invokeMethod(input, "positionToRectangle",
                                  Q_RETURN_ARG(QRectF, head), Q_ARG(int, 6));
        QMetaObject::invokeMethod(input, "positionToRectangle",
                                  Q_RETURN_ARG(QRectF, tail), Q_ARG(int, 11));
        QVERIFY2(tail.x() > head.x(), "the fixture laid out no text at all");
        QCOMPARE(marks.first()->x(), head.x());
        QCOMPARE(marks.first()->width(), tail.x() - head.x());
        // Under the line, not through it.
        QVERIFY(marks.first()->y() >= head.y());
        QVERIFY(marks.first()->y() <= head.y() + head.height());
        QCOMPARE(marks.first()->height(), 2.0);
    }

    void aCorrectlySpelledDraftIsNotMarkedAtAll()
    {
        typeAndSettle(QStringLiteral("hello world"));
        QCOMPARE(underlines().size(), 0);
    }

    void twoMisspellingsGetTwoSeparateMarks()
    {
        // LEADING SPACE, and it is not cosmetic. Caret 0 sits at the leading
        // edge of a word that starts at 0, which the caret rule correctly
        // treats as "still being typed" — so a draft beginning with a
        // misspelling shows one mark, not two. The space puts the caret in
        // whitespace, which is the state this case means to measure.
        typeAndSettle(QStringLiteral(" hallo wrold"));
        const QList<QQuickItem *> marks = underlines();
        QCOMPARE(marks.size(), 2);
        // Left to right and not overlapping: two marks on one line sitting on
        // top of each other would read as one.
        const qreal firstEnd = marks.at(0)->x() + marks.at(0)->width();
        QVERIFY(marks.at(0)->x() < marks.at(1)->x());
        QVERIFY(marks.at(1)->x() >= firstEnd);
    }

    void theWordUnderTheCaretIsNotMarkedInTheRealField()
    {
        // The suppression is policy, but THIS asserts the production path
        // actually passes the caret through: the same text, the same field,
        // only the caret moved.
        typeAndSettle(QStringLiteral("hello wrold"), 11);
        QCOMPARE(underlines().size(), 0);
        typeAndSettle(QStringLiteral("hello wrold"), 0);
        QCOMPARE(underlines().size(), 1);
    }

    void anUnavailableCheckerDrawsNothing()
    {
        auto *checker = qobject_cast<SpellChecker *>(
            m_controller->property("spell").value<QObject *>());
        QVERIFY(checker);
        typeAndSettle(QStringLiteral("hello wrold"));
        QCOMPARE(underlines().size(), 1);

        checker->setEnabled(false);
        typeAndSettle(QStringLiteral("hello wrold"));
        QCOMPARE(underlines().size(), 0);
        checker->setEnabled(true);

        checker->setBackendForTest({});
        QVERIFY(!checker->available());
        typeAndSettle(QStringLiteral("hello wrold"));
        QCOMPARE(underlines().size(), 0);
        // Restored for any case that runs after this one.
        checker->setBackendForTest(std::make_unique<FakeBackend>());
    }
};

int main(int argc, char *argv[])
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    ComposerSpellQmlTest tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "ComposerSpellQmlTest.moc"
