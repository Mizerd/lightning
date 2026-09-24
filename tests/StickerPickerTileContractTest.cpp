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
};

QTEST_MAIN(StickerPickerTileContractTest)
#include "StickerPickerTileContractTest.moc"
