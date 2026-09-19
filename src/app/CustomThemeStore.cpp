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

// The editable surface. Deliberately CURATED rather than exhaustive: AppTheme
// carries a couple of hundred tokens, most of them derived, and a picker with
// two hundred rows is not an editor, it is a haystack. Every entry here is a
// colour a person can point at on screen.
//
// Each key must exist as a role in AppTheme's palette objects, because
// AppTheme merges this map straight over the base palette. `sanitize()` is the
// gate; `customThemeRolesMatchAppTheme` in the test suite is what stops the
// two drifting.
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
// See the header for why this lives here and what it is calibrated against.
// Keys are the SEMANTIC names AppTheme.paletteForTheme() returns, not the
// store's own spelling, because the editor hands us that resolved object
// directly — `inputBackground` here is `inputBg` in kRoles above, and the
// `role` column below is what carries the editable name back to the UI.
//
// `worst` is the tightest margin across the eleven shipped presets, measured
// 2026-09-19 and re-asserted by the suite. Bars were chosen UNDER these, not
// from a standards document alone: a check that fires on a stock theme the
// moment somebody forks it teaches people to ignore every check.
//
// WHAT IS DELIBERATELY NOT CHECKED, and the preset that rules each one out:
//   * `selected` against `sidebar`. Moss Light paints them the SAME colour
//     (ΔL* 0.12) and tells the open room apart by its label instead — which
//     is why `selectedText`/`selected` IS checked. Any bar that would catch a
//     bad custom theme here fires on Moss Light.
//   * `hover` against `sidebar` (Warm, ΔL* 2.61) and `cardElevated` against
//     `surface` (Moss Light, ΔL* 1.85). Both floors are so low that a bar
//     beneath them could not catch anything a user would notice.
//   * `accent` against `background` at 3:1 (Indigo Night, 2.86) and
//     `link` against `background` at 4.5 (Moss Light, 4.47). Both are real
//     near-misses in shipped themes; `link`/`surface` is checked instead,
//     where every preset clears 5.0.
//   * `textDisabled` anywhere. It is SUPPOSED to be low contrast; flagging it
//     would be the audit arguing with the design it is auditing.
struct ReadabilityCheck {
    const char *fg;      // palette key of the ink, or of the upper surface
    const char *fgRole;  // editable role behind it, or nullptr
    const char *bg;      // palette key it sits on
    const char *bgRole;  // editable role behind it, or nullptr
    // WHAT THE ROW SAYS, WRITTEN OUT. Composing it from the two role labels
    // instead — "%1 on %2" — produced "Text on accent on Accent", "Text on
    // your messages on Your messages" and "Text on a selection on Selected
    // room", because three of the foregrounds are roles whose own names
    // already contain the preposition. It also forced "on" onto the four
    // EDGE checks, where the relation is "against": two surfaces meeting at
    // a line are not one stacked on the other. A composed string cannot know
    // either of those things, so each check carries its own sentence.
    const char *phrase;
    double minimum;      // WCAG ratio for Ink, ΔL* for Edge
    bool edge;           // false = WCAG contrast, true = CIE L* separation
};

// A foreground with no editable role of its own needs its own name.
// `ownBubbleText` is the one: AppTheme pins it to white by literal, so the
// only way to fix a failure there is to move the bubble underneath it.
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
    // THE INK THE APP ACTUALLY PAINTS. This graded `textPrimary`, and a
    // reaction pill is painted with `reactionInk`, which AppTheme defines as
    // `textSecondary` (AppTheme.qml) and MessageDelegate uses. Grading the
    // brighter ink made the check strictly LOOSER than reality: it could
    // report a pill readable whose real label was not, and with only
    // `textPrimary` overridden it printed advice about a pixel that does not
    // exist. The editor's own preview already used `reactionInk`.
    { "textSecondary", "textSecondary", "reactionBackground", "reaction",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Reaction text on a reaction pill"),
      // The recorded margin was 6.61 Nordic for the WRONG ink and is not
      // carried over. What guards it now is
      // `everyReadabilityCheckPassesOnEveryShippedPreset`, which was re-run
      // against the corrected ink and passes on all eleven.
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
    // nullptr, NOT a name: AppTheme pins this ink to white by literal, so
    // there is no editable role behind it and the report must send a click
    // to the BUBBLE instead. (It briefly read "None" here — a Python literal
    // that leaked in when this table was regenerated by script. Every case
    // stayed green, because the only guard on the role name ran over the
    // FAILING rows of two fixtures and neither of them overrides ownBubble.
    // The guard now runs over every row of every preset.)
    { "ownBubbleText", nullptr, "ownBubble", "ownBubble",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "Text on your own messages"),
      4.5, false }, //  6.22 Moss Light
    // 3:1 HERE IS A CALIBRATION DECISION, NOT A STANDARD. An earlier note
    // called a filled button's label "LARGE TEXT, so 3:1" — that is wrong:
    // WCAG large text is >=18.66px bold or >=24px and these labels are body
    // sized. (1.4.11 does give 3:1, but for a component's BOUNDARY, not its
    // label.) The number stands on its own evidence instead: AppTheme's
    // `_light` comment records that white-on-accent is pinned at 3:1, which
    // CAPS the accent's luminance, and two shipped presets sit under 4.5 by
    // that design — Nordic 4.03 and Purple Dusk 3.62. Holding them to 4.5
    // would mean the editor failing themes this application ships.
    { "accentText", "accentText", "accent", "accent",
      QT_TRANSLATE_NOOP("CustomThemeStore",
          "A button's label on the accent"),
      3.0, false }, //  3.62 Purple Dusk
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
};

// The collection, and which of its entries theme id 12 renders.
constexpr auto kListKey   = "appearance/customThemeList";
constexpr auto kActiveKey = "appearance/customThemeActive";
// Pre-collection keys, kept only so an existing custom theme survives the
// upgrade. Migrated into the list on first load, after which the colours key
// is cleared so the migration cannot run twice and duplicate the theme.
constexpr auto kColorsKey = "appearance/customThemeColors";
constexpr auto kBaseKey   = "appearance/customThemeBase";
constexpr auto kNameKey   = "appearance/customThemeName";

// The marker on a shared theme. Bumped only if the shape changes in a way an
// older build could not read safely.
constexpr int kShareFormat = 1;
constexpr auto kShareKey = "lightning_theme";

// #RRGGBB only. Not 8-digit ARGB: a translucent SHELL surface composites over
// whatever is behind it, which makes the resulting contrast unknowable, and
// every contrast rule in this app is written against opaque values.
const QRegularExpression &hexRe()
{
    static const QRegularExpression re(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return re;
}

// A palette entry as a colour. AppTheme's resolved palette is a MIXTURE: the
// preset values arrive as QML `color` (a QColor), while the user's own
// overrides are merged in as "#RRGGBB" STRINGS — the same split that made
// AppTheme's own `relativeLuminance` return NaN for every custom theme until
// `_asColor` was added there. Anything doing arithmetic on one has to come
// through here first, and an unparseable entry stays INVALID rather than
// becoming black, so a check over it is skipped instead of inventing a
// failure.
// A TRANSLUCENT ENTRY IS NOT GRADABLE, AND RETURNING IT WOULD BE A LIE.
//
// This class already refuses 8-digit hex from the user, and the comment at
// `hexRe()` says why: a translucent surface composites over whatever is
// behind it, which makes the resulting contrast UNKNOWABLE. The same is true
// of a translucent colour a theme INHERITED, and the base palettes contain
// them — Storm writes `hover: Qt.alpha(_stoHover, 0.22)`.
//
// Grading such an entry by its raw RGB is not an approximation, it is a
// different colour. Measured on the running editor: a brand-new theme on the
// Storm base, with ZERO user overrides, was told "Main text on a hovered row
// 4.4:1 — needs 4.5:1" — the cry-wolf-on-a-stock-theme failure the whole
// table is calibrated to avoid, produced by the one entry the suite's own
// AppTheme parser cannot resolve and therefore could not warn about. Only
// running the editor could show it.
//
// Returning an INVALID colour makes `gradePalette` skip the check, which is
// the honest answer: we do not know, so we do not say.
QColor asColor(const QVariant &value)
{
    QColor c;
    if (value.canConvert<QColor>())
        c = value.value<QColor>();
    if (!c.isValid()) {
        const QString text = value.toString().trimmed();
        c = text.isEmpty() ? QColor() : QColor::fromString(text);
    }
    if (c.isValid() && c.alpha() != 255)
        return QColor();
    return c;
}

double lstarOf(const QColor &c);
double contrastOf(const QColor &a, const QColor &b);

// WCAG 2.x, verbatim. sRGB -> linear with the 0.03928 knee (WCAG's own
// constant; 0.04045 is the sRGB spec's and this comment named it for a
// while), then the
// 0.2126/0.7152/0.0722 luminance weights.
//
// This is a third copy in the tree (AppTheme.qml:111 and
// src/theme/IdentityColors.cpp both carry it) and that is a deliberate,
// bounded choice, not an oversight: IdentityColors' copy is in an anonymous
// namespace and its translation unit is not compiled into `custom-theme-test`,
// so reaching it costs a header change and a CMake change in files this round
// does not own. The hazard §16 warns about is a hand-kept copy of a TUNED
// value drifting — this is a published W3C constant, and
// `contrastMatchesPublishedReferenceValues` pins it against the four
// reference pairs. Consolidating all three behind one exported helper is a
// good follow-up; it is not a correctness risk today.
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
        // See the header: the cache is account-scoped and load() fills it
        // exactly once, so nothing but this connection stops one account's
        // themes being listed under the next one and then WRITTEN over it.
        // sessionChanged is the right signal because it fires after the
        // active account id has already moved (RailLayoutStore's constructor
        // records the same reasoning, and why the earlier loggedOut is not
        // enough on its own).
        connect(m_settings, &SettingsManager::sessionChanged, this,
                &CustomThemeStore::invalidate);
        // roles() builds its labels and group names with tr(), and QML's
        // engine.retranslate() does NOT reach strings a C++ model has already
        // turned into data (main.cpp records that). The `roles` property has
        // always DECLARED NOTIFY rolesChanged and nothing ever emitted it, so
        // the theme editor's role labels kept the old language until it was
        // reopened — the header's claim that it is "re-read on a language
        // change" described an intention, not the code.
        connect(m_settings, &SettingsManager::languageChanged, this,
                &CustomThemeStore::rolesChanged);
    }
}

void CustomThemeStore::invalidate()
{
    // Unconditional, not "only when it changed": the point is that the next
    // read consults the account that is active NOW, and comparing against a
    // cache belonging to the previous account would be answering with it.
    m_loaded = false;
    m_cache.clear();
    m_activeId.clear();
    // The Appearance page and AppTheme both follow this signal, so the
    // incoming account's themes are what gets listed and painted. Re-entry is
    // bounded: a read from here runs load(), and the only write load() can
    // make is the one-time legacy migration, whose save() sets m_loaded and
    // emits once more without touching the session.
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
        // Normalised on the way in, so a comparison against a palette value
        // never fails on case alone.
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

// One row of the report. `failuresOnly` is what the summary wants; the live
// readout under an open picker wants the passes too, so it can show a number
// climbing rather than a warning blinking out of existence.
QVariantList gradePalette(const QVariantMap &palette, const QString &onlyRole,
                          bool failuresOnly)
{
    QVariantList out;
    for (const ReadabilityCheck &check : kReadability) {
        const QString fgRole = check.fgRole ? QString::fromLatin1(check.fgRole)
                                            : QString();
        const QString bgRole = QString::fromLatin1(check.bgRole);
        if (!onlyRole.isEmpty() && onlyRole != fgRole && onlyRole != bgRole)
            continue;

        const QColor fg = asColor(palette.value(QLatin1String(check.fg)));
        const QColor bg = asColor(palette.value(QLatin1String(check.bg)));
        // A palette missing one side of a pair is a palette this build does
        // not understand, not a failure to report at the user.
        if (!fg.isValid() || !bg.isValid())
            continue;

        const double value = check.edge
                                 ? std::abs(lstarOf(fg) - lstarOf(bg))
                                 : contrastOf(fg, bg);
        const bool passes = value >= check.minimum;
        if (failuresOnly && passes)
            continue;

        QVariantMap entry;
        // What the editor opens when this row is clicked. The foreground when
        // the user can edit it; otherwise the background, because a white ink
        // pinned by literal can only be fixed by moving what is under it.
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
        entry.insert(QStringLiteral("value"), value);
        entry.insert(QStringLiteral("minimum"), check.minimum);
        entry.insert(QStringLiteral("passes"), passes);
        out.append(entry);
    }
    return out;
}

} // namespace

QVariantList CustomThemeStore::audit(const QVariantMap &palette) const
{
    return gradePalette(palette, QString(), true);
}

QVariantList CustomThemeStore::auditForRole(const QVariantMap &palette,
                                            const QString &role) const
{
    return gradePalette(palette, role, false);
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
    // A missing or deleted active id resolves to the first theme rather than
    // to nothing: theme id 12 must always render SOMETHING when a theme
    // exists, or selecting Custom would paint an undefined palette.
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
    // Small monotonic ids keep the stored JSON readable; uniqueness is what
    // matters, so a collision just tries the next number.
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
    // Untrusted input that gets to paint the whole window: everything not a
    // known role carrying an opaque #RRGGBB is dropped here, not later.
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
        // Editing before anything exists creates the first theme, so a user
        // who opens the editor and picks a colour has a theme rather than a
        // discarded edit.
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
    // A base must be a REAL palette. Basing a custom theme on the custom
    // theme, or on System (which is a resolution mode rather than a palette),
    // would be a cycle or a moving target.
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
