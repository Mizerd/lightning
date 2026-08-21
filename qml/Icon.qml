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
        "explore": "\ue87a",
        "travel_explore": "\ue2db",
        "add": "\ue145",
        "add_circle": "\ue990",
        "add_reaction": "\ue1d3",
        "settings": "\ue8b8",
        "group": "\uea21",
        "groups": "\uf233",
        "person": "\uf0d3",
        "person_add": "\uea4d",
        "person_remove": "\uef66",
        "block": "\uf08c",
        "undo": "\ue166",
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
        // v0.6.5: was the legacy Material Icons codepoint (\ue853), which the
        // Symbols variable font never carried \u2014 the glyph rendered blank.
        "account_circle": "\uf20b",
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
        "format_bold": "\ue238",
        "format_italic": "\ue23f",
        "strikethrough_s": "\ue257",
        "code": "\ue86f",
        "link": "\ue250",
        "format_list_bulleted": "\ue241",
        "format_quote": "\ue244",
        // v0.7: inline media playback controls.
        "pause": "\ue034",
        "stop": "\ue047",
        "volume_up": "\ue050",
        "volume_off": "\ue04f",
        "open_in_full": "\uf1ce",
        "close_fullscreen": "\uf1cf",
        "graphic_eq": "\ue1b8",
        "speed": "\ue9e4",
        "zoom_in": "\ue8ff",
        "zoom_out": "\ue900",
        "fit_screen": "\uea10",
        "chevron_left": "\ue5cb",
        "chevron_right": "\ue5cc",
        // v0.6.5 menu-language glyphs (SPEC 1a-1v): flyout radio rows, the
        // Lightning bolt (outline at the subset's FILL=0), keycap icons,
        // new-conversation rows, and the emoji-picker category rail.
        "radio_button_checked": "\ue837",
        "radio_button_unchecked": "\ue836",
        "bolt": "\uea0b",
        "keyboard_tab": "\ue31c",
        "group_add": "\ue7f0",
        "person_search": "\uf106",
        "pets": "\ue91d",
        "restaurant": "\ue56c",
        "sports_esports": "\uea28",
        "flight": "\ue539",
        "lightbulb": "\ue90f",
        "emoji_symbols": "\uea1e",
        "flag": "\uf0c6",
    })

    text: _codepoints[name] !== undefined ? _codepoints[name] : ""
    // The family comes from AppTheme like every other face in the app. It was
    // the one raw family literal in the tree that was neither a deliberate
    // preview nor a mirror, which also made it invisible to a family sweep.
    font.family: AppTheme.iconFont
    font.pixelSize: size
    color: AppTheme.textSecondary
    verticalAlignment: Text.AlignVCenter
    horizontalAlignment: Text.AlignHCenter
    renderType: Text.QtRendering
}
