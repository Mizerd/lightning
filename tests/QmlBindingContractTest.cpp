#include <QtTest/QtTest>

#include <QFile>

class QmlBindingContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

    // The `stateActivity` Item through the sibling `layout` ColumnLayout
    // that follows it in MessageDelegate.qml.
    static QString stateActivityBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(QStringLiteral("id: stateActivity"));
        if (start < 0) return {};
        const int end = delegate.indexOf(
            QStringLiteral("\n    ColumnLayout {\n        id: layout"), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

private Q_SLOTS:
    void dialogsHaveIndependentBoundedWidths()
    {
        const QString roomInfo = read(QStringLiteral("RoomInfoPanel.qml"));
        const QString account = read(QStringLiteral("AccountMenu.qml"));
        QVERIFY(!roomInfo.isEmpty());
        QVERIFY(!account.isEmpty());
        QVERIFY(roomInfo.contains(QStringLiteral(
            "width: Math.max(240, Math.min(400, parent ? parent.width - 32 : 400))")));
        QVERIFY(account.contains(QStringLiteral(
            "width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))")));
        QVERIFY(!roomInfo.contains(QStringLiteral("Layout.maximumWidth: 360")));
        QVERIFY(!account.contains(QStringLiteral("Layout.maximumWidth: 380")));
    }

    void paginationVisibilityDoesNotDependOnGeometry()
    {
        const QString pane = read(QStringLiteral("TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("app.pagination.presentationState")));
        QVERIFY(pane.contains(QStringLiteral("PaginationController.Hidden ? 0 : 32")));
        QVERIFY(!pane.contains(QStringLiteral("showPaginationStatus")));
        QVERIFY(!pane.contains(QStringLiteral(
            "height: paginationHeader.visible ? paginationHeader.implicitHeight")));
    }

    void animatedGifSpinnerUsesActiveRendererState()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral(
            "running: imageBox.animateGif")));
        QVERIFY(delegate.contains(QStringLiteral(
            "animatedImg.status === AnimatedImage.Loading")));
        QVERIFY(delegate.contains(QStringLiteral("onMediaIdentityChanged")));
    }

    void stateActivityUsesNeutralGroupedPresentation()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(QStringLiteral("stateGroupEntries")));
        QVERIFY(delegate.contains(QStringLiteral(
            "visible: !root.isVirtualRow && !root.isStateActivity")));
        // No message-bubble-like card: the old Rectangle+cardElevated+border
        // treatment for the collapsed row must be gone, replaced with a
        // compact, clickable summary row.
        const QString block = stateActivityBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(!block.contains(QStringLiteral("AppTheme.cardElevated")));
        QVERIFY(block.contains(QStringLiteral("summaryRow")));
    }

    // 0.5.14 checkpoint 2: clicking Expand did nothing because the summary
    // row referenced the bare `ListView.view` attached property, which is
    // only populated on the delegate's own root item, not on nested
    // children — every other action in this same file correctly qualifies
    // with `root.ListView.view`. Pin that convention for the state-activity
    // controls specifically, since that's exactly where it regressed.
    void stateActivityQualifiesListViewViewOnNestedControls()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = stateActivityBlock(delegate);
        QVERIFY(!block.isEmpty());
        const int qualifiedCount = block.count(QStringLiteral("root.ListView.view"));
        const int totalCount = block.count(QStringLiteral("ListView.view"));
        QVERIFY(qualifiedCount > 0);
        // A bare, unqualified "ListView.view" inside a nested child would
        // silently resolve to that child's own (never populated) attached
        // object instead of the delegate root's — every reference in this
        // block must be root-qualified.
        QCOMPARE(totalCount, qualifiedCount);
    }
};

QTEST_MAIN(QmlBindingContractTest)
#include "QmlBindingContractTest.moc"
