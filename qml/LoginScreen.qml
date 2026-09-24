import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    // The startup-state suite checks this is never instantiated during a
    // valid-session launch.
    objectName: "loginScreen"

    function submit() {
        if (!app.auth.isLoggingIn)
            app.auth.login(homeserverField.text, userField.text,
                           passField.text)
    }

    // The password is wiped when leaving this screen, but survives a failed
    // attempt so a typo can be corrected.
    onVisibleChanged: if (!visible) passField.text = ""

    // The homeserver's bare host, for the browser button's label ("Continue
    // with matrix.org"), which is what distinguishes it from the SSO button.
    // Derived from what the user typed, never from server data, so no server
    // chooses the words on our button. Scheme, port and path are stripped; an
    // IPv6 literal keeps its colons.
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
    // True when a browser path is on offer, so the "Or" divider appears only
    // when there is a choice.
    readonly property bool offersBrowserPath:
        (app.auth.serverOffersBrowserLogin || app.auth.serverOffersSso)
        && !app.auth.browserLoginInProgress

    // Prefill from the identity that actually failed (resolved server-
    // canonically in C++), never the raw typed text, so the repair card targets
    // the right account, including during add-account. See qml/AccountMenu.qml
    // for the switcher-side counterpart.
    function applyFailureIdentityToFields() {
        if (app.localSessionFailureReasonCode === "")
            return
        if (app.localSessionFailureUserId !== "")
            userField.text = app.localSessionFailureUserId
        if (app.localSessionFailureHomeserver !== "")
            homeserverField.text = app.localSessionFailureHomeserver
    }
    Component.onCompleted: applyFailureIdentityToFields()
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

    // A Flickable so an overflowing form scrolls instead of clipping; the panel
    // width tracks the window between a min and max.
    Flickable {
        id: loginFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: panel.implicitHeight + AppTheme.spacingXL * 2
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: AppScrollBar { policy: ScrollBar.AsNeeded }
        // Same wheel/touchpad feel as the room timeline; see
        // qml/SmoothWheelArea.qml.
        SmoothWheelArea {}

        Rectangle {
            id: panel
            objectName: "loginPanel"
            anchors.horizontalCenter: parent.horizontalCenter

            // Centred on the tallest height the form has reached, not its
            // current height. The homeserver probe is async and can remove the
            // browser/SSO sections, which would slide the card (and its fields)
            // under the user's cursor mid-typing, e.g. putting a password into
            // the clear-text homeserver field. `_tallest` only grows, and
            // resets on a viewport resize.
            property real _tallest: implicitHeight
            onImplicitHeightChanged: {
                if (implicitHeight > _tallest)
                    _tallest = implicitHeight
            }
            Connections {
                target: loginFlick
                function onHeightChanged() { panel._tallest = panel.implicitHeight }
            }
            y: Math.max(AppTheme.spacingXL,
                        (loginFlick.height - _tallest) / 2)
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

                // Add-account flow: a way back that doesn't touch the existing
                // session. Bound to the persisted active account, not
                // app.loggedIn: a failed add-account releases the shared
                // client's session and the button must survive that; showMain()
                // self-heals.
                AppButton {
                    id: backToAppButton
                    objectName: "backToAppButton"
                    visible: app.accounts && app.accounts.hasActiveAccount
                    Accessible.name: qsTr("Back to the app")
                    text: qsTr("← Back")
                    onClicked: app.showMain()
                }

                // ── The way back to accounts already on this device ── For
                // when the stored active account can't be restored, which would
                // otherwise leave the other saved accounts unreachable. Gated
                // on the saved list, not the active account.
                ColumnLayout {
                    objectName: "signedInAccountsRecovery"
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingS
                    visible: app.accounts
                             && app.accounts.accounts.length > 0
                             && !(app.accounts.hasActiveAccount && app.loggedIn)

                    Label {
                        text: qsTr("Already on this device")
                        color: AppTheme.textMuted
                        font.family: AppTheme.uiFont
                        font.pixelSize: AppTheme.textMeta
                        font.weight: AppTheme.weightStrong
                    }
                    Repeater {
                        model: app.accounts ? app.accounts.accounts : []
                        delegate: AppButton {
                            required property var modelData
                            objectName: "recoverAccountButton"
                            Layout.fillWidth: true
                            text: modelData.displayName
                                  && modelData.displayName.length > 0
                                  ? modelData.displayName + "  ·  " + modelData.userId
                                  : modelData.userId
                            Accessible.name: qsTr("Open %1").arg(modelData.userId)
                            onClicked: app.switchToAccount(modelData.userId)
                        }
                    }
                }

                // Brand mark.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingS
                    Rectangle {
                        width: 10; height: 10; radius: 3
                        color: AppTheme.accent
                    }
                    Label {
                        text: "Lightning"
                        color: AppTheme.text
                        // The wordmark uses the brand face.
                        font.family: AppTheme.displayFont
                        font.pixelSize: AppTheme.textTitle
                        font.weight: AppTheme.weightDisplay
                        font.letterSpacing: 0.3
                    }
                }

                Label {
                    text: (app.accounts && app.accounts.hasActiveAccount)
                          ? qsTr("Add another account")
                          : qsTr("Sign in")
                    color: AppTheme.text
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textDisplay
                    font.weight: AppTheme.weightDisplay
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    // Display leading: body leading on a 22 px heading reads as
                    // two.
                    lineHeight: AppTheme.lineHeightDisplay
                    lineHeightMode: Text.ProportionalHeight
                }
                Label {
                    text: {
                        // Backend-aware sub-heading.
                        if (app.backendName === "mock")
                            return qsTr("Mock backend — any credentials work")
                        if (app.backendName === "rust")
                            return qsTr("Native Matrix backend with end-to-end encryption")
                        return qsTr("Sign in with your Matrix account")
                    }
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                    Layout.fillWidth: true
                    Layout.bottomMargin: AppTheme.spacingXS
                }

                Label {
                    text: qsTr("Homeserver URL")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }
                AppTextField {
                    id: homeserverField
                    objectName: "homeserverField"
                    Layout.fillWidth: true
                    // Prefilled once from the account-independent login
                    // prefill, not bound to a settings getter (which returns
                    // the active account's server during add-account and would
                    // revert typed input).
                    //
                    // Discovery runs without waiting for Enter: once for the
                    // prefill on open, then debounced while typing. AuthManager
                    // tags results by server, so a stale probe can't label a
                    // newer one.
                    Component.onCompleted: {
                        text = app.settings.loginHomeserverPrefill
                        if (text.length > 0)
                            app.auth.discoverAuthMethods(text)
                    }
                    placeholderText: "https://matrix.org"
                    Accessible.name: qsTr("Homeserver URL")
                    onTextEdited: discoverDebounce.restart()
                    Timer {
                        id: discoverDebounce
                        interval: 700
                        onTriggered:
                            app.auth.discoverAuthMethods(homeserverField.text)
                    }
                    onEditingFinished: {
                        discoverDebounce.stop()
                        app.settings.loginHomeserverPrefill = text
                        // Ask the server what it offers; no auth choices are
                        // shown until it answers.
                        app.auth.discoverAuthMethods(text)
                    }
                    KeyNavigation.tab: userField
                }

                Label {
                    text: qsTr("User")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }
                AppTextField {
                    id: userField
                    objectName: "userField"
                    Layout.fillWidth: true
                    //: Placeholder for the sign-in name field. The field
                    //: takes a plain username - the homeserver is the field
                    //: above it - so the placeholder says so rather than
                    //: showing a full @user:server id, which suggested the
                    //: server had to be typed twice.
                    placeholderText: qsTr("username")
                    Accessible.name: qsTr("User")
                    KeyNavigation.tab: passField
                }

                Label {
                    text: qsTr("Password")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                }
                // Password field + reveal toggle share one row.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: AppTheme.spacingXS
                    AppTextField {
                        id: passField
                        objectName: "passField"
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Password")
                        echoMode: passReveal.checked ? TextInput.Normal
                                                     : TextInput.Password
                        // Enter submits from the password field.
                        onAccepted: root.submit()
                    }
                    IconButton {
                        id: passReveal
                        objectName: "passwordRevealToggle"
                        checkable: true
                        implicitWidth: 34; implicitHeight: 34
                        radius: AppTheme.radiusMd
                        iconName: passReveal.checked ? "visibility_off"
                                                     : "visibility"
                        iconSize: 18
                        Accessible.name: checked ? qsTr("Hide password")
                                                 : qsTr("Show password")
                        ToolTip.text: Accessible.name
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                    }
                }

                Label {
                    objectName: "loginErrorLabel"
                    // A classified reason without a dedicated card (info ===
                    // null) still needs this fallback; only a rendered card
                    // suppresses it.
                    visible: app.auth.lastError !== ""
                             && (!repair.active || repair.info === null)
                    text: app.auth.lastError
                    color: AppTheme.error
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                }

                AppButton {
                    id: loginBtn
                    objectName: "loginSubmitButton"
                    kind: "primary"
                    // Staged progress (AuthManager.loginStage), falling back to
                    // a fixed label for unknown stages.
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
                    Layout.topMargin: AppTheme.spacingXS
                    onClicked: root.submit()
                }

                // ── "Or" ── The password form and the browser buttons are
                // alternatives, not a sequence.
                RowLayout {
                    visible: root.offersBrowserPath
                    Layout.fillWidth: true
                    Layout.topMargin: AppTheme.spacingS
                    spacing: AppTheme.spacingS
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: AppTheme.border
                    }
                    Label {
                        objectName: "loginAlternativesDivider"
                        //: Separates the password form from the
                        //: sign-in-with-your-browser buttons below it.
                        text: qsTr("Or")
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

                // ── Browser sign-in (OAuth 2.0 / OIDC) ── Shown only when the
                // homeserver's discovery offers OAuth and this build supports
                // it. Nothing is hard-coded per provider.
                AppButton {
                    id: browserLoginBtn
                    objectName: "browserLoginButton"
                    // Primary when the server accepts no password (the only way
                    // in); secondary beside a usable password form.
                    kind: app.auth.serverOffersPassword ? "secondary"
                                                        : "primary"
                    visible: app.auth.serverOffersBrowserLogin
                             && !app.auth.browserLoginInProgress
                    // Named after the homeserver to distinguish it from SSO.
                    // Falls back to a bare "Continue" when there is no host.
                    text: root.browserAuthorityName.length > 0
                          ? qsTr("Continue with %1").arg(root.browserAuthorityName)
                          : qsTr("Continue")
                    // Needs no typed user or password: the homeserver
                    // identifies the account.
                    enabled: !app.auth.isLoggingIn
                    Layout.fillWidth: true
                    Layout.topMargin: AppTheme.spacingXS
                    Accessible.name: text
                    onClicked: app.auth.beginBrowserLogin(homeserverField.text)
                }
                Label {
                    objectName: "browserLoginHint"
                    visible: browserLoginBtn.visible
                    Layout.fillWidth: true
                    text: qsTr("Signs you in on your homeserver's own page, "
                               + "in your browser. No password needed here.")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                }

                // The waiting state always offers a way out. Cancel resolves it
                // at once, and the backend also times the attempt out.
                ColumnLayout {
                    visible: app.auth.browserLoginInProgress
                    Layout.fillWidth: true
                    Layout.topMargin: AppTheme.spacingXS
                    spacing: AppTheme.spacingXS

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

                // ── Single sign-on (legacy m.login.sso) ── See
                // rust/src/sso.rs. Driven by what the server advertises, never
                // hard-coded per vendor: no providers gives one generic button
                // (also the state while the list is loading); otherwise one
                // "Continue with <name>" per provider. Wording follows Element
                // and avoids protocol terms and the "SSO" abbreviation.
                AppButton {
                    id: ssoLoginBtn
                    objectName: "ssoLoginButton"
                    // Primary only when nothing else can sign you in.
                    kind: (app.auth.serverOffersPassword
                           || app.auth.serverOffersBrowserLogin)
                          ? "secondary" : "primary"
                    visible: app.auth.serverOffersSso
                             && app.auth.ssoProviders.length === 0
                             && !app.auth.browserLoginInProgress
                    // Element's wording for an unnamed provider.
                    text: qsTr("Sign in with single sign-on")
                    enabled: !app.auth.isLoggingIn
                    Layout.fillWidth: true
                    Layout.topMargin: AppTheme.spacingXS
                    Accessible.name: text
                    onClicked: app.auth.beginSsoLogin(homeserverField.text, "")
                }
                Label {
                    objectName: "ssoLoginHint"
                    visible: ssoLoginBtn.visible
                             || ssoProviderRepeater.count > 0
                    Layout.fillWidth: true
                    // Explains the difference: this goes to whoever the
                    // homeserver trusts to identify you.
                    text: qsTr("Signs you in through an identity provider "
                               + "your homeserver trusts, in your browser.")
                    color: AppTheme.textMuted
                    font.family: AppTheme.uiFont
                    font.pixelSize: AppTheme.textMeta
                    wrapMode: Text.WordWrap
                    lineHeight: AppTheme.lineHeightBody
                    lineHeightMode: Text.ProportionalHeight
                }

                Repeater {
                    id: ssoProviderRepeater
                    objectName: "ssoProviderList"
                    model: app.auth.serverOffersSso
                           && !app.auth.browserLoginInProgress
                           ? app.auth.ssoProviders : []
                    delegate: AppButton {
                        required property var modelData
                        required property int index
                        objectName: "ssoProviderButton" + index
                        kind: "secondary"
                        // Server-chosen name: plain text, never markup. An
                        // unnamed provider falls back to the generic label.
                        // Same "Continue with" shape as the browser button:
                        // both name the authority.
                        text: (modelData.name && modelData.name.length > 0)
                              ? qsTr("Continue with %1").arg(modelData.name)
                              : qsTr("Sign in with single sign-on")
                        enabled: !app.auth.isLoggingIn
                        Layout.fillWidth: true
                        Layout.topMargin: AppTheme.spacingXS
                        Accessible.name: text
                        onClicked: app.auth.beginSsoLogin(homeserverField.text,
                                                          modelData.id || "")
                    }
                }

                // ── Local-session repair card ── Driven by AppController's
                // classified failure (reason code plus the failed account's own
                // identity, never typed form text), so the action targets the
                // right account even during add-account. classify() covers
                // every reasonCode reachable from
                // matrix::rust_session::StoreBlockReason
                // (src/matrix/RustSessionPolicy.cpp) except
                // "existing_store_requires_restore", which AppController
                // intercepts. An unrecognised code falls back to
                // loginErrorLabel so the form is never blank.
                QtObject {
                    id: repair
                    readonly property string reasonCode: app.localSessionFailureReasonCode || ""
                    readonly property string userId: app.localSessionFailureUserId || ""
                    readonly property bool active: reasonCode !== ""
                    readonly property var info: repair.classify(reasonCode)

                    // Which codes get a destructive action mirrors
                    // matrix::rust_session::suggestsLocalReset() in
                    // src/matrix/RustSessionPolicy.cpp; keep them in sync by
                    // hand. Codes it rejects (including access_token_revoked
                    // and ambiguous_store_candidates) must never route to
                    // app.repairLocalSession(): the store holds key material
                    // the user still needs, or Lightning can't tell which store
                    // is real.
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
                                // "Quarantine" is literal: the data is moved
                                // aside on this device, not deleted.
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
                                // No primary action: there is no store to
                                // quarantine, so the backend refuses a reset.
                                // The remedy is the prefilled sign-in form
                                // above.
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: true
                            }
                        case "access_token_revoked":
                            // suggestsLocalReset() is false here: the session
                            // died on the server and the local store is key
                            // material the user still needs. No destructive
                            // primary action. "Remove this account" stays as an
                            // explicit, honestly worded fallback, never the
                            // suggested action.
                            return {
                                headline: qsTr("This session was signed out remotely"),
                                body: qsTr(
                                    "This device's Matrix session is no longer valid on "
                                    + "the server — for example, it may have been signed "
                                    + "out from another client. Your local data, including "
                                    + "this device's encryption keys, is intact. Try "
                                    + "signing in again above with your password. If that "
                                    + "keeps failing, you can remove this account below, "
                                    + "which does delete this device's local copy of your "
                                    + "encryption keys."),
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: true
                            }
                        case "secret_backend_unavailable":
                            // The keyring is locked or the session bus is gone.
                            // The sign-in is saved and the store intact;
                            // informational only, the remedy is outside
                            // Lightning.
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
                                showRemove: false
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
                // Snapshot of what the confirm dialog acts on, taken when it
                // opens, so a failure changing underneath (a slow sign-in,
                // another account failing mid add-account) can't change the
                // dialog's target. See the auto-close below and the
                // refuse-if-changed check in onClicked.
                QtObject {
                    id: confirmState
                    property string kind: "" // "repair" | "remove"
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
                    // Close the dialog if the classified failure changes while
                    // it is open. onClicked re-checks as a backstop.
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
                            // Remote or externally chosen text: never markup.
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

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: AppTheme.spacingXS
                            spacing: AppTheme.spacingS

                            AppButton {
                                id: repairPrimaryButton
                                objectName: "loginRepairPrimaryAction"
                                kind: "danger"
                                // Bound to the backend policy, not a per-reason
                                // list here, so a card never offers an action
                                // repairLocalSession() would refuse.
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

                // One shared confirmation for repair and remove. Cancel is the
                // default button and the body names the exact account.
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
                    // Cancel must actually hold focus when the dialog opens:
                    // `focus: true` alone doesn't reliably grant activeFocus to
                    // a button in a custom-buttoned Dialog, so Enter/Space
                    // would hit something else.
                    onOpened: repairConfirmCancelButton.forceActiveFocus()

                    // Replace Basic's square header strip with the app's title
                    // style.
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
                            // Remote or externally chosen text: never markup.
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
                            // AppButton for both, for one geometry and proper
                            // hover/focus states.
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
                                    // Backstop for the auto-close above: refuse
                                    // if the failure's reason or account
                                    // changed since the dialog opened.
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
    } // Flickable
}
