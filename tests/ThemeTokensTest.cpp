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
            // Storm menu language (SPEC-storm-language §1) — the
            // theme-invariant menu inks on their real fills. stormTextFaint
            // is deliberately dim decorative-scale mono (section headers,
            // metadata) and is exempt, like trustPending before it.
            { "stormText", "stormPanel", 4.5 },
            { "stormTextSecondary", "stormPanel", 4.5 },
            { "stormTextMuted", "stormPanel", 4.5 },
            { "stormText", "stormSelection", 4.5 },
            { "stormTextMuted", "stormInset", 4.5 },
            { "bolt", "stormPanel", 4.5 },
            { "stormPanel", "bolt", 4.5 },
            { "stormDanger", "stormPanel", 4.5 },
            { "stormSuccess", "stormPanel", 4.5 },
            { "stormLink", "stormPanel", 4.5 },
            { "stormText", "stormCanvas", 4.5 },
            { "stormText", "stormDeep", 4.5 },
            { "stormTextMuted", "stormCanvas", 4.5 },
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

    void settingsScreenCarriesNoThemedInk()
    {
        // Storm regression guard (review H1): SettingsScreen is a storm
        // surface end to end — a themed colour token painted onto its navy
        // fills is invisible in every light theme, and the round's dark
        // captures cannot see it. The theme-preview cards use their own
        // FIXED hex palettes (not tokens), so they cannot trip this scan.
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
        // SettingsManager::Theme id (1..7) to one of them.
        const QStringList presets = {
            QStringLiteral("_light"), QStringLiteral("_dark"),
            QStringLiteral("_midnight"), QStringLiteral("_graphite"),
            QStringLiteral("_nord"), QStringLiteral("_purple"),
            QStringLiteral("_warm"), QStringLiteral("_moss"),
            QStringLiteral("_indigo"), QStringLiteral("_teal"),
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
        for (int id = 1; id <= 10; ++id) {
            const QRegularExpression routed(
                QStringLiteral("case\\s+%1\\s*:\\s*return\\s+_").arg(id));
            QVERIFY2(m_theme.contains(routed),
                     qPrintable(QStringLiteral("theme id %1 not routed").arg(id)));
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
