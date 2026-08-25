#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

class SettingsManager;

/// Every rebindable keyboard shortcut in Lightning, in one place.
///
/// WHY THIS EXISTS AT ALL. Before this, all 20 `Shortcut` declarations in
/// qml/ carried hard-coded literal sequences and there was no rebinding
/// infrastructure of any kind — no registry, no QSettings key, no
/// QKeySequence anywhere in C++. That was survivable while Lightning chose
/// every key itself. It stopped being survivable the moment we compared the
/// set against another client's: Ctrl+B is the near-universal editor
/// convention for Bold AND Lightning's own "toggle the room list", and
/// Ctrl+Shift+B is "open bookmarks" elsewhere and "toggle the Spaces rail"
/// here. Two people can both be right about one key, which is precisely the
/// problem a registry solves and a longer list of literals does not.
///
/// TWO QT FACTS SHAPE EVERY DECISION BELOW. Neither is theoretical; both
/// have already cost this codebase something.
///
///  1. A `Shortcut` is consumed BEFORE the focused item ever sees the key.
///     Commit 4c2317f's message records a window-level "Space pauses media"
///     Shortcut silently killing timeline paging, the emoji grid and the GIF
///     grid; qml/SettingsScreen.qml restates the mechanism for its Escape.
///     Space paging in qml/TimelinePane.qml is STILL a `Keys.onPressed` case
///     rather than a Shortcut for exactly this reason. Consequence here: a
///     registry entry carries a CONTEXT, and a Global entry may not be bound
///     to a sequence with no Ctrl/Alt/Meta modifier — a bare letter bound
///     globally would take that letter away from every text field in the
///     application, and the person who did it would have no way to type the
///     rebinding that undoes it.
///
///  2. Two ENABLED `Shortcut`s on one sequence make Qt report an ambiguous
///     overload and fire NEITHER. qml/TimelinePane.qml documents this at its
///     Escape handler, which is why its exclusion of MiddleClickScroller is
///     explicit rather than order-dependent. Consequence here: conflict
///     detection is not cosmetic decoration on a settings page — an
///     undetected duplicate binding silently kills BOTH actions, and the
///     symptom ("two things stopped working") points nowhere near the cause.
///     So `setBinding` REFUSES a conflicting sequence rather than storing it
///     and rendering a warning next to it, and the reserved list below
///     carries the sequences that stay hard-coded and are therefore invisible
///     to the model's own rows.
///
/// GLOBAL vs EDITOR IS NOT A CONFLICT, IT IS A SHADOW. An Editor action is
/// delivered by the composer accepting Qt's ShortcutOverride event, which
/// happens BEFORE shortcut dispatch — so while the message box has focus the
/// Editor action wins, and everywhere else the Global action still works.
/// That is the honest resolution of the Ctrl+B collision: Ctrl+B is Bold
/// while you are typing a message and "toggle the room list" while you are
/// not, and NEITHER existing binding had to be silently taken away. The two
/// rows still say so (`shadowedBy` / `shadows`), because a key that does two
/// things deserves to be described rather than discovered.
///
/// PERSISTENCE is per account with a global fallback, the same rule theme,
/// message layout and text scale already use (SettingsManager::
/// appearanceValue): someone whose work account is a Space-heavy workspace
/// and whose personal account is a handful of DMs may reasonably want
/// different keys, and the global value is the logged-out default.
/// Sequences are stored as QKeySequence::PortableText so they round-trip
/// across platforms and locales, and an unparseable stored value is IGNORED
/// (the default applies) rather than being handed to QML, which would
/// produce a `Shortcut` bound to nothing with no way to notice.
class ShortcutRegistry : public QAbstractListModel
{
    Q_OBJECT
    /// How many rows currently report a hard conflict. Should always be 0 —
    /// setBinding refuses to create one — but a settings file edited by hand
    /// or written by a newer build can still produce one, and the page says
    /// so rather than pretending.
    Q_PROPERTY(int conflictCount READ conflictCount NOTIFY conflictCountChanged)
    /// True when at least one action is not on its default sequence, so the
    /// page can enable/disable "Reset all" honestly.
    Q_PROPERTY(bool anyCustomised READ anyCustomised NOTIFY anyCustomisedChanged)

public:
    /// WHERE an action is dispatched from, which decides what a legal
    /// sequence is (see fact 1 above).
    enum ActionContext {
        /// A window/application `Shortcut`. Pre-empts the focused item, so
        /// it MUST carry Ctrl, Alt or Meta.
        GlobalContext = 0,
        /// Delivered by the message composer accepting a ShortcutOverride
        /// while it has focus. Modifier-less is still refused (a bare letter
        /// would be unreachable — typing it would insert it), but these do
        /// not fight Global entries; they shadow them.
        EditorContext = 1,
    };
    Q_ENUM(ActionContext)

    enum Roles {
        IdRole = Qt::UserRole + 1,
        CategoryRole,
        DescriptionRole,
        DefaultSequenceRole,
        CurrentSequenceRole,
        IsDefaultRole,
        ContextRole,
        /// Human-readable description of the action this row collides with,
        /// or empty. Non-empty means BOTH actions are dead (fact 2).
        ConflictsWithRole,
        /// Human-readable note when the same sequence is also bound in the
        /// other context. Informational: both actions still work.
        ShadowNoteRole,
    };

    explicit ShortcutRegistry(SettingsManager *settings,
                              QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int conflictCount() const;
    bool anyCustomised() const;

    /// The sequence QML should bind a `Shortcut` to, in PortableText.
    /// Returns an empty string for an unknown id — a `Shortcut` with an
    /// empty sequence is inert, which is the correct failure for a typo in
    /// a binding rather than an assert in a running client.
    Q_INVOKABLE QString sequenceFor(const QString &actionId) const;
    Q_INVOKABLE QString defaultSequenceFor(const QString &actionId) const;
    Q_INVOKABLE QString descriptionFor(const QString &actionId) const;

    /// Why `sequence` may not be bound to `actionId`, or an empty string if
    /// it may. Pure: changes nothing. The page calls this while the user is
    /// still holding the keys down, so it must be cheap and side-effect free.
    Q_INVOKABLE QString validationError(const QString &actionId,
                                        const QString &sequence) const;

    /// Binds if `validationError` is empty. Returns that same reason on
    /// refusal, so a caller may use the single call and never see a
    /// half-applied state. NOTHING is stored on refusal.
    Q_INVOKABLE QString setBinding(const QString &actionId,
                                   const QString &sequence);

    Q_INVOKABLE void resetToDefault(const QString &actionId);
    Q_INVOKABLE void resetAll();

    /// True when `sequence` belongs to one of the deliberately hard-coded
    /// keys the model cannot show as a row (Escape, Alt+V, the message
    /// menu's accelerators). Exposed so the capture control can explain the
    /// refusal in the same words the model would.
    Q_INVOKABLE bool isReserved(const QString &sequence) const;
    Q_INVOKABLE QString reservedOwner(const QString &sequence) const;

    /// Turns one live key press into a storable sequence. QML cannot do this
    /// itself: a KeyEvent carries `key` and `modifiers` as integers and there
    /// is no QML-side mapping from Qt.Key_B to the string "B", so the capture
    /// control hands the raw pair here and QKeySequence does the naming.
    /// Returns an empty string while the user is still holding only
    /// modifiers, which is the state a capture control is in for most of the
    /// time it is open.
    Q_INVOKABLE QString sequenceFromKeyEvent(int key, int modifiers) const;

    /// PortableText round-trip. Returns an empty string when Qt cannot parse
    /// the input at all, which is the caller's signal to refuse rather than
    /// to store something that reads back as nothing.
    Q_INVOKABLE static QString normalize(const QString &sequence);

    /// Re-reads every stored override. Called when the active account
    /// changes: bindings are per account, so the account switch has to be
    /// announced or the new account keeps the old one's keys until restart.
    Q_INVOKABLE void reload();

    /// The distinct category labels, in the order the rows use them.
    Q_INVOKABLE QStringList categories() const;

Q_SIGNALS:
    /// One or more bindings changed. QML binds `Shortcut.sequences` through
    /// sequenceFor(), which is a plain function call and therefore NOT a
    /// dependency Qt can track — so a rebind is only visible if something
    /// re-evaluates. Every QML site pairs the call with a counter read
    /// (`bindingRevision`) for exactly this reason; see qml/ShortcutRow.qml
    /// and the handoff notes.
    void bindingsChanged();
    void conflictCountChanged();
    void anyCustomisedChanged();

public:
    /// Bumped on every change. QML reads it inside the sequence binding so
    /// the binding has a real dependency to re-evaluate on — the same
    /// "resolveTick" pattern the media-cache handlers use, and for the same
    /// reason (a bare function call creates no dependency).
    Q_PROPERTY(int bindingRevision READ bindingRevision NOTIFY bindingsChanged)
    int bindingRevision() const { return m_revision; }

private:
    struct Action {
        QString id;
        QString category;
        QString description;
        QString defaultSequence; // PortableText
        ActionContext context = GlobalContext;
    };

    struct Reserved {
        QString sequence; // PortableText
        QString owner;    // human-readable
    };

    const Action *find(const QString &actionId) const;
    QString storedOverride(const QString &actionId) const;
    QString currentSequence(const Action &action) const;
    QString conflictNote(int row) const;
    QString shadowNote(int row) const;
    void recomputeSummary();
    void announceAll();

    SettingsManager *m_settings = nullptr; // not owned; lifetime = process
    QVector<Action> m_actions;
    QVector<Reserved> m_reserved;
    /// Resolved sequence per row, refreshed by reload()/setBinding(). Cached
    /// because data() is called once per role per row on every repaint and
    /// each miss would otherwise be a QSettings read.
    QVector<QString> m_resolved;
    int m_conflicts = 0;
    bool m_anyCustomised = false;
    int m_revision = 0;
};
