// The spell underline, proven against the production composer: loads the real
// MessageComposerBar.qml, puts a misspelling in the real TextArea and asks the
// item tree what was drawn and where. Calling the policy directly would not
// show whether production reaches it. Whether a real dictionary agrees is
// `--spell-status`'s job; a fake backend keeps geometry the only variable.

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
    QString language() const override { return QStringLiteral("en-US"); }
    QStringList availableLanguages() const override
    { return { QStringLiteral("en-US") }; }
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

    // Repeater delegates live only in the visual tree, which findChild cannot
    // see; walk childItems().
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

    // Puts `text` in the real field with the caret out of the way, runs the
    // composer's refresh and lets the delegates be created.
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
        // A dictionary that knows only "hello" and "world", so each case has
        // one predictable misspelling.
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

    // The rich-only toolbar chips are no wider than the icon buttons beside
    // them (not AppButton's 72px minimum), measured on the real bar.
    void theRichToolbarChipsAreNoWiderThanTheIconButtonsBesideThem()
    {
        auto *bar = item(QStringLiteral("composerBar"));
        QVERIFY(bar);
        m_controller->settings()->setComposerMode(QStringLiteral("rich"));
        bar->setProperty("toolbarExpanded", true);
        // Layouts settle on the polish pass, which offscreen runs only on
        // update; processEvents alone reads the old frame.
        QTest::qWait(60);
        QCoreApplication::processEvents();

        auto *bold = item(QStringLiteral("composerFormat_bold"));
        auto *underline = item(QStringLiteral("composerFormat_underline"));
        auto *ordered = item(QStringLiteral("composerFormat_orderedlist"));
        QVERIFY(bold);
        QVERIFY(underline);
        QVERIFY(ordered);
        QVERIFY2(underline->isVisible() && ordered->isVisible(),
                 "the rich-only chips are not shown in rich mode");
        // The icon buttons are 28px; a chip wider than 1.5x that is the 72px
        // box returning.
        const qreal ceiling = bold->width() * 1.5;
        QVERIFY2(underline->width() <= ceiling,
                 qPrintable(QStringLiteral("underline chip is %1px beside a %2px "
                                           "icon button").arg(underline->width())
                                .arg(bold->width())));
        QVERIFY2(ordered->width() <= ceiling,
                 qPrintable(QStringLiteral("numbered-list chip is %1px beside a "
                                           "%2px icon button").arg(ordered->width())
                                .arg(bold->width())));
        m_controller->settings()->setComposerMode(QStringLiteral("markdown"));
        bar->setProperty("toolbarExpanded", false);
        QTest::qWait(60);
    }

    // A list marker in the rich editor clears the placeholder: an empty list
    // item has no characters, so a character count alone is not enough.
    void aListMarkerInTheRichEditorClearsThePlaceholder()
    {
        auto *bar = item(QStringLiteral("composerBar"));
        QVERIFY(bar);
        m_controller->setCurrentRoomId(QStringLiteral("!general:example.org"));
        m_controller->settings()->setComposerMode(QStringLiteral("rich"));
        bar->setProperty("toolbarExpanded", true);
        QTest::qWait(60);
        QCoreApplication::processEvents();

        auto *rich = item(QStringLiteral("composerRichInput"));
        QVERIFY(rich);
        const QString placeholderWhenEmpty =
            rich->property("placeholderText").toString();
        QVERIFY2(!placeholderWhenEmpty.isEmpty(),
                 "an empty rich editor should still offer its placeholder");

        QMetaObject::invokeMethod(bar, "applyRichFormat",
                                  Q_ARG(QVariant, QStringLiteral("orderedlist")),
                                  Q_ARG(QVariant, QString()));
        QTest::qWait(60);
        QCoreApplication::processEvents();
        QCOMPARE(rich->property("length").toInt(), 0); // no characters typed
        QVERIFY2(rich->property("placeholderText").toString().isEmpty(),
                 qPrintable(QStringLiteral(
                     "the list marker is drawn but the placeholder \"%1\" is "
                     "still shown under it")
                     .arg(rich->property("placeholderText").toString())));

        // Removing the list restores the placeholder.
        QMetaObject::invokeMethod(bar, "applyRichFormat",
                                  Q_ARG(QVariant, QStringLiteral("orderedlist")),
                                  Q_ARG(QVariant, QString()));
        QTest::qWait(60);
        QCoreApplication::processEvents();
        QCOMPARE(rich->property("placeholderText").toString(), placeholderWhenEmpty);
        m_controller->settings()->setComposerMode(QStringLiteral("markdown"));
        bar->setProperty("toolbarExpanded", false);
        QTest::qWait(60);
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

        // The mark sits under that word: same left edge as its first character
        // and as wide as the word, read back from the TextArea that drew it.
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
        // Leading space on purpose: caret 0 at the start of a word counts as
        // still typing, which would hide the first mark. The space puts the
        // caret in whitespace.
        typeAndSettle(QStringLiteral(" hallo wrold"));
        const QList<QQuickItem *> marks = underlines();
        QCOMPARE(marks.size(), 2);
        // Left to right and not overlapping.
        const qreal firstEnd = marks.at(0)->x() + marks.at(0)->width();
        QVERIFY(marks.at(0)->x() < marks.at(1)->x());
        QVERIFY(marks.at(1)->x() >= firstEnd);
    }

    void theWordUnderTheCaretIsNotMarkedInTheRealField()
    {
        // The production path passes the caret through: same text, same
        // field, only the caret moved.
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
        // Restored for later cases.
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
