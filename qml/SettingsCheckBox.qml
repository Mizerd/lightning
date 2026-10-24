import QtQuick
import QtQuick.Controls
import MatrixClient

// A Settings option checkbox: label at the app's reading size and a row at
// least a comfortable pointer target tall. The stock Basic CheckBox sizes its
// text from the application font and is only as tall as its 28px indicator
// contents, which read small beside the rest of the app (GitHub reports,
// 2026-10-23). Both come from AppTheme tokens so they follow the text scale.
CheckBox {
    palette.windowText: AppTheme.stormText
    font.pixelSize: AppTheme.settingsOptionSize
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding,
                             AppTheme.settingsControlMinHeight)
}
