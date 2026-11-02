
#include <QtTest/QtTest>

#include <QFile>

class GifHoverStarContractTest : public QObject
{
    Q_OBJECT

    static QString read(const QString &name)
    {
        QFile file(QStringLiteral(QML_DIR "/") + name);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                               : QString{};
    }

    static QString starBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(QStringLiteral("id: gifStarLoader"));
        if (start < 0) return {};
        const int end = delegate.indexOf(QStringLiteral(
            "Label {\n                anchors.centerIn: parent\n"
            "                width: parent.width - 12"), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

    static QString stateBlock(const QString &delegate)
    {
        const int start = delegate.indexOf(
            QStringLiteral("readonly property bool starEligible:"));
        if (start < 0) return {};
        const int end = delegate.indexOf(
            QStringLiteral("objectName: \"imageSkeleton\""), start);
        if (end < start) return {};
        return delegate.mid(start, end - start);
    }

private Q_SLOTS:
    void hoverStarReplacedTheMenuItemEntirely()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("objectName: \"gifHoverStarButton\"")));
        QVERIFY(!delegate.contains(QStringLiteral("starGifMenuItem")));
    }

    void eligibilityGatingMatchesTheRemovedMenuItemForImageRows()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = stateBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("imageBox.isRasterImage")));
        QVERIFY(block.contains(QStringLiteral("model.mediaSourceAvailable === true")));
        QVERIFY(block.contains(QStringLiteral("app.mediaBridge.supported")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool isGif:\n"
            "                (model.mediaMimetype || \"\").toLowerCase() === \"image/gif\"")));
        QVERIFY(delegate.contains(QStringLiteral(
            "readonly property bool isRasterImage: {\n"
            "                var m = (model.mediaMimetype || \"\").toLowerCase()\n"
            "                return m === \"image/gif\" || m === \"image/png\"\n"
            "                    || m === \"image/jpeg\" || m === \"image/webp\"")));
    }

    void hoverStarIsScopedToImageRowsNotStickers()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        const int imageStart = delegate.indexOf(QStringLiteral("id: imageComponent"));
        const int stickerStart = delegate.indexOf(QStringLiteral("id: stickerComponent"));
        QVERIFY(imageStart >= 0 && stickerStart > imageStart);
        const QString imageBlock = delegate.mid(imageStart, stickerStart - imageStart);
        const int stickerEnd = delegate.indexOf(QStringLiteral("id: videoComponent"),
                                                stickerStart);
        QVERIFY(stickerEnd > stickerStart);
        const QString stickerBlock =
            delegate.mid(stickerStart, stickerEnd - stickerStart);
        QVERIFY(imageBlock.contains(QStringLiteral("gifHoverStarButton")));
        QVERIFY(!stickerBlock.contains(QStringLiteral("gifHoverStarButton")));
        QVERIFY(!stickerBlock.contains(QStringLiteral("starEligible")));
    }

    void onlyInstantiatedForAConfirmedGifViaALoader()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const int start = delegate.indexOf(QStringLiteral("id: gifStarLoader"));
        QVERIFY(start >= 0);
        const QString loaderHead = delegate.mid(start, 200);
        QVERIFY(loaderHead.contains(QStringLiteral("active: imageBox.starEligible")));
    }

    void hoverDetectionIsAHoverHandlerNeverAMouseArea()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("HoverHandler { id: starHover }")));
        QVERIFY(!block.contains(QStringLiteral("MouseArea {")));
        QVERIFY(block.contains(QStringLiteral(
            "HoverHandler {\n                            id: gifStarHover\n"
            "                        }")));
    }

    void oneActivationFunctionReachedByTapAndKeyboardAndAccessibility()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QCOMPARE(block.count(QStringLiteral("function activate()")), 1);
        QVERIFY(block.contains(QStringLiteral("onTapped: gifStarButton.activate()")));
        QVERIFY(block.contains(QStringLiteral(
            "Keys.onReturnPressed: (event) => {")));
        QVERIFY(block.contains(QStringLiteral(
            "Keys.onEnterPressed: (event) => {")));
        QVERIFY(block.contains(QStringLiteral(
            "Keys.onSpacePressed: (event) => {")));
        QVERIFY(block.contains(QStringLiteral(
            "Accessible.onPressAction: gifStarButton.activate()")));
        QVERIFY(!block.contains(QStringLiteral("AbstractButton {")));
    }

    void keyboardActivationIgnoresAutoRepeat()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QCOMPARE(block.count(QStringLiteral("if (!event.isAutoRepeat) gifStarButton.activate()")),
                 3);
    }

    void tapHandlerIsGatedOnTheSameVisibilityStateAsOpacity()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral(
            "readonly property bool revealed:\n"
            "                                gifStarHover.hovered || starHover.hovered\n"
            "                                || gifStarButton.activeFocus")));
        QVERIFY(block.contains(QStringLiteral(
            "TapHandler {\n"
            "                                enabled: gifStarButton.revealed\n")));
        QVERIFY(!block.contains(QStringLiteral(
            "enabled: gifStarButton.revealed || imageBox.starred")));
    }

    void hoverOnlyNeverParkedOnTheMedia()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("opacity: revealed ? 1 : 0")));
        QVERIFY(!block.contains(QStringLiteral(
            "opacity: (revealed || imageBox.starred) ? 1 : 0")));
        const QString picker = read(QStringLiteral("GifPicker.qml"));
        QVERIFY(!picker.isEmpty());
        QVERIFY(picker.contains(QStringLiteral(
            "opacity: tileHover.hovered || tile.current\n"
            "                                 || saveButton.visualFocus ? 1 : 0")));
        QVERIFY(!picker.contains(QStringLiteral(
            "opacity: tile.saved || tileHover.hovered ? 1 : 0")));
    }

    void activationResolvesByMediaKeyNeverAnIndex()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("var key = model.mediaKey || \"\"")));
        QVERIFY(block.contains(QStringLiteral("app.isChatGifStarred(key)")));
        QVERIFY(block.contains(QStringLiteral("app.unstarChatGif(key)")));
        QVERIFY(block.contains(QStringLiteral("app.starChatGif(key)")));
        QVERIFY(!block.contains(QStringLiteral(
            "app.gif.starredStore.isStarredThisSession(key)")));
        QVERIFY(!block.contains(QStringLiteral(
            "app.gif.starredStore.unstarByMediaKey(key)")));
        QVERIFY(!block.contains(QStringLiteral("currentIndex")));
    }

    void keyboardReachableWithStateDependentAccessibleName()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("activeFocusOnTab: true")));
        QVERIFY(block.contains(QStringLiteral("Accessible.role: Accessible.Button")));
        QVERIFY(block.contains(QStringLiteral(
            "Accessible.name: imageBox.starred")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Remove from saved\")")));
        QVERIFY(block.contains(QStringLiteral("qsTr(\"Save image\")")));
        QVERIFY(!block.contains(QStringLiteral("Star GIF")));
        QVERIFY(!block.contains(QStringLiteral("starred GIFs")));
        QVERIFY(!block.contains(QStringLiteral("qsTr(\"Save GIF\")")));
        QVERIFY(!block.contains(QStringLiteral("qsTr(\"Remove from saved GIFs\")")));
    }

    void savedStateIsAFillNotOnlyATint()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral(
            "color: imageBox.starred ? AppTheme.bolt")));
        QVERIFY(block.contains(QStringLiteral(
            "color: imageBox.starred\n"
            "                                       ? AppTheme.boltInk : AppTheme.scrimInk")));
        QVERIFY(!block.contains(QStringLiteral("AppTheme.presenceAway")));
    }

    void starIsAnchoredBottomRightNeverTopRight()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = starBlock(delegate);
        QVERIFY(!block.isEmpty());
        const int buttonStart = block.indexOf(QStringLiteral("id: gifStarButton"));
        QVERIFY(buttonStart >= 0);
        const QString buttonHead = block.mid(buttonStart, 200);
        QVERIFY(buttonHead.contains(QStringLiteral("anchors.bottom: parent.bottom")));
        QVERIFY(buttonHead.contains(QStringLiteral("anchors.right: parent.right")));
        QVERIFY(!buttonHead.contains(QStringLiteral("anchors.top: parent.top")));
    }

    void starredStateRefreshesFromEveryStoreSignalIncludingCountChanged()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(!delegate.isEmpty());
        QVERIFY(delegate.contains(QStringLiteral("function refreshStarredState()")));
        const int connStart = delegate.indexOf(
            QStringLiteral("Connections {\n                target: app.gif.starredStore"));
        QVERIFY(connStart >= 0);
        const int connEnd = delegate.indexOf(QStringLiteral("\n            }\n"),
                                             connStart);
        QVERIFY(connEnd > connStart);
        const QString connBlock = delegate.mid(connStart, connEnd - connStart);
        QVERIFY(connBlock.contains(QStringLiteral("function onStarFinished(")));
        QVERIFY(connBlock.contains(QStringLiteral("function onUnstarFinished(")));
        QVERIFY(connBlock.contains(QStringLiteral("function onCountChanged() {\n"
                                                  "                    imageBox.refreshStarredState()")));
        QVERIFY(connBlock.contains(QStringLiteral("imageBox.refreshStarredState()")));
        QVERIFY(delegate.contains(QStringLiteral(
            "onMediaIdentityChanged: {\n"
            "                animatedSource = \"\"\n"
            "                bridgeSource = \"\"\n"
            "                bridgeFailed = false\n"
            "                refreshBridgeSource()\n"
            "                refreshStarredState()\n"
            "            }")));
    }

    void eligibilityFlipRefreshesStarredState()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        QVERIFY(delegate.contains(
            QStringLiteral("onStarEligibleChanged: refreshStarredState()")));
    }

    void starEligibilityExcludesPendingLocalEchoRows()
    {
        const QString delegate = read(QStringLiteral("MessageDelegate.qml"));
        const QString block = stateBlock(delegate);
        QVERIFY(!block.isEmpty());
        QVERIFY(block.contains(QStringLiteral("!imageBox.pendingMedia")));
    }
};

QTEST_GUILESS_MAIN(GifHoverStarContractTest)
#include "GifHoverStarContractTest.moc"
