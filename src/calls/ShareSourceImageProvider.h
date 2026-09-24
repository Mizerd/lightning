// Preview tiles for the screen-share picker, under
// image://lightning-sharesource/<id>.
//
// Each image is a live grab taken when the picker draws its row; nothing is
// written to disk, since a still of the user's screen must not outlive the
// dialog.
//
// Ids: `w<handle>` (a window, by HWND) or `s<index>` (a display, by the
// picker's row index), built from the same fields the controller uses to
// start the capture, so a tile always previews what would be shared.
//
// Off Windows this always returns a null image (Linux uses the portal's own
// picker; macOS lists displays only), and the picker shows its glyph.
#pragma once

#include <QQuickImageProvider>

// Screen grabs are content: produced on demand for one QML Image and never
// cached, stored or logged.
class ShareSourceImageProvider : public QQuickImageProvider
{
public:
    ShareSourceImageProvider();

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
