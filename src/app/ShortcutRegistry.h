#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

class SettingsManager;

/// Every rebindable keyboard shortcut in Lightning, in one place.
///
/// Two Qt behaviours shape the rules below:
///
///  1. A `Shortcut` is consumed before the focused item sees the key. So each
///     entry carries a context, and a Global entry must carry Ctrl, Alt or
///     Meta: a bare letter bound globally would be taken from every text
///     field, including the one needed to undo the binding.
///
///  2. Two enabled `Shortcut`s on one sequence are ambiguous and Qt fires
///     neither. So `setBinding` refuses a conflicting sequence instead of
///     storing it, and the reserved list covers the hard-coded sequences
///     that have no row of their own.
///
/// Global vs Editor on the same sequence is a shadow, not a conflict: the
/// composer accepts ShortcutOverride before shortcut dispatch, so the Editor
/// action wins while it has focus and the Global action works elsewhere
/// (Ctrl+B is Bold while typing, "toggle the room list" otherwise). Rows
/// describe this via `shadowedBy` / `shadows`.
///
/// Persistence is per account with a global fallback, like other appearance
/// settings (SettingsManager::appearanceValue). Sequences are stored as
/// QKeySequence::PortableText; an unparseable stored value is ignored and the
/// default applies.
class ShortcutRegistry : public QAbstractListModel
{
    Q_OBJECT
    /// Rows reporting a hard conflict. setBinding never creates one, but a
    /// hand-edited or newer-build settings file can.
    Q_PROPERTY(int conflictCount READ conflictCount NOTIFY conflictCountChanged)
    /// True when any action is off its default sequence ("Reset all").
    Q_PROPERTY(bool anyCustomised READ anyCustomised NOTIFY anyCustomisedChanged)

public:
    /// Where an action is dispatched from; decides which sequences are legal.
    enum ActionContext {
        /// A window/application `Shortcut`. Pre-empts the focused item, so
        /// it must carry Ctrl, Alt or Meta.
        GlobalContext = 0,
        /// Delivered by the composer accepting ShortcutOverride while it has
        /// focus. Modifier-less is still refused (typing would insert it);
        /// these shadow Global entries rather than conflict with them.
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
        /// Description of the action this row collides with, or empty.
        /// Non-empty means both actions are dead.
        ConflictsWithRole,
        /// Note when the same sequence is bound in the other context.
        /// Informational: both actions still work.
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
    /// Empty for an unknown id, which leaves the `Shortcut` inert.
    Q_INVOKABLE QString sequenceFor(const QString &actionId) const;
    Q_INVOKABLE QString defaultSequenceFor(const QString &actionId) const;
    Q_INVOKABLE QString descriptionFor(const QString &actionId) const;

    /// Why `sequence` may not be bound to `actionId`, or empty if it may.
    /// Side-effect free; called while the user is still holding the keys.
    Q_INVOKABLE QString validationError(const QString &actionId,
                                        const QString &sequence) const;

    /// Binds if `validationError` is empty, otherwise returns that reason.
    /// Nothing is stored on refusal.
    Q_INVOKABLE QString setBinding(const QString &actionId,
                                   const QString &sequence);

    Q_INVOKABLE void resetToDefault(const QString &actionId);
    Q_INVOKABLE void resetAll();

    /// True when `sequence` belongs to a hard-coded key with no row (Escape,
    /// Alt+V, the message menu's accelerators).
    Q_INVOKABLE bool isReserved(const QString &sequence) const;
    Q_INVOKABLE QString reservedOwner(const QString &sequence) const;

    /// Turns one key press into a storable sequence. QML has no mapping from
    /// Qt.Key_B to "B", so the capture control hands the raw pair here.
    /// Returns empty while only modifiers are held.
    Q_INVOKABLE QString sequenceFromKeyEvent(int key, int modifiers) const;

    /// The EditorContext action id this key press resolves to, or empty.
    ///
    /// Composers use it both to decide whether to accept ShortcutOverride
    /// (claiming only what they handle, so Ctrl+K and Ctrl+Q still reach the
    /// window) and to apply the format on the key press that follows.
    Q_INVOKABLE QString editorActionForKey(int key, int modifiers) const;

    /// PortableText round-trip. Empty when Qt cannot parse the input, which
    /// the caller must treat as a refusal.
    Q_INVOKABLE static QString normalize(const QString &sequence);

    /// Re-reads every stored override. Called on account switch, since
    /// bindings are per account.
    Q_INVOKABLE void reload();

    /// The distinct category labels, in the order the rows use them.
    Q_INVOKABLE QStringList categories() const;

Q_SIGNALS:
    /// One or more bindings changed. sequenceFor() is a plain call, which QML
    /// cannot track as a dependency, so every QML site also reads
    /// `bindingRevision` (see qml/ShortcutRow.qml).
    void bindingsChanged();
    void conflictCountChanged();
    void anyCustomisedChanged();

public:
    /// Bumped on every change; QML reads it inside the sequence binding to
    /// get a real dependency.
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
    /// Resolved sequence per row. Cached because data() runs per role per row
    /// on every repaint and a miss would be a QSettings read.
    QVector<QString> m_resolved;
    int m_conflicts = 0;
    bool m_anyCustomised = false;
    int m_revision = 0;
};
