import QtQuick

QtObject {
    id: dialog

    property string fileMode: "open"
    property string title: ""
    property var nameFilters: []
    property string selectedNameFilter: ""
    property url currentFolder
    property string currentName: ""
    property string acceptLabel: ""
    property string purpose: ""

    property url selectedFile
    property var selectedFiles: []

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
