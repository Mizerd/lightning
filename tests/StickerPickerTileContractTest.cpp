// StickerPicker tile contract: two silent failures the component-load gate
// cannot see (it never builds a tile).
//
// 1. A `required property` the model cannot supply by name makes
//    QQuickDelegateModel build no delegate at all while `count` stays
//    correct, so every required property must be a served role.
//
// 2. The picker compares MediaBridge's cache keys as strings (the
//    `animatedMediaReady(cacheKey)` and `mediaCached(cacheKey)` signals carry
//    nothing else), so a prefix rename would silently never match.
//
// Both cases guard their own sweep (found > 0) so they cannot pass
// vacuously.

#include "stickers/StickerImageModel.h"
#include "stickers/StickerPackModel.h"

#include <QByteArray>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QtTest/QtTest>

namespace {

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString pickerQml()
{
    return readFile(QStringLiteral(QML_DIR "/StickerPicker.qml"));
}

QString mediaBridgeCpp()
{
    return readFile(QStringLiteral(LIGHTNING_SRC_DIR "/media/MediaBridge.cpp"));
}

QString editorQml()
{
    return readFile(QStringLiteral(QML_DIR "/StickerPackEditor.qml"));
}

QString settingsQml()
{
    return readFile(QStringLiteral(QML_DIR "/StickerPacksSettings.qml"));
}

} // namespace

class StickerPickerTileContractTest : public QObject
{
    Q_OBJECT

private slots:
    // Every `required property` a StickerPicker delegate declares must be a
    // role one of the picker's two models serves (`index` is built in). A
    // dropped role fails naming that property.
    void everyRequiredPropertyOfAPickerTileIsARoleSomeModelServes()
    {
        const QString qml = pickerQml();
        QVERIFY2(!qml.isEmpty(), "StickerPicker.qml could not be read");

        QSet<QString> served{ QStringLiteral("index") };
        const auto collect = [&served](const QHash<int, QByteArray> &roles) {
            for (const QByteArray &name : roles)
                served.insert(QString::fromUtf8(name));
        };
        collect(StickerImageModel().roleNames());
        collect(StickerPackModel().roleNames());

        static const QRegularExpression re(
            QStringLiteral(R"(required\s+property\s+\w+\s+(\w+))"));
        int found = 0;
        auto it = re.globalMatch(qml);
        while (it.hasNext()) {
            const QString name = it.next().captured(1);
            ++found;
            QVERIFY2(served.contains(name),
                     qPrintable(QStringLiteral(
                         "StickerPicker.qml declares `required property … %1`, "
                         "but neither StickerImageModel nor StickerPackModel "
                         "serves a role by that name. QQuickDelegateModel "
                         "answers that by building ZERO delegates while the "
                         "view's count stays right, so the picker would show "
                         "the correct number of empty tiles.")
                                 .arg(name)));
        }
        // The sweep must have swept something.
        QVERIFY2(found >= 10,
                 qPrintable(QStringLiteral("only %1 required properties matched "
                                           "— the scan regex has drifted")
                                .arg(found)));
    }

    // The picker's `onAnimatedMediaReady` / `onMediaCached` handlers compare
    // the signal's cacheKey against keys they build; both prefixes must exist
    // on both sides, and a rename fails naming the missing prefix.
    void thePickerAndTheBridgeAgreeOnTheCacheKeyPrefixes()
    {
        const QString qml = pickerQml();
        const QString bridge = mediaBridgeCpp();
        QVERIFY2(!qml.isEmpty(), "StickerPicker.qml could not be read");
        QVERIFY2(!bridge.isEmpty(), "MediaBridge.cpp could not be read");

        // The animated ORIGINAL, materialised for the AnimatedImage.
        QVERIFY2(bridge.contains(QStringLiteral("\"mxcanim:\"")),
                 "MediaBridge.cpp no longer builds an \"mxcanim:\" cache key; "
                 "StickerPicker.qml's animatedCacheKey comparison can then "
                 "never match and animated stickers silently stop animating "
                 "in the picker");
        QVERIFY2(qml.contains(QStringLiteral("\"mxcanim:\"")),
                 "StickerPicker.qml no longer builds the \"mxcanim:\" key it "
                 "compares onAnimatedMediaReady against");

        // The still tile's server thumbnail: MediaBridge formats
        // "mxcimg:%1:%2" (edge, mxc); the picker rebuilds that spelling to
        // re-resolve its Image source on mediaCached.
        QVERIFY2(bridge.contains(QStringLiteral("mxcimg:%1:%2")),
                 "MediaBridge.cpp no longer builds \"mxcimg:<edge>:<mxc>\"; "
                 "StickerPicker.qml's stillCacheKey would stop matching and "
                 "a tile would never pick up its thumbnail bytes");
        QVERIFY2(qml.contains(QStringLiteral("\"mxcimg:\"")),
                 "StickerPicker.qml no longer builds the \"mxcimg:\" key");
    }

    // The editor's delegate needs every role it declares, and it must list the
    // un-narrowed model (C41-F07) and read permission from the manager's rule,
    // not the model role that is false for the account's own pack (C41-D01).
    void thePackEditorAndSettingsUseTheRightSources()
    {
        const QString editor = editorQml();
        const QString settings = settingsQml();
        QVERIFY2(!editor.isEmpty(), "StickerPackEditor.qml could not be read");
        QVERIFY2(!settings.isEmpty(), "StickerPacksSettings.qml could not be read");

        QSet<QString> served{ QStringLiteral("index") };
        for (const QByteArray &name : StickerImageModel().roleNames())
            served.insert(QString::fromUtf8(name));
        static const QRegularExpression re(
            QStringLiteral(R"(required\s+property\s+\w+\s+(\w+))"));
        int found = 0;
        auto it = re.globalMatch(editor);
        while (it.hasNext()) {
            const QString name = it.next().captured(1);
            ++found;
            QVERIFY2(served.contains(name), qPrintable(name));
        }
        QVERIFY2(found >= 5, "the scan regex has drifted");

        QVERIFY2(editor.contains(QStringLiteral("stickers.editorImages")),
                 "the editor must list editorImages, not the usage-narrowed grid");
        QVERIFY2(!editor.contains(QStringLiteral("stickers.images")),
                 "the editor reads the narrowed `images` model again");
        QVERIFY2(settings.contains(QStringLiteral("canManagePack(packRow.packId)")),
                 "Settings must ask the manager whether the pack is manageable");
        QVERIFY2(!settings.contains(QStringLiteral("packRow.canManage)")),
                 "Settings passes the ROOM permission role to the editor again");
    }

    // Keyboard behaviour added for C41-F03/F04/F09/F10/F11, as text: Escape
    // cancels an inline rename before it can close the dialog, focus goes
    // back to the opener, the grid takes Home/End/Page/Space and a focused
    // pack tile draws a ring.
    void theKeyboardPathsAreWired()
    {
        const QString editor = editorQml();
        const QString picker = pickerQml();
        QVERIFY(!editor.isEmpty() && !picker.isEmpty());
        QVERIFY(editor.contains(QStringLiteral("Keys.onEscapePressed")));
        QVERIFY(editor.contains(QStringLiteral("restoreTo.forceActiveFocus")));
        QVERIFY(editor.contains(QStringLiteral("positionViewAtIndex")));
        QVERIFY(picker.contains(QStringLiteral("Keys.onSpacePressed")));
        QVERIFY(picker.contains(QStringLiteral("Qt.Key_PageDown")));
        QVERIFY(picker.contains(QStringLiteral("Qt.Key_End")));
        QVERIFY(picker.contains(QStringLiteral("packTile.visualFocus")));
    }
};

QTEST_MAIN(StickerPickerTileContractTest)
#include "StickerPickerTileContractTest.moc"
