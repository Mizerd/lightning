#pragma once

#include <QJsonObject>
#include <QList>
#include <functional>
#include <QObject>
#include <QString>
#include <QUrl>
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
    // recorded; the name field has its own text undo. Gradients are in the
    // history too (2026-10-14).
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    // The short human name of the step Undo / Redo would take ("Change Room
    // header colour"), empty when there is none.
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY historyChanged)
    // User-defined colour groups of the active theme (master prompt Part 6,
    // 4): [{ id, name, color, members: [role], labels: [label] }], in
    // creation order. A group links two or more editable roles, even ones with
    // no common built-in parent: editing the group or any member recolours
    // every member, in one undo step.
    //
    // Model: the group is AUTHORING metadata beside the ordinary materialised
    // colours. Every member's colour is stored in `colors` as usual (the
    // renderer and an older build see nothing new), and the store keeps
    // `colors[member] == group.color` as an invariant after every edit, load
    // and import. Rules: a role belongs to at most one group, groups hold
    // roles only (no nesting), a member never carries a gradient, a group
    // never has fewer than two members (it dissolves, keeping the colours).
    Q_PROPERTY(QVariantList colorGroups READ colorGroups NOTIFY customThemeChanged)
    Q_PROPERTY(QString redoLabel READ redoLabel NOTIFY historyChanged)

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
    // "Apply to all <group>": the role's group (roleFollows) takes `hex`, and
    // the group's element roles (this one included) drop their own colours so
    // every element of the group shows it. One undo step. A gradient the role
    // carries moves to the group too when the group takes gradients. Refused
    // (false, nothing stored) for a role with no group or an invalid colour.
    Q_INVOKABLE bool applyToGroup(const QString &role, const QString &hex);
    // The element roles that follow `group` directly, in editor order.
    Q_INVOKABLE QStringList groupMembers(const QString &group) const;
    QVariantList colorGroups() const;

    // ---- user colour groups ------------------------------------------------
    //
    // Every call below is ONE undo step (setGroupColor coalesces a drag until
    // sealUndoStep(), like setColor). Results that can be refused return
    // { ok, id, error } with a short user-facing `error`.

    // Links `roles` (two or more editable roles) under `name` with `hex` as
    // the group colour. Refused for an unknown role, a role carrying a
    // gradient (it would be flattened silently), or a role already in a group
    // unless `moveFromOtherGroups` (the move is then part of the same step).
    Q_INVOKABLE QVariantMap createColorGroup(const QString &name,
                                             const QStringList &roles,
                                             const QString &hex,
                                             bool moveFromOtherGroups);
    Q_INVOKABLE bool setGroupColor(const QString &groupId, const QString &hex);
    Q_INVOKABLE bool renameColorGroup(const QString &groupId, const QString &name);
    Q_INVOKABLE QVariantMap addToColorGroup(const QString &groupId,
                                            const QString &role,
                                            bool moveFromOtherGroups);
    // "Edit this element only": the role leaves its group and keeps the
    // colour it shows now. A group left with one member dissolves.
    Q_INVOKABLE bool detachFromColorGroup(const QString &role);
    // Ungroup: the relationship goes, every member keeps its current colour.
    Q_INVOKABLE bool deleteColorGroup(const QString &groupId);
    // The group id `role` belongs to, or empty.
    Q_INVOKABLE QString groupOfRole(const QString &role) const;
    // What editing `roles` together would change: { members: [role],
    // dependents: [{ role, label, via }] }, `dependents` being the editable
    // roles with no colour of their own that inherit from a member through
    // roleFollows (so they change too), `via` the member they follow.
    Q_INVOKABLE QVariantMap colorGroupImpact(const QStringList &roles) const;

    static constexpr int kMaxColorGroups = 16;

    // Bounded, deterministic repair of a theme's groups against its colours
    // and gradients (unknown/duplicate/gradient members dropped, groups below
    // two members or with a bad colour dropped, member colours materialised
    // from the group). Returns the number of repairs. Public for the suite.
    static int normalizeColorGroups(QVariantList *groups, QVariantMap *colors,
                                    const QVariantMap &gradients);

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
    QString undoLabel() const;
    QString redoLabel() const;
    // Names the NEXT recorded step (every edit has a default name; this is for
    // an edit the editor composes out of several, such as "Unlink Room list").
    Q_INVOKABLE void nameNextStep(const QString &label);
    // Everything recorded between begin and end is ONE step called `label`
    // ("Reset Room list to group" is a colour and a gradient). Not re-entrant
    // beyond a depth count; end without begin is ignored.
    Q_INVOKABLE void beginUndoGroup(const QString &label);
    Q_INVOKABLE void endUndoGroup();

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
    // What importing `payload` WOULD do, changing nothing: the same validation
    // as importTheme(), so the editor can show a preview and the reason a
    // paste is refused without a second implementation of either.
    //   { ok, error, name, base, colors, gradients, colourCount, gradientCount,
    //     atLimit }
    // `colors` / `gradients` are the sanitised maps (what would be stored),
    // `atLimit` true when "import as new" would be refused for lack of room.
    Q_INVOKABLE QVariantMap previewImport(const QString &payload) const;
    // "Replace current": the active theme takes the payload's base, colours
    // and gradients and KEEPS its own name. One undo step ("Replace with the
    // imported theme"). Returns empty on success or the same short reasons as
    // importTheme(); with no theme yet it imports as a new one.
    Q_INVOKABLE QString replaceActiveTheme(const QString &payload);
    // The text of a dropped theme file, or empty. Only a local regular file
    // under kMaxShareFileBytes is read; anything else is refused. The text is
    // untrusted and goes through previewImport()/importTheme() like a paste.
    Q_INVOKABLE QString readThemeFile(const QUrl &url) const;

    // Bounded so a corrupt config cannot grow without limit.
    static constexpr int kMaxThemes = 24;
    static constexpr qint64 kMaxShareFileBytes = 24576;
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
    // nudgeToPass() for QML.
    Q_INVOKABLE QString readableColor(const QString &hex, const QString &against,
                                      double minimum, bool edge) const;

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
    // The optional `positions` of a gradient spec (one 0..1, non-decreasing
    // value per stop), or empty when absent, malformed or an even spread.
    static QVariantList sanitizeGradientPositions(const QVariant &raw, int stopCount);
    // role -> spec for every gradient role with a usable spec.
    static QVariantMap sanitizeGradients(const QVariantMap &raw);
    static constexpr int kMinGradientStops = 2;
    static constexpr int kMaxGradientStops = 3;
    static double contrast(const QString &a, const QString &b);
    static double lstar(const QString &hex);
    // The nearest colour to `hex` (same hue and saturation, lightness moved
    // the least) that meets `minimum` against `against`: a WCAG ratio for ink
    // (`edge` false), a CIE L* separation for an edge (`edge` true). Returns
    // `hex` itself (upper-cased) when it already passes and an empty string
    // when `hex` or `against` is not a plain opaque colour or no lightness
    // reaches the bar. Backs the editor's "Make readable".
    static QString nudgeToPass(const QString &hex, const QString &against,
                               double minimum, bool edge);
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
    // Palette keys AppTheme derives rather than reads (the badges' painted
    // fills and inks) -> the editable role each is derived from.
    static QVariantMap derivedPaletteKeys();

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
        QVariantList groups;   // see colorGroups / normalizeColorGroups
    };

    void store(const QVariantMap &colors);
    // Parsed once and cached: QML reads colors()/baseTheme()/overrideCount()
    // per role per repaint.
    const QList<Theme> &load() const;
    void save(const QList<Theme> &themes, const QString &activeId);
    int activeIndex() const;
    static QString makeId(const QList<Theme> &existing);
    // A shared payload parsed and sanitised into a Theme (no id), or `error`
    // set to the user-facing reason. One implementation behind importTheme(),
    // previewImport() and replaceActiveTheme().
    static bool parseShared(const QString &payload, Theme *out, QString *error);
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
        QVariantMap gradientsBefore;
        QVariantMap gradientsAfter;
        QVariantList groupsBefore;
        QVariantList groupsAfter;
        QString coalesce;
        QString label;
    };
    // Called after a mutation with the state read before it. `label` is the
    // step's default name; nameNextStep() and an open group override it.
    void recordStep(const QString &themeIdBefore, int baseBefore,
                    const QVariantMap &colorsBefore,
                    const QVariantMap &gradientsBefore, const QString &coalesce,
                    const QString &label);
    // The same, for an edit that may also have changed the groups.
    void recordStep(const QString &themeIdBefore, int baseBefore,
                    const QVariantMap &colorsBefore,
                    const QVariantMap &gradientsBefore,
                    const QVariantList &groupsBefore, const QString &coalesce,
                    const QString &label);
    QVariantList groupsRaw() const;
    // Applies `edit` to the active theme (creating none), normalises its
    // groups, saves, and records one step. False when there is no active
    // theme, `edit` refused, or nothing changed.
    bool editActive(const std::function<bool(Theme &)> &edit,
                    const QString &coalesce, const QString &label);
    void clearHistory();
    void applyHistoryState(int base, const QVariantMap &colors,
                           const QVariantMap &gradients,
                           const QVariantList &groups);
    QString m_pendingLabel;
    QString m_groupLabel;
    int m_groupDepth = 0;

    QList<UndoStep> m_undo;
    QList<UndoStep> m_redo;
    bool m_undoSealed = true;

    SettingsManager *m_settings = nullptr;
    mutable QList<Theme> m_cache;
    mutable QString m_activeId;
    mutable bool m_loaded = false;
};
