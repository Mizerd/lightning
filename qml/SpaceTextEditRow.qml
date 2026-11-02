import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

RowLayout {
    id: root

    property string targetId: ""
    property string authoritative: ""
    property bool canEdit: false
    // The shared roomInfo is bound to this target. Evaluated at submission
    // too, so a stale form cannot write into another room.
    property bool targetCurrent: true
    property bool pending: false
    property bool requireNonEmpty: false
    property bool storm: false
    property string buttonText: qsTr("Save")
    property string fieldObjectName: ""
    property string buttonObjectName: ""
    property string accessibleName: ""
    property string placeholderText: ""
    property string readOnlyHint: qsTr("Only Space admins can change this.")

    property alias field: field
    property alias button: button

    readonly property string draftValue: field.text.trim()
    readonly property bool dirty: draftValue !== root.authoritative
    readonly property bool canSubmit: root.canEdit && root.targetCurrent
                                      && !root.pending && root.dirty
                                      && (!root.requireNonEmpty
                                          || root.draftValue.length > 0)
    property bool _submitted: false
    readonly property bool busy: root._submitted && root.pending

    signal submitted(string value)

    property string _baseline: ""
    property string _draftTarget: ""

    function reset() {
        root._baseline = root.authoritative
        root._draftTarget = root.targetId
        root._submitted = false
        field.text = root._baseline
    }
    function submit() {
        if (!root.canSubmit)
            return false
        root._submitted = true
        root.submitted(root.draftValue)
        return true
    }

    onTargetIdChanged: root.reset()
    onAuthoritativeChanged: {
        if (root._draftTarget !== root.targetId) {
            root.reset()
            return
        }
        if (field.text === root._baseline)
            field.text = root.authoritative
        root._baseline = root.authoritative
    }
    onPendingChanged: if (!root.pending) root._submitted = false
    Component.onCompleted: root.reset()

    spacing: AppTheme.spacing8

    HoverHandler { id: hint }
    ToolTip.visible: hint.hovered && !root.canEdit && root.targetCurrent
                     && root.readOnlyHint.length > 0
    ToolTip.delay: 500
    ToolTip.text: root.readOnlyHint

    AppTextField {
        id: field
        objectName: root.fieldObjectName
        storm: root.storm
        Layout.fillWidth: true
        enabled: root.canEdit
        placeholderText: root.placeholderText
        onAccepted: root.submit()
        Accessible.name: root.accessibleName
    }
    AppButton {
        id: button
        objectName: root.buttonObjectName
        storm: root.storm
        kind: "primary"
        text: root.buttonText
        busy: root.busy
        enabled: root.canSubmit
        onClicked: root.submit()
        Accessible.description: root.busy
                                ? qsTr("Saving")
                                : (!root.dirty && root.canEdit
                                   ? qsTr("No changes to save") : "")
    }
}
