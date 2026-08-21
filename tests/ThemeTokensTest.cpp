// v0.5.9: theme-token and contrast tests. Parses qml/AppTheme.qml as text
// (no QML runtime needed): asserts the required semantic tokens exist,
// computes WCAG 2.1 contrast for the critical colour pairs in both themes,
// and verifies core view QML files contain no stray hex colours outside
// the AppTheme singleton.

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QtTest/QtTest>

#include <array>
#include <cmath>
#include <functional>

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

// CIE76 colour difference in Lab. Contrast answers "is this legible on that
// background"; it says nothing about whether two INKS are telling each other
// apart, and identity colouring needs exactly that. dE below ~10 reads as the
// same colour to a viewer.
double deltaE(const QString &a, const QString &b)
{
    auto toLab = [](const QString &hex) {
        auto ch = [&hex](int i) {
            return channelLinear(hex.mid(1 + i * 2, 2).toInt(nullptr, 16)
                                 / 255.0);
        };
        const double r = ch(0), g = ch(1), bl = ch(2);
        const double X = 0.4124 * r + 0.3576 * g + 0.1805 * bl;
        const double Y = 0.2126 * r + 0.7152 * g + 0.0722 * bl;
        const double Z = 0.0193 * r + 0.1192 * g + 0.9505 * bl;
        auto f = [](double t) {
            return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0;
        };
        const double fx = f(X / 0.95047), fy = f(Y), fz = f(Z / 1.08883);
        return std::array<double, 3>{ 116.0 * fy - 16.0, 500.0 * (fx - fy),
                                      200.0 * (fy - fz) };
    };
    const auto la = toLab(a);
    const auto lb = toLab(b);
    return std::sqrt(std::pow(la[0] - lb[0], 2) + std::pow(la[1] - lb[1], 2)
                     + std::pow(la[2] - lb[2], 2));
}

// QML comments removed, so a "does this file USE token X" scan is not
// defeated — or falsely tripped — by prose. Line comments and block comments
// only; QML has no other comment form. String contents are left alone, which
// is safe here because every caller is looking for a bare token reference.
QString stripComments(const QString &qml)
{
    QString out = qml;
    out.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                  QRegularExpression::DotMatchesEverythingOption));
    out.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
    return out;
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
            // Status FILLS are a different role from status INK — the 40
            // ink call sites and the destructive buttons had been sharing
            // one #DC2626 that was below AA as ink on every Storm surface.
            QStringLiteral("dangerFill"), QStringLiteral("dangerFillHover"),
            QStringLiteral("dangerFillPressed"), QStringLiteral("dangerText"),
            QStringLiteral("successFill"), QStringLiteral("warningFill"),
            QStringLiteral("infoFill"),
            // Messaging
            QStringLiteral("incomingBubble"), QStringLiteral("outgoingBubble"),
            QStringLiteral("codeBlock"), QStringLiteral("reactionBackground"),
            QStringLiteral("reactionBorder"), QStringLiteral("reactionInk"),
            QStringLiteral("reactionSelectedBackground"),
            QStringLiteral("reactionSelectedBorder"),
            QStringLiteral("unreadBadge"), QStringLiteral("mentionBadge"),
            QStringLiteral("mentionChipFill"), QStringLiteral("mentionChipInk"),
            // Presence
            QStringLiteral("presenceOnline"), QStringLiteral("presenceAway"),
            QStringLiteral("presenceOffline"),
            // Controls — every button state is a token, because the four
            // call sites that computed their own with Qt.darker() produced
            // a different result on each of the eleven palettes.
            QStringLiteral("buttonPrimaryFill"),
            QStringLiteral("buttonPrimaryHover"),
            QStringLiteral("buttonPrimaryPressed"),
            QStringLiteral("buttonPrimaryInk"),
            QStringLiteral("buttonNeutralFill"),
            QStringLiteral("buttonNeutralHover"),
            QStringLiteral("buttonNeutralPressed"),
            QStringLiteral("buttonNeutralInk"),
            QStringLiteral("buttonNeutralBorder"),
            QStringLiteral("buttonGhostHover"),
            QStringLiteral("buttonGhostPressed"),
            QStringLiteral("buttonDangerFill"),
            QStringLiteral("buttonDangerHover"),
            QStringLiteral("buttonDangerPressed"),
            QStringLiteral("buttonDisabledFill"),
            QStringLiteral("buttonDisabledInk"),
            // Chips — six families so ACTIVE, MOD and VERIFIED stop being
            // the same pill.
            QStringLiteral("chipNeutralInk"), QStringLiteral("chipNeutralFill"),
            QStringLiteral("chipAccentInk"), QStringLiteral("chipAccentFill"),
            QStringLiteral("chipSuccessInk"), QStringLiteral("chipSuccessFill"),
            QStringLiteral("chipWarningInk"), QStringLiteral("chipWarningFill"),
            QStringLiteral("chipDangerInk"), QStringLiteral("chipDangerFill"),
            QStringLiteral("chipInfoInk"), QStringLiteral("chipInfoFill"),
            QStringLiteral("chipBoltFill"), QStringLiteral("chipBoltInk"),
            // Scrollbars
            QStringLiteral("scrollbarTrack"), QStringLiteral("scrollbarHandle"),
            QStringLiteral("scrollbarHandleHover"),
            QStringLiteral("scrollbarHandlePressed"),
            // Elevation
            QStringLiteral("shadow"), QStringLiteral("shadowSoft"),
            QStringLiteral("shadowStrong"),
            // Committed-dark media chrome (theme-invariant by design)
            QStringLiteral("scrimBackdrop"), QStringLiteral("scrimSurface"),
            QStringLiteral("scrimSurfaceRaised"),
            QStringLiteral("scrimSurfaceHover"), QStringLiteral("scrimBorder"),
            QStringLiteral("scrimInk"), QStringLiteral("scrimInkStrong"),
            QStringLiteral("scrimInkMuted"),
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
        // Semantic typography roles. The first six are THE scale (2026-08-21);
        // the rest are the pre-scale names, kept because 78 files consume them.
        const QStringList type = {
            QStringLiteral("textDisplay"), QStringLiteral("textTitle"),
            QStringLiteral("textSubtitle"), QStringLiteral("textBody"),
            QStringLiteral("textMeta"), QStringLiteral("textMicro"),
            QStringLiteral("weightBody"), QStringLiteral("weightMedium"),
            QStringLiteral("weightStrong"), QStringLiteral("weightBold"),
            QStringLiteral("weightDisplay"),
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
        // Leading. The message body sets no lineHeight, so the UI-font
        // picker silently changes chat leading by 13% (Manrope 1.366em vs
        // Inter 1.210em). The tokens must exist before a consumer can fix it.
        for (const QString &token : { QStringLiteral("lineHeightBody"),
                                      QStringLiteral("lineHeightTight"),
                                      QStringLiteral("lineHeightDisplay") }) {
            const QRegularExpression decl(
                QStringLiteral("property\\s+real\\s+%1\\b").arg(token));
            QVERIFY2(m_theme.contains(decl),
                     qPrintable(QStringLiteral("missing leading token: %1")
                                    .arg(token)));
        }
        // Font families are tokens too — Icon.qml hard-coded the icon face.
        for (const QString &token : { QStringLiteral("uiFont"),
                                      QStringLiteral("monoFont"),
                                      QStringLiteral("iconFont"),
                                      QStringLiteral("displayFont"),
                                      QStringLiteral("menuFont"),
                                      QStringLiteral("menuSectionFont") }) {
            const QRegularExpression decl(
                QStringLiteral("property\\s+string\\s+%1\\b").arg(token));
            QVERIFY2(m_theme.contains(decl),
                     qPrintable(QStringLiteral("missing family token: %1")
                                    .arg(token)));
        }
    }

    // The scale is only a scale if it is small, ordered, and made of the
    // values it claims. 15 rendered sizes reached through 24 names is what
    // this exists to prevent coming back.
    void theTypeScaleIsSmallAndOrdered()
    {
        // Resolves one level of alias, because the retained pre-scale names
        // are declared as aliases ONTO the scale (`fontPageTitle:
        // textDisplay`) — that is the point of them.
        std::function<int(const char *, int)> intToken =
            [this, &intToken](const char *name, int depth) -> int {
            if (depth > 2)
                return -1;
            const QRegularExpression re(
                QStringLiteral("property\\s+int\\s+%1\\s*:\\s*(\\w+)")
                    .arg(QLatin1String(name)));
            const auto m = re.match(m_theme);
            if (!m.hasMatch())
                return -1;
            const QString value = m.captured(1);
            bool numeric = false;
            const int direct = value.toInt(&numeric);
            return numeric ? direct
                           : intToken(value.toUtf8().constData(), depth + 1);
        };
        const int display = intToken("textDisplay", 0);
        const int title = intToken("textTitle", 0);
        const int subtitle = intToken("textSubtitle", 0);
        const int body = intToken("textBody", 0);
        const int meta = intToken("textMeta", 0);
        const int micro = intToken("textMicro", 0);
        for (int v : { display, title, subtitle, body, meta, micro })
            QVERIFY2(v > 0, "a scale token is missing or is not a literal int");
        QVERIFY2(display > title, "textDisplay must be larger than textTitle");
        QVERIFY2(title > subtitle, "textTitle must be larger than textSubtitle");
        QVERIFY2(subtitle >= body, "textSubtitle must not be smaller than body");
        QVERIFY2(body > meta, "textBody must be larger than textMeta");
        QVERIFY2(meta > micro, "textMeta must be larger than textMicro");
        // Five distinct numbers, not six: subtitle and body deliberately
        // share a size and differ by WEIGHT.
        QSet<int> distinct{ display, title, subtitle, body, meta, micro };
        QVERIFY2(distinct.size() <= 5,
                 qPrintable(QStringLiteral("the scale has %1 distinct sizes; "
                                           "five is the budget")
                                .arg(distinct.size())));
        QCOMPARE(subtitle, body);
        // Weights: exactly the five named steps, ascending.
        const int w[] = { intToken("weightBody", 0), intToken("weightMedium", 0),
                          intToken("weightStrong", 0), intToken("weightBold", 0),
                          intToken("weightDisplay", 0) };
        for (int i = 1; i < 5; ++i)
            QVERIFY2(w[i] > w[i - 1], "weights must ascend");
        QCOMPARE(w[0], 400);
        QCOMPARE(w[4], 800);
        // fontPageTitle was declared at 24 and used NOWHERE while three
        // ad-hoc display sizes (22/26/28) existed as literals. It is
        // required by this suite, so it gets the real display role.
        QCOMPARE(intToken("fontPageTitle", 0), display);
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
            // SELECTED SEGMENTED-CONTROL CHIP, every theme.
            //
            // The chip's fill is accentSoft -- a TINT of the surface, not a
            // solid accent -- so its ink must be a surface ink. Pairing it
            // with accentText (the ink for a SOLID accent fill, which is
            // white wherever a theme does not override it) was invisible:
            // Deep Teal measured 1.00 (#062A25 on #112928), Moss Light 1.14
            // and Lightning Light 1.42. Only Deep Teal was reported, because
            // that is the theme the reporter uses; the two light themes were
            // just as broken and unnoticed.
            //
            // Every theme is listed here deliberately. A theme whose
            // accentSoft is not a literal falls back to `selected`, which the
            // selected-row rows below already cover.
            { "_selectedTextLight", "_selectedLight", 4.5 },   // chip == selected
            { "_dkSelectedText", "_dkSelected", 4.5 },         // chip == selected
            { "_mosSelectedText", "_mosAccentSoft", 4.5 },
            { "_indSelectedText", "_indAccentSoft", 4.5 },
            { "_teaSelectedText", "_teaAccentSoft", 4.5 },
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
            // Danger buttons: white label on danger red, in all three states.
            { "dangerText", "_accentDanger", 4.5 },
            { "dangerText", "_dangerFillHover", 4.5 },
            { "dangerText", "_dangerFillPressed", 4.5 },
            // Reaction pills: each theme's own surface, carrying that
            // theme's secondary ink (the count) and primary ink (the emoji
            // fallback glyph). reactionBackground used to be an alias of
            // cardElevated, so no pair could exist at all.
            { "_textSecondaryLight", "_lightReaction", 4.5 },
            { "_textPrimaryLight", "_lightReaction", 4.5 },
            { "_dkTextSecondary", "_dkReaction", 4.5 },
            { "_dkTextPrimary", "_dkReaction", 4.5 },
            { "_graTextSecondary", "_graReaction", 4.5 },
            { "_graTextPrimary", "_graReaction", 4.5 },
            { "_textSecondaryDark", "_midReaction", 4.5 },
            { "_textPrimaryDark", "_midReaction", 4.5 },
            { "_norTextSecondary", "_norReaction", 4.5 },
            { "_norTextPrimary", "_norReaction", 4.5 },
            { "_purTextSecondary", "_purReaction", 4.5 },
            { "_purTextPrimary", "_purReaction", 4.5 },
            { "_warTextSecondary", "_warReaction", 4.5 },
            { "_warTextPrimary", "_warReaction", 4.5 },
            { "_mosTextSecondary", "_mosReaction", 4.5 },
            { "_mosTextPrimary", "_mosReaction", 4.5 },
            { "_indTextSecondary", "_indReaction", 4.5 },
            { "_indTextPrimary", "_indReaction", 4.5 },
            { "_teaTextSecondary", "_teaReaction", 4.5 },
            { "_teaTextPrimary", "_teaReaction", 4.5 },
            // Committed-dark media chrome. scrimSurface is an 8-digit ARGB
            // that the contrast maths cannot read, so its OPAQUE equivalent
            // _scrimBase stands in — which is what the bar composites to
            // over dark media anyway.
            { "scrimInk", "_scrimBase", 4.5 },
            { "scrimInkMuted", "_scrimBase", 4.5 },
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
            // MOVED 2026-08-21, not deleted: boltInk was `_stoCanvas`, so
            // the room-list surface doubled as the ink on every bolt and
            // unread pill and the ladder could not lift the sidebar without
            // darkening every pill label. The ink is now its own literal
            // and the pair moves with it — same assertion, honest operand.
            { "_stoBoltInk", "_stoBolt", 4.5 },  // boltInk on a bolt fill
            { "_stoDanger", "_stoPanel", 4.5 },
            { "_stoSuccess", "_stoPanel", 4.5 },
            { "_stoLink", "_stoPanel", 4.5 },
            // The new elevation rung carries chips and raised cards.
            { "_stoText", "_stoCardElevated", 4.5 },
            { "_stoTextSecondary", "_stoCardElevated", 4.5 },
            { "_stoBolt", "_stoCardElevated", 4.5 },
            { "_stoSuccess", "_stoCardElevated", 4.5 },
            // Reaction pills are their own surface on every theme now.
            { "_stoTextSecondary", "_stoReaction", 4.5 },
            { "_stoText", "_stoReaction", 4.5 },
            //   Full-app shell readability (theme 11):
            { "_stoText", "_stoDeep", 4.5 },
            { "_stoTextSecondary", "_stoDeep", 4.5 },
            { "_stoTextMuted", "_stoDeep", 4.5 },
            { "_stoText", "_stoCanvas", 4.5 },
            { "_stoTextMuted", "_stoCanvas", 4.5 },
            { "_stoText", "_stoSelectedHover", 4.5 }, // selection ink on hover
            // MOVED with boltInk, above: the unread-pill ink is _stoBoltInk.
            { "_stoBoltInk", "_stoLink", 4.5 },    // badge ink on unread pill
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

    // 2026-08-14: the per-user sender-name inks (AppTheme.userColor) are
    // `var` ARRAYS, invisible to the named-literal collection above, and
    // an authoring-time-only AA claim shipped failing on Warm's other-
    // bubble (review M6). Enforce the full matrix: every ink of a mode
    // >= 4.5:1 against every background / card / elevated-card /
    // OTHER-bubble surface its mode's themes render the identity header
    // on (Bubbles-for-DMs puts the name on the other party's bubble).
    void senderNameInksMeetContrastOnMessageSurfaces()
    {
        const auto inks = [this](const char *arrayName) -> QStringList {
            const QRegularExpression re(QStringLiteral(
                "property\\s+var\\s+%1\\s*:\\s*\\[([^\\]]*)\\]")
                .arg(QLatin1String(arrayName)));
            const auto match = re.match(m_theme);
            QStringList out;
            if (!match.hasMatch())
                return out;
            const QRegularExpression hex(
                QStringLiteral("#[0-9A-Fa-f]{6}"));
            auto it = hex.globalMatch(match.captured(1));
            while (it.hasNext())
                out.append(it.next().captured(0));
            return out;
        };
        const QStringList lightInks = inks("_nameInksLight");
        const QStringList darkInks = inks("_nameInksDark");
        QCOMPARE(lightInks.size(), 9);
        QCOMPARE(darkInks.size(), 9);

        const QStringList lightSurfaces = lightInkSurfaces();
        const QStringList darkSurfaces = darkInkSurfaces();

        const auto check = [this](const QStringList &inkList,
                                  const QStringList &surfaceNames) {
            for (const QString &surfaceName : surfaceNames) {
                const QString surface = m_colors.value(surfaceName);
                QVERIFY2(!surface.isEmpty(),
                         qPrintable(QStringLiteral("missing surface: %1")
                                        .arg(surfaceName)));
                for (const QString &ink : inkList) {
                    const double ratio = contrast(ink, surface);
                    QVERIFY2(ratio >= 4.5,
                             qPrintable(QStringLiteral(
                                 "name ink %1 on %2 (%3) = %4 (< 4.5)")
                                 .arg(ink, surfaceName, surface)
                                 .arg(ratio, 0, 'f', 2)));
                }
            }
        };
        check(lightInks, lightSurfaces);
        check(darkInks, darkSurfaces);
    }

    // Contrast alone let the palette rot: every ink cleared 4.5:1 against
    // every surface while two PAIRS of them were the same colour as each
    // other (hues 20.8/24.9 and 147.7/150.0, dE 5.6 dark and 7.4 light). The
    // palette advertised nine identities and delivered seven, and no test
    // could see it because legibility was never the failing property.
    void identityColoursAreTellableApartFromEachOther()
    {
        const auto arrayOf = [this](const char *name) -> QStringList {
            const QRegularExpression re(QStringLiteral(
                "property\\s+var\\s+%1\\s*:\\s*\\[([^\\]]*)\\]")
                .arg(QLatin1String(name)));
            const auto match = re.match(m_theme);
            QStringList out;
            if (!match.hasMatch())
                return out;
            const QRegularExpression hex(QStringLiteral("#[0-9A-Fa-f]{6}"));
            auto it = hex.globalMatch(match.captured(1));
            while (it.hasNext())
                out.append(it.next().captured(0));
            return out;
        };

        // 12 is a deliberate floor rather than the 18.1 the current palette
        // achieves: it forbids a genuine collision without freezing the exact
        // hues, so a future restyle has room to move.
        const double kMinSeparation = 12.0;
        for (const char *name :
             { "_nameInksDark", "_nameInksLight", "avatarPalette" }) {
            const QStringList inks = arrayOf(name);
            QVERIFY2(inks.size() == 9,
                     qPrintable(QStringLiteral("%1 is not 9 entries")
                                    .arg(QLatin1String(name))));
            for (int i = 0; i < inks.size(); ++i) {
                for (int j = i + 1; j < inks.size(); ++j) {
                    const double d = deltaE(inks.at(i), inks.at(j));
                    QVERIFY2(d >= kMinSeparation,
                             qPrintable(QStringLiteral(
                                 "%1[%2]=%3 and [%4]=%5 are the same colour "
                                 "(dE %6 < %7)")
                                 .arg(QLatin1String(name)).arg(i)
                                 .arg(inks.at(i)).arg(j).arg(inks.at(j))
                                 .arg(d, 0, 'f', 1)
                                 .arg(kMinSeparation, 0, 'f', 1)));
                }
            }
        }

        // The avatar disc carries WHITE initials, so its fill is the one
        // place in this palette where legibility is about the fill itself.
        for (const QString &fill : arrayOf("avatarPalette")) {
            const double ratio = contrast(fill, QStringLiteral("#FFFFFF"));
            QVERIFY2(ratio >= 4.5,
                     qPrintable(QStringLiteral(
                         "avatar fill %1 fails white initials (%2 < 4.5)")
                         .arg(fill).arg(ratio, 0, 'f', 2)));
        }

        // The notification fallback avatar re-implements the same identity
        // hash in C++ and keeps its OWN copy of this palette, because a
        // freedesktop notification is painted without a QML engine. A
        // hand-kept copy of an array is exactly the thing that drifts: the
        // palette round changed AppTheme.qml and left FallbackAvatar.cpp
        // behind for one commit, so the same person had a red disc in the app
        // and a green one in their notifications — and NotificationAvatarTest
        // asserted the stale values, so it enforced the mismatch instead of
        // catching it. Parse both and require them equal.
        {
            const QString cpp = readAll(QStringLiteral(FALLBACK_AVATAR_CPP_PATH));
            QVERIFY2(!cpp.isEmpty(), "FallbackAvatar.cpp not readable");
            const QRegularExpression arrayRe(QStringLiteral(
                "kAvatarPalette\\[\\]\\s*=\\s*\\{([^}]*)\\}"));
            const auto m = arrayRe.match(cpp);
            QVERIFY2(m.hasMatch(), "could not find kAvatarPalette");
            QStringList cppPalette;
            const QRegularExpression hex(QStringLiteral("#[0-9A-Fa-f]{6}"));
            auto it = hex.globalMatch(m.captured(1));
            while (it.hasNext())
                cppPalette.append(it.next().captured(0).toUpper());
            QStringList qmlPalette;
            for (const QString &c : arrayOf("avatarPalette"))
                qmlPalette.append(c.toUpper());
            QCOMPARE(cppPalette, qmlPalette);
        }

            // A chip paints its ink on a 14% tint OF THAT SAME INK, and its
        // border on a 32% one, so the ink's real background is not a theme
        // surface — it is a wash of itself. Tuning the inks against the plain
        // surfaces alone left the light themes at 3.83-4.04 on ~10px chip
        // labels, which get no large-text exemption. Every family, every
        // surface a chip can sit on.
        {
            const QStringList chipSurfaces = {
                QStringLiteral("_bgLight"), QStringLiteral("_cardLight"),
                QStringLiteral("_cardElevatedLight"),
                QStringLiteral("_hoverLight"),
                QStringLiteral("_warBg"), QStringLiteral("_warCard"),
                QStringLiteral("_warCardElevated"),
                QStringLiteral("_mosBg"), QStringLiteral("_mosCard"),
                QStringLiteral("_mosCardElevated"),
                QStringLiteral("_bgDark"), QStringLiteral("_cardDark"),
                QStringLiteral("_cardElevatedDark"),
                QStringLiteral("_graBg"), QStringLiteral("_norBg"),
                QStringLiteral("_purBg"), QStringLiteral("_indBg"),
                QStringLiteral("_teaBg"), QStringLiteral("_stoCanvas"),
                QStringLiteral("_stoPanel"),
            };
            const QStringList lightInkNames = {
                QStringLiteral("_dangerInkLight"), QStringLiteral("_warnInkLight"),
                QStringLiteral("_okInkLight"), QStringLiteral("_infoInkLight"),
            };
            const QStringList darkInkNames = {
                QStringLiteral("_dangerInkDark"), QStringLiteral("_warnInkDark"),
                QStringLiteral("_okInkDark"), QStringLiteral("_infoInkDark"),
            };
            for (const QString &inkName : lightInkNames + darkInkNames) {
                const QString ink = m_colors.value(inkName);
                QVERIFY2(!ink.isEmpty(),
                         qPrintable(QStringLiteral("missing %1").arg(inkName)));
                const bool lightInk = lightInkNames.contains(inkName);
                for (const QString &surfaceName : chipSurfaces) {
                    const QString surface = m_colors.value(surfaceName);
                    if (surface.isEmpty())
                        continue;
                    // A light-mode ink is only ever painted on a light
                    // surface, and vice versa.
                    const bool lightSurface =
                        luminance(surface) > 0.18;
                    if (lightInk != lightSurface)
                        continue;
                    const QString fill = composite(ink, 0.14, surface);
                    const double ratio = contrast(ink, fill);
                    QVERIFY2(ratio >= 4.5,
                             qPrintable(QStringLiteral(
                                 "chip ink %1 (%2) on its own 14%% fill over "
                                 "%3 = %4 (< 4.5)")
                                 .arg(inkName, ink, surfaceName)
                                 .arg(ratio, 0, 'f', 2)));
                }
            }
        }

    // Under Storm a sender name sits near bolt-yellow chrome. An ink that
        // close to the brand accent reads as chrome rather than as a person.
        const QRegularExpression boltRe(QStringLiteral(
            "_stoBolt\\s*:\\s*\"(#[0-9A-Fa-f]{6})\""));
        const auto boltMatch = boltRe.match(m_theme);
        QVERIFY2(boltMatch.hasMatch(), "could not read _stoBolt");
        const QString bolt = boltMatch.captured(1);
        for (const QString &ink : arrayOf("_nameInksDark")) {
            const double d = deltaE(ink, bolt);
            QVERIFY2(d >= 20.0,
                     qPrintable(QStringLiteral(
                         "name ink %1 is too close to the Storm bolt %2 "
                         "(dE %3 < 20)").arg(ink, bolt).arg(d, 0, 'f', 1)));
        }
    }

private:
    // Every surface a per-user or status INK can land on. Shared so a new
    // ink family cannot be asserted against a narrower list than the one the
    // identity inks are held to.
    static QStringList lightInkSurfaces()
    {
        return {
            // Lightning Light, Warm, Moss Light: bg / card / elevated /
            // other-bubble.
            QStringLiteral("_bgLight"), QStringLiteral("_cardLight"),
            QStringLiteral("_cardElevatedLight"), QStringLiteral("_hoverLight"),
            QStringLiteral("_warBg"), QStringLiteral("_warCard"),
            QStringLiteral("_warCardElevated"), QStringLiteral("_warOtherBubble"),
            QStringLiteral("_mosBg"), QStringLiteral("_mosCard"),
            QStringLiteral("_mosCardElevated"), QStringLiteral("_mosOtherBubble"),
        };
    }
    static QStringList darkInkSurfaces()
    {
        return {
            QStringLiteral("_bgDark"), QStringLiteral("_cardDark"),
            QStringLiteral("_cardElevatedDark"),
            QStringLiteral("_dkBg"), QStringLiteral("_dkCard"),
            QStringLiteral("_dkCardElevated"),
            QStringLiteral("_graBg"), QStringLiteral("_graCard"),
            QStringLiteral("_graCardElevated"), QStringLiteral("_graOtherBubble"),
            QStringLiteral("_norBg"), QStringLiteral("_norCard"),
            QStringLiteral("_norCardElevated"), QStringLiteral("_norOtherBubble"),
            QStringLiteral("_purBg"), QStringLiteral("_purCard"),
            QStringLiteral("_purCardElevated"), QStringLiteral("_purOtherBubble"),
            QStringLiteral("_indBg"), QStringLiteral("_indCard"),
            QStringLiteral("_indCardElevated"), QStringLiteral("_indOtherBubble"),
            QStringLiteral("_teaBg"), QStringLiteral("_teaCard"),
            QStringLiteral("_teaCardElevated"), QStringLiteral("_teaOtherBubble"),
            QStringLiteral("_stoCanvas"), QStringLiteral("_stoPanel"),
            QStringLiteral("_stoSelection"),
            // ADDED 2026-08-21 with the Storm ladder rebuild: cardElevated
            // stopped being an alias of _stoSelection, so it is a real
            // surface a sender name renders on and has to be checked.
            QStringLiteral("_stoCardElevated"),
        };
    }

private Q_SLOTS:
    // The mechanical cause of the reported "monochrome" shell. Storm is the
    // MOST saturated palette in the app (Lab chroma 27.1 against Moss
    // Light's 0.8) — it never lacked hue, it lacked SEPARATION: deep→canvas
    // measured 1.025:1, canvas→panel 1.138, panel→selection 1.138, and
    // cardElevated / hover / selected were literally the same #132558. A
    // contrast suite could not see any of that, because every one of those
    // surfaces still passed its ink pairs.
    void stormSurfaceLadderIsVisible()
    {
        const auto c = [this](const char *name) { return m_colors.value(QLatin1String(name)); };
        struct Rung { const char *lo; const char *hi; double minimum; };
        const Rung rungs[] = {
            { "_stoDeep", "_stoCanvas", 1.22 },          // timeline -> room list
            { "_stoCanvas", "_stoPanel", 1.22 },         // room list -> bubble
            { "_stoPanel", "_stoCardElevated", 1.22 },   // bubble -> raised chip
            { "_stoPanel", "_stoSelection", 1.22 },      // bubble -> selected row
            { "_stoSelection", "_stoSelectedHover", 1.16 },
            { "_stoPanel", "_stoReaction", 1.15 },       // bubble -> reaction pill
        };
        for (const Rung &r : rungs) {
            const QString lo = c(r.lo);
            const QString hi = c(r.hi);
            QVERIFY2(!lo.isEmpty() && !hi.isEmpty(),
                     qPrintable(QStringLiteral("missing rung: %1 / %2")
                                    .arg(QLatin1String(r.lo), QLatin1String(r.hi))));
            const double ratio = contrast(lo, hi);
            QVERIFY2(ratio >= r.minimum,
                     qPrintable(QStringLiteral("storm rung %1 -> %2 = %3 "
                                               "(< %4) — invisible step")
                                    .arg(QLatin1String(r.lo),
                                         QLatin1String(r.hi))
                                    .arg(ratio, 0, 'f', 3)
                                    .arg(r.minimum)));
        }
        // No two roles may collapse onto one literal again. cardElevated and
        // selection sit at the same lightness ON PURPOSE (elevation and
        // state are different meanings and must not be read off one axis),
        // so they are separated by TINT and the floor here is a colour
        // difference, not a contrast ratio.
        const QStringList distinct = { QStringLiteral("_stoDeep"),
                                       QStringLiteral("_stoCanvas"),
                                       QStringLiteral("_stoPanel"),
                                       QStringLiteral("_stoCardElevated"),
                                       QStringLiteral("_stoSelection"),
                                       QStringLiteral("_stoSelectedHover"),
                                       QStringLiteral("_stoReaction"),
                                       QStringLiteral("_stoHover") };
        for (int i = 0; i < distinct.size(); ++i) {
            for (int j = i + 1; j < distinct.size(); ++j) {
                QVERIFY2(m_colors.value(distinct.at(i))
                             != m_colors.value(distinct.at(j)),
                         qPrintable(QStringLiteral("%1 and %2 are the same "
                                                   "literal again")
                                        .arg(distinct.at(i), distinct.at(j))));
            }
        }
        QVERIFY2(deltaE(c("_stoCardElevated"), c("_stoSelection")) >= 12.0,
                 "elevated and selected sit at one lightness, so the tint "
                 "between them is the only thing telling them apart");
        // Structural: the palette object must not re-alias the roles.
        QVERIFY2(m_theme.contains(QStringLiteral("cardElevated: _stoCardElevated")),
                 "_storm must map cardElevated to its own literal");
        QVERIFY2(m_theme.contains(QStringLiteral("reaction: _stoReaction")),
                 "_storm must map reaction to its own literal");
        QVERIFY2(m_theme.contains(QStringLiteral("hover: Qt.alpha(_stoHover")),
                 "_storm hover must ride its own wash, not the selection");
    }

    // `danger` was a theme-invariant #DC2626 used as INK at ~40 call sites
    // and measured 3.03-4.03:1 on Storm's four surfaces — below AA on the
    // brand theme, everywhere, with no assertion anywhere in this file. The
    // only Storm danger the suite checked was the ROUTED _stoDanger, which
    // is why it passed for a year. Status ink is now held to exactly the
    // matrix the identity inks are held to.
    void statusInksAreReadableOnEveryThemeSurface()
    {
        const auto check = [this](const char *ink, const QStringList &surfaces,
                                  double minimum) {
            const QString value = m_colors.value(QLatin1String(ink));
            QVERIFY2(!value.isEmpty(),
                     qPrintable(QStringLiteral("missing status ink: %1")
                                    .arg(QLatin1String(ink))));
            for (const QString &surfaceName : surfaces) {
                const QString surface = m_colors.value(surfaceName);
                QVERIFY2(!surface.isEmpty(),
                         qPrintable(QStringLiteral("missing surface: %1")
                                        .arg(surfaceName)));
                const double ratio = contrast(value, surface);
                QVERIFY2(ratio >= minimum,
                         qPrintable(QStringLiteral("status ink %1 (%2) on "
                                                   "%3 (%4) = %5 (< %6)")
                                        .arg(QLatin1String(ink), value,
                                             surfaceName, surface)
                                        .arg(ratio, 0, 'f', 2)
                                        .arg(minimum)));
            }
        };
        const QStringList light = lightInkSurfaces();
        const QStringList dark = darkInkSurfaces();
        for (const char *ink : { "_dangerInkLight", "_warnInkLight",
                                 "_okInkLight", "_infoInkLight" })
            check(ink, light, 4.5);
        for (const char *ink : { "_dangerInkDark", "_warnInkDark",
                                 "_okInkDark", "_infoInkDark" })
            check(ink, dark, 4.5);
        // Presence is a DOT, not text: WCAG's graphical-object bar is 3:1.
        // Stated explicitly so nobody "upgrades" it to 4.5 and drives the
        // away amber back into the bolt's neighbourhood to satisfy it.
        check("_awayLight", light, 3.0);
        check("_awayDark", dark, 3.0);
        // Routing, not just values: the four roles must go through _p with
        // a mode fallback, or a future palette cannot override them and the
        // whole family silently reverts to one invariant literal.
        for (const char *role : { "success", "warning", "danger", "info" }) {
            const QRegularExpression routed(
                QStringLiteral("property\\s+color\\s+%1:\\s*_p\\.%1\\s*!==\\s*undefined")
                    .arg(QLatin1String(role)));
            QVERIFY2(m_theme.contains(routed),
                     qPrintable(QStringLiteral("%1 must route through _p")
                                    .arg(QLatin1String(role))));
        }
        // A destructive FILL is a different role from destructive INK. If
        // these ever collapse back into one token the ink fails AA again.
        QVERIFY2(m_colors.value(QStringLiteral("dangerFill"))
                     != m_colors.value(QStringLiteral("_dangerInkDark")),
                 "dangerFill and the dark danger ink must stay distinct");
        // ONE danger and ONE success on the dark themes. Before this round
        // the same concept rendered as _stoDanger #FF8FA0 inside a menu and
        // `danger` #DC2626 outside it, in one window; the fix is only real
        // while the two literals stay equal, and they are separate literals
        // (Storm's are part of the SPEC §1 table the trust card pins), so
        // the equality is asserted rather than assumed.
        QCOMPARE(m_colors.value(QStringLiteral("_stoDanger")),
                 m_colors.value(QStringLiteral("_dangerInkDark")));
        QCOMPARE(m_colors.value(QStringLiteral("_stoSuccess")),
                 m_colors.value(QStringLiteral("_okInkDark")));
    }

    // Yellow discipline, extended past the identity inks. The Storm bolt is
    // the app's reserved "active / selected / verified" signal; anything
    // near it in colour reads as brand chrome rather than as its own state.
    // presenceAway measured dE 18.3 from #FFD447 and `warning` 23.8, so an
    // away contact, a warning chip and the accent were three yellows a
    // reader could not separate — the exact rule the identity round wrote
    // for the gold name-ink slot, never applied to these two.
    void yellowSignalsStayClearOfTheBrandAccent()
    {
        const QString bolt = m_colors.value(QStringLiteral("_stoBolt"));
        QVERIFY2(!bolt.isEmpty(), "missing _stoBolt");
        const char *yellowish[] = { "_awayDark", "_awayLight",
                                    "_warnInkDark", "_warnInkLight" };
        for (const char *name : yellowish) {
            const QString value = m_colors.value(QLatin1String(name));
            QVERIFY2(!value.isEmpty(),
                     qPrintable(QStringLiteral("missing %1").arg(QLatin1String(name))));
            const double d = deltaE(value, bolt);
            QVERIFY2(d >= 25.0,
                     qPrintable(QStringLiteral("%1 (%2) is too close to the "
                                               "brand bolt %3 (dE %4 < 25) — "
                                               "it will read as chrome")
                                    .arg(QLatin1String(name), value, bolt)
                                    .arg(d, 0, 'f', 1)));
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
        const QString settingsRaw = readAll(QStringLiteral(SETTINGS_QML_PATH));
        QVERIFY2(!settingsRaw.isEmpty(), "SettingsScreen.qml not readable");
        // Strip comments before the ban check. This guard is about which
        // tokens the surface USES, and a comment naming a token is not a use
        // — the 2026-08-21 round explained a fix with the words "Basic's
        // BusyIndicator inks palette.dark, which Main.qml maps to
        // AppTheme.textSecondary", and that prose failed the test while the
        // code beneath it correctly used AppTheme.bolt. A guard that forbids
        // NAMING the mistake you just fixed punishes the explanation.
        const QString settings = stripComments(settingsRaw);
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
            stripComments(readAll(QStringLiteral(QML_DIR "/RoomDelegate.qml")));
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
            // 2026-08-21: Moss Light and Deep Teal took 4 and 8 units of
            // their own accent hue into their surfaces (both measured as
            // near-grey shells: Lab chroma 0.8 and 5.3). The preview cards
            // in SettingsScreen.qml paint FIXED copies of those literals,
            // so they are stale until that file is swept — the values are
            // allowed here in advance so the sweep is not blocked by this
            // guard. tests/SettingsShellQmlTest.cpp asserts the same
            // literals and has to move in the same change.
            QStringLiteral("#f1f9f3"), QStringLiteral("#e7efe8"),
            QStringLiteral("#d6dfd8"), QStringLiteral("#e1ebe3"),
            QStringLiteral("#031919"), QStringLiteral("#091f20"),
            QStringLiteral("#193535"), QStringLiteral("#0c2526"),
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
