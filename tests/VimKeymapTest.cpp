#include "models/VimKeymap.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTest>

namespace {

// Applies results the way MessageComposerBar.qml does, with a snapshot stack
// standing in for the TextArea's undo history.
struct Editor {
    VimKeymap vim;
    QString text;
    int cursor = 0;
    QStringList undoStack;
    QStringList redoStack;
    int sends = 0;
    int lastUndo = 0;
    int lastRedo = 0;

    explicit Editor(const QString &marked, bool normal = false)
    {
        vim.setUseSystemClipboard(false);
        cursor = int(marked.indexOf(u'|'));
        text = marked;
        text.remove(cursor, 1);
        if (normal) {
            vim.enterNormal(text, cursor);
            cursor = vim.clampNormal(text, cursor);
        }
    }

    QString marked() const
    {
        QString out = text;
        out.insert(cursor, u'|');
        return out;
    }

    void escape()
    {
        VimKeymap::Result r = vim.handle(Qt::Key_Escape, Qt::NoModifier, QString(),
                                         text, cursor);
        if (!r.accepted && !vim.normal())
            cursor = vim.enterNormal(text, cursor);
    }

    VimKeymap::Result key(int k, Qt::KeyboardModifiers mods = Qt::NoModifier,
                          const QString &t = QString())
    {
        if (!vim.normal()) {
            VimKeymap::Result none;
            if (!t.isEmpty()) {
                undoStack << text;
                text.insert(cursor, t);
                cursor += int(t.size());
            }
            return none;
        }
        VimKeymap::Result r = vim.handle(k, mods, t, text, cursor);
        lastUndo = r.undo;
        lastRedo = r.redo;
        if (r.hasEdit) {
            undoStack << text;
            redoStack.clear();
            text.replace(r.editStart, r.editEnd - r.editStart, r.editText);
        }
        for (int i = 0; i < r.undo && !undoStack.isEmpty(); ++i) {
            redoStack << text;
            text = undoStack.takeLast();
        }
        for (int i = 0; i < r.redo && !redoStack.isEmpty(); ++i) {
            undoStack << text;
            text = redoStack.takeLast();
        }
        if (r.cursor >= 0)
            cursor = r.cursor;
        if (r.clampAfter)
            cursor = vim.clampNormal(text, qMin(cursor, int(text.size())));
        if (r.send)
            ++sends;
        return r;
    }

    // Each character is one key press; "<Esc>", "<CR>" and "<C-r>" are keys.
    void keys(const QString &seq)
    {
        for (int i = 0; i < seq.size(); ++i) {
            if (seq.mid(i).startsWith(QLatin1String("<Esc>"))) {
                escape();
                i += 4;
            } else if (seq.mid(i).startsWith(QLatin1String("<CR>"))) {
                key(Qt::Key_Return);
                i += 3;
            } else if (seq.mid(i).startsWith(QLatin1String("<C-r>"))) {
                key(Qt::Key_R, Qt::ControlModifier);
                i += 4;
            } else {
                const QChar c = seq.at(i);
                key(c.toUpper().unicode(), Qt::NoModifier, QString(c));
            }
        }
    }
};


} // namespace

class VimKeymapTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void startsInInsertAndIgnoresKeys()
    {
        VimKeymap vim;
        QVERIFY(!vim.normal());
        const auto r = vim.handle(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"),
                                  QStringLiteral("abc"), 1);
        QVERIFY(!r.accepted);
        QVERIFY(!r.hasEdit);
    }

    void escapeEntersNormalOneLeft()
    {
        Editor e(QStringLiteral("hello|"));
        QSignalSpy spy(&e.vim, &VimKeymap::modeChanged);
        e.escape();
        QVERIFY(e.vim.normal());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(e.marked(), QStringLiteral("hell|o"));

        Editor start(QStringLiteral("ab\n|cd"));
        start.escape();
        QCOMPARE(start.marked(), QStringLiteral("ab\n|cd"));
    }

    void normalModeNeverTypes()
    {
        Editor e(QStringLiteral("ab|c"), true);
        const QString before = e.text;
        for (const QChar c : QStringLiteral("qzZQ!@#%&*()[]{}<>;:'\",.?/\\|~`-=+")) {
            const auto r = e.key(c.unicode(), Qt::NoModifier, QString(c));
            QVERIFY2(r.accepted, qPrintable(QString(c)));
            QVERIFY2(!r.hasEdit, qPrintable(QString(c)));
        }
        QCOMPARE(e.text, before);
        QVERIFY(e.vim.normal());
    }

    void slashIsNotBound()
    {
        Editor e(QStringLiteral("a|b"), true);
        const auto r = e.key(Qt::Key_Slash, Qt::NoModifier, QStringLiteral("/"));
        QVERIFY(r.accepted);
        QVERIFY(!r.hasEdit);
        QVERIFY(e.vim.normal());
        QVERIFY(e.vim.pending().isEmpty());
        QCOMPARE(e.marked(), QStringLiteral("a|b"));
    }

    void chordsPassThroughExceptRedo()
    {
        Editor e(QStringLiteral("a|b"), true);
        QVERIFY(!e.key(Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03")).accepted);
        QVERIFY(!e.key(Qt::Key_K, Qt::ControlModifier).accepted);
        QVERIFY(!e.key(Qt::Key_X, Qt::AltModifier, QStringLiteral("x")).accepted);
        QVERIFY(!e.key(Qt::Key_Tab).accepted);
        QVERIFY(!e.key(Qt::Key_PageUp).accepted);
        QVERIFY(e.key(Qt::Key_R, Qt::ControlModifier).accepted);
    }

    void escapeClearsPendingThenPassesThrough()
    {
        Editor e(QStringLiteral("a|b"), true);
        e.keys(QStringLiteral("2d"));
        QCOMPARE(e.vim.pending(), QStringLiteral("2d"));
        auto r = e.vim.handle(Qt::Key_Escape, Qt::NoModifier, {}, e.text, e.cursor);
        QVERIFY(r.accepted);
        QVERIFY(e.vim.pending().isEmpty());
        r = e.vim.handle(Qt::Key_Escape, Qt::NoModifier, {}, e.text, e.cursor);
        QVERIFY(!r.accepted);
        QVERIFY(e.vim.normal());
    }

    void horizontalMotions_data()
    {
        QTest::addColumn<QString>("start");
        QTest::addColumn<QString>("keys");
        QTest::addColumn<QString>("expected");
        QTest::newRow("h") << "abc|d" << "h" << "ab|cd";
        QTest::newRow("h stops at line start") << "x\na|b" << "5h" << "x\n|ab";
        QTest::newRow("l") << "a|bcd" << "l" << "ab|cd";
        QTest::newRow("l stops on last char") << "a|bc\nd" << "9l" << "ab|c\nd";
        QTest::newRow("3l") << "|abcdef" << "3l" << "abc|def";
        QTest::newRow("0") << "x\n  ab|c" << "0" << "x\n|  abc";
        QTest::newRow("^") << "x\n  ab|c" << "^" << "x\n  |abc";
        QTest::newRow("$") << "a|bc\nde" << "$" << "ab|c\nde";
        QTest::newRow("2$") << "a|bc\nde\nf" << "2$" << "abc\nd|e\nf";
        QTest::newRow("arrows") << "a|bc" << "" << "a|bc";
    }
    void horizontalMotions()
    {
        QFETCH(QString, start);
        QFETCH(QString, keys);
        QFETCH(QString, expected);
        Editor e(start, true);
        e.keys(keys);
        QCOMPARE(e.marked(), expected);
        QVERIFY(e.vim.normal());
    }

    void wordMotions_data()
    {
        QTest::addColumn<QString>("start");
        QTest::addColumn<QString>("keys");
        QTest::addColumn<QString>("expected");
        QTest::newRow("w") << "|foo bar" << "w" << "foo |bar";
        QTest::newRow("w punctuation") << "|foo.bar" << "w" << "foo|.bar";
        QTest::newRow("w across line") << "fo|o\nbar" << "w" << "foo\n|bar";
        QTest::newRow("w stops on empty line") << "|foo\n\nbar" << "w" << "foo\n|\nbar";
        QTest::newRow("3w") << "|a b c d" << "3w" << "a b c |d";
        QTest::newRow("w at end stays on last char") << "foo b|ar" << "w" << "foo ba|r";
        QTest::newRow("b") << "foo ba|r" << "b" << "foo |bar";
        QTest::newRow("b from word start") << "foo |bar" << "b" << "|foo bar";
        QTest::newRow("b across line") << "foo\n|bar" << "b" << "|foo\nbar";
        QTest::newRow("2b") << "a b |c" << "2b" << "|a b c";
        QTest::newRow("e") << "|foo bar" << "e" << "fo|o bar";
        QTest::newRow("e from word end") << "fo|o bar" << "e" << "foo ba|r";
        QTest::newRow("2e") << "|foo bar baz" << "2e" << "foo ba|r baz";
    }
    void wordMotions()
    {
        QFETCH(QString, start);
        QFETCH(QString, keys);
        QFETCH(QString, expected);
        Editor e(start, true);
        e.keys(keys);
        QCOMPARE(e.marked(), expected);
    }

    void verticalMotions_data()
    {
        QTest::addColumn<QString>("start");
        QTest::addColumn<QString>("keys");
        QTest::addColumn<QString>("expected");
        QTest::newRow("j") << "ab|c\ndef" << "j" << "abc\nde|f";
        QTest::newRow("k") << "abc\nd|ef" << "k" << "a|bc\ndef";
        QTest::newRow("j clamps to short line") << "abc|d\nx" << "j" << "abcd\n|x";
        QTest::newRow("j keeps the column over a short line")
            << "ab|cd\nx\nefgh" << "jj" << "abcd\nx\nef|gh";
        QTest::newRow("2j") << "|a\nb\nc" << "2j" << "a\nb\n|c";
        QTest::newRow("j on last line") << "a\n|b" << "j" << "a\n|b";
        QTest::newRow("k on first line") << "|a\nb" << "k" << "|a\nb";
        QTest::newRow("gg") << "  a\nb\nc|c" << "gg" << "  |a\nb\ncc";
        QTest::newRow("G") << "|a\nb\n  c" << "G" << "a\nb\n  |c";
        QTest::newRow("2G") << "|a\n b\nc" << "2G" << "a\n |b\nc";
        QTest::newRow("3gg") << "|a\nb\nc\nd" << "3gg" << "a\nb\n|c\nd";
    }
    void verticalMotions()
    {
        QFETCH(QString, start);
        QFETCH(QString, keys);
        QFETCH(QString, expected);
        Editor e(start, true);
        e.keys(keys);
        QCOMPARE(e.marked(), expected);
    }

    void arrowKeysMove()
    {
        Editor e(QStringLiteral("ab|c\ndef"), true);
        e.key(Qt::Key_Left);
        QCOMPARE(e.marked(), QStringLiteral("a|bc\ndef"));
        e.key(Qt::Key_Down);
        QCOMPARE(e.marked(), QStringLiteral("abc\nd|ef"));
        e.key(Qt::Key_End);
        QCOMPARE(e.marked(), QStringLiteral("abc\nde|f"));
        e.key(Qt::Key_Home);
        QCOMPARE(e.marked(), QStringLiteral("abc\n|def"));
    }

    void edits_data()
    {
        QTest::addColumn<QString>("start");
        QTest::addColumn<QString>("keys");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<QString>("reg");
        QTest::newRow("x") << "a|bc" << "x" << "a|c" << "b";
        QTest::newRow("3x") << "a|bcde" << "3x" << "a|e" << "bcd";
        QTest::newRow("x at end of line moves left") << "a|b\nc" << "x" << "|a\nc" << "b";
        QTest::newRow("x keeps to the line") << "a|b\nc" << "5x" << "|a\nc" << "b";
        QTest::newRow("X") << "ab|c" << "X" << "a|c" << "b";
        QTest::newRow("2X") << "abc|d" << "2X" << "a|d" << "bc";
        QTest::newRow("dd") << "a\nb|b\nc" << "dd" << "a\n|c" << "bb\n";
        QTest::newRow("2dd") << "a\n|b\nc\nd" << "2dd" << "a\n|d" << "b\nc\n";
        QTest::newRow("d2d") << "a\n|b\nc\nd" << "d2d" << "a\n|d" << "b\nc\n";
        QTest::newRow("dd last line") << "a\n|b" << "dd" << "|a" << "b\n";
        QTest::newRow("dd only line") << "a|b" << "dd" << "|" << "ab\n";
        QTest::newRow("dd lands on first non-blank") << "|a\n  b" << "dd" << "  |b" << "a\n";
        QTest::newRow("dw") << "|foo bar" << "dw" << "|bar" << "foo ";
        QTest::newRow("dw last word of line") << "foo |bar\nbaz" << "dw" << "foo| \nbaz" << "bar";
        QTest::newRow("d2w") << "|a b c" << "d2w" << "|c" << "a b ";
        QTest::newRow("2dw") << "|a b c" << "2dw" << "|c" << "a b ";
        QTest::newRow("de") << "|foo bar" << "de" << "| bar" << "foo";
        QTest::newRow("db") << "foo ba|r" << "db" << "foo |r" << "ba";
        QTest::newRow("d$") << "a|bc\nd" << "d$" << "|a\nd" << "bc";
        QTest::newRow("D") << "a|bc\nd" << "D" << "|a\nd" << "bc";
        QTest::newRow("d0") << "ab|c" << "d0" << "|c" << "ab";
        QTest::newRow("dj") << "a\n|b\nc\nd" << "dj" << "a\n|d" << "b\nc\n";
        QTest::newRow("dk") << "a\nb\n|c\nd" << "dk" << "a\n|d" << "b\nc\n";
        QTest::newRow("dG") << "a\n|b\nc" << "dG" << "|a" << "b\nc\n";
        QTest::newRow("dgg") << "a\nb\n|c" << "dgg" << "|" << "a\nb\nc\n";
        QTest::newRow("dj on last line is a no-op") << "a\n|b" << "dj" << "a\n|b" << "";
        QTest::newRow("dh") << "ab|c" << "dh" << "a|c" << "b";
        QTest::newRow("dl") << "a|bc" << "dl" << "a|c" << "b";
    }
    void edits()
    {
        QFETCH(QString, start);
        QFETCH(QString, keys);
        QFETCH(QString, expected);
        QFETCH(QString, reg);
        Editor e(start, true);
        e.keys(keys);
        QCOMPARE(e.marked(), expected);
        QCOMPARE(e.vim.registerText(), reg);
        QVERIFY(e.vim.normal());
        QVERIFY(e.vim.pending().isEmpty());
    }

    void changes_data()
    {
        QTest::addColumn<QString>("start");
        QTest::addColumn<QString>("keys");
        QTest::addColumn<QString>("expected");
        QTest::newRow("cw") << "|foo bar" << "cwX" << "X| bar";
        QTest::newRow("cw on last char") << "fo|o bar" << "cwX" << "foX| bar";
        QTest::newRow("c2w") << "|a b c" << "c2wX" << "X| c";
        QTest::newRow("2cw") << "|a b c" << "2cwX" << "X| c";
        QTest::newRow("cc") << "a\n  b|b\nc" << "ccX" << "a\nX|\nc";
        QTest::newRow("2cc") << "a\n|b\nc\nd" << "2ccX" << "a\nX|\nd";
        QTest::newRow("C") << "a|bc" << "CX" << "aX|";
        QTest::newRow("C on empty line") << "a\n|\nb" << "CX" << "a\nX|\nb";
        QTest::newRow("c$") << "a|bc" << "c$X" << "aX|";
        QTest::newRow("cb") << "foo ba|r" << "cbX" << "foo X|r";
    }
    void changes()
    {
        QFETCH(QString, start);
        QFETCH(QString, keys);
        QFETCH(QString, expected);
        Editor e(start, true);
        e.keys(keys);
        QVERIFY(!e.vim.normal());
        QCOMPARE(e.marked(), expected);
    }

    void insertEntries_data()
    {
        QTest::addColumn<QString>("start");
        QTest::addColumn<QString>("keys");
        QTest::addColumn<QString>("expected");
        QTest::newRow("i") << "a|bc" << "iX" << "aX|bc";
        QTest::newRow("a") << "a|bc" << "aX" << "abX|c";
        QTest::newRow("a on empty line") << "x\n|" << "aX" << "x\nX|";
        QTest::newRow("I") << "  a|bc" << "IX" << "  X|abc";
        QTest::newRow("A") << "a|bc\nd" << "AX" << "abcX|\nd";
        QTest::newRow("o") << "a|b\nc" << "oX" << "ab\nX|\nc";
        QTest::newRow("O") << "a\nb|c" << "OX" << "a\nX|\nbc";
        QTest::newRow("o on last line") << "a|b" << "oX" << "ab\nX|";
    }
    void insertEntries()
    {
        QFETCH(QString, start);
        QFETCH(QString, keys);
        QFETCH(QString, expected);
        Editor e(start, true);
        QSignalSpy spy(&e.vim, &VimKeymap::modeChanged);
        e.keys(keys);
        QVERIFY(!e.vim.normal());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(e.marked(), expected);
    }

    void yankAndPut_data()
    {
        QTest::addColumn<QString>("start");
        QTest::addColumn<QString>("keys");
        QTest::addColumn<QString>("expected");
        QTest::newRow("yy p") << "|a\nb" << "yyp" << "a\n|a\nb";
        QTest::newRow("yy P") << "a\n|b" << "yyP" << "a\n|b\nb";
        QTest::newRow("yy 2p") << "|a" << "yy2p" << "a\n|a\na";
        QTest::newRow("2yy p") << "|a\nb\nc" << "2yyGp" << "a\nb\nc\n|a\nb";
        QTest::newRow("yy keeps the cursor") << "a|b\nc" << "yy" << "a|b\nc";
        QTest::newRow("yw p") << "|foo bar" << "ywp" << "ffoo| oo bar";
        QTest::newRow("yw P") << "foo |bar" << "bywP" << "foo| foo bar";
        QTest::newRow("ye moves nothing") << "|foo bar" << "ye" << "|foo bar";
        QTest::newRow("yb moves to the start") << "foo ba|r" << "yb" << "foo |bar";
        QTest::newRow("x p swaps") << "|ab" << "xp" << "b|a";
        QTest::newRow("dd p moves a line down") << "|a\nb" << "ddp" << "b\n|a";
        QTest::newRow("p into empty buffer") << "|x" << "ddp" << "\n|x";
        QTest::newRow("y$ P") << "a|bc" << "y$P" << "ab|cbc";
        QTest::newRow("yj p") << "|a\nb\nc" << "yjGp" << "a\nb\nc\n|a\nb";
        QTest::newRow("yk") << "a\nb|b" << "yk" << "|a\nbb";
    }
    void yankAndPut()
    {
        QFETCH(QString, start);
        QFETCH(QString, keys);
        QFETCH(QString, expected);
        Editor e(start, true);
        e.keys(keys);
        QCOMPARE(e.marked(), expected);
        QVERIFY(e.vim.normal());
    }

    void putWithEmptyRegisterDoesNothing()
    {
        Editor e(QStringLiteral("a|b"), true);
        const auto r = e.key(Qt::Key_P, Qt::NoModifier, QStringLiteral("p"));
        QVERIFY(r.accepted);
        QVERIFY(!r.hasEdit);
    }

    void undoAndRedoUseTheEditorStack()
    {
        Editor e(QStringLiteral("|a b c"), true);
        e.keys(QStringLiteral("dwdw"));
        QCOMPARE(e.text, QStringLiteral("c"));
        e.keys(QStringLiteral("u"));
        QCOMPARE(e.lastUndo, 1);
        QCOMPARE(e.text, QStringLiteral("b c"));
        e.keys(QStringLiteral("u"));
        QCOMPARE(e.text, QStringLiteral("a b c"));
        e.keys(QStringLiteral("<C-r>"));
        QCOMPARE(e.lastRedo, 1);
        QCOMPARE(e.text, QStringLiteral("b c"));
        e.keys(QStringLiteral("u2<C-r>"));
        QCOMPARE(e.lastRedo, 2);
        QCOMPARE(e.text, QStringLiteral("c"));
        e.keys(QStringLiteral("2u"));
        QCOMPARE(e.lastUndo, 2);
        QCOMPARE(e.text, QStringLiteral("a b c"));
        QVERIFY(e.vim.normal());
    }

    void enterSendsAndReturnsToInsert()
    {
        Editor e(QStringLiteral("hi|"), true);
        e.keys(QStringLiteral("<CR>"));
        QCOMPARE(e.sends, 1);
        QVERIFY(!e.vim.normal());
        QCOMPARE(e.text, QStringLiteral("hi"));
    }

    void countsAccumulateAndShowAsPending()
    {
        Editor e(QStringLiteral("|a b c d e f g h i j k l"), true);
        QSignalSpy spy(&e.vim, &VimKeymap::pendingChanged);
        e.keys(QStringLiteral("1"));
        QCOMPARE(e.vim.pending(), QStringLiteral("1"));
        e.keys(QStringLiteral("0"));
        QCOMPARE(e.vim.pending(), QStringLiteral("10"));
        e.keys(QStringLiteral("w"));
        QVERIFY(e.vim.pending().isEmpty());
        QCOMPARE(e.marked(), QStringLiteral("a b c d e f g h i j |k l"));
        QVERIFY(spy.count() >= 3);
        e.keys(QStringLiteral("g"));
        QCOMPARE(e.vim.pending(), QStringLiteral("g"));
        e.keys(QStringLiteral("g"));
        QVERIFY(e.vim.pending().isEmpty());
        QCOMPARE(e.cursor, 0);
    }

    void unknownKeyCancelsAPendingOperator()
    {
        Editor e(QStringLiteral("|abc"), true);
        e.keys(QStringLiteral("dz"));
        QVERIFY(e.vim.pending().isEmpty());
        e.keys(QStringLiteral("l"));
        QCOMPARE(e.marked(), QStringLiteral("a|bc"));
        e.keys(QStringLiteral("gx"));
        QVERIFY(e.vim.pending().isEmpty());
        QCOMPARE(e.text, QStringLiteral("abc"));
    }

    void enterInsertResets()
    {
        Editor e(QStringLiteral("|abc"), true);
        e.keys(QStringLiteral("3d"));
        e.vim.enterInsert();
        QVERIFY(!e.vim.normal());
        QVERIFY(e.vim.pending().isEmpty());
    }

    void registerMirrorsTheSystemClipboard()
    {
        QClipboard *clipboard = QGuiApplication::clipboard();
        QVERIFY(clipboard);
        clipboard->setText(QString());
        Editor e(QStringLiteral("|alpha beta"), true);
        e.vim.setUseSystemClipboard(true);
        e.keys(QStringLiteral("yw"));
        if (clipboard->text() != QStringLiteral("alpha "))
            QSKIP("this platform has no readable clipboard");
        clipboard->setText(QStringLiteral("XY"));
        e.keys(QStringLiteral("$p"));
        QCOMPARE(e.marked(), QStringLiteral("alpha betaX|Y"));
    }

    void richInsertHtmlKeepsTextLiteral()
    {
        const QString html = VimKeymap::richInsertHtml(QStringLiteral("a  <b>\nc&"));
        QVERIFY(html.contains(QStringLiteral("a  &lt;b&gt;<br>c&amp;")));
        QVERIFY(html.contains(QStringLiteral("white-space:pre-wrap")));
    }
};

QTEST_MAIN(VimKeymapTest)
#include "VimKeymapTest.moc"
