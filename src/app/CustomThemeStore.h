#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class SettingsManager;

// A user-authored theme.
//
// It is stored as a SPARSE map of role -> colour on top of a base theme, not
// as a full palette snapshot. Two consequences, both deliberate:
//   * overriding three colours means overriding three colours — everything
//     else keeps following the base theme, including any later improvement
//     to it;
//   * the config stays small and readable, and a role added to AppTheme later
//     does not leave every custom theme with a hole in it.
//
// AppTheme.qml remains the sole source of colour VALUES. This class knows the
// role NAMES (so it can validate, and so the editor can be data-driven) and
// never knows what any theme's colours are.
class CustomThemeStore : public QObject
{
    Q_OBJECT

    // [{ key, label, group, hint }] — the editable roles, in editor order.
    // Labels and group names are translated, so this is re-read on a language
    // change rather than being CONSTANT.
    Q_PROPERTY(QVariantList roles READ roles NOTIFY rolesChanged)
    // Every saved theme: [{ id, name, baseTheme, overrideCount }], in the
    // order they were created. Theme id 12 always renders whichever of these
    // is ACTIVE, so switching between them is what selecting one means.
    Q_PROPERTY(QVariantList themes READ themes NOTIFY customThemeChanged)
    Q_PROPERTY(QString activeThemeId READ activeThemeId WRITE setActiveThemeId
                   NOTIFY customThemeChanged)
    // role -> "#RRGGBB", sparse. Only what the user actually changed.
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY customThemeChanged)
    // The theme this one was forked from; every role the user has not
    // overridden resolves through it.
    Q_PROPERTY(int baseTheme READ baseTheme WRITE setBaseTheme
                   NOTIFY customThemeChanged)
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY customThemeChanged)
    // True once anything has been customised. Until then the Settings card
    // offers "create" rather than "edit", and theme id 12 is not offered.
    Q_PROPERTY(bool exists READ exists NOTIFY customThemeChanged)
    Q_PROPERTY(int overrideCount READ overrideCount NOTIFY customThemeChanged)

public:
    explicit CustomThemeStore(SettingsManager *settings,
                              QObject *parent = nullptr);

    // The theme id a custom theme is selected as. One past Storm.
    static constexpr int kCustomThemeId = 12;

    QVariantList roles() const;
    QVariantList themes() const;
    QString activeThemeId() const;
    void setActiveThemeId(const QString &id);
    QVariantMap colors() const;
    int baseTheme() const;
    QString name() const;
    bool exists() const;
    int overrideCount() const;

    void setBaseTheme(int themeId);
    void setName(const QString &name);

    // Sets ONE role. An unknown role or a value that is not #RRGGBB is
    // refused and reported, never stored: AppTheme trusts this map, and a
    // corrupted config must not be able to paint the shell with garbage.
    Q_INVOKABLE bool setColor(const QString &role, const QString &hex);
    // Drops one override, so the role follows the base theme again.
    Q_INVOKABLE void resetColor(const QString &role);
    // Drops every override. The base theme choice and name are kept, because
    // "start over from this base" is the common intent.
    Q_INVOKABLE void resetAll();
    // Forgets every custom theme.
    Q_INVOKABLE void discard();

    // ---- the collection --------------------------------------------------

    // Creates an empty theme on the current base and selects it. Returns its
    // id, or an empty string when the collection is full.
    Q_INVOKABLE QString createTheme(const QString &name);
    // Copies the active theme's base AND colours into a new theme, and
    // selects it. This is how "edit a theme into a new one" works without a
    // separate mode.
    Q_INVOKABLE QString duplicateActiveTheme(const QString &name);
    Q_INVOKABLE void deleteTheme(const QString &id);

    // A one-line, pasteable representation of one theme. Compact JSON rather
    // than an opaque blob on purpose: a shared theme is a small readable
    // thing, and anyone receiving one can see exactly what it will change
    // before importing it.
    Q_INVOKABLE QString exportTheme(const QString &id) const;
    // Reads one back. Returns an empty string on success, or a short
    // user-facing reason. Everything is re-validated: unknown roles and
    // malformed colours are dropped by sanitize(), the base is clamped to a
    // real preset, and the name is bounded. A shared theme is untrusted
    // input that gets to paint the whole window.
    Q_INVOKABLE QString importTheme(const QString &payload);

    // Bounded so a corrupt config cannot grow without limit.
    static constexpr int kMaxThemes = 24;
    static constexpr int kMaxNameLength = 48;

    Q_INVOKABLE bool isValidColor(const QString &hex) const;
    Q_INVOKABLE bool isEditableRole(const QString &role) const;

    // ---- pure, testable ---------------------------------------------------

    static QStringList editableRoles();
    static bool roleIsEditable(const QString &role);
    static bool colorIsValid(const QString &hex);
    // Drops every unknown role and every malformed value. This is what stands
    // between a hand-edited config file and the renderer.
    static QVariantMap sanitize(const QVariantMap &raw);

Q_SIGNALS:
    void customThemeChanged();
    void rolesChanged();

private:
    struct Theme {
        QString id;
        QString name;
        int baseTheme = 0;
        QVariantMap colors;
    };

    void store(const QVariantMap &colors);
    // Parsed once and cached: QML reads colors()/baseTheme()/overrideCount()
    // once per role per repaint, and re-parsing the whole collection from
    // JSON on each of those was measurable in the editor.
    const QList<Theme> &load() const;
    void save(const QList<Theme> &themes, const QString &activeId);
    int activeIndex() const;
    static QString makeId(const QList<Theme> &existing);
    static Theme fromJson(const QJsonObject &object);
    static QJsonObject toJson(const Theme &theme);

    SettingsManager *m_settings = nullptr;
    mutable QList<Theme> m_cache;
    mutable QString m_activeId;
    mutable bool m_loaded = false;
};
