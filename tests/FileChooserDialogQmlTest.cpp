// Lightning's own file chooser (FileChooserDialog.qml), loaded for real
// offscreen against a real FileChooser (app.files) whose environment is
// Linux without a portal: exactly the route the 2026-10-15 report took.
//
// The report, with a screenshot: Save as on Linux without a portal came up
// in a dialog that ignored the theme (a white path bar on a dark theme) with
// its File name field EMPTY. That dialog was Qt Quick's non-native
// FileDialog, which fills the field only from a file that already exists;
// HEAD had moved to Qt's widget QFileDialog, still unthemed. Pins:
//   * Save as prefills the name and pre-selects the base name, with the
//     field focused, so typing replaces "report" and keeps ".pdf";
//   * an existing file is never replaced without a confirmation, and
//     declining it answers nothing;
//   * typing a folder's name enters it and keeps the file's name;
//   * a filter naming one extension changes the name's extension;
//   * open, openMany and folder answer what was chosen, through FileChooser;
//   * Escape and Cancel answer "cancelled"; Backspace goes up;
//   * every surface of it follows the theme, light and dark alike;
//   * openMany chooses like a desktop dialog (2026-10-16 break-it rerun,
//     4/4: Ctrl+click and Shift+click chose nothing and Open did nothing),
//     driven with REAL clicks and keys on a settled, focused list, because
//     the defect lived in a handler that only ran with the list focused and
//     the listing settled, and a case calling clickRow() straight after a
//     listing ran before either and passed on the broken code.
//
// Nothing is shown on a screen: QT_QPA_PLATFORM=offscreen, software Quick.

#include "models/FileBrowserModel.h"
#include "app/FileChooser.h"

#include <QDir>
#include <QFile>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <functional>

#include <cmath>

namespace {

// Only what the chooser reads from `app`.
class StubApp final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *files READ files CONSTANT)
public:
    explicit StubApp(FileChooser *files) : m_files(files) {}
    QObject *files() const { return m_files; }

private:
    FileChooser *m_files;
};

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 1000
    height: 760
    visible: true
    color: AppTheme.background
    property int themeMode: 9
    Binding { target: AppTheme; property: "mode"; value: win.themeMode }
    FileChooserDialog {
        objectName: "chooserUnderTest"
    }
}
)QML";

void touch(const QString &path)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
}

double luminance(const QColor &c)
{
    const auto lin = [](double v) {
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF())
        + 0.0722 * lin(c.blueF());
}

double contrast(const QColor &a, const QColor &b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace

class FileChooserDialogQmlTest : public QObject
{
    Q_OBJECT

private:
    FileChooser *m_chooser = nullptr;
    StubApp *m_app = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QObject *m_dialog = nullptr;
    FileBrowserModel *m_model = nullptr;
    QStringList m_warnings;
    QList<QVariantList> m_answers;

    QObject *child(const char *name) const
    {
        if (QObject *hit = m_dialog->findChild<QObject *>(QLatin1String(name)))
            return hit;
        return m_root->findChild<QObject *>(QLatin1String(name));
    }

    // Opens a request through FileChooser, the way NativeFileDialog does,
    // and waits for the dialog and its first listing.
    int request(const QVariantMap &options)
    {
        const int id = m_chooser->open(options);
        [&] {
            QTRY_VERIFY(m_dialog->property("opened").toBool());
            QTRY_COMPARE(m_dialog->property("requestId").toInt(), id);
            QTRY_VERIFY(!m_model->loading());
        }();
        return id;
    }

    // The dialog's JS functions, which take and return QVariant.
    bool call(const char *method)
    {
        return QMetaObject::invokeMethod(m_dialog, method);
    }
    bool call(const char *method, const QVariant &a)
    {
        return QMetaObject::invokeMethod(m_dialog, method, Q_ARG(QVariant, a));
    }
    bool call(const char *method, const QVariant &a, const QVariant &b)
    {
        return QMetaObject::invokeMethod(m_dialog, method, Q_ARG(QVariant, a),
                                         Q_ARG(QVariant, b));
    }

    QVariantList lastAnswer() const
    {
        return m_answers.isEmpty() ? QVariantList() : m_answers.last();
    }

    QList<QUrl> answerUrls() const
    {
        return lastAnswer().value(2).value<QList<QUrl>>();
    }

    QQuickItem *list() const
    {
        return qobject_cast<QQuickItem *>(child("fileChooserList"));
    }

    QQuickItem *rowItem(int row) const
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(list(), "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(int, row));
        return item;
    }

    // The listing has arrived AND the dialog has finished handling it
    // (afterReset runs a turn later), and the list holds the keyboard: the
    // state a reader's clicks and keys actually meet.
    void settle()
    {
        QTRY_VERIFY(!m_model->loading());
        QTRY_VERIFY(!m_dialog->property("resetting").toBool());
        QTRY_VERIFY(list()->hasActiveFocus());
    }

    void clickRowItem(int row, Qt::KeyboardModifiers modifiers = {})
    {
        QQuickItem *item = rowItem(row);
        QVERIFY2(item, qPrintable(QStringLiteral("no delegate for row %1").arg(row)));
        const QPointF centre = item->mapToScene(
            QPointF(item->width() / 2, item->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, modifiers, centre.toPoint());
    }

    // What the rows DRAW as selected, read off every delegate; the count of
    // delegates read is checked so a short read cannot pass.
    QStringList highlighted() const
    {
        QStringList names;
        int read = 0;
        for (int i = 0; i < m_model->rowCount(); ++i) {
            QQuickItem *item = rowItem(i);
            if (!item)
                continue;
            ++read;
            if (item->property("selected").toBool())
                names << item->property("name").toString();
        }
        if (read != m_model->rowCount())
            names << QStringLiteral("<read %1 of %2 rows>")
                         .arg(read).arg(m_model->rowCount());
        return names;
    }

    bool acceptEnabled() const
    {
        return child("fileChooserAccept")->property("enabled").toBool();
    }

    void clickAccept()
    {
        auto *button = qobject_cast<QQuickItem *>(child("fileChooserAccept"));
        QVERIFY(button);
        const QPointF centre = button->mapToScene(
            QPointF(button->width() / 2, button->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, {}, centre.toPoint());
    }

    QList<QUrl> urlsIn(const QString &dir, const QStringList &names) const
    {
        QList<QUrl> urls;
        for (const QString &n : names)
            urls << QUrl::fromLocalFile(QDir::cleanPath(dir) + QLatin1Char('/') + n);
        return urls;
    }

    void closeAnyRequest()
    {
        if (m_dialog->property("requestId").toInt() >= 0)
            call("cancel");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
    }

private Q_SLOTS:
    void initTestCase()
    {
        FileChooser::Environment env;
        env.linuxDesktop = true; // no session bus: no portal to try
        m_chooser = new FileChooser(env, this);
        QCOMPARE(m_chooser->route(), FileChooser::Route::InApp);
        connect(m_chooser, &FileChooser::finished, this,
                [this](int id, bool ok, const QList<QUrl> &urls,
                       const QString &filter) {
                    m_answers.append(QVariantList{
                        id, ok, QVariant::fromValue(urls), filter});
                });
        m_app = new StubApp(m_chooser);
        m_engine = new QQmlEngine(this);
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const QQmlError &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_app);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("filechooserscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(m_window));
        m_dialog = m_root->findChild<QObject *>(
            QStringLiteral("chooserUnderTest"));
        QVERIFY(m_dialog);
        m_model = qobject_cast<FileBrowserModel *>(child("fileChooserModel"));
        QVERIFY(m_model);
    }

    void init()
    {
        // Each case counts only its own answers (cleanup() cancels whatever
        // the previous case left open, which is an answer too).
        m_answers.clear();
    }

    void cleanup()
    {
        closeAnyRequest();
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_app;
    }

    void loadsWithoutWarningsOfItsOwn()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        request({{QStringLiteral("mode"), QStringLiteral("save")},
                 {QStringLiteral("currentName"), QStringLiteral("a.txt")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QStringList mine;
        for (const QString &w : std::as_const(m_warnings)) {
            if (w.contains(QStringLiteral("FileChooserDialog")))
                mine << w;
        }
        QVERIFY2(mine.isEmpty(), qPrintable(mine.join(QLatin1Char('\n'))));
    }

    // The report's own case.
    void saveAsPrefillsTheNameWithTheBaseNameSelected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        request({{QStringLiteral("mode"), QStringLiteral("save")},
                 {QStringLiteral("title"), QStringLiteral("Save file")},
                 {QStringLiteral("currentName"),
                  QStringLiteral("Quarterly report.pdf")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())},
                 {QStringLiteral("nameFilters"),
                  QStringList{QStringLiteral("PDF file (*.pdf)"),
                              QStringLiteral("All files (*)")}}});
        QObject *name = child("fileChooserName");
        QVERIFY(name);
        QVERIFY(name->property("visible").toBool());
        QCOMPARE(name->property("text").toString(),
                 QStringLiteral("Quarterly report.pdf"));
        QTRY_VERIFY(name->property("activeFocus").toBool());
        QTRY_COMPARE(name->property("selectedText").toString(),
                     QStringLiteral("Quarterly report"));
        QCOMPARE(name->property("selectionStart").toInt(), 0);

        // Typing replaces the base name and keeps the extension.
        QTest::keyClick(m_window, 'Q');
        QTest::keyClick(m_window, '3');
        QCOMPARE(name->property("text").toString(), QStringLiteral("Q3.pdf"));
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(lastAnswer().value(1).toBool(), true);
        QCOMPARE(answerUrls(),
                 QList<QUrl>{QUrl::fromLocalFile(
                     QDir::cleanPath(dir.path()) + QStringLiteral("/Q3.pdf"))});
        QCOMPARE(lastAnswer().value(3).toString(),
                 QStringLiteral("PDF file (*.pdf)"));
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        m_answers.clear();
    }

    void anExistingFileIsReplacedOnlyAfterConfirmation()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        touch(dir.path() + QStringLiteral("/report.pdf"));
        request({{QStringLiteral("mode"), QStringLiteral("save")},
                 {QStringLiteral("currentName"), QStringLiteral("report.pdf")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QObject *confirm = child("fileChooserOverwriteDialog");
        QVERIFY(confirm);
        QVERIFY(call("acceptCurrent"));
        QTRY_VERIFY(confirm->property("opened").toBool());
        QVERIFY(m_answers.isEmpty());

        // Declining answers nothing and leaves the chooser up.
        QObject *keep = child("fileChooserKeep");
        QVERIFY(keep);
        QVERIFY(QMetaObject::invokeMethod(keep, "clicked"));
        QTRY_VERIFY(!confirm->property("visible").toBool());
        QVERIFY(m_answers.isEmpty());
        QVERIFY(m_dialog->property("visible").toBool());

        QVERIFY(call("acceptCurrent"));
        QTRY_VERIFY(confirm->property("opened").toBool());
        QObject *replace = child("fileChooserReplace");
        QVERIFY(replace);
        QVERIFY(QMetaObject::invokeMethod(replace, "clicked"));
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(lastAnswer().value(1).toBool(), true);
        QCOMPARE(answerUrls(),
                 QList<QUrl>{QUrl::fromLocalFile(QDir::cleanPath(dir.path())
                                                 + QStringLiteral("/report.pdf"))});
        m_answers.clear();
    }

    void typingAFolderNameEntersItAndKeepsTheFileName()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("Invoices")));
        request({{QStringLiteral("mode"), QStringLiteral("save")},
                 {QStringLiteral("currentName"), QStringLiteral("bill.pdf")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QObject *name = child("fileChooserName");
        name->setProperty("text", QStringLiteral("Invoices"));
        QVERIFY(call("acceptCurrent"));
        QTRY_COMPARE(m_model->folder(),
                     QUrl::fromLocalFile(QDir::cleanPath(dir.path())
                                         + QStringLiteral("/Invoices")));
        QCOMPARE(name->property("text").toString(), QStringLiteral("bill.pdf"));
        QVERIFY(m_answers.isEmpty());
    }

    void aSingleExtensionFilterChangesTheExtension()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        request({{QStringLiteral("mode"), QStringLiteral("save")},
                 {QStringLiteral("currentName"), QStringLiteral("photo.png")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())},
                 {QStringLiteral("nameFilters"),
                  QStringList{QStringLiteral("PNG image (*.png)"),
                              QStringLiteral("JPEG image (*.jpg)"),
                              QStringLiteral("All files (*)")}}});
        QVERIFY(call("chooseFilter", 1));
        QCOMPARE(child("fileChooserName")->property("text").toString(),
                 QStringLiteral("photo.jpg"));
        QCOMPARE(m_model->nameFilters(), QStringList{QStringLiteral("*.jpg")});
        // "All files" names no extension: the name stays.
        QVERIFY(call("chooseFilter", 2));
        QCOMPARE(child("fileChooserName")->property("text").toString(),
                 QStringLiteral("photo.jpg"));
    }

    void openReturnsTheChosenFileAndEntersFolders()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("sub")));
        touch(dir.path() + QStringLiteral("/sub/inner.txt"));
        touch(dir.path() + QStringLiteral("/a.txt"));
        touch(dir.path() + QStringLiteral("/b.png"));
        request({{QStringLiteral("mode"), QStringLiteral("open")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())},
                 {QStringLiteral("nameFilters"),
                  QStringList{QStringLiteral("Text (*.txt)")}}});
        // The filter hides b.png; the folder stays.
        QTRY_COMPARE(m_model->rowCount(), 2);
        QObject *accept = child("fileChooserAccept");
        QVERIFY(accept);
        // Nothing chosen yet: nothing to open.
        QVERIFY(!accept->property("enabled").toBool());

        // Enter on a folder opens it.
        QVERIFY(call("activateRow", m_model->indexOfName(QStringLiteral("sub"))));
        QTRY_COMPARE(m_model->indexOfName(QStringLiteral("inner.txt")), 0);
        // Backspace goes back up, to the folder just left.
        QObject *list = child("fileChooserList");
        QVERIFY(list);
        QMetaObject::invokeMethod(list, "forceActiveFocus");
        QTRY_VERIFY(list->property("activeFocus").toBool());
        QTest::keyClick(m_window, Qt::Key_Backspace);
        QTRY_COMPARE(m_model->folder(),
                     QUrl::fromLocalFile(QDir::cleanPath(dir.path())));
        QTRY_COMPARE(list->property("currentIndex").toInt(),
                     m_model->indexOfName(QStringLiteral("sub")));

        // Typing a name in the list jumps to it; Enter opens it.
        QTest::keyClick(m_window, Qt::Key_A);
        QTRY_COMPARE(list->property("currentIndex").toInt(),
                     m_model->indexOfName(QStringLiteral("a.txt")));
        QVERIFY(accept->property("enabled").toBool());
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(lastAnswer().value(1).toBool(), true);
        QCOMPARE(answerUrls(),
                 QList<QUrl>{QUrl::fromLocalFile(QDir::cleanPath(dir.path())
                                                 + QStringLiteral("/a.txt"))});
        m_answers.clear();
    }

    void openManyReturnsEverySelectedFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        touch(dir.path() + QStringLiteral("/1.txt"));
        touch(dir.path() + QStringLiteral("/2.txt"));
        touch(dir.path() + QStringLiteral("/3.txt"));
        request({{QStringLiteral("mode"), QStringLiteral("openMany")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QTRY_COMPARE(m_model->rowCount(), 3);
        settle();
        QVERIFY(call("clickRow", 0, int(Qt::NoModifier)));
        QVERIFY(call("clickRow", 2, int(Qt::ControlModifier)));
        QVERIFY(call("acceptCurrent"));
        QTRY_COMPARE(m_answers.size(), 1);
        const QString base = QDir::cleanPath(dir.path());
        QCOMPARE(answerUrls(),
                 (QList<QUrl>{QUrl::fromLocalFile(base + QStringLiteral("/1.txt")),
                              QUrl::fromLocalFile(base + QStringLiteral("/3.txt"))}));
        m_answers.clear();

        // Shift extends from the anchor.
        request({{QStringLiteral("mode"), QStringLiteral("openMany")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QTRY_COMPARE(m_model->rowCount(), 3);
        settle();
        QVERIFY(call("clickRow", 0, int(Qt::NoModifier)));
        QVERIFY(call("clickRow", 2, int(Qt::ShiftModifier)));
        QVERIFY(call("acceptCurrent"));
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(answerUrls().size(), 3);
        m_answers.clear();
    }

    // The report: Ctrl+click and Shift+click chose nothing, and Open stayed
    // on and did nothing. Real clicks, with the modifiers on the events.
    void ctrlAndShiftClicksChooseLikeADesktopDialog()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const char *n : {"1.txt", "2.txt", "3.txt", "4.txt", "5.txt"})
            touch(dir.path() + QLatin1Char('/') + QLatin1String(n));
        request({{QStringLiteral("mode"), QStringLiteral("openMany")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QTRY_COMPARE(m_model->rowCount(), 5);
        settle();

        clickRowItem(0);
        QCOMPARE(highlighted(), QStringList{QStringLiteral("1.txt")});
        // Ctrl adds a row and keeps the others.
        clickRowItem(2, Qt::ControlModifier);
        QCOMPARE(highlighted(),
                 (QStringList{QStringLiteral("1.txt"), QStringLiteral("3.txt")}));
        // Shift picks the run from the last row clicked, replacing the rest.
        clickRowItem(4, Qt::ShiftModifier);
        QCOMPARE(highlighted(),
                 (QStringList{QStringLiteral("3.txt"), QStringLiteral("4.txt"),
                              QStringLiteral("5.txt")}));
        // Ctrl+Shift adds the run from the same anchor.
        clickRowItem(0, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(highlighted().size(), 5);
        // Ctrl removes one.
        clickRowItem(3, Qt::ControlModifier);
        QCOMPARE(highlighted(),
                 (QStringList{QStringLiteral("1.txt"), QStringLiteral("2.txt"),
                              QStringLiteral("3.txt"), QStringLiteral("5.txt")}));
        QVERIFY(acceptEnabled());
        clickAccept();
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(lastAnswer().value(1).toBool(), true);
        QCOMPARE(answerUrls(),
                 urlsIn(dir.path(), {QStringLiteral("1.txt"), QStringLiteral("2.txt"),
                                     QStringLiteral("3.txt"), QStringLiteral("5.txt")}));
        m_answers.clear();
    }

    // Open was on whenever the cursor was on a row, chosen or not, and then
    // did nothing. It is on only when pressing it does something.
    void openIsOffWhenNothingWouldBeChosen()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("sub")));
        touch(dir.path() + QStringLiteral("/1.txt"));
        touch(dir.path() + QStringLiteral("/2.txt"));
        request({{QStringLiteral("mode"), QStringLiteral("openMany")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QTRY_COMPARE(m_model->rowCount(), 3);
        settle();
        const int one = m_model->indexOfName(QStringLiteral("1.txt"));
        const int sub = m_model->indexOfName(QStringLiteral("sub"));
        QVERIFY(one >= 0 && sub >= 0);

        clickRowItem(one);
        QVERIFY(acceptEnabled());
        // Space unselects the row the cursor stays on: nothing is chosen.
        QTest::keyClick(m_window, Qt::Key_Space);
        QCOMPARE(highlighted(), QStringList());
        QCOMPARE(list()->property("currentIndex").toInt(), one);
        QVERIFY(!acceptEnabled());
        clickAccept();
        QTest::qWait(50);
        QCOMPARE(m_answers.size(), 0);
        QVERIFY(m_dialog->property("visible").toBool());

        // A folder under the cursor: the button enters it.
        clickRowItem(sub);
        QVERIFY(acceptEnabled());
        clickAccept();
        QTRY_COMPARE(m_model->folder(),
                     QUrl::fromLocalFile(QDir::cleanPath(dir.path())
                                         + QStringLiteral("/sub")));
        QCOMPARE(m_answers.size(), 0);
    }

    // The keyboard half: plain arrows move the choice, Shift extends it,
    // Ctrl moves the cursor alone, Space toggles, Ctrl+A takes everything,
    // and Enter on a chosen row opens everything chosen.
    void theKeyboardChoosesLikeADesktopDialog()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const char *n : {"1.txt", "2.txt", "3.txt", "4.txt"})
            touch(dir.path() + QLatin1Char('/') + QLatin1String(n));
        request({{QStringLiteral("mode"), QStringLiteral("openMany")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QTRY_COMPARE(m_model->rowCount(), 4);
        settle();
        const auto names = [](std::initializer_list<const char *> l) {
            QStringList out;
            for (const char *n : l)
                out << QLatin1String(n);
            return out;
        };

        clickRowItem(0);
        QTest::keyClick(m_window, Qt::Key_Down);
        QCOMPARE(highlighted(), names({"2.txt"}));
        QTest::keyClick(m_window, Qt::Key_Down, Qt::ShiftModifier);
        QTest::keyClick(m_window, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(highlighted(), names({"2.txt", "3.txt", "4.txt"}));
        QTest::keyClick(m_window, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(highlighted(), names({"2.txt", "3.txt"}));
        QCOMPARE(list()->property("currentIndex").toInt(), 2);
        QTest::keyClick(m_window, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(list()->property("currentIndex").toInt(), 3);
        QCOMPARE(highlighted(), names({"2.txt", "3.txt"}));
        QTest::keyClick(m_window, Qt::Key_Space);
        QCOMPARE(highlighted(), names({"2.txt", "3.txt", "4.txt"}));
        QTest::keyClick(m_window, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(highlighted(), names({"1.txt", "2.txt", "3.txt", "4.txt"}));
        QTest::keyClick(m_window, Qt::Key_Up, Qt::ControlModifier);
        QTest::keyClick(m_window, Qt::Key_Up, Qt::ControlModifier);
        QTest::keyClick(m_window, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(list()->property("currentIndex").toInt(), 0);
        QTest::keyClick(m_window, Qt::Key_Space);
        QCOMPARE(highlighted(), names({"2.txt", "3.txt", "4.txt"}));
        QTest::keyClick(m_window, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(highlighted(), names({"2.txt", "3.txt", "4.txt"}));
        // Enter on a chosen row: all of them, not that one alone.
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(lastAnswer().value(1).toBool(), true);
        QCOMPARE(answerUrls(), urlsIn(dir.path(), names({"2.txt", "3.txt", "4.txt"})));
        m_answers.clear();
    }

    // A re-listing (a sort here; Ctrl+H, a filter) moves the cursor's row
    // and must not throw the choice away, and a Shift range afterwards runs
    // from the same FILE, not from whatever now sits at its old row.
    void aChoiceAndItsAnchorSurviveARelisting()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const char *n : {"1.txt", "2.txt", "3.txt", "4.txt"})
            touch(dir.path() + QLatin1Char('/') + QLatin1String(n));
        const auto restoreSort = qScopeGuard([this] {
            m_model->setSortAscending(true);
        });
        request({{QStringLiteral("mode"), QStringLiteral("openMany")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QTRY_COMPARE(m_model->rowCount(), 4);
        settle();

        clickRowItem(0);                          // 1.txt
        clickRowItem(2, Qt::ControlModifier);     // 3.txt
        clickRowItem(1, Qt::ControlModifier);     // 2.txt, the anchor now
        QTest::keyClick(m_window, Qt::Key_Space); // ...and unselected again
        QCOMPARE(highlighted(),
                 (QStringList{QStringLiteral("1.txt"), QStringLiteral("3.txt")}));

        m_model->setSortAscending(false);         // 4, 3, 2, 1
        QTRY_COMPARE(m_model->indexOfName(QStringLiteral("4.txt")), 0);
        QTRY_VERIFY(!m_dialog->property("resetting").toBool());
        QTRY_COMPARE(list()->property("currentIndex").toInt(),
                     m_model->indexOfName(QStringLiteral("2.txt")));
        QCOMPARE(highlighted(),
                 (QStringList{QStringLiteral("3.txt"), QStringLiteral("1.txt")}));

        clickRowItem(0, Qt::ShiftModifier);       // 4.txt back to 2.txt
        QCOMPARE(highlighted(),
                 (QStringList{QStringLiteral("4.txt"), QStringLiteral("3.txt"),
                              QStringLiteral("2.txt")}));
        clickAccept();
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(answerUrls(),
                 urlsIn(dir.path(), {QStringLiteral("4.txt"), QStringLiteral("3.txt"),
                                     QStringLiteral("2.txt")}));
        m_answers.clear();
    }

    void folderModeListsFoldersAndReturnsTheOneChosen()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("keys")));
        touch(dir.path() + QStringLiteral("/file.txt"));
        request({{QStringLiteral("mode"), QStringLiteral("folder")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QTRY_COMPARE(m_model->rowCount(), 1);
        QVERIFY(!child("fileChooserName")->property("visible").toBool());
        QVERIFY(child("fileChooserNewFolder")->property("visible").toBool());
        // No row chosen: the folder being shown.
        QVERIFY(call("acceptCurrent"));
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(answerUrls(),
                 QList<QUrl>{QUrl::fromLocalFile(QDir::cleanPath(dir.path()))});
        m_answers.clear();

        request({{QStringLiteral("mode"), QStringLiteral("folder")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QTRY_COMPARE(m_model->rowCount(), 1);
        QVERIFY(call("clickRow", 0, int(Qt::NoModifier)));
        QVERIFY(call("acceptCurrent"));
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(answerUrls(),
                 QList<QUrl>{QUrl::fromLocalFile(QDir::cleanPath(dir.path())
                                                 + QStringLiteral("/keys"))});
        m_answers.clear();
    }

    void aNewFolderIsCreatedAndEntered()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        request({{QStringLiteral("mode"), QStringLiteral("save")},
                 {QStringLiteral("currentName"), QStringLiteral("x.txt")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        m_dialog->setProperty("creatingFolder", true);
        QObject *field = child("fileChooserNewFolderName");
        QVERIFY(field);
        field->setProperty("text", QStringLiteral("Taxes"));
        QVERIFY(call("createFolder"));
        const QString made = QDir::cleanPath(dir.path()) + QStringLiteral("/Taxes");
        QVERIFY(QFileInfo(made).isDir());
        QTRY_COMPARE(m_model->folder(), QUrl::fromLocalFile(made));
        QVERIFY(!m_dialog->property("creatingFolder").toBool());
    }

    // B001/B003: the column header is 30px at the top of the list panel and
    // the list takes the rest. A nested layout fills both ways by default, and
    // the header once took the list's height. Geometric on purpose.
    void theListFillsThePanelBelowAThirtyPixelHeader()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        touch(dir.path() + QStringLiteral("/a.txt"));
        touch(dir.path() + QStringLiteral("/b.txt"));
        request({{QStringLiteral("mode"), QStringLiteral("open")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QQuickItem *list = qobject_cast<QQuickItem *>(child("fileChooserList"));
        QVERIFY(list);
        QQuickItem *panel = list->parentItem();
        QVERIFY(panel);
        // The sort headers are Repeater delegates: in the item tree, not
        // QObject children, so findChild() cannot see them.
        std::function<QQuickItem *(QQuickItem *)> findItem =
            [&findItem](QQuickItem *from) -> QQuickItem * {
            for (QQuickItem *item : from->childItems()) {
                if (item->objectName() == QLatin1String("fileChooserSort_name"))
                    return item;
                if (QQuickItem *hit = findItem(item))
                    return hit;
            }
            return nullptr;
        };
        QQuickItem *header = findItem(panel);
        QVERIFY(header);
        QTRY_VERIFY(list->height() > 0);
        QCOMPARE(header->height(), 30.0);
        QVERIFY2(list->height() >= panel->height() - 31 - 2,
                 qPrintable(QStringLiteral("list %1 of panel %2")
                                .arg(list->height()).arg(panel->height())));
        QVERIFY(list->height() > 100);
        QVERIFY(header->mapToItem(panel, QPointF(0, 0)).y() < 5);
    }

    // B134: the new-folder error goes when the row closes.
    void theNewFolderErrorGoesWhenTheRowCloses()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        request({{QStringLiteral("mode"), QStringLiteral("save")},
                 {QStringLiteral("currentName"), QStringLiteral("x.txt")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        m_dialog->setProperty("creatingFolder", true);
        child("fileChooserNewFolderName")
            ->setProperty("text", QStringLiteral("../up"));
        QVERIFY(call("createFolder"));
        QVERIFY(!m_dialog->property("failure").toString().isEmpty());
        m_dialog->setProperty("creatingFolder", false);
        QVERIFY(m_dialog->property("failure").toString().isEmpty());
    }

    void escapeAndCancelAnswerCancelled()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const int id = request(
            {{QStringLiteral("mode"), QStringLiteral("save")},
             {QStringLiteral("currentName"), QStringLiteral("x.txt")},
             {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QTRY_VERIFY(child("fileChooserName")->property("activeFocus").toBool());
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(lastAnswer().value(0).toInt(), id);
        QCOMPARE(lastAnswer().value(1).toBool(), false);
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        m_answers.clear();

        request({{QStringLiteral("mode"), QStringLiteral("open")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QObject *cancel = child("fileChooserCancel");
        QVERIFY(cancel);
        QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        QTRY_COMPARE(m_answers.size(), 1);
        QCOMPARE(lastAnswer().value(1).toBool(), false);
        m_answers.clear();
    }

    void aSecondRequestWaitsForTheFirst()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const int first = request(
            {{QStringLiteral("mode"), QStringLiteral("open")},
             {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        const int second = m_chooser->open(
            {{QStringLiteral("mode"), QStringLiteral("save")},
             {QStringLiteral("currentName"), QStringLiteral("later.txt")},
             {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        QTest::qWait(50);
        QCOMPARE(m_dialog->property("requestId").toInt(), first);
        QVERIFY(call("cancel"));
        QTRY_COMPARE(m_dialog->property("requestId").toInt(), second);
        QTRY_VERIFY(m_dialog->property("opened").toBool());
        QCOMPARE(child("fileChooserName")->property("text").toString(),
                 QStringLiteral("later.txt"));
        QVERIFY(call("cancel"));
        QTRY_COMPARE(m_answers.size(), 2);
        m_answers.clear();
    }

    // "It doesn't match the theme": every surface is a theme token, so the
    // path bar is not white on a dark theme and the ink stays readable on
    // every palette, light and dark.
    void everySurfaceFollowsTheTheme()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        request({{QStringLiteral("mode"), QStringLiteral("save")},
                 {QStringLiteral("currentName"), QStringLiteral("x.txt")},
                 {QStringLiteral("folder"), QUrl::fromLocalFile(dir.path())}});
        auto *background =
            m_dialog->property("background").value<QQuickItem *>();
        QVERIFY(background);
        QObject *title = child("fileChooserTitle");
        QObject *pathBar = child("fileChooserPathBar");
        QObject *name = child("fileChooserName");
        QVERIFY(title && pathBar && name);

        QSet<QRgb> surfaces;
        int light = 0;
        int dark = 0;
        int themes = 0;
        for (int mode = 1; mode <= 11; ++mode) {
            m_root->setProperty("themeMode", mode);
            QCoreApplication::processEvents();
            const QColor surface = background->property("color").value<QColor>();
            const QColor ink = title->property("color").value<QColor>();
            const QColor bar = pathBar->property("color").value<QColor>();
            const QColor fieldInk = name->property("color").value<QColor>();
            const QColor field = name->property("background")
                                     .value<QQuickItem *>()
                                     ->property("color").value<QColor>();
            QVERIFY2(contrast(ink, surface) >= 4.5,
                     qPrintable(QStringLiteral("theme %1: title %2 on %3")
                                    .arg(mode).arg(ink.name(), surface.name())));
            QVERIFY2(contrast(fieldInk, field) >= 4.5,
                     qPrintable(QStringLiteral("theme %1: name %2 on %3")
                                    .arg(mode).arg(fieldInk.name(), field.name())));
            // The path bar is the theme's field, not a fixed white strip:
            // on a dark theme it is dark.
            const bool darkTheme = luminance(surface) < 0.18;
            if (darkTheme)
                QVERIFY2(luminance(bar) < 0.18,
                         qPrintable(QStringLiteral("theme %1: path bar %2")
                                        .arg(mode).arg(bar.name())));
            (darkTheme ? dark : light) += 1;
            surfaces.insert(surface.rgb());
            ++themes;
        }
        QCOMPARE(themes, 11);
        QVERIFY2(light >= 2 && dark >= 2,
                 qPrintable(QStringLiteral("light %1 dark %2").arg(light).arg(dark)));
        // What varied is the palette, not just the loop counter.
        QVERIFY2(surfaces.size() >= 6,
                 qPrintable(QStringLiteral("%1 distinct surfaces")
                                .arg(surfaces.size())));
        m_root->setProperty("themeMode", 9);
    }
};

QTEST_MAIN(FileChooserDialogQmlTest)
#include "FileChooserDialogQmlTest.moc"
