#include "app/CustomThemeStore.h"

#include "app/SettingsManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

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
