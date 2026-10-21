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
#include <QScopeGuard>
#include <QTemporaryDir>

#include <memory>

#include "app/AppController.h"
#include "storage/AppDataPaths.h"
#include "storage/PortableMode.h"
#include "text/SpellBackend.h"
#include "text/SpellChecker.h"

namespace {

class FakeBackend : public SpellBackend
{
public:
    bool isCorrect(const QString &word) const override
    {
        return word.compare(QStringLiteral("hello"), Qt::CaseInsensitive) == 0
            || word.compare(QStringLiteral("world"), Qt::CaseInsensitive) == 0
            || word.compare(QStringLiteral("say"), Qt::CaseInsensitive) == 0
            || word.compare(QStringLiteral("the"), Qt::CaseInsensitive) == 0
            || word.compare(QStringLiteral("and"), Qt::CaseInsensitive) == 0
            || word.compare(QStringLiteral("that"), Qt::CaseInsensitive) == 0;
    }
    QStringList suggest(const QString &) const override
    {
        return { QStringLiteral("world") };
    }
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

    // ---- where the personal dictionary lives ----

    // Under the app-data root every other file uses (FontManager's imported
    // fonts), not QStandardPaths' AppDataLocation: on a portable Windows
    // install that one escapes the portable folder. And this suite never
    // touches the developer's real one (HermeticDataHome.cpp).
    void thePersonalDictionaryLivesUnderTheAppDataRoot()
    {
        const QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
        QVERIFY2(dataHome.contains(QStringLiteral("lightning-test-data-")),
                 qPrintable(QStringLiteral("not hermetic: XDG_DATA_HOME=%1").arg(dataHome)));
        const QString path = checker()->personalDictionaryPath();
        QCOMPARE(path, matrix::app_data::primaryRoot()
                           + QStringLiteral("/spelling/personal-dictionary.txt"));
        QVERIFY(path.startsWith(dataHome + QLatin1Char('/')));

        // Portable: inside the portable folder, wherever XDG_DATA_HOME points.
        QTemporaryDir exeDir;
        QVERIFY(exeDir.isValid());
        lightning::portable::setPortableOverrideForTest(true, exeDir.path());
        // A failed QVERIFY returns early: the override must not outlive this
        // case either way.
        auto clearPortable = qScopeGuard(
            [] { lightning::portable::clearPortableOverrideForTest(); });
        QString portablePath;
        {
            AppController portable(AppController::MockBackend);
            auto *spell = qobject_cast<SpellChecker *>(
                portable.property("spell").value<QObject *>());
            QVERIFY(spell);
            portablePath = spell->personalDictionaryPath();
        }
        clearPortable.dismiss();
        lightning::portable::clearPortableOverrideForTest();
        QVERIFY2(portablePath.startsWith(exeDir.path() + QLatin1Char('/')),
                 qPrintable(QStringLiteral("portable dictionary escaped the folder: %1")
                                .arg(portablePath)));
        QVERIFY(portablePath.endsWith(QStringLiteral("/spelling/personal-dictionary.txt")));
    }

    // ---- autocorrect, through real keystrokes ----

    SpellChecker *checker() const
    {
        return qobject_cast<SpellChecker *>(
            m_controller->property("spell").value<QObject *>());
    }

    // An empty, focused field in the given composer mode.
    QQuickItem *focusedEditor(bool rich)
    {
        m_controller->setCurrentRoomId(QStringLiteral("!general:example.org"));
        m_controller->settings()->setComposerMode(rich ? QStringLiteral("rich")
                                                       : QStringLiteral("markdown"));
        QTest::qWait(60);
        QCoreApplication::processEvents();
        QQuickItem *editor = item(rich ? QStringLiteral("composerRichInput")
                                       : QStringLiteral("composerInput"));
        if (!editor)
            return nullptr;
        if (rich)
            QMetaObject::invokeMethod(editor, "clear");
        else
            editor->setProperty("text", QString());
        m_window->requestActivate();
        editor->forceActiveFocus();
        QCoreApplication::processEvents();
        return editor;
    }

    // QTest::keyClicks has no QWindow overload; ASCII is all these need.
    void typeKeys(const QString &text)
    {
        for (const QChar c : text)
            QTest::keyClick(m_window, c.toLatin1());
    }

    static QString plainTextOf(QQuickItem *editor)
    {
        QString out;
        const int length = editor->property("length").toInt();
        QMetaObject::invokeMethod(editor, "getText", Q_RETURN_ARG(QString, out),
                                  Q_ARG(int, 0), Q_ARG(int, length));
        return out;
    }

    void aTypoIsCorrectedAtTheSpaceAndBackspacePutsItBack_data()
    {
        QTest::addColumn<bool>("rich");
        QTest::addColumn<QString>("typo");
        QTest::addColumn<QString>("fix");
        // Different words per row: Backspace refuses its word for the whole
        // session, and both rows share the one checker.
        QTest::newRow("markdown") << false << QStringLiteral("teh") << QStringLiteral("the");
        QTest::newRow("rich") << true << QStringLiteral("taht") << QStringLiteral("that");
    }

    void aTypoIsCorrectedAtTheSpaceAndBackspacePutsItBack()
    {
        QFETCH(bool, rich);
        QFETCH(QString, typo);
        QFETCH(QString, fix);
        SpellChecker *spell = checker();
        QVERIFY(spell);
        spell->setAutocorrectEnabled(true);
        QQuickItem *editor = focusedEditor(rich);
        QVERIFY(editor);
        QVERIFY2(editor->hasActiveFocus(), "the field did not take focus");

        typeKeys(QStringLiteral("say ") + typo);
        QCOMPARE(plainTextOf(editor), QStringLiteral("say ") + typo);
        // Not before the word is finished...
        QTest::keyClick(m_window, Qt::Key_Space);
        // ...and then corrected in place, the space kept, the caret after it.
        const QString corrected = QStringLiteral("say ") + fix + QLatin1Char(' ');
        QCOMPARE(plainTextOf(editor), corrected);
        QCOMPARE(editor->property("cursorPosition").toInt(), int(corrected.size()));

        // Backspace straight after restores what was typed, keeps the space.
        QTest::keyClick(m_window, Qt::Key_Backspace);
        const QString restored = QStringLiteral("say ") + typo + QLatin1Char(' ');
        QCOMPARE(plainTextOf(editor), restored);
        QCOMPARE(editor->property("cursorPosition").toInt(), int(restored.size()));
        // And that word is left alone from now on.
        typeKeys(typo + QLatin1Char(' '));
        QCOMPARE(plainTextOf(editor), restored + typo + QLatin1Char(' '));

        m_controller->settings()->setComposerMode(QStringLiteral("markdown"));
        spell->setAutocorrectEnabled(false);
        QTest::qWait(60);
    }

    void ctrlZRightAfterACorrectionUndoesItButALaterOneDoesNot()
    {
        SpellChecker *spell = checker();
        QVERIFY(spell);
        QQuickItem *editor = focusedEditor(false);
        QVERIFY(editor);
        spell->setAutocorrectEnabled(true);
        // No table entry for "wrold": the fake engine's best (and only)
        // suggestion "world" is one transposition away, so it is corrected.
        typeKeys(QStringLiteral("hello wrold"));
        QTest::keyClick(m_window, Qt::Key_Space);
        QCOMPARE(plainTextOf(editor), QStringLiteral("hello world "));
        QTest::keyClick(m_window, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(plainTextOf(editor), QStringLiteral("hello wrold "));
        // Typing on ends the undo window: a later Backspace is a Backspace.
        typeKeys(QStringLiteral("say "));
        QTest::keyClick(m_window, Qt::Key_Backspace);
        QCOMPARE(plainTextOf(editor), QStringLiteral("hello wrold say"));
        spell->setAutocorrectEnabled(false);
        editor->setProperty("text", QString());
    }

    void nothingIsCorrectedWhenOffOrWhenTextArrivesAllAtOnce()
    {
        SpellChecker *spell = checker();
        QVERIFY(spell);
        QQuickItem *editor = focusedEditor(false);
        QVERIFY(editor);
        // Off (the default on Linux and Windows): typed through.
        spell->setAutocorrectEnabled(false);
        typeKeys(QStringLiteral("adn "));
        QCOMPARE(plainTextOf(editor), QStringLiteral("adn "));
        // The control: on, typed key by key, it IS corrected.
        spell->setAutocorrectEnabled(true);
        editor->setProperty("text", QString());
        typeKeys(QStringLiteral("adn "));
        QCOMPARE(plainTextOf(editor), QStringLiteral("and "));
        // On, but the text arrives in one piece (a paste, a restored draft):
        // never corrected.
        editor->setProperty("text", QStringLiteral("so adn "));
        editor->setProperty("cursorPosition", 7);
        QCoreApplication::processEvents();
        QCOMPARE(plainTextOf(editor), QStringLiteral("so adn "));
        // Inside inline code, typed key by key: left alone.
        editor->setProperty("text", QString());
        typeKeys(QStringLiteral("run `adn "));
        QCOMPARE(plainTextOf(editor), QStringLiteral("run `adn "));
        spell->setAutocorrectEnabled(false);
        editor->setProperty("text", QString());
    }

    // A draft longer than the un-windowed limit used to get no checking at
    // all; now the visible part is checked and only that.
    void aLongDraftIsCheckedWhereTheUserIsLooking()
    {
        QQuickItem *editor = focusedEditor(false);
        QVERIFY(editor);
        const QString text = QStringLiteral("hello wrold\n").repeated(4000);
        QVERIFY(text.size() > SpellChecker::kMaxCheckedChars);
        typeAndSettle(text, 0);
        const QList<QQuickItem *> marks = underlines();
        QVERIFY2(!marks.isEmpty(), "a long draft is not checked at all");
        QVERIFY(marks.size() <= SpellChecker::kMaxReportedRanges);
        // Only around the viewport: nothing far below it.
        QQuickItem *flick = item(QStringLiteral("composerInputFlick"));
        QVERIFY(flick);
        const qreal bottom = flick->property("contentY").toReal()
            + 3 * flick->height() + 40;
        for (QQuickItem *mark : marks)
            QVERIFY2(mark->y() <= bottom,
                     qPrintable(QStringLiteral("mark at y=%1, viewport ends near %2")
                                    .arg(mark->y()).arg(bottom)));
        editor->setProperty("text", QString());
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
