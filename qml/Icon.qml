import QtQuick
import MatrixClient

// v0.7 design shell: single icon primitive for ALL interface chrome.
// Renders a Material Symbols Rounded glyph by name (codepoint-mapped, so
// no ligature shaping is required), inherits theme colours, and keeps one
// optical size. Interface chrome must never use emoji as icons; real emoji
// belong only in user content and reactions.
Text {
    id: root

    // Material Symbols icon name, e.g. "home", "forum", "group".
    property string name: ""
    // Optical/pixel size of the glyph.
    property int size: 18

    readonly property var _codepoints: ({
        "home": "\ue9b2",
        "search": "\uef7a",
        "add": "\ue145",
        "add_circle": "\ue990",
        "settings": "\ue8b8",
        "group": "\uea21",
        "groups": "\uf233",
        "person": "\uf0d3",
        "person_add": "\uea4d",
        "forum": "\ue8af",
        "chat_bubble": "\ue0cb",
        "info": "\ue88e",
        "call": "\uf0d4",
        "videocam": "\ue04b",
        "push_pin": "\uf10d",
        "notifications": "\ue7f5",
        "notifications_off": "\ue7f6",
        "mood": "\uea22",
        "mic": "\ue31d",
        "send": "\ue163",
        "close": "\ue5cd",
        "check": "\ue668",
        "check_circle": "\uf0be",
        "done_all": "\ue877",
        "lock": "\ue899",
        "lock_open": "\ue898",
        "verified_user": "\uf013",
        "palette": "\ue40a",
        "devices": "\ue326",
        "science": "\uea4b",
        "expand_more": "\ue5cf",
        "expand_less": "\ue5ce",
        "alternate_email": "\ue0e6",
        "edit_square": "\uf88d",
        "unfold_more": "\ue5d7",
        "play_arrow": "\ue037",
        "arrow_back": "\ue5c4",
        "arrow_forward": "\ue5c8",
        "more_horiz": "\ue5d3",
        "more_vert": "\ue5d4",
        "star": "\uf09a",
        "refresh": "\ue5d5",
        "download": "\uf090",
        "attach_file": "\ue226",
        "image": "\ue3f4",
        "description": "\ue873",
        "logout": "\ue9ba",
        "delete": "\ue92e",
        "visibility": "\ue8f4",
        "visibility_off": "\ue8f5",
        "content_copy": "\ue14d",
        "reply": "\ue15e",
        "gif_box": "\ue7a3",
        "keyboard_return": "\ue31b",
        "workspaces": "\uea0f",
        "tag": "\ue9ef",
        "shield": "\ue9e0",
        "key": "\ue73c",
        "warning": "\uf083",
        "error": "\uf8b6",
        "schedule": "\uefd6",
    })

    text: _codepoints[name] !== undefined ? _codepoints[name] : ""
    font.family: "Material Symbols Rounded"
    font.pixelSize: size
    color: AppTheme.textSecondary
    verticalAlignment: Text.AlignVCenter
    horizontalAlignment: Text.AlignHCenter
    renderType: Text.QtRendering
}
