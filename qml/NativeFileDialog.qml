import QtQuick

// A file or folder chooser that is the desktop's own: the XDG portal's
// (Dolphin's, GTK's, COSMIC's) on Linux, the system dialog on Windows and
// macOS, Qt's widget dialog only where neither exists. Never QtQuick.Dialogs'
// FileDialog, which draws a non-native imitation whenever the Qt platform
// theme offers no native dialog (a fake path bar, no thumbnails).
//
// The API mirrors the parts of FileDialog this app used, so a call site reads
// the same: set title / nameFilters / fileMode, call open(), handle
// onAccepted with selectedFile / selectedFiles. The chooser itself lives in
// C++ (app.files, FileChooser) and answers asynchronously.
//
// A plain QtObject, not an Item: it may sit inside a layout without taking
// part in it.
QtObject {
    id: dialog

    // "open" | "openMany" | "save" | "folder"
    property string fileMode: "open"
    property string title: ""
    property var nameFilters: []
    property string selectedNameFilter: ""
    // Where to start when `purpose` remembers no folder.
    property url currentFolder
    // save: the suggested name (a leaf, or a url whose leaf is taken).
    property string currentName: ""
    property string acceptLabel: ""
    // Last-folder memory bucket ("attach", "image", "font", ...): the chooser
    // reopens where the last one of the same purpose ended.
    property string purpose: ""

    // Results, valid in onAccepted.
    property url selectedFile
    property var selectedFiles: []

    // True while the chooser is up; a second open() is ignored, so a double
    // click cannot raise two dialogs.
    readonly property bool busy: requestId >= 0
    property int requestId: -1

    signal accepted()
    signal rejected()

    function open() {
        if (dialog.busy || typeof app === "undefined" || !app || !app.files)
            return
        dialog.requestId = app.files.open({
            mode: dialog.fileMode,
            title: dialog.title,
            nameFilters: dialog.nameFilters,
            selectedNameFilter: dialog.selectedNameFilter,
            folder: dialog.currentFolder,
            currentName: dialog.currentName,
            acceptLabel: dialog.acceptLabel,
            purpose: dialog.purpose
        })
    }

    readonly property Connections chooserConnection: Connections {
        target: (typeof app !== "undefined" && app && app.files)
                ? app.files : null
        function onFinished(id, ok, urls, filter) {
            if (id !== dialog.requestId)
                return
            dialog.requestId = -1
            if (!ok || urls.length === 0) {
                dialog.rejected()
                return
            }
            var list = []
            for (var i = 0; i < urls.length; ++i)
                list.push(urls[i])
            dialog.selectedFiles = list
            dialog.selectedFile = list[0]
            if (filter && filter.length > 0)
                dialog.selectedNameFilter = filter
            dialog.accepted()
        }
    }
}
