import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import MatrixClient

Item {
    id: root
    objectName: "loginScreen"

    function submit() {
        if (!app.auth.isLoggingIn)
            app.auth.login(homeserverField.text, userField.text,
                           passField.text)
    }

    onVisibleChanged: if (!visible) passField.text = ""

    property var labsAccountRows: null
    function refreshLabsAccountRows() {
        root.labsAccountRows = app.accountList
            ? app.accountList.rowsSnapshot() : null
    }
    Connections {
        target: app
        ignoreUnknownSignals: true
        function onLiveAccountsActiveChanged() { root.refreshLabsAccountRows() }
    }
    Connections {
        target: app.accountList ? app.accountList : null
        function onRowsChanged() { root.refreshLabsAccountRows() }
    }

    readonly property string browserAuthorityName: {
        var raw = (app.auth.discoveredHomeserver || "").trim()
        if (raw.length === 0)
            return ""
        var host = raw.replace(/^[A-Za-z][A-Za-z0-9+.-]*:\/\//, "").split("/")[0]
        if (host.indexOf("]") < 0) {
            var colon = host.indexOf(":")
            if (colon > 0)
                host = host.substring(0, colon)
        }
        return host
    }
    readonly property string serverDisplayName:
        (app.auth.discoveredHomeserver || "").replace(/^https:\/\//, "")

    readonly property string signInMode: {
        if (app.auth.discoveryState !== "done")
            return ""
        if (app.auth.serverOffersBrowserLogin
                && !(app.auth.serverOffersPassword
                     && (root.passwordChosen || app.auth.browserLoginUnavailable)))
            return "oauth"
        if (app.auth.serverOffersPassword)
            return "password"
        if (app.auth.serverOffersSso)
            return "sso"
        return ""
    }
    property string passwordChosenFor: ""
    readonly property bool passwordChosen: root.passwordChosenFor !== ""
                                           && root.passwordChosenFor
                                              === app.auth.discoveredHomeserver
    readonly property bool showsSso: (signInMode === "password" || signInMode === "sso")
                                     && app.auth.serverOffersSso
    readonly property int providerCount: app.auth.ssoProviders.length

    property bool editingServer: false
    property string serverTextBeforeEdit: ""
    property bool serverAnsweredSinceEdit: false
    property string lastDiscoveryState: ""
    property bool lastBrowserUnavailable: false
    readonly property bool serverFieldShown: app.auth.discoveryState !== "done"
                                             || homeserverField.activeFocus
                                             || editingServer
    property bool advanceWhenServerAnswers: false

    function choosePassword(chosen) {
        root.passwordChosenFor = chosen ? app.auth.discoveredHomeserver : ""
        app.auth.clearLastError()
        Qt.callLater(root.focusWayIn)
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.visible && app.auth.isLoggingIn
        onActivated: app.auth.cancelLogin()
    }

    function editServer() {
        serverTextBeforeEdit = homeserverField.text
        serverAnsweredSinceEdit = false
        editingServer = true
        homeserverField.forceActiveFocus()
        homeserverField.selectAll()
    }

    function revealRepairCard() {
        if (!repairCard.visible)
            return
        const top = repairCard.mapToItem(loginFlick.contentItem, 0, 0).y
        const bottom = top + repairCard.height + AppTheme.spacingXL
        const overflow = bottom - (loginFlick.contentY + loginFlick.height)
        if (overflow <= 0)
            return
        const maxY = Math.max(0, loginFlick.contentHeight - loginFlick.height)
        if (root.contentYBeforeReveal < 0)
            root.contentYBeforeReveal = loginFlick.contentY
        loginFlick.contentY = Math.min(maxY, loginFlick.contentY + overflow)
    }
    property real contentYBeforeReveal: -1
    function returnFromRepairCard() {
        if (repairCard.visible || root.contentYBeforeReveal < 0)
            return
        const back = root.contentYBeforeReveal
        root.contentYBeforeReveal = -1
        if (!loginFlick.dragging && !loginFlick.flicking)
            loginFlick.contentY = Math.max(0, Math.min(back, loginFlick.contentY))
    }

    function followFocus() {
        const win = root.Window.window
        const item = win ? win.activeFocusItem : null
        if (!item || loginFlick.dragging || loginFlick.flicking)
            return
        let p = item
        while (p && p !== loginFlick.contentItem)
            p = p.parent
        if (!p)
            return
        const margin = AppTheme.spacingL
        const top = item.mapToItem(loginFlick.contentItem, 0, 0).y
        const bottom = top + item.height
        const maxY = Math.max(0, loginFlick.contentHeight - loginFlick.height)
        let y = loginFlick.contentY
        if (top - margin < y)
            y = top - margin
        else if (bottom + margin > y + loginFlick.height)
            y = bottom + margin - loginFlick.height
        loginFlick.contentY = Math.max(0, Math.min(maxY, y))
    }
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() { root.followFocus() }
    }

    function focusWayIn() {
        if (signInMode === "password")
            userField.forceActiveFocus()
        else if (signInMode === "oauth")
            browserLoginBtn.forceActiveFocus()
        else if (signInMode === "sso" && ssoLoginBtn.visible)
            ssoLoginBtn.forceActiveFocus()
        else if (signInMode === "sso" && ssoProviderRepeater.count > 0)
            ssoProviderRepeater.itemAt(0).forceActiveFocus()
    }

    Connections {
        target: app.auth
        function onDiscoveryChanged() {
            const state = app.auth.discoveryState
            if (root.lastDiscoveryState === "probing" && state !== "probing")
                root.serverAnsweredSinceEdit = true
            root.lastDiscoveryState = state
            const unavailable = app.auth.browserLoginUnavailable
            if (unavailable && !root.lastBrowserUnavailable)
                Qt.callLater(root.focusWayIn)
            root.lastBrowserUnavailable = unavailable
            if (!root.advanceWhenServerAnswers)
                return
            if (app.auth.discoveryState === "done") {
                root.advanceWhenServerAnswers = false
                root.editingServer = false
                Qt.callLater(root.focusWayIn)
            } else if (app.auth.discoveryState === "failed") {
                root.advanceWhenServerAnswers = false
            }
        }
    }

    Connections {
        target: app.auth
        function onLoginFailed(reason) {
            if (root.signInMode === "password" && passField.text.length > 0
                    && userField.text.length > 0) {
                passField.forceActiveFocus()
            }
        }
    }

    // A provider's logo, or a neutral glyph for a brand with none. Never a
    // letter. A server-supplied icon is not fetched: nothing can fetch media
    // before sign-in (see docs/feature-contracts.md).
    component ProviderLogo: Item {
        id: logo
        property string brand: ""
        property color ink: AppTheme.textPrimary
        // Own keys only: "constructor" or "__proto__" must not reach the
        // object's prototype.
        readonly property string path: {
            const key = brand.toLowerCase()
            return Object.prototype.hasOwnProperty.call(brandPaths, key)
                ? brandPaths[key] : ""
        }
        readonly property string shownBrand: path.length > 0 ? brand.toLowerCase()
                                                             : "generic"
        readonly property var brandPaths: ({
            "apple": "M12.152 6.896c-.948 0-2.415-1.078-3.96-1.04-2.04.027-3.91 1.183-4.961 3.014-2.117 3.675-.546 9.103 1.519 12.09 1.013 1.454 2.208 3.09 3.792 3.039 1.52-.065 2.09-.987 3.935-.987 1.831 0 2.35.987 3.96.948 1.637-.026 2.676-1.48 3.676-2.948 1.156-1.688 1.636-3.325 1.662-3.415-.039-.013-3.182-1.221-3.22-4.857-.026-3.04 2.48-4.494 2.597-4.559-1.429-2.09-3.623-2.324-4.39-2.376-2-.156-3.675 1.09-4.61 1.09zM15.53 3.83c.843-1.012 1.4-2.427 1.245-3.83-1.207.052-2.662.805-3.532 1.818-.78.896-1.454 2.338-1.273 3.714 1.338.104 2.715-.688 3.559-1.701",
            "facebook": "M9.101 23.691v-7.98H6.627v-3.667h2.474v-1.58c0-4.085 1.848-5.978 5.858-5.978.401 0 .955.042 1.468.103a8.68 8.68 0 0 1 1.141.195v3.325a8.623 8.623 0 0 0-.653-.036 26.805 26.805 0 0 0-.733-.009c-.707 0-1.259.096-1.675.309a1.686 1.686 0 0 0-.679.622c-.258.42-.374.995-.374 1.752v1.297h3.919l-.386 2.103-.287 1.564h-3.246v8.245C19.396 23.238 24 18.179 24 12.044c0-6.627-5.373-12-12-12s-12 5.373-12 12c0 5.628 3.874 10.35 9.101 11.647Z",
            "github": "M12 .297c-6.63 0-12 5.373-12 12 0 5.303 3.438 9.8 8.205 11.385.6.113.82-.258.82-.577 0-.285-.01-1.04-.015-2.04-3.338.724-4.042-1.61-4.042-1.61C4.422 18.07 3.633 17.7 3.633 17.7c-1.087-.744.084-.729.084-.729 1.205.084 1.838 1.236 1.838 1.236 1.07 1.835 2.809 1.305 3.495.998.108-.776.417-1.305.76-1.605-2.665-.3-5.466-1.332-5.466-5.93 0-1.31.465-2.38 1.235-3.22-.135-.303-.54-1.523.105-3.176 0 0 1.005-.322 3.3 1.23.96-.267 1.98-.399 3-.405 1.02.006 2.04.138 3 .405 2.28-1.552 3.285-1.23 3.285-1.23.645 1.653.24 2.873.12 3.176.765.84 1.23 1.91 1.23 3.22 0 4.61-2.805 5.625-5.475 5.92.42.36.81 1.096.81 2.22 0 1.606-.015 2.896-.015 3.286 0 .315.21.69.825.57C20.565 22.092 24 17.592 24 12.297c0-6.627-5.373-12-12-12",
            "gitlab": "m23.6004 9.5927-.0337-.0862L20.3.9814a.851.851 0 0 0-.3362-.405.8748.8748 0 0 0-.9997.0539.8748.8748 0 0 0-.29.4399l-2.2055 6.748H7.5375l-2.2057-6.748a.8573.8573 0 0 0-.29-.4412.8748.8748 0 0 0-.9997-.0537.8585.8585 0 0 0-.3362.4049L.4332 9.5015l-.0325.0862a6.0657 6.0657 0 0 0 2.0119 7.0105l.0113.0087.03.0213 4.976 3.7264 2.462 1.8633 1.4995 1.1321a1.0085 1.0085 0 0 0 1.2197 0l1.4995-1.1321 2.4619-1.8633 5.006-3.7489.0125-.01a6.0682 6.0682 0 0 0 2.0094-7.003z",
            "google": "M12.48 10.92v3.28h7.84c-.24 1.84-.853 3.187-1.787 4.133-1.147 1.147-2.933 2.4-6.053 2.4-4.827 0-8.6-3.893-8.6-8.72s3.773-8.72 8.6-8.72c2.6 0 4.507 1.027 5.907 2.347l2.307-2.307C18.747 1.44 16.133 0 12.48 0 5.867 0 .307 5.387.307 12s5.56 12 12.173 12c3.573 0 6.267-1.173 8.373-3.36 2.16-2.16 2.84-5.213 2.84-7.667 0-.76-.053-1.467-.173-2.053H12.48z",
            "twitter": "M14.234 10.162 22.977 0h-2.072l-7.591 8.824L7.251 0H.258l9.168 13.343L.258 24H2.33l8.016-9.318L16.749 24h6.993zm-2.837 3.299-.929-1.329L3.076 1.56h3.182l5.965 8.532.929 1.329 7.754 11.09h-3.182z",
            "x": "M14.234 10.162 22.977 0h-2.072l-7.591 8.824L7.251 0H.258l9.168 13.343L.258 24H2.33l8.016-9.318L16.749 24h6.993zm-2.837 3.299-.929-1.329L3.076 1.56h3.182l5.965 8.532.929 1.329 7.754 11.09h-3.182z"
        })

        objectName: "providerLogo"
        implicitWidth: 18
        implicitHeight: 18
        Accessible.ignored: true
        Shape {
            anchors.fill: parent
            visible: logo.path.length > 0
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: logo.ink
                strokeColor: "transparent"
                strokeWidth: 0
                scale: Qt.size(logo.width / 24, logo.height / 24)
                PathSvg { path: logo.path }
            }
        }
        Icon {
            anchors.centerIn: parent
            visible: logo.path.length === 0
            name: "account_circle"
            size: 18
            color: logo.ink
        }
    }

    readonly property string failureServer:
        app.localSessionFailureReasonCode !== ""
            ? (app.localSessionFailureHomeserver || "") : ""

    function applyFailureIdentityToFields() {
        if (app.localSessionFailureReasonCode === "")
            return
        if (app.localSessionFailureUserId !== "")
            userField.text = app.localSessionFailureUserId
        if (root.failureServer !== "") {
            if (homeserverField.text !== root.failureServer)
                homeserverField.text = root.failureServer
            app.auth.discoverAuthMethods(homeserverField.text)
        }
    }
    Component.onCompleted: {
        if (lastDiscoveryState === "")
            lastDiscoveryState = app.auth.discoveryState
        applyFailureIdentityToFields()
        root.refreshLabsAccountRows()
    }
    Connections {
        target: app
        function onLocalSessionFailureChanged() {
            root.applyFailureIdentityToFields()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: AppTheme.background
    }

    Flickable {
        id: loginFlick
        objectName: "loginFlick"
        anchors.fill: parent
        contentWidth: width
        contentHeight: panel.y + panel.implicitHeight + AppTheme.spacingXL
        onContentHeightChanged: if (repairCard.visible) Qt.callLater(root.revealRepairCard)
        onHeightChanged: if (repairCard.visible) Qt.callLater(root.revealRepairCard)
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
        SmoothWheelArea {}

        Rectangle {
            id: panel
            objectName: "loginPanel"
            anchors.horizontalCenter: parent.horizontalCenter

            y: Math.max(AppTheme.spacingXL, Math.min(96, Math.round(loginFlick.height * 0.08)))
            width: Math.max(300, Math.min(loginFlick.width - AppTheme.spacingXL * 2, 420))
            implicitHeight: loginForm.implicitHeight + AppTheme.spacingXL * 2
            radius: AppTheme.radiusLg
            color: AppTheme.surface
            border.color: AppTheme.border
            border.width: 1

            ColumnLayout {
                id: loginForm
                x: AppTheme.spacingXL
                y: AppTheme.spacingXL
                width: parent.width - AppTheme.spacingXL * 2
                spacing: AppTheme.spacingM
                AppButton {
                    id: backToAppButton
                    objectName: "backToAppButton"
                    visible: app.accounts && app.accounts.hasActiveAccount
                             && !((app.localSessionFailureReasonCode === "access_token_revoked"
                                   || app.localSessionFailureReasonCode === "access_token_expired")
                                  && app.localSessionFailureUserId
                                     === app.accounts.activeUserId)
                             && !(app.localSessionFailureReasonCode !== ""
                                  && !app.loggedIn
                                  && app.localSessionFailureUserId
                                     === app.accounts.activeUserId)
                             && !(app.keyringUnavailable === true && !app.loggedIn)
                    Accessible.name: qsTr("Back to the app")
                    text: qsTr("← Back")
                    onClicked: {
                        const auth = app.auth
                        if (app.showMain())
                            auth.clearLastError()
                    }
                }

                ColumnLayout {
                    objectName: "signedInAccountsRecovery"
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingS
                    visible: (app.accountList
                              ? (root.labsAccountRows || []).length > 0
                              : (app.accounts
                                 && app.accounts.accounts.length > 0))
                             && !(app.accounts.hasActiveAccount && app.loggedIn)

                    Label {
                        text: qsTr("Already on this device")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                    }
                    Repeater {
                        model: app.accountList
                               ? (root.labsAccountRows || [])
                               : (app.accounts ? app.accounts.accounts : [])
                        delegate: AppButton {
                            required property var modelData
                            objectName: "recoverAccountButton"
                            Layout.fillWidth: true
                            text: modelData.displayName
                                  && modelData.displayName.length > 0
                                  ? modelData.displayName + "  ·  " + modelData.userId
                                  : modelData.userId
                            Accessible.name: qsTr("Open %1").arg(modelData.userId)
                            onClicked: {
                                app.auth.clearLastError()
                                app.switchToAccount(modelData.userId)
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "settingsReadProblemNotice"
                    visible: !!(app.settings && app.settings.settingsReadProblem)
                    Layout.fillWidth: true
                    radius: AppTheme.radiusMd
                    color: AppTheme.surfaceAlt
                    border.color: AppTheme.warning
                    border.width: 1
                    implicitHeight: settingsProblemText.implicitHeight + AppTheme.spacingM * 2
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: settingsProblemText.text

                    Label {
                        id: settingsProblemText
                        objectName: "settingsReadProblemText"
                        x: AppTheme.spacingM
                        y: AppTheme.spacingM
                        width: parent.width - AppTheme.spacingM * 2
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        text: app.settings && app.settings.settingsReadProblem === 2
                              ? qsTr("Lightning found data on this computer for an "
                                     + "account it doesn't list, for example from a "
                                     + "sign-in that didn't finish. Sign in to that "
                                     + "account to use it again. Nothing has been "
                                     + "deleted.")
                              : qsTr("Lightning couldn't read its saved settings, so "
                                     + "accounts on this computer may not be listed. "
                                     + "Their data has not been deleted.")
                        color: AppTheme.text
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                }

                Rectangle {
                    objectName: "keyringUnavailableNotice"
                    visible: app.keyringUnavailable === true
                    Layout.fillWidth: true
                    radius: AppTheme.radiusMd
                    color: AppTheme.surfaceAlt
                    border.color: AppTheme.warning
                    border.width: 1
                    implicitHeight: keyringNoticeColumn.implicitHeight + AppTheme.spacingM * 2
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: keyringNoticeText.text

                    ColumnLayout {
                        id: keyringNoticeColumn
                        x: AppTheme.spacingM
                        y: AppTheme.spacingM
                        width: parent.width - AppTheme.spacingM * 2
                        spacing: AppTheme.spacingS

                        Label {
                            id: keyringNoticeText
                            objectName: "keyringUnavailableText"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            text: qsTranslate("AppController",
                                              "Lightning can't read this device's saved sign-ins right now — "
                                              + "the system keyring is locked or unavailable. Unlock it and "
                                              + "try again.")
                            color: AppTheme.text
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }
                        AppButton {
                            objectName: "keyringRetryButton"
                            kind: "primary"
                            enabled: !app.auth.isLoggingIn
                            text: qsTr("Try again")
                            Accessible.name: text
                            onClicked: app.retryKeyring()
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingXS
                    Icon {
                        objectName: "loginBrandBolt"
                        name: "bolt"
                        size: 18
                        color: AppTheme.wordmarkBolt
                        Accessible.ignored: true
                    }
                    Label {
                        text: "Lightning"
                        color: AppTheme.text
                        font.family: AppTheme.displayFont
                        font.pixelSize: AppTheme.textTitle
                        font.weight: AppTheme.weightDisplay
                        font.letterSpacing: 0.3
                    }
                }

                Label {
                    objectName: "loginScreenHeading"
                    text: repair.active
                          ? qsTr("Fix this account")
                          : (app.accounts && app.accounts.hasActiveAccount)
                            ? qsTr("Add another account")
                            : qsTr("Sign in")
                    color: AppTheme.text
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textDisplay
                    font.weight: AppTheme.weightDisplay
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightDisplay
                    lineHeightMode: Text.ProportionalHeight
                }
                Label {
                    text: app.backendName === "mock"
                          ? qsTr("Mock backend — any credentials work")
                          : qsTr("Sign in with your Matrix account")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    Layout.fillWidth: true
                    Layout.bottomMargin: AppTheme.spacingXS
                }
                ColumnLayout {
                    objectName: "serverBlock"
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingXS

                    Label {
                        //: Label above the field for the Matrix server the
                        //: account is on, e.g. matrix.org.
                        text: qsTr("Server")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }

                    Item {
                        Layout.fillWidth: true
                        implicitHeight: AppTheme.buttonHeight

                        AppTextField {
                            id: homeserverField
                            objectName: "homeserverField"
                            anchors.fill: parent
                            visible: root.serverFieldShown
                            Component.onCompleted: {
                                if (text.length > 0)
                                    return
                                text = root.failureServer !== ""
                                    ? root.failureServer
                                    : app.settings.loginHomeserverPrefill
                                if (text.length > 0)
                                    app.auth.discoverAuthMethods(text)
                            }
                            placeholderText: "https://matrix.org"
                            Accessible.name: qsTr("Server")
                            onTextEdited: {
                                app.auth.clearLastError()
                                // A password typed for the server that was
                                // here must never ride along to another one
                                // (C48-F02).
                                passField.text = ""
                                root.serverAnsweredSinceEdit = false
                                discoverDebounce.restart()
                            }
                            Timer {
                                id: discoverDebounce
                                interval: 700
                                onTriggered:
                                    app.auth.discoverAuthMethods(homeserverField.text)
                            }
                            onEditingFinished: {
                                discoverDebounce.stop()
                                app.settings.loginHomeserverPrefill = text
                                app.auth.discoverAuthMethods(text)
                            }
                            onAccepted: {
                                discoverDebounce.stop()
                                if (root.passwordChosen && root.editingServer
                                        && app.auth.discoveryState === "done"
                                        && text.trim()
                                           === root.serverTextBeforeEdit.trim()) {
                                    text = root.serverTextBeforeEdit
                                    root.editingServer = false
                                    root.focusWayIn()
                                    return
                                }
                                app.auth.discoverAuthMethods(
                                    text, !root.serverAnsweredSinceEdit)
                                if (app.auth.discoveryState === "done") {
                                    root.editingServer = false
                                    root.focusWayIn()
                                } else if (app.auth.discoveryState === "probing") {
                                    root.advanceWhenServerAnswers = true
                                }
                            }
                            onActiveFocusChanged: {
                                if (!activeFocus)
                                    root.editingServer = false
                            }
                            Keys.onEscapePressed: function(event) {
                                if (!root.editingServer) {
                                    event.accepted = false
                                    return
                                }
                                text = root.serverTextBeforeEdit
                                discoverDebounce.stop()
                                app.auth.discoverAuthMethods(text)
                                root.editingServer = false
                                serverChangeButton.forceActiveFocus()
                            }
                        }

                        RowLayout {
                            objectName: "serverSummary"
                            anchors.fill: parent
                            visible: !root.serverFieldShown
                            spacing: AppTheme.spacingS

                            Label {
                                objectName: "serverNameLabel"
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: root.serverDisplayName
                                elide: Text.ElideRight
                                color: AppTheme.textPrimary
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textBody
                                font.weight: AppTheme.weightStrong
                                Accessible.role: Accessible.StaticText
                                Accessible.name: qsTr("Server: %1").arg(text)
                            }
                            StatusChip {
                                id: insecureChip
                                objectName: "serverNotEncryptedChip"
                                visible: app.auth.serverConnectionInsecure
                                tone: "warning"
                                iconName: "lock_open"
                                //: Shown next to the server when its connection
                                //: uses http:// rather than https://.
                                label: qsTr("Not encrypted")
                                Accessible.role: Accessible.StaticText
                                Accessible.name: qsTr("The connection to this server "
                                                      + "is not encrypted (http).")
                                HoverHandler { id: insecureHover }
                                ToolTip.text: Accessible.name
                                ToolTip.visible: insecureHover.hovered
                                ToolTip.delay: 300
                            }
                            AppButton {
                                id: serverChangeButton
                                objectName: "serverChangeButton"
                                kind: "ghost"
                                size: "sm"
                                minWidth: 0
                                text: qsTr("Change")
                                Accessible.name: qsTr("Change server")
                                enabled: !app.auth.isLoggingIn
                                onClicked: root.editServer()
                            }
                        }
                    }

                    Item {
                        id: statusLine
                        Layout.fillWidth: true
                        implicitHeight: Math.max(18, serverStatus.implicitHeight)

                        readonly property string discovery:
                            discoverDebounce.running ? "idle"
                                                     : app.auth.discoveryState
                        readonly property string problem: app.auth.discoveryProblem

                        RowLayout {
                            anchors.fill: parent
                            spacing: AppTheme.spacingXS

                            AppBusyIndicator {
                                visible: statusLine.discovery === "probing"
                                running: visible
                                size: 14
                                color: AppTheme.textMuted
                            }
                            Icon {
                                visible: statusLine.discovery === "done"
                                         || statusLine.discovery === "failed"
                                name: statusLine.discovery === "done" ? "check_circle"
                                                                     : "error"
                                size: 14
                                color: statusLine.discovery === "done" ? AppTheme.success
                                                                      : AppTheme.danger
                            }
                            Label {
                                id: serverStatus
                                objectName: "serverStatusLabel"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                maximumLineCount: 3
                                elide: Text.ElideRight
                                font.family: AppTheme.uiFont
                                font.pixelSize: AppTheme.textMeta
                                color: statusLine.discovery === "failed" ? AppTheme.danger
                                       : statusLine.discovery === "done" ? AppTheme.success
                                       : AppTheme.textMuted
                                text: {
                                    switch (statusLine.discovery) {
                                    case "probing":
                                        return qsTr("Checking…")
                                    case "done":
                                        //: The server was found and answered.
                                        return qsTr("Found")
                                    case "failed":
                                        if (statusLine.problem === "not_an_address")
                                            return qsTr("That is not a server address. "
                                                        + "Enter one like matrix.org.")
                                        if (statusLine.problem === "unsupported")
                                            return qsTr("This server has no sign-in method "
                                                        + "Lightning supports.")
                                        return qsTr("Can't reach this server. Check the address.")
                                    default:
                                        return qsTr("Where your account is, like matrix.org")
                                    }
                                }
                                Accessible.role: statusLine.discovery === "failed"
                                                 ? Accessible.AlertMessage
                                                 : Accessible.StaticText
                                Accessible.name: text
                            }
                        }
                    }
                }

                AppButton {
                    objectName: "serverRetryButton"
                    visible: app.auth.discoveryState === "failed"
                             && app.auth.discoveryProblem === "unreachable"
                    kind: "secondary"
                    text: qsTr("Try again")
                    Layout.fillWidth: true
                    onClicked: app.auth.discoverAuthMethods(homeserverField.text)
                }

                AppButton {
                    id: browserLoginBtn
                    objectName: "browserLoginButton"
                    kind: "primary"
                    visible: root.signInMode === "oauth"
                             && !app.auth.browserLoginInProgress
                    text: root.browserAuthorityName.length > 0
                          && !discoverDebounce.running
                          ? qsTr("Continue with %1").arg(root.browserAuthorityName)
                          : qsTr("Continue")
                    enabled: !app.auth.isLoggingIn && !discoverDebounce.running
                    Layout.fillWidth: true
                    Accessible.name: text
                    onClicked: app.auth.beginBrowserLogin(app.auth.discoveredHomeserver)
                }
                Label {
                    objectName: "browserLoginHint"
                    visible: browserLoginBtn.visible
                    Layout.fillWidth: true
                    text: qsTr("Opens %1's sign-in page in your browser.")
                          .arg(root.browserAuthorityName)
                    textFormat: Text.PlainText
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                }
                AppButton {
                    objectName: "passwordInsteadButton"
                    kind: "secondary"
                    visible: browserLoginBtn.visible && app.auth.serverOffersPassword
                    text: qsTr("Sign in with password")
                    enabled: !app.auth.isLoggingIn
                    Layout.fillWidth: true
                    Accessible.name: text
                    onClicked: root.choosePassword(true)
                }

                ColumnLayout {
                    id: passwordForm
                    objectName: "passwordForm"
                    visible: root.signInMode === "password"
                             && !app.auth.browserLoginInProgress
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingXS

                    Label {
                        text: qsTr("Username")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                    AppTextField {
                        id: userField
                        objectName: "userField"
                        Layout.fillWidth: true
                        //: Placeholder for the sign-in name field. The field
                        //: takes a plain username - the server is the field
                        //: above it - so the placeholder says so rather than
                        //: showing a full @user:server id, which suggested the
                        //: server had to be typed twice.
                        placeholderText: qsTr("username")
                        Accessible.name: qsTr("Username")
                        onTextEdited: app.auth.clearLastError()
                    }

                    Label {
                        Layout.topMargin: AppTheme.spacingS
                        text: qsTr("Password")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                    AppTextField {
                        id: passField
                        objectName: "passField"
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Password")
                        echoMode: passReveal.checked ? TextInput.Normal
                                                     : TextInput.Password
                        leftPadding: passField.LayoutMirroring.enabled
                                     ? passReveal.width + AppTheme.spacingXS * 2
                                     : AppTheme.buttonPaddingH
                        rightPadding: passField.LayoutMirroring.enabled
                                      ? AppTheme.buttonPaddingH
                                      : passReveal.width + AppTheme.spacingXS * 2
                        onAccepted: root.submit()
                        onTextEdited: app.auth.clearLastError()
                        IconButton {
                            id: passReveal
                            objectName: "passwordRevealToggle"
                            anchors.right: parent.right
                            anchors.rightMargin: AppTheme.spacingXS
                            anchors.verticalCenter: parent.verticalCenter
                            checkable: true
                            size: "md"
                            iconName: passReveal.checked ? "visibility_off"
                                                         : "visibility"
                            iconSize: 17
                            Accessible.name: checked ? qsTr("Hide password")
                                                     : qsTr("Show password")
                            ToolTip.text: Accessible.name
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                        }
                    }

                    RowLayout {
                        objectName: "loginErrorRow"
                        Layout.fillWidth: true
                        spacing: AppTheme.spacingXS
                        visible: app.auth.lastError !== ""
                                 && (!repair.active || repair.info === null
                                     || !app.signInErrorExplainedByCard)
                        Icon {
                            objectName: "loginErrorIcon"
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 2
                            name: "error"
                            size: 14
                            color: AppTheme.error
                        }
                        Label {
                            objectName: "loginErrorLabel"
                            text: app.auth.lastError
                            textFormat: Text.PlainText
                            color: AppTheme.error
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            Accessible.role: Accessible.AlertMessage
                            Accessible.name: text
                        }
                    }

                    AppButton {
                        id: loginBtn
                        objectName: "loginSubmitButton"
                        kind: "primary"
                        text: {
                            if (!app.auth.isLoggingIn) return qsTr("Sign in")
                            switch (app.auth.loginStage) {
                            case "connecting":     return qsTr("Connecting…")
                            case "opening_store":  return qsTr("Opening secure store…")
                            case "authenticating": return qsTr("Signing in…")
                            case "starting_sync":  return qsTr("Starting sync…")
                            case "ready":          return qsTr("Signing in…")
                            case "waiting_for_browser":
                                return qsTr("Waiting for your browser…")
                            default:               return qsTr("Signing in…")
                            }
                        }
                        enabled: !app.auth.isLoggingIn
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacingS
                        onClicked: root.submit()
                    }
                    AppButton {
                        objectName: "passwordLoginCancelButton"
                        kind: "secondary"
                        visible: app.auth.isLoggingIn
                        text: qsTr("Cancel")
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Cancel sign-in")
                        onClicked: app.auth.cancelLogin()
                    }
                    AppButton {
                        objectName: "browserInsteadButton"
                        kind: "ghost"
                        size: "sm"
                        minWidth: 0
                        Layout.alignment: Qt.AlignHCenter
                        visible: root.passwordChosen && app.auth.serverOffersBrowserLogin
                                 && !app.auth.browserLoginUnavailable
                                 && !app.auth.isLoggingIn
                        text: qsTr("Sign in with your browser instead")
                        Accessible.name: text
                        onClicked: root.choosePassword(false)
                    }
                }

                RowLayout {
                    visible: root.signInMode === "password" && root.showsSso
                             && !app.auth.browserLoginInProgress
                    Layout.fillWidth: true
                    Layout.topMargin: AppTheme.spacingXS
                    spacing: AppTheme.spacingS
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: AppTheme.border
                    }
                    Label {
                        objectName: "loginAlternativesDivider"
                        //: Separates the password form from the
                        //: sign-in-with-your-browser buttons below it. The
                        //: longer form introduces a grid of provider names.
                        text: root.providerCount > 2 ? qsTr("Or continue with")
                                                     : qsTr("Or")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: AppTheme.border
                    }
                }

                AppButton {
                    id: ssoLoginBtn
                    objectName: "ssoLoginButton"
                    kind: root.signInMode === "sso" ? "primary" : "secondary"
                    visible: root.showsSso && root.providerCount === 0
                             && !app.auth.browserLoginInProgress
                    text: qsTr("Continue in browser")
                    enabled: !app.auth.isLoggingIn && !discoverDebounce.running
                    Layout.fillWidth: true
                    Accessible.name: text
                    onClicked: app.auth.beginSsoLogin(app.auth.discoveredHomeserver, "")
                }

                GridLayout {
                    objectName: "ssoProviderGrid"
                    visible: root.showsSso && root.providerCount > 0
                             && !app.auth.browserLoginInProgress
                    Layout.fillWidth: true
                    columns: root.providerCount > 2 ? 2 : 1
                    columnSpacing: AppTheme.spacingS
                    rowSpacing: AppTheme.spacingS

                    Repeater {
                        id: ssoProviderRepeater
                        objectName: "ssoProviderList"
                        model: root.showsSso && !app.auth.browserLoginInProgress
                               ? app.auth.ssoProviders : []
                        delegate: AppButton {
                            id: providerButton
                            required property var modelData
                            required property int index
                            objectName: "ssoProviderButton" + index
                            readonly property bool compact: root.providerCount > 2
                            kind: root.signInMode === "sso" && root.providerCount === 1
                                  ? "primary" : "secondary"
                            readonly property string providerName:
                                (modelData.name && modelData.name.length > 0)
                                ? modelData.name : ""
                            text: providerName.length === 0
                                  ? qsTr("Continue in browser")
                                  : compact ? providerName
                                  : qsTr("Continue with %1").arg(providerName)
                            enabled: !app.auth.isLoggingIn && !discoverDebounce.running
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            minWidth: 0
                            leftPadding: 34
                            rightPadding: compact ? AppTheme.spacingS : 34
                            Accessible.name: providerName.length > 0
                                             ? qsTr("Continue with %1").arg(providerName)
                                             : qsTr("Continue in browser")
                            ToolTip.text: Accessible.name
                            ToolTip.visible: compact && hovered
                            ToolTip.delay: 500
                            onClicked: app.auth.beginSsoLogin(app.auth.discoveredHomeserver,
                                                              modelData.id || "")

                            ProviderLogo {
                                anchors.left: parent.left
                                anchors.leftMargin: AppTheme.spacingS + 2
                                anchors.verticalCenter: parent.verticalCenter
                                brand: providerButton.modelData.brand || ""
                                ink: !providerButton.enabled ? AppTheme.buttonDisabledInk
                                     : providerButton.kind === "primary"
                                     ? AppTheme.buttonPrimaryInk
                                     : AppTheme.buttonNeutralInk
                            }
                        }
                    }
                }
                Label {
                    objectName: "ssoLoginHint"
                    visible: root.signInMode === "sso" && root.providerCount <= 1
                             && !app.auth.browserLoginInProgress
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    text: root.providerCount === 1
                          && (app.auth.ssoProviders[0].name || "").length > 0
                          ? qsTr("%1 signs you in with %2, in your browser.")
                                .arg(root.browserAuthorityName)
                                .arg(app.auth.ssoProviders[0].name)
                          : qsTr("Opens %1's sign-in page in your browser.")
                                .arg(root.browserAuthorityName)
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                }

                Label {
                    objectName: "loginErrorLabelNoForm"
                    visible: !passwordForm.visible
                             && app.auth.lastError !== ""
                             && (!repair.active || repair.info === null
                                 || !app.signInErrorExplainedByCard)
                    text: app.auth.lastError
                    textFormat: Text.PlainText
                    color: AppTheme.error
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: text
                }

                ColumnLayout {
                    visible: app.auth.browserLoginInProgress
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingS

                    RowLayout {
                        spacing: AppTheme.spacingS
                        AppBusyIndicator {
                            running: parent.visible
                            size: 16
                        }
                        Label {
                            text: qsTr("Waiting for your browser…")
                            color: AppTheme.textPrimary
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textBody
                            font.weight: AppTheme.weightStrong
                        }
                    }
                    Label {
                        objectName: "browserLoginWaitingLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        lineHeight: AppTheme.lineHeightBody
                        lineHeightMode: Text.ProportionalHeight
                        text: qsTr("Finish signing in with the page that opened "
                                   + "in your browser, then return to Lightning.")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                    }
                    AppButton {
                        objectName: "browserLoginCancelButton"
                        kind: "secondary"
                        text: qsTr("Cancel")
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Cancel browser sign-in")
                        onClicked: app.auth.cancelBrowserLogin()
                    }
                }

                RowLayout {
                    objectName: "accountLinks"
                    visible: root.signInMode === "oauth"
                             && !app.auth.browserLoginInProgress
                             && (app.auth.serverCanCreateAccount
                                 || app.auth.serverOffersAccountPage)
                    Layout.fillWidth: true
                    Layout.topMargin: AppTheme.spacingXS
                    spacing: AppTheme.spacingS

                    Item { Layout.fillWidth: true }
                    AppButton {
                        objectName: "createAccountButton"
                        visible: app.auth.serverCanCreateAccount
                        kind: "ghost"
                        size: "sm"
                        minWidth: 0
                        text: qsTr("Create account")
                        Accessible.description: qsTr("Opens %1's sign-up page in your browser.")
                                                .arg(root.browserAuthorityName)
                        enabled: !app.auth.isLoggingIn && !discoverDebounce.running
                        onClicked: app.auth.beginBrowserSignUp(app.auth.discoveredHomeserver)
                    }
                    AppButton {
                        objectName: "forgotPasswordButton"
                        visible: app.auth.serverOffersAccountPage
                        kind: "ghost"
                        size: "sm"
                        minWidth: 0
                        text: qsTr("Forgot password?")
                        Accessible.description: qsTr("Opens %1's account page in your browser.")
                                                .arg(root.browserAuthorityName)
                        enabled: !discoverDebounce.running
                        onClicked: app.auth.openAccountPage()
                    }
                    Item { Layout.fillWidth: true }
                }

                QtObject {
                    id: repair
                    readonly property string reasonCode: app.localSessionFailureReasonCode || ""
                    readonly property string userId: app.localSessionFailureUserId || ""
                    readonly property bool active: reasonCode !== ""
                    readonly property var info: repair.classify(reasonCode)

                    function classify(code) {
                        switch (code) {
                        case "session_without_device_id":
                        case "session_account_mismatch":
                        case "sdk_store_ownership_mismatch":
                        case "store_without_session_metadata": {
                            var headlineByCode = {
                                "session_without_device_id":
                                    qsTr("This local session doesn't match this account"),
                                "session_account_mismatch":
                                    qsTr("This local session doesn't match this account"),
                                "sdk_store_ownership_mismatch":
                                    qsTr("This local session doesn't match this account"),
                                "store_without_session_metadata":
                                    qsTr("This device has an incomplete local session")
                            }
                            var bodyByCode = {
                                "session_without_device_id":
                                    qsTr("Lightning's saved record for this account is "
                                         + "missing its device ID and can't be used to "
                                         + "sign back in. Rebuilding the local session is "
                                         + "safe — your messages stay on the server."),
                                "session_account_mismatch":
                                    qsTr("Lightning found a local session store that "
                                         + "doesn't belong to this account. Rebuilding it "
                                         + "is safe — your messages stay on the server."),
                                "sdk_store_ownership_mismatch":
                                    qsTr("Lightning found a local session store that "
                                         + "doesn't belong to this account. Rebuilding it "
                                         + "is safe — your messages stay on the server."),
                                "store_without_session_metadata":
                                    qsTr("Lightning found a local encryption store for this "
                                         + "account with no sign-in saved alongside it. "
                                         + "Rebuilding it is safe — your messages stay on "
                                         + "the server.")
                            }
                            return {
                                headline: headlineByCode[code],
                                body: bodyByCode[code],
                                primaryLabel: qsTr("Quarantine and rebuild"),
                                confirmTitle: qsTr("Rebuild the local session?"),
                                confirmBody: qsTr(
                                    "This moves Lightning's local session data for %1 on "
                                    + "this device aside — kept, not deleted — so a fresh "
                                    + "one can be built. Server messages, Element data, and "
                                    + "other accounts on this device are untouched. You'll "
                                    + "sign in again afterwards.")
                                    .arg(repair.userId),
                                showRemove: false
                            }
                        }
                        case "saved_session_without_store":
                            // The old device's Olm identity is gone and can't
                            // be resurrected; never imply otherwise.
                            return {
                                headline: qsTr("This device needs to sign in again"),
                                body: qsTr(
                                    "The local encryption store for this device is gone, "
                                    + "so this device can't resume its old session — "
                                    + "signing in creates a new device instead. Your "
                                    + "messages stay on the server; encrypted history may "
                                    + "need your recovery key or another verified device "
                                    + "afterwards."),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: true
                            }
                        case "access_token_revoked":
                            // suggestsLocalReset() is false here: the session
                            // died on the server and the local store is key
                            // material the user still needs. No destructive
                            // primary action. Signing in again moves the old
                            // store aside (RustSdkMatrixClient::
                            // moveRevokedDeviceStoreAside), never deletes it.
                            // "Remove this account" stays as an explicit,
                            // honestly worded fallback, never the suggested
                            // action.
                            return {
                                headline: qsTr("This session was signed out remotely"),
                                body: qsTr(
                                    "This device's Matrix session is no longer valid on "
                                    + "the server — for example, it may have been signed "
                                    + "out from another client. Your local data, including "
                                    + "this device's encryption keys, is intact. Signing in "
                                    + "again above starts a new session on this device; the "
                                    + "old session's data is kept aside, not deleted. "
                                    + "Encrypted history can come back through key backup "
                                    + "or another verified session, if you have one. If "
                                    + "signing in keeps failing, you can remove this "
                                    + "account below, which does delete this device's "
                                    + "local copy of your encryption keys."),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: true
                            }
                        case "access_token_expired":
                            return {
                                headline: qsTr("Your session expired"),
                                body: qsTr(
                                    "The server ended this device's session but kept "
                                    + "the device. Signing in again above continues it "
                                    + "as the same session on this device, with its "
                                    + "encryption keys and history. Nothing on this "
                                    + "device has been moved or deleted. If signing in "
                                    + "keeps failing, you can remove this account below, "
                                    + "which does delete this device's local copy of your "
                                    + "encryption keys."),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: true
                            }
                        case "secret_backend_unavailable":
                            return {
                                headline: qsTr("Lightning can't read your saved sign-in"),
                                body: qsTr(
                                    "Your system keyring is locked or unavailable, so "
                                    + "Lightning can't read the saved sign-in for %1. "
                                    + "Unlock your keyring and try again. Nothing has "
                                    + "been deleted, and this device's encryption keys "
                                    + "are untouched.")
                                    .arg(repair.userId),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: false,
                                showRetryKeyring: true
                            }
                        case "keyring_other_install":
                            // The adoption check: the sign-in this install had
                            // copied from before install scoping is another
                            // copy's device. Using it would run this store on
                            // that device and sign that copy out on a
                            // sign-out here. This install's copy is removed;
                            // the other copy's sign-in and this device's
                            // local data are untouched. Not a rebuild prompt.
                            return {
                                headline: qsTr("This sign-in belongs to another copy of Lightning"),
                                body: qsTr(
                                    "Another copy of Lightning on this computer, "
                                    + "for example a Flatpak or a different package, "
                                    + "is using the saved sign-in for %1. Sharing it "
                                    + "would sign that copy out too, so this copy "
                                    + "will not use it. Sign in again above to give "
                                    + "this copy a sign-in of its own. The other "
                                    + "copy is not affected.")
                                    .arg(repair.userId),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: true
                            }
                        case "keyring_lost_session":
                            return {
                                headline: qsTr("Your keyring isn't returning this sign-in"),
                                body: qsTr(
                                    "Lightning saved the sign-in for %1 in your system "
                                    + "keyring, and the keyring isn't returning it. "
                                    + "It may be locked, it may have been reset, or a "
                                    + "Flatpak or snap may have started using a different "
                                    + "keyring. Nothing on this device has been deleted, "
                                    + "and its encryption keys are intact. Unlock your "
                                    + "keyring and try again, or sign in with your "
                                    + "password to continue as this same device.")
                                    .arg(repair.userId),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: false,
                                showRetryKeyring: true
                            }
                        case "ambiguous_store_candidates":
                            // suggestsLocalReset() is false: several candidate
                            // stores hold real key material and Lightning can't
                            // tell which is valid. No destructive action of any
                            // kind.
                            return {
                                headline: qsTr("More than one local session was found"),
                                body: qsTr(
                                    "Lightning found more than one local encryption store "
                                    + "that could belong to this account and will not guess "
                                    + "between them. Sign out of the accounts you no longer "
                                    + "use, or remove the unused one from Settings, then "
                                    + "sign in again. Nothing has been deleted."),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: false
                            }
                        case "existing_store_requires_restore":
                            return {
                                headline: qsTr("This account already has a session here"),
                                body: qsTr(
                                    "Lightning keeps a session for %1 on this device. "
                                    + "Open it. If it doesn't open, remove it from this "
                                    + "device and sign in again: that deletes this "
                                    + "device's copy of its encryption keys, and your "
                                    + "messages stay on the server.")
                                    .arg(repair.userId),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: true,
                                showOpen: true
                            }
                        case "removal_incomplete":
                            return {
                                headline: qsTr("Some of this account's files are still here"),
                                body: qsTr(
                                    "%1 was removed from this device, but Lightning "
                                    + "could not delete all of its files. Close anything "
                                    + "that may be using them, then try again. Still "
                                    + "here: %2")
                                    .arg(repair.userId)
                                    .arg((app.accountRemovalLeftovers || []).map(function(p) {
                                        const parts = String(p).split(/[\\/]/)
                                        return parts[parts.length - 1] || String(p)
                                    }).join(", ")),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: false,
                                showRetryRemoval: true
                            }
                        case "invalid_saved_account_identity":
                            return {
                                headline: qsTr("This account's saved details are corrupted"),
                                body: qsTr(
                                    "Lightning couldn't read this account's saved sign-in "
                                    + "details. Remove it and sign in again."),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: true
                            }
                        case "cleanup_incomplete":
                            return {
                                headline: qsTr("The repair didn't finish"),
                                body: qsTr(
                                    "Lightning couldn't completely clean up the local "
                                    + "files for this account. Check that Lightning has "
                                    + "permission to modify its data folder, then try "
                                    + "again."),
                                primaryLabel: qsTr("Retry"),
                                confirmTitle: qsTr("Try the repair again?"),
                                confirmBody: qsTr(
                                    "Lightning will try again to clean up local files for "
                                    + "%1. This doesn't touch server messages or other "
                                    + "accounts.").arg(repair.userId),
                                showRemove: false
                            }
                        default:
                            return null
                        }
                    }
                }

                QtObject {
                    id: repairPanel
                    property bool running: false
                    property bool statusOk: false
                    property string statusText: ""
                }
                QtObject {
                    id: confirmState
                    property string kind: ""
                    property string reasonCode: ""
                    property string userId: ""
                    property var info: null

                    function openFor(newKind) {
                        kind = newKind
                        reasonCode = repair.reasonCode
                        userId = repair.userId
                        info = repair.info
                        repairConfirmDialog.open()
                    }
                }
                Connections {
                    target: app
                    function onLocalRustStoreResetResult(ok, message) {
                        repairPanel.running = false
                        repairPanel.statusOk = ok
                        repairPanel.statusText = message
                    }
                    function onLocalSessionFailureChanged() {
                        if (repairConfirmDialog.visible
                                && (confirmState.reasonCode !== repair.reasonCode
                                    || confirmState.userId !== repair.userId)) {
                            repairConfirmDialog.close()
                        }
                    }
                }

                Rectangle {
                    id: repairCard
                    objectName: "loginRepairCard"
                    visible: repair.active && repair.info !== null
                    Layout.fillWidth: true
                    Layout.topMargin: AppTheme.spacingS
                    radius: AppTheme.radiusMd
                    color: AppTheme.surfaceAlt
                    border.color: AppTheme.danger
                    border.width: 1
                    implicitHeight: repairColumn.implicitHeight + AppTheme.spacingM * 2
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: repair.info ? repair.info.headline : ""
                    onVisibleChanged: visible ? Qt.callLater(root.revealRepairCard)
                                              : root.returnFromRepairCard()
                    onHeightChanged: if (visible) Qt.callLater(root.revealRepairCard)

                    ColumnLayout {
                        id: repairColumn
                        x: AppTheme.spacingM
                        y: AppTheme.spacingM
                        width: parent.width - AppTheme.spacingM * 2
                        spacing: AppTheme.spacingS

                        Label {
                            objectName: "loginRepairHeadline"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            text: repair.info ? repair.info.headline : ""
                            color: AppTheme.text
                            font.family: AppTheme.uiFont
                            font.weight: AppTheme.weightStrong
                            font.pixelSize: AppTheme.textBody
                        }
                        Label {
                            textFormat: Text.PlainText
                            objectName: "loginRepairBody"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            text: repair.info ? repair.info.body : ""
                            color: AppTheme.textMuted
                            font.family: AppTheme.uiFont
                            font.pixelSize: AppTheme.textMeta
                        }

                        Flow {
                            objectName: "loginRepairActions"
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacingXS
                            spacing: AppTheme.spacingS

                            AppButton {
                                objectName: "loginRepairRetryRemoval"
                                visible: repair.info && repair.info.showRetryRemoval === true
                                enabled: !repairPanel.running && !app.auth.isLoggingIn
                                kind: "primary"
                                text: qsTr("Try again")
                                Accessible.name: qsTr("Try removing %1 again").arg(repair.userId)
                                onClicked: app.retryAccountRemoval()
                            }
                            AppButton {
                                objectName: "loginRepairRetryKeyring"
                                visible: repair.info && repair.info.showRetryKeyring === true
                                enabled: !repairPanel.running && !app.auth.isLoggingIn
                                kind: "primary"
                                text: qsTr("Try again")
                                Accessible.name: text
                                onClicked: app.retryKeyring()
                            }
                            AppButton {
                                objectName: "loginRepairOpenAccount"
                                visible: repair.info && repair.info.showOpen === true
                                enabled: !repairPanel.running
                                kind: "primary"
                                text: qsTr("Open it")
                                Accessible.name: qsTr("Open %1").arg(repair.userId)
                                onClicked: {
                                    if (app.loggedIn && app.accounts
                                            && app.accounts.activeUserId === repair.userId)
                                        app.showMain()
                                    else
                                        app.switchToAccount(repair.userId)
                                }
                            }
                            AppButton {
                                id: repairPrimaryButton
                                objectName: "loginRepairPrimaryAction"
                                kind: "danger"
                                visible: repair.info && repair.info.primaryLabel !== ""
                                         && repair.reasonCode !== "cleanup_incomplete"
                                         && app.localResetHelpsFor(repair.reasonCode)
                                enabled: !repairPanel.running
                                text: repair.info ? repair.info.primaryLabel : ""
                                Accessible.name: text
                                onClicked: confirmState.openFor("repair")
                            }
                            AppButton {
                                id: repairRetryButton
                                objectName: "loginRepairRetry"
                                kind: "danger"
                                visible: repair.reasonCode === "cleanup_incomplete"
                                enabled: !repairPanel.running
                                text: repair.info ? repair.info.primaryLabel : ""
                                Accessible.name: text
                                onClicked: confirmState.openFor("repair")
                            }
                            AppButton {
                                id: repairRemoveButton
                                objectName: "loginRepairRemoveAccount"
                                visible: repair.info && repair.info.showRemove === true
                                enabled: !repairPanel.running
                                text: qsTr("Remove this account")
                                Accessible.name: text
                                onClicked: confirmState.openFor("remove")
                            }
                            AppButton {
                                id: repairCopyDiagnosticsButton
                                objectName: "loginRepairCopyDiagnostics"
                                visible: repair.active
                                         && typeof app.copySessionDiagnostics === "function"
                                text: qsTr("Copy sanitized diagnostics")
                                Accessible.name: text
                                onClicked: app.copySessionDiagnostics()
                            }
                        }

                        Label {
                            objectName: "loginRepairResult"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            visible: repairPanel.statusText !== ""
                            text: repairPanel.statusText
                            textFormat: Text.PlainText
                            color: repairPanel.statusOk ? AppTheme.success : AppTheme.danger
                            font.pixelSize: AppTheme.textMeta
                        }
                    }
                }

                Dialog {
                    id: repairConfirmDialog
                    objectName: "loginRepairConfirmDialog"
                    parent: Overlay.overlay
                    anchors.centerIn: parent
                    width: Math.max(240, Math.min(420, parent ? parent.width - 32 : 420))
                    modal: true
                    title: confirmState.kind === "remove"
                           ? qsTr("Remove account?")
                           : (confirmState.info ? confirmState.info.confirmTitle : "")
                    standardButtons: Dialog.NoButton
                    closePolicy: Popup.CloseOnEscape
                    onOpened: repairConfirmCancelButton.forceActiveFocus()

                    header: Label {
                        text: repairConfirmDialog.title
                        visible: text.length > 0
                        color: AppTheme.textPrimary
                        font.family: AppTheme.menuFont
                        font.pixelSize: AppTheme.textTitle
                        font.weight: AppTheme.weightBold
                        wrapMode: Text.WordWrap
                        leftPadding: AppTheme.spacing16
                        rightPadding: AppTheme.spacing16
                        topPadding: AppTheme.spacing16
                        bottomPadding: AppTheme.spacing8
                    }

                    background: Rectangle {
                        color: AppTheme.surface
                        border.color: AppTheme.border
                        radius: AppTheme.radiusLg
                    }

                    contentItem: ColumnLayout {
                        spacing: AppTheme.spacing12
                        Label {
                            textFormat: Text.PlainText
                            objectName: "loginRepairConfirmBody"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            lineHeight: AppTheme.lineHeightBody
                            lineHeightMode: Text.ProportionalHeight
                            text: confirmState.kind === "remove"
                                ? qsTr("Remove %1 from this device? Its local Lightning "
                                       + "data, encryption store, and sign-in are deleted "
                                       + "from this computer only. Messages stay on the "
                                       + "server, and other accounts are not affected.")
                                  .arg(confirmState.userId)
                                : (confirmState.info ? confirmState.info.confirmBody : "")
                            color: AppTheme.textPrimary
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Item { Layout.fillWidth: true }
                            AppButton {
                                id: repairConfirmCancelButton
                                objectName: "loginRepairCancel"
                                text: qsTr("Cancel")
                                focus: true
                                onClicked: repairConfirmDialog.close()
                            }
                            AppButton {
                                objectName: "loginRepairConfirmAction"
                                kind: "dangerPrimary"
                                text: confirmState.kind === "remove"
                                      ? qsTr("Remove")
                                      : (confirmState.info ? confirmState.info.primaryLabel : "")
                                Accessible.name: qsTr("Confirm: %1").arg(text)
                                onClicked: {
                                    var kind = confirmState.kind
                                    var target = confirmState.userId
                                    var capturedReason = confirmState.reasonCode
                                    var capturedUserId = confirmState.userId
                                    repairConfirmDialog.close()
                                    if (capturedReason !== repair.reasonCode
                                            || capturedUserId !== repair.userId) {
                                        return
                                    }
                                    if (kind === "remove") {
                                        app.removeAccount(target)
                                    } else {
                                        repairPanel.running = true
                                        repairPanel.statusOk = false
                                        repairPanel.statusText = qsTr("Repairing…")
                                        app.repairLocalSession()
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    MiddleClickScroller {
        objectName: "loginMiddleClickScroller"
        anchors.fill: parent
        z: 1
        view: loginFlick
    }
}
