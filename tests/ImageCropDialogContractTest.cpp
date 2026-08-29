// Every display image Lightning uploads goes through ONE crop dialog.
//
// This is a SOURCE contract, and it exists because the failure it guards is
// invisible at runtime until somebody uploads a picture: a new avatar or
// banner surface added later would go straight to its sink, the picture
// would upload uncropped, and nothing would look broken. There is no test
// that catches an upload site that simply never learned about the cropper.
//
// It pins four things:
//
//   1. Every display-image FileDialog hands its result to an ImageCropDialog
//      and NOT to the sink; the sink moves into `onCropped`.
//   2. There is exactly one cropper. `app.imageCrop` is reachable from
//      ImageCropDialog.qml alone, so nobody grows a second one.
//   3. The dialog previews through the staged-image provider and never
//      points an Image at the chosen file — that is the SVG gate
//      (CLAUDE.md §6), and it is only a gate if it is the ONLY route.
//   4. The sites deliberately left alone stay left alone: a chat attachment
//      must not be silently cropped, and a Save-As is not an upload.
//
// EVERY SWEEP HERE CARRIES A found > 0 GUARD. A scan that matches nothing
// passes silently and is worth nothing; that lesson is in CLAUDE.md §16 and
// this file obeys it.

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

/// Strip comments so a ban assertion cannot be satisfied — or defeated — by
/// prose. The `\n` in the trailing-comment class is load-bearing: a negated
/// character class matches newlines, and without it the expression eats
/// every following line until one ends in a quote.
QString withoutComments(const QString &source)
{
    QString out = source;
    out.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                  QRegularExpression::DotMatchesEverythingOption));
    out.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
    out.remove(QRegularExpression(QStringLiteral("(?m)\\s//[^\"'\\n]*$")));
    return out;
}

/// The object block declaring `id: <name>`, by BRACE MATCHING rather than by
/// a fixed window after the name. A fixed window is the trap this file has
/// recorded four times: a comment added inside the block pushes the code out
/// of range and the assertion silently reports it absent.
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

// Every display-image upload site in the application. Adding a surface and
// not adding it here is the omission this test cannot see, so the count is
// asserted too — a site DELETED from this table fails loudly rather than
// quietly reducing the coverage.
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
        // Guards the guard: a table trimmed to nothing would make every
        // sweep below pass by having no work to do.
        QCOMPARE(int(std::size(kSites)), 7);
    }

    // The picker CHOOSES a file. It must not upload one.
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

    // The sink moves into onCropped, with the role that decides the shape,
    // the mask and the output cap.
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

    // ONE cropper, written once. Two would drift.
    void onlyTheSharedDialogTouchesTheCropper()
    {
        QDir dir(QStringLiteral(QML_DIR));
        const QStringList files = dir.entryList({ QStringLiteral("*.qml") },
                                                QDir::Files);
        QVERIFY(files.size() > 20);   // the sweep really did see the tree
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

    // The preview is bytes the cropper already sniffed, served through the
    // staged-image provider. Pointing an Image at the user's own file:// URL
    // is exactly what would hand an .svg to Qt's loader.
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
        // The Image is bound to the staged URL and to nothing else, and
        // `previewUrl` is only ever written from the gate's own answer. A
        // second writer is how the chosen path would find its way back in.
        QVERIFY2(code.contains(QStringLiteral("source: root.previewUrl")),
                 "the preview Image no longer draws the staged bytes");
        QCOMPARE(code.count(QStringLiteral("root.previewUrl =")), 2);
        QVERIFY2(code.contains(QStringLiteral("root.previewUrl = info.previewUrl")),
                 "the preview URL no longer comes from the sniffing gate");
        QVERIFY2(code.contains(QStringLiteral("root.previewUrl = \"\"")),
                 "a refused file leaves the previous picture on screen");
        // Nothing in the dialog may touch a file:// URL itself.
        QVERIFY2(!code.contains(QStringLiteral("file://")),
                 "the dialog handles a raw file URL, which is the one thing "
                 "the staged provider exists to avoid");
        // The crop is computed in C++, not here.
        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.crop(")),
                 "the dialog no longer asks C++ to do the cropping");
        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.maxEdgeForRole(")),
                 "the dialog invents its own output ceiling instead of taking "
                 "the one the role defines");
        // Whichever way it closed, the staged bytes and decoded source go.
        QVERIFY2(code.contains(QStringLiteral("app.imageCrop.discard()")),
                 "a closed dialog leaves the source staged");
    }

    // A dialog that opens empty when a file is refused says nothing, and "I
    // chose a picture and nothing happened" is the worst possible answer.
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

    // A square picture is uploaded whatever the mask says: Matrix avatars are
    // square and every client draws its own circle. Punching transparent
    // corners in would make the picture wrong everywhere else.
    void theCircularMaskIsPresentationAndTheOutputStaysSquare()
    {
        const QString code = withoutComments(read(QStringLiteral("ImageCropDialog.qml")));
        QVERIFY(!code.isEmpty());
        QVERIFY2(code.contains(QStringLiteral("circular")),
                 "an avatar no longer previews as the circle it renders as");
        QVERIFY2(code.contains(QStringLiteral("aspect: role === \"banner\" ? 3.0 : 1.0")),
                 "the aspect ratio no longer follows from the role, so a site "
                 "can pick a shape the sink does not expect");
        // Nothing here may reach for a mask on the OUTPUT: the C++ side is
        // handed a rectangle and nothing else.
        QVERIFY2(!code.contains(QStringLiteral("imageCrop.setCircular"))
                     && !code.contains(QStringLiteral("imageCrop.mask")),
                 "the circle escaped into the uploaded image");
    }

    // AppTheme is the sole token source. The one exception is the slider
    // thumb, which rides its own fill boundary and is white on every theme —
    // the same decision the microphone slider documents.
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

    // The sites deliberately NOT wired. Sending a picture in a chat is not a
    // display image and must never be silently cropped; a Save-As is not an
    // upload at all; the application icon is a local window/taskbar icon that
    // is normalised by its own path and never reaches Matrix.
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
};

QTEST_APPLESS_MAIN(ImageCropDialogContractTest)
#include "ImageCropDialogContractTest.moc"
