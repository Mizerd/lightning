#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtTest>

class ProfilePopoverContractTest : public QObject
{
    Q_OBJECT
private:
    static QString read(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    }

    static QString qmlPath(const QString &file)
    {
        return QStringLiteral(QML_DIR "/") + file;
    }

private Q_SLOTS:
    void exactlyOneInstancePerView()
    {
        const QStringList views = {
            QStringLiteral("TimelinePane.qml"),
            QStringLiteral("ThreadPanel.qml"),
            QStringLiteral("RoomInfoPanel.qml"),
        };
        for (const QString &view : views) {
            const QString src = read(qmlPath(view));
            QVERIFY2(!src.isEmpty(), qUtf8Printable("could not read " + view));
            QCOMPARE(src.count(QStringLiteral("MemberProfilePopover {")), 1);
        }
    }

    void zeroInstancesInAnyDelegate()
    {
        QDir dir(QStringLiteral(QML_DIR));
        const QStringList delegateFiles =
            dir.entryList({ QStringLiteral("*Delegate*.qml") }, QDir::Files);
        QVERIFY2(delegateFiles.size() >= 3,
                 "expected at least the known MessageDelegate/RoomDelegate/"
                 "RoomActivityDelegate.qml files to be found");
        for (const QString &file : delegateFiles) {
            const QString src = read(qmlPath(file));
            QVERIFY2(!src.isEmpty(), qUtf8Printable("could not read " + file));
            QVERIFY2(!src.contains(QStringLiteral("MemberProfilePopover {")),
                     qUtf8Printable(file + " must not instantiate its own "
                                    "MemberProfilePopover — route through "
                                    "the view's openSenderProfile instead"));
        }
    }

    void messageDelegateRoutesThroughTheViewsOpenSenderProfile()
    {
        const QString delegate = read(qmlPath(QStringLiteral("MessageDelegate.qml")));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("root.timelineView.openSenderProfile")));
        QVERIFY(!delegate.contains(QStringLiteral("MemberProfilePopover {")));
    }

    void everyOpenerPassesAnAnchorAndNoInstanceIsCentred()
    {
        for (const char *view : {"TimelinePane.qml", "ThreadPanel.qml",
                                 "RoomInfoPanel.qml"}) {
            const QString src = read(qmlPath(QString::fromLatin1(view)));
            const int at = src.indexOf(QStringLiteral("MemberProfilePopover {"));
            QVERIFY(at > 0);
            const QString block = src.mid(at, 300);
            QVERIFY2(!block.contains(QStringLiteral("anchors.centerIn")),
                     view);
        }
        const QString delegate =
            read(qmlPath(QStringLiteral("MessageDelegate.qml")));
        QVERIFY(delegate.contains(QStringLiteral("openSenderProfileForRow(parent)")));
        QVERIFY(delegate.contains(QStringLiteral("root.pointerAnchor()")));
        const QString pane = read(qmlPath(QStringLiteral("TimelinePane.qml")));
        QVERIFY(pane.contains(QStringLiteral("receiptReaderRow")));
        QVERIFY(pane.contains(QStringLiteral("timeline.openSenderProfile({")));
        const QString info = read(qmlPath(QStringLiteral("RoomInfoPanel.qml")));
        QVERIFY(info.contains(QStringLiteral("memberProfile.openFor(member, parent)")));
    }

    void timelineAndThreadViewsExposeOpenSenderProfile()
    {
        const QString pane = read(qmlPath(QStringLiteral("TimelinePane.qml")));
        const QString thread = read(qmlPath(QStringLiteral("ThreadPanel.qml")));
        QVERIFY(pane.contains(QStringLiteral("property var openSenderProfile:")));
        QVERIFY(thread.contains(QStringLiteral("property var openSenderProfile:")));
    }
};

QTEST_MAIN(ProfilePopoverContractTest)
#include "ProfilePopoverContractTest.moc"
