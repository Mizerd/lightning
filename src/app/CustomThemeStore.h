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

    // ---- readability -------------------------------------------------------
    //
    // WHY THIS EXISTS. Until 2026-09-19 nothing anywhere checked what a user
    // had built. Two plausible themes — one light, one dark, neither
    // deliberately bad — were authored and measured ON THE RENDERED PIXELS of
    // the editor's own preview: every ink pair failed AA, the worst being a
    // white button label on a mint accent at 1.62:1, where 1273 of the
    // button's ~1456 pixels were flat accent and the label was effectively
    // gone. The editor said nothing, the import said nothing, and applying
    // the theme repainted the whole application that way.
    //
    // WHY IT IS HERE AND NOT IN AppTheme.qml. §5 puts derived semantic facts
    // and policy in C++ and presentation in QML, and this is policy: WHICH
    // pairs matter and what each one needs. It also keeps the class header's
    // invariant intact — the store still "never knows what any theme's
    // colours are". It GRADES a palette it is handed; it looks nothing up.
    // QML passes AppTheme.paletteForTheme(12), which is the same resolved
    // object the preview paints, so the report is about the pixels the user
    // is looking at rather than about the sparse override map.
    //
    // TWO INSTRUMENTS, DELIBERATELY. WCAG contrast for ink on a surface, and
    // CIE L* separation for one surface against another. A ratio is the wrong
    // instrument for a hairline: a shipped preset's border sits at 1.17:1
    // against its own surface, which as a ratio reads as catastrophic and as
    // ΔL* reads as "thin but present". §16 records the same distinction for
    // the Spaces rail's tint ladder.
    //
    // CALIBRATED AGAINST THE SHIPPED PRESETS, AND THAT IS THE POINT. Every
    // check below passes on ALL ELEVEN presets, with the worst margin
    // recorded beside it; `everyReadabilityCheckPassesOnEveryShippedPreset`
    // is what keeps that true. A warning that fires on a stock theme the
    // moment you fork it is a warning users learn to ignore, so several
    // obvious-looking checks are deliberately ABSENT — see the table's own
    // comments in the .cpp for which, and for the preset that rules each out.
    //
    // It WARNS and never refuses. Blocking a colour in a taste tool is worse
    // than the defect it prevents.

    // Grades one resolved palette. Returns one entry per failing check:
    //   { role, fg, bg, label, fgLabel, bgLabel, kind, value, minimum, passes }
    // `label` is the written sentence for the row ("Message previews in the
    // room list"); `fgLabel`/`bgLabel` are the two role names behind it.
    // `role` is the editable role the editor should open when the row is
    // clicked — the foreground when that is editable, otherwise the
    // background (a white-on-bubble failure is fixed by moving the bubble).
    // `kind` is "ink" (value is a WCAG ratio) or "edge" (value is ΔL*).
    Q_INVOKABLE QVariantList audit(const QVariantMap &palette) const;
    // The same grading narrowed to the checks that mention one role, PASSING
    // ONES INCLUDED. This is the live readout under the open picker: watching
    // 2.1:1 become 4.6:1 as you drag is what makes the editor teach, and that
    // needs the passes as well as the failures.
    //
    // AN EMPTY `role` NARROWS NOTHING and returns the whole table, passes
    // included. That is not an accident of the filter: it is how the suite
    // counts how many checks a given palette could actually be graded
    // against, which is what stops
    // `everyReadabilityCheckPassesOnEveryShippedPreset` going vacuous if the
    // palette ever stops resolving. Do not "fix" it to return nothing.
    Q_INVOKABLE QVariantList auditForRole(const QVariantMap &palette,
                                          const QString &role) const;
    // WCAG 2.x contrast ratio, 1.0 .. 21.0. Accepts "#RRGGBB" or "#AARRGGBB"
    // (QML hands a `color` over as the latter). Returns 0 for anything it
    // cannot parse, which no caller treats as a pass.
    Q_INVOKABLE double contrastRatio(const QString &a, const QString &b) const;
    // CIE L*, 0 .. 100. The editor sorts the picker's suggestion swatches on
    // it so the strip reads as a ladder instead of nine indistinguishable
    // near-blacks in semantic key order.
    Q_INVOKABLE double lightness(const QString &hex) const;

    // ---- pure, testable ---------------------------------------------------

    static QStringList editableRoles();
    static bool roleIsEditable(const QString &role);
    static bool colorIsValid(const QString &hex);
    // Drops every unknown role and every malformed value. This is what stands
    // between a hand-edited config file and the renderer.
    static QVariantMap sanitize(const QVariantMap &raw);
    static double contrast(const QString &a, const QString &b);
    static double lstar(const QString &hex);
    // The translated label kRoles carries for one role, or the key itself for
    // a role that is not editable.
    static QString roleLabel(const QString &role);
    // How many checks the table holds. Asserted by the suite so a check
    // cannot be dropped silently — the failure this project keeps meeting is
    // a sweep that comes back quietly short, never one that comes back loud.
    static int readabilityCheckCount();
    // Every distinct palette key the table reads, so the suite can assert
    // them against the object AppTheme.paletteForTheme() actually returns.
    // `gradePalette` SKIPS a check whose key is absent, so a rename in
    // AppTheme would quietly stop grading a pair in the running app while
    // every existing case stayed green — the silently-short-sweep shape §16
    // keeps recording.
    static QStringList readabilityPaletteKeys();

Q_SIGNALS:
    void customThemeChanged();
    void rolesChanged();

private:
    // Drops the parsed cache so the next read consults whichever account is
    // active NOW.
    //
    // THIS IS A DATA-LOSS GUARD, not a freshness nicety. The collection is
    // ACCOUNT-SCOPED storage (SettingsManager::appearanceValue), and the
    // cache below is filled once and returned forever. Without this, account
    // B's Appearance page lists account A's themes — and the first write B
    // makes (setName, setBaseTheme, deleteTheme, importTheme, a colour) calls
    // save() with that CACHED list, which persists A's themes over B's
    // record. B's themes are then gone, permanently and silently.
    //
    // Wired to SettingsManager::sessionChanged for the reason RailLayoutStore
    // records at its own connect(): that signal fires AFTER the active
    // account id has moved, so a re-read triggered by it resolves the
    // INCOMING account. It covers every way the answer can change — the
    // switch (setActiveAccountUserId), a new account being added
    // (saveSession), and the sign-out of the active one.
    void invalidate();

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
