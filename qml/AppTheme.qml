pragma Singleton
import QtQuick

// Design tokens: the single source of truth for colours, spacing, radii, font
// sizes and families. Views use the semantic tokens below and never branch on
// the theme themselves.
//
// Themes (SettingsManager::Theme):
//   0 System          — follows the platform colour scheme
//   1 Lightning Light
//   2 Lightning Dark  (cool near-black)
//   3 Graphite        (neutral dark grey)
//   4 Midnight        (slate/navy dark)
//   5 Nordic          (polar-night surfaces, frost accent)
//   6 Purple Dusk     (deep violet surfaces)
//   7 Warm            (light cream surfaces, amber accent)
//   8 Moss Light      (warm neutrals, moss accent)
//   9 Indigo Night    (near-black, indigo accent)
//  10 Deep Teal       (deep teal surfaces and accent)
//  11 Storm           (brand theme: deep navy surfaces, bolt yellow)
//
// The raw `_xxxLight` / `_xxxDark` literals are parsed by name by the
// theme-token/contrast test.
QtObject {
    id: root

    // Raw SettingsManager::Theme value, pushed in from Main.qml.
    property int mode: 0
    // Content text scale (settings.textScale / 100), pushed in from Main.qml.
    // Despite the name, `scaled()` also drives geometry, e.g. the Spaces rail's
    // widths, indents, tiles and badges. Whole-window scaling is the separate
    // Interface zoom setting (QT_SCALE_FACTOR).
    property real textScale: 1.0
    // Includes the UI font's optical correction so text size and font choice
    // stay independent (Manrope, the default, has factor 1.0).
    function scaled(px) { return Math.round(px * textScale * uiFontOptical) }

    // Shared height of the top header strip (room list, room header, side
    // panel) so the rule beneath them lines up across the whole window.
    readonly property int headerBandHeight: 60
    // Platform dark-mode hint (QStyleHints::colorScheme), pushed in from
    // Main.qml.
    property bool systemDark: false
    // Custom theme (id 12): a sparse role -> "#RRGGBB" map laid over
    // `customBase`'s palette. CustomThemeStore::sanitize() already filters it;
    // _custom re-checks because the config file is user-editable.
    property var customOverrides: ({})
    property int customBase: 11

    // Reduced-motion hint for skeletons and decorative animation, bound from
    // Main.qml. Defaults to false so a harness that never binds it still
    // animates.
    property bool reducedMotion: false

    // System (0) resolves to Moss Light or Indigo Night. Explicitly stored
    // theme ids are never rerouted.
    readonly property int effectiveTheme: mode === 0
                                          ? (systemDark ? 9 : 8)
                                          : mode
    // Coerce a palette value to a color. The custom palette stores "#RRGGBB"
    // strings, which have no .r/.g/.b, so arithmetic on a palette entry must go
    // through here (a bare relativeLuminance(_p.background) would be NaN).
    function _asColor(v) { return typeof v === "string" ? Qt.color(v) : v }

    // WCAG relative luminance. Only used to classify a custom palette as light
    // or dark.
    function relativeLuminance(c) {
        function lin(v) {
            return v <= 0.03928 ? v / 12.92
                                : Math.pow((v + 0.055) / 1.055, 2.4)
        }
        return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b)
    }

    // Presets are classified by id. A custom theme is classified by its own
    // background, since a light palette can sit on a dark base.
    readonly property bool dark: effectiveTheme === 12
                                 ? relativeLuminance(_asColor(_p.background)) < 0.18
                                 : (effectiveTheme !== 1 && effectiveTheme !== 7
                                    && effectiveTheme !== 8)
    // True while Storm (11) is the effective palette; used only to route the
    // storm* tokens inside this singleton.
    readonly property bool storm: effectiveTheme === 11

    // ---- Raw palette literals (the token test reads these by name) ----
    readonly property color _bgLight:            "#D3E1F2"
    readonly property color _railLight:            "#CCD7E5"
    readonly property color _bgDark:             "#09182C"
    readonly property color _sidebarLight:       "#DEEBFD"
    readonly property color _sidebarDark:        "#1B263B"
    // Slightly tinted rather than pure white so cards don't read as holes in
    // the pale-blue canvas; still the lightest surface.
    readonly property color _cardLight:          "#F5F9FE"
    readonly property color _cardDark:           "#273246"
    // One clear rung below the card and above the canvas.
    readonly property color _cardElevatedLight:  "#E4EEFA"
    readonly property color _cardElevatedDark:   "#333D50"
    readonly property color _hoverLight:         "#CEE3FA"
    readonly property color _hoverDark:          "#2B4A6A"
    readonly property color _selectedLight:      "#AAD4FF"
    readonly property color _selectedDark:       "#13588B"
    readonly property color _selectedHoverLight: "#94C7F9"
    readonly property color _selectedHoverDark:  "#286599"
    readonly property color _selectedTextLight:  "#1E293B"
    readonly property color _selectedTextDark:   "#F8FAFC"
    readonly property color _accentBlue:         "#1D57FF"
    readonly property color _accentBlueHover:    "#3569FF"
    readonly property color _accentBluePressed:  "#214ED1"
    readonly property color _outgoingBubbleBlue: "#2D4BAB"
    readonly property color _accentGreen:        "#22C55E"
    readonly property color _accentWarning:      "#E5A23C"
    readonly property color _accentDanger:       "#DC2626"
    readonly property color _accentInfo:         "#0CAEF6"
    readonly property color _textPrimaryLight:   "#1E293B"
    readonly property color _textPrimaryDark:    "#F8FAFC"
    readonly property color _textSecondaryLight: "#4C5661"
    readonly property color _textSecondaryDark:  "#CBD5E1"
    readonly property color _textMutedLight:     "#525C68"
    readonly property color _textMutedDark:      "#94A3B8"
    readonly property color _textDisabledLight:  "#A8B3C4"
    readonly property color _textDisabledDark:   "#64748B"
    readonly property color _borderLight:        "#C4D2E7"
    readonly property color _borderDark:         "#394859"
    readonly property color _borderStrongLight:  "#7A94B6"
    readonly property color _borderStrongDark:   "#526172"
    readonly property color _inputBgLight:       "#E8F1FB"
    readonly property color _inputBgDark:        "#0E1F30"
    readonly property color _codeBlockLight:     "#CDDFF3"
    readonly property color _lightLink:            "#005FB5"
    readonly property color _codeBlockDark:      "#000E1F"
    readonly property color _midLink:              "#8CC3FF"

    // Lightning Dark — cool near-black, calmer than the navy Midnight.
    readonly property color _dkBg:            "#0D1117"
    readonly property color _dkSidebar:       "#1B242F"
    readonly property color _dkCard:          "#2A3140"
    readonly property color _dkCardElevated:  "#303E52"
    readonly property color _dkHover:         "#3D4964"
    readonly property color _dkSelected:      "#3C527E"
    readonly property color _dkSelectedHover: "#4B5E94"
    readonly property color _dkSelectedText:  "#F0F4FA"
    readonly property color _dkTextPrimary:   "#E9EEF6"
    readonly property color _dkTextSecondary: "#B7C2D4"
    readonly property color _dkTextMuted:     "#98A5BB"
    readonly property color _dkTextDisabled:  "#7A889F"
    readonly property color _dkBorder:        "#4C596D"
    readonly property color _dkBorderStrong:  "#5D6B87"
    readonly property color _dkInputBg:       "#35373B"
    readonly property color _dkCodeBlock:     "#212224"
    // Lightning Dark's link ink. The shared _accentBlue is below AA here; this
    // clears 4.96:1 on cardElevated. _accentBlue stays as Lightning Light needs
    // it.
    readonly property color _dkLink:          "#9EAAFC"

    // Graphite — neutral dark grey.
    readonly property color _graBg:            "#141417"
    readonly property color _graSidebar:       "#1D1E21"
    readonly property color _graCard:          "#27272B"
    readonly property color _graCardElevated:  "#38383D"
    readonly property color _graHover:         "#4B4B53"
    readonly property color _graSelected:      "#55586C"
    readonly property color _graSelectedHover: "#61667B"
    readonly property color _graSelectedText:  "#F5F5F7"
    readonly property color _graTextPrimary:   "#F5F5F7"
    readonly property color _graTextSecondary: "#C7C7CC"
    readonly property color _graTextMuted:     "#9A9AA2"
    readonly property color _graTextDisabled:  "#79787F"
    readonly property color _graBorder:        "#43444A"
    readonly property color _graBorderStrong:  "#5C5C64"
    readonly property color _graInputBg:       "#151518"
    readonly property color _graCodeBlock:     "#0D0D11"
    readonly property color _graLink:              "#A8AFF9"
    readonly property color _graAccent:        "#2E6EEB"
    readonly property color _graAccentHover:   "#4680ED"
    readonly property color _graAccentPressed: "#305EB8"
    readonly property color _graOwnBubble:     "#314D9A"
    readonly property color _graOtherBubble:   "#2D2D32"

    // Nord — polar-night surfaces, frost accent.
    readonly property color _norBg:            "#2E3440"
    readonly property color _norRail:              "#1E232E"
    readonly property color _norSidebar:       "#262C38"
    readonly property color _norCard:          "#3B4252"
    readonly property color _norCardElevated:  "#444D61"
    readonly property color _norHover:         "#4C566A"
    readonly property color _norSelected:      "#4A6285"
    readonly property color _norSelectedHover: "#587197"
    readonly property color _norSelectedText:  "#ECEFF4"
    readonly property color _norTextPrimary:   "#ECEFF4"
    readonly property color _norTextSecondary: "#D8DEE9"
    readonly property color _norTextMuted:     "#ABB2C0"
    readonly property color _norTextDisabled:  "#949CAC"
    readonly property color _norBorder:        "#464F62"
    readonly property color _norBorderStrong:  "#5A6478"
    readonly property color _norInputBg:       "#21262F"
    readonly property color _norCodeBlock:     "#292E39"
    readonly property color _norLink:              "#9DE4FF"
    readonly property color _norAccent:        "#5E81AC"
    readonly property color _norAccentHover:   "#6C8FBA"
    readonly property color _norAccentPressed: "#4E6E96"
    readonly property color _norOwnBubble:     "#3B5A80"
    readonly property color _norOtherBubble:   "#3F4757"

    // Purple Dusk — deep violet surfaces.
    readonly property color _purBg:            "#17142A"
    readonly property color _purSidebar:       "#2B2642"
    readonly property color _purCard:          "#3C3455"
    readonly property color _purCardElevated:  "#4A4168"
    readonly property color _purHover:         "#56487E"
    readonly property color _purSelected:      "#594992"
    readonly property color _purSelectedHover: "#6B57BB"
    readonly property color _purSelectedText:  "#F3EEFF"
    readonly property color _purTextPrimary:   "#F3EEFF"
    readonly property color _purTextSecondary: "#D6CCF0"
    readonly property color _purTextMuted:     "#B5ABD6"
    readonly property color _purTextDisabled:  "#988DC1"
    readonly property color _purBorder:        "#5D537E"
    readonly property color _purBorderStrong:  "#716598"
    readonly property color _purInputBg:       "#150F32"
    readonly property color _purCodeBlock:     "#0D0819"
    // Purple Dusk's link ink. The accent can't serve: white-on-accent (3:1)
    // caps its luminance below what an AA link needs on these surfaces. Only
    // 3.41:1 inside the own bubble, as with Storm.
    readonly property color _purLink:          "#C7B2FC"
    readonly property color _purAccent:        "#8F73E9"
    readonly property color _purAccentHover:   "#9D7EEA"
    readonly property color _purAccentPressed: "#7D5ED6"
    readonly property color _purOwnBubble:     "#6452A4"
    readonly property color _purOtherBubble:   "#3C3261"

    // Warm — light cream surfaces with an amber accent.
    readonly property color _warBg:            "#EBDCC6"
    readonly property color _warRail:              "#E3D2BB"
    readonly property color _warSidebar:       "#F6E6D0"
    readonly property color _warCard:          "#FFFDF8"
    readonly property color _warCardElevated:  "#FBF1E3"
    readonly property color _warHover:         "#EEDFC6"
    readonly property color _warSelected:      "#E7CCA8"
    readonly property color _warSelectedHover: "#DABC92"
    readonly property color _warSelectedText:  "#3B3428"
    readonly property color _warTextPrimary:   "#3B3428"
    readonly property color _warTextSecondary: "#675944"
    readonly property color _warTextMuted:     "#6A604D"
    readonly property color _warTextDisabled:  "#B3A78F"
    readonly property color _warBorder:        "#DCD0B8"
    readonly property color _warBorderStrong:  "#C2B394"
    readonly property color _warInputBg:       "#F5ECDE"
    readonly property color _warCodeBlock:     "#E7DAC5"
    readonly property color _warLink:              "#00598B"
    readonly property color _warAccent:        "#A34C00"
    readonly property color _warAccentHover:   "#BC5A05"
    readonly property color _warAccentPressed: "#8A3F00"
    readonly property color _warOwnBubble:     "#7A3A05"
    readonly property color _warOtherBubble:   "#FFE0CC"

    // Moss Light — design-handoff light theme (option 1a).
    readonly property color _mosBg:            "#D2E5D6"
    readonly property color _mosRail:          "#C8DBCD"
    readonly property color _mosSidebar:       "#DCEEE2"
    // See _cardLight.
    readonly property color _mosCard:          "#F6FBF7"
    readonly property color _mosCardElevated:  "#EAF8ED"
    readonly property color _mosHover:         "#D4E6D8"
    readonly property color _mosSelected:      "#D1F1E5"
    readonly property color _mosSelectedHover: "#AFDACA"
    readonly property color _mosSelectedText:  "#006242"
    readonly property color _mosTextPrimary:   "#161D18"
    readonly property color _mosTextSecondary: "#4E5A51"
    readonly property color _mosTextMuted:     "#57625A"
    readonly property color _mosTextDisabled:  "#8D9890"
    readonly property color _mosBorder:        "#D2E2D6"
    readonly property color _mosBorderStrong:  "#A0B2A4"
    readonly property color _mosInputBg:       "#E9F2EB"
    readonly property color _mosCodeBlock:     "#CBDFD0"
    readonly property color _mosLink:              "#00734E"
    readonly property color _mosAccent:        "#007757"
    readonly property color _mosAccentHover:   "#00654A"
    readonly property color _mosAccentPressed: "#005440"
    readonly property color _mosAccentSoft:    "#D1F1E5"
    readonly property color _mosAccentBorder:  "#7DBAA0"
    readonly property color _mosOwnBubble:     "#0D6E55"
    readonly property color _mosOtherBubble:   "#D9EADE"
    readonly property color _mosMention:       "#E04848"

    // Indigo Night — design-handoff dark theme (option 2a).
    readonly property color _indBg:            "#1F1D26"
    readonly property color _indRail:          "#0E0E14"
    readonly property color _indSidebar:       "#292632"
    readonly property color _indCard:          "#32303D"
    readonly property color _indCardElevated:  "#3D3A4A"
    readonly property color _indHover:         "#484455"
    readonly property color _indSelected:      "#3D415F"
    readonly property color _indSelectedHover: "#474C6E"
    readonly property color _indSelectedText:  "#C3C5FF"
    readonly property color _indTextPrimary:   "#E8E8EF"
    readonly property color _indTextSecondary: "#A4A6B8"
    readonly property color _indTextMuted:     "#9E9BA6"
    readonly property color _indTextDisabled:  "#6F7183"
    readonly property color _indBorder:        "#423E4E"
    readonly property color _indBorderStrong:  "#5A5768"
    readonly property color _indInputBg:       "#24212C"
    readonly property color _indCodeBlock:     "#100C18"
    // A blue rather than violet so links don't share a hue with the selection;
    // at least 4.79:1 on every Indigo surface.
    readonly property color _indLink:              "#93C5FD"
    readonly property color _indAccent:        "#4A4EED"
    readonly property color _indAccentHover:   "#5C61F0"
    readonly property color _indAccentPressed: "#4043CC"
    readonly property color _indAccentSoft:    "#25253D"
    readonly property color _indAccentBorder:  "#303057"
    readonly property color _indOwnBubble:     "#3E409E"
    readonly property color _indOtherBubble:   "#32303D"
    readonly property color _indMention:       "#E5677A"
    readonly property color _indOnline:        "#63D6A3"

    // Deep Teal. The accent is bright, so accent fills use dark ink.
    readonly property color _teaBg:            "#022323"
    readonly property color _teaRail:          "#000909"
    readonly property color _teaSidebar:       "#0C3031"
    readonly property color _teaCard:          "#163D3D"
    readonly property color _teaCardElevated:  "#214B4B"
    readonly property color _teaHover:         "#225056"
    readonly property color _teaSelected:      "#1C544E"
    readonly property color _teaSelectedHover: "#216059"
    readonly property color _teaSelectedText:  "#DEF5F0"
    readonly property color _teaTextPrimary:   "#E6ECEC"
    readonly property color _teaTextSecondary: "#B9C8C8"
    readonly property color _teaTextMuted:     "#8FA5A8"
    readonly property color _teaTextDisabled:  "#5F7A7E"
    readonly property color _teaBorder:        "#284E4E"
    readonly property color _teaBorderStrong:  "#406666"
    readonly property color _teaInputBg:       "#082929"
    readonly property color _teaCodeBlock:     "#000C0C"
    readonly property color _teaLink:              "#61C4C7"
    readonly property color _teaAccent:        "#27C2AD"
    readonly property color _teaAccentHover:   "#3FD2BE"
    readonly property color _teaAccentPressed: "#1EA593"
    readonly property color _teaAccentSoft:    "#13403D"
    readonly property color _teaAccentBorder:  "#1F4A44"
    readonly property color _teaAccentText:    "#062A25"
    readonly property color _teaOwnBubble:     "#1C4A43"
    readonly property color _teaOtherBubble:   "#163D3D"
    readonly property color _teaMention:       "#E5677A"

    // Storm (id 11): deep navy surfaces, bolt-yellow accent. These literals are
    // also what the storm* namespace carries when Storm is active.
    //
    // Adjacent surfaces step by ~1.25:1 (WCAG). cardElevated and selection
    // share a lightness and differ by hue only: elevation and state are
    // different meanings. Do not push the rungs further apart: ThemeTokensTest
    // requires every dark display-name ink to clear 4.5:1 on canvas, panel,
    // selection and cardElevated, which caps those surfaces at luminance
    // 0.0757. If a surface change breaks the identity matrix, move the surface
    // back, not the inks.
    //
    // Canvas is the room/people list ground. It can't go much darker without
    // merging into _stoDeep on both sides of it.
    readonly property color _stoCanvas:        "#121655"
    // Storm blues are tuned for Lab chroma at fixed luminance, so saturation
    // can change without moving any asserted contrast pair. HLS saturation is
    // not perceptual chroma. Selection, hover and borders are deliberately less
    // saturated. cardElevated and selection keep distinct hues (dE ~25) because
    // tint is the only thing separating them.
    readonly property color _stoPanel:         "#202473"
    readonly property color _stoInset:         "#0A112E"
    readonly property color _stoDeep:          "#02051D"
    readonly property color _stoBorder:        "#303C80"
    readonly property color _stoBorderStrong:  "#434F9D"
    readonly property color _stoSelection:     "#283097"
    // Elevation rung above the panel: reaction pills, keycaps, raised cards.
    readonly property color _stoCardElevated:  "#22397E"
    // Row-hover lift. Translucent so one value gives a consistent ~1.25:1 lift
    // over the timeline, room list and menu rows.
    readonly property color _stoHover:         "#6469BF"
    // Reaction-pill surface, distinct from cardElevated.
    readonly property color _stoReaction:      "#24317B"
    readonly property color _stoBolt:          "#FFD447"
    // Ink painted on a bolt or link fill.
    readonly property color _stoBoltInk:       "#0A0F24"
    readonly property color _stoText:          "#F2F4FF"
    readonly property color _stoTextSecondary: "#C9D2F2"
    readonly property color _stoTextMuted:     "#9CA3D2"
    readonly property color _stoTextFaint:     "#7881B5"
    readonly property color _stoDanger:        "#FFA7AF"
    readonly property color _stoSuccess:       "#63D6A3"
    readonly property color _stoLink:          "#9295F5"
    // Storm derivatives the spec table lacks: bolt hover/pressed steps, the
    // own-bubble navy, and the mention red (stormDanger is too light under the
    // pill's white ink).
    readonly property color _stoAccentHover:   "#FFDF6E"
    readonly property color _stoAccentPressed: "#E9BC2F"
    readonly property color _stoOwnBubble:     "#2B3B8D"
    readonly property color _stoSelectedHover: "#3037AD"
    readonly property color _stoMention:       "#E5677A"

    // Ink on accent fills for palettes without their own accentText (read by
    // name by the contrast test).
    readonly property color _onAccent:         "#FFFFFF"

    // ---- Status inks, per mode ---- `danger` fills (_accentDanger) and status
    // inks are separate roles: the fill red is below AA as text on dark
    // surfaces. Dark inks need luminance >= 0.4977 (Nordic's elevated card);
    // light inks need <= 0.1319 (Warm's other-bubble). They must also stay
    // readable on a 14% tint of themselves, as chips paint them
    // (chipInkOnItsOwnFillIsReadable).
    readonly property color _dangerInkLight:   "#9F2A30"
    readonly property color _dangerInkDark:    "#FFA7AF"
    readonly property color _warnInkLight:     "#814A00"
    readonly property color _warnInkDark:      "#FFAD67"
    readonly property color _okInkLight:       "#056435"
    readonly property color _okInkDark:        "#63D6A3"
    readonly property color _infoInkLight:     "#005994"
    readonly property color _infoInkDark:      "#73CEFC"
    // Destructive fill steps darken on interaction, unlike the accent: #DC2626
    // is only 4.83:1 under white text, so a lighter hover would fall below AA.
    readonly property color _dangerFillHover:  "#C81F1F"
    readonly property color _dangerFillPressed:"#A81919"
    // Away presence. A dot, so held to the 3:1 graphical bar, but it must stay
    // clearly distinct from the Storm bolt (CIE76 dE >= 38) or an away contact
    // reads as highlighted.
    readonly property color _awayLight:        "#BA671B"
    readonly property color _awayDark:         "#F59349"

    // ---- Reaction-pill surfaces ----
    // One visible step off each theme's message surface, keeping the secondary
    // ink at >= 4.6:1.
    readonly property color _lightReaction:    "#DCE9F8"
    readonly property color _dkReaction:       "#464C57"
    readonly property color _graReaction:      "#38434C"
    readonly property color _midReaction:      "#394A5E"
    readonly property color _norReaction:      "#2F596A"
    readonly property color _purReaction:      "#444058"
    readonly property color _warReaction:      "#E4D5BD"
    readonly property color _mosReaction:      "#D0E3D5"
    readonly property color _indReaction:      "#1F3F52"
    readonly property color _teaReaction:      "#275454"

    // ---- Media / scrim chrome (theme-invariant) ----
    // _scrimBase is the opaque equivalent of scrimSurface (#D9111111), so the
    // theme test can assert scrim inks against a real background.
    readonly property color _scrimBase:        "#111111"

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
        ownBubble: _outgoingBubbleBlue,
        reaction: _lightReaction,
        // Links get their own ink: white-on-accent (3:1) caps the accent's
        // luminance below what an AA link needs on raised surfaces.
        link: _lightLink, rail: _railLight,
        // Not _hoverLight, or hovering a message dissolves its bubble into the
        // row highlight.
        otherBubble: _cardElevatedLight
    })
    readonly property var _dark: ({
        // The rail takes the timeline's ground; without a `rail` key it falls
        // back to `sidebar` and merges with the room list.
        background: _dkBg, rail: _dkBg, sidebar: _dkSidebar, surface: _dkCard,
        cardElevated: _dkCardElevated, hover: _dkHover,
        selected: _dkSelected, selectedHover: _dkSelectedHover,
        selectedText: _dkSelectedText, inputBg: _dkInputBg,
        codeBlock: _dkCodeBlock, textPrimary: _dkTextPrimary,
        textSecondary: _dkTextSecondary, textMuted: _dkTextMuted,
        textDisabled: _dkTextDisabled, border: _dkBorder,
        borderStrong: _dkBorderStrong, accent: _accentBlue,
        accentHover: _accentBlueHover, accentPressed: _accentBluePressed,
        ownBubble: _outgoingBubbleBlue, otherBubble: _dkCard,
        // Not _dkCardElevated: raised chips drawn on an incoming bubble (link
        // preview, poll, pills) would be invisible against it.
        reaction: _dkReaction, link: _dkLink
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
        ownBubble: _outgoingBubbleBlue, otherBubble: _cardElevatedDark,
        reaction: _midReaction,
        // See _light.link.
        link: _midLink, rail: _bgDark
    })
    readonly property var _graphite: ({
        // See _dark.rail.
        background: _graBg, rail: _graBg, sidebar: _graSidebar, surface: _graCard,
        cardElevated: _graCardElevated, hover: _graHover,
        selected: _graSelected, selectedHover: _graSelectedHover,
        selectedText: _graSelectedText, inputBg: _graInputBg,
        codeBlock: _graCodeBlock, textPrimary: _graTextPrimary,
        textSecondary: _graTextSecondary, textMuted: _graTextMuted,
        textDisabled: _graTextDisabled, border: _graBorder,
        borderStrong: _graBorderStrong, accent: _graAccent,
        accentHover: _graAccentHover, accentPressed: _graAccentPressed,
        ownBubble: _graOwnBubble, otherBubble: _graOtherBubble,
        reaction: _graReaction,
        // See _light.link.
        link: _graLink
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
        ownBubble: _norOwnBubble, otherBubble: _norOtherBubble,
        reaction: _norReaction,
        // See _light.link.
        link: _norLink, rail: _norRail
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
        ownBubble: _purOwnBubble, otherBubble: _purOtherBubble,
        // See _dark.rail.
        rail: _purBg,
        reaction: _purReaction, link: _purLink
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
        ownBubble: _warOwnBubble, otherBubble: _warOtherBubble,
        reaction: _warReaction,
        // See _light.link.
        link: _warLink, rail: _warRail
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
        mention: _mosMention, online: _mosAccent, reaction: _mosReaction,
        // See _light.link.
        link: _mosLink
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
        mention: _indMention, online: _indOnline, reaction: _indReaction,
        // See _light.link.
        link: _indLink
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
        mention: _teaMention, online: _teaAccent, reaction: _teaReaction,
        // See _light.link.
        link: _teaLink
    })
    // Storm's shell mapping. The bolt accent is reserved for focus, checked
    // state, one primary action and the Home tile; hover, selection ink and
    // unread badges get their own values so the shell isn't over-yellowed.
    // accentSoft/accentBorder are explicit translucent bolt (own-reaction
    // pill).
    readonly property var _storm: ({
        background: _stoDeep, rail: _stoDeep, sidebar: _stoCanvas,
        surface: _stoPanel, cardElevated: _stoCardElevated,
        hover: Qt.alpha(_stoHover, 0.22),
        selected: _stoSelection, selectedHover: _stoSelectedHover,
        selectedText: _stoText, inputBg: _stoInset,
        codeBlock: _stoDeep, textPrimary: _stoText,
        textSecondary: _stoTextSecondary, textMuted: _stoTextMuted,
        textDisabled: _stoTextFaint, border: _stoBorder,
        borderStrong: _stoBorderStrong, accent: _stoBolt,
        accentHover: _stoAccentHover, accentPressed: _stoAccentPressed,
        accentSoft: Qt.alpha(_stoBolt, 0.14),
        accentBorder: Qt.alpha(_stoBolt, 0.35),
        accentText: _stoBoltInk,
        ownBubble: _stoOwnBubble, otherBubble: _stoPanel,
        reaction: _stoReaction,
        unreadBadge: _stoLink, mentionHighlight: _stoMention,
        mention: _stoMention, online: _stoSuccess, link: _stoLink
    })
    // Theme presets for the Settings picker, in display order. System (0) is a
    // resolution mode, shown as the match-system toggle rather than a card.
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
        { id: 7,  name: qsTr("Warm") },
        // The user-authored palette; the picker lists it only once it exists.
        { id: 12, name: qsTr("Your theme") }
    ]

    // The one theme-id -> palette switch, shared by _p, the preview cards and
    // the custom theme's base lookup.
    function rawPaletteForTheme(id) {
        switch (id) {
        case 1:  return _light
        case 2:  return _dark
        case 3:  return _graphite
        case 4:  return _midnight
        case 5:  return _nord
        case 6:  return _purple
        case 7:  return _warm
        case 8:  return _moss
        case 9:  return _indigo
        case 10: return _teal
        case 11: return _storm
        case 12: return _custom
        default: return _storm
        }
    }

    // `customBase`'s preset with the user's colours laid over it. The base is
    // clamped to a real preset: basing it on 12 would be a cycle.
    readonly property var _custom: {
        var base = rawPaletteForTheme(
            (customBase >= 1 && customBase <= 11) ? customBase : 11)
        var out = {}
        for (var k in base)
            out[k] = base[k]
        var ov = customOverrides
        if (ov) {
            for (var role in ov) {
                var v = ov[role]
                // Second gate: the config file is hand-editable, and an
                // unparseable colour would paint the shell transparent.
                if (typeof v === "string" && /^#[0-9A-Fa-f]{6}$/.test(v))
                    out[role] = v
            }
        }
        return out
    }

    // Palette for a theme by id, resolved to semantic roles. Used by the theme
    // preview cards and the custom-theme editor, which render themes the app
    // isn't running. Every fallback must match the live alias of the same name;
    // previewPaletteMatchesLiveTokens (SettingsShellQmlTest) enforces this.
    function paletteForTheme(id) {
        var p = rawPaletteForTheme(id)
        // `dark` and `isStorm` mirror the live `dark` and `storm` properties.
        var isDark = id === 12
                     ? relativeLuminance(_asColor(p.background)) < 0.18
                     : (id !== 1 && id !== 7 && id !== 8)
        var isStorm = id === 11
        var cardElevated = p.cardElevated
        var textPrimary = p.textPrimary
        var textMuted = p.textMuted
        var accent = p.accent
        var mentionBadge = p.mention !== undefined ? p.mention : _accentDanger
        return {
            background:      p.background,
            rail:            p.rail !== undefined ? p.rail : p.sidebar,
            sidebar:         p.sidebar,
            surface:         p.surface,
            cardElevated:    cardElevated,
            hover:           p.hover,
            selected:        p.selected,
            selectedHover:   p.selectedHover,
            selectedText:    p.selectedText,
            border:          p.border,
            borderStrong:    p.borderStrong,
            inputBackground: p.inputBg,
            codeBlock:       p.codeBlock,
            accent:          accent,
            accentHover:     p.accentHover,
            accentPressed:   p.accentPressed,
            accentText:      p.accentText !== undefined ? p.accentText
                                                        : _onAccent,
            accentSoft:      p.accentSoft !== undefined ? p.accentSoft
                                                        : p.selected,
            accentBorder:    p.accentBorder !== undefined ? p.accentBorder
                                                          : p.borderStrong,
            link:            p.link !== undefined ? p.link : accent,
            textPrimary:     textPrimary,
            textSecondary:   p.textSecondary,
            textMuted:       textMuted,
            textDisabled:    p.textDisabled,
            icon:            textMuted,
            sectionLabelColor: textMuted,
            ownBubble:       p.ownBubble,
            ownBubbleText:   ownBubbleText,
            otherBubble:     p.otherBubble,
            otherBubbleText: textPrimary,
            embedSurface:    isStorm ? _stoPanel : cardElevated,
            embedBorder:     isStorm ? _stoBorder : p.border,
            reactionBackground: p.reaction !== undefined ? p.reaction
                                                         : cardElevated,
            reactionBorder:  p.border,
            reactionInk:     p.textSecondary,
            unreadBadge:     p.unreadBadge !== undefined ? p.unreadBadge
                                                         : accent,
            mentionHighlight: p.mentionHighlight !== undefined
                              ? p.mentionHighlight : accent,
            mentionBadge:    mentionBadge,
            success:         p.success !== undefined ? p.success
                                                     : (isDark ? _okInkDark
                                                               : _okInkLight),
            danger:          p.danger !== undefined ? p.danger
                                                    : (isDark ? _dangerInkDark
                                                              : _dangerInkLight)
        }
    }

    readonly property var _p: rawPaletteForTheme(effectiveTheme)

    // ---- Semantic aliases (preferred). ----
    readonly property color background:          _p.background
    // Role names for the shell regions, so a future theme can separate them
    // without a call-site sweep.
    readonly property color windowBackground:    background
    readonly property color sidebar:             _p.sidebar
    readonly property color navBackground:       sidebar
    readonly property color panelBackground:     sidebar
    // Spaces-rail surface; falls back to the sidebar.
    readonly property color rail:                _p.rail !== undefined
                                                 ? _p.rail : _p.sidebar
    // ── Channels navigation layout ──
    // Derived with fallbacks to existing tokens, so every palette works without
    // new required keys and any tone can be overridden per palette.
    // The category header is a quiet label: structure, not content.
    readonly property color channelCategoryText:
        _p.channelCategoryText !== undefined ? _p.channelCategoryText
                                             : textMuted
    /// Channel name at rest; dimmer than a Classic row since unread earns
    /// full-strength ink.
    readonly property color channelText:
        _p.channelText !== undefined ? _p.channelText : textSecondary
    /// Unread channel. Weight is the only unread signal for a row without a
    /// count.
    readonly property color channelTextUnread:
        _p.channelTextUnread !== undefined ? _p.channelTextUnread : text
    /// Current channel; reuses the room list's selected pair.
    readonly property color channelSelected:
        _p.channelSelected !== undefined ? _p.channelSelected : selected
    readonly property color channelSelectedText:
        _p.channelSelectedText !== undefined ? _p.channelSelectedText
                                             : selectedText
    readonly property color channelHover:
        _p.channelHover !== undefined ? _p.channelHover : hover
    /// Unread mark on a channel row.
    readonly property color channelUnreadMark:
        _p.channelUnreadMark !== undefined ? _p.channelUnreadMark : accent

    // ── Spaces rail: folders and drag feedback ──
    // Derived, like the Channels tokens.
    // Container behind an open folder and its Spaces.
    readonly property color railFolderSurface:
        _p.railFolderSurface !== undefined ? _p.railFolderSurface
                                           : cardElevated
    // Nested-region tints for the rail, one rung per depth, drawn stacked and
    // inset. Index 0 is a folder's container and 1..4 are hierarchy depth; the
    // rail maps deeper rows onto the last entries so that touching regions
    // never share a tint.
    //
    // Rungs are built from the rail's own ground, mixed towards `text` and
    // increasingly towards `accent`, so depth reads as both a lightness and a
    // chroma step while the rail stays darker than the room list. Rungs 1-4 are
    // solved for an even ~3.4 ΔL* along the chain actually painted
    // (rail -> 1 -> 2 -> 3); rung 0 is solved separately as the folder step.
    // Equal alpha steps are not equal lightness steps (sRGB), so light and dark
    // rails use separately solved alphas. See docs/round-history.md.
    readonly property bool _railIsDark: rail.hslLightness < 0.5
    readonly property var _railNestAccentMix: [0.10, 0.18, 0.30, 0.42, 0.54]
    readonly property var _railNestAlphas:
        _railIsDark ? [0.027, 0.039, 0.081, 0.133, 0.200]
                    : [0.040, 0.058, 0.119, 0.185, 0.256]
    // Written out rather than generated by a function: a binding through a call
    // would not track these colours and would miss theme changes.
    readonly property var railNestSurfaces: [
        Qt.tint(rail, Qt.rgba(
            text.r + (accent.r - text.r) * _railNestAccentMix[0],
            text.g + (accent.g - text.g) * _railNestAccentMix[0],
            text.b + (accent.b - text.b) * _railNestAccentMix[0],
            _railNestAlphas[0])),
        Qt.tint(rail, Qt.rgba(
            text.r + (accent.r - text.r) * _railNestAccentMix[1],
            text.g + (accent.g - text.g) * _railNestAccentMix[1],
            text.b + (accent.b - text.b) * _railNestAccentMix[1],
            _railNestAlphas[1])),
        Qt.tint(rail, Qt.rgba(
            text.r + (accent.r - text.r) * _railNestAccentMix[2],
            text.g + (accent.g - text.g) * _railNestAccentMix[2],
            text.b + (accent.b - text.b) * _railNestAccentMix[2],
            _railNestAlphas[2])),
        Qt.tint(rail, Qt.rgba(
            text.r + (accent.r - text.r) * _railNestAccentMix[3],
            text.g + (accent.g - text.g) * _railNestAccentMix[3],
            text.b + (accent.b - text.b) * _railNestAccentMix[3],
            _railNestAlphas[3])),
        Qt.tint(rail, Qt.rgba(
            text.r + (accent.r - text.r) * _railNestAccentMix[4],
            text.g + (accent.g - text.g) * _railNestAccentMix[4],
            text.b + (accent.b - text.b) * _railNestAccentMix[4],
            _railNestAlphas[4]))
    ]
    // Kept for external readers (none today); pinned to the first hierarchy
    // rung.
    readonly property color railNestSurface: railNestSurfaces[1]

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
    // ink.
    readonly property color accentText:          _p.accentText !== undefined
                                                 ? _p.accentText : _onAccent
    // Soft accent tint and border: selection chips, active icon chips,
    // own-reaction pills. Older palettes fall back to their selection tones.
    readonly property color accentSoft:          _p.accentSoft !== undefined
                                                 ? _p.accentSoft : _p.selected
    readonly property color accentBorder:        _p.accentBorder !== undefined
                                                 ? _p.accentBorder
                                                 : _p.borderStrong
    // ---- Status roles: ink and fill are different colours ----
    // Text, icons and inline labels use `danger` / `warning` / `success` /
    // `info`; solid destructive buttons or badges use `dangerFill` with
    // `dangerText`. Inks route by mode and can be overridden per palette.
    readonly property color success:             _p.success !== undefined
                                                 ? _p.success
                                                 : (dark ? _okInkDark
                                                         : _okInkLight)
    readonly property color warning:             _p.warning !== undefined
                                                 ? _p.warning
                                                 : (dark ? _warnInkDark
                                                         : _warnInkLight)
    readonly property color danger:              _p.danger !== undefined
                                                 ? _p.danger
                                                 : (dark ? _dangerInkDark
                                                         : _dangerInkLight)
    readonly property color info:                _p.info !== undefined
                                                 ? _p.info
                                                 : (dark ? _infoInkDark
                                                         : _infoInkLight)
    // Solid destructive fills (Remove, Leave, Sign out…). Theme-invariant:
    // white-on-red reads the same everywhere.
    readonly property color dangerFill:          _accentDanger
    readonly property color dangerFillHover:     _dangerFillHover
    readonly property color dangerFillPressed:   _dangerFillPressed
    readonly property color dangerText:          "#FFFFFF"
    // Solid non-destructive status fills; ink is dangerText.
    readonly property color successFill:         _accentGreen
    readonly property color warningFill:         _accentWarning
    readonly property color infoFill:            _accentInfo
    // dangerInk for icon/label ink; the soft pair for destructive-row fills and
    // warning-chip borders.
    readonly property color dangerInk:           danger
    readonly property color dangerSoft:          Qt.alpha(mentionBadge, 0.10)
    readonly property color dangerBorder:        Qt.alpha(mentionBadge, 0.25)
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
    // Bare interface icons at rest.
    readonly property color icon:                textMuted
    // Shadow tint for the composer card, slider thumb and centred popovers.
    // Context menus stay border-only: their geometry feeds anchor maths.
    readonly property color shadow:              dark ? "#59000000"
                                                      : "#0A000000"
    readonly property color overlayScrim:        "#80000000"
    // Ink on overlayScrim media badges. Always white: the scrim is near-black
    // on every theme, so accentText (dark on Deep Teal) must not be used here.
    readonly property color scrimInk:            "#FFFFFF"
    // Modal backdrop for the quick switcher and centred dialogs; distinct from
    // overlayScrim, which media badges and the account-switch blocker use.
    readonly property color modalScrim:          "#7308080C"
    readonly property color codeBlock:           _p.codeBlock
    // Link colour. Storm uses its periwinkle ink: the bolt is never used for
    // inline links.
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
    // In-timeline embed cards (voice, polls, link previews, file and video
    // placeholders) sit on the timeline ground, not a panel, so they use a
    // raised-but-dark step rather than a pale block.
    readonly property color embedSurface: storm ? _stoPanel : cardElevated
    readonly property color embedBorder:  storm ? _stoBorder : border

    readonly property color reactionBackground:  _p.reaction !== undefined
                                                 ? _p.reaction : cardElevated
    readonly property color reactionBorder:      border
    readonly property color reactionInk:         textSecondary
    readonly property color reactionSelectedBackground: accentSoft
    readonly property color reactionSelectedBorder:     accentBorder
    readonly property color reactionSelectedInk:        textPrimary
    readonly property color reactionHighlight:   selected
    // Unread pill fill. Storm uses periwinkle so a busy list isn't all bolt.
    readonly property color unreadBadge:         _p.unreadBadge !== undefined
                                                 ? _p.unreadBadge : accent
    // Base hue of the mention-row wash (the timeline applies alpha). Storm uses
    // the mention rose: a bolt wash over the timeline composites to brown.
    readonly property color mentionHighlight:    _p.mentionHighlight !== undefined
                                                 ? _p.mentionHighlight : accent
    // Mention pill fill. Falls back to the danger fill, never `danger`: the
    // pill carries white ink and `danger` is a light rose on dark themes.
    readonly property color mentionBadge:        _p.mention !== undefined
                                                 ? _p.mention : _accentDanger
    // Inline "@name" chip: tinted wash and body ink, no border (an outline
    // reads as an error box).
    readonly property color mentionChipFill:     Qt.alpha(mentionHighlight, 0.16)
    readonly property color mentionChipInk:      textPrimary
    readonly property color mentionChipBorder:   "transparent"
    // The chip when the reader is the one mentioned.
    readonly property color mentionSelfFill:     accentSoft
    readonly property color mentionSelfInk:      textPrimary
    readonly property color undecryptableText:   textMuted
    // Jumped-to message rows and active thread affordances.
    readonly property color pressedSurface:      selectedHover
    readonly property color messageHighlight:    selected
    readonly property color threadHighlight:     accent
    // Presence dots: online green, away amber, offline muted.
    readonly property color presenceOnline:      _p.online !== undefined
                                                 ? _p.online : success
    // Kept clear of the Storm bolt hue (see _awayLight).
    readonly property color presenceAway:        _p.away !== undefined
                                                 ? _p.away
                                                 : (dark ? _awayDark
                                                         : _awayLight)
    readonly property color presenceOffline:     textMuted

    // ---- Control surfaces ----
    // Theme-routed hover/pressed steps for buttons, icon chips and segmented
    // controls. Use these rather than Qt.darker()/lighter() at call sites.
    readonly property color buttonPrimaryFill:    accent
    readonly property color buttonPrimaryHover:   accentHover
    readonly property color buttonPrimaryPressed: accentPressed
    readonly property color buttonPrimaryInk:     accentText
    readonly property color buttonNeutralFill:    cardElevated
    readonly property color buttonNeutralHover:   selected
    readonly property color buttonNeutralPressed: selectedHover
    readonly property color buttonNeutralInk:     textPrimary
    readonly property color buttonNeutralBorder:  border
    // Ghost: no resting fill.
    readonly property color buttonGhostHover:     hover
    readonly property color buttonGhostPressed:   selected
    readonly property color buttonGhostInk:       textSecondary
    readonly property color buttonDangerFill:     dangerFill
    readonly property color buttonDangerHover:    dangerFillHover
    readonly property color buttonDangerPressed:  dangerFillPressed
    readonly property color buttonDangerInk:      dangerText
    // Disabled controls are exempt from WCAG contrast, so textDisabled is
    // allowed here and nowhere else.
    readonly property color buttonDisabledFill:   cardElevated
    readonly property color buttonDisabledInk:    textDisabled
    readonly property color buttonDisabledBorder: border
    // Three heights, one radius, one horizontal rhythm.
    readonly property int   buttonHeightSm:       26
    readonly property int   buttonHeight:         32
    readonly property int   buttonHeightLg:       40
    readonly property int   buttonRadius:         radiusTile   // 9
    readonly property int   buttonPaddingH:       spacing12
    readonly property int   buttonPaddingHSm:     spacing8
    readonly property int   buttonIconGap:        spacing6
    readonly property int   buttonMinWidth:       72

    // ---- Status chips ----
    // A chip is a tint of its own ink (fill 14%, border 32%), never a solid
    // accent, so families stay distinguishable and fills track their inks.
    readonly property color chipNeutralInk:    textSecondary
    readonly property color chipNeutralFill:   Qt.alpha(textSecondary, 0.14)
    readonly property color chipNeutralBorder: Qt.alpha(textSecondary, 0.32)
    readonly property color chipAccentInk:     link
    readonly property color chipAccentFill:    Qt.alpha(link, 0.14)
    readonly property color chipAccentBorder:  Qt.alpha(link, 0.32)
    readonly property color chipSuccessInk:    success
    readonly property color chipSuccessFill:   Qt.alpha(success, 0.14)
    readonly property color chipSuccessBorder: Qt.alpha(success, 0.32)
    readonly property color chipWarningInk:    warning
    readonly property color chipWarningFill:   Qt.alpha(warning, 0.14)
    readonly property color chipWarningBorder: Qt.alpha(warning, 0.32)
    readonly property color chipDangerInk:     danger
    readonly property color chipDangerFill:    Qt.alpha(danger, 0.14)
    readonly property color chipDangerBorder:  Qt.alpha(danger, 0.32)
    readonly property color chipInfoInk:       info
    readonly property color chipInfoFill:      Qt.alpha(info, 0.14)
    readonly property color chipInfoBorder:    Qt.alpha(info, 0.32)
    // The one solid chip, for "current selection / verified". Use sparingly.
    readonly property color chipBoltFill:      bolt
    readonly property color chipBoltInk:       boltInk
    readonly property int   chipHeight:        20
    readonly property int   chipRadius:        radiusPill
    readonly property int   chipPaddingH:      spacing8

    // ---- Scrollbars ----
    readonly property color scrollbarTrack:         "transparent"
    readonly property color scrollbarTrackHover:    Qt.alpha(border, 0.35)
    readonly property color scrollbarHandle:        borderStrong
    readonly property color scrollbarHandleHover:   textDisabled
    readonly property color scrollbarHandlePressed: textMuted
    readonly property int   scrollbarWidth:      10
    readonly property int   scrollbarWidthThin:   6
    readonly property int   scrollbarRadius:     radiusPill
    readonly property int   scrollbarMargin:      2

    // ---- Elevation ----
    // Three steps so a stacked popover can sit visibly above its opener.
    readonly property color shadowSoft:          dark ? "#40000000"
                                                      : "#0D000000"
    readonly property color shadowStrong:        dark ? "#8C000000"
                                                      : "#26000000"
    readonly property int   elevationCardBlur:      8
    readonly property int   elevationCardY:         2
    readonly property int   elevationPopoverBlur:  18
    readonly property int   elevationPopoverY:      6
    readonly property int   elevationModalBlur:    32
    readonly property int   elevationModalY:       12

    // ---- Media / scrim chrome ----
    // Dark on every theme: these paint over arbitrary media.
    readonly property color scrimBackdrop:       "#66000000"  // behind media
    readonly property color scrimSurface:        "#D9111111"  // chrome bar
    readonly property color scrimSurfaceRaised:  "#33FFFFFF"  // button on it
    readonly property color scrimSurfaceHover:   "#59FFFFFF"
    readonly property color scrimBorder:         "#33FFFFFF"
    readonly property color scrimInkStrong:      "#E6FFFFFF"
    readonly property color scrimInkMuted:       "#94A3B8"

    // ---- Storm surface language ---- Shared tokens for menus, popovers,
    // pickers, dialogs and Settings. Under Storm they carry the Storm literals;
    // under any other theme they resolve to that theme's semantic equivalent.
    // Consumers use storm* by role and never branch on the theme.
    readonly property color stormCanvas:        storm ? _stoCanvas : background
    readonly property color stormPanel:         storm ? _stoPanel : surface
    readonly property color stormInset:         storm ? _stoInset : inputBackground
    readonly property color stormDeep:          storm ? _stoDeep : background
    readonly property color stormBorder:        storm ? _stoBorder : border
    readonly property color stormBorderStrong:  storm ? _stoBorderStrong : borderStrong
    readonly property color stormSelection:     storm ? _stoSelection : hover
    // The accent: active/selected/complete/primary only.
    readonly property color bolt:               storm ? _stoBolt : accent
    // Ink on a bolt/accent fill (primary buttons, count pills).
    readonly property color boltInk:            storm ? _stoBoltInk : accentText
    // The wordmark's bolt is a brand mark, not a control. Outside Storm it's
    // the accent blended towards the header ink so it doesn't read as a status
    // light or compete with the primary button.
    readonly property color wordmarkBolt:       storm ? _stoBolt
                                                : Qt.tint(textSecondary,
                                                          Qt.alpha(accent, 0.55))
    readonly property color stormText:          storm ? _stoText : textPrimary
    readonly property color stormTextSecondary: storm ? _stoTextSecondary : textSecondary
    readonly property color stormTextMuted:     storm ? _stoTextMuted : textMuted
    // Deliberately dim ink for section headers, footers and metadata; never
    // sentence text.
    readonly property color stormTextFaint:     storm ? _stoTextFaint : textDisabled
    readonly property color stormDanger:        storm ? _stoDanger : dangerInk
    readonly property color stormSuccess:       storm ? _stoSuccess : success
    readonly property color stormLink:          storm ? _stoLink : link
    // Derived soft danger treatments; legacy themes share the global ones.
    readonly property color stormDangerSoft:    storm ? Qt.alpha(_stoDanger, 0.10)
                                                      : dangerSoft
    readonly property color stormDangerBorder:  storm ? Qt.alpha(_stoDanger, 0.30)
                                                      : dangerBorder
    readonly property color stormBoltGlow:      Qt.alpha(bolt, 0.12)     // input focus halo
    readonly property color stormWatermark:     Qt.alpha(bolt, 0.12)     // hero-card bolt

    // ── Soft-fill legibility ──
    // A soft chip fills with its own tone at softChipFillAlpha, which pulls the
    // ground towards the label's ink, so an ink that clears AA on the card can
    // fail on the chip. softChipInk() derives a per-family ink step via
    // IdentityPalette.legibleChoice, which keeps hue and saturation and moves
    // lightness only. Computed rather than tabled so custom themes are covered.
    //
    // Source-over composite of `top` at `alpha` onto opaque `bottom`, in sRGB
    // component space as the scene graph blends.
    function compositeOver(top, alpha, bottom) {
        var t = _asColor(top)
        var b = _asColor(bottom)
        return Qt.rgba(alpha * t.r + (1.0 - alpha) * b.r,
                       alpha * t.g + (1.0 - alpha) * b.g,
                       alpha * t.b + (1.0 - alpha) * b.b, 1.0)
    }

    // Fill alpha for soft StatusChips; shared so the ink derivation and the
    // fill cannot drift apart.
    readonly property real softChipFillAlpha: 0.14

    // Grounds a soft chip may sit on; the ink clears the worst of them.
    readonly property var _softChipGrounds: [
        stormPanel, stormCanvas, surface, cardElevated, stormInset
    ]

    // Label ink for a soft chip of `base`: unchanged if it already clears its
    // fill, otherwise the same hue a step deeper or paler. Fill and border keep
    // the raw tone.
    function softChipInk(base) {
        var tone = _asColor(base)
        var fills = []
        for (var i = 0; i < _softChipGrounds.length; ++i)
            fills.push(compositeOver(tone, softChipFillAlpha,
                                     _softChipGrounds[i]))
        return IdentityPalette.legibleChoice(tone, fills)
    }

    // ── A row is four surfaces ──
    // A list row paints hover/selected/selectedHover over the canvas, some of
    // them translucent, so secondary ink must be checked per state.
    // flatten() composites `c` onto `ground` using c's own alpha.
    function flatten(c, ground) {
        var t = _asColor(c)
        return compositeOver(t, t.a, ground)
    }

    // One ink for all four grounds does not work: an ink that survives the
    // loudest fill over-contrasts at rest and inverts the name/id hierarchy.
    // So the ground is per state, and a token that already clears it is
    // returned unchanged.
    function legibleInkOn(base, ground) {
        return IdentityPalette.legibleChoice(_asColor(base), [ground])
    }

    // ---- The trust surface has no tokens of its own ----
    // TrustCard.qml uses the storm* tokens by role, so it follows the selected
    // theme; under Storm they resolve to the brand literals
    // (TrustCardTest::stormKeepsTheBrandLiterals).

    // Chrome for the custom-theme editor. It follows the selected theme but
    // never the palette being edited, or a user could make the editor's own
    // controls invisible. It resolves against a preset: the selected one, or
    // the custom theme's base (clamped to 1..11). This is also why the editor
    // draws its own controls instead of AppButton / AppTextField / AppComboBox,
    // which follow the effective (possibly custom) theme.
    readonly property int editorChromeTheme: {
        if (effectiveTheme === 12)
            return (customBase >= 1 && customBase <= 11) ? customBase : 11
        return (effectiveTheme >= 1 && effectiveTheme <= 11) ? effectiveTheme : 11
    }
    readonly property var _editorPalette: paletteForTheme(editorChromeTheme)
    // Storm keeps its own deeper chrome tones, as in the storm* namespace.
    readonly property bool _editorStorm: editorChromeTheme === 11
    readonly property color editorCanvas:        _editorStorm ? _stoCanvas
                                     : _asColor(_editorPalette.background)
    readonly property color editorPanel:         _editorStorm ? _stoPanel
                                     : _asColor(_editorPalette.surface)
    readonly property color editorInset:         _editorStorm ? _stoInset
                                     : _asColor(_editorPalette.inputBackground)
    readonly property color editorDeep:          _editorStorm ? _stoDeep
                                     : _asColor(_editorPalette.background)
    readonly property color editorBorder:        _editorStorm ? _stoBorder
                                     : _asColor(_editorPalette.border)
    readonly property color editorBorderStrong:  _editorStorm ? _stoBorderStrong
                                     : _asColor(_editorPalette.borderStrong)
    readonly property color editorSelection:     _editorStorm ? _stoSelection
                                     : _asColor(_editorPalette.selected)
    readonly property color editorAccent:        _editorStorm ? _stoBolt
                                     : _asColor(_editorPalette.accent)
    readonly property color editorAccentInk:     _editorStorm ? _stoBoltInk
                                     : _asColor(_editorPalette.accentText)
    readonly property color editorText:          _editorStorm ? _stoText
                                     : _asColor(_editorPalette.textPrimary)
    readonly property color editorTextSecondary: _editorStorm ? _stoTextSecondary
                                     : _asColor(_editorPalette.textSecondary)
    readonly property color editorTextMuted:     _editorStorm ? _stoTextMuted
                                     : _asColor(_editorPalette.textMuted)
    readonly property color editorDanger:        _editorStorm ? _stoDanger
                                     : _asColor(_editorPalette.danger)

    // Initials-avatar discs, derived from the active theme: nine hues in an arc
    // centred on the theme's identity anchor. The arithmetic lives in C++
    // (src/theme/IdentityPalette.*) so the notification avatar, painted without
    // a QML engine, uses the same implementation. Reading `accent` makes
    // avatarColor() bindings follow theme changes.
    //
    // The original fixed ladder, kept as a reference. Nothing reads it.
    readonly property var avatarPaletteLegacy: [
        "#D04339", "#AE6424", "#8F7224", "#4F822B", "#2E8460",
        "#2F7F93", "#4163C8", "#8941C8", "#C84190"
    ]
    // The identity hash behind avatar fills and sender-name inks, so a user's
    // disc and name agree on hue.
    function identityIndex(key) {
        var h = 0
        for (var i = 0; i < key.length; ++i)
            h = ((h << 5) - h + key.charCodeAt(i)) | 0
        return Math.abs(h) % 9
    }
    // The colour the discs are derived from: normally the accent, but the
    // background when the accent is far from the shell's hue (Storm's bolt on
    // navy). Custom themes get the same rule.
    readonly property color identityAnchor: {
        var bg = _asColor(background)
        var ink = _asColor(accent)
        if (bg.hslSaturation < 0.20)
            return ink
        var gap = Math.abs(bg.hslHue - ink.hslHue)
        if (gap > 0.5)
            gap = 1.0 - gap
        return gap > (60.0 / 360.0) ? bg : ink
    }
    function avatarColor(key) {
        return IdentityPalette.disc(identityIndex(key), identityAnchor)
    }
    // Initials ink for the disc. Never assume white: half the discs are pale.
    function avatarInk(key) {
        return IdentityPalette.ink(identityIndex(key), identityAnchor)
    }
    // Grounds a sender name is painted on; the ink must clear the worst of them
    // (incoming bubble in Bubbles-for-DMs mode, elevated card in profile
    // popovers).
    readonly property var _nameGrounds: [
        background, surface, cardElevated, incomingBubble
    ]
    // Per-user display-name colour, derived from the active theme in C++
    // (src/theme/IdentityColors.cpp) beside the disc arithmetic so names and
    // discs share a hue family. Reading the theme's colours makes these
    // bindings follow theme switches and live custom-theme edits.
    //
    // nameInkForSlot() exposes the nine derived inks as a picker palette for
    // the user's own name colour.
    function nameInkForSlot(slot) {
        return IdentityPalette.nameInk(slot, identityAnchor, _nameGrounds)
    }
    function userColor(key) {
        if (!key || key.length === 0)
            return textPrimary
        // A colour the user chose (from their Matrix profile) outranks the
        // derived one, but its lightness is clamped to stay legible on this
        // window's surfaces; the hue is kept.
        if (typeof app !== "undefined" && app.nameColors
                && app.nameColors.available) {
            // `revision` is read so this binding re-evaluates when an answer
            // lands; colorFor() dispatches the fetch on first call.
            const dependOnAnswers = app.nameColors.revision
            const chosen = app.nameColors.colorFor(key)
            if (chosen && chosen.length > 0)
                return IdentityPalette.legibleChoice(chosen, _nameGrounds)
        }
        return IdentityPalette.nameInk(identityIndex(key), identityAnchor,
                                       _nameGrounds)
    }

    // ---- Legacy aliases retained for existing QML. ----
    readonly property color surfaceAlt:          cardElevated
    readonly property color text:                textPrimary
    readonly property color muted:               textMuted
    readonly property color selectedBg:          selected
    // Legacy name for the danger ink. Destructive fills must use dangerFill.
    readonly property color error:               danger

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
    // ── Reaction chips ── Emoji metrics vary widely (ZWJ sequences, modifiers,
    // keycaps, flags), so the glyph is drawn inside a fixed box and layout sees
    // the box. Custom MSC2545 images use the same box.
    readonly property int reactionEmojiSize: 16
    readonly property int reactionEmojiBox:  18
    readonly property int reactionChipHeight: 20

// Media corner radius; matches the card radius.
    readonly property int radiusMedia: 12
    // Menu-language radii.
    readonly property int radiusChip:    radiusSm   // keycap chips (named role)
    readonly property int radiusControl: 7   // 28px action-bar buttons, emoji cells
    readonly property int radiusTile:    9   // icon tiles, footer action buttons
    readonly property int radiusThumb:  10   // GIF grid thumbnails
    readonly property int radiusOmnibox: 11  // new-conversation omnibox field
    readonly property int radiusCard:   14   // identity cards, quick-switcher modal

    // ---- Menu / popover language ----
    readonly property int menuPadding:        spacing6   // popover internal padding
    readonly property int menuRadius:         radiusLg   // popover container corner
    readonly property int menuItemHeight:     32
    readonly property int menuItemRadius:     radiusMd
    readonly property int menuItemPadding:    spacing8   // row side padding
    readonly property int menuIconSize:       17
    readonly property int menuIconGap:        spacing10  // icon-to-label gap
    readonly property int menuContextHeaderHeight: 24    // context header
    readonly property int menuWidthDefault:   220
    readonly property int menuWidthMessage:   252        // message menu
    readonly property int menuWidthRoom:      196        // room menu
    readonly property int menuWidthFlyout:    150        // notifications flyout
    readonly property int menuDividerVMargin: spacing6
    readonly property int menuDividerHMargin: spacing4
    // Accelerator keycap chips (MenuKeycap.qml).
    readonly property color keycapBackground:     cardElevated
    readonly property color keycapBorder:         borderStrong
    readonly property color keycapText:           textMuted
    readonly property int   keycapPaddingH:       5   // row-level chips
    readonly property int   keycapPaddingV:       1
    readonly property int   keycapHeaderPaddingH: 6   // header-level chips (ESC)
    readonly property int   keycapHeaderPaddingV: 2
    // Section labels and mono identity strings inside popovers. They use the
    // AA-asserted muted ink; textDisabled is reserved for disabled controls.
    readonly property color sectionLabelColor: textMuted
    readonly property color monoIdentityColor: textMuted
    // Letter-spacing is in pixels in QML, converted at the design size.
    readonly property real  trackingSection:   0.8   // .08em at 10px
    readonly property real  trackingMono:      1.2   // .12em at 10px
    // Storm mono headers (.16–.18em at ~9.5px).
    readonly property real  trackingStorm:     1.6
    // Emoji grid cells for the picker and the quick-react strip, sized for
    // colour emoji.
    readonly property int   emojiCellSize:  46
    readonly property int   emojiGlyphSize: 30

    // ---- Layout constraints. ----
    // Maximum width of a timeline row's content, so wide windows stay readable.
    readonly property int timelineContentMaxWidth: 760

    // ---- Typography ----
    //
    // The type scale. Use these six names:
    //
    //   token          px  weight            role
    //   textDisplay    22  weightDisplay 800 login hero, empty-state hero,
    //                                        verification panel headline
    //   textTitle      16  weightBold    700 dialog titles, pane headers,
    //                                        settings section headings
    //   textSubtitle   14  weightStrong  600 group headers, section labels,
    //                                        result-row titles
    //   textBody       14  weightBody    400 message body, list primary,
    //                                        menu item labels
    //   textMeta       12  weightMedium  500 timestamps, subtitles, chips,
    //                                        secondary rows, reaction counts
    //   textMicro      10  weightBold    700 unread badges and keycaps only
    //
    // A "strong" body is a weight change at textBody, never a size change. The
    // older size tokens below are retained for existing call sites and
    // annotated with their replacement; convert files as you edit them.
    readonly property int textDisplay:   22
    readonly property int textTitle:     16
    readonly property int textSubtitle:  14
    readonly property int textBody:      14
    readonly property int textMeta:      12
    readonly property int textMicro:     10

    // Weights. font.weight takes an int in Qt 6.
    readonly property int weightBody:    400
    readonly property int weightMedium:  500
    readonly property int weightStrong:  600
    readonly property int weightBold:    700
    readonly property int weightDisplay: 800

    // Leading. Set `lineHeight: AppTheme.lineHeightBody; lineHeightMode:
    // Text.ProportionalHeight` on wrapping text, otherwise the font's own
    // metrics apply and leading changes with the chosen UI font. Single-line
    // chrome keeps the default.
    readonly property real lineHeightBody:    1.5
    readonly property real lineHeightTight:   1.3
    readonly property real lineHeightDisplay: 1.2

    // ---- Superseded size tokens (do not add new call sites) ----
    readonly property int fontSizeXS:        11   // -> textMeta / textMicro
    readonly property int fontSizeS:         13   // -> textBody / textMeta
    readonly property int fontSizeM:         14   // -> textBody
    readonly property int fontSizeRoom:      16   // -> textTitle
    readonly property int fontSizeHeader:    18   // -> textTitle
    // fontSizePageTitle and fontSizeXL have no callers. fontPageTitle is
    // required by tests/ThemeTokensTest.cpp and maps to the display size.
    readonly property int fontSizePageTitle: 24   // unused -> textDisplay
    readonly property int fontSizeL:         fontSizeRoom
    readonly property int fontSizeXL:        fontSizeHeader // unused -> textTitle

    readonly property int fontPageTitle:     textDisplay       // 22
    readonly property int fontSectionTitle:  fontSizeHeader    // 18 -> textTitle
    readonly property int fontRoomTitle:     fontSizeRoom      // 16 -> textTitle
    readonly property int fontBody:          fontSizeM         // 14 -> textBody
    // Misnomer: `fontSecondary` is the primary menu-item label size.
    readonly property int fontSecondary:     fontSizeS         // 13 -> textBody
    readonly property int fontMessageSender: 12                // -> textMeta
    readonly property int fontCaption:       fontSizeXS        // 11 -> textMeta
    readonly property int fontMono:          fontSizeS         // 13 -> textBody
    // Menu-language sizes, rounded up to whole pixels. Chrome sizes: never wrap
    // in scaled().
    readonly property int fontMicro:     9   // trust captions, GIF badge, role chips
    readonly property int fontChip:      10  // keycaps, section labels -> textMicro
    readonly property int fontMonoXS:    11  // mono identity strings -> textMeta
    readonly property int fontMonoSm:    12  // footer hints, status lines -> textMeta
    readonly property int fontResult:    14  // result-row titles -> textSubtitle
    readonly property int fontQuery:     15  // search/omnibox query -> textTitle
    readonly property int fontTrustName: 17  // trust-card display name -> textTitle
    readonly property int fontNavTitle:  17  // Settings-nav pane title -> textTitle

    // ---- Font families ---- Bundled UI families ship in data/fonts (loaded in
    // main.cpp); Main.qml pushes the per-account selection here. The lists are
    // fallbacks. Mono, icon and emoji faces are independent of the UI font
    // choice.
    property string uiFont:          "Manrope"
    // Normalises each bundled family to Manrope's 0.540em x-height (OS/2
    // sxHeight / unitsPerEm) so the font choice doesn't change apparent size.
    function opticalScale(family) {
        switch (family) {
        case "Inter":            return 0.99
        case "Plus Jakarta Sans": return 1.01
        case "IBM Plex Sans":    return 1.05
        case "Source Sans 3":    return 1.13
        case "Space Grotesk":    return 1.11
        default:                 return 1.0
        }
    }
    readonly property real uiFontOptical: opticalScale(uiFont)
    readonly property var    uiFontFamilies:  [
        "Manrope",
        "Inter",
        "SF Pro Display",
        "Segoe UI Variable",
        "Segoe UI",
        "system-ui",
        "sans-serif"
    ]
    // The emoji face is not a token: QML's font type has no `families`, so a
    // fallback list can't be assigned here. AppController::emojiFontFamily
    // picks one from the fonts the host has. The icon face.
    readonly property string iconFont:         "Material Symbols Rounded"
    // The monospace face. Main.qml pushes FontManager's resolved per-account
    // choice, so a font the host lacks falls back to the bundled one.
    property string monoFont:                  "JetBrains Mono"
    readonly property var    monoFontFamilies: [
        "JetBrains Mono",
        "Fira Mono",
        "SF Mono",
        "Consolas",
        "monospace"
    ]
    // Brand face, used for the trust surface and the wordmark.
    readonly property string brandFont:         "Space Grotesk"
    readonly property var    brandFontFamilies: [
        "Space Grotesk",
        "Manrope",
        "sans-serif"
    ]
    // The wordmark / trust-surface face, by role.
    readonly property string displayFont:       brandFont

    // Menu-surface label face. Follows the user's UI font: the brand face isn't
    // theme-routed, is absent from the font picker, and reads smaller at the
    // same pixel size.
    readonly property string menuFont:          uiFont

    // Menu/popover section labels ("ROOMS", "RECENTLY", context headers):
    // sentence case, UI face, meta size, strong weight. Keep mono for code,
    // keycaps and Matrix identifiers.
    readonly property string menuSectionFont:    uiFont
    readonly property int    menuSectionSize:    textMeta
    readonly property int    menuSectionWeight:  weightStrong
    readonly property real   menuSectionTracking: 0.0
}
