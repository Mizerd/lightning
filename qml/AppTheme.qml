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
    // Settings > Appearance > Accessibility, pushed in from Main.qml.
    // highContrast pulls secondary and muted text toward the primary ink and
    // thickens the border inks; the defaults leave every token untouched.
    property bool highContrast: false
    // Floor for a sender-name ink: 4.5 Strong, 3.0 Weak, 1.0 Off.
    property real nameColorMinContrast: 4.5
    // Timeline row rhythm: 0 Compact, 1 Cozy (the shipped spacing), 2 Roomy.
    property int messageSpacing: 1
    // Pixels added to the gap above a sender's first message, and above each
    // continuation line, per mode. Cozy is 0 so the shipped layout is
    // bit-for-bit what it was. MessageDelegate.messageTopSpacing is the one
    // reader, so all three layouts follow.
    readonly property int messageGroupGapExtra:
        messageSpacing === 0 ? -4 : messageSpacing === 2 ? 6 : 0
    readonly property int messageContinuationGapExtra:
        messageSpacing === 0 ? -1 : messageSpacing === 2 ? 2 : 0
    // Also underline mentions and in-app links (URLs always are).
    property bool underlineLinks: false
    // A token pulled `amount` of the way toward `ink` when high contrast is on.
    function _contrast(base, ink, amount) {
        return highContrast ? Qt.tint(base, Qt.alpha(ink, amount)) : base
    }
    // Includes the UI font's optical correction so text size and font choice
    // stay independent (Manrope, the default, has factor 1.0).
    function scaled(px) { return Math.round(px * textScale * uiFontOptical) }
    // A media clock position, "m:ss". A position FLOORS (at 25.7 s you have
    // not reached 0:26); a total rounds and is each player's own. Shared by the
    // video bar and the video covers' "Resume from" hints.
    function clockPosition(ms) {
        return clockText(Math.floor(Math.max(0, ms || 0) / 1000))
    }
    // One media clock for every player: m:ss under an hour, h:mm:ss from an
    // hour on ("1:02:03", not a growing "62:03"). Callers decide whether a
    // value floors (a position) or rounds (a length) before passing seconds.
    function clockText(totalSeconds) {
        var t = Math.max(0, Math.floor(totalSeconds || 0))
        var h = Math.floor(t / 3600)
        var m = Math.floor((t % 3600) / 60)
        var sec = t % 60
        var ss = (sec < 10 ? "0" : "") + sec
        if (h > 0)
            return h + ":" + (m < 10 ? "0" : "") + m + ":" + ss
        return m + ":" + ss
    }

    // Shared height of the top header strip (room list, room header, side
    // panel) so the rule beneath them lines up across the whole window.
    // Layout preset density (Settings > Appearance > Layout preset: Nheko
    // compact), read straight from the settings so every window and test
    // scene that has `app` follows it without a Binding of its own. Off
    // reproduces the Lightning geometry exactly. It moves chrome geometry only
    // -- header bands, room rows, the composer's outer inset -- and never a
    // text size, so every consumer below keeps the user's fonts and text scale.
    // (The timeline's own density is the preset's message layout and spacing.)
    readonly property bool compactDensity:
        typeof app !== "undefined" && !!app && !!app.settings
        && app.settings.layoutPreset === 1
    // Rail rhythm is shared by its delegate and pointer-band arithmetic.
    readonly property int railTileMax: compactDensity ? 36 : 48
    readonly property int railTileMin: compactDensity ? 32 : 40
    readonly property int railRowPadding: compactDensity ? spacing2 : spacing4
    readonly property int railGroupGap: compactDensity ? spacing6 : spacing8
    readonly property int railGroupBreakGap: compactDensity ? spacing12 + spacing2 : spacing16 + spacing2
    // 48 still fits the 34 px room avatar and the 34 px header buttons with
    // 7 px above and below; Nheko's own header is a 40 px avatar plus padding.
    readonly property int headerBandHeight: compactDensity ? 48 : 60
    // The thread panel's header sits 2 px under the band it lines up with.
    readonly property int threadHeaderHeight: headerBandHeight - 2
    // Vertical padding of one room-list row (above and below its content).
    readonly property int roomRowPaddingV: compactDensity ? spacing4 : spacing6
    // Floor of a Channels row (rooms, folders, navigation rows); each row
    // also keeps two lines of its own text size, so text never clips.
    readonly property int channelRowMinHeight: compactDensity ? 28 : 32
    // The message composer's distance from the conversation's edges.
    readonly property int composerInsetH: compactDensity ? spacing12 : spacing20
    readonly property int composerInsetBottom: compactDensity ? spacing8 : spacing16
    // Platform dark-mode hint (QStyleHints::colorScheme), pushed in from
    // Main.qml.
    property bool systemDark: false
    // Custom theme (id 12): a sparse role -> "#RRGGBB" map laid over
    // `customBase`'s palette. CustomThemeStore::sanitize() already filters it;
    // _custom re-checks because the config file is user-editable.
    property var customOverrides: ({})
    property int customBase: 11
    // The custom theme's surface gradients: role -> { type, angle, stops },
    // already sanitised by CustomThemeStore::sanitizeGradients(). Pushed in
    // from Main.qml; applies only while theme 12 is in use.
    property var customGradients: ({})
    // Settings -> Appearance "Depth" (0 flat, 1 depth), pushed in from
    // Main.qml. Gives the shell's large surfaces a two-stop gradient moved
    // AWAY from the ink, so text contrast can only rise (backdrop::depthStops,
    // proven on every preset by ChatBackdropTest).
    property int surfaceDepth: 0

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
    // WCAG contrast of two colours (alpha ignored).
    function contrastRatio(a, b) {
        var la = relativeLuminance(_asColor(a))
        var lb = relativeLuminance(_asColor(b))
        return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
    }
    function relativeLuminance(c) {
        function lin(v) {
            return v <= 0.03928 ? v / 12.92
                                : Math.pow((v + 0.055) / 1.055, 2.4)
        }
        return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b)
    }

    // ── Badge colours (2026-10-20) ──
    // A badge's fill, ink and dot are DERIVED from the theme's (or a custom
    // theme's) own colours: the same hue and saturation, moved in HSL
    // lightness only, as little as it takes to read. The base colours stay
    // free for highlights, washes and danger tints.

    // A colour as it shows over `base` (an opaque colour; a translucent
    // surface such as Storm's hover is composited first).
    function _over(c, base) {
        var f = _asColor(c)
        if (f.a >= 1)
            return f
        var b = _asColor(base)
        return Qt.rgba(f.r * f.a + b.r * (1 - f.a), f.g * f.a + b.g * (1 - f.a),
                       f.b * f.a + b.b * (1 - f.a), 1)
    }
    function _ratio(a, b) {
        var la = relativeLuminance(_asColor(a)) + 0.05
        var lb = relativeLuminance(_asColor(b)) + 0.05
        return la > lb ? la / lb : lb / la
    }
    // The lowest ratio of `c` against every colour in `against`.
    function _worstRatio(c, against) {
        var worst = Infinity
        for (var i = 0; i < against.length; ++i)
            worst = Math.min(worst, _ratio(c, against[i]))
        return worst
    }
    // `fill` moved in lightness, trying BOTH directions in growing steps,
    // until it reads at `minRatio` against everything in `against`. The
    // smallest such move wins; if none reaches it, the move that reads best
    // (never a silent pure white or black). Returns {color, shift, ok}.
    function _lightnessFit(fill, against, minRatio) {
        var f = _asColor(fill)
        var best = { color: f, shift: 0, ok: false, worst: _worstRatio(f, against) }
        if (best.worst >= minRatio) {
            best.ok = true
            return best
        }
        var h = f.hslHue < 0 ? 0 : f.hslHue
        var sat = f.hslSaturation
        var l = f.hslLightness
        for (var i = 1; i <= 200; ++i) {
            var step = i * 0.005
            var tries = [l - step, l + step]
            for (var k = 0; k < 2; ++k) {
                var nl = tries[k]
                if (nl < 0 || nl > 1)
                    continue
                var c = Qt.hsla(h, sat, nl, 1)
                var w = _worstRatio(c, against)
                if (w >= minRatio)
                    return { color: c, shift: step, ok: true, worst: w }
                if (w > best.worst)
                    best = { color: c, shift: step, ok: false, worst: w }
            }
        }
        return best
    }
    // A badge FILL its ink reads on at `minRatio` (the smallest lightness
    // move, either way).
    function inkSafeFill(fill, ink, minRatio) {
        return _lightnessFit(fill, [ink], minRatio).color
    }
    // The ink a badge's text uses: the theme's own ink, unless keeping it
    // would move the fill by more than _badgeMaxShift (a yellow, or an orange
    // such as #FF8800, under white text) while the opposite ink needs less;
    // then the opposite ink, so the colour is kept, as Storm's dark-on-light
    // badges do. The choice is made at the default text target (4.5:1)
    // whatever `minRatio` the fill is then fitted to, so high contrast moves
    // the fill further but never swaps the ink: every shipped preset keeps its
    // own ink with high contrast on as off, and #FF8800 takes the dark ink in
    // both (a single threshold at 7:1 cannot tell them apart).
    function badgeInk(fill, ink, minRatio) {
        var own = _lightnessFit(fill, [ink], _badgeInkChoiceRatio)
        if (own.ok && own.shift <= _badgeMaxShift)
            return _asColor(ink)
        var other = relativeLuminance(_asColor(ink)) > 0.18 ? _badgeDarkInk : _onAccent
        var alt = _lightnessFit(fill, [other], _badgeInkChoiceRatio)
        if (alt.ok && (!own.ok || alt.shift < own.shift))
            return _asColor(other)
        return _asColor(ink)
    }
    // WCAG targets, with a margin for 8-bit rounding: text 4.5:1 (7:1 with
    // high contrast on), and a text-less dot 3:1 (4.5:1).
    readonly property real _badgeInkRatio: highContrast ? 7.1 : 4.6
    readonly property real _badgeDotRatio: highContrast ? 4.6 : 3.1
    // A text-less dot keeps the theme's own (saturated) colour and carries
    // its 3:1 (4.5:1) in a thin ring around it, thicker with high contrast.
    readonly property real badgeDotRingWidth: highContrast ? 2.5 : 1.5
    // How far (HSL lightness) a count's fill may move under its own ink
    // before the opposite ink is chosen instead.
    readonly property real _badgeMaxShift: 0.14
    readonly property real _badgeInkChoiceRatio: 4.6
    readonly property color _badgeDarkInk: "#0B0D12"

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
        var mentionBadgeText = p.mentionBadgeText !== undefined
                ? p.mentionBadgeText : dangerText
        var out = {
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
            ownBubbleText:   p.ownBubbleText !== undefined ? p.ownBubbleText
                                                           : _ownBubbleTextDefault,
            otherBubble:     p.otherBubble,
            // Element roles whose stock colour is not their group's on Storm
            // (or that have no group) are spelled out; the rest come from
            // `elementGroups` below.
            embedSurface:    p.embedSurface !== undefined ? p.embedSurface
                             : (isStorm ? _stoPanel : cardElevated),
            reactionBackground: p.reaction !== undefined ? p.reaction
                                                         : cardElevated,
            reactionBorder:  p.reactionBorder !== undefined ? p.reactionBorder
                             : (isDark ? p.border : p.borderStrong),
            mentionBadgeText: mentionBadgeText,
            chipDanger:      p.chipDanger !== undefined ? p.chipDanger
                             : (isStorm ? _stoDanger
                                : (p.dangerTint !== undefined ? p.dangerTint
                                                              : mentionBadge)),
            menuDangerTint:  p.menuDangerTint !== undefined ? p.menuDangerTint
                             : (isStorm ? _stoDanger
                                : (p.dangerTint !== undefined ? p.dangerTint
                                                              : mentionBadge)),
            menuDangerText:  p.menuDangerText !== undefined ? p.menuDangerText
                             : (isStorm ? _stoDanger
                                : (p.danger !== undefined ? p.danger
                                   : (isDark ? _dangerInkDark : _dangerInkLight))),
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
                                                              : _dangerInkLight),
            // Roles split out of a shared token (2026-10-06). Each falls back
            // to the token it used to be, so a theme that never set one
            // looks exactly as before.
            warning:         p.warning !== undefined ? p.warning
                                                     : (isDark ? _warnInkDark
                                                               : _warnInkLight),
            popoverSurface:  p.popoverSurface !== undefined ? p.popoverSurface
                                                            : p.surface,
            focusRing:       p.focusRing !== undefined ? p.focusRing : accent,
            scrollbarHandle: p.scrollbarHandle !== undefined
                             ? p.scrollbarHandle : p.borderStrong,
            reactionSelectedBackground:
                p.reactionSelectedBackground !== undefined
                ? p.reactionSelectedBackground
                : (p.accentSoft !== undefined ? p.accentSoft : p.selected),
            presenceOnline:  p.presenceOnline !== undefined ? p.presenceOnline
                             : (p.online !== undefined ? p.online
                                : (p.success !== undefined ? p.success
                                   : (isDark ? _okInkDark : _okInkLight))),
            // Round 2 (2026-10-06): shared tokens split per element. Same rule.
            dangerTint:      p.dangerTint !== undefined ? p.dangerTint
                                                        : mentionBadge,
            messageHighlight: p.messageHighlight !== undefined
                              ? p.messageHighlight : p.selected,
            textSelection:   p.textSelection !== undefined ? p.textSelection
                                                           : p.selectedHover,
            paletteHighlight: p.paletteHighlight !== undefined
                              ? p.paletteHighlight : p.selected,
            roomSelected:    p.roomSelected !== undefined ? p.roomSelected
                                                          : p.selected,
            roomHover:       p.roomHover !== undefined ? p.roomHover : p.hover,
            channelSelected: p.channelSelected !== undefined
                             ? p.channelSelected : p.selected,
            channelHover:    p.channelHover !== undefined ? p.channelHover
                                                          : p.hover,
            menuHighlight:   p.menuHighlight !== undefined ? p.menuHighlight
                             : (isStorm ? p.selected : p.hover),
            buttonGhostHover: p.buttonGhostHover !== undefined
                              ? p.buttonGhostHover : p.hover,
            timestampInk:    p.timestampInk !== undefined ? p.timestampInk
                                                          : textMuted,
            placeholderInk:  p.placeholderInk !== undefined ? p.placeholderInk
                                                            : textMuted,
            settingsPage:    p.settingsPage !== undefined ? p.settingsPage
                                                          : p.background,
            settingsNav:     p.settingsNav !== undefined ? p.settingsNav
                             : (isStorm ? p.sidebar : p.background)
        }
        // Per-element roles (2026-10-13): the palette's own value, else the
        // resolved colour of the group the element follows. One loop over one
        // table, run once per palette build (not per frame).
        var groups = elementGroups
        for (var e in groups) {
            if (out[e] === undefined)
                out[e] = p[e] !== undefined ? p[e] : out[groups[e]]
        }
        // The derived badge colours, by the live rules (mentionBadgeInk,
        // mentionBadgeFill, roomMentionBadge, unreadCountInk, unreadCountFill,
        // unreadDot, mentionDot), from this palette's own resolved roles.
        out.mentionBadgeInk = p.mentionBadgeInk !== undefined ? p.mentionBadgeInk
                : badgeInk(out.mentionBadge, out.mentionBadgeText, _badgeInkRatio)
        out.mentionBadgeFill = p.mentionBadgeFill !== undefined ? p.mentionBadgeFill
                : inkSafeFill(out.mentionBadge, out.mentionBadgeInk, _badgeInkRatio)
        if (p.roomMentionBadge === undefined)
            out.roomMentionBadge = out.mentionBadgeFill
        out.unreadCountInk = p.unreadCountInk !== undefined ? p.unreadCountInk
                : badgeInk(out.roomUnreadBadge, out.unreadBadgeText, _badgeInkRatio)
        out.unreadCountFill = p.unreadCountFill !== undefined ? p.unreadCountFill
                : inkSafeFill(out.roomUnreadBadge, out.unreadCountInk, _badgeInkRatio)
        var dotSurfaces = badgeDotSurfaces({
            rail: out.rail !== undefined ? out.rail : out.sidebar,
            stormPanel: isStorm ? _stoPanel : out.surface,
            stormCanvas: isStorm ? _stoCanvas : out.background,
            roomList: out.roomList, roomHover: out.roomHover,
            roomSelected: out.roomSelected, channelHover: out.channelHover,
            channelSelected: out.channelSelected, sidePanel: out.sidePanel,
            roomHeader: out.roomHeader, timelineBackground: out.timelineBackground
        })
        out.unreadDot = p.unreadDot !== undefined ? p.unreadDot : out.roomUnreadBadge
        out.mentionDot = p.mentionDot !== undefined ? p.mentionDot : out.mentionBadge
        out.unreadDotRing = _lightnessFit(out.unreadDot, dotSurfaces, _badgeDotRatio).color
        out.mentionDotRing = _lightnessFit(out.mentionDot, dotSurfaces, _badgeDotRatio).color
        out.railUnreadDotRing = _lightnessFit(out.railUnreadDot, dotSurfaces,
                                              _badgeDotRatio).color
        if (p.channelUnreadMark === undefined)
            out.channelUnreadMark = _lightnessFit(out.accent, dotSurfaces, _badgeDotRatio).color
        return out
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
    /// A folder's open/close arrow: its own role since 2026-10-13, falling
    /// back to what the category text falls back to.
    readonly property color channelChevron:
        _p.channelChevron !== undefined ? _p.channelChevron : textMuted
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
    /// Unread mark on a channel row: the accent moved in lightness only until
    /// it reads at 3:1 (4.5:1 high contrast) on every row state, unless a
    /// palette sets its own. Storm's bolt-yellow accent already reads 7.53:1,
    /// so it is not moved.
    readonly property color channelUnreadMark:
        _p.channelUnreadMark !== undefined ? _p.channelUnreadMark
                                           : _lightnessFit(accent, badgeDotSurfaces(root),
                                                           _badgeDotRatio).color

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
    // The tint behind destructive rows and the danger chip. Its own role since
    // 2026-10-06; follows the mention pill until a custom theme sets it.
    readonly property color dangerTint:          _p.dangerTint !== undefined
                                                 ? _p.dangerTint : mentionBadge
    readonly property color dangerSoft:          Qt.alpha(dangerTint, 0.10)
    readonly property color dangerBorder:        Qt.alpha(dangerTint, 0.25)
    readonly property color textPrimary:         _p.textPrimary
    readonly property color textSecondary:       _contrast(_p.textSecondary,
                                                           _p.textPrimary, 0.7)
    readonly property color textMuted:           _contrast(_p.textMuted,
                                                           _p.textPrimary, 0.75)
    readonly property color textDisabled:        _p.textDisabled
    readonly property color border:              _contrast(_p.border,
                                                           _p.textPrimary, 0.45)
    readonly property color borderSubtle:        border
    readonly property color borderStrong:        _contrast(_p.borderStrong,
                                                           _p.textPrimary, 0.7)
    readonly property color separator:           border
    readonly property color inputBackground:     _p.inputBg
    readonly property color inputBorder:         border
    readonly property color focusRing:           _p.focusRing !== undefined
                                                 ? _p.focusRing : _p.accent
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
    // White unless a custom theme sets its own ink; the literal stays named so
    // the contrast table can read it.
    readonly property color _ownBubbleTextDefault: "#FFFFFF"
    readonly property color ownBubbleText:       _p.ownBubbleText !== undefined
                                                 ? _p.ownBubbleText
                                                 : _ownBubbleTextDefault
    readonly property color onAccentMuted:       "#DCE4FF"
    readonly property color otherMessageBubble:  _p.otherBubble
    readonly property color otherBubble:         otherMessageBubble
    // Message words everywhere but on your own bubble (an element role since
    // 2026-10-13; it was `text`).
    readonly property color otherBubbleText:     _p.otherBubbleText !== undefined
                                                 ? _p.otherBubbleText : textPrimary
    readonly property color incomingBubble:      otherMessageBubble
    readonly property color outgoingBubble:      ownMessageBubble
    readonly property color bubbleOverlay:       "#26000000"
    readonly property color bubbleOverlaySubtle: "#0F000000"
    // In-timeline embed cards (voice, polls, link previews, file and video
    // placeholders) sit on the timeline ground, not a panel, so they use a
    // raised-but-dark step rather than a pale block.
    readonly property color embedSurface:        _p.embedSurface !== undefined
                                                 ? _p.embedSurface
                                                 : (storm ? _stoPanel : cardElevated)
    // The card's edge and title. The cards always drew `border` and `text`
    // (the Storm panel edge this used to name had no reader).
    readonly property color embedBorder:         _p.embedBorder !== undefined
                                                 ? _p.embedBorder : border
    readonly property color embedText:           _p.embedText !== undefined
                                                 ? _p.embedText : textPrimary

    readonly property color reactionBackground:  _p.reaction !== undefined
                                                 ? _p.reaction : cardElevated
    // Someone else's pill at rest: a hairline on dark themes and a strong
    // line on light ones, so it follows no single group.
    readonly property color reactionBorder:      _p.reactionBorder !== undefined
                                                 ? _p.reactionBorder
                                                 : (dark ? border : borderStrong)
    readonly property color reactionInk:         _p.reactionInk !== undefined
                                                 ? _p.reactionInk : textSecondary
    readonly property color reactionSelectedBackground:
        _p.reactionSelectedBackground !== undefined
        ? _p.reactionSelectedBackground : accentSoft
    // Your own pill's 2px edge (the pill always drew `accent`).
    readonly property color reactionSelectedBorder:
        _p.reactionSelectedBorder !== undefined ? _p.reactionSelectedBorder
                                                : accent
    readonly property color reactionSelectedInk:
        _p.reactionSelectedInk !== undefined ? _p.reactionSelectedInk
                                             : textPrimary
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
    // What MessageHtml actually paints for a mention of you (and @room): bold
    // ink, no chip. MainScreen hands it to the timeline as the mention accent.
    readonly property color selfMentionInk:      _p.selfMentionInk !== undefined
                                                 ? _p.selfMentionInk : accent
    readonly property color undecryptableText:   textMuted
    // Jumped-to message rows and active thread affordances.
    readonly property color pressedSurface:      selectedHover
    readonly property color messageHighlight:    _p.messageHighlight !== undefined
                                                 ? _p.messageHighlight : selected
    // Text selected inside a field or label.
    readonly property color textSelection:       _p.textSelection !== undefined
                                                 ? _p.textSelection
                                                 : selectedHover
    // The `highlight` of the Qt control palette (Main.qml).
    readonly property color paletteHighlight:    _p.paletteHighlight !== undefined
                                                 ? _p.paletteHighlight : selected
    // The open room's row, and a row of the room list under the pointer.
    readonly property color roomSelected:        _p.roomSelected !== undefined
                                                 ? _p.roomSelected : selected
    readonly property color roomHover:           _p.roomHover !== undefined
                                                 ? _p.roomHover : hover
    // Timestamps beside messages, and the hint inside an empty field.
    readonly property color timestampInk:        _p.timestampInk !== undefined
                                                 ? _p.timestampInk : textMuted
    readonly property color placeholderInk:      _p.placeholderInk !== undefined
                                                 ? _p.placeholderInk
                                                 : stormTextMuted
    readonly property color threadHighlight:     accent
    // Presence dots: online green, away amber, offline muted.
    readonly property color presenceOnline:      _p.presenceOnline !== undefined
                                                 ? _p.presenceOnline
                                                 : (_p.online !== undefined
                                                    ? _p.online : success)
    // Kept clear of the Storm bolt hue (see _awayLight).
    readonly property color presenceAway:        _p.away !== undefined
                                                 ? _p.away
                                                 : (dark ? _awayDark
                                                         : _awayLight)
    readonly property color presenceOffline:     textMuted

    // ---- Control surfaces ----
    // Theme-routed hover/pressed steps for buttons, icon chips and segmented
    // controls. Use these rather than Qt.darker()/lighter() at call sites.
    readonly property color buttonPrimaryFill:    _p.buttonPrimaryFill !== undefined
                                                  ? _p.buttonPrimaryFill : accent
    readonly property color buttonPrimaryHover:   accentHover
    readonly property color buttonPrimaryPressed: accentPressed
    readonly property color buttonPrimaryInk:     _p.buttonPrimaryInk !== undefined
                                                  ? _p.buttonPrimaryInk : accentText
    // The composer's send button: a filled IconButton, primary until a theme
    // gives it its own colours.
    readonly property color sendButton:           _p.sendButton !== undefined
                                                  ? _p.sendButton : accent
    readonly property color sendButtonInk:        _p.sendButtonInk !== undefined
                                                  ? _p.sendButtonInk : accentText
    readonly property color buttonNeutralFill:    cardElevated
    readonly property color buttonNeutralHover:   selected
    readonly property color buttonNeutralPressed: selectedHover
    readonly property color buttonNeutralInk:     textPrimary
    readonly property color buttonNeutralBorder:  border
    // AppButton's secondary kind. Its resting state has to read as a control
    // on a card before hover: the old transparent field inside a `border`
    // hairline was the faint Rename/Save of Space Home. A translucent tint of
    // the label ink works on every surface it can sit on, and the edge is the
    // control-strength border, not the divider one.
    readonly property color buttonSecondaryFill:  _p.buttonSecondaryFill !== undefined
                                                  ? _p.buttonSecondaryFill
                                                  : Qt.alpha(buttonNeutralInk, 0.06)
    readonly property color buttonSecondaryBorder: _p.buttonSecondaryBorder !== undefined
                                                  ? _p.buttonSecondaryBorder : borderStrong
    // Ghost: no resting fill.
    readonly property color buttonGhostHover:     _p.buttonGhostHover !== undefined
                                                  ? _p.buttonGhostHover : hover
    readonly property color buttonGhostPressed:   selected
    readonly property color buttonGhostInk:       _p.buttonGhostInk !== undefined
                                                  ? _p.buttonGhostInk : textSecondary
    readonly property color buttonDangerFill:     dangerFill
    readonly property color buttonDangerHover:    dangerFillHover
    readonly property color buttonDangerPressed:  dangerFillPressed
    readonly property color buttonDangerInk:      dangerText
    // Disabled controls are exempt from WCAG contrast, so textDisabled is
    // allowed here and nowhere else.
    readonly property color buttonDisabledFill:   cardElevated
    readonly property color buttonDisabledInk:    _p.buttonDisabledInk !== undefined
                                                  ? _p.buttonDisabledInk : textDisabled
    readonly property color buttonDisabledBorder: _p.buttonDisabledBorder !== undefined
                                                  ? _p.buttonDisabledBorder : border
    // A disabled AppButton's resting fill: the disabled ink at a low alpha,
    // so the shape survives on a card whose colour equals buttonDisabledFill.
    readonly property color buttonDisabledTint:   Qt.alpha(buttonDisabledInk, 0.08)
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
    readonly property color scrollbarHandle:        _p.scrollbarHandle !== undefined
                                                    ? _p.scrollbarHandle
                                                    : borderStrong
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
    // Success / failure text on the scrim (the media viewers' save notice).
    // The theme's success/danger inks are tuned for the theme's own surface
    // and fall to ~2.6:1 on this always-dark one in light themes; these clear
    // 4.5:1 on _scrimBase (#111111): 10.5:1 and 10.3:1.
    readonly property color scrimSuccessInk:     "#63D6A3"
    readonly property color scrimDangerInk:      "#FFA7AF"

    // ---- Storm surface language ---- Shared tokens for menus, popovers,
    // pickers, dialogs and Settings. Under Storm they carry the Storm literals;
    // under any other theme they resolve to that theme's semantic equivalent.
    // Consumers use storm* by role and never branch on the theme.
    readonly property color stormCanvas:        storm ? _stoCanvas : background
    readonly property color stormPanel:         storm ? _stoPanel : surface
    // Menus, pop-up menus and dialog boxes. A custom theme may set its own;
    // otherwise this IS stormPanel, so every other theme is unchanged.
    readonly property color popoverSurface:     _p.popoverSurface !== undefined
                                                ? _p.popoverSurface : stormPanel
    // The highlighted item of a menu or list popup (the same colour as
    // stormSelection until a custom theme sets it).
    readonly property color menuHighlight:      _p.menuHighlight !== undefined
                                                ? _p.menuHighlight
                                                : stormSelection
    // The Settings page ground and its navigation column.
    readonly property color settingsPage:       _p.settingsPage !== undefined
                                                ? _p.settingsPage : stormDeep
    readonly property color settingsNav:        _p.settingsNav !== undefined
                                                ? _p.settingsNav : stormCanvas

    // ---- Per-element roles (2026-10-13) ----
    //
    // One token per element the theme editor's preview draws, so each can be
    // recoloured alone. Each falls back to EXACTLY the token its views read
    // before it existed, so every preset paints as it did; for a custom theme
    // (never Storm, so a storm* token is its plain twin) that fallback is the
    // group CustomThemeStore's kRoleFollows names. paletteForTheme() resolves
    // the same roles through `elementGroups` below.

    // Shell surfaces.
    readonly property color roomList:           _p.roomList !== undefined
                                                ? _p.roomList : sidebar
    // The header band meets the side columns' top edge, gradient included.
    // Read from the group, not from `roomList`, so giving the room list a
    // colour of its own leaves the header alone.
    readonly property color roomHeader:         _p.roomHeader !== undefined
                                                ? _p.roomHeader
                                                : surfaceColorAt("sidebar", 0)
    readonly property color sidePanel:          _p.sidePanel !== undefined
                                                ? _p.sidePanel : sidebar
    readonly property color timelineBackground: _p.timelineBackground !== undefined
                                                ? _p.timelineBackground : background
    readonly property color searchField:        _p.searchField !== undefined
                                                ? _p.searchField : surface
    readonly property color composerField:      _p.composerField !== undefined
                                                ? _p.composerField : surface

    // Lines.
    readonly property color headerDivider:      _p.headerDivider !== undefined
                                                ? _p.headerDivider : border
    readonly property color searchFieldBorder:  _p.searchFieldBorder !== undefined
                                                ? _p.searchFieldBorder : border
    readonly property color composerBorder:     _p.composerBorder !== undefined
                                                ? _p.composerBorder : border
    readonly property color fieldBorder:        _p.fieldBorder !== undefined
                                                ? _p.fieldBorder : border
    // A field drawn in the Storm style reads the Storm edge under Storm.
    readonly property color stormFieldBorder:   storm ? _stoBorder : fieldBorder
    readonly property color codeBlockBorder:    _p.codeBlockBorder !== undefined
                                                ? _p.codeBlockBorder : border
    readonly property color channelFolderBorder: _p.channelFolderBorder !== undefined
                                                 ? _p.channelFolderBorder : border
    readonly property color settingsDivider:    _p.settingsDivider !== undefined
                                                ? _p.settingsDivider : stormBorder
    readonly property color menuBorder:         _p.menuBorder !== undefined
                                                ? _p.menuBorder : stormBorderStrong

    // States.
    readonly property color railTile:           _p.railTile !== undefined
                                                ? _p.railTile : cardElevated
    readonly property color roomSelectedHover:  _p.roomSelectedHover !== undefined
                                                ? _p.roomSelectedHover : selectedHover
    readonly property color settingsNavSelected:
        _p.settingsNavSelected !== undefined ? _p.settingsNavSelected
                                             : selectedHover
    // The caret and icon of the SELECTED Settings nav row. The accent (bolt)
    // where it keeps 3:1 against the selection fill, the row's text ink where
    // it does not: on Nordic the accent #5E81AC sat on #587197 (1.1:1) and
    // both marks vanished (live, 2026-10-26).
    readonly property color settingsNavSelectedMark:
        contrastRatio(bolt, settingsNavSelected) >= 3.0 ? bolt : stormText
    readonly property color memberRowHover:     _p.memberRowHover !== undefined
                                                ? _p.memberRowHover : hover
    readonly property color roomUnreadBadge:    _p.roomUnreadBadge !== undefined
                                                ? _p.roomUnreadBadge : unreadBadge
    readonly property color railUnreadDot:      _p.railUnreadDot !== undefined
                                                ? _p.railUnreadDot : unreadBadge
    // Every surface a text-less unread or mention DOT sits on: the Spaces
    // rail, the account menu and its panels, room-list rows at rest, hovered and selected (both
    // layouts), the side panel, the room header and the conversation. A
    // translucent surface is taken as it shows over the room list.
    function badgeDotSurfaces(t) {
        return [ t.rail, t.stormPanel, t.stormCanvas, t.roomList,
                 _over(t.roomHover, t.roomList), _over(t.roomSelected, t.roomList),
                 _over(t.channelHover, t.roomList), _over(t.channelSelected, t.roomList),
                 _over(t.sidePanel, t.roomList), _over(t.roomHeader, t.roomList),
                 _over(t.timelineBackground, t.roomList) ]
    }
    // Every unread DOT (room rows, threads, the Threads button, Home,
    // account badges): FILLED with the theme's own unread colour, saturated,
    // as the count pills are, and drawn with a thin ring (unreadDotRing,
    // badgeDotRingWidth) that carries the 3:1 (4.5:1 high contrast) against
    // every surface above. Follows a custom theme's unread colour.
    readonly property color unreadDot:          _p.unreadDot !== undefined
                                                ? _p.unreadDot : roomUnreadBadge
    // A mention shown as a DOT (no number: the Spaces rail's room dot, the
    // Threads button): the theme's mention colour, with its own ring.
    readonly property color mentionDot:         _p.mentionDot !== undefined
                                                ? _p.mentionDot : mentionBadge
    // The rings: the dot's own colour moved in lightness only until it reads
    // at 3:1 (4.5:1) on every dot surface, so unread and mention rings keep
    // their two hues. Equal to the fill where the fill already reads.
    readonly property color unreadDotRing:      _lightnessFit(unreadDot,
                                                              badgeDotSurfaces(root),
                                                              _badgeDotRatio).color
    readonly property color mentionDotRing:     _lightnessFit(mentionDot,
                                                              badgeDotSurfaces(root),
                                                              _badgeDotRatio).color
    readonly property color railUnreadDotRing:  _lightnessFit(railUnreadDot,
                                                              badgeDotSurfaces(root),
                                                              _badgeDotRatio).color
    // The "New messages" divider's words: the unread colour, moved in
    // lightness only until it reads as text (4.5:1; 7:1 high contrast) on the
    // conversation background. Its rules keep unreadBadge.
    readonly property color unreadDividerInk:   _lightnessFit(unreadBadge,
                                                              [timelineBackground],
                                                              _badgeInkRatio).color
    // Every unread COUNT pill: its number's ink (unreadBadgeText unless that
    // would move the fill too far; see badgeInk) and the fill it reads on at
    // 4.5:1 (7:1 high contrast), the unread colour moved in lightness only.
    readonly property color unreadCountInk:     _p.unreadCountInk !== undefined
                                                ? _p.unreadCountInk
                                                : badgeInk(roomUnreadBadge, unreadBadgeText,
                                                           _badgeInkRatio)
    readonly property color unreadCountFill:    _p.unreadCountFill !== undefined
                                                ? _p.unreadCountFill
                                                : inkSafeFill(roomUnreadBadge, unreadCountInk,
                                                              _badgeInkRatio)

    // Accent.
    readonly property color railTileActive:     _p.railTileActive !== undefined
                                                ? _p.railTileActive : accentSoft
    readonly property color callBanner:         _p.callBanner !== undefined
                                                ? _p.callBanner : accentSoft
    readonly property color railActiveRing:     _p.railActiveRing !== undefined
                                                ? _p.railActiveRing : accent
    readonly property color unreadBadgeText:    _p.unreadBadgeText !== undefined
                                                ? _p.unreadBadgeText : accentText

    // Mention counts.
    // Every mention badge's fill (room list, Spaces rail, Home, threads, the
    // timeline's thread button, account badges): the mention colour, darker
    // or lighter only as far as its count needs to read at 4.5:1. The base
    // `mentionBadge` stays the theme's (and a custom theme's) mention colour
    // for everything that is not a badge.
    readonly property color mentionBadgeInk:    _p.mentionBadgeInk !== undefined
                                                ? _p.mentionBadgeInk
                                                : badgeInk(mentionBadge, mentionBadgeText,
                                                           _badgeInkRatio)
    readonly property color mentionBadgeFill:   _p.mentionBadgeFill !== undefined
                                                ? _p.mentionBadgeFill
                                                : inkSafeFill(mentionBadge, mentionBadgeInk,
                                                              _badgeInkRatio)
    readonly property color roomMentionBadge:   _p.roomMentionBadge !== undefined
                                                ? _p.roomMentionBadge : mentionBadgeFill
    readonly property color mentionBadgeText:   _p.mentionBadgeText !== undefined
                                                ? _p.mentionBadgeText : dangerText

    // Inks.
    readonly property color roomListTitle:      _p.roomListTitle !== undefined
                                                ? _p.roomListTitle : textPrimary
    readonly property color roomHeaderTitle:    _p.roomHeaderTitle !== undefined
                                                ? _p.roomHeaderTitle : textPrimary
    readonly property color roomNameUnread:     _p.roomNameUnread !== undefined
                                                ? _p.roomNameUnread : textPrimary
    readonly property color codeText:           _p.codeText !== undefined
                                                ? _p.codeText : textPrimary
    readonly property color composerText:       _p.composerText !== undefined
                                                ? _p.composerText : textPrimary
    readonly property color fieldText:          _p.fieldText !== undefined
                                                ? _p.fieldText : textPrimary
    readonly property color stormFieldText:     storm ? _stoText : fieldText
    readonly property color menuItemActiveText: _p.menuItemActiveText !== undefined
                                                ? _p.menuItemActiveText : stormText
    readonly property color callBannerText:     _p.callBannerText !== undefined
                                                ? _p.callBannerText : textPrimary
    readonly property color settingsHeading:    _p.settingsHeading !== undefined
                                                ? _p.settingsHeading : stormText
    readonly property color roomName:           _p.roomName !== undefined
                                                ? _p.roomName : textSecondary
    readonly property color menuItemText:       _p.menuItemText !== undefined
                                                ? _p.menuItemText : stormTextSecondary
    readonly property color settingsNavText:    _p.settingsNavText !== undefined
                                                ? _p.settingsNavText : stormTextSecondary
    readonly property color roomPreview:        _p.roomPreview !== undefined
                                                ? _p.roomPreview : textMuted
    readonly property color roomListSection:    _p.roomListSection !== undefined
                                                ? _p.roomListSection : sectionLabelColor
    readonly property color sidePanelSection:   _p.sidePanelSection !== undefined
                                                ? _p.sidePanelSection : stormTextMuted
    readonly property color settingsText:       _p.settingsText !== undefined
                                                ? _p.settingsText : stormTextMuted
    readonly property color roomSelectedName:   _p.roomSelectedName !== undefined
                                                ? _p.roomSelectedName : selectedText
    readonly property color roomSelectedPreview:
        _p.roomSelectedPreview !== undefined ? _p.roomSelectedPreview
                                             : selectedText
    readonly property color paletteHighlightText:
        _p.paletteHighlightText !== undefined ? _p.paletteHighlightText
                                              : selectedText

    // Status.
    readonly property color presenceDot:        _p.presenceDot !== undefined
                                                ? _p.presenceDot : presenceOnline
    readonly property color chipSuccess:        _p.chipSuccess !== undefined
                                                ? _p.chipSuccess
                                                : (storm ? stormSuccess : presenceOnline)
    // A chip drawn in the Storm style keeps the Storm ink it always had until
    // a theme sets the chip itself.
    readonly property color stormChipSuccess:   _p.chipSuccess !== undefined
                                                ? _p.chipSuccess : stormSuccess
    readonly property color stormChipDanger:    _p.chipDanger !== undefined
                                                ? _p.chipDanger : stormDanger
    readonly property color chipWarning:        _p.chipWarning !== undefined
                                                ? _p.chipWarning : warning
    readonly property color chipDanger:         _p.chipDanger !== undefined
                                                ? _p.chipDanger
                                                : (storm ? stormDanger : dangerTint)
    readonly property color menuDangerTint:     _p.menuDangerTint !== undefined
                                                ? _p.menuDangerTint
                                                : (storm ? _stoDanger : dangerTint)
    readonly property color menuDangerText:     _p.menuDangerText !== undefined
                                                ? _p.menuDangerText : stormDanger

    // Element role -> the paletteForTheme() key of its group: the resolver's
    // copy of CustomThemeStore's kRoleFollows for these roles (the suite holds
    // the two equal). Parents before children: a group here is never itself
    // an element of this table.
    readonly property var elementGroups: ({
        roomList: "sidebar", roomHeader: "sidebar", sidePanel: "sidebar",
        timelineBackground: "background",
        searchField: "surface", composerField: "surface",
        headerDivider: "border", searchFieldBorder: "border",
        composerBorder: "border", fieldBorder: "border",
        embedBorder: "border", codeBlockBorder: "border",
        channelFolderBorder: "border", settingsDivider: "border",
        buttonDisabledBorder: "border",
        menuBorder: "borderStrong",
        railTile: "cardElevated",
        roomSelectedHover: "selectedHover", settingsNavSelected: "selectedHover",
        memberRowHover: "hover",
        roomUnreadBadge: "unreadBadge", railUnreadDot: "unreadBadge",
        railTileActive: "accentSoft", callBanner: "accentSoft",
        railActiveRing: "accent", reactionSelectedBorder: "accent",
        selfMentionInk: "accent", buttonPrimaryFill: "accent",
        sendButton: "accent",
        unreadBadgeText: "accentText", buttonPrimaryInk: "accentText",
        sendButtonInk: "accentText",
        roomMentionBadge: "mentionBadge",
        otherBubbleText: "textPrimary", roomListTitle: "textPrimary",
        roomHeaderTitle: "textPrimary", roomNameUnread: "textPrimary",
        codeText: "textPrimary", embedText: "textPrimary",
        reactionSelectedInk: "textPrimary", composerText: "textPrimary",
        fieldText: "textPrimary", menuItemActiveText: "textPrimary",
        callBannerText: "textPrimary", settingsHeading: "textPrimary",
        roomName: "textSecondary", channelText: "textSecondary",
        menuItemText: "textSecondary", reactionInk: "textSecondary",
        buttonGhostInk: "textSecondary", settingsNavText: "textSecondary",
        roomPreview: "textMuted", roomListSection: "textMuted",
        sidePanelSection: "textMuted", channelCategoryText: "textMuted",
        channelChevron: "textMuted", settingsText: "textMuted",
        roomSelectedName: "selectedText", roomSelectedPreview: "selectedText",
        channelSelectedText: "selectedText",
        paletteHighlightText: "selectedText",
        buttonDisabledInk: "textDisabled",
        presenceDot: "presenceOnline", chipSuccess: "presenceOnline",
        chipWarning: "warning",
        menuDangerTint: "dangerTint", chipDanger: "dangerTint",
        menuDangerText: "danger"
    })

    // ---- Surface gradients ("Depth" and custom-theme gradients) ----
    //
    // ThemedSurface paints through gradientFor(); everything else keeps
    // reading the flat token, which stays the role's colour of record (and
    // what the readability table grades alongside the worst stop).

    // The roles "Depth" moves: the shell's large grounds. The conversation's
    // ground is `timelineBackground` since it became its own element; the
    // `background` group keeps Depth for anything still painted in it.
    readonly property var _depthRoles: ["background", "timelineBackground",
                                        "sidebar", "roomList", "rail"]

    // Element surfaces that show their group's gradient until they have a
    // colour or a gradient of their own: the two grounds that painted the
    // group's gradient before they were split out of it, so a theme saved
    // before the split paints exactly as it did. (`sidePanel` takes a
    // gradient too, but only its own: it never painted the group's.)
    readonly property var gradientGroups: ({
        timelineBackground: "background",
        roomList: "sidebar"
    })

    // The flat colour of a gradient-capable role.
    function surfaceFlat(role) {
        switch (role) {
        case "background":   return background
        case "timelineBackground": return timelineBackground
        case "sidebar":      return sidebar
        case "roomList":     return roomList
        case "rail":         return rail
        case "surface":      return surface
        case "sidePanel":    return sidePanel
        case "settingsPage": return settingsPage
        case "settingsNav":  return settingsNav
        default:             return background
        }
    }

    // The gradient a role shows in a custom theme: its own, else (for an
    // element of a gradient group that has no colour of its own) its
    // group's. `gradients` and `overrides` are CustomThemeStore's sparse
    // maps. Shared by the live shell and the editor's preview, so the two
    // cannot disagree about inheritance.
    function inheritedGradient(role, gradients, overrides) {
        if (!gradients)
            return null
        var own = gradients[role]
        if (own && own.stops && own.stops.length >= 2)
            return own
        var group = gradientGroups[role]
        if (group === undefined || (overrides && overrides[role] !== undefined))
            return null
        var g = gradients[group]
        return g && g.stops && g.stops.length >= 2 ? g : null
    }

    // { type: "linear"|"radial", angle, stops: ["#RRGGBB", ...] } or null for
    // a flat role. A custom theme's own gradient wins; otherwise "Depth"
    // derives one. The stops come from C++ (app.backdrops.depthStops), which
    // is the copy ChatBackdropTest grades on every preset; without `app` (a
    // harness) the surface stays flat.
    function gradientFor(role) {
        if (effectiveTheme === 12 && customGradients) {
            var own = inheritedGradient(role, customGradients, customOverrides)
            if (own)
                return own
        }
        if (surfaceDepth !== 1 || _depthRoles.indexOf(role) < 0)
            return null
        if (typeof app === "undefined" || !app || !app.backdrops)
            return null
        var stops = app.backdrops.depthStops(surfaceFlat(role), dark)
        if (!stops || stops.length < 2)
            return null
        return { type: "linear", angle: 180, stops: stops }
    }

    // The colour a role's surface actually shows at vertical position t
    // (0 top .. 1 bottom), so a strip painted over it (a fade, a header) can
    // meet it without a seam. Linear and radial both read their stops along
    // the vertical, which is exact for Depth and close enough for a custom
    // gradient's edge.
    function surfaceColorAt(role, t) {
        var flat = surfaceFlat(role)
        var g = gradientFor(role)
        if (!g)
            return flat
        var stops = g.stops
        var pos = Math.max(0, Math.min(1, t))
        // A gradient pointing up runs the stops bottom to top.
        if (g.type !== "radial" && (g.angle < 90 || g.angle > 270))
            pos = 1 - pos
        var span = stops.length - 1
        // Stop positions: the spec's own when it has them, else even.
        var at = function (k) {
            return g.positions && g.positions.length === stops.length
                   ? g.positions[k] : k / span
        }
        var i = 0
        while (i < span - 1 && pos > at(i + 1))
            ++i
        var lo = at(i)
        var hi = at(i + 1)
        var f = hi > lo ? Math.max(0, Math.min(1, (pos - lo) / (hi - lo)))
                        : (pos >= hi ? 1 : 0)
        var a = Qt.color(stops[i])
        var b = Qt.color(stops[i + 1])
        return Qt.rgba(a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f,
                       a.b + (b.b - a.b) * f, 1)
    }
    readonly property color stormInset:         storm ? _stoInset : inputBackground
    readonly property color stormDeep:          storm ? _stoDeep : background
    readonly property color stormBorder:        storm ? _contrast(_stoBorder, _stoText, 0.45) : border
    readonly property color stormBorderStrong:  storm ? _contrast(_stoBorderStrong, _stoText, 0.7) : borderStrong
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
    readonly property color stormTextSecondary: storm ? _contrast(_stoTextSecondary, _stoText, 0.7) : textSecondary
    readonly property color stormTextMuted:     storm ? _contrast(_stoTextMuted, _stoText, 0.75) : textMuted
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
        const floor = nameColorMinContrast
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
                return IdentityPalette.legibleChoice(chosen, _nameGrounds, floor)
        }
        return IdentityPalette.nameInk(identityIndex(key), identityAnchor,
                                       _nameGrounds, floor)
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
    // Nheko compact (compactDensity) tightens menus and dialogs by geometry
    // only; menu text keeps its size, and 28 still holds a 14 px label with
    // room above and below.
    readonly property int menuPadding:        compactDensity ? spacing4 : spacing6   // popover internal padding
    readonly property int menuRadius:         radiusLg   // popover container corner
    readonly property int menuItemHeight:     compactDensity ? 28 : 32
    // A dialog's outer inset and the gap above its button row.
    readonly property int dialogInset:        compactDensity ? spacing16 : spacing20
    readonly property int dialogFooterGap:    compactDensity ? spacing12 : spacing16
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
    // Widest a code block grows before its lines scroll. Message text is not
    // capped by it: Modern/Compact rows wrap at the row's right edge.
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
    // Settings option descriptions and hints: one step above textMeta, which
    // read too small beside the 14px option labels (GitHub chat, 2026-10-22).
    readonly property int settingsHintSize: scaled(13)
    // Settings option labels, checkboxes and combo text: the same reading size
    // as the rest of the app (scaled textBody), not the unscaled base.
    readonly property int settingsOptionSize: scaled(textBody)
    // The navigation column's sub-section labels ("On this page"). They are
    // navigation, not hints, so they no longer share settingsHintSize: one
    // logical step larger (13 -> 14 at 100%), by explicit request (master
    // prompt Part 6, 3.3). Only these labels; no other text grows.
    readonly property int settingsNavSubsectionSize: scaled(14)
    // A Settings control's row is never shorter than this, so a checkbox,
    // switch or combo is a comfortable pointer target at every text scale.
    readonly property int settingsControlMinHeight: scaled(buttonHeight)

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
