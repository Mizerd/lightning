// Preview tiles for the screen-share picker, under
// image://lightning-sharesource/<id>.
//
// WHY A PROVIDER AND NOT A FILE. These images do not exist until they are
// asked for: each one is a live grab of a window or a display taken at the
// moment the picker draws its row. Nothing is written to disk — a still of
// whatever the user has on screen is exactly the kind of thing that must not
// outlive the dialog that asked for it.
//
// ID FORMAT, deliberately two shapes rather than one opaque token:
//   `w<handle>`  a window, by HWND
//   `s<index>`   a display, by the picker's row index
// The picker builds these from the same fields the CONTROLLER reads when it
// starts the capture, so a tile and the share it previews cannot disagree
// about which thing they mean.
//
// Off Windows this always returns a null image: Linux has the xdg portal,
// which draws its own picker with its own previews, and the macOS list is
// displays only. The picker falls back to its glyph, which is what it showed
// before previews existed.
#pragma once

#include <QQuickImageProvider>

// A grab of the user's screen is content, not decoration. It is produced on
// demand, handed to one QML Image, and never cached, stored or logged.
class ShareSourceImageProvider : public QQuickImageProvider
{
public:
    ShareSourceImageProvider();

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
