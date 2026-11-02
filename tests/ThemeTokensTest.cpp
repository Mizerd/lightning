
#include <QFile>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QtTest/QtTest>

#include "theme/IdentityColors.h"

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
    const QString h = hex.mid(1);
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

double lstarOf(const QString &hex)
{
    const double y = luminance(hex);
    return y <= 0.008856 ? y * 903.3 : 116.0 * std::cbrt(y) - 16.0;
}

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

// QML with line and block comments removed, so a "does this file use token X"
// scan is not tripped by prose. String contents are left alone, which is safe
// for callers looking for bare token references.
QString stripComments(const QString &qml)
{
    QString out = qml;
    out.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                  QRegularExpression::DotMatchesEverythingOption));
    out.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
    return out;
}

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

}

class ThemeTokensTest : public QObject
{
    Q_OBJECT

    QString m_theme;
    QHash<QString, QString> m_colors;

private Q_SLOTS:
    void initTestCase()
    {
        m_theme = readAll(QStringLiteral(APPTHEME_QML_PATH));
        QVERIFY2(!m_theme.isEmpty(), "AppTheme.qml not readable");

        const QRegularExpression re(QStringLiteral(
            "property\\s+color\\s+(\\w+)\\s*:\\s*\"(#[0-9A-Fa-f]{6,8})\""));
        auto it = re.globalMatch(m_theme);
        while (it.hasNext()) {
            const auto match = it.next();
            m_colors.insert(match.captured(1), match.captured(2));
        }
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
            QStringLiteral("windowBackground"), QStringLiteral("navBackground"),
            QStringLiteral("panelBackground"), QStringLiteral("surface"),
            QStringLiteral("surfaceElevated"), QStringLiteral("hover"),
            QStringLiteral("selected"), QStringLiteral("selectedHover"),
            QStringLiteral("inputBackground"),
            QStringLiteral("borderSubtle"), QStringLiteral("borderStrong"),
            QStringLiteral("textPrimary"), QStringLiteral("textSecondary"),
            QStringLiteral("textMuted"), QStringLiteral("textDisabled"),
            QStringLiteral("accent"), QStringLiteral("accentHover"),
            QStringLiteral("accentPressed"), QStringLiteral("accentText"),
            QStringLiteral("success"), QStringLiteral("warning"),
            QStringLiteral("danger"), QStringLiteral("info"),
            QStringLiteral("dangerFill"), QStringLiteral("dangerFillHover"),
            QStringLiteral("dangerFillPressed"), QStringLiteral("dangerText"),
            QStringLiteral("successFill"), QStringLiteral("warningFill"),
            QStringLiteral("infoFill"),
            QStringLiteral("incomingBubble"), QStringLiteral("outgoingBubble"),
            QStringLiteral("codeBlock"), QStringLiteral("reactionBackground"),
            QStringLiteral("reactionBorder"), QStringLiteral("reactionInk"),
            QStringLiteral("reactionSelectedBackground"),
            QStringLiteral("reactionSelectedBorder"),
            QStringLiteral("unreadBadge"), QStringLiteral("mentionBadge"),
            QStringLiteral("mentionChipFill"), QStringLiteral("mentionChipInk"),
            QStringLiteral("presenceOnline"), QStringLiteral("presenceAway"),
            QStringLiteral("presenceOffline"),
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
            QStringLiteral("chipNeutralInk"), QStringLiteral("chipNeutralFill"),
            QStringLiteral("chipAccentInk"), QStringLiteral("chipAccentFill"),
            QStringLiteral("chipSuccessInk"), QStringLiteral("chipSuccessFill"),
            QStringLiteral("chipWarningInk"), QStringLiteral("chipWarningFill"),
            QStringLiteral("chipDangerInk"), QStringLiteral("chipDangerFill"),
            QStringLiteral("chipInfoInk"), QStringLiteral("chipInfoFill"),
            QStringLiteral("chipBoltFill"), QStringLiteral("chipBoltInk"),
            QStringLiteral("scrollbarTrack"), QStringLiteral("scrollbarHandle"),
            QStringLiteral("scrollbarHandleHover"),
            QStringLiteral("scrollbarHandlePressed"),
            QStringLiteral("shadow"), QStringLiteral("shadowSoft"),
            QStringLiteral("shadowStrong"),
            QStringLiteral("scrimBackdrop"), QStringLiteral("scrimSurface"),
            QStringLiteral("scrimSurfaceRaised"),
            QStringLiteral("scrimSurfaceHover"), QStringLiteral("scrimBorder"),
            QStringLiteral("scrimInk"), QStringLiteral("scrimInkStrong"),
            QStringLiteral("scrimInkMuted"),
            QStringLiteral("scrimSuccessInk"), QStringLiteral("scrimDangerInk"),
            QStringLiteral("focusRing"), QStringLiteral("overlayScrim"),
            QStringLiteral("bolt"), QStringLiteral("boltInk"),
            QStringLiteral("stormPanel"), QStringLiteral("stormText"),
        };
        for (const QString &token : required) {
            const QRegularExpression decl(
                QStringLiteral("property\\s+color\\s+%1\\b").arg(token));
            QVERIFY2(m_theme.contains(decl),
                     qPrintable(QStringLiteral("missing token: %1").arg(token)));
        }
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
        for (const QString &token : { QStringLiteral("lineHeightBody"),
                                      QStringLiteral("lineHeightTight"),
                                      QStringLiteral("lineHeightDisplay") }) {
            const QRegularExpression decl(
                QStringLiteral("property\\s+real\\s+%1\\b").arg(token));
            QVERIFY2(m_theme.contains(decl),
                     qPrintable(QStringLiteral("missing leading token: %1")
                                    .arg(token)));
        }
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

    void theTypeScaleIsSmallAndOrdered()
    {
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
        QSet<int> distinct{ display, title, subtitle, body, meta, micro };
        QVERIFY2(distinct.size() <= 5,
                 qPrintable(QStringLiteral("the scale has %1 distinct sizes; "
                                           "five is the budget")
                                .arg(distinct.size())));
        QCOMPARE(subtitle, body);
        const int w[] = { intToken("weightBody", 0), intToken("weightMedium", 0),
                          intToken("weightStrong", 0), intToken("weightBold", 0),
                          intToken("weightDisplay", 0) };
        for (int i = 1; i < 5; ++i)
            QVERIFY2(w[i] > w[i - 1], "weights must ascend");
        QCOMPARE(w[0], 400);
        QCOMPARE(w[4], 800);
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
            { "_textPrimaryLight", "_bgLight", 4.5 },
            { "_textSecondaryLight", "_bgLight", 4.5 },
            { "_textMutedLight", "_bgLight", 4.5 },
            { "_textMutedLight", "_sidebarLight", 4.5 },
            { "_textPrimaryDark", "_bgDark", 4.5 },
            { "_textSecondaryDark", "_bgDark", 4.5 },
            { "_textMutedDark", "_bgDark", 4.5 },
            { "_textMutedDark", "_cardDark", 4.5 },
            { "_selectedTextLight", "_selectedLight", 4.5 },
            { "_dkSelectedText", "_dkSelected", 4.5 },
            { "_mosSelectedText", "_mosAccentSoft", 4.5 },
            { "_indSelectedText", "_indAccentSoft", 4.5 },
            { "_teaSelectedText", "_teaAccentSoft", 4.5 },
            { "_selectedTextLight", "_selectedLight", 4.5 },
            { "_selectedTextLight", "_selectedHoverLight", 4.5 },
            { "_selectedTextDark", "_selectedDark", 4.5 },
            { "_selectedTextDark", "_selectedHoverDark", 4.5 },
            { "_ownBubbleTextDefault", "_outgoingBubbleBlue", 4.5 },
            { "onAccentMuted", "_outgoingBubbleBlue", 4.5 },
            { "_textPrimaryLight", "_hoverLight", 4.5 },
            { "_textPrimaryDark", "_cardElevatedDark", 4.5 },
            { "dangerText", "_accentDanger", 4.5 },
            { "dangerText", "_dangerFillHover", 4.5 },
            { "dangerText", "_dangerFillPressed", 4.5 },
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
            { "scrimInk", "_scrimBase", 4.5 },
            { "scrimInkMuted", "_scrimBase", 4.5 },
            { "scrimSuccessInk", "_scrimBase", 4.5 },
            { "scrimDangerInk", "_scrimBase", 4.5 },
            { "_onAccent", "_accentBlue", 3.0 },
            { "_onAccent", "_graAccent", 3.0 },
            { "_onAccent", "_norAccent", 3.0 },
            { "_onAccent", "_purAccent", 3.0 },
            { "_dkTextPrimary", "_dkBg", 4.5 },
            { "_dkTextSecondary", "_dkBg", 4.5 },
            { "_dkTextMuted", "_dkBg", 4.5 },
            { "_dkTextMuted", "_dkCard", 4.5 },
            { "_dkSelectedText", "_dkSelected", 4.5 },
            { "_dkSelectedText", "_dkSelectedHover", 4.5 },
            { "_dkTextPrimary", "_dkCardElevated", 4.5 },
            { "_warTextPrimary", "_warBg", 4.5 },
            { "_warTextSecondary", "_warBg", 4.5 },
            { "_warTextMuted", "_warBg", 4.5 },
            { "_warTextMuted", "_warCard", 4.5 },
            { "_warSelectedText", "_warSelected", 4.5 },
            { "_warSelectedText", "_warSelectedHover", 4.5 },
            { "_warTextPrimary", "_warHover", 4.5 },
            { "_ownBubbleTextDefault", "_warOwnBubble", 4.5 },
            { "onAccentMuted", "_warOwnBubble", 4.5 },
            { "_onAccent", "_warAccent", 3.0 },
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
            { "_ownBubbleTextDefault", "_graOwnBubble", 4.5 },
            { "_ownBubbleTextDefault", "_norOwnBubble", 4.5 },
            { "_ownBubbleTextDefault", "_purOwnBubble", 4.5 },
            { "_mosTextPrimary", "_mosBg", 4.5 },
            { "_mosTextSecondary", "_mosBg", 4.5 },
            { "_mosTextMuted", "_mosBg", 4.5 },
            { "_mosTextMuted", "_mosSidebar", 4.5 },
            { "_mosTextMuted", "_mosCard", 4.5 },
            { "_mosSelectedText", "_mosSelected", 4.5 },
            { "_mosSelectedText", "_mosSelectedHover", 4.5 },
            { "_mosTextPrimary", "_mosHover", 4.5 },
            { "_ownBubbleTextDefault", "_mosOwnBubble", 4.5 },
            { "onAccentMuted", "_mosOwnBubble", 4.5 },
            { "_onAccent", "_mosAccent", 3.0 },
            { "_indTextPrimary", "_indBg", 4.5 },
            { "_indTextSecondary", "_indBg", 4.5 },
            { "_indTextMuted", "_indBg", 4.5 },
            { "_indTextMuted", "_indSidebar", 4.5 },
            { "_indTextMuted", "_indCard", 4.5 },
            { "_indSelectedText", "_indSelected", 4.5 },
            { "_indSelectedText", "_indSelectedHover", 4.5 },
            { "_indTextPrimary", "_indCardElevated", 4.5 },
            { "_ownBubbleTextDefault", "_indOwnBubble", 4.5 },
            { "onAccentMuted", "_indOwnBubble", 4.5 },
            { "_onAccent", "_indAccent", 3.0 },
            { "_teaTextPrimary", "_teaBg", 4.5 },
            { "_teaTextSecondary", "_teaBg", 4.5 },
            { "_teaTextMuted", "_teaBg", 4.5 },
            { "_teaTextMuted", "_teaSidebar", 4.5 },
            { "_teaTextMuted", "_teaCard", 4.5 },
            { "_teaSelectedText", "_teaSelected", 4.5 },
            { "_teaSelectedText", "_teaSelectedHover", 4.5 },
            { "_teaTextPrimary", "_teaCardElevated", 4.5 },
            { "_ownBubbleTextDefault", "_teaOwnBubble", 4.5 },
            { "onAccentMuted", "_teaOwnBubble", 4.5 },
            { "_teaAccentText", "_teaAccent", 4.5 },
            { "_textPrimaryDark", "_hoverDark", 4.5 },
            { "_dkTextPrimary", "_dkHover", 4.5 },
            { "_graTextPrimary", "_graHover", 4.5 },
            { "_norTextPrimary", "_norHover", 4.5 },
            { "_purTextPrimary", "_purHover", 4.5 },
            { "_indTextPrimary", "_indHover", 4.5 },
            { "_teaTextPrimary", "_teaHover", 4.5 },
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
            { "_textSecondaryLight", "_inputBgLight", 4.5 },
            { "_textSecondaryDark", "_inputBgDark", 4.5 },
            { "_dkTextSecondary", "_dkInputBg", 4.5 },
            { "_graTextSecondary", "_graInputBg", 4.5 },
            { "_norTextSecondary", "_norInputBg", 4.5 },
            { "_purTextSecondary", "_purInputBg", 4.5 },
            { "_warTextSecondary", "_warInputBg", 4.5 },
            { "_mosTextSecondary", "_mosInputBg", 4.5 },
            { "_indTextSecondary", "_indInputBg", 4.5 },
            { "_teaTextSecondary", "_teaInputBg", 4.5 },
            { "_stoTextSecondary", "_stoInset", 4.5 },
            { "_graTextSecondary", "_graBg", 4.5 },
            { "_norTextSecondary", "_norBg", 4.5 },
            { "_purTextSecondary", "_purBg", 4.5 },
            { "_stoTextSecondary", "_stoCanvas", 4.5 },
            { "_stoText", "_stoPanel", 4.5 },
            { "_stoTextSecondary", "_stoPanel", 4.5 },
            { "_stoTextMuted", "_stoPanel", 4.5 },
            { "_stoText", "_stoSelection", 4.5 },
            { "_stoTextMuted", "_stoInset", 4.5 },
            { "_stoBolt", "_stoPanel", 4.5 },
            { "_stoBoltInk", "_stoBolt", 4.5 },
            { "_stoDanger", "_stoPanel", 4.5 },
            { "_stoSuccess", "_stoPanel", 4.5 },
            { "_stoLink", "_stoPanel", 4.5 },
            { "_stoText", "_stoCardElevated", 4.5 },
            { "_stoTextSecondary", "_stoCardElevated", 4.5 },
            { "_stoBolt", "_stoCardElevated", 4.5 },
            { "_stoSuccess", "_stoCardElevated", 4.5 },
            { "_stoTextSecondary", "_stoReaction", 4.5 },
            { "_stoText", "_stoReaction", 4.5 },
            { "_stoText", "_stoDeep", 4.5 },
            { "_stoTextSecondary", "_stoDeep", 4.5 },
            { "_stoTextMuted", "_stoDeep", 4.5 },
            { "_stoText", "_stoCanvas", 4.5 },
            { "_stoTextMuted", "_stoCanvas", 4.5 },
            { "_stoText", "_stoSelectedHover", 4.5 },
            { "_stoBoltInk", "_stoLink", 4.5 },
            { "_ownBubbleTextDefault", "_stoOwnBubble", 4.5 },
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

    void senderNameInksAreDerivedAndMeetContrastOnMessageSurfaces()
    {
        struct ThemeGrounds { int id; const char *name; QStringList surfaces; };
        const QList<ThemeGrounds> themes = {
            { 1, "Lightning Light", { "_bgLight", "_cardLight",
                                      "_cardElevatedLight", "_hoverLight" } },
            { 2, "Lightning Dark",  { "_dkBg", "_dkCard", "_dkCardElevated" } },
            { 3, "Graphite",        { "_graBg", "_graCard", "_graCardElevated" } },
            { 5, "Nordic",          { "_norBg", "_norCard", "_norCardElevated" } },
            { 6, "Purple Dusk",     { "_purBg", "_purCard", "_purCardElevated" } },
            { 7, "Warm",            { "_warBg", "_warCard", "_warCardElevated",
                                      "_warOtherBubble" } },
            { 8, "Moss Light",      { "_mosBg", "_mosCard", "_mosCardElevated",
                                      "_mosOtherBubble" } },
            { 4,  "Midnight",     { "_bgDark", "_cardDark", "_cardElevatedDark" } },
            { 9,  "Indigo Night", { "_indBg", "_indCard", "_indCardElevated",
                                    "_indOtherBubble" } },
            { 10, "Deep Teal",    { "_teaBg", "_teaCard", "_teaCardElevated",
                                    "_teaOtherBubble" } },
            { 11, "Storm",        { "_stoDeep", "_stoCanvas", "_stoPanel",
                                    "_stoCardElevated" } },
        };

        constexpr double kMinNameSeparation = 9.0;
        double worstSeen = 21.0;
        for (const ThemeGrounds &theme : themes) {
            QList<QColor> grounds;
            for (const QString &token : theme.surfaces) {
                const QString hex = m_colors.value(token);
                QVERIFY2(!hex.isEmpty(),
                         qPrintable(QStringLiteral("missing surface: %1")
                                        .arg(token)));
                grounds.append(QColor(hex));
            }
            const QColor anchor = lightning::theme::anchorForTheme(theme.id);

            QList<QColor> inks;
            for (int slot = 0; slot < 9; ++slot) {
                const QColor ink =
                    lightning::theme::nameInk(slot, anchor, grounds);
                inks.append(ink);
                for (int g = 0; g < grounds.size(); ++g) {
                    const double ratio = contrast(ink.name(),
                                                  grounds.at(g).name());
                    worstSeen = qMin(worstSeen, ratio);
                    QVERIFY2(ratio >= 4.5,
                             qPrintable(QStringLiteral(
                                 "%1 slot %2 ink %3 on %4 (%5) = %6 (< 4.5)")
                                 .arg(QLatin1String(theme.name))
                                 .arg(slot).arg(ink.name(),
                                                theme.surfaces.at(g),
                                                grounds.at(g).name())
                                 .arg(ratio, 0, 'f', 2)));
                }
                if (slot == 4 && ink.saturation() > 20 && anchor.saturation() > 20) {
                    double gap = std::abs(anchor.hueF() - ink.hueF());
                    if (gap > 0.5)
                        gap = 1.0 - gap;
                    QVERIFY2(gap * 360.0 <= 6.0,
                             qPrintable(QStringLiteral(
                                 "%1: the ink family is not centred on the "
                                 "theme anchor (anchor hue %2, middle ink %3)")
                                 .arg(QLatin1String(theme.name))
                                 .arg(anchor.hue()).arg(ink.hue())));
                }
            }

            double themeWorst = 99.0;
            int wa = 0, wb = 0;
            for (int a = 0; a < inks.size(); ++a) {
                for (int b = a + 1; b < inks.size(); ++b) {
                    const double separation =
                        deltaE(inks.at(a).name(), inks.at(b).name());
                    if (separation < themeWorst) {
                        themeWorst = separation; wa = a; wb = b;
                    }
                }
            }
            qInfo("%-16s worst dE %5.1f (slots %d/%d)", theme.name,
                  themeWorst, wa, wb);
            QVERIFY2(themeWorst >= kMinNameSeparation,
                     qPrintable(QStringLiteral(
                         "%1 slots %2/%3 are the same colour (dE %4)")
                         .arg(QLatin1String(theme.name)).arg(wa).arg(wb)
                         .arg(themeWorst, 0, 'f', 1)));
        }
        qInfo("derived name inks: worst contrast across all themes %.2f",
              worstSeen);
    }

private:
    static QStringList lightInkSurfaces()
    {
        return {
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
            QStringLiteral("_stoCardElevated"),
        };
    }

private Q_SLOTS:
    void stormSurfaceLadderIsVisible()
    {
        const auto c = [this](const char *name) { return m_colors.value(QLatin1String(name)); };
        struct Rung { const char *lo; const char *hi; double minimum; };
        const Rung rungs[] = {
            { "_stoDeep", "_stoCanvas", 1.22 },
            { "_stoCanvas", "_stoPanel", 1.22 },
            { "_stoPanel", "_stoCardElevated", 1.22 },
            { "_stoPanel", "_stoSelection", 1.22 },
            { "_stoSelection", "_stoSelectedHover", 1.16 },
            { "_stoPanel", "_stoReaction", 1.15 },
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
        QVERIFY2(m_theme.contains(QStringLiteral("cardElevated: _stoCardElevated")),
                 "_storm must map cardElevated to its own literal");
        QVERIFY2(m_theme.contains(QStringLiteral("reaction: _stoReaction")),
                 "_storm must map reaction to its own literal");
        QVERIFY2(m_theme.contains(QStringLiteral("hover: Qt.alpha(_stoHover")),
                 "_storm hover must ride its own wash, not the selection");
    }

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
        check("_awayLight", light, 3.0);
        check("_awayDark", dark, 3.0);
        for (const char *role : { "success", "warning", "danger", "info" }) {
            const QRegularExpression routed(
                QStringLiteral("property\\s+color\\s+%1:\\s*_p\\.%1\\s*!==\\s*undefined")
                    .arg(QLatin1String(role)));
            QVERIFY2(m_theme.contains(routed),
                     qPrintable(QStringLiteral("%1 must route through _p")
                                    .arg(QLatin1String(role))));
        }
        QVERIFY2(m_colors.value(QStringLiteral("dangerFill"))
                     != m_colors.value(QStringLiteral("_dangerInkDark")),
                 "dangerFill and the dark danger ink must stay distinct");
        QCOMPARE(m_colors.value(QStringLiteral("_stoDanger")),
                 m_colors.value(QStringLiteral("_dangerInkDark")));
        QCOMPARE(m_colors.value(QStringLiteral("_stoSuccess")),
                 m_colors.value(QStringLiteral("_okInkDark")));
    }

    void theTextSelectionIsVisibleOnEveryTheme()
    {
        QStringList prefixes;
        for (auto it = m_colors.cbegin(); it != m_colors.cend(); ++it) {
            if (it.key().endsWith(QStringLiteral("InputBg")))
                prefixes << it.key().chopped(7);
        }
        QVERIFY2(prefixes.size() >= 8,
                 qPrintable(QStringLiteral("only %1 palettes define an input "
                                           "background; the scan is broken")
                                .arg(prefixes.size())));
        int checked = 0;
        for (const QString &prefix : prefixes) {
            const QString field = m_colors.value(prefix + QStringLiteral("InputBg"));
            const QString sel = m_colors.value(prefix + QStringLiteral("SelectedHover"));
            if (field.isEmpty() || sel.isEmpty())
                continue;
            ++checked;
            const double d = qAbs(lstarOf(sel) - lstarOf(field));
            QVERIFY2(d >= 8.0,
                     qPrintable(QStringLiteral(
                         "%1: the text selection %2 is only %3 dL* from the "
                         "field %4 — a selection nobody can see reads as "
                         "\"select all does not work\"")
                                    .arg(prefix, sel)
                                    .arg(d, 0, 'f', 1)
                                    .arg(field)));
        }
        QVERIFY2(checked >= 8,
                 qPrintable(QStringLiteral("only %1 palettes were actually "
                                           "measured").arg(checked)));
    }

    void theSecondaryButtonEdgeReadsOnEveryCard()
    {
        const QString button = readAll(QStringLiteral(QML_DIR "/AppButton.qml"));
        QVERIFY(!button.isEmpty());
        QVERIFY2(button.contains(QStringLiteral("AppTheme.buttonSecondaryBorder")),
                 "AppButton's secondary edge is not the control-strength token");
        QVERIFY2(button.contains(QStringLiteral("AppTheme.buttonSecondaryFill")),
                 "AppButton's secondary kind has no resting fill");
        QVERIFY(m_theme.contains(QStringLiteral(
            "buttonSecondaryBorder: _p.buttonSecondaryBorder !== undefined")));
        QVERIFY(m_theme.contains(QStringLiteral(": borderStrong")));
        int checked = 0;
        for (auto it = m_colors.cbegin(); it != m_colors.cend(); ++it) {
            if (!it.key().endsWith(QStringLiteral("CardElevated"))
                || !it.key().startsWith(QLatin1Char('_')))
                continue;
            const QString prefix = it.key().chopped(12);
            const QString card = it.value();
            const QString strong = m_colors.value(prefix + QStringLiteral("BorderStrong"));
            const QString divider = m_colors.value(prefix + QStringLiteral("Border"));
            if (strong.isEmpty() || divider.isEmpty())
                continue;
            ++checked;
            const double edge = contrast(strong, card);
            QVERIFY2(edge >= 1.4,
                     qPrintable(QStringLiteral("%1: the secondary button edge %2 "
                                               "is %3:1 on the card %4")
                                    .arg(prefix, strong).arg(edge, 0, 'f', 2)
                                    .arg(card)));
            QVERIFY2(edge > contrast(divider, card),
                     qPrintable(QStringLiteral("%1: the control edge is no "
                                               "stronger than the divider")
                                    .arg(prefix)));
        }
        QVERIFY2(checked >= 8,
                 qPrintable(QStringLiteral("only %1 palettes were measured")
                                .arg(checked)));
    }

    void theSelectedSettingsNavMarksKeepContrastOnEveryTheme()
    {
        QVERIFY(m_theme.contains(QStringLiteral(
            "contrastRatio(bolt, settingsNavSelected) >= 3.0 ? bolt : stormText")));
        const QString screen = readAll(QStringLiteral(SETTINGS_QML_PATH));
        QVERIFY(!screen.isEmpty());
        QVERIFY2(screen.count(QStringLiteral("AppTheme.settingsNavSelectedMark")) >= 2,
                 "the selected row's icon and caret do not both take the mark token");
        int checked = 0;
        bool nordFellBack = false;
        for (auto it = m_colors.cbegin(); it != m_colors.cend(); ++it) {
            if (!it.key().endsWith(QStringLiteral("SelectedHover"))
                || !it.key().startsWith(QLatin1Char('_')))
                continue;
            const QString prefix = it.key().chopped(13);
            const QString fill = it.value();
            QString accent = m_colors.value(prefix + QStringLiteral("Accent"));
            QString ink = m_colors.value(prefix + QStringLiteral("TextPrimary"));
            if (prefix == QStringLiteral("_sto")) {
                accent = m_colors.value(QStringLiteral("_stoBolt"));
                ink = m_colors.value(QStringLiteral("_stoText"));
            }
            if (accent.isEmpty() || ink.isEmpty())
                continue;
            ++checked;
            const bool useAccent = contrast(accent, fill) >= 3.0;
            if (prefix == QStringLiteral("_nor"))
                nordFellBack = !useAccent;
            const QString mark = useAccent ? accent : ink;
            QVERIFY2(contrast(mark, fill) >= 3.0,
                     qPrintable(QStringLiteral("%1: the selected nav mark %2 is %3:1 on %4")
                                    .arg(prefix, mark)
                                    .arg(contrast(mark, fill), 0, 'f', 2)
                                    .arg(fill)));
        }
        QVERIFY2(checked >= 6, qPrintable(QStringLiteral("only %1 palettes measured")
                                              .arg(checked)));
        QVERIFY2(nordFellBack, "Nordic's accent passes on its own; the fixture no longer "
                               "reproduces the report");
    }

    void messageTextEditsUseTheThemedSelection()
    {
        QFile f(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(f.readAll());
        const QRegularExpression re(QStringLiteral("selectByMouse:\\s*(true|!/spoiler:toggle[^\\n]*\\n[^\\n]*\\))"));
        int count = 0;
        auto it = re.globalMatch(src);
        while (it.hasNext()) {
            const auto m = it.next();
            ++count;
            const QString after = src.mid(m.capturedEnd(), 200);
            QVERIFY2(after.contains(QStringLiteral("selectionColor: AppTheme.textSelection")),
                     "a selectByMouse item has no themed selectionColor");
            QVERIFY2(after.contains(QStringLiteral("selectedTextColor: AppTheme.text")),
                     "a selectByMouse item has no themed selectedTextColor");
        }
        QVERIFY2(count >= 2, qPrintable(QStringLiteral("scanned %1").arg(count)));
    }

    void selectedMessageTextIsReadableOnEveryTheme()
    {
        const QList<QPair<QString, QString>> pairs = {
            {"_textPrimaryLight", "_selectedHoverLight"},
            {"_textPrimaryDark", "_selectedHoverDark"},
            {"_stoText", "_stoSelectedHover"},
            {"_dkTextPrimary", "_dkSelectedHover"},
            {"_graTextPrimary", "_graSelectedHover"},
            {"_norTextPrimary", "_norSelectedHover"},
            {"_purTextPrimary", "_purSelectedHover"},
            {"_warTextPrimary", "_warSelectedHover"},
            {"_mosTextPrimary", "_mosSelectedHover"},
            {"_indTextPrimary", "_indSelectedHover"},
            {"_teaTextPrimary", "_teaSelectedHover"},
        };
        int checked = 0;
        for (const auto &pr : pairs) {
            const QString ink = m_colors.value(pr.first);
            const QString fill = m_colors.value(pr.second);
            QVERIFY2(!ink.isEmpty() && !fill.isEmpty(),
                     qPrintable(QStringLiteral("%1 / %2 not found")
                                    .arg(pr.first, pr.second)));
            ++checked;
            const double c = contrast(ink, fill);
            QVERIFY2(c >= 4.0,
                     qPrintable(QStringLiteral("%1 on %2 is only %3:1")
                                    .arg(ink, fill).arg(c, 0, 'f', 2)));
        }
        QCOMPARE(checked, 11);
    }

    void theVoicePreviewWaveformReadsOnEveryTheme()
    {
        QStringList prefixes;
        for (auto it = m_colors.cbegin(); it != m_colors.cend(); ++it) {
            if (it.key().endsWith(QStringLiteral("Accent")))
                prefixes << it.key().chopped(6);
        }
        int checked = 0;
        for (const QString &prefix : prefixes) {
            QString fill = m_colors.value(prefix + QStringLiteral("AccentSoft"));
            if (fill.isEmpty())
                fill = m_colors.value(prefix + QStringLiteral("Selected"));
            const QString played = m_colors.value(prefix + QStringLiteral("TextPrimary"));
            const QString rest = m_colors.value(prefix + QStringLiteral("TextMuted"));
            if (fill.isEmpty() || played.isEmpty() || rest.isEmpty())
                continue;
            ++checked;
            QVERIFY2(contrast(played, fill) >= 3.0,
                     qPrintable(QStringLiteral(
                         "%1: the played waveform ink %2 is only %3:1 on the "
                         "preview pill %4")
                                    .arg(prefix, played)
                                    .arg(contrast(played, fill), 0, 'f', 2)
                                    .arg(fill)));
            QVERIFY2(contrast(rest, fill) >= 2.0,
                     qPrintable(QStringLiteral(
                         "%1: the unplayed waveform ink %2 is only %3:1 on "
                         "the preview pill %4 — bars nobody can see")
                                    .arg(prefix, rest)
                                    .arg(contrast(rest, fill), 0, 'f', 2)
                                    .arg(fill)));
            const double d = qAbs(lstarOf(played) - lstarOf(rest));
            QVERIFY2(d >= 12.0,
                     qPrintable(QStringLiteral(
                         "%1: played %2 and unplayed %3 are %4 dL* apart — "
                         "the strip shows no progress")
                                    .arg(prefix, played, rest)
                                    .arg(d, 0, 'f', 1)));
        }
        QVERIFY2(checked >= 7,
                 qPrintable(QStringLiteral("only %1 palettes were actually "
                                           "measured").arg(checked)));
    }

    void everySettingsCardSurfaceNamesTheSameRaisedPlane()
    {
        struct Probe {
            const char *path;
            const char *what;
            const char *pattern;
        };
        const Probe probes[] = {
            { QML_DIR "/SettingsScreen.qml", "SettingsCard",
              "component\\s+SettingsCard\\s*:\\s*Pane\\s*\\{"
              ".*?(?<![.\\w])color:\\s*AppTheme\\.(\\w+)" },
            { QML_DIR "/SettingsScreen.qml", "the danger-zone card",
              "(?<![.\\w])color:\\s*AppTheme\\.(\\w+)\\s*"
              "border\\.color:\\s*dangerZone\\.expanded" },
            { QML_DIR "/UpdatesSettingsSection.qml", "UpdateCard",
              "component\\s+UpdateCard\\s*:\\s*Pane\\s*\\{"
              ".*?(?<![.\\w])color:\\s*AppTheme\\.(\\w+)" },
            { QML_DIR "/TrustCard.qml", "the trust card",
              "objectName:\\s*\"trustCardSurface\""
              ".*?(?<![.\\w])color:\\s*AppTheme\\.(\\w+)" },
        };
        int checked = 0;
        for (const Probe &probe : probes) {
            const QString src =
                stripComments(readAll(QString::fromLatin1(probe.path)));
            QVERIFY2(!src.isEmpty(),
                     qPrintable(QStringLiteral("%1 not readable")
                                    .arg(QLatin1String(probe.path))));
            const QRegularExpression re(
                QString::fromLatin1(probe.pattern),
                QRegularExpression::DotMatchesEverythingOption);
            QVERIFY2(re.isValid(), probe.pattern);
            const auto m = re.match(src);
            QVERIFY2(m.hasMatch(),
                     qPrintable(QStringLiteral(
                         "could not find the fill of %1 in %2 — the probe is "
                         "broken, which is not the same as the code being "
                         "right")
                             .arg(QLatin1String(probe.what),
                                  QLatin1String(probe.path))));
            QCOMPARE(m.captured(1), QStringLiteral("stormPanel"));
            ++checked;
        }
        QCOMPARE(checked, 4);
    }

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
        const QRegularExpression routed(QStringLiteral(
            "mentionHighlight:\\s*_p\\.mentionHighlight\\s*!==\\s*undefined"));
        QVERIFY2(m_theme.contains(routed),
                 "mentionHighlight must use the _p override idiom");
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
        const QString settingsRaw = readAll(QStringLiteral(SETTINGS_QML_PATH));
        QVERIFY2(!settingsRaw.isEmpty(), "SettingsScreen.qml not readable");
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
        for (int id = 1; id <= 12; ++id) {
            const QRegularExpression routed(
                QStringLiteral("case\\s+%1\\s*:\\s*return\\s+_").arg(id));
            QVERIFY2(m_theme.contains(routed),
                     qPrintable(QStringLiteral("theme id %1 not routed").arg(id)));
        }
        QVERIFY2(m_theme.contains(QStringLiteral("function rawPaletteForTheme")),
                 "the single theme-id -> palette switch is gone");
        QVERIFY2(m_theme.contains(QStringLiteral(
                     "readonly property var _p: rawPaletteForTheme(effectiveTheme)")),
                 "_p must route through rawPaletteForTheme, not a second switch");
        QVERIFY2(m_theme.contains(QStringLiteral("var p = rawPaletteForTheme(id)")),
                 "paletteForTheme must route through rawPaletteForTheme");

        const QRegularExpression customBase(
            QStringLiteral("customBase\\s*>=\\s*1\\s*&&\\s*customBase\\s*<=\\s*11"));
        QVERIFY2(m_theme.contains(customBase),
                 "the custom theme's base must be clamped to a real preset");
    }

    void lightThemeIsNotInvertedDark()
    {
        QVERIFY(m_colors.value(QStringLiteral("_textMutedLight"))
                != m_colors.value(QStringLiteral("_textMutedDark")));
        QVERIFY(m_colors.value(QStringLiteral("_bgLight"))
                != m_colors.value(QStringLiteral("_bgDark")));
    }

    void coreViewsUseTokensNotHex()
    {
        const QStringList files = {
            QStringLiteral(QML_DIR "/RoomDelegate.qml"),
            QStringLiteral(QML_DIR "/RoomActionsMenu.qml"),
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

    void everyAppThemeTokenReferencedInQmlIsDeclared()
    {
        const QString theme = readAll(QStringLiteral(QML_DIR "/AppTheme.qml"));
        QVERIFY(!theme.isEmpty());

        QSet<QString> declared;
        static const QRegularExpression propertyRe(
            QStringLiteral(R"(property\s+\w+\s+(\w+)\s*:)"));
        for (auto it = propertyRe.globalMatch(theme); it.hasNext();)
            declared.insert(it.next().captured(1));
        static const QRegularExpression functionRe(
            QStringLiteral(R"(function\s+(\w+)\s*\()"));
        for (auto it = functionRe.globalMatch(theme); it.hasNext();)
            declared.insert(it.next().captured(1));
        static const QRegularExpression signalRe(
            QStringLiteral(R"(signal\s+(\w+))"));
        for (auto it = signalRe.globalMatch(theme); it.hasNext();)
            declared.insert(it.next().captured(1));
        declared.insert(QStringLiteral("objectName"));
        QVERIFY2(declared.size() > 50,
                 "AppTheme declaration scan found implausibly little");

        static const QRegularExpression useRe(
            QStringLiteral(R"(AppTheme\.(\w+))"));
        QStringList missing;
        QDirIterator walker(QStringLiteral(QML_DIR),
                            { QStringLiteral("*.qml") }, QDir::Files,
                            QDirIterator::Subdirectories);
        while (walker.hasNext()) {
            const QString path = walker.next();
            if (path.endsWith(QLatin1String("AppTheme.qml")))
                continue;
            const QString source = stripComments(readAll(path));
            for (auto it = useRe.globalMatch(source); it.hasNext();) {
                const QString name = it.next().captured(1);
                if (declared.contains(name))
                    continue;
                const QString entry =
                    QFileInfo(path).fileName() + QStringLiteral(" -> AppTheme.")
                    + name;
                if (!missing.contains(entry))
                    missing.append(entry);
            }
        }
        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral("undeclared AppTheme tokens: ")
                                + missing.join(QStringLiteral(", "))));
    }
};

QTEST_GUILESS_MAIN(ThemeTokensTest)
#include "ThemeTokensTest.moc"
