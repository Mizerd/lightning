// StickerPicker tile contract.
//
// Two silent-failure modes that the animated-sticker round introduced or
// depends on. Neither throws, neither logs, and neither is visible to the
// component-load gate — which loads StickerPicker.qml but never builds a
// tile, because nothing in that test has a populated sticker model.
//
// 1. A `required property` the model cannot supply BY NAME makes
//    QQuickDelegateModel refuse to build the delegate AT ALL, while
//    rowCount() and the view's `count` stay correct (CLAUDE.md §16, the
//    Qt 6.8 proxy-roleNames finding: "roleNames PRESENT -> count=3
//    delegates=3, STRIPPED -> count=3 delegates=0"). The observable is a
//    picker with the right count and zero tiles. Adding `mimetype` to the
//    image tile put a new name on that contract, so the contract is pinned
//    here rather than trusted.
//
// 2. The picker compares MediaBridge's cache keys as STRINGS — it has to,
//    because `animatedMediaReady(cacheKey)` and `mediaCached(cacheKey)`
//    carry nothing else. A rename on the C++ side therefore does not break
//    a build or a test; it makes the comparison silently never match, and
//    the animation simply never appears. That is precisely the defect this
//    round fixed, so it must not be reintroducible by a rename.
//
// Both cases GUARD THEIR OWN SWEEP (found > 0): a scan that matches nothing
// passes vacuously, which is the recorded "mutation-check every new sweep"
// trap.

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
    // role name one of the picker's two models actually serves. `index` is
    // the exception: it is a built-in delegate property, not a model role.
    //
    // On a tree where StickerImageModel drops "mimetype" from roleNames()
    // this fails naming that exact property.
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
    // the signal's cacheKey against keys they build themselves. Those keys
    // are MediaBridge's, so both spellings must exist on both sides.
    //
    // On a tree where MediaBridge's prefix is renamed (or the picker's is)
    // this fails naming the prefix that went missing.
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
