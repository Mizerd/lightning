pragma Singleton
import QtQuick

QtObject {
    id: root

    property int mode: 0
    property real textScale: 1.0
    property bool highContrast: false
    property real nameColorMinContrast: 4.5
    property int messageSpacing: 1
    readonly property int messageGroupGapExtra:
        messageSpacing === 0 ? -4 : messageSpacing === 2 ? 6 : 0
    readonly property int messageContinuationGapExtra:
        messageSpacing === 0 ? -1 : messageSpacing === 2 ? 2 : 0
    property bool underlineLinks: false
    function _contrast(base, ink, amount) {
        return highContrast ? Qt.tint(base, Qt.alpha(ink, amount)) : base
    }
    function scaled(px) { return Math.round(px * textScale * uiFontOptical) }
    function clockPosition(ms) {
        return clockText(Math.floor(Math.max(0, ms || 0) / 1000))
    }
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

    readonly property bool compactDensity:
        typeof app !== "undefined" && !!app && !!app.settings
        && app.settings.layoutPreset === 1
    readonly property int railTileMax: compactDensity ? 36 : 48
    readonly property int railTileMin: compactDensity ? 32 : 40
    readonly property int railRowPadding: compactDensity ? spacing2 : spacing4
    readonly property int railGroupGap: compactDensity ? spacing6 : spacing8
    readonly property int railGroupBreakGap: compactDensity ? spacing12 + spacing2 : spacing16 + spacing2
    readonly property int headerBandHeight: compactDensity ? 48 : 60
    readonly property int threadHeaderHeight: headerBandHeight - 2
    readonly property int roomRowPaddingV: compactDensity ? spacing4 : spacing6
    readonly property int channelRowMinHeight: compactDensity ? 28 : 32
    readonly property int composerInsetH: compactDensity ? spacing12 : spacing20
    readonly property int composerInsetBottom: compactDensity ? spacing8 : spacing16
    property bool systemDark: false
    property var customOverrides: ({})
    property int customBase: 11
    property var customGradients: ({})
    property int surfaceDepth: 0

    property bool reducedMotion: false

    readonly property int effectiveTheme: mode === 0
                                          ? (systemDark ? 9 : 8)
                                          : mode
    // Coerce a palette value to a color. The custom palette stores "#RRGGBB"
    // strings, which have no .r/.g/.b, so arithmetic on a palette entry must go
    // through here (a bare relativeLuminance(_p.background) would be NaN).
    function _asColor(v) { return typeof v === "string" ? Qt.color(v) : v }

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
    function _worstRatio(c, against) {
        var worst = Infinity
        for (var i = 0; i < against.length; ++i)
            worst = Math.min(worst, _ratio(c, against[i]))
        return worst
    }
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
    function inkSafeFill(fill, ink, minRatio) {
        return _lightnessFit(fill, [ink], minRatio).color
    }
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
    readonly property real _badgeInkRatio: highContrast ? 7.1 : 4.6
    readonly property real _badgeDotRatio: highContrast ? 4.6 : 3.1
    readonly property real badgeDotRingWidth: highContrast ? 2.5 : 1.5
    readonly property real _badgeMaxShift: 0.14
    readonly property real _badgeInkChoiceRatio: 4.6
    readonly property color _badgeDarkInk: "#0B0D12"

    readonly property bool dark: effectiveTheme === 12
                                 ? relativeLuminance(_asColor(_p.background)) < 0.18
                                 : (effectiveTheme !== 1 && effectiveTheme !== 7
                                    && effectiveTheme !== 8)
    readonly property bool storm: effectiveTheme === 11

    readonly property color _bgLight:            "#D3E1F2"
    readonly property color _railLight:            "#CCD7E5"
    readonly property color _bgDark:             "#09182C"
    readonly property color _sidebarLight:       "#DEEBFD"
    readonly property color _sidebarDark:        "#1B263B"
    readonly property color _cardLight:          "#F5F9FE"
    readonly property color _cardDark:           "#273246"
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
    readonly property color _dkLink:          "#9EAAFC"

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
    readonly property color _purLink:          "#C7B2FC"
    readonly property color _purAccent:        "#8F73E9"
    readonly property color _purAccentHover:   "#9D7EEA"
    readonly property color _purAccentPressed: "#7D5ED6"
    readonly property color _purOwnBubble:     "#6452A4"
    readonly property color _purOtherBubble:   "#3C3261"

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

    readonly property color _mosBg:            "#D2E5D6"
    readonly property color _mosRail:          "#C8DBCD"
    readonly property color _mosSidebar:       "#DCEEE2"
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

    readonly property color _stoCanvas:        "#121655"
    readonly property color _stoPanel:         "#202473"
    readonly property color _stoInset:         "#0A112E"
    readonly property color _stoDeep:          "#02051D"
    readonly property color _stoBorder:        "#303C80"
    readonly property color _stoBorderStrong:  "#434F9D"
    readonly property color _stoSelection:     "#283097"
    readonly property color _stoCardElevated:  "#22397E"
    readonly property color _stoHover:         "#6469BF"
    readonly property color _stoReaction:      "#24317B"
    readonly property color _stoBolt:          "#FFD447"
    readonly property color _stoBoltInk:       "#0A0F24"
    readonly property color _stoText:          "#F2F4FF"
    readonly property color _stoTextSecondary: "#C9D2F2"
    readonly property color _stoTextMuted:     "#9CA3D2"
    readonly property color _stoTextFaint:     "#7881B5"
    readonly property color _stoDanger:        "#FFA7AF"
    readonly property color _stoSuccess:       "#63D6A3"
    readonly property color _stoLink:          "#9295F5"
    readonly property color _stoAccentHover:   "#FFDF6E"
    readonly property color _stoAccentPressed: "#E9BC2F"
    readonly property color _stoOwnBubble:     "#2B3B8D"
    readonly property color _stoSelectedHover: "#3037AD"
    readonly property color _stoMention:       "#E5677A"

    readonly property color _onAccent:         "#FFFFFF"

    readonly property color _dangerInkLight:   "#9F2A30"
    readonly property color _dangerInkDark:    "#FFA7AF"
    readonly property color _warnInkLight:     "#814A00"
    readonly property color _warnInkDark:      "#FFAD67"
    readonly property color _okInkLight:       "#056435"
    readonly property color _okInkDark:        "#63D6A3"
    readonly property color _infoInkLight:     "#005994"
    readonly property color _infoInkDark:      "#73CEFC"
    readonly property color _dangerFillHover:  "#C81F1F"
    readonly property color _dangerFillPressed:"#A81919"
    readonly property color _awayLight:        "#BA671B"
    readonly property color _awayDark:         "#F59349"

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

    readonly property color _scrimBase:        "#111111"

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
        link: _lightLink, rail: _railLight,
        otherBubble: _cardElevatedLight
    })
    readonly property var _dark: ({
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
        link: _midLink, rail: _bgDark
    })
    readonly property var _graphite: ({
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
        link: _teaLink
    })
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
        { id: 12, name: qsTr("Your theme") }
    ]

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

    function paletteForTheme(id) {
        var p = rawPaletteForTheme(id)
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
        var groups = elementGroups
        for (var e in groups) {
            if (out[e] === undefined)
                out[e] = p[e] !== undefined ? p[e] : out[groups[e]]
        }
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

    readonly property color background:          _p.background
    readonly property color windowBackground:    background
    readonly property color sidebar:             _p.sidebar
    readonly property color navBackground:       sidebar
    readonly property color panelBackground:     sidebar
    readonly property color rail:                _p.rail !== undefined
                                                 ? _p.rail : _p.sidebar
    readonly property color channelCategoryText:
        _p.channelCategoryText !== undefined ? _p.channelCategoryText
                                             : textMuted
    readonly property color channelChevron:
        _p.channelChevron !== undefined ? _p.channelChevron : textMuted
    readonly property color channelText:
        _p.channelText !== undefined ? _p.channelText : textSecondary
    readonly property color channelTextUnread:
        _p.channelTextUnread !== undefined ? _p.channelTextUnread : text
    readonly property color channelSelected:
        _p.channelSelected !== undefined ? _p.channelSelected : selected
    readonly property color channelSelectedText:
        _p.channelSelectedText !== undefined ? _p.channelSelectedText
                                             : selectedText
    readonly property color channelHover:
        _p.channelHover !== undefined ? _p.channelHover : hover
    readonly property color channelUnreadMark:
        _p.channelUnreadMark !== undefined ? _p.channelUnreadMark
                                           : _lightnessFit(accent, badgeDotSurfaces(root),
                                                           _badgeDotRatio).color

    readonly property color railFolderSurface:
        _p.railFolderSurface !== undefined ? _p.railFolderSurface
                                           : cardElevated
    readonly property bool _railIsDark: rail.hslLightness < 0.5
    readonly property var _railNestAccentMix: [0.10, 0.18, 0.30, 0.42, 0.54]
    readonly property var _railNestAlphas:
        _railIsDark ? [0.027, 0.039, 0.081, 0.133, 0.200]
                    : [0.040, 0.058, 0.119, 0.185, 0.256]
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
    readonly property color accentText:          _p.accentText !== undefined
                                                 ? _p.accentText : _onAccent
    readonly property color accentSoft:          _p.accentSoft !== undefined
                                                 ? _p.accentSoft : _p.selected
    readonly property color accentBorder:        _p.accentBorder !== undefined
                                                 ? _p.accentBorder
                                                 : _p.borderStrong
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
    readonly property color dangerFill:          _accentDanger
    readonly property color dangerFillHover:     _dangerFillHover
    readonly property color dangerFillPressed:   _dangerFillPressed
    readonly property color dangerText:          "#FFFFFF"
    readonly property color successFill:         _accentGreen
    readonly property color warningFill:         _accentWarning
    readonly property color infoFill:            _accentInfo
    readonly property color dangerInk:           danger
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
    readonly property color icon:                textMuted
    readonly property color shadow:              dark ? "#59000000"
                                                      : "#0A000000"
    readonly property color overlayScrim:        "#80000000"
    readonly property color scrimInk:            "#FFFFFF"
    readonly property color modalScrim:          "#7308080C"
    readonly property color codeBlock:           _p.codeBlock
    readonly property color link:                _p.link !== undefined
                                                 ? _p.link : _p.accent

    readonly property color ownMessageBubble:    _p.ownBubble
    readonly property color ownBubble:           ownMessageBubble
    readonly property color _ownBubbleTextDefault: "#FFFFFF"
    readonly property color ownBubbleText:       _p.ownBubbleText !== undefined
                                                 ? _p.ownBubbleText
                                                 : _ownBubbleTextDefault
    readonly property color onAccentMuted:       "#DCE4FF"
    readonly property color otherMessageBubble:  _p.otherBubble
    readonly property color otherBubble:         otherMessageBubble
    readonly property color otherBubbleText:     _p.otherBubbleText !== undefined
                                                 ? _p.otherBubbleText : textPrimary
    readonly property color incomingBubble:      otherMessageBubble
    readonly property color outgoingBubble:      ownMessageBubble
    readonly property color bubbleOverlay:       "#26000000"
    readonly property color bubbleOverlaySubtle: "#0F000000"
    readonly property color embedSurface:        _p.embedSurface !== undefined
                                                 ? _p.embedSurface
                                                 : (storm ? _stoPanel : cardElevated)
    readonly property color embedBorder:         _p.embedBorder !== undefined
                                                 ? _p.embedBorder : border
    readonly property color embedText:           _p.embedText !== undefined
                                                 ? _p.embedText : textPrimary

    readonly property color reactionBackground:  _p.reaction !== undefined
                                                 ? _p.reaction : cardElevated
    readonly property color reactionBorder:      _p.reactionBorder !== undefined
                                                 ? _p.reactionBorder
                                                 : (dark ? border : borderStrong)
    readonly property color reactionInk:         _p.reactionInk !== undefined
                                                 ? _p.reactionInk : textSecondary
    readonly property color reactionSelectedBackground:
        _p.reactionSelectedBackground !== undefined
        ? _p.reactionSelectedBackground : accentSoft
    readonly property color reactionSelectedBorder:
        _p.reactionSelectedBorder !== undefined ? _p.reactionSelectedBorder
                                                : accent
    readonly property color reactionSelectedInk:
        _p.reactionSelectedInk !== undefined ? _p.reactionSelectedInk
                                             : textPrimary
    readonly property color reactionHighlight:   selected
    readonly property color unreadBadge:         _p.unreadBadge !== undefined
                                                 ? _p.unreadBadge : accent
    readonly property color mentionHighlight:    _p.mentionHighlight !== undefined
                                                 ? _p.mentionHighlight : accent
    readonly property color mentionBadge:        _p.mention !== undefined
                                                 ? _p.mention : _accentDanger
    readonly property color mentionChipFill:     Qt.alpha(mentionHighlight, 0.16)
    readonly property color mentionChipInk:      textPrimary
    readonly property color mentionChipBorder:   "transparent"
    readonly property color mentionSelfFill:     accentSoft
    readonly property color mentionSelfInk:      textPrimary
    readonly property color selfMentionInk:      _p.selfMentionInk !== undefined
                                                 ? _p.selfMentionInk : accent
    readonly property color undecryptableText:   textMuted
    readonly property color pressedSurface:      selectedHover
    readonly property color messageHighlight:    _p.messageHighlight !== undefined
                                                 ? _p.messageHighlight : selected
    readonly property color textSelection:       _p.textSelection !== undefined
                                                 ? _p.textSelection
                                                 : selectedHover
    readonly property color paletteHighlight:    _p.paletteHighlight !== undefined
                                                 ? _p.paletteHighlight : selected
    readonly property color roomSelected:        _p.roomSelected !== undefined
                                                 ? _p.roomSelected : selected
    readonly property color roomHover:           _p.roomHover !== undefined
                                                 ? _p.roomHover : hover
    readonly property color timestampInk:        _p.timestampInk !== undefined
                                                 ? _p.timestampInk : textMuted
    readonly property color placeholderInk:      _p.placeholderInk !== undefined
                                                 ? _p.placeholderInk
                                                 : stormTextMuted
    readonly property color threadHighlight:     accent
    readonly property color presenceOnline:      _p.presenceOnline !== undefined
                                                 ? _p.presenceOnline
                                                 : (_p.online !== undefined
                                                    ? _p.online : success)
    readonly property color presenceAway:        _p.away !== undefined
                                                 ? _p.away
                                                 : (dark ? _awayDark
                                                         : _awayLight)
    readonly property color presenceOffline:     textMuted

    readonly property color buttonPrimaryFill:    _p.buttonPrimaryFill !== undefined
                                                  ? _p.buttonPrimaryFill : accent
    readonly property color buttonPrimaryHover:   accentHover
    readonly property color buttonPrimaryPressed: accentPressed
    readonly property color buttonPrimaryInk:     _p.buttonPrimaryInk !== undefined
                                                  ? _p.buttonPrimaryInk : accentText
    readonly property color sendButton:           _p.sendButton !== undefined
                                                  ? _p.sendButton : accent
    readonly property color sendButtonInk:        _p.sendButtonInk !== undefined
                                                  ? _p.sendButtonInk : accentText
    readonly property color buttonNeutralFill:    cardElevated
    readonly property color buttonNeutralHover:   selected
    readonly property color buttonNeutralPressed: selectedHover
    readonly property color buttonNeutralInk:     textPrimary
    readonly property color buttonNeutralBorder:  border
    readonly property color buttonSecondaryFill:  _p.buttonSecondaryFill !== undefined
                                                  ? _p.buttonSecondaryFill
                                                  : Qt.alpha(buttonNeutralInk, 0.06)
    readonly property color buttonSecondaryBorder: _p.buttonSecondaryBorder !== undefined
                                                  ? _p.buttonSecondaryBorder : borderStrong
    readonly property color buttonGhostHover:     _p.buttonGhostHover !== undefined
                                                  ? _p.buttonGhostHover : hover
    readonly property color buttonGhostPressed:   selected
    readonly property color buttonGhostInk:       _p.buttonGhostInk !== undefined
                                                  ? _p.buttonGhostInk : textSecondary
    readonly property color buttonDangerFill:     dangerFill
    readonly property color buttonDangerHover:    dangerFillHover
    readonly property color buttonDangerPressed:  dangerFillPressed
    readonly property color buttonDangerInk:      dangerText
    readonly property color buttonDisabledFill:   cardElevated
    readonly property color buttonDisabledInk:    _p.buttonDisabledInk !== undefined
                                                  ? _p.buttonDisabledInk : textDisabled
    readonly property color buttonDisabledBorder: _p.buttonDisabledBorder !== undefined
                                                  ? _p.buttonDisabledBorder : border
    readonly property color buttonDisabledTint:   Qt.alpha(buttonDisabledInk, 0.08)
    readonly property int   buttonHeightSm:       26
    readonly property int   buttonHeight:         32
    readonly property int   buttonHeightLg:       40
    readonly property int   buttonRadius:         radiusTile
    readonly property int   buttonPaddingH:       spacing12
    readonly property int   buttonPaddingHSm:     spacing8
    readonly property int   buttonIconGap:        spacing6
    readonly property int   buttonMinWidth:       72

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
    readonly property color chipBoltFill:      bolt
    readonly property color chipBoltInk:       boltInk
    readonly property int   chipHeight:        20
    readonly property int   chipRadius:        radiusPill
    readonly property int   chipPaddingH:      spacing8

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

    readonly property color scrimBackdrop:       "#66000000"
    readonly property color scrimSurface:        "#D9111111"
    readonly property color scrimSurfaceRaised:  "#33FFFFFF"
    readonly property color scrimSurfaceHover:   "#59FFFFFF"
    readonly property color scrimBorder:         "#33FFFFFF"
    readonly property color scrimInkStrong:      "#E6FFFFFF"
    readonly property color scrimInkMuted:       "#94A3B8"
    readonly property color scrimSuccessInk:     "#63D6A3"
    readonly property color scrimDangerInk:      "#FFA7AF"

    readonly property color stormCanvas:        storm ? _stoCanvas : background
    readonly property color stormPanel:         storm ? _stoPanel : surface
    readonly property color popoverSurface:     _p.popoverSurface !== undefined
                                                ? _p.popoverSurface : stormPanel
    readonly property color menuHighlight:      _p.menuHighlight !== undefined
                                                ? _p.menuHighlight
                                                : stormSelection
    readonly property color settingsPage:       _p.settingsPage !== undefined
                                                ? _p.settingsPage : stormDeep
    readonly property color settingsNav:        _p.settingsNav !== undefined
                                                ? _p.settingsNav : stormCanvas

    readonly property color roomList:           _p.roomList !== undefined
                                                ? _p.roomList : sidebar
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

    readonly property color headerDivider:      _p.headerDivider !== undefined
                                                ? _p.headerDivider : border
    readonly property color searchFieldBorder:  _p.searchFieldBorder !== undefined
                                                ? _p.searchFieldBorder : border
    readonly property color composerBorder:     _p.composerBorder !== undefined
                                                ? _p.composerBorder : border
    readonly property color fieldBorder:        _p.fieldBorder !== undefined
                                                ? _p.fieldBorder : border
    readonly property color stormFieldBorder:   storm ? _stoBorder : fieldBorder
    readonly property color codeBlockBorder:    _p.codeBlockBorder !== undefined
                                                ? _p.codeBlockBorder : border
    readonly property color channelFolderBorder: _p.channelFolderBorder !== undefined
                                                 ? _p.channelFolderBorder : border
    readonly property color settingsDivider:    _p.settingsDivider !== undefined
                                                ? _p.settingsDivider : stormBorder
    readonly property color menuBorder:         _p.menuBorder !== undefined
                                                ? _p.menuBorder : stormBorderStrong

    readonly property color railTile:           _p.railTile !== undefined
                                                ? _p.railTile : cardElevated
    readonly property color roomSelectedHover:  _p.roomSelectedHover !== undefined
                                                ? _p.roomSelectedHover : selectedHover
    readonly property color settingsNavSelected:
        _p.settingsNavSelected !== undefined ? _p.settingsNavSelected
                                             : selectedHover
    readonly property color settingsNavSelectedMark:
        contrastRatio(bolt, settingsNavSelected) >= 3.0 ? bolt : stormText
    readonly property color settingsControlEdge:
        contrastRatio(stormTextMuted, stormPanel) >= 3.0 ? stormTextMuted
        : contrastRatio(stormTextSecondary, stormPanel) >= 3.0
          ? stormTextSecondary : stormText
    readonly property color memberRowHover:     _p.memberRowHover !== undefined
                                                ? _p.memberRowHover : hover
    readonly property color roomUnreadBadge:    _p.roomUnreadBadge !== undefined
                                                ? _p.roomUnreadBadge : unreadBadge
    readonly property color railUnreadDot:      _p.railUnreadDot !== undefined
                                                ? _p.railUnreadDot : unreadBadge
    function badgeDotSurfaces(t) {
        return [ t.rail, t.stormPanel, t.stormCanvas, t.roomList,
                 _over(t.roomHover, t.roomList), _over(t.roomSelected, t.roomList),
                 _over(t.channelHover, t.roomList), _over(t.channelSelected, t.roomList),
                 _over(t.sidePanel, t.roomList), _over(t.roomHeader, t.roomList),
                 _over(t.timelineBackground, t.roomList) ]
    }
    readonly property color unreadDot:          _p.unreadDot !== undefined
                                                ? _p.unreadDot : roomUnreadBadge
    readonly property color mentionDot:         _p.mentionDot !== undefined
                                                ? _p.mentionDot : mentionBadge
    readonly property color unreadDotRing:      _lightnessFit(unreadDot,
                                                              badgeDotSurfaces(root),
                                                              _badgeDotRatio).color
    readonly property color mentionDotRing:     _lightnessFit(mentionDot,
                                                              badgeDotSurfaces(root),
                                                              _badgeDotRatio).color
    readonly property color railUnreadDotRing:  _lightnessFit(railUnreadDot,
                                                              badgeDotSurfaces(root),
                                                              _badgeDotRatio).color
    readonly property color unreadDividerInk:   _lightnessFit(unreadBadge,
                                                              [timelineBackground],
                                                              _badgeInkRatio).color
    readonly property color unreadCountInk:     _p.unreadCountInk !== undefined
                                                ? _p.unreadCountInk
                                                : badgeInk(roomUnreadBadge, unreadBadgeText,
                                                           _badgeInkRatio)
    readonly property color unreadCountFill:    _p.unreadCountFill !== undefined
                                                ? _p.unreadCountFill
                                                : inkSafeFill(roomUnreadBadge, unreadCountInk,
                                                              _badgeInkRatio)

    readonly property color railTileActive:     _p.railTileActive !== undefined
                                                ? _p.railTileActive : accentSoft
    readonly property color callBanner:         _p.callBanner !== undefined
                                                ? _p.callBanner : accentSoft
    readonly property color railActiveRing:     _p.railActiveRing !== undefined
                                                ? _p.railActiveRing : accent
    readonly property color unreadBadgeText:    _p.unreadBadgeText !== undefined
                                                ? _p.unreadBadgeText : accentText

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

    readonly property color presenceDot:        _p.presenceDot !== undefined
                                                ? _p.presenceDot : presenceOnline
    readonly property color chipSuccess:        _p.chipSuccess !== undefined
                                                ? _p.chipSuccess
                                                : (storm ? stormSuccess : presenceOnline)
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

    readonly property var _depthRoles: ["background", "timelineBackground",
                                        "sidebar", "roomList", "rail"]

    readonly property var gradientGroups: ({
        timelineBackground: "background",
        roomList: "sidebar"
    })

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

    function surfaceColorAt(role, t) {
        var flat = surfaceFlat(role)
        var g = gradientFor(role)
        if (!g)
            return flat
        var stops = g.stops
        var pos = Math.max(0, Math.min(1, t))
        if (g.type !== "radial" && (g.angle < 90 || g.angle > 270))
            pos = 1 - pos
        var span = stops.length - 1
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
    readonly property color bolt:               storm ? _stoBolt : accent
    readonly property color boltInk:            storm ? _stoBoltInk : accentText
    readonly property color wordmarkBolt:       storm ? _stoBolt
                                                : Qt.tint(textSecondary,
                                                          Qt.alpha(accent, 0.55))
    readonly property color stormText:          storm ? _stoText : textPrimary
    readonly property color stormTextSecondary: storm ? _contrast(_stoTextSecondary, _stoText, 0.7) : textSecondary
    readonly property color stormTextMuted:     storm ? _contrast(_stoTextMuted, _stoText, 0.75) : textMuted
    readonly property color stormTextFaint:     storm ? _stoTextFaint : textDisabled
    readonly property color stormDanger:        storm ? _stoDanger : dangerInk
    readonly property color stormSuccess:       storm ? _stoSuccess : success
    readonly property color stormLink:          storm ? _stoLink : link
    readonly property color stormDangerSoft:    storm ? Qt.alpha(_stoDanger, 0.10)
                                                      : dangerSoft
    readonly property color stormDangerBorder:  storm ? Qt.alpha(_stoDanger, 0.30)
                                                      : dangerBorder
    readonly property color stormBoltGlow:      Qt.alpha(bolt, 0.12)
    readonly property color stormWatermark:     Qt.alpha(bolt, 0.12)

    function compositeOver(top, alpha, bottom) {
        var t = _asColor(top)
        var b = _asColor(bottom)
        return Qt.rgba(alpha * t.r + (1.0 - alpha) * b.r,
                       alpha * t.g + (1.0 - alpha) * b.g,
                       alpha * t.b + (1.0 - alpha) * b.b, 1.0)
    }

    readonly property real softChipFillAlpha: 0.14

    readonly property var _softChipGrounds: [
        stormPanel, stormCanvas, surface, cardElevated, stormInset
    ]

    function softChipInk(base) {
        var tone = _asColor(base)
        var fills = []
        for (var i = 0; i < _softChipGrounds.length; ++i)
            fills.push(compositeOver(tone, softChipFillAlpha,
                                     _softChipGrounds[i]))
        return IdentityPalette.legibleChoice(tone, fills)
    }

    function flatten(c, ground) {
        var t = _asColor(c)
        return compositeOver(t, t.a, ground)
    }

    function legibleInkOn(base, ground) {
        return IdentityPalette.legibleChoice(_asColor(base), [ground])
    }

    readonly property int editorChromeTheme: {
        if (effectiveTheme === 12)
            return (customBase >= 1 && customBase <= 11) ? customBase : 11
        return (effectiveTheme >= 1 && effectiveTheme <= 11) ? effectiveTheme : 11
    }
    readonly property var _editorPalette: paletteForTheme(editorChromeTheme)
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

    readonly property var avatarPaletteLegacy: [
        "#D04339", "#AE6424", "#8F7224", "#4F822B", "#2E8460",
        "#2F7F93", "#4163C8", "#8941C8", "#C84190"
    ]
    function identityIndex(key) {
        var h = 0
        for (var i = 0; i < key.length; ++i)
            h = ((h << 5) - h + key.charCodeAt(i)) | 0
        return Math.abs(h) % 9
    }
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
    function avatarInk(key) {
        return IdentityPalette.ink(identityIndex(key), identityAnchor)
    }
    readonly property var _nameGrounds: [
        background, surface, cardElevated, incomingBubble
    ]
    function nameInkForSlot(slot) {
        return IdentityPalette.nameInk(slot, identityAnchor, _nameGrounds)
    }
    function userColor(key) {
        if (!key || key.length === 0)
            return textPrimary
        const floor = nameColorMinContrast
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

    readonly property color surfaceAlt:          cardElevated
    readonly property color text:                textPrimary
    readonly property color muted:               textMuted
    readonly property color selectedBg:          selected
    readonly property color error:               danger

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

    readonly property int radiusSm:   4
    readonly property int radiusMd:   8
    readonly property int radiusLg:   12
    readonly property int radiusPill: 999
    readonly property int radius:     radiusMd
    readonly property int reactionEmojiSize: 16
    readonly property int reactionEmojiBox:  18
    readonly property int reactionChipHeight: 20

    readonly property int radiusMedia: 12
    readonly property int radiusChip:    radiusSm
    readonly property int radiusControl: 7
    readonly property int radiusTile:    9
    readonly property int radiusThumb:  10
    readonly property int radiusOmnibox: 11
    readonly property int radiusCard:   14

    readonly property int menuPadding:        compactDensity ? spacing4 : spacing6
    readonly property int menuRadius:         radiusLg
    readonly property int menuItemHeight:     compactDensity ? 28 : 32
    readonly property int dialogInset:        compactDensity ? spacing16 : spacing20
    readonly property int dialogFooterGap:    compactDensity ? spacing12 : spacing16
    readonly property int menuItemRadius:     radiusMd
    readonly property int menuItemPadding:    spacing8
    readonly property int menuIconSize:       17
    readonly property int menuIconGap:        spacing10
    readonly property int menuContextHeaderHeight: 24
    readonly property int menuWidthDefault:   220
    readonly property int menuWidthMessage:   252
    readonly property int menuWidthRoom:      196
    readonly property int menuWidthFlyout:    150
    readonly property int menuDividerVMargin: spacing6
    readonly property int menuDividerHMargin: spacing4
    readonly property color keycapBackground:     cardElevated
    readonly property color keycapBorder:         borderStrong
    readonly property color keycapText:           textMuted
    readonly property int   keycapPaddingH:       5
    readonly property int   keycapPaddingV:       1
    readonly property int   keycapHeaderPaddingH: 6
    readonly property int   keycapHeaderPaddingV: 2
    readonly property color sectionLabelColor: textMuted
    readonly property color monoIdentityColor: textMuted
    readonly property real  trackingSection:   0.8
    readonly property real  trackingMono:      1.2
    readonly property real  trackingStorm:     1.6
    readonly property int   emojiCellSize:  46
    readonly property int   emojiGlyphSize: 30

    readonly property int timelineContentMaxWidth: 760

    readonly property int textDisplay:   22
    readonly property int textTitle:     16
    readonly property int textSubtitle:  14
    readonly property int textBody:      14
    readonly property int textMeta:      12
    readonly property int textMicro:     10
    readonly property int settingsHintSize: scaled(13)
    readonly property int settingsOptionSize: scaled(textBody)
    readonly property int settingsNavSubsectionSize: scaled(14)
    readonly property int settingsControlMinHeight: scaled(buttonHeight)

    readonly property int weightBody:    400
    readonly property int weightMedium:  500
    readonly property int weightStrong:  600
    readonly property int weightBold:    700
    readonly property int weightDisplay: 800

    readonly property real lineHeightBody:    1.5
    readonly property real lineHeightTight:   1.3
    readonly property real lineHeightDisplay: 1.2

    readonly property int fontSizeXS:        11
    readonly property int fontSizeS:         13
    readonly property int fontSizeM:         14
    readonly property int fontSizeRoom:      16
    readonly property int fontSizeHeader:    18
    readonly property int fontSizePageTitle: 24
    readonly property int fontSizeL:         fontSizeRoom
    readonly property int fontSizeXL:        fontSizeHeader

    readonly property int fontPageTitle:     textDisplay
    readonly property int fontSectionTitle:  fontSizeHeader
    readonly property int fontRoomTitle:     fontSizeRoom
    readonly property int fontBody:          fontSizeM
    readonly property int fontSecondary:     fontSizeS
    readonly property int fontMessageSender: 12
    readonly property int fontCaption:       fontSizeXS
    readonly property int fontMono:          fontSizeS
    readonly property int fontMicro:     9
    readonly property int fontChip:      10
    readonly property int fontMonoXS:    11
    readonly property int fontMonoSm:    12
    readonly property int fontResult:    14
    readonly property int fontQuery:     15
    readonly property int fontTrustName: 17
    readonly property int fontNavTitle:  17

    property string uiFont:          "Manrope"
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
    property string monoFont:                  "JetBrains Mono"
    readonly property var    monoFontFamilies: [
        "JetBrains Mono",
        "Fira Mono",
        "SF Mono",
        "Consolas",
        "monospace"
    ]
    readonly property string brandFont:         "Space Grotesk"
    readonly property var    brandFontFamilies: [
        "Space Grotesk",
        "Manrope",
        "sans-serif"
    ]
    readonly property string displayFont:       brandFont

    readonly property string menuFont:          uiFont

    readonly property string menuSectionFont:    uiFont
    readonly property int    menuSectionSize:    textMeta
    readonly property int    menuSectionWeight:  weightStrong
    readonly property real   menuSectionTracking: 0.0
}
