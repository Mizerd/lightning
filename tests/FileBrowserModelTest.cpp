// FileBrowserModel: the folder listing behind Lightning's own file chooser
// (FileChooserDialog.qml), shown on Linux when no portal FileChooser exists.
//
// Every case lists a QTemporaryDir; nothing outside it is read except the
// home folder's path (for "~" and the places column).

#include "models/FileBrowserModel.h"

#include <QDir>
#include <QLocale>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest/QtTest>

#include <atomic>
#include <future>
#include <memory>
#include <vector>

namespace {

void touch(const QString &path, int bytes = 1)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(bytes, 'x'));
}

QStringList names(const FileBrowserModel &model)
{
    QStringList out;
    for (int i = 0; i < model.rowCount(); ++i)
        out << model.get(i).value(QStringLiteral("name")).toString();
    return out;
}

} // namespace

class FileBrowserModelTest : public QObject
{
    Q_OBJECT

private:
    // A folder with two subfolders, a hidden file and three files.
    void populate(const QString &root)
    {
        QVERIFY(QDir(root).mkdir(QStringLiteral("Photos")));
        QVERIFY(QDir(root).mkdir(QStringLiteral("archive")));
        touch(root + QStringLiteral("/.secret"));
        touch(root + QStringLiteral("/report.pdf"), 300);
        touch(root + QStringLiteral("/notes 10.txt"), 20);
        touch(root + QStringLiteral("/notes 9.txt"), 4000);
    }

    bool load(FileBrowserModel &model, const QString &path)
    {
        QSignalSpy listed(&model, &FileBrowserModel::listed);
        model.setFolder(QUrl::fromLocalFile(path));
        return listed.wait(5000) && !model.loading();
    }

private Q_SLOTS:
    void foldersComeFirstInNaturalOrderAndHiddenFilesWaitForTheToggle_data()
    {
        QTest::addColumn<QLocale>("locale");
        QTest::newRow("C.UTF-8") << QLocale::c();
        QTest::newRow("en_US.UTF-8") << QLocale(QLocale::English, QLocale::UnitedStates);
    }

    void foldersComeFirstInNaturalOrderAndHiddenFilesWaitForTheToggle()
    {
        QFETCH(QLocale, locale);
        const QLocale previous;
        const auto restoreLocale = qScopeGuard([previous] { QLocale::setDefault(previous); });
        QLocale::setDefault(locale);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        populate(dir.path());
        FileBrowserModel model;
        QVERIFY(load(model, dir.path()));
        QCOMPARE(model.error(), QString());
        // Folders first, case-insensitive; "notes 9" before "notes 10".
        QCOMPARE(names(model), (QStringList{
                                   QStringLiteral("archive"),
                                   QStringLiteral("Photos"),
                                   QStringLiteral("notes 9.txt"),
                                   QStringLiteral("notes 10.txt"),
                                   QStringLiteral("report.pdf")}));
        QCOMPARE(model.get(0).value(QStringLiteral("kind")).toString(),
                 QStringLiteral("folder"));
        QCOMPARE(model.get(4).value(QStringLiteral("kind")).toString(),
                 QStringLiteral("pdf"));

        // The toggle is a filter over the same read.
        model.setShowHidden(true);
        QCOMPARE(model.rowCount(), 6);
        QVERIFY(names(model).contains(QStringLiteral(".secret")));
    }

    void sortingBySizeAndDateKeepsFoldersFirst()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        populate(dir.path());
        FileBrowserModel model;
        QVERIFY(load(model, dir.path()));
        model.setSortField(QStringLiteral("size"));
        model.setSortAscending(false);
        const QStringList bySize = names(model);
        QCOMPARE(bySize.mid(2), (QStringList{
                                    QStringLiteral("notes 9.txt"),
                                    QStringLiteral("report.pdf"),
                                    QStringLiteral("notes 10.txt")}));
        QVERIFY(bySize.mid(0, 2).contains(QStringLiteral("Photos")));
        // An unknown field is "name", not a silent no-op.
        model.setSortField(QStringLiteral("colour"));
        QCOMPARE(model.sortField(), QStringLiteral("name"));
    }

    void nameFiltersHideFilesButNeverFolders()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        populate(dir.path());
        touch(dir.path() + QStringLiteral("/SCAN.PDF"));
        FileBrowserModel model;
        QVERIFY(load(model, dir.path()));
        model.setNameFilters({QStringLiteral("*.pdf")});
        // Case-insensitive, as the desktop's own dialogs match.
        QCOMPARE(names(model), (QStringList{
                                   QStringLiteral("archive"),
                                   QStringLiteral("Photos"),
                                   QStringLiteral("report.pdf"),
                                   QStringLiteral("SCAN.PDF")}));
        QCOMPARE(model.filteredOut(), 2);
        model.setNameFilters({QStringLiteral("*")});
        QCOMPARE(model.rowCount(), 6);
        model.setFoldersOnly(true);
        QCOMPARE(names(model), (QStringList{QStringLiteral("archive"),
                                            QStringLiteral("Photos")}));
    }

    void typeToSelectWrapsAndCycles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        populate(dir.path());
        FileBrowserModel model;
        QVERIFY(load(model, dir.path()));
        // archive, Photos, notes 9.txt, notes 10.txt, report.pdf
        QCOMPARE(model.findPrefix(QStringLiteral("n"), -1), 2);
        QCOMPARE(model.findPrefix(QStringLiteral("n"), 2), 3);
        QCOMPARE(model.findPrefix(QStringLiteral("n"), 3), 2); // wraps
        QCOMPARE(model.findPrefix(QStringLiteral("PH"), -1), 1);
        QCOMPARE(model.findPrefix(QStringLiteral("zz"), -1), -1);
        QCOMPARE(model.indexOfName(QStringLiteral("report.pdf")), 4);
        QCOMPARE(model.indexOfName(QStringLiteral("Report.pdf")), -1);
    }

    // Windows uses this chooser since 2026-10-16: a folder on another drive
    // starts its path bar at that drive, not at C:/ with the drive's folders
    // spelled under it.
    void thePathBarStartsAtTheFoldersOwnDrive()
    {
        QCOMPARE(FileBrowserModel::pathRoot(QStringLiteral("D:/data/photos")),
                 QStringLiteral("D:/"));
        QCOMPARE(FileBrowserModel::pathRoot(QStringLiteral("e:")),
                 QStringLiteral("e:/"));
        QCOMPARE(FileBrowserModel::pathRoot(
                     QStringLiteral("//nas/media/films/2026")),
                 QStringLiteral("//nas/media/"));
        // Neither: the platform root.
        QCOMPARE(FileBrowserModel::pathRoot(QStringLiteral("/tmp/inner")),
                 QDir::rootPath());
        QCOMPARE(FileBrowserModel::pathRoot(QStringLiteral("1:/x")),
                 QDir::rootPath());
    }

    // Review M4: what the in-app chooser needs on Windows, as pure helpers.
    void windowsPathsNamesAndDrivesForTheChooser()
    {
        // Paths read as Windows writes them, never "/C:/...".
        QCOMPARE(FileBrowserModel::displayPathFor(
                     QStringLiteral("C:/Users/six/Downloads"), true),
                 QStringLiteral("C:\\Users\\six\\Downloads"));
        QCOMPARE(FileBrowserModel::displayPathFor(
                     QStringLiteral("/C:/Users/six"), true),
                 QStringLiteral("C:\\Users\\six"));
        QCOMPARE(FileBrowserModel::displayPathFor(
                     QStringLiteral("//nas/media/films"), true),
                 QStringLiteral("\\\\nas\\media\\films"));
        QCOMPARE(FileBrowserModel::displayPathFor(
                     QStringLiteral("/home/six/a"), false),
                 QStringLiteral("/home/six/a"));

        // Names Windows refuses.
        for (const char *bad : {"a:b", "a<b", "a>b", "a\"b", "a\\b", "a|b",
                                "a?b", "a*b", "trailing.", "trailing ",
                                "CON", "con", "NUL.txt", "nul .tar.gz", "PRN",
                                "AUX", "COM1", "lpt9.log", ""}) {
            QVERIFY2(!FileBrowserModel::isValidWindowsLeaf(
                         QString::fromUtf8(bad)),
                     bad);
            QVERIFY2(!FileBrowserModel::isValidLeaf(QString::fromUtf8(bad),
                                                    true),
                     bad);
        }
        QVERIFY(!FileBrowserModel::isValidWindowsLeaf(
            QStringLiteral("COM") + QChar(0x00B9)));
        QVERIFY(!FileBrowserModel::isValidWindowsLeaf(
            QStringLiteral("tab\there")));
        // And names it accepts, including near misses of the device names.
        for (const char *good : {"report.pdf", "CONSOLE.txt", "COM10", "LPT0",
                                 "nullable", ".hidden", "a b.c", "Ąžuolas"}) {
            QVERIFY2(FileBrowserModel::isValidWindowsLeaf(
                         QString::fromUtf8(good)),
                     good);
        }
        // Elsewhere only a separator, NUL, "." and ".." are refused.
        QVERIFY(FileBrowserModel::isValidLeaf(QStringLiteral("a:b"), false));
        QVERIFY(FileBrowserModel::isValidLeaf(QStringLiteral("CON"), false));
        QVERIFY(!FileBrowserModel::isValidLeaf(QStringLiteral("a/b"), false));
        QVERIFY(!FileBrowserModel::isValidLeaf(QStringLiteral(".."), false));

        // Drive roots become labelled places.
        const QVariantList drives = FileBrowserModel::driveEntries(
            {QStringLiteral("C:/"), QStringLiteral("D:/")});
        QCOMPARE(drives.size(), 2);
        QCOMPARE(drives.at(1).toMap().value(QStringLiteral("label")).toString(),
                 QStringLiteral("D:"));
        QCOMPARE(drives.at(1).toMap().value(QStringLiteral("url")).toUrl(),
                 QUrl::fromLocalFile(QStringLiteral("D:/")));
#ifndef Q_OS_WIN
        FileBrowserModel model;
        QVERIFY(model.drives().isEmpty());
#endif
    }

    void thePathBarStartsAtHomeUnderHomeAndAtTheRootElsewhere()
    {
        FileBrowserModel model;
        const QString home = QDir::cleanPath(QDir::homePath());
        model.setFolder(QUrl::fromLocalFile(home));
        QVariantList crumbs = model.breadcrumbs();
        QCOMPARE(crumbs.size(), 1);
        QCOMPARE(crumbs.at(0).toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("home"));

        QTemporaryDir dir; // under /tmp, not under home on any sane host
        QVERIFY(dir.isValid());
        const QString path = QDir::cleanPath(dir.path());
        if (path.startsWith(home + QLatin1Char('/')))
            QSKIP("the temporary folder is under home on this host");
        QVERIFY(QDir(path).mkdir(QStringLiteral("inner")));
        model.setFolder(QUrl::fromLocalFile(path + QStringLiteral("/inner")));
        crumbs = model.breadcrumbs();
        QCOMPARE(crumbs.first().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("root"));
        QCOMPARE(crumbs.last().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("inner"));
        QCOMPARE(crumbs.last().toMap().value(QStringLiteral("url")).toUrl(),
                 QUrl::fromLocalFile(path + QStringLiteral("/inner")));
        QCOMPARE(model.parentFolder(), QUrl::fromLocalFile(path));
        // Every crumb is the folder above the next.
        for (int i = 1; i < crumbs.size(); ++i) {
            const QString child = crumbs.at(i).toMap().value(
                QStringLiteral("url")).toUrl().toLocalFile();
            const QString parent = crumbs.at(i - 1).toMap().value(
                QStringLiteral("url")).toUrl().toLocalFile();
            QCOMPARE(QFileInfo(child).absolutePath(), QDir::cleanPath(parent));
        }
        model.setFolder(QUrl::fromLocalFile(QStringLiteral("/")));
        QCOMPARE(model.parentFolder(), QUrl());
    }

    void typedPathsResolveAgainstTheFolderAndHome()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FileBrowserModel model;
        QVERIFY(load(model, dir.path()));
        const QString base = QDir::cleanPath(dir.path());
        QCOMPARE(model.resolve(QStringLiteral("a.pdf")),
                 QUrl::fromLocalFile(base + QStringLiteral("/a.pdf")));
        QCOMPARE(model.resolve(QStringLiteral("  sub/../b.txt ")),
                 QUrl::fromLocalFile(base + QStringLiteral("/b.txt")));
        QCOMPARE(model.resolve(QStringLiteral("~")),
                 QUrl::fromLocalFile(QDir::cleanPath(QDir::homePath())));
        QCOMPARE(model.resolve(QStringLiteral("~/x")),
                 QUrl::fromLocalFile(QDir::cleanPath(QDir::homePath())
                                     + QStringLiteral("/x")));
        QCOMPARE(model.resolve(QStringLiteral("/etc/hosts")),
                 QUrl::fromLocalFile(QStringLiteral("/etc/hosts")));
        QCOMPARE(model.resolve(QString()), QUrl());

        touch(base + QStringLiteral("/there.txt"));
        QVariantMap info = model.stat(model.resolve(QStringLiteral("there.txt")));
        QVERIFY(info.value(QStringLiteral("exists")).toBool());
        QVERIFY(!info.value(QStringLiteral("isDir")).toBool());
        QVERIFY(info.value(QStringLiteral("parentExists")).toBool());
        info = model.stat(model.resolve(QStringLiteral("nowhere/x.txt")));
        QVERIFY(!info.value(QStringLiteral("exists")).toBool());
        QVERIFY(!info.value(QStringLiteral("parentExists")).toBool());
    }

    void aNewFolderIsCreatedOnceAndListed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FileBrowserModel model;
        QVERIFY(load(model, dir.path()));
        QSignalSpy listed(&model, &FileBrowserModel::listed);
        QVariantMap result = model.createFolder(QStringLiteral("  New one "));
        QVERIFY(result.value(QStringLiteral("ok")).toBool());
        QCOMPARE(result.value(QStringLiteral("url")).toUrl(),
                 QUrl::fromLocalFile(QDir::cleanPath(dir.path())
                                     + QStringLiteral("/New one")));
        QVERIFY(listed.wait(5000));
        QCOMPARE(model.indexOfName(QStringLiteral("New one")), 0);

        result = model.createFolder(QStringLiteral("New one"));
        QCOMPARE(result.value(QStringLiteral("error")).toString(),
                 QStringLiteral("exists"));
        for (const QString &bad : {QStringLiteral(""), QStringLiteral(".."),
                                   QStringLiteral("a/b")}) {
            result = model.createFolder(bad);
            QCOMPARE(result.value(QStringLiteral("error")).toString(),
                     QStringLiteral("invalid"));
        }
    }

    void aMissingFolderSaysSoAndAStaleListingIsDropped()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        populate(dir.path());
        FileBrowserModel model;
        QVERIFY(load(model, dir.path() + QStringLiteral("/gone")));
        QCOMPARE(model.error(), QStringLiteral("notFound"));
        QCOMPARE(model.rowCount(), 0);

        // Two folders in a row: only the second one's rows may land.
        QTemporaryDir other;
        QVERIFY(other.isValid());
        touch(other.path() + QStringLiteral("/only.txt"));
        QSignalSpy listed(&model, &FileBrowserModel::listed);
        model.setFolder(QUrl::fromLocalFile(dir.path()));
        model.setFolder(QUrl::fromLocalFile(other.path()));
        // The first folder's rows are gone at once, before any answer.
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(listed.wait(5000));
        QTest::qWait(50);
        QCOMPARE(listed.count(), 1);
        QCOMPARE(names(model), QStringList{QStringLiteral("only.txt")});
    }

    void placesAreRealFoldersAndNeverHomeTwice()
    {
        FileBrowserModel model;
        const QVariantList places = model.places();
        QVERIFY(!places.isEmpty());
        QCOMPARE(places.first().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("home"));
        QCOMPARE(places.last().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("root"));
        QStringList seen;
        for (const QVariant &v : places) {
            const QString path = v.toMap().value(QStringLiteral("url"))
                                     .toUrl().toLocalFile();
            QVERIFY2(QFileInfo(path).isDir(), qPrintable(path));
            QVERIFY2(!seen.contains(path), qPrintable(path));
            seen << path;
        }
    }

    // §18 review, 2026-10-15: the listing ran on QThreadPool::globalInstance(),
    // so a read stuck on a hung network mount held a global-pool thread for
    // good; ~QCoreApplication waits for that pool at quit, and the call media
    // engine shares it. A lister that blocks until released stands in for the
    // hung mount. On the pool code, globalInstance() reports an active thread
    // and waitForDone() times out, which is exactly the hang at quit.
    void aStalledListingBlocksNeitherQuitNorOtherFolders()
    {
        const QString stalled = QStringLiteral("/stalled-mount/folder");
        std::promise<void> release;
        const std::shared_future<void> released = release.get_future().share();
        FileBrowserModel::setListerForTesting(
            [stalled, released](const QString &path) {
                FileBrowserModel::Listing listing;
                if (path == stalled) {
                    released.wait();
                    FileBrowserModel::Entry late;
                    late.name = QStringLiteral("late.txt");
                    late.kind = QStringLiteral("text");
                    listing.entries.append(late);
                    return listing;
                }
                FileBrowserModel::Entry only;
                only.name = QStringLiteral("only.txt");
                only.kind = QStringLiteral("text");
                listing.entries.append(only);
                return listing;
            });
        const auto restore = qScopeGuard([] {
            FileBrowserModel::setListerForTesting({});
        });
        const int before = FileBrowserModel::runningListings();

        auto *stuck = new FileBrowserModel;
        stuck->setFolder(QUrl::fromLocalFile(stalled));
        QVERIFY(stuck->loading());
        QCOMPARE(FileBrowserModel::runningListings(), before + 1);

        // What ~QCoreApplication waits for is not holding it.
        QCOMPARE(QThreadPool::globalInstance()->activeThreadCount(), 0);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(100));

        // Another folder lists at once, in another model and in this one.
        FileBrowserModel other;
        QVERIFY(load(other, QStringLiteral("/elsewhere")));
        QCOMPARE(names(other), QStringList{QStringLiteral("only.txt")});
        FileBrowserModel moved;
        moved.setFolder(QUrl::fromLocalFile(stalled));
        QVERIFY(load(moved, QStringLiteral("/elsewhere")));

        // Destroying the model whose listing is stuck does not wait for it.
        QElapsedTimer clock;
        clock.start();
        delete stuck;
        QVERIFY2(clock.elapsed() < 500, qPrintable(QString::number(clock.elapsed())));

        // The mount answers at last: nothing lands anywhere, nothing breaks.
        release.set_value();
        QTRY_COMPARE(FileBrowserModel::runningListings(), before);
        QTest::qWait(50);
        QCOMPARE(names(moved), QStringList{QStringLiteral("only.txt")});
        QVERIFY(!moved.loading());
    }

    // §18 recheck: leaked listing threads had no cap. Reopening a folder on a
    // hung mount (a refresh, a second chooser) joins the read already stuck
    // there, and past kMaxListingThreads a new folder answers "unavailable"
    // without starting another thread.
    void aPathStillBeingReadIsNotReadTwiceAndThreadsAreCapped()
    {
        std::promise<void> release;
        const std::shared_future<void> released = release.get_future().share();
        auto calls = std::make_shared<std::atomic<int>>(0);
        FileBrowserModel::setListerForTesting(
            [released, calls](const QString &path) {
                ++*calls;
                FileBrowserModel::Listing listing;
                if (path.startsWith(QStringLiteral("/stalled/")))
                    released.wait();
                FileBrowserModel::Entry e;
                e.name = QStringLiteral("late.txt");
                e.kind = QStringLiteral("text");
                listing.entries.append(e);
                return listing;
            });
        const auto restore = qScopeGuard([] {
            FileBrowserModel::setListerForTesting({});
        });
        // Even if this case fails half-way, the stuck reads are let go.
        const auto letGo = qScopeGuard([&] {
            try { release.set_value(); } catch (const std::future_error &) {}
        });
        // Earlier cases' reads have all finished.
        QTRY_COMPARE(FileBrowserModel::runningListings(), 0);

        // One folder, asked for three times: one thread, one read so far.
        const QUrl same = QUrl::fromLocalFile(QStringLiteral("/stalled/same"));
        FileBrowserModel first;
        first.setFolder(same);
        FileBrowserModel second;
        second.setFolder(same);
        first.refresh();
        QCOMPARE(FileBrowserModel::runningListings(), 1);
        // The read happens on its thread: wait for it, then make sure no
        // second one follows.
        QTRY_COMPARE(calls->load(), 1);
        QTest::qWait(50);
        QCOMPARE(calls->load(), 1);
        QVERIFY(first.loading());
        QVERIFY(second.loading());

        // Fill the ceiling with distinct stuck folders.
        std::vector<std::unique_ptr<FileBrowserModel>> stuck;
        for (int i = 1; i < FileBrowserModel::kMaxListingThreads; ++i) {
            stuck.push_back(std::make_unique<FileBrowserModel>());
            stuck.back()->setFolder(QUrl::fromLocalFile(
                QStringLiteral("/stalled/%1").arg(i)));
        }
        QCOMPARE(FileBrowserModel::runningListings(),
                 FileBrowserModel::kMaxListingThreads);
        QTRY_COMPARE(calls->load(), FileBrowserModel::kMaxListingThreads);

        // One more: refused, not a seventeenth thread.
        FileBrowserModel refused;
        QSignalSpy refusedListed(&refused, &FileBrowserModel::listed);
        refused.setFolder(QUrl::fromLocalFile(QStringLiteral("/stalled/over")));
        QCOMPARE(refusedListed.count(), 1);
        QVERIFY(!refused.loading());
        QCOMPARE(refused.error(), QStringLiteral("unavailable"));
        QCOMPARE(FileBrowserModel::runningListings(),
                 FileBrowserModel::kMaxListingThreads);
        QTest::qWait(50);
        QCOMPARE(calls->load(), FileBrowserModel::kMaxListingThreads);

        // The mount answers. The two requests made while it ran get the same
        // thread's second read of that path, not the first one's answer.
        QSignalSpy firstListed(&first, &FileBrowserModel::listed);
        QSignalSpy secondListed(&second, &FileBrowserModel::listed);
        release.set_value();
        QTRY_COMPARE(FileBrowserModel::runningListings(), 0);
        QCOMPARE(calls->load(), FileBrowserModel::kMaxListingThreads + 1);
        QTRY_VERIFY(!first.loading());
        QTRY_VERIFY(!second.loading());
        QCOMPARE(names(first), QStringList{QStringLiteral("late.txt")});
        QCOMPARE(names(second), QStringList{QStringLiteral("late.txt")});
        // first's two generations (open, refresh): only the newest applies.
        QCOMPARE(firstListed.count(), 1);
        QCOMPARE(secondListed.count(), 1);

        // And with the threads back, a folder lists again.
        QVERIFY(load(refused, QStringLiteral("/elsewhere")));
        QCOMPARE(refused.error(), QString());
    }

    // §18 recheck: a refresh that joins a read already under way must not be
    // handed that read's answer, which can predate the change it is
    // refreshing for. createFolder() refreshes; if the listing it joins
    // started before the mkdir, the new folder must still appear.
    void aRefreshDuringAReadSeesWhatChangedSinceItBegan()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir::cleanPath(dir.path());
        std::promise<void> release;
        const std::shared_future<void> released = release.get_future().share();
        auto calls = std::make_shared<std::atomic<int>>(0);
        // Like a real read: it takes its snapshot of the folder first, and
        // only the first read then stalls before answering.
        FileBrowserModel::setListerForTesting(
            [released, calls](const QString &folder) {
                FileBrowserModel::Listing listing;
                const QStringList entries = QDir(folder).entryList(
                    QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
                for (const QString &name : entries) {
                    FileBrowserModel::Entry e;
                    e.name = name;
                    e.isDir = QFileInfo(QDir(folder).filePath(name)).isDir();
                    e.kind = e.isDir ? QStringLiteral("folder")
                                     : QStringLiteral("file");
                    listing.entries.append(e);
                }
                if (calls->fetch_add(1) == 0)
                    released.wait();
                return listing;
            });
        const auto restore = qScopeGuard([] {
            FileBrowserModel::setListerForTesting({});
        });
        const auto letGo = qScopeGuard([&] {
            try { release.set_value(); } catch (const std::future_error &) {}
        });
        QTRY_COMPARE(FileBrowserModel::runningListings(), 0);

        FileBrowserModel model;
        model.setFolder(QUrl::fromLocalFile(path));
        QTRY_COMPARE(calls->load(), 1); // the snapshot is taken, then it stalls
        const QVariantMap made = model.createFolder(QStringLiteral("Taxes"));
        QVERIFY(made.value(QStringLiteral("ok")).toBool());
        QCOMPARE(FileBrowserModel::runningListings(), 1); // no second thread

        QSignalSpy listed(&model, &FileBrowserModel::listed);
        release.set_value();
        QVERIFY(listed.wait(5000));
        QCOMPARE(calls->load(), 2);
        QCOMPARE(names(model), QStringList{QStringLiteral("Taxes")});
        QTRY_COMPARE(FileBrowserModel::runningListings(), 0);
    }

    // The same hung mount, reached by a path typed into the name or path
    // field: stat() gives up after kStatTimeoutMs instead of freezing.
    void aHungStatAnswersTimedOutInsteadOfFreezing()
    {
        std::promise<void> release;
        const std::shared_future<void> released = release.get_future().share();
        FileBrowserModel::setStaterForTesting([released](const QString &) {
            released.wait();
            return QVariantMap{{QStringLiteral("exists"), true}};
        });
        const auto restore = qScopeGuard([&] {
            FileBrowserModel::setStaterForTesting({});
        });
        FileBrowserModel model;
        QElapsedTimer clock;
        clock.start();
        const QVariantMap info = model.stat(
            QUrl::fromLocalFile(QStringLiteral("/stalled-mount/x.pdf")));
        const qint64 waited = clock.elapsed();
        QVERIFY(info.value(QStringLiteral("timedOut")).toBool());
        QVERIFY(!info.value(QStringLiteral("exists")).toBool());
        QVERIFY2(waited >= FileBrowserModel::kStatTimeoutMs - 50
                     && waited < FileBrowserModel::kStatTimeoutMs + 2000,
                 qPrintable(QString::number(waited)));
        release.set_value();

        // A stat that answers in time says so.
        FileBrowserModel::setStaterForTesting({});
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QVariantMap real = model.stat(QUrl::fromLocalFile(dir.path()));
        QVERIFY(!real.value(QStringLiteral("timedOut")).toBool());
        QVERIFY(real.value(QStringLiteral("isDir")).toBool());
    }

    void aModelDestroyedMidListingIsSafe()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        populate(dir.path());
        for (int i = 0; i < 20; ++i) {
            auto *model = new FileBrowserModel;
            model->setFolder(QUrl::fromLocalFile(dir.path()));
            delete model;
        }
        // Any listing posted to a deleted model would run here.
        QTest::qWait(100);
    }
};

QTEST_GUILESS_MAIN(FileBrowserModelTest)
#include "FileBrowserModelTest.moc"
