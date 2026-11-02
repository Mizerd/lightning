
#include <QtTest/QtTest>

#include <QFile>

class MessageActionBarFitTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

private Q_SLOTS:
    void theRowDoesNotClipItsHoverBar()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY2(delegate.contains(QStringLiteral("clip: false")),
                 "the row must not clip, or a thin row cuts the action bar");
        QVERIFY2(!delegate.contains(
                     QStringLiteral("clip: ListView.view === null")),
                 "the TableView-era conditional clip is what chopped the bar");
    }

    void theBarIsAnchoredInTheRowNotMappedIntoAnOverlay()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("id: messageActionBarLoader")));
        QVERIFY(delegate.contains(QStringLiteral("anchors.top: parent.top")));
        QVERIFY(delegate.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY2(!delegate.contains(QStringLiteral("mappedRowAnchor")),
                 "an overlay-mapped anchor cannot survive the row rotation");

        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY2(!pane.contains(QStringLiteral("sharedMessageActionBar")),
                 "the shared instance was reverted");
        QVERIFY2(!pane.contains(QStringLiteral("claimActionBar")),
                 "the claim/release handshake was reverted");
    }

    void bothPanelsUseTheSameBar()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY2(!delegate.contains(QStringLiteral("active: root.inThreadPanel")),
                 "the bar must not be gated to the thread panel");
        QVERIFY(delegate.contains(
            QStringLiteral("active: latched || root.actionsVisible")));
        QVERIFY(delegate.contains(QStringLiteral("visible: root.actionsVisible")));
    }

    void onlyOneRowCanShowTheBar()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY2(pane.contains(QStringLiteral("property string hoveredActionsKey")),
                 "the view must own the single hovered-row key");

        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral(
            "root.timelineView.hoveredActionsKey === actionKey")));
        QVERIFY2(delegate.contains(QStringLiteral(
                     "&& root.timelineView.hoveredActionsKey === \"\"")),
                 "a pinned row must yield to a hovered one, or two bars show");
        QVERIFY(delegate.contains(QStringLiteral(
            "root.timelineView.hoveredActionsKey = root.actionKey")));
    }

    void theBarIsCreatedOnFirstNeed()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("property bool latched: false")));
        QVERIFY(delegate.contains(QStringLiteral(
            "onLoaded: Qt.callLater(function() { latched = true })")));
    }
};

QTEST_MAIN(MessageActionBarFitTest)
#include "MessageActionBarFitTest.moc"
