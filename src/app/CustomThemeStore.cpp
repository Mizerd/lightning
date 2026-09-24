#include "app/CustomThemeStore.h"

#include "app/SettingsManager.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

struct Role {
    const char *key;     // must match a key in AppTheme.qml's palette objects
    const char *group;   // editor section
    const char *label;   // what the user sees
    const char *hint;    // where on screen this colour actually lands
};

// The editable roles: curated rather than exhaustive, since AppTheme carries
// hundreds of mostly derived tokens. Each key must exist in AppTheme's
// palette objects, which this map is merged over; `sanitize()` is the gate and
// `customThemeRolesMatchAppTheme` keeps the two in sync.
constexpr Role kRoles[] = {
    // ---- the four shell regions the user actually named ----
    { "rail",          QT_TRANSLATE_NOOP("CustomThemeStore", "Shell"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Spaces rail"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The narrow strip of Spaces down the far edge") },
    { "sidebar",       QT_TRANSLATE_NOOP("CustomThemeStore", "Shell"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Room list"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The column of rooms and people") },
    { "background",    QT_TRANSLATE_NOOP("CustomThemeStore", "Shell"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Conversation background"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The ground the timeline sits on — the largest area on screen") },
    { "surface",       QT_TRANSLATE_NOOP("CustomThemeStore", "Shell"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Panels and cards"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Side panels, dialogs, the composer") },
    { "inputBg",       QT_TRANSLATE_NOOP("CustomThemeStore", "Shell"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Text fields"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The inside of the message box and every input") },

    // ---- row states ----
    { "hover",         QT_TRANSLATE_NOOP("CustomThemeStore", "States"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Hovered row"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "A room row, menu item or message under the pointer") },
    { "selected",      QT_TRANSLATE_NOOP("CustomThemeStore", "States"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Selected room"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The open room, menu highlights and selected text") },
    { "selectedHover", QT_TRANSLATE_NOOP("CustomThemeStore", "States"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Selected and hovered"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The open room with the pointer on it") },
    { "cardElevated",  QT_TRANSLATE_NOOP("CustomThemeStore", "States"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Raised chips"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Keycaps, link previews, neutral buttons") },
    { "reaction",      QT_TRANSLATE_NOOP("CustomThemeStore", "States"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Reaction pill"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The background of an emoji reaction under a message") },

    // ---- messages ----
    { "ownBubble",     QT_TRANSLATE_NOOP("CustomThemeStore", "Messages"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Your messages"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The bubble behind messages you sent") },
    { "otherBubble",   QT_TRANSLATE_NOOP("CustomThemeStore", "Messages"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Their messages"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The bubble behind messages from everyone else") },
    { "codeBlock",     QT_TRANSLATE_NOOP("CustomThemeStore", "Messages"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Code blocks"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The surface behind fenced code in a message") },
    { "mention",       QT_TRANSLATE_NOOP("CustomThemeStore", "Messages"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Mentions"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The colour that marks a message naming you") },

    // ---- accent ----
    { "accent",        QT_TRANSLATE_NOOP("CustomThemeStore", "Accent"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Accent"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Primary buttons, focus rings, the checked state") },
    { "accentHover",   QT_TRANSLATE_NOOP("CustomThemeStore", "Accent"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Accent, hovered"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "A primary button under the pointer") },
    { "accentPressed", QT_TRANSLATE_NOOP("CustomThemeStore", "Accent"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Accent, pressed"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "A primary button while it is held down") },
    { "accentText",    QT_TRANSLATE_NOOP("CustomThemeStore", "Accent"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Text on accent"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The label painted on a filled accent button") },
    { "link",          QT_TRANSLATE_NOOP("CustomThemeStore", "Accent"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Links"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Web links inside messages") },

    // ---- text ----
    { "textPrimary",   QT_TRANSLATE_NOOP("CustomThemeStore", "Text"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Main text"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Message bodies, room names, headings") },
    { "textSecondary", QT_TRANSLATE_NOOP("CustomThemeStore", "Text"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Secondary text"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Message previews and supporting lines") },
    { "textMuted",     QT_TRANSLATE_NOOP("CustomThemeStore", "Text"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Muted text"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Timestamps, counts, section labels, icons") },
    { "textDisabled",  QT_TRANSLATE_NOOP("CustomThemeStore", "Text"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Disabled text"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "A control that cannot be used right now") },
    { "selectedText",  QT_TRANSLATE_NOOP("CustomThemeStore", "Text"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Text on a selection"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The room name in the open room's row") },

    // ---- lines ----
    { "border",        QT_TRANSLATE_NOOP("CustomThemeStore", "Lines"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Hairlines"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "The 1px line around every panel — the eye follows these") },
    { "borderStrong",  QT_TRANSLATE_NOOP("CustomThemeStore", "Lines"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Strong lines"),
      QT_TRANSLATE_NOOP("CustomThemeStore", "Field outlines and the scrollbar handle") },
};

// ---- the readability table -------------------------------------------------
//
// Keys are the names AppTheme.paletteForTheme() returns (e.g.
// `inputBackground`, not the store's `inputBg`); the role columns carry the
// editable name back to the UI.
//
// `worst` is the tightest margin across the eleven shipped presets,
// re-asserted by the suite. Bars sit under these so no stock theme warns.
//
// Deliberately not checked (the preset that rules each out):
//   * `selected` vs `sidebar`: Moss Light paints them the same (ΔL* 0.12)
//     and distinguishes by label, so `selectedText`/`selected` is checked.
//   * `hover` vs `sidebar` (Warm, ΔL* 2.61), `cardElevated` vs `surface`
//     (Moss Light, 1.85): floors too low for a useful bar.
//   * `accent` vs `background` at 3:1 (Indigo Night, 2.86) and `link` vs
//     `background` at 4.5 (Moss Light, 4.47); `link`/`surface` is checked.
//   * `textDisabled`: low contrast by design.
//   * `selectedText` vs `selectedHover` at 4.5: Nordic is 4.31. The bar is
//     not lowered to admit a preset; `selectedText`/`selected` covers the role.
//   * White on `mentionBadge`: its ink (`dangerText`) is a literal, not a
//     paletteForTheme() key, and three presets sit at 3.20. `mentionBadge`
//     vs `sidebar` is checked instead.
struct ReadabilityCheck {
    const char *fg;      // palette key of the ink, or of the upper surface
    const char *fgRole;  // editable role behind it, or nullptr
    const char *bg;      // palette key it sits on
    const char *bgRole;  // editable role behind it, or nullptr
    // Each check carries its own sentence: composing "%1 on %2" from role
    // labels doubles prepositions ("Text on accent on Accent") and is wrong
    // for edge checks, which compare surfaces rather than stack them.
    const char *phrase;
    double minimum;      // WCAG ratio for Ink, ΔL* for Edge
    bool edge;           // false = WCAG contrast, true = CIE L* separation
};

// Name for a foreground with no editable role: AppTheme pins `ownBubbleText`
// to white, so a failure there is fixed by moving the bubble.
constexpr auto kOwnBubbleInk =
    QT_TRANSLATE_NOOP("CustomThemeStore", "Text on your messages");

constexpr ReadabilityCheck kReadability[] = {
    // ---- ink on a surface: WCAG 4.5:1 -------------------- worst preset ----
    { "textPrimary", "textPrimary", "background", "background",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Main text on the conversation background"),
      4.5, false }, //  9.13 Warm
    { "textPrimary", "textPrimary", "sidebar", "sidebar",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Main text in the room list"),
      4.5, false }, // 10.04 Warm
    { "textPrimary", "textPrimary", "surface", "surface",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Main text on panels and cards"),
      4.5, false }, //  8.73 Nordic
    { "textPrimary", "textPrimary", "otherBubble", "otherBubble",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Main text on their messages"),
      4.5, false }, //  8.10 Nordic
    { "textPrimary", "textPrimary", "inputBackground", "inputBg",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Main text inside a text field"),
      4.5, false }, // 10.24 Lightning Dark
    { "textPrimary", "textPrimary", "codeBlock", "codeBlock",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Main text inside a code block"),
      4.5, false }, //  8.92 Warm
    { "textPrimary", "textPrimary", "hover", "hover",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Main text on a hovered row"),
      4.5, false }, //  6.40 Nordic
    { "textPrimary", "textPrimary", "cardElevated", "cardElevated",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Main text on a raised chip"),
      4.5, false }, //  7.35 Nordic
    // Grades `reactionInk` (textSecondary), the ink reaction pills actually
    // use.
    { "textSecondary", "textSecondary", "reactionBackground", "reaction",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Reaction text on a reaction pill"),
      // Guarded by `everyReadabilityCheckPassesOnEveryShippedPreset`.
      4.5, false },
    { "textSecondary", "textSecondary", "background", "background",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Secondary text on the conversation background"),
      4.5, false }, //  5.05 Warm
    { "textSecondary", "textSecondary", "sidebar", "sidebar",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Message previews in the room list"),
      4.5, false }, //  5.55 Warm
    { "textSecondary", "textSecondary", "surface", "surface",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Secondary text on panels and cards"),
      4.5, false }, //  5.37 Indigo Night
    { "textSecondary", "textSecondary", "inputBackground", "inputBg",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Secondary text inside a text field"),
      4.5, false }, //  5.81 Warm
    { "textMuted", "textMuted", "background", "background",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Timestamps on the conversation background"),
      4.5, false }, //  4.59 Warm
    { "textMuted", "textMuted", "sidebar", "sidebar",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Timestamps in the room list"),
      4.5, false }, //  5.05 Warm
    { "textMuted", "textMuted", "surface", "surface",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Timestamps on panels and cards"),
      4.5, false }, //  4.59 Deep Teal
    { "selectedText", "selectedText", "selected", "selected",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "The open room's name in the room list"),
      4.5, false }, //  5.40 Nordic
    { "link", "link", "surface", "surface",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Links on panels and cards"),
      4.5, false }, //  5.02 Storm
    // nullptr: the ink is a white literal with no editable role, so the
    // report sends the click to the bubble.
    { "ownBubbleText", nullptr, "ownBubble", "ownBubble",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Text on your own messages"),
      4.5, false }, //  6.22 Moss Light
    // 3:1 is a calibration choice, not WCAG large text (these labels are
    // body sized). AppTheme pins white-on-accent at 3:1, capping the accent's
    // luminance, and shipped presets sit below 4.5 (Nordic 4.03, Purple Dusk
    // 3.62).
    { "accentText", "accentText", "accent", "accent",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "A button's label on the accent"),
      3.0, false }, //  3.62 Purple Dusk
    // The same button's hover and pressed states, at the same bar: at 4.5
    // the hover pair fails on Purple Dusk (3.18), Nordic (3.35) and Graphite
    // (3.77).
    { "accentText", "accentText", "accentHover", "accentHover",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "A button's label while the pointer is on it"),
      3.0, false }, //  3.18 Purple Dusk
    { "accentText", "accentText", "accentPressed", "accentPressed",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "A button's label while it is held down"),
      3.0, false }, //  4.73 Purple Dusk
    // ---- surface against surface: CIE L* separation -------------------------
    { "border", "border", "surface", "surface",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Hairlines against panels and cards"),
      3.0, true  }, //  5.57 Nordic
    { "borderStrong", "borderStrong", "inputBackground", "inputBg",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "A field's outline against its inside"),
      6.0, true  }, // 20.30 Warm
    { "surface", "surface", "background", "background",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Panels against the conversation background"),
      2.5, true  }, //  6.34 Nordic
    { "rail", "rail", "sidebar", "sidebar",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "The Spaces rail against the room list"),
      2.0, true  }, //  4.23 Nordic
    // The mention badge against the room list it sits on. An edge check,
    // since the badge is a filled pill; every preset clears 30.
    { "mentionBadge", "mention", "sidebar", "sidebar",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "A mention badge against the room list"),
      6.0, true  }, // 30.02 Nordic
};

// Store role -> palette key, for the three roles whose spellings differ.
struct RoleAlias {
    const char *role;
    const char *paletteKey;
};

constexpr RoleAlias kRoleAliases[] = {
    { "inputBg",  "inputBackground" },
    { "reaction", "reactionBackground" },
    { "mention",  "mentionBadge" },
};

// The collection, and which of its entries theme id 12 renders.
constexpr auto kListKey   = "appearance/customThemeList";
constexpr auto kActiveKey = "appearance/customThemeActive";
// Pre-collection keys, kept so an existing custom theme survives the upgrade.
// Migrated into the list on first load, then the colours key is cleared so
// the migration cannot run twice.
constexpr auto kColorsKey = "appearance/customThemeColors";
constexpr auto kBaseKey   = "appearance/customThemeBase";
constexpr auto kNameKey   = "appearance/customThemeName";

// The marker on a shared theme. Bumped only if the shape changes in a way an
// older build could not read safely.
constexpr int kShareFormat = 1;
constexpr auto kShareKey = "lightning_theme";

// #RRGGBB only. A translucent shell surface composites over whatever is
// behind it, making contrast unknowable.
const QRegularExpression &hexRe()
{
    static const QRegularExpression re(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return re;
}

// A palette entry as a colour. AppTheme's resolved palette mixes QML `color`
// values (presets) with "#RRGGBB" strings (user overrides). An unparseable
// entry stays invalid, not black, so checks over it are skipped.
//
// This parse has no opacity guard, so `auditSkipped` can tell "translucent"
// from "missing".
QColor rawColor(const QVariant &value)
{
    QColor c;
    if (value.canConvert<QColor>())
        c = value.value<QColor>();
    if (!c.isValid()) {
        const QString text = value.toString().trimmed();
        c = text.isEmpty() ? QColor() : QColor::fromString(text);
    }
    return c;
}

// As rawColor(), but a translucent entry (e.g. Storm's
// `Qt.alpha(_stoHover, 0.22)`) is returned invalid: its contrast depends on
// what is behind it, so `gradePalette` skips it rather than grading raw RGB.
QColor asColor(const QVariant &value)
{
    const QColor c = rawColor(value);
    if (c.isValid() && c.alpha() != 255)
        return QColor();
    return c;
}

double lstarOf(const QColor &c);
double contrastOf(const QColor &a, const QColor &b);

// WCAG 2.x: sRGB -> linear with the 0.03928 knee, then the
// 0.2126/0.7152/0.0722 luminance weights. Also duplicated in AppTheme.qml and
// src/theme/IdentityColors.cpp; it is a published constant, pinned by
// `contrastMatchesPublishedReferenceValues`.
double channelLinear(double c)
{
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor &c)
{
    return 0.2126 * channelLinear(c.redF())
         + 0.7152 * channelLinear(c.greenF())
         + 0.0722 * channelLinear(c.blueF());
}

double contrastOf(const QColor &a, const QColor &b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

double lstarOf(const QColor &c)
{
    const double y = luminance(c);
    // The CIE break is at (6/29)^3; below it L* is linear in Y.
    return y > 0.008856 ? 116.0 * std::cbrt(y) - 16.0 : 903.3 * y;
}

} // namespace

CustomThemeStore::CustomThemeStore(SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    if (m_settings) {
        // Required: the cache is account-scoped, and sessionChanged fires
        // after the active account id has moved.
        connect(m_settings, &SettingsManager::sessionChanged, this,
                &CustomThemeStore::invalidate);
        // roles() labels come from tr(), which engine.retranslate() does not
        // reach once they are data, so announce a new role list.
        connect(m_settings, &SettingsManager::languageChanged, this,
                &CustomThemeStore::rolesChanged);
    }
}

void CustomThemeStore::invalidate()
{
    // Unconditional: the next read must consult the account active now.
    m_loaded = false;
    m_cache.clear();
    m_activeId.clear();
    // Re-entry is bounded: load() can only write the one-time legacy
    // migration, whose save() sets m_loaded.
    Q_EMIT customThemeChanged();
}

QStringList CustomThemeStore::editableRoles()
{
    QStringList out;
    out.reserve(int(std::size(kRoles)));
    for (const Role &r : kRoles)
        out << QLatin1String(r.key);
    return out;
}

bool CustomThemeStore::roleIsEditable(const QString &role)
{
    for (const Role &r : kRoles) {
        if (role == QLatin1String(r.key))
            return true;
    }
    return false;
}

bool CustomThemeStore::colorIsValid(const QString &hex)
{
    return hexRe().match(hex).hasMatch();
}

QVariantMap CustomThemeStore::sanitize(const QVariantMap &raw)
{
    QVariantMap out;
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it) {
        if (!roleIsEditable(it.key()))
            continue;
        const QString value = it.value().toString();
        if (!colorIsValid(value))
            continue;
        // Normalised so comparisons with palette values ignore case.
        out.insert(it.key(), value.toUpper());
    }
    return out;
}

// ---- readability -----------------------------------------------------------

double CustomThemeStore::contrast(const QString &a, const QString &b)
{
    const QColor ca = asColor(a);
    const QColor cb = asColor(b);
    if (!ca.isValid() || !cb.isValid())
        return 0.0;
    return contrastOf(ca, cb);
}

double CustomThemeStore::lstar(const QString &hex)
{
    const QColor c = asColor(hex);
    return c.isValid() ? lstarOf(c) : 0.0;
}

int CustomThemeStore::readabilityCheckCount()
{
    return int(std::size(kReadability));
}

QVariantList CustomThemeStore::readabilityChecks()
{
    QVariantList out;
    for (const ReadabilityCheck &check : kReadability) {
        QVariantMap entry;
        entry.insert(QStringLiteral("fg"), QLatin1String(check.fg));
        entry.insert(QStringLiteral("fgRole"),
                     check.fgRole ? QString::fromLatin1(check.fgRole)
                                  : QString());
        entry.insert(QStringLiteral("bg"), QLatin1String(check.bg));
        entry.insert(QStringLiteral("bgRole"), QLatin1String(check.bgRole));
        entry.insert(QStringLiteral("label"), tr(check.phrase));
        entry.insert(QStringLiteral("minimum"), check.minimum);
        entry.insert(QStringLiteral("kind"),
                     check.edge ? QStringLiteral("edge")
                                : QStringLiteral("ink"));
        out.append(entry);
    }
    return out;
}

QVariantMap CustomThemeStore::paletteKeyAliases()
{
    QVariantMap out;
    for (const RoleAlias &alias : kRoleAliases)
        out.insert(QLatin1String(alias.role), QLatin1String(alias.paletteKey));
    return out;
}

QVariantMap CustomThemeStore::roleAliases() const
{
    return paletteKeyAliases();
}

QString CustomThemeStore::paletteKeyForRole(const QString &role) const
{
    for (const RoleAlias &alias : kRoleAliases) {
        if (role == QLatin1String(alias.role))
            return QString::fromLatin1(alias.paletteKey);
    }
    return role;
}

QStringList CustomThemeStore::readabilityPaletteKeys()
{
    QStringList out;
    for (const ReadabilityCheck &check : kReadability) {
        if (!out.contains(QLatin1String(check.fg)))
            out << QLatin1String(check.fg);
        if (!out.contains(QLatin1String(check.bg)))
            out << QLatin1String(check.bg);
    }
    return out;
}

double CustomThemeStore::contrastRatio(const QString &a, const QString &b) const
{
    return contrast(a, b);
}

double CustomThemeStore::lightness(const QString &hex) const
{
    return lstar(hex);
}

namespace {

// What one pass over the table is being asked for.
enum class GradeMode {
    FailuresOnly,  // the report: what is wrong
    All,           // the live readout: every number, passes included
    SkippedOnly,   // what could not be graded at all
};

// Grades the table. FailuresOnly feeds the summary, All the live readout (so
// a number is seen climbing), SkippedOnly `auditSkipped`.
QVariantList gradePalette(const QVariantMap &palette, const QString &onlyRole,
                          GradeMode mode)
{
    QVariantList out;
    for (const ReadabilityCheck &check : kReadability) {
        const QString fgRole = check.fgRole ? QString::fromLatin1(check.fgRole)
                                            : QString();
        const QString bgRole = QString::fromLatin1(check.bgRole);
        if (!onlyRole.isEmpty() && onlyRole != fgRole && onlyRole != bgRole)
            continue;

        const QVariant fgValue = palette.value(QLatin1String(check.fg));
        const QVariant bgValue = palette.value(QLatin1String(check.bg));
        const QColor fg = asColor(fgValue);
        const QColor bg = asColor(bgValue);
        // A missing key (our bug) or a translucent colour is neither a
        // failure nor a pass; it is reported by SkippedOnly.
        const bool graded = fg.isValid() && bg.isValid();
        // Explicit, so a future fourth mode is not silently "wants graded".
        const bool wantGraded = mode != GradeMode::SkippedOnly;
        if (graded != wantGraded)
            continue;

        double value = 0.0;
        bool passes = false;
        if (graded) {
            value = check.edge ? std::abs(lstarOf(fg) - lstarOf(bg))
                               : contrastOf(fg, bg);
            passes = value >= check.minimum;
            if (mode == GradeMode::FailuresOnly && passes)
                continue;
        }

        QVariantMap entry;
        if (!graded) {
            // "missing" wins over "translucent": a renamed-away key is our
            // bug and must not be reported as the user's colour. Keyed on the
            // key being absent or empty, not on the parse failing.
            const auto absent = [&palette](const char *key) {
                const QString name = QLatin1String(key);
                return !palette.contains(name)
                       || palette.value(name).toString().trimmed().isEmpty();
            };
            entry.insert(QStringLiteral("reason"),
                         (absent(check.fg) || absent(check.bg))
                             ? QStringLiteral("missing")
                             : QStringLiteral("translucent"));
        }
        // Opened on click: the foreground when editable, otherwise the
        // background (a literal white ink is fixed by moving what is under it).
        entry.insert(QStringLiteral("role"),
                     fgRole.isEmpty() ? bgRole : fgRole);
        entry.insert(QStringLiteral("fg"), QLatin1String(check.fg));
        entry.insert(QStringLiteral("bg"), QLatin1String(check.bg));
        entry.insert(QStringLiteral("label"),
                     CustomThemeStore::tr(check.phrase));
        entry.insert(QStringLiteral("fgLabel"),
                     fgRole.isEmpty()
                         ? CustomThemeStore::tr(kOwnBubbleInk)
                         : CustomThemeStore::roleLabel(fgRole));
        entry.insert(QStringLiteral("bgLabel"),
                     CustomThemeStore::roleLabel(bgRole));
        entry.insert(QStringLiteral("kind"),
                     check.edge ? QStringLiteral("edge") : QStringLiteral("ink"));
        entry.insert(QStringLiteral("minimum"), check.minimum);
        // A skipped row carries no value and no verdict; 0/false would read
        // as a severe failure.
        if (graded) {
            entry.insert(QStringLiteral("value"), value);
            entry.insert(QStringLiteral("passes"), passes);
        }
        out.append(entry);
    }
    return out;
}

} // namespace

QVariantList CustomThemeStore::audit(const QVariantMap &palette) const
{
    return gradePalette(palette, QString(), GradeMode::FailuresOnly);
}

QVariantList CustomThemeStore::auditForRole(const QVariantMap &palette,
                                            const QString &role) const
{
    return gradePalette(palette, role, GradeMode::All);
}

QVariantList CustomThemeStore::auditSkipped(const QVariantMap &palette,
                                            const QString &role) const
{
    return gradePalette(palette, role, GradeMode::SkippedOnly);
}

QString CustomThemeStore::roleLabel(const QString &role)
{
    for (const Role &r : kRoles) {
        if (role == QLatin1String(r.key))
            return tr(r.label);
    }
    return role;
}

QVariantList CustomThemeStore::roles() const
{
    QVariantList out;
    for (const Role &r : kRoles) {
        QVariantMap entry;
        entry.insert(QStringLiteral("key"), QLatin1String(r.key));
        entry.insert(QStringLiteral("group"), tr(r.group));
        entry.insert(QStringLiteral("label"), tr(r.label));
        entry.insert(QStringLiteral("hint"), tr(r.hint));
        out.append(entry);
    }
    return out;
}

const QList<CustomThemeStore::Theme> &CustomThemeStore::load() const
{
    if (m_loaded)
        return m_cache;
    m_loaded = true;
    m_cache.clear();
    m_activeId.clear();
    if (!m_settings)
        return m_cache;

    const QString json =
        m_settings->appearanceValue(kListKey, QString()).toString();
    if (!json.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
        if (doc.isArray()) {
            const QJsonArray array = doc.array();
            for (const QJsonValue &value : array) {
                if (!value.isObject())
                    continue;
                Theme theme = fromJson(value.toObject());
                if (theme.id.isEmpty())
                    continue;
                m_cache.append(theme);
                if (m_cache.size() >= kMaxThemes)
                    break;
            }
        }
    }

    // One-time migration from the single-theme keys. Runs only when the list
    // is genuinely empty, so it cannot resurrect a theme the user deleted.
    if (m_cache.isEmpty()) {
        const QString legacyJson =
            m_settings->appearanceValue(kColorsKey, QString()).toString();
        const QJsonDocument legacy = QJsonDocument::fromJson(legacyJson.toUtf8());
        const QVariantMap legacyColors =
            legacy.isObject() ? sanitize(legacy.object().toVariantMap())
                              : QVariantMap();
        if (!legacyColors.isEmpty()) {
            Theme theme;
            theme.id = QStringLiteral("1");
            theme.name = m_settings->appearanceValue(kNameKey, QString())
                             .toString()
                             .left(kMaxNameLength);
            theme.baseTheme =
                m_settings->appearanceValue(kBaseKey, SettingsManager::StormTheme)
                    .toInt();
            theme.colors = legacyColors;
            m_cache.append(theme);
            // Written through save() so the list exists before the legacy key
            // is cleared — a crash between the two must not lose the theme.
            const_cast<CustomThemeStore *>(this)->save(m_cache, theme.id);
            m_settings->setAppearanceValue(kColorsKey, QString());
        }
    }

    m_activeId = m_settings->appearanceValue(kActiveKey, QString()).toString();
    bool known = false;
    for (const Theme &theme : m_cache) {
        if (theme.id == m_activeId) {
            known = true;
            break;
        }
    }
    // A missing or deleted active id resolves to the first theme, so theme
    // id 12 always renders something while a theme exists.
    if (!known)
        m_activeId = m_cache.isEmpty() ? QString() : m_cache.first().id;
    return m_cache;
}

CustomThemeStore::Theme CustomThemeStore::fromJson(const QJsonObject &object)
{
    Theme theme;
    theme.id = object.value(QStringLiteral("id")).toString();
    theme.name = object.value(QStringLiteral("name")).toString()
                     .left(kMaxNameLength);
    theme.baseTheme = object.value(QStringLiteral("base"))
                          .toInt(SettingsManager::StormTheme);
    if (theme.baseTheme < SettingsManager::LightTheme
        || theme.baseTheme > SettingsManager::StormTheme)
        theme.baseTheme = SettingsManager::StormTheme;
    theme.colors =
        sanitize(object.value(QStringLiteral("colors")).toObject().toVariantMap());
    return theme;
}

QJsonObject CustomThemeStore::toJson(const Theme &theme)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), theme.id);
    object.insert(QStringLiteral("name"), theme.name);
    object.insert(QStringLiteral("base"), theme.baseTheme);
    object.insert(QStringLiteral("colors"),
                  QJsonObject::fromVariantMap(theme.colors));
    return object;
}

void CustomThemeStore::save(const QList<Theme> &themes, const QString &activeId)
{
    if (!m_settings)
        return;
    QJsonArray array;
    for (const Theme &theme : themes)
        array.append(toJson(theme));
    m_settings->setAppearanceValue(
        kListKey, QString::fromUtf8(
                      QJsonDocument(array).toJson(QJsonDocument::Compact)));
    m_settings->setAppearanceValue(kActiveKey, activeId);
    m_cache = themes;
    m_activeId = activeId;
    m_loaded = true;
    Q_EMIT customThemeChanged();
}

QString CustomThemeStore::makeId(const QList<Theme> &existing)
{
    // Small monotonic ids keep the stored JSON readable; on collision, try
    // the next number.
    for (int candidate = 1; candidate <= kMaxThemes * 4; ++candidate) {
        const QString id = QString::number(candidate);
        bool taken = false;
        for (const Theme &theme : existing) {
            if (theme.id == id) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return id;
    }
    return QUuid::createUuid().toString(QUuid::Id128);
}

int CustomThemeStore::activeIndex() const
{
    const QList<Theme> &themes = load();
    for (int i = 0; i < themes.size(); ++i) {
        if (themes.at(i).id == m_activeId)
            return i;
    }
    return -1;
}

QVariantList CustomThemeStore::themes() const
{
    QVariantList out;
    for (const Theme &theme : load()) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), theme.id);
        entry.insert(QStringLiteral("name"), theme.name);
        entry.insert(QStringLiteral("baseTheme"), theme.baseTheme);
        entry.insert(QStringLiteral("overrideCount"), int(theme.colors.size()));
        out.append(entry);
    }
    return out;
}

QString CustomThemeStore::activeThemeId() const
{
    load();
    return m_activeId;
}

void CustomThemeStore::setActiveThemeId(const QString &id)
{
    const QList<Theme> themes = load();
    if (id == m_activeId)
        return;
    for (const Theme &theme : themes) {
        if (theme.id == id) {
            save(themes, id);
            return;
        }
    }
}

QString CustomThemeStore::createTheme(const QString &name)
{
    QList<Theme> themes = load();
    if (themes.size() >= kMaxThemes)
        return {};
    Theme theme;
    theme.id = makeId(themes);
    theme.name = name.trimmed().left(kMaxNameLength);
    if (theme.name.isEmpty())
        theme.name = tr("My theme");
    theme.baseTheme = themes.isEmpty() ? int(SettingsManager::StormTheme)
                                       : themes.at(qMax(0, activeIndex())).baseTheme;
    themes.append(theme);
    save(themes, theme.id);
    return theme.id;
}

QString CustomThemeStore::duplicateActiveTheme(const QString &name)
{
    QList<Theme> themes = load();
    const int index = activeIndex();
    if (index < 0 || themes.size() >= kMaxThemes)
        return {};
    Theme theme = themes.at(index);
    theme.id = makeId(themes);
    theme.name = name.trimmed().left(kMaxNameLength);
    if (theme.name.isEmpty())
        theme.name = tr("%1 copy").arg(themes.at(index).name);
    themes.append(theme);
    save(themes, theme.id);
    return theme.id;
}

void CustomThemeStore::deleteTheme(const QString &id)
{
    QList<Theme> themes = load();
    for (int i = 0; i < themes.size(); ++i) {
        if (themes.at(i).id != id)
            continue;
        themes.removeAt(i);
        QString nextActive = m_activeId;
        if (nextActive == id)
            nextActive = themes.isEmpty() ? QString()
                                          : themes.at(qMin(i, themes.size() - 1)).id;
        save(themes, nextActive);
        return;
    }
}

QString CustomThemeStore::exportTheme(const QString &id) const
{
    for (const Theme &theme : load()) {
        if (theme.id != id)
            continue;
        QJsonObject object;
        object.insert(QLatin1String(kShareKey), kShareFormat);
        object.insert(QStringLiteral("name"), theme.name);
        object.insert(QStringLiteral("base"), theme.baseTheme);
        object.insert(QStringLiteral("colors"),
                      QJsonObject::fromVariantMap(theme.colors));
        return QString::fromUtf8(
            QJsonDocument(object).toJson(QJsonDocument::Compact));
    }
    return {};
}

QString CustomThemeStore::importTheme(const QString &payload)
{
    QList<Theme> themes = load();
    if (themes.size() >= kMaxThemes)
        return tr("You already have the maximum number of themes.");
    const QJsonDocument doc =
        QJsonDocument::fromJson(payload.trimmed().toUtf8());
    if (!doc.isObject())
        return tr("That does not look like a shared theme.");
    const QJsonObject object = doc.object();
    if (object.value(QLatin1String(kShareKey)).toInt(0) != kShareFormat)
        return tr("That does not look like a shared theme.");

    Theme theme;
    theme.id = makeId(themes);
    theme.name = object.value(QStringLiteral("name")).toString()
                     .trimmed().left(kMaxNameLength);
    if (theme.name.isEmpty())
        theme.name = tr("Shared theme");
    theme.baseTheme = object.value(QStringLiteral("base"))
                          .toInt(SettingsManager::StormTheme);
    if (theme.baseTheme < SettingsManager::LightTheme
        || theme.baseTheme > SettingsManager::StormTheme)
        theme.baseTheme = SettingsManager::StormTheme;
    // Untrusted input: anything but a known role with an opaque #RRGGBB is
    // dropped here.
    theme.colors =
        sanitize(object.value(QStringLiteral("colors")).toObject().toVariantMap());
    if (theme.colors.isEmpty())
        return tr("That theme has no colours in it.");
    themes.append(theme);
    save(themes, theme.id);
    return {};
}

QVariantMap CustomThemeStore::colors() const
{
    const int index = activeIndex();
    return index < 0 ? QVariantMap() : m_cache.at(index).colors;
}

void CustomThemeStore::store(const QVariantMap &colors)
{
    QList<Theme> themes = load();
    int index = activeIndex();
    if (index < 0) {
        // Editing before anything exists creates the first theme, so the edit
        // is not discarded.
        Theme theme;
        theme.id = makeId(themes);
        theme.name = tr("My theme");
        theme.baseTheme = SettingsManager::StormTheme;
        themes.append(theme);
        index = themes.size() - 1;
    }
    themes[index].colors = sanitize(colors);
    save(themes, themes.at(index).id);
}

int CustomThemeStore::baseTheme() const
{
    const int index = activeIndex();
    if (index < 0)
        return SettingsManager::StormTheme;
    return m_cache.at(index).baseTheme;
}

void CustomThemeStore::setBaseTheme(int themeId)
{
    // A base must be a real palette: the custom theme itself would be a cycle
    // and System is a resolution mode, not a palette.
    if (themeId < SettingsManager::LightTheme || themeId > SettingsManager::StormTheme)
        return;
    QList<Theme> themes = load();
    int index = activeIndex();
    if (index < 0) {
        Theme theme;
        theme.id = makeId(themes);
        theme.name = tr("My theme");
        themes.append(theme);
        index = themes.size() - 1;
    } else if (themes.at(index).baseTheme == themeId) {
        return;
    }
    themes[index].baseTheme = themeId;
    save(themes, themes.at(index).id);
}

QString CustomThemeStore::name() const
{
    const int index = activeIndex();
    return index < 0 ? QString() : m_cache.at(index).name;
}

void CustomThemeStore::setName(const QString &name)
{
    QList<Theme> themes = load();
    const int index = activeIndex();
    if (index < 0)
        return;
    // Bounded: this lands in a theme picker row, not in a document.
    const QString clean = name.left(kMaxNameLength);
    if (themes.at(index).name == clean)
        return;
    themes[index].name = clean;
    save(themes, themes.at(index).id);
}

bool CustomThemeStore::exists() const
{
    return !load().isEmpty();
}

int CustomThemeStore::overrideCount() const
{
    return int(colors().size());
}

bool CustomThemeStore::setColor(const QString &role, const QString &hex)
{
    if (!roleIsEditable(role) || !colorIsValid(hex))
        return false;
    QVariantMap next = colors();
    next.insert(role, hex.toUpper());
    store(next);
    return true;
}

void CustomThemeStore::resetColor(const QString &role)
{
    QVariantMap next = colors();
    if (next.remove(role) > 0)
        store(next);
}

void CustomThemeStore::resetAll()
{
    if (colors().isEmpty())
        return;
    store({});
}

void CustomThemeStore::discard()
{
    if (!m_settings)
        return;
    save({}, QString());
    m_settings->setAppearanceValue(kColorsKey, QString());
    m_settings->setAppearanceValue(kNameKey, QString());
}

bool CustomThemeStore::isValidColor(const QString &hex) const
{
    return colorIsValid(hex);
}

bool CustomThemeStore::isEditableRole(const QString &role) const
{
    return roleIsEditable(role);
}
