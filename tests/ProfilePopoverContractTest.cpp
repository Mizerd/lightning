// MemberProfilePopover is instantiated exactly once per view (TimelinePane,
// ThreadPanel, RoomInfoPanel), never once per delegate row. Delegates route
// the click through their ListView's `openSenderProfile` function instead.
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
    // Every view that shows a member's profile owns exactly one
    // MemberProfilePopover instance.
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

    // No delegate may instantiate its own MemberProfilePopover. Scans every
    // *Delegate*.qml under qml/, so a future delegate is covered
    // automatically.
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

    // MessageDelegate specifically must route profile clicks through its
    // ListView's shared handler rather than reaching for a popover directly.
    void messageDelegateRoutesThroughTheViewsOpenSenderProfile()
    {
        const QString delegate = read(qmlPath(QStringLiteral("MessageDelegate.qml")));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("root.timelineView.openSenderProfile")));
        QVERIFY(!delegate.contains(QStringLiteral("MemberProfilePopover {")));
    }

    // The three owning views actually define the `openSenderProfile` function
    // their delegates call into (TimelinePane/ThreadPanel — RoomInfoPanel's
    // member rows use the popover directly, not a ListView-forwarded call).
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
