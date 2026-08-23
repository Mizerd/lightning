import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

Item {
    id: root
    // Located by the startup-state suite: the login form must never be
    // instantiated during a valid-session launch.
    objectName: "loginScreen"

    function submit() {
        if (!app.auth.isLoggingIn)
            app.auth.login(homeserverField.text, userField.text,
                           passField.text)
    }

    // The password never outlives this screen: leaving it (successful
    // login, or navigating away) wipes the field. It deliberately survives
    // a FAILED attempt so a typo can be corrected — the failure keeps the
    // user on this screen, with the secret still under their control.
    onVisibleChanged: if (!visible) passField.text = ""

    // The homeserver's bare host, for the browser button's label.
    //
    // "Continue with matrix.org" names the authority you are about to hand
    // the sign-in to; a bare "Continue in browser" did not, which is why it
    // was indistinguishable from the single-sign-on button right below it.
    // Both are browser trips — what differs is WHO authenticates you, so
    // that is what the labels now say (Element classic words its
    // per-provider buttons the same way: "Continue with <provider>").
    //
    // Derived from what the USER typed, never from anything the server sent
    // back: no homeserver gets to choose the words on Lightning's own
    // button. Scheme, port and path are stripped; an IPv6 literal keeps its
    // colons.
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
    // True when either browser path is on offer, so the divider that
    // separates "type a password" from "sign in somewhere else" appears
    // exactly when there is actually a choice to make.
    readonly property bool offersBrowserPath:
        (app.auth.serverOffersBrowserLogin || app.auth.serverOffersSso)
        && !app.auth.browserLoginInProgress

    // The identity that actually failed (from AppController, resolved
    // server-canonically by the C++ layer) — never the raw typed text.
    // Prefilling from it means the repair card always targets and displays
    // the correct account, including during add-account, where the typed
    // fields belong to a DIFFERENT account than whatever is currently
    // active. See qml/AccountMenu.qml for the switcher-side counterpart.
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

    // v0.5.11: the login panel is a Flickable so an overflowing form (long
    // errors, high-DPI scaling, short windows) scrolls instead of clipping,
    // and the panel width tracks the window between a sensible min and max.
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
            anchors.horizontalCenter: parent.horizontalCenter
            y: Math.max(AppTheme.spacingXL,
                        (loginFlick.height - implicitHeight) / 2)
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

                // v0.7 add-account flow: reached from the account switcher
                // while another account stays signed in — offer a way back
                // that does not touch the existing session. Bound to the
                // persisted active account (NOT app.loggedIn): a failed
                // add-account attempt releases the shared client's session,
                // and the Back button must survive that so the user can
                // return; showMain() self-heals.
                AppButton {
                    id: backToAppButton
                    objectName: "backToAppButton"
                    visible: app.accounts && app.accounts.hasActiveAccount
                    Accessible.name: qsTr("Back to the app")
                    text: qsTr("← Back")
                    onClicked: app.showMain()
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
                        // The wordmark is the one place the brand face is
                        // unambiguously right — displayFont keeps Space
                        // Grotesk here now that menuFont has moved to the
                        // user's chosen UI face everywhere else.
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
                    // Display leading, not body leading: 1.5 on a 22px
                    // heading opens a gap the eye reads as two headings.
                    lineHeight: AppTheme.lineHeightDisplay
                    lineHeightMode: Text.ProportionalHeight
                }
                Label {
                    text: {
                        // Backend-aware sub-heading so the user knows what
                        // backend they're signing into.
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
                    // Prefill ONCE from the account-independent login prefill,
                    // then let the user edit freely — no live binding to a
                    // settings getter. Binding text to homeserverUrl (which
                    // returns the ACTIVE account's server during the
                    // add-account flow) re-asserted itself and reverted typed
                    // input back to "your own" homeserver, making it
                    // impossible to point the field at a different one.
                    //
                    // Discovery runs WITHOUT waiting for Enter (live report
                    // 2026-08-15: the sign-in options were "hidden" until
                    // the server was typed and Enter pressed): once for the
                    // prefilled server on open, then debounced while
                    // typing. AuthManager resets per call and tags results
                    // by server, so a stale probe can never label a newer
                    // one; the UI still never guesses on a homeserver's
                    // behalf — it just asks sooner.
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
                        // Ask the server what it actually offers. Until it
                        // answers, no auth-method choices are shown at all —
                        // the UI never guesses on a homeserver's behalf.
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
                    //: An EXAMPLE Matrix ID shown as placeholder text.
                    //: Translate the local part to a name that reads as
                    //: an example in your language, but keep the
                    //: @user:server shape - it is the protocol's syntax,
                    //: not prose.
                    placeholderText: qsTr("@alice:matrix.org")
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
                    // A classified reason with no dedicated card (info ===
                    // null) must still show SOMETHING — repair.active alone
                    // hid this for every classified code, including ones
                    // with no case in classify() below, leaving a blank
                    // form. Only a code that both is classified AND has a
                    // rendered card suppresses this fallback.
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
                    // Staged progress (AuthManager.loginStage): a flat
                    // "Signing in…" hides real progress on a slow SDK
                    // handle/store open, especially right after a repair.
                    // Falls back to a fixed label if the stage token isn't
                    // recognized, so this stays safe even before the
                    // property lands.
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

                // ── "Or" ────────────────────────────────────────────────
                // The password form above and the browser buttons below are
                // alternatives, not a sequence. Without a divider they read
                // as a stack of four things to try, which is how "I wouldn't
                // know which one I want" happens. Element classic separates
                // the same two groups with the same word.
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

                // ── Browser sign-in (OAuth 2.0 / OIDC) ──────────────────
                // Shown ONLY when the homeserver's own discovery says it
                // offers OAuth and this build can perform it. Nothing here
                // is hard-coded for any particular provider.
                AppButton {
                    id: browserLoginBtn
                    objectName: "browserLoginButton"
                    // Primary when the server accepts no password: this is
                    // then the only way in, and Element classic makes the
                    // same call. Secondary alongside a usable password form,
                    // so that form keeps its own primary.
                    kind: app.auth.serverOffersPassword ? "secondary"
                                                        : "primary"
                    visible: app.auth.serverOffersBrowserLogin
                             && !app.auth.browserLoginInProgress
                    // Named after the homeserver, because that is what
                    // distinguishes it from the single-sign-on buttons below
                    // — both open a browser; only the authority differs. The
                    // fallback is Element's own bare "Continue", used when
                    // the field is empty and there is no host to name.
                    text: root.browserAuthorityName.length > 0
                          ? qsTr("Continue with %1").arg(root.browserAuthorityName)
                          : qsTr("Continue")
                    // Browser sign-in needs no typed user or password — the
                    // homeserver identifies the account, which is why the
                    // store cannot be chosen until it answers.
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

                // The waiting state ALWAYS offers a way out. A browser that
                // is closed, denied, or simply ignored must never leave this
                // screen stuck: Cancel resolves it immediately, and the
                // backend also times the attempt out on its own.
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

                // ── Single sign-on (legacy m.login.sso) ─────────────────
                // A REAL action since 0.7.6+: this was a "not supported"
                // notice, because the SDK's login_sso convenience helper needs
                // a feature this build cannot vendor. The primitives under it
                // are not gated, so the flow works — see the module docs in
                // rust/src/sso.rs.
                //
                // Two shapes, decided by what the SERVER advertises and never
                // hard-coded per vendor:
                //
                //   no providers  one generic "Sign in with single sign-on"
                //                 button. This is the common case, and it is
                //                 also the correct state while the provider
                //                 list is still being fetched;
                //   providers     one "Continue with <name>" button each,
                //                 named by the server, rather than silently
                //                 picking an arbitrary one.
                //
                // Both strings are Element classic's, verbatim. The wording
                // avoids Matrix protocol terms — never "m.login.sso" — and
                // avoids the "SSO" abbreviation, which assumed the reader
                // already knew which of two browser buttons they wanted.
                AppButton {
                    id: ssoLoginBtn
                    objectName: "ssoLoginButton"
                    // Primary only when nothing else can sign you in: with a
                    // password form or a browser button present, this is the
                    // fallback of the three. Element classic makes the same
                    // distinction.
                    kind: (app.auth.serverOffersPassword
                           || app.auth.serverOffersBrowserLogin)
                          ? "secondary" : "primary"
                    visible: app.auth.serverOffersSso
                             && app.auth.ssoProviders.length === 0
                             && !app.auth.browserLoginInProgress
                    // Element classic's exact wording for an unnamed
                    // provider. "Sign in with SSO" was an abbreviation the
                    // user had to already know; spelled out, it at least says
                    // it is a sign-on run by someone else.
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
                    // Says what the difference IS, rather than naming the
                    // protocol: the browser button above goes to the
                    // homeserver, this one goes to whoever the homeserver
                    // trusts to identify you.
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
                        // The server chose this name. It is remote text, so it
                        // is rendered as a plain string and never as markup;
                        // an unnamed provider falls back to the generic label
                        // rather than showing an empty button.
                        //
                        // "Continue with <provider>" is Element classic's own
                        // string for this button, and it is the same shape as
                        // the browser button above on purpose: both name the
                        // authority, which is the only thing that differs
                        // between them.
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

                // ── Local-session repair card ───────────────────────────
                // Replaces the old single-message reset dead-end. Driven
                // entirely by AppController's classified failure (reason
                // code + the FAILED account's own identity — never typed
                // form text), so the repair action always targets the
                // right account even mid add-account flow. classify()
                // below covers every reasonCode currently reachable from
                // matrix::rust_session::StoreBlockReason (see
                // src/matrix/RustSessionPolicy.cpp), except
                // "existing_store_requires_restore", which AppController
                // intercepts before it ever becomes a classified failure
                // (it is "already signed in — switch instead", not a
                // repair). If a reasonCode ever arrives that classify()
                // doesn't recognise — a real gap, not something to shrug
                // off — loginErrorLabel is the fallback so the form is
                // never silently blank; see its visibility binding below.
                QtObject {
                    id: repair
                    readonly property string reasonCode: app.localSessionFailureReasonCode || ""
                    readonly property string userId: app.localSessionFailureUserId || ""
                    readonly property bool active: reasonCode !== ""
                    readonly property var info: repair.classify(reasonCode)

                    // Which reasonCode gets a destructive primary action
                    // mirrors matrix::rust_session::suggestsLocalReset() in
                    // src/matrix/RustSessionPolicy.cpp — keep these two in
                    // sync by hand; there is no live-bound property for it
                    // yet. Reasons it returns true (a store with something
                    // real to clear) get "Quarantine and rebuild"/"Retry".
                    // Reasons it returns false — access_token_revoked and
                    // ambiguous_store_candidates included — must NEVER
                    // route to app.repairLocalSession(): for a revoked
                    // token the store is exactly the key material the user
                    // still needs, and for an ambiguous store Lightning
                    // does not know which one is real. Never re-arm here
                    // what the policy layer disarmed.
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
                                // "Quarantine" is literal, not a euphemism
                                // for delete: the old local session data is
                                // moved aside on this device, not removed,
                                // in case it's ever needed for recovery.
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
                            // Honest outcome only: the old device's Olm identity is
                            // gone and cannot be resurrected — never imply otherwise.
                            return {
                                headline: qsTr("This device needs to sign in again"),
                                body: qsTr(
                                    "The local encryption store for this device is gone, "
                                    + "so this device can't resume its old session — "
                                    + "signing in creates a new device instead. Your "
                                    + "messages stay on the server; encrypted history may "
                                    + "need your recovery key or another verified device "
                                    + "afterwards."),
                                // NO primary action. There is no store left to
                                // quarantine, so a local reset has nothing to
                                // repair and the backend refuses it. The
                                // remedy is the sign-in form directly above,
                                // already prefilled with this account — the
                                // body says so. Offering a button here gave
                                // the user a red "this would destroy
                                // encryption keys" error on the exact state
                                // the store-identity bug leaves them in.
                                primaryLabel: "",
                                confirmTitle: "",
                                confirmBody: "",
                                showRemove: true
                            }
                        case "access_token_revoked":
                            // matrix::rust_session::suggestsLocalReset() is
                            // FALSE for this reason on purpose: the session
                            // died on the SERVER, so the local store is
                            // exactly the key material the user still
                            // needs, not something to clear. NO destructive
                            // primary action here — that is the whole fix
                            // for this reason code. "Remove this account"
                            // stays available as an explicit, honestly-
                            // worded fallback (its own confirm copy already
                            // states the encryption store is deleted), but
                            // it is never the default/suggested action.
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
                            // The keyring could not be read — locked, or the
                            // session bus is gone. The sign-in IS saved and
                            // the store is intact; nothing here is broken and
                            // nothing should be cleared. Purely informational:
                            // the remedy is outside Lightning.
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
                            // suggestsLocalReset() is FALSE here too, and for
                            // a sharper reason than access_token_revoked: the
                            // store DOES exist and DOES hold real key
                            // material, but Lightning cannot tell which of
                            // several candidates is the right one — clearing
                            // (or removing the account) could destroy the
                            // valid store instead of a stale one. Never guess:
                            // no destructive action of any kind here.
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
                // Snapshot of what the confirm dialog is acting on,
                // captured at OPEN time — never read `repair.*` live once
                // the dialog is up. Without this, a slow sign-in leaving a
                // stale card up (or a different account failing mid
                // add-account) could change the reason code or the target
                // account out from under an already-open dialog: the title
                // and body would visibly change (or go blank, if the new
                // reason has no card), and confirming would silently act
                // on the NEW failure/account instead of the one the user
                // actually reviewed and clicked through to. See the
                // confirm button's onClicked below for the matching
                // refuse-if-changed check, and the Connections block after
                // this one for the proactive auto-close.
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
                    // Proactive half of the fix: close immediately if the
                    // classified failure changes while the dialog is open,
                    // rather than leaving a stale confirmation showing
                    // (possibly blank) content the user never reviewed.
                    // onClicked's own reasonCode/userId comparison is the
                    // backstop for the (effectively zero-width, but not
                    // provably impossible) gap between this firing and the
                    // dialog actually closing.
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
                                // Bound to the BACKEND policy, not to a
                                // per-reason list kept in this file. A card
                                // must never offer an action that
                                // repairLocalSession() will refuse: that
                                // produced a red "this would destroy
                                // encryption keys" error on the one state a
                                // user recovering from the store-identity
                                // bug actually lands in.
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
                            color: repairPanel.statusOk ? AppTheme.success : AppTheme.danger
                            font.pixelSize: AppTheme.textMeta
                        }
                    }
                }

                // One shared confirmation dialog for both destructive
                // actions (repair, remove) — Cancel is the default/focused
                // button in both cases, and the body always names the
                // exact account being affected.
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
                    // Cancel is the default/focused button for every
                    // destructive confirmation in this card: `focus: true`
                    // on the button marks it as the candidate, but a plain
                    // Button (Qt.TabFocus policy, not a text-input control)
                    // is not reliably granted activeFocus purely from that
                    // the instant a custom-buttoned Dialog opens — grab it
                    // explicitly so the safe choice is genuinely what a
                    // keyboard Enter/Space press activates first.
                    onOpened: repairConfirmCancelButton.forceActiveFocus()

                    // Basic's Dialog header is a Label on a square
                    // palette.window strip; supply the app's own title
                    // treatment so this reads as one surface with the card
                    // behind it.
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
                            // Both were raw stock Buttons until 2026-08-21,
                            // and the two disagreed about everything: Cancel
                            // kept Basic's 100x40 SQUARE box (no background
                            // override, so implicitHeight came from Basic's
                            // 40), while the destructive one replaced the
                            // background with a Rectangle carrying no
                            // implicit size and landed at ~30px. Different
                            // heights, different radii, different colour
                            // systems, side by side — and the destructive
                            // one acknowledged neither hover nor keyboard
                            // focus, having dropped Basic's focus border
                            // with the background it replaced. AppButton
                            // gives both one geometry and all three states.
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
                                    // Backstop for the Connections auto-close
                                    // above: if the classified failure moved
                                    // on to a different reason OR a different
                                    // account since this dialog opened,
                                    // refuse rather than act on stale
                                    // confirmation — the account the user
                                    // actually reviewed is not necessarily
                                    // the one this would now affect.
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
