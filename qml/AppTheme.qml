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
    // Content text scale. Folds in the UI font's optical correction so the
    // text-size slider and the font picker stay independent; Manrope (the
    // default) has factor 1.0, so nothing changes unless the user picks
    // another family. Fixed chrome and icon sizes stay unscaled.
    function scaled(px) { return Math.round(px * textScale * uiFontOptical) }
    // System dark-mode hint from the platform (QStyleHints::colorScheme),
    // pushed in from Main.qml via AppController.systemDarkMode.
    property bool systemDark: false
    // ---- Custom theme (SettingsManager::CustomTheme, id 12) ----
    // A SPARSE map of role -> "#RRGGBB" laid over `customBase`'s palette,
    // pushed in from Main.qml as app.customTheme.colors. Sparse on purpose:
    // overriding three colours overrides three colours and everything else
    // keeps following the base theme. CustomThemeStore::sanitize() has
    // already dropped unknown roles and malformed values before this sees
    // them; the guard in _custom below is a second gate, because this is the
    // one palette whose contents a user can hand-edit in a config file.
    property var customOverrides: ({})
    property int customBase: 11

    // v0.7: app-wide reduced-motion hint consumed by loading skeletons and
    // other decorative animation.
    //
    // BOUND FROM Main.qml (2026-08-26), like `mode` and `customOverrides`.
    // It sat at a hardcoded false from the design round until then, so every
    // one of the ~20 branches reading it across ten files was dead code and
    // the accessibility setting it was waiting for did not exist. It stays
    // false HERE so a host that never binds it — a test harness loading one
    // component — still gets motion rather than an undefined.
    property bool reducedMotion: false

    // Resolve System (0) to the flagship design pair — Moss Light for light
    // systems, INDIGO NIGHT for dark ones. Storm held the dark half from the
    // 0.6.5 round until 2026-08-25, when the maintainer made Indigo Night the
    // flagship; Storm remains the brand theme and the shell's own chrome, and
    // stays a featured card.
    //
    // Only the UNSET/System state resolves here; an explicitly persisted theme
    // id 1–11 is never rerouted, so changing which theme System means cannot
    // touch anybody's stored choice.
    readonly property int effectiveTheme: mode === 0
                                          ? (systemDark ? 9 : 8)
                                          : mode
    // A palette value as a COLOR. The preset palettes hold `color`
    // properties, but the custom palette merges the user's own values in as
    // "#RRGGBB" STRINGS, and a string has no .r/.g/.b — so anything that does
    // arithmetic on a palette entry has to come through here first.
    // relativeLuminance(_p.background) returned NaN for every custom theme
    // whose background was overridden, which made `dark` silently false and
    // put light-theme scrims on a dark shell.
    function _asColor(v) { return typeof v === "string" ? Qt.color(v) : v }

    // WCAG relative luminance of a QML color. Used only to classify a
    // CUSTOM palette as light or dark — the presets are known by id.
    function relativeLuminance(c) {
        function lin(v) {
            return v <= 0.03928 ? v / 12.92
                                : Math.pow((v + 0.055) / 1.055, 2.4)
        }
        return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b)
    }

    // Any preset that is not one of the light surfaces is dark (used by
    // overlay chrome). A CUSTOM theme is classified by its own background
    // instead: the user can build a light palette on a dark base, and
    // inheriting the base's answer would leave shadows and scrims fighting
    // the surface they sit on.
    readonly property bool dark: effectiveTheme === 12
                                 ? relativeLuminance(_asColor(_p.background)) < 0.18
                                 : (effectiveTheme !== 1 && effectiveTheme !== 7
                                    && effectiveTheme !== 8)
    // True while the Storm brand theme (11) is the effective palette. Views
    // never branch on this — it exists so the storm* token namespace below
    // can route between the Storm literals and each legacy theme's own
    // semantic tones inside this singleton.
    readonly property bool storm: effectiveTheme === 11

    // ---- Raw palette literals (retained; the token test reads these). ----
    readonly property color _bgLight:            "#E4EDF7"
    readonly property color _railLight:            "#D8E1EB"
    readonly property color _bgDark:             "#09182C"
    readonly property color _sidebarLight:       "#EEF5FE"
    readonly property color _sidebarDark:        "#1B263B"
    readonly property color _cardLight:          "#FFFFFF"
    readonly property color _cardDark:           "#273246"
    readonly property color _cardElevatedLight:  "#EFF9FF"
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
    // Lightning Dark's own link/mention ink. It used to inherit _accentBlue
    // through `link: _p.link !== undefined ? _p.link : _p.accent`, which was
    // already below AA on this theme (3.48 on the ground, 3.01 on an incoming
    // bubble) and got worse when the ladder above rose while the ink did not:
    // 2.39 on surface, 1.65 on hover. Same accent hue family, lighter — 8.66
    // on background, 5.96 on surface, 4.96 on cardElevated. _accentBlue
    // itself is NOT moved: Lightning Light depends on it at 4.75/5.44.
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
    // Purple Dusk's link ink. The accent CANNOT do this job, and the reason is
    // over-determined rather than a tuning miss: white-on-accent is asserted
    // at 3:1, which caps any accent at luminance 0.30, while an AA link needs
    // 0.36 on this theme's surface and 0.46 on cardElevated. Even the raised
    // accent leaves every URL in every message at 3.20 / 2.58. Held to the
    // shell's violet hue. 6.17 on surface, 4.99 on cardElevated, 4.27 on
    // hover. Honest limit: 3.41 inside your own outgoing bubble — Storm has
    // the same shape (_stoLink on _stoOwnBubble is 3.71), so it is an app-wide
    // pattern rather than a Purple defect.
    readonly property color _purLink:          "#C7B2FC"
    readonly property color _purAccent:        "#8F73E9"
    readonly property color _purAccentHover:   "#9D7EEA"
    readonly property color _purAccentPressed: "#7D5ED6"
    readonly property color _purOwnBubble:     "#6452A4"
    readonly property color _purOtherBubble:   "#3C3261"

    // Warm — light cream surfaces with an amber accent.
    readonly property color _warBg:            "#F1E7D7"
    readonly property color _warRail:              "#EBDFCE"
    readonly property color _warSidebar:       "#F9EFE1"
    readonly property color _warCard:          "#FFFDF8"
    readonly property color _warCardElevated:  "#FCF4E8"
    readonly property color _warHover:         "#EEDFC6"
    readonly property color _warSelected:      "#E7CCA8"
    readonly property color _warSelectedHover: "#DABC92"
    readonly property color _warSelectedText:  "#3B3428"
    readonly property color _warTextPrimary:   "#3B3428"
    readonly property color _warTextSecondary: "#675944"
    readonly property color _warTextMuted:     "#6E6350"
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
    readonly property color _mosBg:            "#E3EFE6"
    readonly property color _mosRail:          "#D8E5DB"
    readonly property color _mosSidebar:       "#ECF6EF"
    readonly property color _mosCard:          "#FFFFFF"
    readonly property color _mosCardElevated:  "#EFFAF1"
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
    // Blue, on request (2026-08-22): the previous #ACAAFD was a violet
    // sitting between the accent and the selected-row ink, and a link
    // that shares a hue with the selection does not read as a link.
    // 4.79:1 at worst across every Indigo surface (the own bubble),
    // so it clears AA everywhere it can land.
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

    // Deep Teal — design-handoff dark theme (option 2b). Accent fills use
    // dark ink (the accent itself is bright).
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

    // Storm — the 0.6.5 brand theme (selectable id 11). Deep navy surfaces,
    // bolt-yellow accent, SPEC-storm-language §1 palette. These literals are
    // ALSO the values the storm* namespace carries when Storm is the
    // effective theme; the token test parses them by name. They are no
    // longer the trust card's fixed palette — 2026-08-26 routed that surface
    // like everything else (see the storm* block below).
    //
    // 2026-08-21 SURFACE-LADDER REBUILD. The audited defect was NOT a lack of
    // hue — Storm measured the most saturated shell in the app (Lab chroma
    // 27.1 against Moss Light's 0.8). It was a lack of SEPARATION: every rung
    // but one sat below 1.25:1 (deep→canvas 1.025, canvas→panel 1.138,
    // panel→selection 1.138), and cardElevated / hover / selected were the
    // SAME literal #132558, so a hovered row, a selected row and a raised
    // card were one tone. The whole left half of the window read as one
    // continuous field carried by 1px borders.
    //
    // The rebuilt ladder, measured (WCAG ratio between adjacent surfaces):
    //   deep → canvas             1.247   (was 1.025)
    //   canvas → panel            1.248   (was 1.138)
    //   panel → cardElevated      1.422   (was 1.138, and it was the SAME
    //                                      literal as hover and selected)
    //   panel → selection         1.441   (was 1.138)
    //   selection → selectedHover 1.205   (was 1.187)
    // cardElevated and selection are deliberately SIBLINGS, not rungs: they
    // sit at the same L* (31.0 / 31.3) and are told apart by tint — elevated
    // stays neutral navy (Lab C 35.6, h 291), selection carries the
    // periwinkle link hue (C 49.9, h 297), CIE76 dE 15. Elevation and state
    // are different meanings and must not be read off the same axis.
    //
    // WHY THE LADDER STOPS WHERE IT DOES — do not "fix" this by pushing the
    // rungs further apart. tests/ThemeTokensTest.cpp requires every dark
    // display-name ink to clear 4.5:1 on _stoCanvas, _stoPanel, _stoSelection
    // and (since this round) _stoCardElevated. The dimmest of those inks has
    // relative luminance 0.5156, which caps ANY of those surfaces at
    // luminance 0.0757. Four 1.25:1 rungs from a near-black ground reach that
    // cap exactly, which is why cardElevated/selection are siblings rather
    // than a fifth and sixth rung. The name inks were rebuilt and measured in
    // the round before this one and are NOT to be retuned to buy headroom:
    // if a surface move breaks the identity matrix, move the SURFACE back.
    // The room / people list ground. Deepened and enriched 2026-08-21 on
    // Rokas's report that it "looks kinda pale": #181F41 was only 46%
    // saturated, so it read as slate rather than as the theme's navy. This
    // is 70% at a LOWER luminance — HLS lightness goes up, luminance goes
    // down, because a saturated blue carries far less of it than a greyish
    // tone at the same nominal lightness.
    //
    // It cannot go much darker than this without merging into _stoDeep,
    // which is the ground on BOTH sides of it (the rail and the timeline).
    // At this value the rail->list step is 1.223; taking the list to a
    // genuinely near-black navy drops that under 1.15 and brings back the
    // one-continuous-field look the ladder was widened to fix.
    readonly property color _stoCanvas:        "#121655"
    // ── Storm blues: chroma, not HLS saturation ─────────────────────────
    //
    // The first pass re-saturated these by HLS `s`, which is NOT perceptual
    // chroma — and the error compounded up the ladder. Measured Lab C* per
    // unit L* before: canvas 3.24, panel 2.29, reaction 1.67, cardElevated
    // 1.31. So the HIGHER and LARGER a surface sat, the greyer it actually
    // was, and cardElevated — the link preview, reaction pills, keycaps —
    // ended up the palest rung in the theme. WCAG is a pure function of
    // luminance, so the whole test matrix was blind to it.
    //
    // Re-derived at bit-identical luminance, targeting C* directly: ladder
    // chroma +23% overall, every rung and every asserted pair unchanged.
    //
    // _stoDeep matters most and is easiest to miss: it is ~55% of the
    // window's pixels and carried C* 9.3 — a near-neutral black rather than a
    // navy. #020622 nearly doubles it for free.
    //
    // _stoSelection and _stoSelectedHover were the ONLY Storm surfaces with
    // R > G — that channel inversion is what made the selected room read
    // purple against a navy list, and it reaches ~38 call sites (menu
    // highlight, combo selection, emoji hover, text selection), so it was not
    // one row. Moved into the navy family; dE from cardElevated is 25.6
    // against a floor of 12.
    //
    // ── Storm blues, re-saturated 2026-08-21 ────────────────────────────
    // "replace all pale blue color with a darker one, it looks weak
    // evrywhere its used". These sat at 0.31-0.42 — navy in name, washed
    // slate on screen.
    //
    // Every one was re-derived at its EXACT existing luminance (binary
    // search on HLS lightness, drift < 0.002). WCAG contrast is a pure
    // function of relative luminance, so holding luminance fixed while
    // raising chroma preserves every asserted pair and every ladder rung
    // BIT-FOR-BIT — measured before and after: 1.223 / 1.272 / 1.422.
    // The palette got richer at zero contrast risk.
    //
    // Selection, hover and the two borders are deliberately held back
    // (0.40-0.46) where the rest go to 0.5-0.64: a neon selected row or a
    // vivid hairline reads worse than a pale one, and the borders are what
    // the eye follows around every panel.
    //
    // cardElevated and selection keep their DIFFERENT HUES (222 vs 252).
    // They sit at the same lightness by necessity — the identity inks cap
    // how far the ladder can climb — so tint is the only thing telling a
    // raised chip from a selected row apart. Re-saturating both toward one
    // hue collapsed that to dE 8 and the ladder test caught it; they are now
    // dE 25, blue-navy against violet.
    readonly property color _stoPanel:         "#202473"
    readonly property color _stoInset:         "#0A112E"
    readonly property color _stoDeep:          "#02051D"
    readonly property color _stoBorder:        "#303C80"
    readonly property color _stoBorderStrong:  "#434F9D"
    readonly property color _stoSelection:     "#283097"
    // Elevation rung above the panel: reaction pills, keycaps, raised cards.
    // Was an alias of _stoSelection, which is what made "hovered", "selected"
    // and "raised" indistinguishable.
    readonly property color _stoCardElevated:  "#22397E"
    // Row-hover lift. Kept as a translucent wash rather than an opaque rung
    // because hover applies over the timeline (deep), the room list (canvas)
    // AND menu rows (panel); at 0.22 alpha it measures 1.25/1.31/1.27:1 over
    // those three, i.e. one consistent lift instead of one tuned surface.
    readonly property color _stoHover:         "#6469BF"
    // Reaction-pill surface. Its OWN value, not an alias of cardElevated:
    // 1.21:1 above the bubble it attaches to and 1.88:1 above the timeline.
    readonly property color _stoReaction:      "#24317B"
    readonly property color _stoBolt:          "#FFD447"
    // Ink painted ON a bolt or link fill. Split out of _stoCanvas this round:
    // the room-list surface had been doing double duty as the badge ink, so
    // the ladder could not lift the sidebar without darkening every pill
    // label. 13.34:1 on bolt, 7.08:1 on the periwinkle unread pill.
    readonly property color _stoBoltInk:       "#0A0F24"
    readonly property color _stoText:          "#F2F4FF"
    readonly property color _stoTextSecondary: "#C9D2F2"
    readonly property color _stoTextMuted:     "#9CA3D2"
    readonly property color _stoTextFaint:     "#7881B5"
    readonly property color _stoDanger:        "#FFA7AF"
    readonly property color _stoSuccess:       "#63D6A3"
    readonly property color _stoLink:          "#9295F5"
    // Storm derivatives that the spec table does not carry: hover/pressed
    // steps of the bolt accent, the own-bubble navy (distinct from panel and
    // selection so outgoing messages read as their own surface), and the
    // mention red — the handoff mention hue, NOT stormDanger: the mention
    // pill carries white ink, and #FF8FA0 is too light under it.
    readonly property color _stoAccentHover:   "#FFDF6E"
    readonly property color _stoAccentPressed: "#E9BC2F"
    readonly property color _stoOwnBubble:     "#2B3B8D"
    readonly property color _stoSelectedHover: "#3037AD"
    readonly property color _stoMention:       "#E5677A"

    // Ink used on top of accent fills for every palette without its own
    // accentText (the contrast test reads this literal by name).
    readonly property color _onAccent:         "#FFFFFF"

    // ---- Status INK, per mode (2026-08-21). ----
    // `danger` used to be a theme-invariant #DC2626 applied as INK at ~40
    // call sites. It measures 4.03 / 3.93 / 3.45 / 3.03:1 on Storm's four
    // surfaces — below AA on all of them — and no test asserted it, because
    // the only Storm danger the suite checked was the ROUTED _stoDanger.
    // The same red is fine as a FILL under white text, so the role is split:
    // _accentDanger stays the FILL (dangerFill, asserted against dangerText)
    // and these are the INKS.
    //
    // Two constraints fix these values and neither is negotiable:
    //   * a dark-mode status ink renders on Nordic's #434C5E elevated card
    //     (luminance 0.0717), so it needs luminance >= 0.4977 — the same
    //     ceiling that makes the dark identity inks pastel;
    //   * a light-mode status ink renders on Warm's #EDE2CE other-bubble, so
    //     it needs luminance <= 0.1319.
    // Measured worst case over every surface in the theme test's matrices:
    // danger 4.68/4.85, warning 4.69/4.85, success 4.80/4.83, info 4.91/4.63.
    //
    // The dark danger and success ARE Storm's own _stoDanger / _stoSuccess,
    // deliberately: before this round the same concept rendered as #FF8FA0
    // inside a menu (storm namespace) and #DC2626 outside it, in one window.
    // Darkened one step in review: a chip paints its ink on a 14% tint of
    // THAT SAME INK, and the original values were tuned only against the
    // plain surfaces (4.68-4.85). The self-tint spent the rest of the margin
    // and the light themes landed at 3.83-4.04 on ~10px chip labels, which
    // get no large-text exemption. These clear 4.62 on their own tint and
    // 5.6+ on plain surfaces; chipInkOnItsOwnFillIsReadable pins both.
    readonly property color _dangerInkLight:   "#9F2A30"
    readonly property color _dangerInkDark:    "#FFA7AF"
    readonly property color _warnInkLight:     "#814A00"
    readonly property color _warnInkDark:      "#FFAD67"
    readonly property color _okInkLight:       "#056435"
    readonly property color _okInkDark:        "#63D6A3"
    readonly property color _infoInkLight:     "#005994"
    readonly property color _infoInkDark:      "#73CEFC"
    // Destructive FILL steps. These DARKEN on interaction where the accent
    // LIGHTENS, and that is not an inconsistency: #DC2626 already measures
    // only 4.83:1 against the white label it carries, so a lighter hover
    // takes the pair below AA — the first attempt at #EF4444 measured 3.76
    // and the assertion below caught it. 5.72:1 and 7.45:1.
    readonly property color _dangerFillHover:  "#C81F1F"
    readonly property color _dangerFillPressed:"#A81919"
    // Away presence. It is a DOT, so it is held to the 3:1 graphical-object
    // bar rather than 4.5:1 — but it must clear the Storm bolt by CIE76 dE,
    // or an away contact reads as a highlighted one. The old invariant
    // #C9B23A measured dE 18.3 from #FFD447 and `warning` #E5A23C measured
    // 23.8, so three unrelated signals all rendered as "bolt". These clear
    // at dE 38.5 (dark) and 47.4 (light); warning now clears at 33.8 / 53.8.
    readonly property color _awayLight:        "#BA671B"
    readonly property color _awayDark:         "#F59349"

    // ---- Reaction-pill surfaces (2026-08-21). ----
    // reactionBackground was an alias of cardElevated, so on Storm a pill,
    // a hovered row and a selected row were one navy. Each value is one
    // visible step off ITS OWN theme's message surface — lighter on the dark
    // themes, darker on the light ones — with the theme's secondary ink kept
    // at >= 4.6:1 on it (measured worst case 4.71, on Warm).
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

    // ---- Media / scrim chrome (theme-invariant by design). ----
    // The image viewer and the video control bar are committed-dark surfaces:
    // they sit over arbitrary media, so they do NOT follow the theme. Before
    // this round they carried nine different literals across two files with
    // exactly one token (scrimInk) to their name, which is why the token
    // audit could report "covered" for a role painted entirely by hex.
    // _scrimBase is the OPAQUE equivalent of scrimSurface's #D9111111 — it
    // exists so the theme test can assert the scrim inks against a real
    // background instead of an 8-digit ARGB the contrast maths cannot read.
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
        // `link` cannot be the accent: white-on-accent is pinned
        // at 3:1, which CAPS the accent's luminance, while an AA
        // link needs more than that cap on the raised surfaces.
        // Over-determined, so links get their own ink.
        link: _lightLink, rail: _railLight,
        // NOT _hoverLight, which is what it was: one literal served both the
        // incoming bubble AND row hover, so pointing at a message in a DM
        // dissolved its bubble into the row highlight.
        otherBubble: _cardElevatedLight
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
        ownBubble: _outgoingBubbleBlue, otherBubble: _dkCard,
        // NOT _dkCardElevated: that is the same literal cardElevated uses, so
        // a raised chip drawn ON an incoming bubble (link preview, poll, the
        // pagination and navigation pills, the Qt `button` palette role) was
        // invisible — ratio 1.000. Pointing the bubble at the panel tone puts
        // the chip 1.201 above it and matches what Lightning Light and Storm
        // already do. _dkCard is already in the test's darkInkSurfaces(), so
        // the bubble surface keeps its 13-ink coverage with no test edit.
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
        // `link` cannot be the accent: white-on-accent is pinned
        // at 3:1, which CAPS the accent's luminance, while an AA
        // link needs more than that cap on the raised surfaces.
        // Over-determined, so links get their own ink.
        link: _midLink, rail: _bgDark
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
        ownBubble: _graOwnBubble, otherBubble: _graOtherBubble,
        reaction: _graReaction,
        // `link` cannot be the accent: white-on-accent is pinned
        // at 3:1, which CAPS the accent's luminance, while an AA
        // link needs more than that cap on the raised surfaces.
        // Over-determined, so links get their own ink.
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
        // `link` cannot be the accent: white-on-accent is pinned
        // at 3:1, which CAPS the accent's luminance, while an AA
        // link needs more than that cap on the raised surfaces.
        // Over-determined, so links get their own ink.
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
        // The rail shares the timeline's ground rather than the room list's
        // surface (what Storm does too). Without it `rail` falls back to
        // `sidebar` and the 68px spaces rail and the 300px room list are one
        // colour with no edge between them — 1.000, dE 0.00.
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
        // `link` cannot be the accent: white-on-accent is pinned
        // at 3:1, which CAPS the accent's luminance, while an AA
        // link needs more than that cap on the raised surfaces.
        // Over-determined, so links get their own ink.
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
        // `link` cannot be the accent: white-on-accent is pinned
        // at 3:1, which CAPS the accent's luminance, while an AA
        // link needs more than that cap on the raised surfaces.
        // Over-determined, so links get their own ink.
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
        // `link` cannot be the accent: white-on-accent is pinned
        // at 3:1, which CAPS the accent's luminance, while an AA
        // link needs more than that cap on the raised surfaces.
        // Over-determined, so links get their own ink.
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
        // `link` cannot be the accent: white-on-accent is pinned
        // at 3:1, which CAPS the accent's luminance, while an AA
        // link needs more than that cap on the raised surfaces.
        // Over-determined, so links get their own ink.
        link: _teaLink
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
        { id: 7,  name: qsTr("Warm") },
        // The user-authored palette. Listed LAST and only once it exists —
        // an empty custom theme in the picker is a row that does nothing.
        // Settings → Appearance offers the editor separately, so this entry
        // is purely for re-selecting a theme already built.
        { id: 12, name: qsTr("Your theme") }
    ]

    // THE theme-id -> palette switch. One switch, three consumers (_p, the
    // preview cards, and the custom theme's base lookup) so a new theme
    // cannot be routed in one place and missed in another.
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

    // The user-authored palette: `customBase`'s preset with the user's own
    // colours laid over it.
    //
    // The base is clamped to a real PRESET, never to 12 — a custom theme
    // based on the custom theme is a cycle, and QML would resolve it as an
    // undefined palette rather than as an error.
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
                // Second gate. CustomThemeStore already sanitised this, but
                // it is the one palette whose contents reach a config file a
                // user can edit by hand, and an unparseable colour here
                // paints the shell transparent rather than failing loudly.
                if (typeof v === "string" && /^#[0-9A-Fa-f]{6}$/.test(v))
                    out[role] = v
            }
        }
        return out
    }

    // Palette lookup for a theme BY ID, resolved to semantic roles.
    //
    // Two consumers: the Settings theme-preview cards, and ThemePreviewDemo
    // inside the custom-theme editor. The editor's preview must render a
    // theme the application is NOT currently running, so it cannot read the
    // live aliases below — it asks for a palette by id and paints from that.
    //
    // Every fallback here MUST match the semantic alias of the same name.
    // They drifted once already (accentSoft/accentBorder fell back to a
    // translucent accent while the aliases fell back to `selected` /
    // `borderStrong`, so a preview card painted chrome the running theme
    // never renders). `previewPaletteMatchesLiveTokens` in
    // tests/SettingsShellQmlTest.cpp now walks every id and every key here
    // against the live singleton, so the two cannot separate again.
    function paletteForTheme(id) {
        var p = rawPaletteForTheme(id)
        // The two palette-shaped answers the aliases take from OUTSIDE the
        // palette object. `dark` is id-based for the presets and
        // luminance-based for the custom theme, exactly like the `dark`
        // property; `isStorm` gates the two embed roles.
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
    // Role names for the three shell regions. They are aliases on purpose —
    // reach for the one that says what the surface IS (the window's ground,
    // the nav column, a side panel) rather than for `background`/`sidebar`,
    // so a future theme can pull them apart without a call-site sweep.
    readonly property color windowBackground:    background
    readonly property color sidebar:             _p.sidebar
    readonly property color navBackground:       sidebar
    readonly property color panelBackground:     sidebar
    // Spaces-rail surface; palettes without a dedicated rail tone reuse the
    // sidebar so the shell stays coherent.
    readonly property color rail:                _p.rail !== undefined
                                                 ? _p.rail : _p.sidebar
    // ── Channels navigation layout ───────────────────────────────────────
    //
    // DERIVED, not added to eleven palettes. Every one of these falls back to
    // a token the palette already defines, so the Channels layout works in
    // all eleven themes on the day it ships and a palette can override any
    // single tone later without touching this file. A new required key in
    // eleven palettes is how a theme ends up with one undefined colour and a
    // transparent row.
    //
    // The category header is a quiet, all-caps label — Sable and Discord both
    // make it the least prominent thing in the column, because it is
    // structure rather than content and it repeats down the whole list.
    readonly property color channelCategoryText:
        _p.channelCategoryText !== undefined ? _p.channelCategoryText
                                             : textMuted
    /// The channel name at rest. Deliberately DIMMER than a Classic room
    /// row's name: a channel list is long and mostly-read, so "read" is the
    /// resting state and unread is what earns full-strength ink.
    readonly property color channelText:
        _p.channelText !== undefined ? _p.channelText : textSecondary
    /// A channel with unread messages. Full-strength, because in this layout
    /// weight is the ONLY unread signal for a row without a count.
    readonly property color channelTextUnread:
        _p.channelTextUnread !== undefined ? _p.channelTextUnread : text
    /// The channel you are in. Reuses the room list's own selected pair so a
    /// user switching layouts does not have to relearn what "here" looks
    /// like.
    readonly property color channelSelected:
        _p.channelSelected !== undefined ? _p.channelSelected : selected
    readonly property color channelSelectedText:
        _p.channelSelectedText !== undefined ? _p.channelSelectedText
                                             : selectedText
    readonly property color channelHover:
        _p.channelHover !== undefined ? _p.channelHover : hover
    /// The unread dot/rail on a channel row. The accent, so it reads as
    /// "attention" rather than as an error.
    readonly property color channelUnreadMark:
        _p.channelUnreadMark !== undefined ? _p.channelUnreadMark : accent

    // ── Spaces rail: folders and drag feedback ───────────────────────────
    //
    // DERIVED for the same reason the Channels tokens are: a new required key
    // in eleven palettes is how a theme ends up with one undefined colour and
    // a transparent row, and the only thing that catches it is the
    // no-QML-warnings gate.
    //
    // The container drawn behind an OPEN folder and its Spaces. It has to read
    // as one surface holding several tiles, which is the whole difference
    // between a folder and four Spaces that happen to be adjacent.
    readonly property color railFolderSurface:
        _p.railFolderSurface !== undefined ? _p.railFolderSurface
                                           : cardElevated

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
    // ---- Status roles: INK and FILL are different colours. ----
    // Before 2026-08-21 these four were theme-invariant literals used for
    // both jobs, and `danger` #DC2626 measured 3.03–4.03:1 on Storm's four
    // surfaces — below AA everywhere on the brand theme, at ~40 ink call
    // sites, with no test asserting it. INK now routes by mode (and can be
    // overridden per palette via the `_p` idiom the other roles use); FILL
    // keeps the saturated mid-tone that white text sits on.
    //
    // Pick by role, not by name: text, icons and inline labels take
    // `danger` / `warning` / `success` / `info`; a solid destructive button
    // or badge takes `dangerFill` with `dangerText` on it.
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
    // Solid destructive fills (Remove, Leave, Sign out …). Theme-invariant
    // on purpose: a destructive confirmation reads the same everywhere, and
    // white-on-red is the one pair every user already knows.
    readonly property color dangerFill:          _accentDanger
    readonly property color dangerFillHover:     _dangerFillHover
    readonly property color dangerFillPressed:   _dangerFillPressed
    readonly property color dangerText:          "#FFFFFF"
    // Solid non-destructive status fills, for badges that need a filled
    // chip rather than a tinted one. Ink on all three is dangerText.
    readonly property color successFill:         _accentGreen
    readonly property color warningFill:         _accentWarning
    readonly property color infoFill:            _accentInfo
    // v0.6.5 danger roles (SPEC §0 names "mentionBadge/danger red"; the two
    // diverge, so they are split by role): dangerInk for icon/label ink,
    // the soft tint pair for destructive-row fills and warning-chip borders.
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
    // Reactions and badges. reactionBackground is its OWN per-theme surface,
    // not an alias of cardElevated: aliasing it meant a reaction pill, a
    // hovered row and a raised card were one tone on Storm. The selected
    // (own-reaction) pill rides accentSoft — the sanctioned "this is your
    // current selection" accent tint — with accentBorder around it.
    // In-timeline EMBED cards: voice/audio, polls, link previews, file and
    // video placeholders. These sit on the TIMELINE ground — the deepest
    // surface in the theme, with a transparent bubble over it in the Modern
    // layout — not on a panel. cardElevated is the step above a PANEL, so
    // used here it rendered as a pale block floating on near-black, which is
    // the 2026-08-21 report "the voice message poll and other imbed color
    // like the one from links is too pale make it dark blue".
    //
    // This is the raised-but-DARK step: still clearly lifted off the ground,
    // still unmistakably navy rather than slate.
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
    // palettes fall back to the shared danger FILL, never to `danger`: this
    // is a filled pill carrying white ink, and `danger` became a light rose
    // on the dark themes this round (a rose pill under white text is
    // unreadable — the exact regression this fallback would have shipped).
    readonly property color mentionBadge:        _p.mention !== undefined
                                                 ? _p.mention : _accentDanger
    // The inline "@name" chip inside message text. A tinted wash plus the
    // body ink, deliberately WITHOUT a border: an outlined chip reads as a
    // red error box around the name rather than as a highlight.
    readonly property color mentionChipFill:     Qt.alpha(mentionHighlight, 0.16)
    readonly property color mentionChipInk:      textPrimary
    readonly property color mentionChipBorder:   "transparent"
    // The same chip when it is YOU being mentioned: the accent tint, so the
    // row that concerns the reader is the one that carries the accent.
    readonly property color mentionSelfFill:     accentSoft
    readonly property color mentionSelfInk:      textPrimary
    readonly property color undecryptableText:   textMuted
    // Highlight semantics (jumped-to message rows, active thread affordances).
    readonly property color pressedSurface:      selectedHover
    readonly property color messageHighlight:    selected
    readonly property color threadHighlight:     accent
    // Presence dots (design: online = accent-family green, away = yellow,
    // offline = the theme's muted ink — visibly "off" on every palette).
    readonly property color presenceOnline:      _p.online !== undefined
                                                 ? _p.online : success
    // Away was a theme-invariant #C9B23A: CIE76 dE 18.3 from the Storm bolt
    // #FFD447, so an away contact rendered as brand chrome. Routed by mode
    // and moved to amber-orange, clearing the bolt at dE 38.5 / 47.4.
    readonly property color presenceAway:        _p.away !== undefined
                                                 ? _p.away
                                                 : (dark ? _awayDark
                                                         : _awayLight)
    readonly property color presenceOffline:     textMuted

    // ---- Control surfaces (2026-08-21). ----
    // Buttons, icon chips and segmented controls were computing their own
    // hover and pressed steps with Qt.darker(AppTheme.bolt, 1.05 / 1.12) at
    // four call sites, which (a) made hover DARKER where the designed step
    // is LIGHTER, so hover read as pressed, and (b) produced a different
    // result on each of the eleven palettes that no parse-based test can
    // see. Every state below is a theme-routed token. Reach for these
    // instead of multiplying a colour at the call site.
    readonly property color buttonPrimaryFill:    accent
    readonly property color buttonPrimaryHover:   accentHover
    readonly property color buttonPrimaryPressed: accentPressed
    readonly property color buttonPrimaryInk:     accentText
    readonly property color buttonNeutralFill:    cardElevated
    readonly property color buttonNeutralHover:   selected
    readonly property color buttonNeutralPressed: selectedHover
    readonly property color buttonNeutralInk:     textPrimary
    readonly property color buttonNeutralBorder:  border
    // Ghost = no resting fill; the surface only appears under the pointer.
    readonly property color buttonGhostHover:     hover
    readonly property color buttonGhostPressed:   selected
    readonly property color buttonGhostInk:       textSecondary
    readonly property color buttonDangerFill:     dangerFill
    readonly property color buttonDangerHover:    dangerFillHover
    readonly property color buttonDangerPressed:  dangerFillPressed
    readonly property color buttonDangerInk:      dangerText
    // A disabled control keeps its shape and loses its voice. WCAG exempts
    // disabled controls from the contrast minimum, which is why the ink is
    // allowed to be textDisabled here and nowhere else.
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

    // ---- Status chips. ----
    // A chip is a TINT of its own ink, never a solid accent: before this
    // round StatusChip folded "accent", "onAccent" and "bolt" into one
    // bolt-yellow fill and everything else into one muted periwinkle, so
    // ACTIVE, MOD and VERIFIED were the same pill. Six families, each with
    // its own ink; the fill is that ink at 14% and the border at 32%, so a
    // chip cannot drift away from the ink it carries.
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
    // The one SOLID chip: "this is the current selection / this is verified".
    // Reserve it — the audit found the bolt already doing five jobs at once
    // (brand mark, home tile, focus ring, active state, primary action), and
    // an accent that means five things means none of them.
    readonly property color chipBoltFill:      bolt
    readonly property color chipBoltInk:       boltInk
    readonly property int   chipHeight:        20
    readonly property int   chipRadius:        radiusPill
    readonly property int   chipPaddingH:      spacing8

    // ---- Scrollbars. ----
    // The track stays out of the way; the handle is a theme ink, so it is
    // legible on every surface without a per-pane literal.
    readonly property color scrollbarTrack:         "transparent"
    readonly property color scrollbarTrackHover:    Qt.alpha(border, 0.35)
    readonly property color scrollbarHandle:        borderStrong
    readonly property color scrollbarHandleHover:   textDisabled
    readonly property color scrollbarHandlePressed: textMuted
    readonly property int   scrollbarWidth:      10
    readonly property int   scrollbarWidthThin:   6
    readonly property int   scrollbarRadius:     radiusPill
    readonly property int   scrollbarMargin:      2

    // ---- Elevation. ----
    // `shadow` (above) is the single per-theme tint; these are the three
    // sanctioned steps, so a stacked popover can sit visibly above the
    // popover that opened it instead of both borrowing the card shadow.
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

    // ---- Media / scrim chrome. ----
    // Committed dark, on every theme: these paint over arbitrary media, so
    // they must not follow the palette. The image viewer and the video
    // control bar carried nine literals between them and one token before
    // this round; that is the whole family.
    readonly property color scrimBackdrop:       "#66000000"  // behind media
    readonly property color scrimSurface:        "#D9111111"  // chrome bar
    readonly property color scrimSurfaceRaised:  "#33FFFFFF"  // button on it
    readonly property color scrimSurfaceHover:   "#59FFFFFF"
    readonly property color scrimBorder:         "#33FFFFFF"
    readonly property color scrimInkStrong:      "#E6FFFFFF"
    readonly property color scrimInkMuted:       "#94A3B8"

    // ---- Storm surface language (0.6.5). ----
    // The shared vocabulary of every menu, popover, picker, dialog and the
    // Settings surface. Since the Storm round made Storm a SELECTABLE
    // full-application theme (id 11), this namespace is theme-ROUTED, not
    // invariant: under Storm each token carries the §1 navy/bolt literal;
    // under every legacy theme it resolves to that theme's own semantic
    // equivalent, so menus and Settings follow the user's chosen theme
    // again. Consumers keep reaching for storm* by role and never branch on
    // the theme themselves. There is NO exception left: the Sessions trust
    // card was the last invariant surface and 2026-08-26 routed it here too
    // (see the trust-role note below the derived treatments).
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
    readonly property color boltInk:            storm ? _stoBoltInk : accentText
    // The wordmark's trailing bolt is a BRAND MARK, not a control. Painted in
    // the raw accent it put a primary-action blue immediately beside plain
    // header text, where it read as a status light rather than as part of the
    // name — reported as "the blue lightning session status". Storm keeps its
    // brand yellow; every other theme blends the accent most of the way to
    // the header's own secondary ink, so the mark is still recognisably the
    // accent and no longer competes with the primary button below it.
    readonly property color wordmarkBolt:       storm ? _stoBolt
                                                : Qt.tint(textSecondary,
                                                          Qt.alpha(accent, 0.55))
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

    // ---- The trust surface has NO tokens of its own (2026-08-26). ----
    //
    // There used to be ten `trust*` tokens here, pinned to the raw _sto*
    // literals — trustNavy: _stoPanel, trustYellow: _stoBolt, trustInk,
    // trustMuted, trustChainBg, trustChainBorder, trustPending, plus three
    // bare hex inks (trustCaption #AAB5E0, trustCaptionDim #6F7EB6,
    // trustVerifyInk #C9D2F2). They were pinned deliberately: when Storm
    // became a selectable theme the storm* namespace above was converted
    // from invariant to ROUTED, and the trust card was held back so the
    // "trust moment" stayed the brand moment in every theme.
    //
    // That belief was wrong once its neighbourhood moved. The card sits
    // between SettingsCards painted stormCanvas/stormBorder, above a
    // sessions list already painted stormTextFaint/stormLink — so on any
    // theme but Storm the one surface that never followed the theme was the
    // one the eye reads as foreign. Reported 2026-08-26: "the blue lightning
    // session status should match the rest of the theme."
    //
    // TrustCard.qml now reaches for storm* by ROLE like every other surface:
    //   card fill        -> stormCanvas        (was trustNavy)
    //   card/chain edge  -> stormBorder        (was trustChainBorder)
    //   chain panel      -> stormInset         (was trustChainBg)
    //   display name     -> stormText          (was trustInk)
    //   user id, status  -> stormTextMuted     (was trustMuted)
    //   captions         -> stormTextSecondary (was trustCaption/trustVerifyInk)
    //   pending caption  -> stormTextMuted     (was trustCaptionDim)
    //   complete state   -> bolt / boltInk     (was trustYellow / trustNavy)
    //   pending state    -> stormBorderStrong  (was trustPending)
    // Under Storm every one of those lands on the SPEC §1 literal the pin
    // used, with two deliberate exceptions worth naming rather than
    // discovering: the card fill moves _stoPanel #202473 -> _stoCanvas
    // #121655 (because stormCanvas is what SettingsCard paints, and matching
    // it on Storm too is the same fix, not a separate one), and the 120px
    // watermark moves from a 10%-opacity bolt to stormWatermark's 12% alpha
    // (the token IdentityCard and MemberProfilePopover already use for that
    // glyph). TrustCardTest::stormKeepsTheBrandLiterals pins all of it by
    // value. ThemeTokensTest asserts the replacement AA pairs per theme
    // instead of once — an invariant palette needed eight assertions, a
    // routed one needs them on all eleven themes.

    // The custom-theme editor's own chrome (Settings -> Appearance -> Edit).
    //
    // It follows the SELECTED theme like everything else — opening a Moss
    // Light window into a navy workspace was jarring and wrong — with one
    // exception that is the whole reason these tokens exist at all.
    //
    // The editor must never paint itself from the palette it is EDITING. A
    // user can set the panel white and the body ink white in two clicks, and
    // if the editor followed that, the role list, the picker and the button
    // that undoes it would all vanish at the same moment: the editor would
    // have locked its user out of the only control that fixes it. Screenshot
    // evidence, 2026-08-22.
    //
    // So the chrome resolves against a PRESET, always — the selected one
    // while a preset is selected, and the custom theme's own BASE preset once
    // the user applies their custom theme (a base is clamped to 1..11 by
    // construction, so it can never be the thing being edited). Preset
    // literals are not user-editable, which is the property that matters; the
    // theme being authored appears only inside the preview, where it belongs.
    //
    // This is also why the editor draws its own buttons, fields and scrollbar
    // instead of reaching for AppButton / AppTextField / AppComboBox: those
    // follow the storm* namespace, which follows the effective theme and
    // would therefore follow the custom palette.
    readonly property int editorChromeTheme: {
        if (effectiveTheme === 12)
            return (customBase >= 1 && customBase <= 11) ? customBase : 11
        return (effectiveTheme >= 1 && effectiveTheme <= 11) ? effectiveTheme : 11
    }
    readonly property var _editorPalette: paletteForTheme(editorChromeTheme)
    // Storm keeps its own deeper chrome tones; every other theme uses its own
    // semantic surfaces, the same routing the storm* namespace does.
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

    // Deterministic initials-avatar discs, DERIVED FROM THE ACTIVE THEME.
    //
    // This used to be one fixed nine-colour ladder shared by every theme, on
    // the reasoning that a user should keep one colour everywhere. The
    // 2026-08-21 round then moved its centre of gravity warm, and on a cool
    // theme that reads as a mistake rather than as identity: a deep indigo
    // window with amber and rust discs down its room list. The slots are now
    // nine evenly spaced hues in a 190-degree arc CENTRED ON THIS THEME'S
    // ACCENT, so the family belongs to the theme it is sitting in.
    //
    // The arithmetic lives in C++ (src/theme/IdentityPalette.*) and NOT here,
    // because the desktop-notification fallback avatar is painted with no QML
    // engine anywhere near it. A second implementation of this in C++ is
    // exactly the thing that drifts — a hand-kept copy of the old array
    // already did, and the same person had one colour in the window and
    // another in their notifications. One implementation, two callers.
    //
    // `accent` is read here on purpose: it makes every binding that calls
    // avatarColor() depend on the theme, custom overrides included, so the
    // discs follow a theme switch with no extra signal.
    //
    // The ORIGINAL fixed ladder, kept as the reference the derivation was
    // measured against (nine hues at 4/28/44/95/155/192/225/272/325 degrees,
    // closest pair dE 22). Nothing reads it any more.
    readonly property var avatarPaletteLegacy: [
        "#D04339", "#AE6424", "#8F7224", "#4F822B", "#2E8460",
        "#2F7F93", "#4163C8", "#8941C8", "#C84190"
    ]
    // The ONE identity hash behind avatar fills and sender-name inks, so a
    // user's avatar disc and name label always agree on the hue family.
    // (Formerly private to Avatar.qml; lifted here so name colouring cannot
    // drift out of sync with it.)
    function identityIndex(key) {
        var h = 0
        for (var i = 0; i < key.length; ++i)
            h = ((h << 5) - h + key.charCodeAt(i)) | 0
        return Math.abs(h) % 9
    }
    // The colour the discs are derived from.
    //
    // Usually the accent: in ten of eleven themes the accent IS the shell's
    // own hue (never more than 29 degrees from the background). Storm is the
    // exception — a navy shell at 233 degrees with the brand bolt at 46 — and
    // anchoring there built a magenta-red-orange-lime family and dropped it
    // onto a navy window. So the accent anchors the discs unless it is
    // nowhere near the surface they sit on, in which case the surface wins.
    //
    // A custom theme gets the same rule applied to its own two colours, live.
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
    // The initials ink for that disc. NEVER assume white: half of these
    // discs are pale, which is what separates them from each other once
    // their hues share one family.
    function avatarInk(key) {
        return IdentityPalette.ink(identityIndex(key), identityAnchor)
    }
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
    //
    // 2026-08-21: rebalanced WARM and de-duplicated. The previous set spent
    // two of its nine slots on collisions — hues 20.8/24.9 and 147.7/150.0 —
    // so its closest pair was dE 5.6 (dark) and 7.4 (light), which is "the
    // same colour" to a reader: two users hashed to indistinguishable names.
    // The replacement walks nine separated hues (4/28/44/95/155/192/225/272/
    // 325) with the centre of gravity moved warm — coral, orange, gold and
    // rose, plus a warm-leaning olive — and its closest pair is dE 18.1 dark
    // / 24.5 light.
    //
    // A full hue sweep clears 4.5:1 on EVERY hue in both modes, so the gate
    // below never constrained the hue choice; the old palette's coldness was
    // a choice, not a limit.
    //
    // One Storm trap, measured: the brand accent is _stoBolt #FFD447, and a
    // gold slot at hue 50 / sat 0.75 lands dE 10 from it — a sender's name
    // then reads as brand chrome. The gold slot is therefore hue 44 at the
    // lower saturation that buys dE 37. Minimum bolt clearance across this
    // set is 37; do not raise that slot's saturation or move its hue toward
    // 48 without re-checking it.
    readonly property var _nameInksDark: [
        "#F6ACA7", "#F4B176", "#D4BD7D", "#96CF6E", "#7ECEAC",
        "#87C9D9", "#AEBEEF", "#D3B2F0", "#F1ACD4"
    ]
    readonly property var _nameInksLight: [
        "#C31F13", "#9C4F0D", "#776128", "#416B24", "#2A6F52",
        "#276B7C", "#2D58D7", "#8A31D8", "#B81E78"
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
    // Legacy name for the danger INK (RoomDelegate's failed-send marker and
    // four other call sites). Follows `danger`, so it is readable on every
    // theme; a destructive FILL must ask for dangerFill by name.
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
    //
    // THE TYPE SCALE (2026-08-21). Use these six names and nothing else.
    //
    // The audit that produced them counted 15 distinct rendered sizes,
    // reached through 24 token names carrying 11 values PLUS 262 bare
    // numeric literals in qml/. Three names for one number (fontSizeS ==
    // fontSecondary == fontMono == 13; fontSizeXS == fontCaption ==
    // fontMonoXS == 11) meant no call site could tell which one was right,
    // so authors typed the number instead — which is what makes the UI read
    // as accidental rather than designed. Element ships about five sizes.
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
    //   textMicro      10  weightBold    700 unread badges and keycaps ONLY
    //
    // Five numeric sizes (10/12/14/16/22) and three line heights. A "strong"
    // variant of body is a WEIGHT change at textBody, never a size change.
    //
    // Everything below this block is retained so the 78 files that already
    // consume the old names keep compiling and rendering exactly as they do
    // today; each is annotated with the scale token that supersedes it.
    // Convert a file when you are already editing it — do NOT do a global
    // sweep in the same change as a behaviour fix.
    readonly property int textDisplay:   22
    readonly property int textTitle:     16
    readonly property int textSubtitle:  14
    readonly property int textBody:      14
    readonly property int textMeta:      12
    readonly property int textMicro:     10

    // Three weights carry every role. The tree currently uses five
    // (DemiBold 98, Bold 65, ExtraBold 28, Medium 8, plus Normal and one
    // `font.bold: true`), which is more variation than the design has roles
    // for. font.weight takes an int in Qt 6, so these are usable directly.
    readonly property int weightBody:    400
    readonly property int weightMedium:  500
    readonly property int weightStrong:  600
    readonly property int weightBold:    700
    readonly property int weightDisplay: 800

    // Leading. Message body text sets NO lineHeight today — in the entire
    // tree `lineHeight` appears twice, and neither is a message — so Qt
    // falls back to the font's own hhea metrics and the UI-font picker
    // silently changes chat leading by 13% (Manrope 1.366em, Inter 1.210em).
    // Set `lineHeight: AppTheme.lineHeightBody; lineHeightMode:
    // Text.ProportionalHeight` on every WRAPPING text item; single-line
    // chrome keeps the default.
    readonly property real lineHeightBody:    1.5
    readonly property real lineHeightTight:   1.3
    readonly property real lineHeightDisplay: 1.2

    // ---- Superseded size tokens (retained; do not add new call sites). ----
    readonly property int fontSizeXS:        11   // -> textMeta / textMicro
    readonly property int fontSizeS:         13   // -> textBody / textMeta
    readonly property int fontSizeM:         14   // -> textBody
    readonly property int fontSizeRoom:      16   // -> textTitle
    readonly property int fontSizeHeader:    18   // -> textTitle
    // DEAD: zero call sites in qml/, src/ or tests/ before this round.
    // fontSizePageTitle and fontSizeXL are kept only because removing a
    // token is a separate, sweepable change; fontPageTitle is REQUIRED by
    // tests/ThemeTokensTest.cpp and is therefore given the real display
    // role instead (22, the size five hero labels already use as a literal).
    readonly property int fontSizePageTitle: 24   // DEAD -> textDisplay
    readonly property int fontSizeL:         fontSizeRoom
    readonly property int fontSizeXL:        fontSizeHeader // DEAD -> textTitle

    readonly property int fontPageTitle:     textDisplay       // 22
    readonly property int fontSectionTitle:  fontSizeHeader    // 18 -> textTitle
    readonly property int fontRoomTitle:     fontSizeRoom      // 16 -> textTitle
    readonly property int fontBody:          fontSizeM         // 14 -> textBody
    // NOTE the misnomer: `fontSecondary` is the size of the PRIMARY menu-item
    // label. -> textBody.
    readonly property int fontSecondary:     fontSizeS         // 13 -> textBody
    readonly property int fontMessageSender: 12                // -> textMeta
    readonly property int fontCaption:       fontSizeXS        // 11 -> textMeta
    readonly property int fontMono:          fontSizeS         // 13 -> textBody
    // v0.6.5 menu-language sizes. font.pixelSize is an int in Qt, so the
    // spec's half-pixel sizes are resolved to whole numbers (rounded up so
    // small ink stays legible). Chrome sizes — never wrapped in scaled().
    readonly property int fontMicro:     9   // trust captions, GIF badge, role chips
    readonly property int fontChip:      10  // keycaps, section labels -> textMicro
    readonly property int fontMonoXS:    11  // mono identity strings -> textMeta
    readonly property int fontMonoSm:    12  // footer hints, status lines -> textMeta
    readonly property int fontResult:    14  // result-row titles -> textSubtitle
    readonly property int fontQuery:     15  // search/omnibox query -> textTitle
    readonly property int fontTrustName: 17  // trust-card display name -> textTitle
    readonly property int fontNavTitle:  17  // Settings-nav pane title -> textTitle

    // ---- Font families. ----
    // The selectable bundled UI families ship with Lightning (data/fonts,
    // loaded in main.cpp); Main.qml pushes the persisted per-account
    // selection in here, and the family lists below stay as graceful
    // fallbacks for stripped-down builds. Mono, icon, and emoji roles are
    // deliberately not affected by the UI font selection.
    property string uiFont:          "Manrope"
    // The bundled UI families differ by up to 13% in x-height, so choosing
    // "Source Sans 3" shrinks apparent text ~11% with no size change — two
    // settings that are supposed to be independent, coupled through the font
    // metrics. This normalises every family to Manrope's 0.540em x-height
    // (OS/2 sxHeight / unitsPerEm, read from data/fonts). Manrope itself is
    // 1.0, so the DEFAULT rendering is byte-identical to before.
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
    // The icon face. Icon.qml hard-coded this string with no token and no
    // fallback list, which is the only raw family literal in the tree that
    // is neither a deliberate preview nor a mirror.
    readonly property string iconFont:         "Material Symbols Rounded"
    readonly property string monoFont:         "JetBrains Mono"
    readonly property var    monoFontFamilies: [
        "JetBrains Mono",
        "Fira Mono",
        "SF Mono",
        "Consolas",
        "monospace"
    ]
    // v0.6.5: brand face — originally the trust surface only (SPEC 1r); the
    // Storm language (SPEC-storm-language §2) then extended it to every menu
    // item label, title and button. It is BACK to the trust surface and the
    // wordmark (2026-08-21) — see menuFont below.
    readonly property string brandFont:         "Space Grotesk"
    readonly property var    brandFontFamilies: [
        "Space Grotesk",
        "Manrope",
        "sans-serif"
    ]
    // The wordmark / trust-surface face, by role rather than by brand name.
    readonly property string displayFont:       brandFont

    // The menu-surface label face. It was a hard alias of brandFont, applied
    // at 61 call sites — every menu item, every dialog title, the quick
    // switcher, the member popover, Settings chrome, even message-search
    // result bodies — while the timeline next to them rendered Manrope. Three
    // things were wrong with that:
    //   * it is not theme-routed, so the Storm display language shipped on
    //     Lightning Light, Moss Light, Nordic and Warm, where it was never
    //     designed to appear;
    //   * it is deliberately absent from the Settings font list, so a user
    //     who picks "Inter" still got Space Grotesk on half the UI — the
    //     picker was lying;
    //   * Space Grotesk's x-height is 0.486em against Manrope's 0.540em, so
    //     a 13px menu label optically reads ~11.7px beside 13px Manrope. The
    //     sizes were specified per-face with no optical correction, which is
    //     why the chrome reads lighter and smaller than the body it borders.
    // menuFont now follows the user's chosen UI face. The brand face is kept
    // for the wordmark and the trust card, which is what it was drawn for.
    // (opticalScale() above still carries Space Grotesk's factor, so a
    // surface that deliberately reaches for displayFont can correct for it.)
    readonly property string menuFont:          uiFont

    // Menu / popover SECTION labels ("ROOMS", "RECENTLY", the message-menu
    // context header). The current treatment is JetBrains Mono at 10px with
    // 1.6px tracking in full caps — decorative terminal typography carrying
    // wayfinding text, re-typed inline in seven places, and not theme-routed
    // either. These tokens are the replacement: sentence case, UI face,
    // meta size, strong weight, no tracking. Keep mono for what is genuinely
    // monospaced — code, keycaps, and Matrix identifiers.
    readonly property string menuSectionFont:    uiFont
    readonly property int    menuSectionSize:    textMeta
    readonly property int    menuSectionWeight:  weightStrong
    readonly property real   menuSectionTracking: 0.0
}
