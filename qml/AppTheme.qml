pragma Singleton
import QtQuick

// v0.5.2: design-token foundation for the Lightning UI redesign.
// v0.5.11: extended into a multi-preset semantic theme system.
//
// This singleton is the SOLE source of truth for colors, spacing, radii,
// font sizes, and font families. Consumers should reach only for the
// semantic tokens below (background, textPrimary, accent, spacing8, ...)
// rather than hardcoding hex or magic-number pixel sizes.
//
// v0.7 themes (SettingsManager::Theme):
//   0 System          — follows the platform colour scheme
//                       (Moss Light or Indigo Night)
//   1 Lightning Light
//   2 Lightning Dark  (cool near-black; was a legacy alias of Midnight)
//   3 Graphite        (neutral dark grey)
//   4 Midnight        (the original slate/navy dark palette)
//   5 Nordic          (polar-night surfaces, frost accent)
//   6 Purple Dusk     (deep violet surfaces)
//   7 Warm            (light cream surfaces, amber accent)
//   8 Moss Light      (design-handoff light: warm neutrals, moss accent)
//   9 Indigo Night    (design-handoff dark: near-black, indigo accent)
//  10 Deep Teal       (design-handoff dark: deep teal surfaces + accent)
//
// Every preset provides the full set of semantic values; components never
// branch on the theme themselves. The raw `_xxxLight` / `_xxxDark` colour
// literals are retained (the theme-token/contrast test parses them by name)
// and the Light + Midnight palettes are byte-for-byte the previous values,
// so no existing view changes appearance under those presets.
QtObject {
    id: root

    // Raw SettingsManager::Theme enum int, pushed in from Main.qml.
    property int mode: 0
    // System dark-mode hint from the platform (QStyleHints::colorScheme),
    // pushed in from Main.qml via AppController.systemDarkMode.
    property bool systemDark: false

    // Resolve System (0) to the flagship design pair — Moss Light or
    // Indigo Night — based on the platform colour scheme.
    readonly property int effectiveTheme: mode === 0
                                          ? (systemDark ? 9 : 8)
                                          : mode
    // Any preset that is not one of the light surfaces is dark (used by
    // overlay chrome).
    readonly property bool dark: effectiveTheme !== 1 && effectiveTheme !== 7
                                 && effectiveTheme !== 8

    // ---- Raw palette literals (retained; the token test reads these). ----
    readonly property color _bgLight:            "#EBF0F7"
    readonly property color _bgDark:             "#0F172A"
    readonly property color _sidebarLight:       "#F4F8FC"
    readonly property color _sidebarDark:        "#111827"
    readonly property color _cardLight:          "#FFFFFF"
    readonly property color _cardDark:           "#1E293B"
    readonly property color _cardElevatedLight:  "#E8EEF7"
    readonly property color _cardElevatedDark:   "#243244"
    readonly property color _hoverLight:         "#DCE8FF"
    readonly property color _hoverDark:          "#243B6B"
    readonly property color _selectedLight:      "#BEDBFF"
    readonly property color _selectedDark:       "#2D4FA8"
    readonly property color _selectedHoverLight: "#A9CEFF"
    readonly property color _selectedHoverDark:  "#3A5DC0"
    readonly property color _selectedTextLight:  "#1E293B"
    readonly property color _selectedTextDark:   "#F8FAFC"
    readonly property color _accentBlue:         "#4F7CFF"
    readonly property color _accentBlueHover:    "#6B91FF"
    readonly property color _accentBluePressed:  "#3D66E0"
    readonly property color _outgoingBubbleBlue: "#3558C9"
    readonly property color _accentGreen:        "#22C55E"
    readonly property color _accentWarning:      "#E5A23C"
    readonly property color _accentDanger:       "#DC2626"
    readonly property color _accentInfo:         "#38BDF8"
    readonly property color _textPrimaryLight:   "#1E293B"
    readonly property color _textPrimaryDark:    "#F8FAFC"
    readonly property color _textSecondaryLight: "#5B6B84"
    readonly property color _textSecondaryDark:  "#CBD5E1"
    readonly property color _textMutedLight:     "#5B6B82"
    readonly property color _textMutedDark:      "#94A3B8"
    readonly property color _textDisabledLight:  "#A8B3C4"
    readonly property color _textDisabledDark:   "#64748B"
    readonly property color _borderLight:        "#C4D2E7"
    readonly property color _borderDark:         "#334155"
    readonly property color _borderStrongLight:  "#9DB0C9"
    readonly property color _borderStrongDark:   "#475569"
    readonly property color _inputBgLight:       "#FFFFFF"
    readonly property color _inputBgDark:        "#111827"
    readonly property color _codeBlockLight:     "#E3EAF5"
    readonly property color _codeBlockDark:      "#0B1220"

    // Lightning Dark — cool near-black, calmer than the navy Midnight.
    readonly property color _dkBg:            "#0D1117"
    readonly property color _dkSidebar:       "#10161D"
    readonly property color _dkCard:          "#161C26"
    readonly property color _dkCardElevated:  "#1E2633"
    readonly property color _dkHover:         "#202A3A"
    readonly property color _dkSelected:      "#2C4170"
    readonly property color _dkSelectedHover: "#395088"
    readonly property color _dkSelectedText:  "#F0F4FA"
    readonly property color _dkTextPrimary:   "#E9EEF6"
    readonly property color _dkTextSecondary: "#B7C2D4"
    readonly property color _dkTextMuted:     "#8D9AB0"
    readonly property color _dkTextDisabled:  "#5C6A80"
    readonly property color _dkBorder:        "#273143"
    readonly property color _dkBorderStrong:  "#3A4762"
    readonly property color _dkInputBg:       "#10161D"
    readonly property color _dkCodeBlock:     "#0A0E14"

    // Graphite — neutral dark grey.
    readonly property color _graBg:            "#1A1A1D"
    readonly property color _graSidebar:       "#202024"
    readonly property color _graCard:          "#26262B"
    readonly property color _graCardElevated:  "#303036"
    readonly property color _graHover:         "#34343B"
    readonly property color _graSelected:      "#3E4048"
    readonly property color _graSelectedHover: "#4A4C55"
    readonly property color _graSelectedText:  "#F5F5F7"
    readonly property color _graTextPrimary:   "#F5F5F7"
    readonly property color _graTextSecondary: "#C7C7CC"
    readonly property color _graTextMuted:     "#9A9AA2"
    readonly property color _graTextDisabled:  "#6A6A72"
    readonly property color _graBorder:        "#37373E"
    readonly property color _graBorderStrong:  "#4A4A52"
    readonly property color _graInputBg:       "#202024"
    readonly property color _graCodeBlock:     "#141416"
    readonly property color _graAccent:        "#5B8DEF"
    readonly property color _graAccentHover:   "#77A2F2"
    readonly property color _graAccentPressed: "#4372CE"
    readonly property color _graOwnBubble:     "#3A5BB5"
    readonly property color _graOtherBubble:   "#303036"

    // Nord — polar-night surfaces, frost accent.
    readonly property color _norBg:            "#2E3440"
    readonly property color _norSidebar:       "#2B303B"
    readonly property color _norCard:          "#3B4252"
    readonly property color _norCardElevated:  "#434C5E"
    readonly property color _norHover:         "#3B4252"
    readonly property color _norSelected:      "#4C566A"
    readonly property color _norSelectedHover: "#5A6377"
    readonly property color _norSelectedText:  "#ECEFF4"
    readonly property color _norTextPrimary:   "#ECEFF4"
    readonly property color _norTextSecondary: "#D8DEE9"
    readonly property color _norTextMuted:     "#ABB2C0"
    readonly property color _norTextDisabled:  "#727A8A"
    readonly property color _norBorder:        "#434C5E"
    readonly property color _norBorderStrong:  "#4C566A"
    readonly property color _norInputBg:       "#2B303B"
    readonly property color _norCodeBlock:     "#292E39"
    readonly property color _norAccent:        "#5E81AC"
    readonly property color _norAccentHover:   "#6D93C0"
    readonly property color _norAccentPressed: "#4C6C94"
    readonly property color _norOwnBubble:     "#4C6C94"
    readonly property color _norOtherBubble:   "#3B4252"

    // Purple Dusk — deep violet surfaces.
    readonly property color _purBg:            "#1E1B2E"
    readonly property color _purSidebar:       "#241F35"
    readonly property color _purCard:          "#2A2440"
    readonly property color _purCardElevated:  "#332C4E"
    readonly property color _purHover:         "#372E57"
    readonly property color _purSelected:      "#4A3D7A"
    readonly property color _purSelectedHover: "#574899"
    readonly property color _purSelectedText:  "#F3EEFF"
    readonly property color _purTextPrimary:   "#F3EEFF"
    readonly property color _purTextSecondary: "#D6CCF0"
    readonly property color _purTextMuted:     "#A99FC7"
    readonly property color _purTextDisabled:  "#726899"
    readonly property color _purBorder:        "#3A3255"
    readonly property color _purBorderStrong:  "#4C4270"
    readonly property color _purInputBg:       "#241F35"
    readonly property color _purCodeBlock:     "#161327"
    readonly property color _purAccent:        "#7C5CD6"
    readonly property color _purAccentHover:   "#9370E0"
    readonly property color _purAccentPressed: "#6748BE"
    readonly property color _purOwnBubble:     "#6748BE"
    readonly property color _purOtherBubble:   "#332C4E"

    // Warm — light cream surfaces with an amber accent.
    readonly property color _warBg:            "#F6F1E7"
    readonly property color _warSidebar:       "#FBF7EF"
    readonly property color _warCard:          "#FFFDF8"
    readonly property color _warCardElevated:  "#F1E9DB"
    readonly property color _warHover:         "#EDE2CE"
    readonly property color _warSelected:      "#E0CBA4"
    readonly property color _warSelectedHover: "#D5BC8E"
    readonly property color _warSelectedText:  "#3B3428"
    readonly property color _warTextPrimary:   "#3B3428"
    readonly property color _warTextSecondary: "#6B5F4C"
    readonly property color _warTextMuted:     "#6E6350"
    readonly property color _warTextDisabled:  "#B3A78F"
    readonly property color _warBorder:        "#DCD0B8"
    readonly property color _warBorderStrong:  "#C2B394"
    readonly property color _warInputBg:       "#FFFDF8"
    readonly property color _warCodeBlock:     "#EFE7D6"
    readonly property color _warAccent:        "#C2410C"
    readonly property color _warAccentHover:   "#D9581F"
    readonly property color _warAccentPressed: "#A83809"
    readonly property color _warOwnBubble:     "#8F420C"
    readonly property color _warOtherBubble:   "#EDE2CE"

    // Moss Light — design-handoff light theme (option 1a).
    readonly property color _mosBg:            "#F7F7F5"
    readonly property color _mosRail:          "#ECEDED"
    readonly property color _mosSidebar:       "#F2F3F1"
    readonly property color _mosCard:          "#FFFFFF"
    readonly property color _mosCardElevated:  "#FAFAF9"
    readonly property color _mosHover:         "#E8E9E7"
    readonly property color _mosSelected:      "#E2F4EE"
    readonly property color _mosSelectedHover: "#D5EEE5"
    readonly property color _mosSelectedText:  "#0D6E55"
    readonly property color _mosTextPrimary:   "#1C1E21"
    readonly property color _mosTextSecondary: "#5B6067"
    readonly property color _mosTextMuted:     "#6A6F76"
    readonly property color _mosTextDisabled:  "#9AA0A6"
    readonly property color _mosBorder:        "#E4E6E4"
    readonly property color _mosBorderStrong:  "#CFD3CF"
    readonly property color _mosInputBg:       "#FFFFFF"
    readonly property color _mosCodeBlock:     "#E8E9E7"
    readonly property color _mosAccent:        "#12A67F"
    readonly property color _mosAccentHover:   "#15B78C"
    readonly property color _mosAccentPressed: "#0F8F6D"
    readonly property color _mosAccentSoft:    "#E2F4EE"
    readonly property color _mosAccentBorder:  "#BFE6DA"
    readonly property color _mosOwnBubble:     "#0D6E55"
    readonly property color _mosOtherBubble:   "#E8E9E7"
    readonly property color _mosMention:       "#E04848"

    // Indigo Night — design-handoff dark theme (option 2a).
    readonly property color _indBg:            "#101016"
    readonly property color _indRail:          "#0E0E14"
    readonly property color _indSidebar:       "#14141B"
    readonly property color _indCard:          "#1B1B24"
    readonly property color _indCardElevated:  "#2A2A36"
    readonly property color _indHover:         "#1D1D26"
    readonly property color _indSelected:      "#25253D"
    readonly property color _indSelectedHover: "#2C2D4A"
    readonly property color _indSelectedText:  "#C3C5FF"
    readonly property color _indTextPrimary:   "#E8E8EF"
    readonly property color _indTextSecondary: "#A4A6B8"
    readonly property color _indTextMuted:     "#8D8FA0"
    readonly property color _indTextDisabled:  "#6F7183"
    readonly property color _indBorder:        "#23232D"
    readonly property color _indBorderStrong:  "#33333F"
    readonly property color _indInputBg:       "#1D1D26"
    readonly property color _indCodeBlock:     "#0B0B10"
    readonly property color _indAccent:        "#7C7FF2"
    readonly property color _indAccentHover:   "#9295F5"
    readonly property color _indAccentPressed: "#6568D6"
    readonly property color _indAccentSoft:    "#25253D"
    readonly property color _indAccentBorder:  "#383966"
    readonly property color _indOwnBubble:     "#4A4CB8"
    readonly property color _indOtherBubble:   "#2A2A36"
    readonly property color _indMention:       "#E5677A"
    readonly property color _indOnline:        "#63D6A3"

    // Deep Teal — design-handoff dark theme (option 2b). Accent fills use
    // dark ink (the accent itself is bright).
    readonly property color _teaBg:            "#0E1416"
    readonly property color _teaRail:          "#0A0F11"
    readonly property color _teaSidebar:       "#111A1D"
    readonly property color _teaCard:          "#132024"
    readonly property color _teaCardElevated:  "#182428"
    readonly property color _teaHover:         "#182428"
    readonly property color _teaSelected:      "#152E2C"
    readonly property color _teaSelectedHover: "#1D3B39"
    readonly property color _teaSelectedText:  "#DEF5F0"
    readonly property color _teaTextPrimary:   "#E6ECEC"
    readonly property color _teaTextSecondary: "#B9C8C8"
    readonly property color _teaTextMuted:     "#8FA5A8"
    readonly property color _teaTextDisabled:  "#5F7A7E"
    readonly property color _teaBorder:        "#1C2A2E"
    readonly property color _teaBorderStrong:  "#234046"
    readonly property color _teaInputBg:       "#182428"
    readonly property color _teaCodeBlock:     "#0A1113"
    readonly property color _teaAccent:        "#27C2AD"
    readonly property color _teaAccentHover:   "#3FD2BE"
    readonly property color _teaAccentPressed: "#1EA593"
    readonly property color _teaAccentSoft:    "#112928"
    readonly property color _teaAccentBorder:  "#1F4A44"
    readonly property color _teaAccentText:    "#062A25"
    readonly property color _teaOwnBubble:     "#1C4A43"
    readonly property color _teaOtherBubble:   "#182428"
    readonly property color _teaMention:       "#E5677A"

    // Ink used on top of accent fills for every palette without its own
    // accentText (the contrast test reads this literal by name).
    readonly property color _onAccent:         "#FFFFFF"

    // ---- Resolved palette object for the effective theme. ----
    readonly property var _light: ({
        background: _bgLight, sidebar: _sidebarLight, surface: _cardLight,
        cardElevated: _cardElevatedLight, hover: _hoverLight,
        selected: _selectedLight, selectedHover: _selectedHoverLight,
        selectedText: _selectedTextLight, inputBg: _inputBgLight,
        codeBlock: _codeBlockLight, textPrimary: _textPrimaryLight,
        textSecondary: _textSecondaryLight, textMuted: _textMutedLight,
        textDisabled: _textDisabledLight, border: _borderLight,
        borderStrong: _borderStrongLight, accent: _accentBlue,
        accentHover: _accentBlueHover, accentPressed: _accentBluePressed,
        ownBubble: _outgoingBubbleBlue, otherBubble: _hoverLight
    })
    readonly property var _dark: ({
        background: _dkBg, sidebar: _dkSidebar, surface: _dkCard,
        cardElevated: _dkCardElevated, hover: _dkHover,
        selected: _dkSelected, selectedHover: _dkSelectedHover,
        selectedText: _dkSelectedText, inputBg: _dkInputBg,
        codeBlock: _dkCodeBlock, textPrimary: _dkTextPrimary,
        textSecondary: _dkTextSecondary, textMuted: _dkTextMuted,
        textDisabled: _dkTextDisabled, border: _dkBorder,
        borderStrong: _dkBorderStrong, accent: _accentBlue,
        accentHover: _accentBlueHover, accentPressed: _accentBluePressed,
        ownBubble: _outgoingBubbleBlue, otherBubble: _dkCardElevated
    })
    readonly property var _midnight: ({
        background: _bgDark, sidebar: _sidebarDark, surface: _cardDark,
        cardElevated: _cardElevatedDark, hover: _hoverDark,
        selected: _selectedDark, selectedHover: _selectedHoverDark,
        selectedText: _selectedTextDark, inputBg: _inputBgDark,
        codeBlock: _codeBlockDark, textPrimary: _textPrimaryDark,
        textSecondary: _textSecondaryDark, textMuted: _textMutedDark,
        textDisabled: _textDisabledDark, border: _borderDark,
        borderStrong: _borderStrongDark, accent: _accentBlue,
        accentHover: _accentBlueHover, accentPressed: _accentBluePressed,
        ownBubble: _outgoingBubbleBlue, otherBubble: _cardElevatedDark
    })
    readonly property var _graphite: ({
        background: _graBg, sidebar: _graSidebar, surface: _graCard,
        cardElevated: _graCardElevated, hover: _graHover,
        selected: _graSelected, selectedHover: _graSelectedHover,
        selectedText: _graSelectedText, inputBg: _graInputBg,
        codeBlock: _graCodeBlock, textPrimary: _graTextPrimary,
        textSecondary: _graTextSecondary, textMuted: _graTextMuted,
        textDisabled: _graTextDisabled, border: _graBorder,
        borderStrong: _graBorderStrong, accent: _graAccent,
        accentHover: _graAccentHover, accentPressed: _graAccentPressed,
        ownBubble: _graOwnBubble, otherBubble: _graOtherBubble
    })
    readonly property var _nord: ({
        background: _norBg, sidebar: _norSidebar, surface: _norCard,
        cardElevated: _norCardElevated, hover: _norHover,
        selected: _norSelected, selectedHover: _norSelectedHover,
        selectedText: _norSelectedText, inputBg: _norInputBg,
        codeBlock: _norCodeBlock, textPrimary: _norTextPrimary,
        textSecondary: _norTextSecondary, textMuted: _norTextMuted,
        textDisabled: _norTextDisabled, border: _norBorder,
        borderStrong: _norBorderStrong, accent: _norAccent,
        accentHover: _norAccentHover, accentPressed: _norAccentPressed,
        ownBubble: _norOwnBubble, otherBubble: _norOtherBubble
    })
    readonly property var _purple: ({
        background: _purBg, sidebar: _purSidebar, surface: _purCard,
        cardElevated: _purCardElevated, hover: _purHover,
        selected: _purSelected, selectedHover: _purSelectedHover,
        selectedText: _purSelectedText, inputBg: _purInputBg,
        codeBlock: _purCodeBlock, textPrimary: _purTextPrimary,
        textSecondary: _purTextSecondary, textMuted: _purTextMuted,
        textDisabled: _purTextDisabled, border: _purBorder,
        borderStrong: _purBorderStrong, accent: _purAccent,
        accentHover: _purAccentHover, accentPressed: _purAccentPressed,
        ownBubble: _purOwnBubble, otherBubble: _purOtherBubble
    })
    readonly property var _warm: ({
        background: _warBg, sidebar: _warSidebar, surface: _warCard,
        cardElevated: _warCardElevated, hover: _warHover,
        selected: _warSelected, selectedHover: _warSelectedHover,
        selectedText: _warSelectedText, inputBg: _warInputBg,
        codeBlock: _warCodeBlock, textPrimary: _warTextPrimary,
        textSecondary: _warTextSecondary, textMuted: _warTextMuted,
        textDisabled: _warTextDisabled, border: _warBorder,
        borderStrong: _warBorderStrong, accent: _warAccent,
        accentHover: _warAccentHover, accentPressed: _warAccentPressed,
        ownBubble: _warOwnBubble, otherBubble: _warOtherBubble
    })
    readonly property var _moss: ({
        background: _mosBg, rail: _mosRail, sidebar: _mosSidebar,
        surface: _mosCard, cardElevated: _mosCardElevated, hover: _mosHover,
        selected: _mosSelected, selectedHover: _mosSelectedHover,
        selectedText: _mosSelectedText, inputBg: _mosInputBg,
        codeBlock: _mosCodeBlock, textPrimary: _mosTextPrimary,
        textSecondary: _mosTextSecondary, textMuted: _mosTextMuted,
        textDisabled: _mosTextDisabled, border: _mosBorder,
        borderStrong: _mosBorderStrong, accent: _mosAccent,
        accentHover: _mosAccentHover, accentPressed: _mosAccentPressed,
        accentSoft: _mosAccentSoft, accentBorder: _mosAccentBorder,
        ownBubble: _mosOwnBubble, otherBubble: _mosOtherBubble,
        mention: _mosMention, online: _mosAccent
    })
    readonly property var _indigo: ({
        background: _indBg, rail: _indRail, sidebar: _indSidebar,
        surface: _indCard, cardElevated: _indCardElevated, hover: _indHover,
        selected: _indSelected, selectedHover: _indSelectedHover,
        selectedText: _indSelectedText, inputBg: _indInputBg,
        codeBlock: _indCodeBlock, textPrimary: _indTextPrimary,
        textSecondary: _indTextSecondary, textMuted: _indTextMuted,
        textDisabled: _indTextDisabled, border: _indBorder,
        borderStrong: _indBorderStrong, accent: _indAccent,
        accentHover: _indAccentHover, accentPressed: _indAccentPressed,
        accentSoft: _indAccentSoft, accentBorder: _indAccentBorder,
        ownBubble: _indOwnBubble, otherBubble: _indOtherBubble,
        mention: _indMention, online: _indOnline
    })
    readonly property var _teal: ({
        background: _teaBg, rail: _teaRail, sidebar: _teaSidebar,
        surface: _teaCard, cardElevated: _teaCardElevated, hover: _teaHover,
        selected: _teaSelected, selectedHover: _teaSelectedHover,
        selectedText: _teaSelectedText, inputBg: _teaInputBg,
        codeBlock: _teaCodeBlock, textPrimary: _teaTextPrimary,
        textSecondary: _teaTextSecondary, textMuted: _teaTextMuted,
        textDisabled: _teaTextDisabled, border: _teaBorder,
        borderStrong: _teaBorderStrong, accent: _teaAccent,
        accentHover: _teaAccentHover, accentPressed: _teaAccentPressed,
        accentSoft: _teaAccentSoft, accentBorder: _teaAccentBorder,
        accentText: _teaAccentText,
        ownBubble: _teaOwnBubble, otherBubble: _teaOtherBubble,
        mention: _teaMention, online: _teaAccent
    })
    // Selectable theme presets for the Settings theme picker, design order
    // (handoff themes first). System (0) is a resolution mode, not a palette,
    // so it is represented by the match-system toggle instead of a card.
    readonly property var themeList: [
        { id: 8,  name: qsTr("Moss Light") },
        { id: 9,  name: qsTr("Indigo Night") },
        { id: 10, name: qsTr("Deep Teal") },
        { id: 1,  name: qsTr("Lightning Light") },
        { id: 2,  name: qsTr("Lightning Dark") },
        { id: 3,  name: qsTr("Graphite") },
        { id: 4,  name: qsTr("Midnight") },
        { id: 5,  name: qsTr("Nordic") },
        { id: 6,  name: qsTr("Purple Dusk") },
        { id: 7,  name: qsTr("Warm") }
    ]

    // Palette lookup for rendering theme preview cards. Must route exactly
    // like _p below; keys missing from pre-handoff palettes (rail,
    // accentSoft, accentBorder, accentText) are filled with the same
    // fallbacks the semantic aliases use.
    function paletteForTheme(id) {
        var p
        switch (id) {
        case 1:  p = _light; break
        case 2:  p = _dark; break
        case 3:  p = _graphite; break
        case 4:  p = _midnight; break
        case 5:  p = _nord; break
        case 6:  p = _purple; break
        case 7:  p = _warm; break
        case 8:  p = _moss; break
        case 9:  p = _indigo; break
        case 10: p = _teal; break
        default: p = _p; break
        }
        return {
            background: p.background,
            rail: p.rail !== undefined ? p.rail : p.sidebar,
            sidebar: p.sidebar,
            surface: p.surface,
            hover: p.hover,
            border: p.border,
            accent: p.accent,
            accentSoft: p.accentSoft !== undefined ? p.accentSoft
                                                   : Qt.alpha(p.accent, 0.16),
            accentBorder: p.accentBorder !== undefined ? p.accentBorder
                                                       : Qt.alpha(p.accent, 0.35),
            textPrimary: p.textPrimary,
            textMuted: p.textMuted
        }
    }

    readonly property var _p: {
        switch (effectiveTheme) {
        case 1:  return _light
        case 2:  return _dark       // Lightning Dark
        case 3:  return _graphite
        case 4:  return _midnight   // Midnight
        case 5:  return _nord
        case 6:  return _purple
        case 7:  return _warm
        case 8:  return _moss
        case 9:  return _indigo
        case 10: return _teal
        default: return _light
        }
    }

    // ---- Semantic aliases (preferred). ----
    readonly property color background:          _p.background
    readonly property color windowBackground:    background
    readonly property color sidebar:             _p.sidebar
    readonly property color navBackground:       sidebar
    readonly property color panelBackground:     sidebar
    // Spaces-rail surface; palettes without a dedicated rail tone reuse the
    // sidebar so the shell stays coherent.
    readonly property color rail:                _p.rail !== undefined
                                                 ? _p.rail : _p.sidebar
    readonly property color surface:             _p.surface
    readonly property color card:                surface
    readonly property color cardElevated:        _p.cardElevated
    readonly property color surfaceElevated:     cardElevated
    readonly property color hover:               _p.hover
    readonly property color selected:            _p.selected
    readonly property color selectedHover:       _p.selectedHover
    readonly property color selectedText:        _p.selectedText
    readonly property color accent:              _p.accent
    readonly property color accentHover:         _p.accentHover
    readonly property color accentPressed:       _p.accentPressed
    // Ink on accent fills. Bright accents (Deep Teal) supply their own dark
    // ink; everything else uses white.
    readonly property color accentText:          _p.accentText !== undefined
                                                 ? _p.accentText : _onAccent
    // Soft accent tint + its border — selection chips, active icon chips,
    // own-reaction pills (design-handoff themes carry exact values; older
    // palettes fall back to their selection tones).
    readonly property color accentSoft:          _p.accentSoft !== undefined
                                                 ? _p.accentSoft : _p.selected
    readonly property color accentBorder:        _p.accentBorder !== undefined
                                                 ? _p.accentBorder
                                                 : _p.borderStrong
    readonly property color success:             _accentGreen
    readonly property color warning:             _accentWarning
    readonly property color danger:              _accentDanger
    readonly property color dangerText:          "#FFFFFF"
    readonly property color info:                _accentInfo
    readonly property color textPrimary:         _p.textPrimary
    readonly property color textSecondary:       _p.textSecondary
    readonly property color textMuted:           _p.textMuted
    readonly property color textDisabled:        _p.textDisabled
    readonly property color border:              _p.border
    readonly property color borderSubtle:        border
    readonly property color borderStrong:        _p.borderStrong
    readonly property color separator:           border
    readonly property color inputBackground:     _p.inputBg
    readonly property color inputBorder:         border
    readonly property color focusRing:           _p.accent
    // Design --icon token: bare interface icons at rest (handoff themes map
    // it to their muted tone; older palettes follow the same rule).
    readonly property color icon:                textMuted
    // The design's shadow budget allows exactly four shadows (composer card,
    // quick-switcher modal, account popover, slider thumb); this is their
    // per-theme tint.
    readonly property color shadow:              dark ? "#59000000"
                                                      : "#0A000000"
    readonly property color overlayScrim:        "#80000000"
    readonly property color codeBlock:           _p.codeBlock
    // Link colour — accent by default, readable on every surface.
    readonly property color link:                _p.accent

    // Message-bubble semantics.
    readonly property color ownMessageBubble:    _p.ownBubble
    readonly property color ownBubble:           ownMessageBubble
    readonly property color ownBubbleText:       "#FFFFFF"
    readonly property color onAccentMuted:       "#DCE4FF"
    readonly property color otherMessageBubble:  _p.otherBubble
    readonly property color otherBubble:         otherMessageBubble
    readonly property color otherBubbleText:     textPrimary
    readonly property color incomingBubble:      otherMessageBubble
    readonly property color outgoingBubble:      ownMessageBubble
    readonly property color bubbleOverlay:       "#26000000"
    readonly property color bubbleOverlaySubtle: "#0F000000"
    // Reactions and badges.
    readonly property color reactionBackground:  cardElevated
    readonly property color reactionSelectedBackground: selected
    readonly property color reactionHighlight:   selected
    readonly property color unreadBadge:         accent
    // Mention red — the design themes carry their exact handoff hue; older
    // palettes fall back to the shared danger red.
    readonly property color mentionBadge:        _p.mention !== undefined
                                                 ? _p.mention : danger
    readonly property color undecryptableText:   textMuted
    // Highlight semantics (jumped-to message rows, active thread affordances).
    readonly property color pressedSurface:      selectedHover
    readonly property color messageHighlight:    selected
    readonly property color threadHighlight:     accent
    // Presence dots (design: online = accent-family green, away = yellow).
    readonly property color presenceOnline:      _p.online !== undefined
                                                 ? _p.online : success
    readonly property color presenceAway:        "#C9B23A"

    // Deterministic initials-avatar palette from the design handoff; shared
    // by every theme so a user or room keeps one colour everywhere.
    readonly property var avatarPalette: [
        "#2F8F5B", "#A3542F", "#6D5BD0", "#3A6EA5", "#B04A7E",
        "#C9662A", "#4A8F6D", "#B3823A", "#A05A92"
    ]

    // ---- Legacy aliases retained for existing QML. ----
    readonly property color surfaceAlt:          cardElevated
    readonly property color text:                textPrimary
    readonly property color muted:               textMuted
    readonly property color selectedBg:          selected
    readonly property color error:               _accentDanger

    // ---- Spacing scale. ----
    readonly property int spacing2:  2
    readonly property int spacing4:  4
    readonly property int spacing6:  6
    readonly property int spacing8:  8
    readonly property int spacing12: 12
    readonly property int spacing16: 16
    readonly property int spacing20: 20
    readonly property int spacing24: 24

    readonly property int spacingXS: spacing4
    readonly property int spacingS:  spacing8
    readonly property int spacingM:  spacing12
    readonly property int spacingL:  spacing16
    readonly property int spacingXL: spacing24

    // ---- Radii. ----
    readonly property int radiusSm:   4
    readonly property int radiusMd:   8
    readonly property int radiusLg:   12
    readonly property int radiusPill: 999
    readonly property int radius:     radiusMd

    // ---- Typography. ----
    readonly property int fontSizeXS:        11
    readonly property int fontSizeS:         13
    readonly property int fontSizeM:         14
    readonly property int fontSizeRoom:      16
    readonly property int fontSizeHeader:    18
    readonly property int fontSizePageTitle: 24
    readonly property int fontSizeL:         fontSizeRoom
    readonly property int fontSizeXL:        fontSizeHeader

    readonly property int fontPageTitle:     fontSizePageTitle // 24
    readonly property int fontSectionTitle:  fontSizeHeader    // 18
    readonly property int fontRoomTitle:     fontSizeRoom      // 16
    readonly property int fontBody:          fontSizeM         // 14
    readonly property int fontSecondary:     fontSizeS         // 13
    readonly property int fontMessageSender: 12
    readonly property int fontCaption:       fontSizeXS        // 11
    readonly property int fontMono:          fontSizeS

    // ---- Font families. ----
    // Manrope ships with Lightning (data/fonts, loaded in main.cpp); the
    // rest are graceful fallbacks for stripped-down builds.
    readonly property string uiFont:          "Manrope"
    readonly property var    uiFontFamilies:  [
        "Manrope",
        "Inter",
        "SF Pro Display",
        "Segoe UI Variable",
        "Segoe UI",
        "system-ui",
        "sans-serif"
    ]
    readonly property string monoFont:         "JetBrains Mono"
    readonly property var    monoFontFamilies: [
        "JetBrains Mono",
        "Fira Mono",
        "SF Mono",
        "Consolas",
        "monospace"
    ]
}
