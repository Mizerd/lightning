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
// v0.5.11 themes (SettingsManager::Theme):
//   0 System — follows the platform colour scheme (Light or Midnight Blue)
//   1 Light
//   2 Dark        (retained alias → Midnight Blue palette)
//   3 Graphite    (neutral dark grey)
//   4 Midnight Blue (the original slate/navy dark palette)
//   5 Nord
//   6 Purple Dusk
//
// Every preset provides the full set of semantic values; components never
// branch on the theme themselves. The raw `_xxxLight` / `_xxxDark` colour
// literals are retained (the theme-token/contrast test parses them by name)
// and the Light + Midnight palettes are byte-for-byte the previous values,
// so no existing view changes appearance under Light/Dark.
QtObject {
    id: root

    // Raw SettingsManager::Theme enum int, pushed in from Main.qml.
    property int mode: 0
    // System dark-mode hint from the platform (QStyleHints::colorScheme),
    // pushed in from Main.qml via AppController.systemDarkMode.
    property bool systemDark: false

    // Resolve System (0) to Light or Midnight Blue based on the platform.
    readonly property int effectiveTheme: mode === 0
                                          ? (systemDark ? 4 : 1)
                                          : mode
    // Any non-Light preset is a dark surface (used by overlay chrome).
    readonly property bool dark: effectiveTheme !== 1

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
    readonly property color _norTextMuted:     "#A6ADBB"
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
    readonly property var _p: {
        switch (effectiveTheme) {
        case 1:  return _light
        case 2:  return _midnight   // Dark alias
        case 3:  return _graphite
        case 4:  return _midnight   // Midnight Blue
        case 5:  return _nord
        case 6:  return _purple
        default: return _light
        }
    }

    // ---- Semantic aliases (preferred). ----
    readonly property color background:          _p.background
    readonly property color windowBackground:    background
    readonly property color sidebar:             _p.sidebar
    readonly property color navBackground:       sidebar
    readonly property color panelBackground:     sidebar
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
    readonly property color accentText:          "#FFFFFF"
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
    readonly property color mentionBadge:        danger
    readonly property color undecryptableText:   textMuted

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
    readonly property string uiFont:          "Inter"
    readonly property var    uiFontFamilies:  [
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
