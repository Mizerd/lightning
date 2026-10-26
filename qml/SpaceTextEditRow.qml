import QtQuick
import QtQuick.Layouts
import MatrixClient

// One text field plus its commit button for a Space/room identity value
// (name, topic). Shared by the inline Space Home card and SpaceSettingsDialog
// so the two forms cannot drift apart again: they used to disagree on whether
// an unchanged value could be re-sent and on which kind the commit button was.
//
// The contract (master prompt Part 6, 2.3):
//   * the draft belongs to one explicit `targetId`; a target change always
//     re-snaps the field, whatever was typed;
//   * `authoritative` is the server value; a remote change re-snaps the field
//     only while the reader has not edited it;
//   * dirty compares the value that will actually be sent (trimmed);
//   * `requireNonEmpty` refuses an empty value (a name); a topic may be
//     cleared;
//   * permission, target identity and the pending flag are re-checked at
//     submission, not only in the button's enabled binding;
//   * Return in the field takes the same guarded path as the button.
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

    property alias field: field
    property alias button: button

    // What a commit would send.
    readonly property string draftValue: field.text.trim()
    readonly property bool dirty: draftValue !== root.authoritative
    readonly property bool canSubmit: root.canEdit && root.targetCurrent
                                      && !root.pending && root.dirty
                                      && (!root.requireNonEmpty
                                          || root.draftValue.length > 0)
    // True between a submission and the end of the pending edit it started,
    // so only the row that was committed shows progress.
    property bool _submitted: false
    readonly property bool busy: root._submitted && root.pending

    // Emitted with the trimmed value once every guard passed.
    signal submitted(string value)

    // The value the field was last snapped to, and for which target.
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
        // Only an untouched field follows the server.
        if (field.text === root._baseline)
            field.text = root.authoritative
        root._baseline = root.authoritative
    }
    onPendingChanged: if (!root.pending) root._submitted = false
    Component.onCompleted: root.reset()

    spacing: AppTheme.spacing8

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
        // The field's own onAccepted already submits on Return; a focused
        // button keeps AppButton's guarded Return handling.
        onClicked: root.submit()
        Accessible.description: root.busy
                                ? qsTr("Saving")
                                : (!root.dirty && root.canEdit
                                   ? qsTr("No changes to save") : "")
    }
}
