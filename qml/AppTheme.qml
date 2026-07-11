pragma Singleton
import QtQuick

// v0.5.2: design-token foundation for the Lightning UI redesign.
//
// This singleton is the SOLE source of truth for colors, spacing, radii,
// font sizes, and font families. Consumers should reach only for the
// semantic tokens below (background, textPrimary, accent, spacing8, ...)
// rather than hardcoding hex or magic-number pixel sizes.
//
// Palette targets:
//   Light — background #F6F8FC, sidebar #FFFFFF, primary #4F7CFF,
//           hover #EDF3FF, selected #DCE8FF, text #1E293B / #64748B / #94A3B8,
//           border #E2E8F0.
//   Dark  — background #0F172A, sidebar #111827, cards #1E293B,
//           primary #4F7CFF, hover #243B6B, selected #2D4FA8,
//           text #F8FAFC / #CBD5E1 / #94A3B8, border #334155.
//
// Legacy aliases (background, surface, textMuted, ownBubble, etc.) are
// preserved so existing QML compiles unchanged; they resolve to the new
// palette values.
QtObject {
    id: root

    // 0 = System, 1 = Light, 2 = Dark. Kept in sync with
    // SettingsManager::Theme by an external Binding in Main.qml.
    property int mode: 0
    readonly property bool dark: mode === 2

    // ---- Raw palette (kept private-ish; prefer semantic aliases). ----
    // v0.5.9: slate/navy neutral base; the light theme is its own palette,
    // not inverted dark values. Light muted text darkened from #94A3B8 to
    // keep timestamps/labels at ≥ 4.5:1 on the light background.
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
    // Outgoing bubbles carry body text, so they use a deeper blue than the
    // control accent: white body ≈ 6.2:1, muted meta ink ≈ 4.9:1.
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

    // ---- Semantic aliases (preferred). ----
    readonly property color background:          dark ? _bgDark             : _bgLight
    readonly property color windowBackground:    background
    readonly property color sidebar:             dark ? _sidebarDark        : _sidebarLight
    readonly property color navBackground:       sidebar
    readonly property color panelBackground:     sidebar
    readonly property color surface:             dark ? _cardDark           : _cardLight
    readonly property color card:                surface
    readonly property color cardElevated:        dark ? _cardElevatedDark   : _cardElevatedLight
    readonly property color surfaceElevated:     cardElevated
    readonly property color hover:               dark ? _hoverDark          : _hoverLight
    readonly property color selected:            dark ? _selectedDark       : _selectedLight
    readonly property color selectedHover:       dark ? _selectedHoverDark  : _selectedHoverLight
    readonly property color selectedText:        dark ? _selectedTextDark   : _selectedTextLight
    readonly property color accent:              _accentBlue
    readonly property color accentHover:         _accentBlueHover
    readonly property color accentPressed:       _accentBluePressed
    readonly property color accentText:          "#FFFFFF"
    readonly property color success:             _accentGreen
    readonly property color warning:             _accentWarning
    readonly property color danger:              _accentDanger
    readonly property color dangerText:          "#FFFFFF"
    readonly property color info:                _accentInfo
    readonly property color textPrimary:         dark ? _textPrimaryDark    : _textPrimaryLight
    readonly property color textSecondary:       dark ? _textSecondaryDark  : _textSecondaryLight
    readonly property color textMuted:           dark ? _textMutedDark      : _textMutedLight
    readonly property color textDisabled:        dark ? _textDisabledDark   : _textDisabledLight
    readonly property color border:              dark ? _borderDark         : _borderLight
    readonly property color borderSubtle:        border
    readonly property color borderStrong:        dark ? _borderStrongDark   : _borderStrongLight
    readonly property color separator:           border
    readonly property color inputBackground:     dark ? _inputBgDark        : _inputBgLight
    readonly property color inputBorder:         border
    // A focus ring readable in both themes — accent blue at high opacity.
    readonly property color focusRing:           _accentBlue
    // Full-window scrim behind dialogs/viewers.
    readonly property color overlayScrim:        "#80000000"
    readonly property color codeBlock:           dark ? _codeBlockDark      : _codeBlockLight

    // Message-bubble semantics.
    readonly property color ownMessageBubble:    _outgoingBubbleBlue
    readonly property color ownBubble:           ownMessageBubble
    readonly property color ownBubbleText:       "#FFFFFF"
    // Secondary ink on the accent bubble (timestamps, reply previews);
    // ≈ 4.6:1 on the accent blue.
    readonly property color onAccentMuted:       "#DCE4FF"
    readonly property color otherMessageBubble:  dark ? _cardElevatedDark   : _hoverLight
    readonly property color otherBubble:         otherMessageBubble
    readonly property color otherBubbleText:     textPrimary
    readonly property color incomingBubble:      otherMessageBubble
    readonly property color outgoingBubble:      ownMessageBubble
    // Subtle darkening overlays used inside bubbles (reply quote, file box)
    // — alpha blacks so they work on both bubble colours.
    readonly property color bubbleOverlay:       "#26000000"
    readonly property color bubbleOverlaySubtle: "#0F000000"
    // Reactions and badges.
    readonly property color reactionBackground:  cardElevated
    readonly property color reactionSelectedBackground: selected
    readonly property color unreadBadge:         accent
    readonly property color mentionBadge:        danger
    // Undecryptable placeholder — deliberately muted, italic in
    // MessageDelegate, so it reads as "waiting" rather than "error".
    readonly property color undecryptableText:   textMuted

    // ---- Legacy aliases retained for existing QML. ----
    // surfaceAlt was the old "elevated" tone; map onto cardElevated.
    readonly property color surfaceAlt:          cardElevated
    // Old "text" role → new textPrimary.
    readonly property color text:                textPrimary
    // Old "muted" (undecryptable/notice) → new textMuted.
    readonly property color muted:               textMuted
    // Old "selectedBg" → new selected.
    readonly property color selectedBg:          selected
    // Old error semantic is danger.
    readonly property color error:               _accentDanger

    // ---- Spacing scale. ----
    // Both the numeric names (spacing2..spacing24) from the redesign
    // spec AND the legacy XS/S/M/L/XL sizes stay valid so existing QML
    // compiles unchanged.
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
    // Legacy "radius" == md.
    readonly property int radius:     radiusMd

    // ---- Typography. ----
    // fontSizeS raised from 12 to 13 to match the redesign spec's
    // "13px secondary" rule. Existing consumers that read fontSizeS
    // (timestamps, meta rows) get a single-pixel nudge but no layout
    // breakage.
    readonly property int fontSizeXS:        11
    readonly property int fontSizeS:         13
    readonly property int fontSizeM:         14
    readonly property int fontSizeRoom:      16
    readonly property int fontSizeHeader:    18
    readonly property int fontSizePageTitle: 24
    // Legacy L/XL kept.
    readonly property int fontSizeL:         fontSizeRoom
    readonly property int fontSizeXL:        fontSizeHeader

    // v0.5.9 semantic typography roles (one scale, desktop-density).
    readonly property int fontPageTitle:     fontSizePageTitle // 24
    readonly property int fontSectionTitle:  fontSizeHeader    // 18
    readonly property int fontRoomTitle:     fontSizeRoom      // 16
    readonly property int fontBody:          fontSizeM         // 14
    readonly property int fontSecondary:     fontSizeS         // 13
    readonly property int fontMessageSender: 12
    readonly property int fontCaption:       fontSizeXS        // 11
    readonly property int fontMono:          fontSizeS

    // ---- Font families. ----
    // Consumers using `Label { font.families: AppTheme.uiFontFamilies }`
    // get proper fallback; consumers using `font.family: AppTheme.uiFont`
    // get the primary family, with Qt falling back to system sans on
    // its own when missing.
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
