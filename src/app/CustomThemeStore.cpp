#include "app/CustomThemeStore.h"

#include "app/SettingsManager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

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
    { "rail",          QT_TR_NOOP("Shell"),      QT_TR_NOOP("Spaces rail"),
      QT_TR_NOOP("The narrow strip of Spaces down the far edge") },
    { "sidebar",       QT_TR_NOOP("Shell"),      QT_TR_NOOP("Room list"),
      QT_TR_NOOP("The column of rooms and people") },
    { "background",    QT_TR_NOOP("Shell"),      QT_TR_NOOP("Conversation background"),
      QT_TR_NOOP("The ground the timeline sits on — the largest area on screen") },
    { "surface",       QT_TR_NOOP("Shell"),      QT_TR_NOOP("Panels and cards"),
      QT_TR_NOOP("Side panels, dialogs, the composer") },
    { "inputBg",       QT_TR_NOOP("Shell"),      QT_TR_NOOP("Text fields"),
      QT_TR_NOOP("The inside of the message box and every input") },

    // ---- row states ----
    { "hover",         QT_TR_NOOP("States"),     QT_TR_NOOP("Hovered row"),
      QT_TR_NOOP("A room row, menu item or message under the pointer") },
    { "selected",      QT_TR_NOOP("States"),     QT_TR_NOOP("Selected room"),
      QT_TR_NOOP("The open room, menu highlights and selected text") },
    { "selectedHover", QT_TR_NOOP("States"),     QT_TR_NOOP("Selected and hovered"),
      QT_TR_NOOP("The open room with the pointer on it") },
    { "cardElevated",  QT_TR_NOOP("States"),     QT_TR_NOOP("Raised chips"),
      QT_TR_NOOP("Keycaps, link previews, neutral buttons") },
    { "reaction",      QT_TR_NOOP("States"),     QT_TR_NOOP("Reaction pill"),
      QT_TR_NOOP("The background of an emoji reaction under a message") },

    // ---- messages ----
    { "ownBubble",     QT_TR_NOOP("Messages"),   QT_TR_NOOP("Your messages"),
      QT_TR_NOOP("The bubble behind messages you sent") },
    { "otherBubble",   QT_TR_NOOP("Messages"),   QT_TR_NOOP("Their messages"),
      QT_TR_NOOP("The bubble behind messages from everyone else") },
    { "codeBlock",     QT_TR_NOOP("Messages"),   QT_TR_NOOP("Code blocks"),
      QT_TR_NOOP("The surface behind fenced code in a message") },
    { "mention",       QT_TR_NOOP("Messages"),   QT_TR_NOOP("Mentions"),
      QT_TR_NOOP("The colour that marks a message naming you") },

    // ---- accent ----
    { "accent",        QT_TR_NOOP("Accent"),     QT_TR_NOOP("Accent"),
      QT_TR_NOOP("Primary buttons, focus rings, the checked state") },
    { "accentHover",   QT_TR_NOOP("Accent"),     QT_TR_NOOP("Accent, hovered"),
      QT_TR_NOOP("A primary button under the pointer") },
    { "accentPressed", QT_TR_NOOP("Accent"),     QT_TR_NOOP("Accent, pressed"),
      QT_TR_NOOP("A primary button while it is held down") },
    { "accentText",    QT_TR_NOOP("Accent"),     QT_TR_NOOP("Text on accent"),
      QT_TR_NOOP("The label painted on a filled accent button") },
    { "link",          QT_TR_NOOP("Accent"),     QT_TR_NOOP("Links"),
      QT_TR_NOOP("Web links inside messages") },

    // ---- text ----
    { "textPrimary",   QT_TR_NOOP("Text"),       QT_TR_NOOP("Main text"),
      QT_TR_NOOP("Message bodies, room names, headings") },
    { "textSecondary", QT_TR_NOOP("Text"),       QT_TR_NOOP("Secondary text"),
      QT_TR_NOOP("Message previews and supporting lines") },
    { "textMuted",     QT_TR_NOOP("Text"),       QT_TR_NOOP("Muted text"),
      QT_TR_NOOP("Timestamps, counts, section labels, icons") },
    { "textDisabled",  QT_TR_NOOP("Text"),       QT_TR_NOOP("Disabled text"),
      QT_TR_NOOP("A control that cannot be used right now") },
    { "selectedText",  QT_TR_NOOP("Text"),       QT_TR_NOOP("Text on a selection"),
      QT_TR_NOOP("The room name in the open room's row") },

    // ---- lines ----
    { "border",        QT_TR_NOOP("Lines"),      QT_TR_NOOP("Hairlines"),
      QT_TR_NOOP("The 1px line around every panel — the eye follows these") },
    { "borderStrong",  QT_TR_NOOP("Lines"),      QT_TR_NOOP("Strong lines"),
      QT_TR_NOOP("Field outlines and the scrollbar handle") },
};

constexpr auto kColorsKey = "appearance/customThemeColors";
constexpr auto kBaseKey   = "appearance/customThemeBase";
constexpr auto kNameKey   = "appearance/customThemeName";

// #RRGGBB only. Not 8-digit ARGB: a translucent SHELL surface composites over
// whatever is behind it, which makes the resulting contrast unknowable, and
// every contrast rule in this app is written against opaque values.
const QRegularExpression &hexRe()
{
    static const QRegularExpression re(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return re;
}

} // namespace

CustomThemeStore::CustomThemeStore(SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
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

QVariantMap CustomThemeStore::colors() const
{
    if (!m_settings)
        return {};
    const QString json =
        m_settings->appearanceValue(kColorsKey, QString()).toString();
    if (json.isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject())
        return {};
    return sanitize(doc.object().toVariantMap());
}

void CustomThemeStore::store(const QVariantMap &colors)
{
    if (!m_settings)
        return;
    const QVariantMap clean = sanitize(colors);
    const QString json = QString::fromUtf8(
        QJsonDocument(QJsonObject::fromVariantMap(clean)).toJson(
            QJsonDocument::Compact));
    m_settings->setAppearanceValue(kColorsKey, json);
    Q_EMIT customThemeChanged();
}

int CustomThemeStore::baseTheme() const
{
    if (!m_settings)
        return SettingsManager::StormTheme;
    const int stored =
        m_settings->appearanceValue(kBaseKey, SettingsManager::StormTheme).toInt();
    // A base must be a REAL palette. Basing a custom theme on the custom
    // theme, or on System (which is a resolution mode rather than a palette),
    // would be a cycle or a moving target.
    if (stored < SettingsManager::LightTheme || stored > SettingsManager::StormTheme)
        return SettingsManager::StormTheme;
    return stored;
}

void CustomThemeStore::setBaseTheme(int themeId)
{
    if (!m_settings || themeId == baseTheme())
        return;
    if (themeId < SettingsManager::LightTheme || themeId > SettingsManager::StormTheme)
        return;
    m_settings->setAppearanceValue(kBaseKey, themeId);
    Q_EMIT customThemeChanged();
}

QString CustomThemeStore::name() const
{
    if (!m_settings)
        return {};
    return m_settings->appearanceValue(kNameKey, QString()).toString();
}

void CustomThemeStore::setName(const QString &name)
{
    if (!m_settings || name == this->name())
        return;
    // Bounded: this lands in a theme picker row, not in a document.
    m_settings->setAppearanceValue(kNameKey, name.left(48));
    Q_EMIT customThemeChanged();
}

bool CustomThemeStore::exists() const
{
    return !colors().isEmpty();
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
    m_settings->setAppearanceValue(kColorsKey, QString());
    m_settings->setAppearanceValue(kNameKey, QString());
    Q_EMIT customThemeChanged();
}

bool CustomThemeStore::isValidColor(const QString &hex) const
{
    return colorIsValid(hex);
}

bool CustomThemeStore::isEditableRole(const QString &role) const
{
    return roleIsEditable(role);
}
