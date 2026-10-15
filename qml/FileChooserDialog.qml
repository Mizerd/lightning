import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

// Lightning's own file chooser: what Linux shows when the desktop has no XDG
// portal FileChooser (a bare window manager, a portal without a backend).
// It replaced Qt's widget QFileDialog there, which ignored the theme, and
// before that Qt Quick's FileDialog, which came up with an empty name on
// every Save as (it fills the field only from a file that already exists).
//
// One instance lives in Main.qml and serves every request: FileChooser
// (app.files) emits inAppRequested(id, request) and this answers with
// answerInApp(id, ...). Call sites never see it; they keep using
// NativeFileDialog, which cannot tell this route from the portal's.
//
// The listing is FileBrowserModel (C++): one folder, read off the GUI thread.
//
// Keys: Enter opens a folder or accepts, Backspace and Alt+Up go up,
// Alt+Home goes home, Ctrl+L types a path, Ctrl+H shows hidden files,
// Escape cancels, and typing a name in the list jumps to it.
Dialog {
    id: root
    objectName: "fileChooserDialog"
    modal: true
    focus: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(840, parent ? parent.width - AppTheme.spacing24 * 2 : 840)
    height: Math.min(600, parent ? parent.height - AppTheme.spacing24 * 2 : 600)
    padding: AppTheme.spacing16
    standardButtons: Dialog.NoButton
    // Escape is handled below: it first leaves the path field or the new
    // folder row, and only then cancels.
    closePolicy: Popup.NoAutoClose
    Overlay.modal: Rectangle { color: AppTheme.modalScrim }
    // Above every other dialog, whichever was created last: a chooser is
    // always opened FROM something (Export room, Settings, a sticker pack).
    z: 1000

    readonly property var chooser: (typeof app !== "undefined" && app && app.files)
                                   ? app.files : null

    // The request being shown; -1 when none.
    property int requestId: -1
    // "open" | "openMany" | "save" | "folder"
    property string mode: "open"
    property string acceptLabel: ""
    // The caller's title; empty for the mode's own. (Not Dialog.title, which
    // the Basic style would draw a second time as a header.)
    property string heading: ""
    // [{label, patterns, raw}]
    property var filters: []
    property int filterIndex: 0
    // The save name FileChooser suggested, kept to put back after a folder
    // name is typed into the name field and entered.
    property string suggestedName: ""
    // openMany: the selected rows, by name.
    property var selection: ({})
    property int selectionAnchor: -1
    property bool editingPath: false
    property bool creatingFolder: false
    property string failure: ""
    // A name to select once the next listing arrives (a folder just left, a
    // folder just created).
    property string pendingSelect: ""
    // Requests that arrived while one was showing.
    property var queue: []
    property var places: []

    readonly property bool saving: mode === "save"
    readonly property bool pickingFolder: mode === "folder"
    readonly property bool many: mode === "openMany"
    readonly property bool active: requestId >= 0

    readonly property var currentFilter: filters.length > 0
                                         && filterIndex >= 0
                                         && filterIndex < filters.length
                                         ? filters[filterIndex] : null

    Connections {
        target: root.chooser
        function onInAppRequested(id, request) {
            var copy = request
            copy.id = id
            if (root.active) {
                var q = root.queue.slice()
                q.push(copy)
                root.queue = q
                return
            }
            root.begin(copy)
        }
    }

    FileBrowserModel {
        id: browser
        objectName: "fileChooserModel"
    }

    // A reset (a new listing, a sort, a filter, the hidden-files toggle)
    // keeps the row the reader was on, by name; a view left to itself would
    // jump to row 0, and in folder mode Select would then return that row.
    property bool resetting: false
    property string keepName: ""
    Connections {
        target: browser
        function onModelAboutToBeReset() {
            root.resetting = true
            var r = browser.get(fileList.currentIndex)
            root.keepName = r.name !== undefined ? r.name : ""
        }
        function onModelReset() {
            Qt.callLater(root.afterReset)
        }
    }

    // ── Requests ──

    function begin(request) {
        requestId = request.id
        mode = request.mode || "open"
        heading = request.title || ""
        acceptLabel = request.acceptLabel || ""
        filters = request.filters || []
        filterIndex = Math.max(0, Math.min(request.filterIndex || 0,
                                           filters.length - 1))
        suggestedName = saving ? (request.currentName || "") : ""
        selection = ({})
        selectionAnchor = -1
        editingPath = false
        creatingFolder = false
        failure = ""
        pendingSelect = ""
        places = browser.places()
        browser.foldersOnly = pickingFolder
        browser.nameFilters = currentFilter ? currentFilter.patterns : []
        filterBox.currentIndex = filterIndex
        var start = request.folder
        if (!start || start.toString().length === 0)
            start = places.length > 0 ? places[0].url : ""
        fileList.currentIndex = -1
        // The same folder as last time is read again: it may have changed.
        if (browser.folder.toString() === start.toString())
            browser.refresh()
        else
            browser.folder = start
        nameField.text = suggestedName
        open()
        // The name, without its extension, is selected and ready to be typed
        // over, as in every desktop's save dialog.
        var length = request.selectionLength !== undefined
                     ? request.selectionLength : suggestedName.length
        Qt.callLater(function() { root.focusStart(length) })
    }

    function focusStart(length) {
        if (saving) {
            nameField.forceActiveFocus()
            nameField.select(0, Math.max(0, Math.min(length, nameField.length)))
        } else {
            fileList.forceActiveFocus()
        }
    }

    function finish(urls) {
        var id = requestId
        if (id < 0)
            return
        requestId = -1
        var filter = currentFilter ? currentFilter.raw : ""
        close()
        if (chooser)
            chooser.answerInApp(id, true, urls, filter)
        next()
    }

    function cancel() {
        var id = requestId
        if (id < 0)
            return
        requestId = -1
        close()
        if (chooser)
            chooser.answerInApp(id, false, [], "")
        next()
    }

    function next() {
        if (queue.length === 0)
            return
        var q = queue.slice()
        var request = q.shift()
        queue = q
        Qt.callLater(function() { root.begin(request) })
    }

    // ── Navigation ──

    function navigate(url) {
        if (!url || url.toString().length === 0)
            return
        failure = ""
        editingPath = false
        creatingFolder = false
        selection = ({})
        selectionAnchor = -1
        fileList.currentIndex = -1
        browser.folder = url
    }

    function goUp() {
        var parentUrl = browser.parentFolder
        if (!parentUrl || parentUrl.toString().length === 0)
            return
        var crumbs = browser.breadcrumbs
        pendingSelect = crumbs.length > 0 ? crumbs[crumbs.length - 1].name : ""
        navigate(parentUrl)
    }

    function goHome() {
        if (places.length > 0)
            navigate(places[0].url)
    }

    function afterReset() {
        resetting = false
        var name = pendingSelect.length > 0 ? pendingSelect : keepName
        var i = name.length > 0 ? browser.indexOfName(name) : -1
        fileList.currentIndex = i
        if (i < 0)
            return
        if (name === pendingSelect) {
            pendingSelect = ""
            if (many)
                selectOnly(i)
        }
        fileList.positionViewAtIndex(i, ListView.Contain)
    }

    // ── Selection ──

    function selectOnly(row) {
        var r = browser.get(row)
        var s = ({})
        if (r.name !== undefined)
            s[r.name] = true
        selection = s
        selectionAnchor = row
    }

    function clickRow(row, modifiers) {
        fileList.currentIndex = row
        fileList.forceActiveFocus()
        var r = browser.get(row)
        if (many && (modifiers & Qt.ControlModifier)) {
            var s = Object.assign({}, selection)
            if (s[r.name])
                delete s[r.name]
            else
                s[r.name] = true
            selection = s
            selectionAnchor = row
        } else if (many && (modifiers & Qt.ShiftModifier)
                   && selectionAnchor >= 0) {
            var range = ({})
            var lo = Math.min(selectionAnchor, row)
            var hi = Math.max(selectionAnchor, row)
            for (var i = lo; i <= hi; ++i)
                range[browser.get(i).name] = true
            selection = range
        } else {
            selectOnly(row)
        }
        // Save: a file picked from the list is the name to save under.
        if (saving && !r.isDir)
            nameField.text = r.name
    }

    function isSelected(row, name) {
        if (many)
            return selection[name] === true
        return row === fileList.currentIndex
    }

    // Enter or a double-click on a row.
    function activateRow(row) {
        var r = browser.get(row)
        if (r.name === undefined)
            return
        if (r.isDir) {
            navigate(r.url)
            return
        }
        if (saving) {
            nameField.text = r.name
            acceptSave()
        } else if (!pickingFolder) {
            if (many)
                selectOnly(row)
            acceptCurrent()
        }
    }

    // The files the accept button would return now (open modes).
    function chosenUrls() {
        var urls = []
        if (many) {
            for (var i = 0; i < browser.count; ++i) {
                var r = browser.get(i)
                if (selection[r.name] && !r.isDir)
                    urls.push(r.url)
            }
            return urls
        }
        var one = browser.get(fileList.currentIndex)
        if (one.name !== undefined && !one.isDir)
            urls.push(one.url)
        return urls
    }

    readonly property bool canAccept: {
        if (!active)
            return false
        if (saving)
            return nameField.text.trim().length > 0
        if (pickingFolder)
            return browser.error.length === 0
        // A folder can always be entered with the accept button too.
        var current = browser.get(fileList.currentIndex)
        var chosen = root.selection
        return current.name !== undefined
               || (chosen !== undefined && chosenUrls().length > 0)
    }

    function acceptCurrent() {
        failure = ""
        if (saving) {
            acceptSave()
            return
        }
        if (pickingFolder) {
            var row = browser.get(fileList.currentIndex)
            finish([row.name !== undefined && row.isDir ? row.url : browser.folder])
            return
        }
        var urls = chosenUrls()
        if (urls.length === 0) {
            var current = browser.get(fileList.currentIndex)
            if (current.name !== undefined && current.isDir)
                navigate(current.url)
            return
        }
        finish(urls)
    }

    function acceptSave() {
        failure = ""
        var typed = nameField.text.trim()
        if (typed.length === 0)
            return
        var url = browser.resolve(typed)
        var info = browser.stat(url)
        if (info.timedOut) {
            failure = qsTr("That location isn't responding.")
            return
        }
        if (info.isDir) {
            // A folder's name: go there, and keep the file's name.
            navigate(url)
            nameField.text = suggestedName
            return
        }
        if (!info.parentExists) {
            failure = qsTr("That folder doesn't exist.")
            return
        }
        if (info.exists) {
            overwriteDialog.ask(url)
            return
        }
        finish([url])
    }

    // Save: choosing "PNG file (*.png)" makes "photo.jpg" "photo.png", as
    // Qt's and GTK's dialogs do. Only a filter naming one extension changes
    // the name, and only a name that has one.
    function chooseFilter(index) {
        filterIndex = index
        browser.nameFilters = currentFilter ? currentFilter.patterns : []
        if (!saving || !currentFilter || currentFilter.patterns.length !== 1)
            return
        var m = /^\*\.([A-Za-z0-9_+-]+)$/.exec(currentFilter.patterns[0])
        if (!m || !chooser)
            return
        var name = nameField.text
        var base = chooser.selectionLength(name)
        if (base > 0 && base < name.length)
            nameField.text = name.substring(0, base) + "." + m[1]
    }

    function submitPath() {
        var url = browser.resolve(pathField.text)
        var info = browser.stat(url)
        if (info.timedOut) {
            failure = qsTr("That location isn't responding.")
            return
        }
        if (info.isDir) {
            navigate(url)
            return
        }
        if (saving && info.parentExists) {
            var path = url.toString()
            nameField.text = decodeURIComponent(path.substring(path.lastIndexOf("/") + 1))
            navigate(browser.resolve(pathField.text + "/.."))
            return
        }
        if (!saving && !pickingFolder && info.exists) {
            finish([url])
            return
        }
        failure = qsTr("Nothing exists at that path.")
    }

    function createFolder() {
        var result = browser.createFolder(newFolderField.text)
        if (result.ok) {
            creatingFolder = false
            navigate(result.url)
            return
        }
        failure = result.error === "exists"
                  ? qsTr("A file or folder with that name already exists.")
                  : result.error === "invalid"
                    ? qsTr("Enter a folder name without “/”.")
                    : qsTr("The folder couldn't be created.")
    }

    // ── Presentation helpers ──

    function iconFor(kind) {
        switch (kind) {
        case "folder": return "folder"
        case "image": return "image"
        case "video": return "movie"
        case "audio": return "audio_file"
        case "pdf": return "picture_as_pdf"
        case "archive": return "folder_zip"
        case "text": return "description"
        }
        return "draft"
    }

    function placeIcon(kind) {
        switch (kind) {
        case "home": return "home"
        case "desktop": return "desktop_windows"
        case "documents": return "description"
        case "downloads": return "download"
        case "pictures": return "image"
        }
        return "hard_drive"
    }

    function placeLabel(kind) {
        switch (kind) {
        case "home": return qsTr("Home")
        case "desktop": return qsTr("Desktop")
        case "documents": return qsTr("Documents")
        case "downloads": return qsTr("Downloads")
        case "pictures": return qsTr("Pictures")
        }
        return qsTr("File system")
    }

    // Binary units, one decimal, and never "0.0 KB" for a 400-byte file.
    function humanSize(bytes) {
        if (bytes < 1024)
            return qsTr("%1 B").arg(bytes)
        if (bytes < 1024 * 1024)
            return qsTr("%1 KB").arg((bytes / 1024).toFixed(1))
        if (bytes < 1024 * 1024 * 1024)
            return qsTr("%1 MB").arg((bytes / (1024 * 1024)).toFixed(1))
        return qsTr("%1 GB").arg((bytes / (1024 * 1024 * 1024)).toFixed(1))
    }

    function humanDate(d) {
        if (!d || isNaN(d.getTime()))
            return ""
        var now = new Date()
        if (d.getFullYear() === now.getFullYear()
                && d.getMonth() === now.getMonth()
                && d.getDate() === now.getDate()) {
            var settings = (typeof app !== "undefined" && app && app.settings)
                           ? app.settings : null
            return settings ? Qt.formatTime(d, settings.clockTimeFormat)
                            : Qt.locale().toString(d, Locale.ShortFormat)
        }
        // The UI locale's month names, not Qt.formatDate's English ones.
        return Qt.locale().toString(d, "d MMM yyyy")
    }

    function sortBy(field) {
        if (browser.sortField === field) {
            browser.sortAscending = !browser.sortAscending
        } else {
            browser.sortField = field
            // Newest and largest first is what a reader wants from those.
            browser.sortAscending = field === "name"
        }
    }

    // Set once a listing has been running for a while.
    Timer {
        id: slowListing
        property bool slow: false
        interval: 1500
        running: browser.loading
        onRunningChanged: if (running) slow = false
        onTriggered: slow = true
    }

    // ── Typing a name in the list jumps to it ──

    property string typeAhead: ""
    Timer {
        id: typeAheadReset
        interval: 900
        onTriggered: root.typeAhead = ""
    }

    function typeToSelect(text) {
        typeAhead += text
        typeAheadReset.restart()
        // The first letter moves on from the current row (so pressing it
        // again cycles); a longer prefix may stay on it.
        var from = typeAhead.length === 1 ? fileList.currentIndex
                                          : fileList.currentIndex - 1
        var i = browser.findPrefix(typeAhead, from)
        if (i >= 0) {
            fileList.currentIndex = i
            fileList.positionViewAtIndex(i, ListView.Contain)
            if (many)
                selectOnly(i)
        }
    }

    onClosed: {
        // Closed some other way (the window went away): still an answer.
        if (root.requestId >= 0)
            root.cancel()
    }

    background: Rectangle {
        radius: AppTheme.radiusCard
        color: AppTheme.popoverSurface
        border.color: AppTheme.stormBorderStrong
        border.width: 1
    }

    contentItem: FocusScope {
        id: scope
        focus: true

        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Escape) {
                if (root.editingPath) {
                    root.editingPath = false
                    fileList.forceActiveFocus()
                } else if (root.creatingFolder) {
                    root.creatingFolder = false
                    fileList.forceActiveFocus()
                } else {
                    root.cancel()
                }
                event.accepted = true
            } else if ((event.modifiers & Qt.AltModifier)
                       && event.key === Qt.Key_Up) {
                root.goUp()
                event.accepted = true
            } else if ((event.modifiers & Qt.AltModifier)
                       && event.key === Qt.Key_Home) {
                root.goHome()
                event.accepted = true
            } else if ((event.modifiers & Qt.ControlModifier)
                       && event.key === Qt.Key_L) {
                pathField.text = decodeURIComponent(
                    browser.folder.toString().replace(/^file:\/\//, ""))
                root.editingPath = true
                Qt.callLater(function() {
                    pathField.forceActiveFocus()
                    pathField.selectAll()
                })
                event.accepted = true
            } else if ((event.modifiers & Qt.ControlModifier)
                       && event.key === Qt.Key_H) {
                browser.showHidden = !browser.showHidden
                event.accepted = true
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: AppTheme.spacing12

            // ── Title ──
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Label {
                    objectName: "fileChooserTitle"
                    // Caller-chosen text: never markup.
                    textFormat: Text.PlainText
                    text: root.heading.length > 0
                          ? root.heading
                          : root.saving ? qsTr("Save file")
                          : root.pickingFolder ? qsTr("Choose a folder")
                          : root.many ? qsTr("Open files")
                          : qsTr("Open file")
                    color: AppTheme.stormText
                    font.family: AppTheme.menuFont
                    font.pixelSize: AppTheme.textTitle
                    font.weight: AppTheme.weightBold
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                IconButton {
                    storm: true
                    iconName: "close"
                    iconSize: 18
                    implicitWidth: 28; implicitHeight: 28
                    Accessible.name: qsTr("Close")
                    onClicked: root.cancel()
                }
            }

            // ── Toolbar: up, home, the path, new folder, hidden files ──
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing6

                IconButton {
                    objectName: "fileChooserUp"
                    storm: true
                    size: "md"
                    iconName: "arrow_upward"
                    enabled: browser.parentFolder.toString().length > 0
                    Accessible.name: qsTr("Up one folder")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Up one folder (Alt+Up)")
                    onClicked: root.goUp()
                }
                IconButton {
                    objectName: "fileChooserHome"
                    storm: true
                    size: "md"
                    iconName: "home"
                    Accessible.name: qsTr("Home folder")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Home folder (Alt+Home)")
                    onClicked: root.goHome()
                }

                // The path: one button per folder, or a field after Ctrl+L
                // or a click on its empty end.
                Rectangle {
                    objectName: "fileChooserPathBar"
                    Layout.fillWidth: true
                    implicitHeight: AppTheme.buttonHeight
                    radius: AppTheme.radiusMd
                    color: AppTheme.stormInset
                    border.width: 1
                    border.color: AppTheme.stormFieldBorder
                    visible: !root.editingPath

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            pathField.text = decodeURIComponent(
                                browser.folder.toString().replace(/^file:\/\//, ""))
                            root.editingPath = true
                            Qt.callLater(function() {
                                pathField.forceActiveFocus()
                                pathField.selectAll()
                            })
                        }
                    }

                    ListView {
                        id: crumbList
                        objectName: "fileChooserCrumbs"
                        anchors.fill: parent
                        anchors.leftMargin: AppTheme.spacing4
                        anchors.rightMargin: AppTheme.spacing4
                        orientation: ListView.Horizontal
                        clip: true
                        interactive: contentWidth > width
                        boundsBehavior: Flickable.StopAtBounds
                        model: browser.breadcrumbs
                        onCountChanged: Qt.callLater(function() {
                            crumbList.positionViewAtEnd()
                        })
                        delegate: Row {
                            id: crumb
                            required property var modelData
                            required property int index
                            readonly property bool last:
                                index === crumbList.count - 1
                            height: crumbList.height
                            spacing: 0
                            Icon {
                                visible: crumb.index > 0
                                name: "chevron_right"
                                size: 16
                                color: AppTheme.stormTextMuted
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            AbstractButton {
                                id: crumbButton
                                anchors.verticalCenter: parent.verticalCenter
                                implicitHeight: AppTheme.buttonHeight - 8
                                implicitWidth: crumbRow.implicitWidth
                                               + AppTheme.spacing8 * 2
                                hoverEnabled: true
                                Accessible.name: crumbLabel.text
                                onClicked: root.navigate(crumb.modelData.url)
                                background: Rectangle {
                                    radius: AppTheme.radiusControl
                                    color: crumbButton.hovered
                                           ? AppTheme.stormSelection
                                           : "transparent"
                                }
                                contentItem: Row {
                                    id: crumbRow
                                    spacing: AppTheme.spacing4
                                    leftPadding: AppTheme.spacing8
                                    Icon {
                                        visible: crumb.modelData.kind !== "dir"
                                        name: crumb.modelData.kind === "home"
                                              ? "home" : "hard_drive"
                                        size: 16
                                        color: AppTheme.stormTextSecondary
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Label {
                                        id: crumbLabel
                                        // A folder name is whatever is on disk:
                                        // never markup.
                                        textFormat: Text.PlainText
                                        visible: crumb.modelData.kind !== "root"
                                        text: crumb.modelData.kind === "home"
                                              ? qsTr("Home") : crumb.modelData.name
                                        color: crumb.last ? AppTheme.stormText
                                                          : AppTheme.stormTextSecondary
                                        font.pixelSize: AppTheme.textBody
                                        font.weight: crumb.last
                                                     ? AppTheme.weightStrong
                                                     : AppTheme.weightBody
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                            }
                        }
                    }
                }
                AppTextField {
                    id: pathField
                    objectName: "fileChooserPathField"
                    storm: true
                    visible: root.editingPath
                    Layout.fillWidth: true
                    placeholderText: qsTr("Type a path")
                    Accessible.name: qsTr("Location")
                    onAccepted: root.submitPath()
                    onActiveFocusChanged: {
                        if (!activeFocus && root.editingPath)
                            root.editingPath = false
                    }
                }

                IconButton {
                    objectName: "fileChooserNewFolder"
                    storm: true
                    size: "md"
                    visible: root.saving || root.pickingFolder
                    iconName: "create_new_folder"
                    active: root.creatingFolder
                    Accessible.name: qsTr("New folder")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("New folder")
                    onClicked: {
                        root.creatingFolder = !root.creatingFolder
                        if (root.creatingFolder) {
                            newFolderField.text = ""
                            Qt.callLater(function() {
                                newFolderField.forceActiveFocus()
                            })
                        }
                    }
                }
                IconButton {
                    objectName: "fileChooserHidden"
                    storm: true
                    size: "md"
                    iconName: browser.showHidden ? "visibility" : "visibility_off"
                    active: browser.showHidden
                    Accessible.name: browser.showHidden
                                     ? qsTr("Hide hidden files")
                                     : qsTr("Show hidden files")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("%1 (Ctrl+H)").arg(Accessible.name)
                    onClicked: browser.showHidden = !browser.showHidden
                }
            }

            // ── New folder ──
            RowLayout {
                objectName: "fileChooserNewFolderRow"
                visible: root.creatingFolder
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                AppTextField {
                    id: newFolderField
                    objectName: "fileChooserNewFolderName"
                    storm: true
                    Layout.fillWidth: true
                    placeholderText: qsTr("Folder name")
                    Accessible.name: qsTr("New folder name")
                    onAccepted: root.createFolder()
                }
                AppButton {
                    storm: true
                    kind: "ghost"
                    text: qsTr("Cancel")
                    onClicked: root.creatingFolder = false
                }
                AppButton {
                    objectName: "fileChooserCreateFolder"
                    storm: true
                    kind: "primary"
                    text: qsTr("Create")
                    enabled: newFolderField.text.trim().length > 0
                    onClicked: root.createFolder()
                }
            }

            // ── Places | files ──
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: AppTheme.spacing12

                ListView {
                    id: placeList
                    objectName: "fileChooserPlaces"
                    Layout.preferredWidth: 172
                    Layout.fillHeight: true
                    // Narrow windows give the room to the files.
                    visible: root.width >= 560
                    clip: true
                    spacing: AppTheme.spacing2
                    model: root.places
                    activeFocusOnTab: false
                    delegate: AbstractButton {
                        id: place
                        required property var modelData
                        readonly property bool current:
                            browser.folder.toString() === modelData.url.toString()
                        width: placeList.width
                        implicitHeight: 34
                        hoverEnabled: true
                        Accessible.name: root.placeLabel(modelData.kind)
                        onClicked: root.navigate(modelData.url)
                        background: Rectangle {
                            radius: AppTheme.radiusMd
                            color: place.current ? AppTheme.menuHighlight
                                 : place.hovered ? AppTheme.stormSelection
                                 : "transparent"
                        }
                        contentItem: RowLayout {
                            spacing: AppTheme.spacing8
                            Icon {
                                Layout.leftMargin: AppTheme.spacing8
                                name: root.placeIcon(place.modelData.kind)
                                size: 18
                                color: place.current ? AppTheme.bolt
                                                     : AppTheme.stormTextSecondary
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.placeLabel(place.modelData.kind)
                                color: place.current ? AppTheme.menuItemActiveText
                                                     : AppTheme.stormText
                                font.pixelSize: AppTheme.textBody
                                font.weight: place.current ? AppTheme.weightStrong
                                                           : AppTheme.weightBody
                                elide: Label.ElideRight
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: AppTheme.radiusMd
                    color: AppTheme.stormInset
                    border.width: 1
                    border.color: AppTheme.stormBorder
                    clip: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 1
                        spacing: 0

                        // Column headers: click to sort, again to reverse.
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 30
                            Layout.leftMargin: AppTheme.spacing12
                            Layout.rightMargin: AppTheme.spacing12
                            spacing: AppTheme.spacing8
                            Repeater {
                                model: [
                                    { field: "name", label: qsTr("Name"), width: -1 },
                                    { field: "size", label: qsTr("Size"), width: 84 },
                                    { field: "modified", label: qsTr("Modified"), width: 132 }
                                ]
                                delegate: AbstractButton {
                                    id: header
                                    required property var modelData
                                    readonly property bool sorted:
                                        browser.sortField === modelData.field
                                    objectName: "fileChooserSort_" + modelData.field
                                    visible: modelData.field !== "size"
                                             || !root.pickingFolder
                                    Layout.fillWidth: modelData.width < 0
                                    Layout.preferredWidth: modelData.width < 0
                                                           ? -1 : modelData.width
                                    Layout.fillHeight: true
                                    hoverEnabled: true
                                    Accessible.name: qsTr("Sort by %1")
                                                     .arg(modelData.label)
                                    onClicked: root.sortBy(modelData.field)
                                    background: Item {}
                                    contentItem: RowLayout {
                                        spacing: AppTheme.spacing2
                                        Label {
                                            text: header.modelData.label
                                            textFormat: Text.PlainText
                                            color: header.hovered || header.sorted
                                                   ? AppTheme.stormText
                                                   : AppTheme.stormTextMuted
                                            font.pixelSize: AppTheme.textMeta
                                            font.weight: AppTheme.weightStrong
                                            elide: Label.ElideRight
                                            Layout.fillWidth: header.modelData.width < 0
                                        }
                                        Icon {
                                            visible: header.sorted
                                            name: browser.sortAscending
                                                  ? "expand_less" : "expand_more"
                                            size: 14
                                            color: AppTheme.stormTextMuted
                                        }
                                        Item {
                                            Layout.fillWidth: header.modelData.width >= 0
                                        }
                                    }
                                }
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 1
                            color: AppTheme.stormBorder
                        }

                        ListView {
                            id: fileList
                            objectName: "fileChooserList"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: browser
                            currentIndex: -1
                            keyNavigationEnabled: true
                            boundsBehavior: Flickable.StopAtBounds
                            highlightMoveDuration: 0
                            ScrollBar.vertical: AppScrollBar {
                                policy: ScrollBar.AsNeeded
                            }
                            Accessible.role: Accessible.List
                            Accessible.name: qsTr("Files")

                            onCurrentIndexChanged: {
                                // Arrow keys move the selection with them.
                                if (root.many && currentIndex >= 0
                                        && activeFocus && !root.resetting
                                        && root.selection[browser.get(currentIndex).name] !== true)
                                    root.selectOnly(currentIndex)
                            }

                            Keys.onPressed: (event) => {
                                if (event.key === Qt.Key_Return
                                        || event.key === Qt.Key_Enter) {
                                    if (fileList.currentIndex >= 0)
                                        root.activateRow(fileList.currentIndex)
                                    else if (root.canAccept)
                                        root.acceptCurrent()
                                    event.accepted = true
                                } else if (event.key === Qt.Key_Backspace) {
                                    root.goUp()
                                    event.accepted = true
                                } else if (event.key === Qt.Key_Space && root.many
                                           && fileList.currentIndex >= 0) {
                                    root.clickRow(fileList.currentIndex,
                                                  Qt.ControlModifier)
                                    event.accepted = true
                                } else if (event.text.length > 0
                                           && !(event.modifiers
                                                & (Qt.ControlModifier | Qt.AltModifier))
                                           && event.text.charCodeAt(0) > 32
                                           && event.key !== Qt.Key_Delete) {
                                    root.typeToSelect(event.text)
                                    event.accepted = true
                                }
                            }

                            delegate: Rectangle {
                                id: row
                                required property int index
                                required property string name
                                required property url url
                                required property bool isDir
                                required property real size
                                required property var modified
                                required property string kind
                                required property bool hidden
                                readonly property bool selected:
                                    root.isSelected(index, name)
                                width: fileList.width
                                height: 32
                                radius: AppTheme.radiusControl
                                color: selected ? AppTheme.menuHighlight
                                     : rowMouse.containsMouse
                                       ? Qt.alpha(AppTheme.menuHighlight, 0.5)
                                       : "transparent"
                                border.width: fileList.activeFocus
                                              && index === fileList.currentIndex ? 1 : 0
                                border.color: AppTheme.focusRing
                                Accessible.role: Accessible.ListItem
                                Accessible.name: name
                                Accessible.selected: selected

                                MouseArea {
                                    id: rowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: (mouse) => root.clickRow(row.index,
                                                                        mouse.modifiers)
                                    onDoubleClicked: root.activateRow(row.index)
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: AppTheme.spacing12
                                    anchors.rightMargin: AppTheme.spacing12
                                    spacing: AppTheme.spacing8
                                    Icon {
                                        name: root.iconFor(row.kind)
                                        size: 18
                                        color: row.isDir ? AppTheme.bolt
                                                         : AppTheme.stormTextSecondary
                                        opacity: row.hidden ? 0.6 : 1
                                    }
                                    Label {
                                        // A file name is whatever is on disk:
                                        // never markup.
                                        textFormat: Text.PlainText
                                        text: row.name
                                        Layout.fillWidth: true
                                        elide: Label.ElideMiddle
                                        color: row.selected ? AppTheme.menuItemActiveText
                                                            : AppTheme.stormText
                                        opacity: row.hidden ? 0.7 : 1
                                        font.pixelSize: AppTheme.textBody
                                    }
                                    Label {
                                        visible: !root.pickingFolder
                                        Layout.preferredWidth: 84
                                        horizontalAlignment: Text.AlignRight
                                        text: row.isDir ? "" : root.humanSize(row.size)
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
                                    }
                                    Label {
                                        Layout.preferredWidth: 132
                                        text: root.humanDate(row.modified)
                                        color: AppTheme.stormTextMuted
                                        font.pixelSize: AppTheme.textMeta
                                        elide: Label.ElideRight
                                    }
                                }
                            }
                        }
                    }
                    Label {
                        objectName: "fileChooserEmpty"
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: 15
                        width: parent.width - AppTheme.spacing24 * 2
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        // A listing still running after a moment says so: a
                        // hung network mount never answers at all.
                        visible: fileList.count === 0
                                 && (!browser.loading || slowListing.slow)
                        text: browser.loading
                              ? qsTr("Waiting for this folder to answer…")
                              : browser.error === "unavailable"
                              ? qsTr("This folder can't be read right now.")
                              : browser.error === "notFound"
                              ? qsTr("This folder doesn't exist.")
                              : browser.error === "denied"
                                ? qsTr("You don't have permission to open "
                                       + "this folder.")
                                : browser.filteredOut > 0
                                  ? qsTr("No files here match the filter.")
                                  : root.pickingFolder
                                    ? qsTr("No folders here.")
                                    : qsTr("This folder is empty.")
                        color: AppTheme.stormTextMuted
                        font.pixelSize: AppTheme.textBody
                    }
                }
            }

            Label {
                objectName: "fileChooserFailure"
                Layout.fillWidth: true
                visible: root.failure.length > 0
                text: root.failure
                wrapMode: Text.WordWrap
                color: AppTheme.stormDanger
                font.pixelSize: AppTheme.textMeta
            }

            // ── Name, type, buttons ──
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8

                Label {
                    visible: root.saving
                    text: qsTr("Name")
                    color: AppTheme.stormTextSecondary
                    font.pixelSize: AppTheme.textBody
                }
                AppTextField {
                    id: nameField
                    objectName: "fileChooserName"
                    storm: true
                    visible: root.saving
                    Layout.fillWidth: true
                    Accessible.name: qsTr("File name")
                    onAccepted: root.acceptSave()
                    Keys.onDownPressed: fileList.forceActiveFocus()
                }
                Item {
                    visible: !root.saving
                    Layout.fillWidth: true
                }
                AppComboBox {
                    id: filterBox
                    objectName: "fileChooserFilter"
                    storm: true
                    visible: root.filters.length > 0 && !root.pickingFolder
                    Layout.preferredWidth: 220
                    model: root.filters.map(function(f) { return f.label })
                    Accessible.name: qsTr("File type")
                    onActivated: (index) => root.chooseFilter(index)
                }
                AppButton {
                    objectName: "fileChooserCancel"
                    storm: true
                    kind: "ghost"
                    text: qsTr("Cancel")
                    onClicked: root.cancel()
                }
                AppButton {
                    objectName: "fileChooserAccept"
                    storm: true
                    kind: "primary"
                    enabled: root.canAccept
                    text: root.acceptLabel.length > 0 ? root.acceptLabel
                          : root.saving ? qsTr("Save")
                          : root.pickingFolder ? qsTr("Select folder")
                          : qsTr("Open")
                    onClicked: root.acceptCurrent()
                }
            }
        }
    }

    // ── Overwrite ──
    AppDialog {
        id: overwriteDialog
        objectName: "fileChooserOverwriteDialog"
        parent: Overlay.overlay
        destructive: true
        z: 1001
        width: Math.min(440, parent ? parent.width - AppTheme.spacing24 * 2 : 440)
        title: qsTr("Replace “%1”?").arg(fileName)
        closePolicy: Popup.CloseOnEscape

        property url target
        property string fileName: ""

        function ask(url) {
            target = url
            var s = url.toString()
            fileName = decodeURIComponent(s.substring(s.lastIndexOf("/") + 1))
            open()
            Qt.callLater(function() { keepButton.forceActiveFocus() })
        }

        onClosed: {
            if (root.requestId >= 0)
                nameField.forceActiveFocus()
        }

        ColumnLayout {
            width: overwriteDialog.availableWidth
            spacing: AppTheme.spacing16
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: qsTr("A file with this name already exists in this "
                           + "folder. Replacing it overwrites its contents.")
                color: AppTheme.stormText
                font.pixelSize: AppTheme.textBody
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: AppTheme.spacing8
                Item { Layout.fillWidth: true }
                AppButton {
                    id: keepButton
                    objectName: "fileChooserKeep"
                    storm: true
                    kind: "ghost"
                    text: qsTr("Cancel")
                    onClicked: overwriteDialog.close()
                }
                AppButton {
                    objectName: "fileChooserReplace"
                    storm: true
                    kind: "dangerPrimary"
                    text: qsTr("Replace")
                    onClicked: {
                        var url = overwriteDialog.target
                        overwriteDialog.close()
                        root.finish([url])
                    }
                }
            }
        }
    }
}
