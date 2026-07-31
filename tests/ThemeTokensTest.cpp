// v0.5.9: theme-token and contrast tests. Parses qml/AppTheme.qml as text
// (no QML runtime needed): asserts the required semantic tokens exist,
// computes WCAG 2.1 contrast for the critical colour pairs in both themes,
// and verifies core view QML files contain no stray hex colours outside
// the AppTheme singleton.

#include <QFile>
#include <QRegularExpression>
#include <QtTest/QtTest>

#include <cmath>

namespace {

QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

double channelLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QString &hex)
{
    const QString h = hex.mid(1); // strip '#'
    const double r = h.mid(0, 2).toInt(nullptr, 16) / 255.0;
    const double g = h.mid(2, 2).toInt(nullptr, 16) / 255.0;
    const double b = h.mid(4, 2).toInt(nullptr, 16) / 255.0;
    return 0.2126 * channelLinear(r) + 0.7152 * channelLinear(g)
        + 0.0722 * channelLinear(b);
}

double contrast(const QString &fg, const QString &bg)
{
    const double lf = luminance(fg);
    const double lb = luminance(bg);
    const double hi = std::max(lf, lb);
    const double lo = std::min(lf, lb);
    return (hi + 0.05) / (lo + 0.05);
}

// Source-over composite of `top` at `alpha` onto opaque `bottom`, both
// #RRGGBB — what a translucent wash actually renders as. Used to assert
// legibility over tinted rows (the mention wash) instead of guessing.
QString composite(const QString &top, double alpha, const QString &bottom)
{
    auto ch = [](const QString &hex, int i) {
        return hex.mid(1 + i * 2, 2).toInt(nullptr, 16);
    };
    QString out = QStringLiteral("#");
    for (int i = 0; i < 3; ++i) {
        const int v = int(std::lround(alpha * ch(top, i)
                                      + (1.0 - alpha) * ch(bottom, i)));
        out += QStringLiteral("%1").arg(v, 2, 16, QLatin1Char('0')).toUpper();
    }
    return out;
}

} // namespace

class ThemeTokensTest : public QObject
{
    Q_OBJECT

    QString m_theme;
    QHash<QString, QString> m_colors; // property name -> #RRGGBB

private Q_SLOTS:
    void initTestCase()
    {
        m_theme = readAll(QStringLiteral(APPTHEME_QML_PATH));
        QVERIFY2(!m_theme.isEmpty(), "AppTheme.qml not readable");

        // Collect every direct colour literal: `property color name: "#..."`.
        const QRegularExpression re(QStringLiteral(
            "property\\s+color\\s+(\\w+)\\s*:\\s*\"(#[0-9A-Fa-f]{6,8})\""));
        auto it = re.globalMatch(m_theme);
        while (it.hasNext()) {
            const auto match = it.next();
            m_colors.insert(match.captured(1), match.captured(2));
        }
        // Storm aliases: the trust tokens are re-expressed as plain
        // references into the storm namespace (`property color trustNavy:
        // stormPanel`) — resolve ONE level of alias so the AA pairs below
        // keep asserting their real values.
        const QRegularExpression aliasRe(QStringLiteral(
            "property\\s+color\\s+(\\w+)\\s*:\\s*(\\w+)\\s*(?://.*)?$"),
            QRegularExpression::MultilineOption);
        auto aliasIt = aliasRe.globalMatch(m_theme);
        while (aliasIt.hasNext()) {
            const auto match = aliasIt.next();
            if (!m_colors.contains(match.captured(1))
                && m_colors.contains(match.captured(2))) {
                m_colors.insert(match.captured(1),
                                m_colors.value(match.captured(2)));
            }
        }
        QVERIFY(m_colors.size() > 20);
    }

    void requiredSemanticTokensExist()
    {
        const QStringList required = {
            // Surfaces
            QStringLiteral("windowBackground"), QStringLiteral("navBackground"),
            QStringLiteral("panelBackground"), QStringLiteral("surface"),
            QStringLiteral("surfaceElevated"), QStringLiteral("hover"),
            QStringLiteral("selected"), QStringLiteral("selectedHover"),
            QStringLiteral("inputBackground"),
            // Borders
            QStringLiteral("borderSubtle"), QStringLiteral("borderStrong"),
            // Text
            QStringLiteral("textPrimary"), QStringLiteral("textSecondary"),
            QStringLiteral("textMuted"), QStringLiteral("textDisabled"),
            // Accent + status
            QStringLiteral("accent"), QStringLiteral("accentHover"),
            QStringLiteral("accentPressed"), QStringLiteral("accentText"),
            QStringLiteral("success"), QStringLiteral("warning"),
            QStringLiteral("danger"), QStringLiteral("info"),
            // Messaging
            QStringLiteral("incomingBubble"), QStringLiteral("outgoingBubble"),
            QStringLiteral("codeBlock"), QStringLiteral("reactionBackground"),
            QStringLiteral("reactionSelectedBackground"),
            QStringLiteral("unreadBadge"), QStringLiteral("mentionBadge"),
            // Focus / overlay
            QStringLiteral("focusRing"), QStringLiteral("overlayScrim"),
            // Storm namespace (theme-routed; bolt ink pairs with bolt fills)
            QStringLiteral("bolt"), QStringLiteral("boltInk"),
            QStringLiteral("stormPanel"), QStringLiteral("stormText"),
        };
        for (const QString &token : required) {
            const QRegularExpression decl(
                QStringLiteral("property\\s+color\\s+%1\\b").arg(token));
            QVERIFY2(m_theme.contains(decl),
                     qPrintable(QStringLiteral("missing token: %1").arg(token)));
        }
        // Semantic typography roles.
        const QStringList type = {
            QStringLiteral("fontPageTitle"), QStringLiteral("fontSectionTitle"),
            QStringLiteral("fontRoomTitle"), QStringLiteral("fontMessageSender"),
            QStringLiteral("fontBody"), QStringLiteral("fontSecondary"),
            QStringLiteral("fontCaption"), QStringLiteral("fontMono"),
        };
        for (const QString &token : type) {
            const QRegularExpression decl(
                QStringLiteral("property\\s+int\\s+%1\\b").arg(token));
            QVERIFY2(m_theme.contains(decl),
                     qPrintable(QStringLiteral("missing type token: %1").arg(token)));
        }
    }

    void criticalPairsMeetContrast()
    {
        const auto c = [this](const char *name) -> QString {
            return m_colors.value(QLatin1String(name));
        };

        struct Pair {
            const char *fg;
            const char *bg;
            double minimum;
        };
        const Pair pairs[] = {
            // Normal text on the main surfaces — WCAG AA 4.5:1.
            { "_textPrimaryLight", "_bgLight", 4.5 },
            { "_textSecondaryLight", "_bgLight", 4.5 },
            { "_textMutedLight", "_bgLight", 4.5 },
            { "_textMutedLight", "_sidebarLight", 4.5 },
            { "_textPrimaryDark", "_bgDark", 4.5 },
            { "_textSecondaryDark", "_bgDark", 4.5 },
            { "_textMutedDark", "_bgDark", 4.5 },
            { "_textMutedDark", "_cardDark", 4.5 },
            // Selected room rows stay readable, including on hover.
            { "_selectedTextLight", "_selectedLight", 4.5 },
            { "_selectedTextLight", "_selectedHoverLight", 4.5 },
            { "_selectedTextDark", "_selectedDark", 4.5 },
            { "_selectedTextDark", "_selectedHoverDark", 4.5 },
            // Outgoing bubble body + muted meta ink.
            { "ownBubbleText", "_outgoingBubbleBlue", 4.5 },
            { "onAccentMuted", "_outgoingBubbleBlue", 4.5 },
            // Incoming bubble body, both themes.
            { "_textPrimaryLight", "_hoverLight", 4.5 },
            { "_textPrimaryDark", "_cardElevatedDark", 4.5 },
            // Danger buttons: white label on danger red.
            { "dangerText", "_accentDanger", 4.5 },
            // Controls/badges (large or bold UI text): ≥ 3:1.
            { "_onAccent", "_accentBlue", 3.0 },
            // Ink-on-accent for the themes that had no pair at any
            // threshold (review M2): once the storm* namespace routes,
            // boltInk-on-bolt becomes each theme's accentText-on-accent,
            // so every theme's pair must be asserted. Recorded trade: the
            // previously invariant navy-on-bolt 11.72:1 becomes
            // 3.09–6.89:1 under legacy themes (bold UI-chip text, 3:1 bar)
            // as the price of theme-following menus.
            { "_onAccent", "_graAccent", 3.0 },
            { "_onAccent", "_norAccent", 3.0 },
            { "_onAccent", "_purAccent", 3.0 },
            // Lightning Dark.
            { "_dkTextPrimary", "_dkBg", 4.5 },
            { "_dkTextSecondary", "_dkBg", 4.5 },
            { "_dkTextMuted", "_dkBg", 4.5 },
            { "_dkTextMuted", "_dkCard", 4.5 },
            { "_dkSelectedText", "_dkSelected", 4.5 },
            { "_dkSelectedText", "_dkSelectedHover", 4.5 },
            { "_dkTextPrimary", "_dkCardElevated", 4.5 },
            // Warm.
            { "_warTextPrimary", "_warBg", 4.5 },
            { "_warTextSecondary", "_warBg", 4.5 },
            { "_warTextMuted", "_warBg", 4.5 },
            { "_warTextMuted", "_warCard", 4.5 },
            { "_warSelectedText", "_warSelected", 4.5 },
            { "_warSelectedText", "_warSelectedHover", 4.5 },
            { "_warTextPrimary", "_warHover", 4.5 },
            { "ownBubbleText", "_warOwnBubble", 4.5 },
            { "onAccentMuted", "_warOwnBubble", 4.5 },
            { "_onAccent", "_warAccent", 3.0 },
            // Graphite / Nordic / Purple Dusk core readability.
            { "_graTextPrimary", "_graBg", 4.5 },
            { "_graTextMuted", "_graBg", 4.5 },
            { "_graTextMuted", "_graCard", 4.5 },
            { "_graSelectedText", "_graSelected", 4.5 },
            { "_norTextPrimary", "_norBg", 4.5 },
            { "_norTextMuted", "_norBg", 4.5 },
            { "_norTextMuted", "_norCard", 4.5 },
            { "_norSelectedText", "_norSelected", 4.5 },
            { "_purTextPrimary", "_purBg", 4.5 },
            { "_purTextMuted", "_purBg", 4.5 },
            { "_purTextMuted", "_purCard", 4.5 },
            { "_purSelectedText", "_purSelected", 4.5 },
            // Own-bubble body text stays readable in every preset.
            { "ownBubbleText", "_graOwnBubble", 4.5 },
            { "ownBubbleText", "_norOwnBubble", 4.5 },
            { "ownBubbleText", "_purOwnBubble", 4.5 },
            // Moss Light (design handoff).
            { "_mosTextPrimary", "_mosBg", 4.5 },
            { "_mosTextSecondary", "_mosBg", 4.5 },
            { "_mosTextMuted", "_mosBg", 4.5 },
            { "_mosTextMuted", "_mosSidebar", 4.5 },
            { "_mosTextMuted", "_mosCard", 4.5 },
            { "_mosSelectedText", "_mosSelected", 4.5 },
            { "_mosSelectedText", "_mosSelectedHover", 4.5 },
            { "_mosTextPrimary", "_mosHover", 4.5 },
            { "ownBubbleText", "_mosOwnBubble", 4.5 },
            { "onAccentMuted", "_mosOwnBubble", 4.5 },
            { "_onAccent", "_mosAccent", 3.0 },
            // Indigo Night (design handoff).
            { "_indTextPrimary", "_indBg", 4.5 },
            { "_indTextSecondary", "_indBg", 4.5 },
            { "_indTextMuted", "_indBg", 4.5 },
            { "_indTextMuted", "_indSidebar", 4.5 },
            { "_indTextMuted", "_indCard", 4.5 },
            { "_indSelectedText", "_indSelected", 4.5 },
            { "_indSelectedText", "_indSelectedHover", 4.5 },
            { "_indTextPrimary", "_indCardElevated", 4.5 },
            { "ownBubbleText", "_indOwnBubble", 4.5 },
            { "onAccentMuted", "_indOwnBubble", 4.5 },
            { "_onAccent", "_indAccent", 3.0 },
            // Deep Teal (design handoff; accent carries its own dark ink).
            { "_teaTextPrimary", "_teaBg", 4.5 },
            { "_teaTextSecondary", "_teaBg", 4.5 },
            { "_teaTextMuted", "_teaBg", 4.5 },
            { "_teaTextMuted", "_teaSidebar", 4.5 },
            { "_teaTextMuted", "_teaCard", 4.5 },
            { "_teaSelectedText", "_teaSelected", 4.5 },
            { "_teaSelectedText", "_teaSelectedHover", 4.5 },
            { "_teaTextPrimary", "_teaCardElevated", 4.5 },
            { "ownBubbleText", "_teaOwnBubble", 4.5 },
            { "onAccentMuted", "_teaOwnBubble", 4.5 },
            { "_teaAccentText", "_teaAccent", 4.5 },
            // Primary ink on hover fills — the menu language brightens a
            // hovered row's ink to stormText, which routes to textPrimary
            // under legacy themes; the pairs the file did not already
            // carry are asserted here (review LOW2).
            { "_textPrimaryDark", "_hoverDark", 4.5 },
            { "_dkTextPrimary", "_dkHover", 4.5 },
            { "_graTextPrimary", "_graHover", 4.5 },
            { "_norTextPrimary", "_norHover", 4.5 },
            { "_purTextPrimary", "_purHover", 4.5 },
            { "_indTextPrimary", "_indHover", 4.5 },
            { "_teaTextPrimary", "_teaHover", 4.5 },
            // Muted ink on input fills — the storm* routing exposes this
            // pairing (search fields, category chips, omnibox) to every
            // legacy theme for the first time, so it is asserted per theme.
            { "_textMutedLight", "_inputBgLight", 4.5 },
            { "_textMutedDark", "_inputBgDark", 4.5 },
            { "_dkTextMuted", "_dkInputBg", 4.5 },
            { "_graTextMuted", "_graInputBg", 4.5 },
            { "_norTextMuted", "_norInputBg", 4.5 },
            { "_purTextMuted", "_purInputBg", 4.5 },
            { "_warTextMuted", "_warInputBg", 4.5 },
            { "_mosTextMuted", "_mosInputBg", 4.5 },
            { "_indTextMuted", "_indInputBg", 4.5 },
            { "_teaTextMuted", "_teaInputBg", 4.5 },
            // v0.6.5 trust-card brand constants (SPEC 1r) — theme-invariant,
            // so their readability is asserted once, here.
            { "trustInk", "trustNavy", 4.5 },
            { "trustYellow", "trustNavy", 4.5 },
            { "trustMuted", "trustNavy", 4.5 },
            { "trustMuted", "trustChainBg", 4.5 },
            { "trustCaption", "trustChainBg", 4.5 },
            { "trustCaptionDim", "trustChainBg", 4.5 },
            { "trustNavy", "trustYellow", 4.5 },
            { "trustVerifyInk", "trustNavy", 4.5 },
            // Storm (selectable theme 11 + the trust card's fixed palette).
            // The storm* tokens themselves are theme-ROUTED expressions now,
            // so the assertions read the raw _sto* literals they resolve to
            // under Storm. _stoTextFaint is deliberately dim decorative-scale
            // mono (section headers, metadata) and is exempt, like
            // trustPending before it.
            //   Menu/panel inks on their real fills:
            { "_stoText", "_stoPanel", 4.5 },
            { "_stoTextSecondary", "_stoPanel", 4.5 },
            { "_stoTextMuted", "_stoPanel", 4.5 },
            { "_stoText", "_stoSelection", 4.5 },
            { "_stoTextMuted", "_stoInset", 4.5 },
            { "_stoBolt", "_stoPanel", 4.5 },
            { "_stoCanvas", "_stoBolt", 4.5 },   // boltInk on a bolt fill
            { "_stoDanger", "_stoPanel", 4.5 },
            { "_stoSuccess", "_stoPanel", 4.5 },
            { "_stoLink", "_stoPanel", 4.5 },
            //   Full-app shell readability (theme 11):
            { "_stoText", "_stoDeep", 4.5 },
            { "_stoTextSecondary", "_stoDeep", 4.5 },
            { "_stoTextMuted", "_stoDeep", 4.5 },
            { "_stoText", "_stoCanvas", 4.5 },
            { "_stoTextMuted", "_stoCanvas", 4.5 },
            { "_stoText", "_stoSelectedHover", 4.5 }, // selection ink on hover
            { "_stoCanvas", "_stoLink", 4.5 },     // badge ink on unread pill
            { "ownBubbleText", "_stoOwnBubble", 4.5 },
            { "onAccentMuted", "_stoOwnBubble", 4.5 },
        };
        for (const Pair &pair : pairs) {
            const QString fg = c(pair.fg);
            const QString bg = c(pair.bg);
            QVERIFY2(!fg.isEmpty() && !bg.isEmpty(),
                     qPrintable(QStringLiteral("missing palette value: %1 / %2")
                                    .arg(QLatin1String(pair.fg),
                                         QLatin1String(pair.bg))));
            const double ratio = contrast(fg, bg);
            QVERIFY2(ratio >= pair.minimum,
                     qPrintable(QStringLiteral("%1 on %2 = %3 (< %4)")
                                    .arg(QLatin1String(pair.fg),
                                         QLatin1String(pair.bg))
                                    .arg(ratio, 0, 'f', 2)
                                    .arg(pair.minimum)));
        }
    }

    void mentionWashKeepsBodyTextReadable()
    {
        // Review M1's lesson encoded: the mention-row wash derives from
        // mentionHighlight over the timeline background, and one theme's
        // base hue behaving differently from the other ten went uncaught.
        // mentionHighlight resolves to accent for the legacy palettes and
        // to the Storm mention rose for theme 11 — assert body text stays
        // AA over the composited wash for every theme, computed, not
        // guessed. Computed at 0.14 alpha: an UPPER bound above the live
        // washes (0.05 mentionsMe / 0.03 room since the 2026-07-31 live-
        // feedback round), so any retune up to 0.14 stays covered.
        const QRegularExpression routed(QStringLiteral(
            "mentionHighlight:\\s*_p\\.mentionHighlight\\s*!==\\s*undefined"));
        QVERIFY2(m_theme.contains(routed),
                 "mentionHighlight must use the _p override idiom");
        // Lock the HUE decision, not only readability: M1's defect was
        // never a contrast failure (white over the brown wash measured
        // 13.4:1) — it was bolt landing on a passive row. Storm's base
        // must stay the mention rose, never the bolt.
        const QRegularExpression stormBase(QStringLiteral(
            "mentionHighlight:\\s*_stoMention\\b"));
        QVERIFY2(m_theme.contains(stormBase),
                 "_storm must pin mentionHighlight to _stoMention "
                 "(yellow-discipline: no bolt on a passive row)");
        struct Wash { const char *ink; const char *base; const char *bg; };
        const Wash washes[] = {
            { "_textPrimaryLight", "_accentBlue", "_bgLight" },
            { "_textPrimaryDark", "_accentBlue", "_bgDark" },
            { "_dkTextPrimary", "_accentBlue", "_dkBg" },
            { "_graTextPrimary", "_graAccent", "_graBg" },
            { "_norTextPrimary", "_norAccent", "_norBg" },
            { "_purTextPrimary", "_purAccent", "_purBg" },
            { "_warTextPrimary", "_warAccent", "_warBg" },
            { "_mosTextPrimary", "_mosAccent", "_mosBg" },
            { "_indTextPrimary", "_indAccent", "_indBg" },
            { "_teaTextPrimary", "_teaAccent", "_teaBg" },
            // Storm routes the base to the mention rose, not the bolt.
            { "_stoText", "_stoMention", "_stoDeep" },
        };
        for (const Wash &w : washes) {
            const QString ink = m_colors.value(QLatin1String(w.ink));
            const QString base = m_colors.value(QLatin1String(w.base));
            const QString bg = m_colors.value(QLatin1String(w.bg));
            QVERIFY2(!ink.isEmpty() && !base.isEmpty() && !bg.isEmpty(),
                     qPrintable(QStringLiteral("missing wash value: %1/%2/%3")
                                    .arg(QLatin1String(w.ink),
                                         QLatin1String(w.base),
                                         QLatin1String(w.bg))));
            const QString washed = composite(base, 0.14, bg);
            const double ratio = contrast(ink, washed);
            QVERIFY2(ratio >= 4.5,
                     qPrintable(QStringLiteral("%1 over 14%% %2 wash on %3 "
                                               "= %4 (< 4.5)")
                                    .arg(QLatin1String(w.ink),
                                         QLatin1String(w.base),
                                         QLatin1String(w.bg))
                                    .arg(ratio, 0, 'f', 2)));
        }
    }

    void settingsScreenCarriesNoThemedInk()
    {
        // Storm namespace guard (review H1, retargeted for the selectable
        // Storm theme): SettingsScreen speaks ONLY the storm* vocabulary.
        // The storm* tokens are theme-routed inside AppTheme, so Settings
        // follows the selected theme — but a general themed token mixed
        // onto a storm-token fill would pair inks and surfaces from two
        // different routing tables, which is exactly the class of invisible-
        // ink bug this guard caught in review. The theme-preview cards use
        // their own FIXED hex palettes (not tokens), so they cannot trip
        // this scan.
        // NOTE the double escaping: "\\." and "\\b" — a single backslash in
        // a C++ literal would put a literal dot-wildcard and a BACKSPACE
        // byte in the pattern, and the guard would pass forever (caught in
        // review: the first version of this test was exactly that no-op).
        const QString settings = readAll(QStringLiteral(SETTINGS_QML_PATH));
        QVERIFY2(!settings.isEmpty(), "SettingsScreen.qml not readable");
        const QStringList banned = {
            QStringLiteral("AppTheme\\.text\\b"),
            QStringLiteral("AppTheme\\.textPrimary\\b"),
            QStringLiteral("AppTheme\\.textSecondary\\b"),
            QStringLiteral("AppTheme\\.textMuted\\b"),
            QStringLiteral("AppTheme\\.textDisabled\\b"),
            QStringLiteral("AppTheme\\.card\\b"),
            QStringLiteral("AppTheme\\.cardElevated\\b"),
            QStringLiteral("AppTheme\\.surface\\b"),
            QStringLiteral("AppTheme\\.surfaceAlt\\b"),
            QStringLiteral("AppTheme\\.background\\b"),
            QStringLiteral("AppTheme\\.sidebar\\b"),
            QStringLiteral("AppTheme\\.hover\\b"),
            QStringLiteral("AppTheme\\.accent\\b"),
            QStringLiteral("AppTheme\\.accentSoft\\b"),
            QStringLiteral("AppTheme\\.accentText\\b"),
            QStringLiteral("AppTheme\\.selectedText\\b"),
            QStringLiteral("AppTheme\\.separator\\b"),
            QStringLiteral("AppTheme\\.inputBackground\\b"),
            QStringLiteral("AppTheme\\.warning\\b"),
            QStringLiteral("AppTheme\\.danger\\b"),
            QStringLiteral("AppTheme\\.success\\b"),
            QStringLiteral("AppTheme\\.focusRing\\b"),
            QStringLiteral("AppTheme\\.icon\\b"),
        };
        // Positive control: the SAME regex list must bite on a file that
        // legitimately uses themed ink (the room list keeps the user theme
        // per SPEC-storm-language §5). If this stops matching, the guard
        // has gone inert — fail loudly instead of passing forever.
        const QString themedControl =
            readAll(QStringLiteral(QML_DIR "/RoomDelegate.qml"));
        QVERIFY2(!themedControl.isEmpty(), "RoomDelegate.qml not readable");
        bool controlHit = false;
        for (const QString &pattern : banned) {
            const QRegularExpression re(pattern);
            QVERIFY2(re.isValid(), qPrintable(pattern));
            if (themedControl.contains(re))
                controlHit = true;
            QVERIFY2(!settings.contains(re),
                     qPrintable(QStringLiteral(
                         "themed token on the storm Settings surface: %1")
                                    .arg(pattern)));
        }
        QVERIFY2(controlHit,
                 "positive control failed: the banned-token regexes no "
                 "longer match RoomDelegate.qml's themed ink — the guard "
                 "has gone inert");
    }

    void allPresetsDefineFullRoleSet()
    {
        // Every registered theme preset must supply the complete palette
        // object, and the effective-theme switch must route every valid
        // SettingsManager::Theme id (1..11) to one of them.
        const QStringList presets = {
            QStringLiteral("_light"), QStringLiteral("_dark"),
            QStringLiteral("_midnight"), QStringLiteral("_graphite"),
            QStringLiteral("_nord"), QStringLiteral("_purple"),
            QStringLiteral("_warm"), QStringLiteral("_moss"),
            QStringLiteral("_indigo"), QStringLiteral("_teal"),
            QStringLiteral("_storm"),
        };
        const QStringList roles = {
            QStringLiteral("background"), QStringLiteral("sidebar"),
            QStringLiteral("surface"), QStringLiteral("cardElevated"),
            QStringLiteral("hover"), QStringLiteral("selected"),
            QStringLiteral("selectedHover"), QStringLiteral("selectedText"),
            QStringLiteral("inputBg"), QStringLiteral("codeBlock"),
            QStringLiteral("textPrimary"), QStringLiteral("textSecondary"),
            QStringLiteral("textMuted"), QStringLiteral("textDisabled"),
            QStringLiteral("border"), QStringLiteral("borderStrong"),
            QStringLiteral("accent"), QStringLiteral("accentHover"),
            QStringLiteral("accentPressed"), QStringLiteral("ownBubble"),
            QStringLiteral("otherBubble"),
        };
        for (const QString &preset : presets) {
            const QRegularExpression block(
                QStringLiteral("property\\s+var\\s+%1\\s*:\\s*\\(\\{(.*?)\\}\\)")
                    .arg(preset),
                QRegularExpression::DotMatchesEverythingOption);
            const auto match = block.match(m_theme);
            QVERIFY2(match.hasMatch(),
                     qPrintable(QStringLiteral("missing preset: %1").arg(preset)));
            const QString body = match.captured(1);
            for (const QString &role : roles) {
                const QRegularExpression key(
                    QStringLiteral("\\b%1\\s*:").arg(role));
                QVERIFY2(body.contains(key),
                         qPrintable(QStringLiteral("%1 lacks role %2")
                                        .arg(preset, role)));
            }
        }
        for (int id = 1; id <= 11; ++id) {
            const QRegularExpression routed(
                QStringLiteral("case\\s+%1\\s*:\\s*return\\s+_").arg(id));
            QVERIFY2(m_theme.contains(routed),
                     qPrintable(QStringLiteral("theme id %1 not routed").arg(id)));
            // paletteForTheme() has its own switch (`case N: p = _x; break`)
            // feeding the Settings preview cards. Its default branch returns
            // the ACTIVE palette, so a missing case paints a wrong-but-
            // plausible preview no runtime suite would catch.
            const QRegularExpression preview(
                QStringLiteral("case\\s+%1\\s*:\\s*p\\s*=\\s*_").arg(id));
            QVERIFY2(m_theme.contains(preview),
                     qPrintable(QStringLiteral(
                         "theme id %1 missing from paletteForTheme").arg(id)));
        }
    }

    void lightThemeIsNotInvertedDark()
    {
        // The old light theme reused the dark theme's muted grey, which fell
        // to 2.2:1. The palettes must stay distinct.
        QVERIFY(m_colors.value(QStringLiteral("_textMutedLight"))
                != m_colors.value(QStringLiteral("_textMutedDark")));
        QVERIFY(m_colors.value(QStringLiteral("_bgLight"))
                != m_colors.value(QStringLiteral("_bgDark")));
    }

    void coreViewsUseTokensNotHex()
    {
        // View QML must not scatter its own hex values; deliberate
        // exceptions: AppTheme.qml (the palette itself) and the image
        // viewer's committed-dark overlay chrome.
        const QStringList files = {
            QStringLiteral(QML_DIR "/RoomDelegate.qml"),
            QStringLiteral(QML_DIR "/MessageDelegate.qml"),
            QStringLiteral(QML_DIR "/RoomsPanel.qml"),
            QStringLiteral(QML_DIR "/TimelinePane.qml"),
            QStringLiteral(QML_DIR "/MessageComposerBar.qml"),
            QStringLiteral(QML_DIR "/SettingsScreen.qml"),
            QStringLiteral(QML_DIR "/AccountMenu.qml"),
            QStringLiteral(QML_DIR "/RoomInfoPanel.qml"),
            QStringLiteral(QML_DIR "/UserPicker.qml"),
            QStringLiteral(QML_DIR "/NewConversationDialog.qml"),
            QStringLiteral(QML_DIR "/InvitePeopleDialog.qml"),
        };
        const QRegularExpression hexColor(
            QStringLiteral("color\\s*:\\s*\"#[0-9A-Fa-f]{3,8}\""));
        const QRegularExpression rgba(QStringLiteral("Qt\\.rgba\\("));
        // The ONE sanctioned hex exception (correction spec §3): the
        // Settings theme-preview cards always paint their own theme's
        // fixed palette, and the switch/slider thumbs are the spec's
        // white circle with its permitted shadow tint.
        const QStringList allowedSettingsLiterals = {
            QStringLiteral("#f7f7f5"), QStringLiteral("#eceded"),
            QStringLiteral("#dcdedc"), QStringLiteral("#e6e8e6"),
            QStringLiteral("#12a67f"), QStringLiteral("#101016"),
            QStringLiteral("#1d1d26"), QStringLiteral("#2a2a36"),
            QStringLiteral("#23232d"), QStringLiteral("#7c7ff2"),
            QStringLiteral("#0e1416"), QStringLiteral("#182428"),
            QStringLiteral("#1d2b30"), QStringLiteral("#152023"),
            QStringLiteral("#27c2ad"), QStringLiteral("#FFFFFF"),
            QStringLiteral("#40000000"),
        };
        for (const QString &path : files) {
            QString content = readAll(path);
            QVERIFY2(!content.isEmpty(), qPrintable(path));
            if (path.endsWith(QLatin1String("SettingsScreen.qml"))) {
                for (const QString &allowed : allowedSettingsLiterals)
                    content.replace(
                        QStringLiteral("\"%1\"").arg(allowed),
                        QStringLiteral("AppTheme.background"));
            }
            QVERIFY2(!content.contains(hexColor),
                     qPrintable(QStringLiteral("hardcoded hex colour in %1")
                                    .arg(path)));
            QVERIFY2(!content.contains(rgba),
                     qPrintable(QStringLiteral("Qt.rgba literal in %1")
                                    .arg(path)));
        }
    }
};

QTEST_GUILESS_MAIN(ThemeTokensTest)
#include "ThemeTokensTest.moc"
