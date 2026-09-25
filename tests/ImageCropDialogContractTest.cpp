// Every display image Lightning uploads goes through one crop dialog. A
// source contract, because a new upload surface that skips the cropper looks
// fine at runtime. Pinned:
//   1. Every display-image FileDialog hands its result to an ImageCropDialog,
//      and the sink moves into `onCropped`.
//   2. There is one cropper: only ImageCropDialog.qml reaches `app.imageCrop`.
//   3. The dialog previews through the staged-image provider and never points
//      an Image at the chosen file; that is the SVG gate, and only a gate if
//      it is the only route.
//   4. Chat attachments are not cropped, and Save-As is not an upload.
// Every sweep carries a found > 0 guard.

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

/// Strip comments so prose cannot satisfy or defeat a ban. The `\n` in the
/// trailing-comment class matters: a negated class matches newlines and
/// would otherwise eat following lines.
QString withoutComments(const QString &source)
{
    QString out = source;
    out.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                  QRegularExpression::DotMatchesEverythingOption));
    out.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
    out.remove(QRegularExpression(QStringLiteral("(?m)\\s//[^\"'\\n]*$")));
    return out;
}

/// The object block declaring `id: <name>`, by brace matching rather than a
/// fixed window, which a comment inside the block could push out of range.
QString blockForId(const QString &source, const QString &id)
{
    const int marker = source.indexOf(QStringLiteral("id: ") + id);
    if (marker < 0)
        return {};
    // Walk back to the brace that opened this object.
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

/// One display-image upload site.
struct Site
{
    const char *file;
    const char *pickerId;   // the FileDialog
    const char *cropperId;  // the ImageCropDialog it opens
    const char *role;       // "avatar" | "banner"
    const char *sink;       // the call that actually uploads
};

// Every display-image upload site. The count is asserted too, so a deleted
// row fails loudly instead of reducing coverage.
const Site kSites[] = {
    // Room Information → the room's own avatar.
    { "RoomInfoPanel.qml", "avatarDialog", "avatarCrop", "avatar",
      "app.roomInfo.setRoomAvatar(" },
    // Space Home card → the Space's avatar (m.room.avatar; a Space IS a room).
    { "TimelinePane.qml", "spaceAvatarDialog", "spaceAvatarCrop", "avatar",
      "app.roomInfo.setRoomAvatar(" },
    // Space Home → the Space banner.
    { "TimelinePane.qml", "spaceBannerDialog", "spaceBannerCrop", "banner",
      "app.banners.setRoomBanner(" },
    // Space settings → avatar.
    { "SpaceSettingsDialog.qml", "spaceAvatarFile", "spaceAvatarCrop", "avatar",
      "app.roomInfo.setRoomAvatar(" },
    // Space settings → banner.
    { "SpaceSettingsDialog.qml", "spaceBannerFile", "spaceBannerCrop", "banner",
      "app.banners.setRoomBanner(" },
    // Room creation → the picture applied after the room exists.
    { "NewConversationDialog.qml", "avatarFileDialog", "avatarCrop", "avatar",
      "root.roomAvatarPath =" },
    // Settings → the account's own profile banner.
    { "SettingsScreen.qml", "bannerFileDialog", "ownBannerCrop", "banner",
      "app.banners.setOwnBanner(" },
};

} // namespace

class ImageCropDialogContractTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    void theTableItselfIsIntact()
    {
        // Guards the guard: an empty table would make every sweep pass.
        QCOMPARE(int(std::size(kSites)), 7);
    }

    // The picker chooses a file; it does not upload one.
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

    // The sink runs in onCropped, with the role that decides shape, mask and
    // output cap.
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

    // One cropper; two would drift.
    void onlyTheSharedDialogTouchesTheCropper()
    {
        QDir dir(QStringLiteral(QML_DIR));
        const QStringList files = dir.entryList({ QStringLiteral("*.qml") },
                                                QDir::Files);
        QVERIFY(files.size() > 20);   // the sweep saw the tree
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

    // The preview shows bytes the cropper already sniffed, via the
    // staged-image provider; an Image on the user's file:// URL would hand an
    // .svg to Qt's loader.
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
        // The Image binds only to the staged URL, and `previewUrl` is written
        // only from the gate's answer; a second writer would let the chosen
        // path back in.
        QVERIFY2(code.contains(QStringLiteral("source: root.previewUrl")),
                 "the preview Image no longer draws the staged bytes");
        QCOMPARE(code.count(QStringLiteral("root.previewUrl =")), 2);
        QVERIFY2(code.contains(QStringLiteral("root.previewUrl = info.previewUrl")),
                 "the preview URL no longer comes from the sniffing gate");
        QVERIFY2(code.contains(QStringLiteral("root.previewUrl = \"\"")),
                 "a refused file leaves the previous picture on screen");
        // Nothing in the dialog touches a file:// URL.
        QVERIFY2(!code.contains(QStringLiteral("file://")),
                 "the dialog handles a raw file URL, which is the one thing "
                 "the staged provider exists to avoid");
        // The crop is computed in C++, not here.
        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.crop(")),
                 "the dialog no longer asks C++ to do the cropping");
        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.maxEdgeForRole(")),
                 "the dialog invents its own output ceiling instead of taking "
                 "the one the role defines");
        // However it closed, the staged bytes and decoded source are discarded.
        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.discard()")),
                 "a closed dialog leaves the source staged");
    }

    // An animation is previewed from the copy the cropper wrote after
    // sniffing it, never the chosen file, and "Keep animation" uploads through
    // the cropper too.
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

    // A refused file is explained in the dialog, not swallowed.
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

    // The circular mask is presentation only: Matrix avatars are square and
    // clients draw their own circle, so the uploaded image stays square.
    void theCircularMaskIsPresentationAndTheOutputStaysSquare()
    {
        const QString code = withoutComments(read(QStringLiteral("ImageCropDialog.qml")));
        QVERIFY(!code.isEmpty());
        QVERIFY2(code.contains(QStringLiteral("circular")),
                 "an avatar no longer previews as the circle it renders as");
        QVERIFY2(code.contains(QStringLiteral("aspect: role === \"banner\" ? 3.0 : 1.0")),
                 "the aspect ratio no longer follows from the role, so a site "
                 "can pick a shape the sink does not expect");
        // No mask on the output: the C++ side is handed a rectangle only.
        QVERIFY2(!code.contains(QStringLiteral("imageCrop.setCircular"))
                     && !code.contains(QStringLiteral("imageCrop.mask")),
                 "the circle escaped into the uploaded image");
    }

    // AppTheme is the only token source, except the slider thumb, which is
    // white on every theme (as the microphone slider documents).
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

    // Deliberately not wired: chat pictures are not display images and are
    // never silently cropped; Save-As is not an upload; the application icon
    // is local and never reaches Matrix.
    void chatAttachmentsAndSaveAsAreDeliberatelyNotCropped()
    {
        struct Untouched { const char *file; const char *id; };
        const Untouched kUntouched[] = {
            { "MessageComposerBar.qml", "pickImageDialog" },
            { "MessageComposerBar.qml", "pickAttachmentsDialog" },
            { "MessageComposerBar.qml", "pickFileDialog" },
            { "ThreadPanel.qml", "threadAttachDialog" },
            { "ImageViewerOverlay.qml", "saveDialog" },
            { "TimelinePane.qml", "saveMediaDialog" },
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
    // Every banner surface uses the cropper's 3:1 ratio, or
    // PreserveAspectCrop crops the user's chosen strip a second time.
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

        // The dialog itself must still be 3:1.
        const QString dialog = read(QStringLiteral("ImageCropDialog.qml"));
        QVERIFY(!dialog.isEmpty());
        QVERIFY2(dialog.contains(QStringLiteral("role === \"banner\" ? 3.0 : 1.0")),
                 "the crop aspect changed; every surface above pins 3:1");
    }

};

QTEST_APPLESS_MAIN(ImageCropDialogContractTest)
#include "ImageCropDialogContractTest.moc"
