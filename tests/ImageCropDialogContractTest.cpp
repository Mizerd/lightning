
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QtTest/QtTest>

#include <iterator>

namespace {

QString read(const QString &relative)
{
    QFile file(QStringLiteral(QML_DIR "/") + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString withoutComments(const QString &source)
{
    QString out = source;
    out.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                  QRegularExpression::DotMatchesEverythingOption));
    out.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
    out.remove(QRegularExpression(QStringLiteral("(?m)\\s//[^\"'\\n]*$")));
    return out;
}

QString blockForId(const QString &source, const QString &id)
{
    const int marker = source.indexOf(QStringLiteral("id: ") + id);
    if (marker < 0)
        return {};
    int open = source.lastIndexOf(QLatin1Char('{'), marker);
    if (open < 0)
        return {};
    int depth = 0;
    for (int i = open; i < source.size(); ++i) {
        const QChar c = source.at(i);
        if (c == QLatin1Char('{'))
            ++depth;
        else if (c == QLatin1Char('}')) {
            if (--depth == 0)
                return source.mid(open, i - open + 1);
        }
    }
    return {};
}

struct Site
{
    const char *file;
    const char *pickerId;
    const char *cropperId;
    const char *role;
    const char *sink;
};

const Site kSites[] = {
    { "RoomInfoPanel.qml", "avatarDialog", "avatarCrop", "avatar",
      "app.roomInfo.setRoomAvatar(" },
    { "TimelinePane.qml", "spaceAvatarDialog", "spaceAvatarCrop", "avatar",
      "app.roomInfo.setRoomAvatar(" },
    { "TimelinePane.qml", "spaceBannerDialog", "spaceBannerCrop", "banner",
      "app.banners.setRoomBanner(" },
    { "SpaceSettingsDialog.qml", "spaceAvatarFile", "spaceAvatarCrop", "avatar",
      "app.roomInfo.setRoomAvatar(" },
    { "SpaceSettingsDialog.qml", "spaceBannerFile", "spaceBannerCrop", "banner",
      "app.banners.setRoomBanner(" },
    { "NewConversationDialog.qml", "avatarFileDialog", "avatarCrop", "avatar",
      "root.roomAvatarPath =" },
    { "SettingsScreen.qml", "bannerFileDialog", "ownBannerCrop", "banner",
      "app.banners.setOwnBanner(" },
};

}

class ImageCropDialogContractTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    void theTableItselfIsIntact()
    {
        // Guards the guard: an empty table would make every sweep pass.
        QCOMPARE(int(std::size(kSites)), 7);
    }

    void everyUploadSitePicksIntoTheCropperAndNotIntoItsSink()
    {
        int checked = 0;
        for (const Site &site : kSites) {
            const QString source = withoutComments(read(QLatin1String(site.file)));
            QVERIFY2(!source.isEmpty(), site.file);

            const QString picker = blockForId(source, QLatin1String(site.pickerId));
            QVERIFY2(!picker.isEmpty(),
                     qPrintable(QStringLiteral("%1: no FileDialog %2")
                                    .arg(QLatin1String(site.file),
                                         QLatin1String(site.pickerId))));
            QVERIFY2(picker.contains(QLatin1String(site.cropperId)
                                     + QStringLiteral(".openFor(")),
                     qPrintable(QStringLiteral("%1/%2 does not open the crop "
                                               "dialog — a chosen picture "
                                               "uploads uncropped")
                                    .arg(QLatin1String(site.file),
                                         QLatin1String(site.pickerId))));
            QVERIFY2(!picker.contains(QLatin1String(site.sink)),
                     qPrintable(QStringLiteral("%1/%2 still uploads straight "
                                               "from the picker, so the crop "
                                               "dialog is bypassed")
                                    .arg(QLatin1String(site.file),
                                         QLatin1String(site.pickerId))));
            ++checked;
        }
        QCOMPARE(checked, int(std::size(kSites)));
    }

    void everySiteUploadsTheCroppedResultUnderTheRightRole()
    {
        int checked = 0;
        for (const Site &site : kSites) {
            const QString source = withoutComments(read(QLatin1String(site.file)));
            QVERIFY2(!source.isEmpty(), site.file);

            const QString cropper = blockForId(source, QLatin1String(site.cropperId));
            QVERIFY2(!cropper.isEmpty(),
                     qPrintable(QStringLiteral("%1: no ImageCropDialog %2")
                                    .arg(QLatin1String(site.file),
                                         QLatin1String(site.cropperId))));
            QVERIFY2(cropper.contains(QStringLiteral("role: \"")
                                      + QLatin1String(site.role)
                                      + QStringLiteral("\"")),
                     qPrintable(QStringLiteral("%1/%2 is not declared %3, so "
                                               "it crops to the wrong shape "
                                               "and the wrong ceiling")
                                    .arg(QLatin1String(site.file),
                                         QLatin1String(site.cropperId),
                                         QLatin1String(site.role))));
            QVERIFY2(cropper.contains(QStringLiteral("onCropped")),
                     qPrintable(QStringLiteral("%1/%2 has no onCropped, so the "
                                               "crop is discarded")
                                    .arg(QLatin1String(site.file),
                                         QLatin1String(site.cropperId))));
            QVERIFY2(cropper.contains(QLatin1String(site.sink)),
                     qPrintable(QStringLiteral("%1/%2 no longer reaches %3")
                                    .arg(QLatin1String(site.file),
                                         QLatin1String(site.cropperId),
                                         QLatin1String(site.sink))));
            ++checked;
        }
        QCOMPARE(checked, int(std::size(kSites)));
    }

    void onlyTheSharedDialogTouchesTheCropper()
    {
        QDir dir(QStringLiteral(QML_DIR));
        const QStringList files = dir.entryList({ QStringLiteral("*.qml") },
                                                QDir::Files);
        QVERIFY(files.size() > 20);
        int offenders = 0;
        QStringList names;
        for (const QString &name : files) {
            if (name == QStringLiteral("ImageCropDialog.qml"))
                continue;
            const QString source = withoutComments(read(name));
            if (source.contains(QStringLiteral("app.imageCrop"))) {
                ++offenders;
                names.append(name);
            }
        }
        QVERIFY2(offenders == 0,
                 qPrintable(QStringLiteral("a second cropper grew in: %1")
                                .arg(names.join(QStringLiteral(", ")))));
    }

    void theDialogPreviewsThroughTheStagedProviderAndNeverTheChosenFile()
    {
        const QString source = read(QStringLiteral("ImageCropDialog.qml"));
        QVERIFY(!source.isEmpty());
        const QString code = withoutComments(source);

        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.load(")),
                 "the dialog no longer routes the chosen file through the "
                 "sniffing gate");
        QVERIFY2(code.contains(QStringLiteral("info.previewUrl")),
                 "the preview is no longer the staged URL the gate produced");
        QVERIFY2(!code.contains(QStringLiteral("source: fileUrl"))
                     && !code.contains(QStringLiteral("source: root.fileUrl")),
                 "the preview points at the chosen file, so an SVG reaches "
                 "the image loader");
        QVERIFY2(code.contains(QStringLiteral("source: root.previewUrl")),
                 "the preview Image no longer draws the staged bytes");
        QCOMPARE(code.count(QStringLiteral("root.previewUrl =")), 2);
        QVERIFY2(code.contains(QStringLiteral("root.previewUrl = info.previewUrl")),
                 "the preview URL no longer comes from the sniffing gate");
        QVERIFY2(code.contains(QStringLiteral("root.previewUrl = \"\"")),
                 "a refused file leaves the previous picture on screen");
        QVERIFY2(!code.contains(QStringLiteral("file://")),
                 "the dialog handles a raw file URL, which is the one thing "
                 "the staged provider exists to avoid");
        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.crop(")),
                 "the dialog no longer asks C++ to do the cropping");
        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.maxEdgeForRole(")),
                 "the dialog invents its own output ceiling instead of taking "
                 "the one the role defines");
        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.discard()")),
                 "a closed dialog leaves the source staged");
    }

    void aKeptAnimationComesOnlyFromTheCropper()
    {
        const QString code =
            withoutComments(read(QStringLiteral("ImageCropDialog.qml")));
        QVERIFY(!code.isEmpty());
        QVERIFY2(code.contains(QStringLiteral("source: root.animatedUrl")),
                 "the animated preview is gone, so this case tests nothing");
        QCOMPARE(code.count(QStringLiteral("root.animatedUrl =")), 2);
        QVERIFY2(code.contains(
                     QStringLiteral("root.animatedUrl = info.animatedUrl")),
                 "the animated preview no longer comes from the sniffing gate");
        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.useAnimation(")),
                 "a kept animation no longer goes through the cropper");
        QVERIFY2(code.contains(
                     QStringLiteral("app.imageCrop.canKeepAnimation(")),
                 "the dialog decides by itself whether an animation fits");
    }

    void aRefusalIsShownInTheDialogRatherThanSwallowed()
    {
        const QString code = withoutComments(read(QStringLiteral("ImageCropDialog.qml")));
        QVERIFY(!code.isEmpty());
        int described = 0;
        for (const auto *category : { "unsupported_image", "too_large",
                                      "undecodable", "unreadable" }) {
            if (code.contains(QLatin1String(category)))
                ++described;
        }
        QCOMPARE(described, 4);
        QVERIFY2(code.contains(QStringLiteral("cropErrorLabel")),
                 "the refusal has nowhere to render");
    }

    void theCircularMaskIsPresentationAndTheOutputStaysSquare()
    {
        const QString code = withoutComments(read(QStringLiteral("ImageCropDialog.qml")));
        QVERIFY(!code.isEmpty());
        QVERIFY2(code.contains(QStringLiteral("circular")),
                 "an avatar no longer previews as the circle it renders as");
        QVERIFY2(code.contains(QStringLiteral("aspect: role === \"banner\" ? 3.0 : 1.0")),
                 "the aspect ratio no longer follows from the role, so a site "
                 "can pick a shape the sink does not expect");
        QVERIFY2(!code.contains(QStringLiteral("imageCrop.setCircular"))
                     && !code.contains(QStringLiteral("imageCrop.mask")),
                 "the circle escaped into the uploaded image");
    }

    void theDialogPaintsFromThemeTokens()
    {
        const QString code = withoutComments(read(QStringLiteral("ImageCropDialog.qml")));
        QVERIFY(!code.isEmpty());
        static const QRegularExpression hex(QStringLiteral("#[0-9A-Fa-f]{3,8}"));
        QStringList literals;
        auto it = hex.globalMatch(code);
        while (it.hasNext())
            literals.append(it.next().captured(0).toUpper());
        for (const QString &literal : literals) {
            QVERIFY2(literal == QStringLiteral("#FFFFFF"),
                     qPrintable(QStringLiteral("raw colour literal %1 — "
                                               "AppTheme is the sole token "
                                               "source").arg(literal)));
        }
        QVERIFY2(code.count(QStringLiteral("AppTheme.")) > 10,
                 "the dialog stopped painting from tokens altogether");
    }

    void chatAttachmentsAndSaveAsAreDeliberatelyNotCropped()
    {
        struct Untouched { const char *file; const char *id; };
        const Untouched kUntouched[] = {
            { "MessageComposerBar.qml", "pickImageDialog" },
            { "MessageComposerBar.qml", "pickAttachmentsDialog" },
            { "MessageComposerBar.qml", "pickFileDialog" },
            { "ThreadPanel.qml", "threadAttachDialog" },
            { "SettingsScreen.qml", "appIconDialog" },
            { "SettingsScreen.qml", "importFileDialog" },
        };
        int checked = 0;
        for (const Untouched &site : kUntouched) {
            const QString source = withoutComments(read(QLatin1String(site.file)));
            QVERIFY2(!source.isEmpty(), site.file);
            const QString block = blockForId(source, QLatin1String(site.id));
            QVERIFY2(!block.isEmpty(),
                     qPrintable(QStringLiteral("%1: %2 is gone — decide "
                                               "deliberately whether its "
                                               "replacement crops")
                                    .arg(QLatin1String(site.file),
                                         QLatin1String(site.id))));
            QVERIFY2(!block.contains(QStringLiteral(".openFor(")),
                     qPrintable(QStringLiteral("%1/%2 now crops; a chat "
                                               "attachment or a Save-As is "
                                               "not a display image")
                                    .arg(QLatin1String(site.file),
                                         QLatin1String(site.id))));
            ++checked;
        }
        QCOMPARE(checked, int(std::size(kUntouched)));
    }
    void everyBannerSurfaceUsesTheRatioTheCropperProduces()
    {
        struct Surface { const char *file; const char *what; };
        const Surface surfaces[] = {
            { "MemberProfilePopover.qml", "the member profile card" },
            { "SpaceSettingsDialog.qml",  "the Space settings preview" },
            { "SettingsScreen.qml",       "your own banner preview" },
        };
        for (const Surface &s : surfaces) {
            const QString src = read(QString::fromLatin1(s.file));
            QVERIFY2(!src.isEmpty(), s.file);
            QVERIFY2(src.contains(QStringLiteral("width / 3")),
                     qPrintable(QStringLiteral("%1 (%2) does not derive its "
                                               "banner height from the 3:1 the "
                                               "cropper produces, so it crops "
                                               "the chosen region again")
                                    .arg(QString::fromLatin1(s.file),
                                         QString::fromLatin1(s.what))));
        }

        const QString dialog = read(QStringLiteral("ImageCropDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY2(dialog.contains(QStringLiteral("role === \"banner\" ? 3.0 : 1.0")),
                 "the crop aspect changed; every surface above pins 3:1");
    }

};

QTEST_APPLESS_MAIN(ImageCropDialogContractTest)
#include "ImageCropDialogContractTest.moc"
