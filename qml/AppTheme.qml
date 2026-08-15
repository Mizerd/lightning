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
//                       (Moss Light or Storm)
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
//  11 Storm           (0.6.5 brand theme: deep navy surfaces, bolt yellow)
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
    // Content text scale (Settings → Appearance → Text size), pushed in from
    // Main.qml as settings.textScale / 100. Applies to message/content text
    // through scaled(); fixed chrome and icon sizes stay unscaled.
    property real textScale: 1.0
    function scaled(px) { return Math.round(px * textScale) }
    // System dark-mode hint from the platform (QStyleHints::colorScheme),
    // pushed in from Main.qml via AppController.systemDarkMode.
    property bool systemDark: false
    // v0.7: app-wide reduced-motion hint consumed by loading skeletons and
    // other decorative animation. False by default; a future accessibility
    // setting or platform hint can drive it without touching consumers.
    property bool reducedMotion: false

    // Resolve System (0) to the flagship design pair — Moss Light or, since
    // the 0.6.5 Storm round, Storm for dark systems. Only the UNSET/System
    // state resolves here; an explicitly persisted theme id 1–11 is never
    // rerouted, so introducing Storm silently changes nobody's stored choice.
    readonly property int effectiveTheme: mode === 0
                                          ? (systemDark ? 11 : 8)
                                          : mode
    // Any preset that is not one of the light surfaces is dark (used by
    // overlay chrome).
    readonly property bool dark: effectiveTheme !== 1 && effectiveTheme !== 7
                                 && effectiveTheme !== 8
    // True while the Storm brand theme (11) is the effective palette. Views
    // never branch on this — it exists so the storm* token namespace below
    // can route between the Storm literals and each legacy theme's own
    // semantic tones inside this singleton.
    readonly property bool storm: effectiveTheme === 11

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

    // Storm — the 0.6.5 brand theme (selectable id 11). Deep navy surfaces,
    // bolt-yellow accent, SPEC-storm-language §1 palette. These literals are
    // ALSO the fixed values behind the theme-invariant trust card and the
    // storm* namespace when Storm is the effective theme; the token test
    // parses them by name.
    readonly property color _stoCanvas:        "#0A0F24"
    readonly property color _stoPanel:         "#0D1B45"
    readonly property color _stoInset:         "#0A1231"
    readonly property color _stoDeep:          "#080C1C"
    readonly property color _stoBorder:        "#1B2C60"
    readonly property color _stoBorderStrong:  "#2B3C78"
    readonly property color _stoSelection:     "#132558"
    readonly property color _stoBolt:          "#FFD447"
    readonly property color _stoText:          "#F2F4FF"
    readonly property color _stoTextSecondary: "#C9D2F2"
    readonly property color _stoTextMuted:     "#7D8BBF"
    readonly property color _stoTextFaint:     "#5C6BA3"
    readonly property color _stoDanger:        "#FF8FA0"
    readonly property color _stoSuccess:       "#63D6A3"
    readonly property color _stoLink:          "#9295F5"
    // Storm derivatives that the spec table does not carry: hover/pressed
    // steps of the bolt accent, the own-bubble navy (distinct from panel and
    // selection so outgoing messages read as their own surface), and the
    // mention red — the handoff mention hue, NOT stormDanger: the mention
    // pill carries white ink, and #FF8FA0 is too light under it.
    readonly property color _stoAccentHover:   "#FFDF6E"
    readonly property color _stoAccentPressed: "#E9BC2F"
    readonly property color _stoOwnBubble:     "#3345A6"
    readonly property color _stoSelectedHover: "#1A3070"
    readonly property color _stoMention:       "#E5677A"

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
    // Storm's shell mapping. accent stays bolt for the sanctioned roles
    // (focus, checked state, ONE primary action, the Home tile) — the roles
    // that would over-yellow the shell if they inherited it get their own
    // values instead: hover is the SettingsNav 55%-alpha selection idiom,
    // selection ink brightens like a menu row (never yellow text), unread
    // badges ride the periwinkle link tone so a busy room list is not a
    // wall of bolt pills, and bubbles keep the app-wide blue=own language.
    // accentSoft/accentBorder are EXPLICIT translucent-bolt treatments: the
    // shared aliases would otherwise fall back to selected/borderStrong
    // navy, and the own-reaction pill is a sanctioned "current selection"
    // yellow moment (review NIT1 corrected the earlier fallback claim).
    readonly property var _storm: ({
        background: _stoDeep, rail: _stoDeep, sidebar: _stoCanvas,
        surface: _stoPanel, cardElevated: _stoSelection,
        hover: Qt.alpha(_stoSelection, 0.55),
        selected: _stoSelection, selectedHover: _stoSelectedHover,
        selectedText: _stoText, inputBg: _stoInset,
        codeBlock: _stoDeep, textPrimary: _stoText,
        textSecondary: _stoTextSecondary, textMuted: _stoTextMuted,
        textDisabled: _stoTextFaint, border: _stoBorder,
        borderStrong: _stoBorderStrong, accent: _stoBolt,
        accentHover: _stoAccentHover, accentPressed: _stoAccentPressed,
        accentSoft: Qt.alpha(_stoBolt, 0.14),
        accentBorder: Qt.alpha(_stoBolt, 0.35),
        accentText: _stoCanvas,
        ownBubble: _stoOwnBubble, otherBubble: _stoPanel,
        unreadBadge: _stoLink, mentionHighlight: _stoMention,
        mention: _stoMention, online: _stoSuccess, link: _stoLink
    })
    // Selectable theme presets for the Settings theme picker, design order
    // (Storm — the 0.6.5 brand theme — first, then the handoff themes).
    // System (0) is a resolution mode, not a palette, so it is represented
    // by the match-system toggle instead of a card.
    readonly property var themeList: [
        { id: 11, name: qsTr("Storm") },
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
        case 11: p = _storm; break
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
        case 11: return _storm
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
    // v0.6.5 danger roles (SPEC §0 names "mentionBadge/danger red"; the two
    // diverge, so they are split by role): dangerInk for icon/label ink,
    // the soft tint pair for destructive-row fills and warning-chip borders.
    readonly property color dangerInk:           danger
    readonly property color dangerSoft:          Qt.alpha(mentionBadge, 0.10)
    readonly property color dangerBorder:        Qt.alpha(mentionBadge, 0.25)
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
    // The design's shadow budget: the composer card, the slider thumb, and
    // the v0.6.5 overlay-centred popovers (quick switcher, account switcher,
    // member profile). Context menus stay border-only — their popup geometry
    // feeds delegate anchor maths and must not inflate. Shared per-theme tint.
    readonly property color shadow:              dark ? "#59000000"
                                                      : "#0A000000"
    readonly property color overlayScrim:        "#80000000"
    // Ink on overlayScrim media badges (GIF/size pills, media chrome).
    // ALWAYS white: the scrim is theme-invariant near-black, so accentText —
    // which Deep Teal deliberately resolves to a dark ink for its bright
    // accent fills — must never be used here.
    readonly property color scrimInk:            "#FFFFFF"
    // v0.6.5: modal backdrop for the quick switcher and centred dialogs
    // (SPEC §1j rgba(8,8,12,.45)). Deliberately distinct from overlayScrim,
    // which media badges and the account-switch blocker already consume.
    readonly property color modalScrim:          "#7308080C"
    readonly property color codeBlock:           _p.codeBlock
    // Link colour — accent by default, readable on every surface. Storm
    // carries its own periwinkle link ink: the bolt accent is reserved for
    // selection/focus/primary (§1 yellow discipline), never inline links.
    readonly property color link:                _p.link !== undefined
                                                 ? _p.link : _p.accent

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
    // Unread pill fill — accent by default; Storm overrides it (periwinkle)
    // so a busy room list is not a wall of bolt-yellow pills (§1 discipline).
    readonly property color unreadBadge:         _p.unreadBadge !== undefined
                                                 ? _p.unreadBadge : accent
    // Base hue of the mention-row wash (the timeline alphas it itself).
    // Accent by default — Storm redirects it to the mention rose, because a
    // 14% bolt wash over the deep timeline composites to a hueless brown
    // AND puts the reserved yellow on a passive row (review M1, §1).
    readonly property color mentionHighlight:    _p.mentionHighlight !== undefined
                                                 ? _p.mentionHighlight : accent
    // Mention red — the design themes carry their exact handoff hue; older
    // palettes fall back to the shared danger red.
    readonly property color mentionBadge:        _p.mention !== undefined
                                                 ? _p.mention : danger
    readonly property color undecryptableText:   textMuted
    // Highlight semantics (jumped-to message rows, active thread affordances).
    readonly property color pressedSurface:      selectedHover
    readonly property color messageHighlight:    selected
    readonly property color threadHighlight:     accent
    // Presence dots (design: online = accent-family green, away = yellow,
    // offline = the theme's muted ink — visibly "off" on every palette).
    readonly property color presenceOnline:      _p.online !== undefined
                                                 ? _p.online : success
    readonly property color presenceAway:        "#C9B23A"
    readonly property color presenceOffline:     textMuted

    // ---- Trust-card brand constants (SPEC 1r). ----
    // ---- Storm surface language (0.6.5). ----
    // The shared vocabulary of every menu, popover, picker, dialog and the
    // Settings surface. Since the Storm round made Storm a SELECTABLE
    // full-application theme (id 11), this namespace is theme-ROUTED, not
    // invariant: under Storm each token carries the §1 navy/bolt literal;
    // under every legacy theme it resolves to that theme's own semantic
    // equivalent, so menus and Settings follow the user's chosen theme
    // again. Consumers keep reaching for storm* by role and never branch on
    // the theme themselves. The trust card is the ONE deliberate invariant
    // exception and pins the raw _sto* literals below.
    readonly property color stormCanvas:        storm ? _stoCanvas : background
    readonly property color stormPanel:         storm ? _stoPanel : surface
    readonly property color stormInset:         storm ? _stoInset : inputBackground
    readonly property color stormDeep:          storm ? _stoDeep : background
    readonly property color stormBorder:        storm ? _stoBorder : border
    readonly property color stormBorderStrong:  storm ? _stoBorderStrong : borderStrong
    readonly property color stormSelection:     storm ? _stoSelection : hover
    // THE accent — active/selected/complete/primary ONLY (legacy: accent).
    readonly property color bolt:               storm ? _stoBolt : accent
    // Ink painted ON a bolt/accent fill (primary buttons, count pills).
    readonly property color boltInk:            storm ? _stoCanvas : accentText
    readonly property color stormText:          storm ? _stoText : textPrimary
    readonly property color stormTextSecondary: storm ? _stoTextSecondary : textSecondary
    readonly property color stormTextMuted:     storm ? _stoTextMuted : textMuted
    // Deliberately-dim non-body ink: section headers, footers and metadata
    // only — never sentence text (legacy: the disabled ink, same rule).
    readonly property color stormTextFaint:     storm ? _stoTextFaint : textDisabled
    readonly property color stormDanger:        storm ? _stoDanger : dangerInk
    readonly property color stormSuccess:       storm ? _stoSuccess : success
    readonly property color stormLink:          storm ? _stoLink : link
    // Derived soft treatments (§1: danger borders at 30% alpha, fills 10%).
    // Under legacy themes these resolve to the SHARED danger treatments so a
    // storm-namespace control and a themed control show one danger language.
    readonly property color stormDangerSoft:    storm ? Qt.alpha(_stoDanger, 0.10)
                                                      : dangerSoft
    readonly property color stormDangerBorder:  storm ? Qt.alpha(_stoDanger, 0.30)
                                                      : dangerBorder
    readonly property color stormBoltGlow:      Qt.alpha(bolt, 0.12)     // input focus halo
    readonly property color stormWatermark:     Qt.alpha(bolt, 0.12)     // hero-card bolt

    // The ONE deliberate theme-invariant exception: the verification/trust
    // surface always renders in Lightning's brand navy + yellow, in every
    // theme — the trust moment is the brand moment. ThemeTokensTest asserts
    // AA pairs for every INK role here (trustInk, trustYellow, trustMuted,
    // trustCaption, trustCaptionDim, trustVerifyInk); trustPending /
    // trustChainBorder are deliberately-dim non-text pending treatments
    // whose state is also carried by the caption ink and icon size.
    // Pinned to the raw _sto* literals — NOT the routed storm* tokens —
    // precisely so the trust card stays brand-navy under every theme now
    // that the storm* namespace follows the selected theme.
    readonly property color trustNavy:        _stoPanel
    readonly property color trustYellow:      _stoBolt
    readonly property color trustInk:         _stoText
    readonly property color trustMuted:       _stoTextMuted
    readonly property color trustChainBg:     _stoInset
    readonly property color trustChainBorder: _stoBorder
    readonly property color trustPending:     _stoBorderStrong
    readonly property color trustCaption:     "#AAB5E0"
    // Dim-but-AA pending caption (4.66:1 on trustChainBg; the mock's
    // #5C6BA3 computed 3.57:1 and failed normal-text AA).
    readonly property color trustCaptionDim:  "#6F7EB6"
    readonly property color trustVerifyInk:   "#C9D2F2"

    // Deterministic initials-avatar palette from the design handoff; shared
    // by every theme so a user or room keeps one colour everywhere.
    readonly property var avatarPalette: [
        "#2F8F5B", "#A3542F", "#6D5BD0", "#3A6EA5", "#B04A7E",
        "#C9662A", "#4A8F6D", "#B3823A", "#A05A92"
    ]
    // The ONE identity hash behind avatar fills and sender-name inks, so a
    // user's avatar disc and name label always agree on the hue family.
    // (Formerly private to Avatar.qml; lifted here so name colouring cannot
    // drift out of sync with it.)
    function identityIndex(key) {
        var h = 0
        for (var i = 0; i < key.length; ++i)
            h = ((h << 5) - h + key.charCodeAt(i)) | 0
        return Math.abs(h) % avatarPalette.length
    }
    function avatarColor(key) { return avatarPalette[identityIndex(key)] }
    // Sender-name text inks, hue-matched index-for-index to avatarPalette
    // but tuned as TEXT ink: the avatar fills are mid-tone disc colours
    // sized for white initials and fail normal-text contrast as ink in
    // both modes. Every value below is >= 4.5:1 (WCAG AA, normal text)
    // against every background / card / elevated-card / OTHER-bubble
    // token of its mode's themes — the identity header renders on the
    // other party's bubble in Bubbles-for-DMs mode, and Warm's #EDE2CE
    // bubble is the binding constraint for the light greens (an earlier
    // #1F7A49/#2F7455 pair computed 4.16/4.37 there and failed AA). The
    // theme-token test enforces this matrix; do not edit an ink without
    // re-running it.
    readonly property var _nameInksDark: [
        "#79D6A4", "#F2B08D", "#C0B4F5", "#9CC4EF", "#F1ACCD",
        "#F2B183", "#93D5B4", "#E2C387", "#E5ACD7"
    ]
    readonly property var _nameInksLight: [
        "#196B3F", "#96481F", "#5B48C0", "#2F5E93", "#A03A6E",
        "#9C4A12", "#27654A", "#7D5A17", "#8E4680"
    ]
    // Deterministic per-user display-name colour (Element-style identity
    // colouring). Falls back to the primary ink when there is no stable
    // key to hash.
    function userColor(key) {
        if (!key || key.length === 0)
            return textPrimary
        return (dark ? _nameInksDark : _nameInksLight)[identityIndex(key)]
    }

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
    readonly property int spacing10: 10
    readonly property int spacing12: 12
    readonly property int spacing14: 14
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
    // Media (images/video/GIF) corner radius — matches the card radius so media
    // reads as part of the message rather than pasted into the window.
    readonly property int radiusMedia: 12
    // v0.6.5 menu-language radii (SPEC §0 and chosen surfaces).
    readonly property int radiusChip:    radiusSm   // keycap chips (named role)
    readonly property int radiusControl: 7   // 28px action-bar buttons, emoji cells
    readonly property int radiusTile:    9   // icon tiles, footer action buttons
    readonly property int radiusThumb:  10   // GIF grid thumbnails
    readonly property int radiusOmnibox: 11  // new-conversation omnibox field
    readonly property int radiusCard:   14   // identity cards, quick-switcher modal

    // ---- Menu / popover language (v0.6.5, SPEC §0). ----
    readonly property int menuPadding:        spacing6   // popover internal padding
    readonly property int menuRadius:         radiusLg   // popover container corner
    readonly property int menuItemHeight:     32
    readonly property int menuItemRadius:     radiusMd
    readonly property int menuItemPadding:    spacing8   // row side padding
    readonly property int menuIconSize:       17         // storm §3.2 (was 18)
    readonly property int menuIconGap:        spacing10  // icon-to-label gap
    readonly property int menuContextHeaderHeight: 24    // storm §3.1 mono header
    readonly property int menuWidthDefault:   220
    readonly property int menuWidthMessage:   252        // SPEC 1a
    readonly property int menuWidthRoom:      196        // SPEC 1d
    readonly property int menuWidthFlyout:    150        // SPEC 1d notifications flyout
    readonly property int menuDividerVMargin: spacing6
    readonly property int menuDividerHMargin: spacing4
    // Accelerator keycap chips (MenuKeycap.qml).
    readonly property color keycapBackground:     cardElevated
    readonly property color keycapBorder:         borderStrong
    readonly property color keycapText:           textMuted
    readonly property int   keycapPaddingH:       5   // row-level chips (SPEC §0 1×5)
    readonly property int   keycapPaddingV:       1
    readonly property int   keycapHeaderPaddingH: 6   // header-level chips (ESC, 2×6)
    readonly property int   keycapHeaderPaddingV: 2
    // Section labels and mono identity strings inside popovers (SPEC §0).
    // §0 asks for textDisabled on section labels, but every preset's
    // disabled ink sits below WCAG AA for normal text and these labels are
    // load-bearing (ROOMS/PEOPLE, picker headings) — they ride the
    // AA-asserted muted ink instead. textDisabled stays reserved for
    // genuinely disabled controls, which AA exempts.
    readonly property color sectionLabelColor: textMuted
    readonly property color monoIdentityColor: textMuted
    // Letter-spacing is pixels in QML, not em; converted at the design size.
    readonly property real  trackingSection:   0.8   // .08em at 10px
    readonly property real  trackingMono:      1.2   // .12em at 10px
    // Storm mono headers (§2: .16–.18em, resolved at the ~9.5px header size).
    readonly property real  trackingStorm:     1.6
    // Emoji grid cells shared by the picker body and the quick-react strip.
    readonly property int   emojiCellSize:  32
    readonly property int   emojiGlyphSize: 19

    // ---- Layout constraints. ----
    // Maximum width of a timeline message row's content (text, media, mention
    // highlight). Keeps long messages readable and the timeline balanced on wide
    // desktop windows instead of stretching edge to edge; narrow windows shrink
    // below it responsively.
    readonly property int timelineContentMaxWidth: 760

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
    // v0.6.5 menu-language sizes. font.pixelSize is an int in Qt, so the
    // spec's half-pixel sizes are resolved to whole numbers (rounded up so
    // small ink stays legible). Chrome sizes — never wrapped in scaled().
    readonly property int fontMicro:     9   // trust captions, GIF badge, role chips
    readonly property int fontChip:      10  // keycaps, section labels, ACTIVE chip
    readonly property int fontMonoXS:    11  // mono identity strings (10.5–11 in spec)
    readonly property int fontMonoSm:    12  // footer hints, status lines (11.5–12)
    readonly property int fontResult:    14  // result-row titles (13.5 in spec)
    readonly property int fontQuery:     15  // search/omnibox query, card names
    readonly property int fontTrustName: 17  // trust-card display name
    readonly property int fontNavTitle:  17  // Settings-nav pane title (SPEC 1v, 17px/800)

    // ---- Font families. ----
    // The selectable bundled UI families ship with Lightning (data/fonts,
    // loaded in main.cpp); Main.qml pushes the persisted per-account
    // selection in here, and the family lists below stay as graceful
    // fallbacks for stripped-down builds. Mono, icon, and emoji roles are
    // deliberately not affected by the UI font selection.
    property string uiFont:          "Manrope"
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
    // v0.6.5: brand face — originally the trust surface only (SPEC 1r); the
    // Storm language (SPEC-storm-language §2) extends it to every menu item
    // label, title and button. Still never a timeline/body face, and still
    // deliberately absent from the Settings font choices.
    readonly property string brandFont:         "Space Grotesk"
    readonly property var    brandFontFamilies: [
        "Space Grotesk",
        "Manrope",
        "sans-serif"
    ]
    // Storm role alias: the menu-surface label face (§2).
    readonly property string menuFont:          brandFont
}
