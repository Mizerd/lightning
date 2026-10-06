#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class SettingsManager;

// A user-authored theme, stored as a sparse role -> colour map over a base
// theme rather than a full palette. Unchanged roles keep following the base,
// and roles added to AppTheme later leave no holes.
//
// AppTheme.qml remains the sole source of colour values; this class knows
// only role names.
class CustomThemeStore : public QObject
{
    Q_OBJECT

    // [{ key, label, group, hint }], the editable roles in editor order.
    // Translated, so re-read on a language change rather than CONSTANT.
    Q_PROPERTY(QVariantList roles READ roles NOTIFY rolesChanged)
    // Every saved theme: [{ id, name, baseTheme, overrideCount }], in creation
    // order. Theme id 12 renders whichever of these is active.
    Q_PROPERTY(QVariantList themes READ themes NOTIFY customThemeChanged)
    Q_PROPERTY(QString activeThemeId READ activeThemeId WRITE setActiveThemeId
                   NOTIFY customThemeChanged)
    // role -> "#RRGGBB", sparse: only what the user changed.
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY customThemeChanged)
    // The base theme; every role not overridden resolves through it.
    Q_PROPERTY(int baseTheme READ baseTheme WRITE setBaseTheme
                   NOTIFY customThemeChanged)
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY customThemeChanged)
    // True once anything is customised. Until then Settings offers "create"
    // and theme id 12 is not offered.
    Q_PROPERTY(bool exists READ exists NOTIFY customThemeChanged)
    Q_PROPERTY(int overrideCount READ overrideCount NOTIFY customThemeChanged)
    // role -> { type: "linear"|"radial", angle: 0..359, stops: ["#RRGGBB" x2..3] },
    // sparse: only surface roles the user gave a gradient (gradientRoles()).
    // A theme saved before gradients existed simply has none.
    Q_PROPERTY(QVariantMap gradients READ gradients NOTIFY customThemeChanged)
    // The editor's undo history for the active theme: colour edits, resets,
    // Reset all and base changes. Cleared when the active theme changes
    // (switch, new, duplicate, delete, import, account switch). Names are not
    // recorded; the name field has its own text undo.
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)

public:
    explicit CustomThemeStore(SettingsManager *settings,
                              QObject *parent = nullptr);

    // The theme id a custom theme is selected as (one past Storm).
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
    QVariantMap gradients() const;

    void setBaseTheme(int themeId);
    void setName(const QString &name);

    // Sets one role. An unknown role or a non-#RRGGBB value is refused and
    // never stored, since AppTheme trusts this map.
    Q_INVOKABLE bool setColor(const QString &role, const QString &hex);
    // Drops one override, so the role follows the base theme again.
    Q_INVOKABLE void resetColor(const QString &role);
    // Gives a surface role a gradient. `spec` is { type, angle, stops } and is
    // sanitised exactly as a loaded file is; refused (false, nothing stored)
    // for a role outside gradientRoles() or a spec that sanitises to nothing.
    Q_INVOKABLE bool setGradient(const QString &role, const QVariantMap &spec);
    // Drops one gradient, so the role is flat again.
    Q_INVOKABLE void resetGradient(const QString &role);
    // Whether a role may carry a gradient (surfaces only: ink and states stay
    // flat, since a gradient under text is graded at its worst stop).
    Q_INVOKABLE bool isGradientRole(const QString &role) const;
    // Drops every override and every gradient, keeping the base theme and name.
    Q_INVOKABLE void resetAll();
    // Forgets every custom theme.
    Q_INVOKABLE void discard();

    // ---- undo --------------------------------------------------------------
    //
    // Successive setColor() calls on the same role merge into one step (a
    // drag in the picker is dozens of samples) until sealUndoStep(), which
    // the editor calls when a gesture ends or another role is opened. Every
    // other edit is a step of its own.
    bool canUndo() const;
    bool canRedo() const;
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    Q_INVOKABLE void sealUndoStep();

    // ---- the collection --------------------------------------------------

    // Creates an empty theme on the current base and selects it. Returns its
    // id, or an empty string when the collection is full.
    Q_INVOKABLE QString createTheme(const QString &name);
    // Copies the active theme's base and colours into a new theme and
    // selects it.
    Q_INVOKABLE QString duplicateActiveTheme(const QString &name);
    Q_INVOKABLE void deleteTheme(const QString &id);

    // One-line, compact JSON representation of one theme, readable so a
    // recipient can see what it changes before importing.
    Q_INVOKABLE QString exportTheme(const QString &id) const;
    // Reads one back. Returns empty on success or a short user-facing reason.
    // A shared theme is untrusted input: unknown roles and malformed colours
    // are dropped by sanitize(), the base is clamped to a real preset, and the
    // name is bounded.
    Q_INVOKABLE QString importTheme(const QString &payload);

    // Bounded so a corrupt config cannot grow without limit.
    static constexpr int kMaxThemes = 24;
    static constexpr int kMaxNameLength = 48;

    Q_INVOKABLE bool isValidColor(const QString &hex) const;
    Q_INVOKABLE bool isEditableRole(const QString &role) const;

    // ---- readability -------------------------------------------------------
    //
    // Grades a resolved palette handed in by QML
    // (AppTheme.paletteForTheme(12), the object the preview paints); it looks
    // nothing up itself. Two instruments: WCAG contrast for ink on a surface,
    // and CIE L* separation between surfaces, since a ratio misjudges
    // hairlines.
    //
    // Every check passes on all eleven shipped presets
    // (`everyReadabilityCheckPassesOnEveryShippedPreset`); a warning that
    // fires on a stock theme gets ignored, so some checks are deliberately
    // absent (see the table in the .cpp). It warns and never refuses.

    // Grades one resolved palette. Returns one entry per failing check:
    //   { role, fg, bg, label, fgLabel, bgLabel, kind, value, minimum, passes }
    // `label` is the row's sentence; `fgLabel`/`bgLabel` the two role names.
    // `role` is the editable role to open on click: the foreground when
    // editable, otherwise the background. `kind` is "ink" (WCAG ratio) or
    // "edge" (ΔL*).
    Q_INVOKABLE QVariantList audit(const QVariantMap &palette) const;
    // The checks that mention one role, passes included, for the live
    // readout under the open picker.
    //
    // An empty `role` returns the whole table. The suite relies on this to
    // count gradable checks, which keeps
    // `everyReadabilityCheckPassesOnEveryShippedPreset` from going vacuous.
    Q_INVOKABLE QVariantList auditForRole(const QVariantMap &palette,
                                          const QString &role) const;
    // Checks that were skipped: `gradePalette` does not guess at a
    // translucent or absent endpoint (see `asColor`), and an unreported skip
    // would look like a pass. Same empty-`role` convention as `auditForRole`.
    //
    // Returns { role, fg, bg, label, fgLabel, bgLabel, kind, minimum,
    // reason }, where `reason` is "translucent" (depends on what is behind
    // it) or "missing" (the palette has no such key, e.g. an AppTheme rename).
    Q_INVOKABLE QVariantList auditSkipped(const QVariantMap &palette,
                                          const QString &role) const;
    // audit() for a palette whose surface roles may carry gradients
    // (`gradients`: role -> sanitised spec, the shape `gradients` holds, or
    // the Depth stops). Every check is graded at its WORST stop: a gradient
    // role against a flat one is graded at each of its stops; two gradient
    // roles against each other are graded position by position (stop i with
    // stop i), since they are drawn side by side over the same extent. Same
    // row shape as audit(), failing rows only, plus `stop` (the index that
    // was worst). With no gradients it equals audit().
    Q_INVOKABLE QVariantList auditWithGradients(const QVariantMap &palette,
                                                const QVariantMap &gradients) const;
    // WCAG 2.x contrast ratio, 1.0 .. 21.0. Accepts "#RRGGBB" or "#AARRGGBB"
    // (how QML passes a `color`). Returns 0 for anything unparseable or
    // translucent; no caller treats 0 as a pass.
    Q_INVOKABLE double contrastRatio(const QString &a, const QString &b) const;
    // CIE L*, 0 .. 100. Used to sort the picker's suggestion swatches into a
    // ladder.
    Q_INVOKABLE double lightness(const QString &hex) const;

    // ---- pure, testable ---------------------------------------------------

    static QStringList editableRoles();
    static bool roleIsEditable(const QString &role);
    static bool colorIsValid(const QString &hex);
    // Drops every unknown role and malformed value; this is what stands
    // between a hand-edited config file and the renderer.
    static QVariantMap sanitize(const QVariantMap &raw);
    // The surface roles that may carry a gradient.
    static QStringList gradientRoles();
    static bool roleTakesGradient(const QString &role);
    // One gradient spec, or an empty map when unusable: type linear|radial
    // (default linear), angle wrapped into 0..359 (default 180, top to
    // bottom), 2 or 3 stops each an opaque #RRGGBB (upper-cased). Anything
    // else is dropped, never repaired into a guess.
    static QVariantMap sanitizeGradient(const QVariant &raw);
    // role -> spec for every gradient role with a usable spec.
    static QVariantMap sanitizeGradients(const QVariantMap &raw);
    static constexpr int kMinGradientStops = 2;
    static constexpr int kMaxGradientStops = 3;
    static double contrast(const QString &a, const QString &b);
    static double lstar(const QString &hex);
    // The translated label for a role, or the key itself if not editable.
    static QString roleLabel(const QString &role);
    // The editable role this one falls back to while unset (empty if none):
    // editing that role also recolours this one. Also in roles() as
    // `follows`.
    static QString roleFollows(const QString &role);
    // Number of checks in the table; asserted by the suite so a check cannot
    // be dropped silently.
    static int readabilityCheckCount();
    // The table itself: { fg, fgRole, bg, bgRole, label, minimum, kind }.
    // `fgRole` is empty for an ink AppTheme pins by literal. Lets the suite
    // assert that a check's palette key and editable role name the same
    // colour.
    static QVariantList readabilityChecks();
    // Store role -> palette key for the three roles whose spellings differ
    // (`inputBg`/`inputBackground`, `reaction`/`reactionBackground`,
    // `mention`/`mentionBadge`). Owned here so QML and the readability table
    // share one copy.
    static QVariantMap paletteKeyAliases();
    // The same map for QML, which looks roles up in JavaScript because
    // `effectiveColor` runs for every row on every repaint.
    Q_INVOKABLE QVariantMap roleAliases() const;
    // The palette key a role resolves to: its alias, or its own name.
    Q_INVOKABLE QString paletteKeyForRole(const QString &role) const;
    // Every palette key the table reads, so the suite can assert them against
    // AppTheme.paletteForTheme(). `gradePalette` skips absent keys, so a
    // rename would otherwise silently stop grading a pair.
    static QStringList readabilityPaletteKeys();

Q_SIGNALS:
    void customThemeChanged();
    void rolesChanged();
    void historyChanged();

private:
    // Drops the parsed cache so the next read uses the account active now.
    //
    // This is a data-loss guard: the collection is account-scoped
    // (SettingsManager::appearanceValue), and without it account B's first
    // write would save account A's cached themes over B's record. Wired to
    // SettingsManager::sessionChanged, which fires after the active account
    // id has moved (switch, add, sign-out).
    void invalidate();

    struct Theme {
        QString id;
        QString name;
        int baseTheme = 0;
        QVariantMap colors;
        QVariantMap gradients;
    };

    void store(const QVariantMap &colors);
    // Parsed once and cached: QML reads colors()/baseTheme()/overrideCount()
    // per role per repaint.
    const QList<Theme> &load() const;
    void save(const QList<Theme> &themes, const QString &activeId);
    int activeIndex() const;
    static QString makeId(const QList<Theme> &existing);
    static Theme fromJson(const QJsonObject &object);
    static QJsonObject toJson(const Theme &theme);

    // One undoable edit of one theme: the whole (small) colour map and the
    // base, before and after. `coalesce` is the role a colour edit touched,
    // empty for edits that never merge.
    struct UndoStep {
        QString themeId;
        int baseBefore = 0;
        int baseAfter = 0;
        QVariantMap colorsBefore;
        QVariantMap colorsAfter;
        QString coalesce;
    };
    // Called after a mutation with the state read before it.
    void recordStep(const QString &themeIdBefore, int baseBefore,
                    const QVariantMap &colorsBefore, const QString &coalesce);
    void clearHistory();
    void applyHistoryState(int base, const QVariantMap &colors);

    QList<UndoStep> m_undo;
    QList<UndoStep> m_redo;
    bool m_undoSealed = true;

    SettingsManager *m_settings = nullptr;
    mutable QList<Theme> m_cache;
    mutable QString m_activeId;
    mutable bool m_loaded = false;
};
