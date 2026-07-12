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
        QVERIFY(delegate.contains(QStringLiteral("qsTr(\"Expand\")")));
        QVERIFY(delegate.contains(QStringLiteral("qsTr(\"Collapse\")")));
        QVERIFY(delegate.contains(QStringLiteral(
            "visible: !root.isVirtualRow && !root.isStateActivity")));
    }
};

QTEST_MAIN(QmlBindingContractTest)
#include "QmlBindingContractTest.moc"
