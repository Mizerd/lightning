#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QPair>
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
    Q_PROPERTY(int conflictCount READ conflictCount NOTIFY conflictCountChanged)
    Q_PROPERTY(bool anyCustomised READ anyCustomised NOTIFY anyCustomisedChanged)

public:
    enum ActionContext {
        GlobalContext = 0,
        /// Delivered by the composer accepting ShortcutOverride while it has
        /// focus. Modifier-less is still refused (typing would insert it);
        /// these shadow Global entries rather than conflict with them.
        EditorContext = 1,
        TimelineContext = 2,
        /// Only the focused room timeline dispatches these. ShortcutOverride
        /// claims them before Global shortcuts; compose never receives them.
        /// Plain letters are safe here and may shadow Global/Editor bindings.
        MessageSelectionContext = 3,
        DialogContext = 4,
        SettingsContext = 5,
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
        ConflictsWithRole,
        ShadowNoteRole,
    };

    explicit ShortcutRegistry(SettingsManager *settings,
                              QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int conflictCount() const;
    bool anyCustomised() const;

    Q_INVOKABLE QString sequenceFor(const QString &actionId) const;
    Q_INVOKABLE QString defaultSequenceFor(const QString &actionId) const;
    Q_INVOKABLE QString descriptionFor(const QString &actionId) const;

    Q_INVOKABLE QString validationError(const QString &actionId,
                                        const QString &sequence) const;

    Q_INVOKABLE QString setBinding(const QString &actionId,
                                   const QString &sequence);

    Q_INVOKABLE QString resetToDefault(const QString &actionId);
    Q_INVOKABLE void resetAll();

    Q_INVOKABLE bool isReserved(const QString &sequence) const;
    Q_INVOKABLE QString reservedOwner(const QString &sequence) const;

    Q_INVOKABLE QString sequenceFromKeyEvent(int key, int modifiers) const;

    Q_INVOKABLE QString editorActionForKey(int key, int modifiers) const;

    Q_INVOKABLE QString timelineActionForKey(int key, int modifiers) const;

    Q_INVOKABLE QString messageSelectionActionForKey(int key, int modifiers) const;

    Q_INVOKABLE QString dialogActionForKey(int key, int modifiers,
                                          bool copyingSelectedText = false) const;

    QString settingsActionForKey(int key, int modifiers,
                                 bool copyingSelectedText = false) const;

    Q_INVOKABLE QString editorOverrideFor(int key, int modifiers,
                                          bool hasSelection) const;

    Q_INVOKABLE static QString normalize(const QString &sequence);

    Q_INVOKABLE void reload();

    Q_INVOKABLE QStringList categories() const;

    void retranslate();

Q_SIGNALS:
    void bindingsChanged();
    void conflictCountChanged();
    void anyCustomisedChanged();

public:
    Q_PROPERTY(int bindingRevision READ bindingRevision NOTIFY bindingsChanged)
    int bindingRevision() const { return m_revision; }

private:
    struct Action {
        QString id;
        QString category;
        QString description;
        QString defaultSequence;
        ActionContext context = GlobalContext;
    };

    struct Reserved {
        QString sequence;
        QString owner;
    };

    void buildTables();
    const Action *find(const QString &actionId) const;
    QString storedOverride(const QString &actionId) const;
    QString currentSequence(const Action &action) const;
    QString conflictNote(int row) const;
    QString blockingReservedOwner(const QString &portable,
                                  ActionContext context,
                                  const QString &actionId) const;
    QString shadowNote(int row) const;
    void recomputeSummary();
    void announceAll();

    SettingsManager *m_settings = nullptr;
    QVector<Action> m_actions;
    QVector<Reserved> m_reserved;
    QVector<QPair<QString, QString>> m_exclusive;
    bool exclusive(const QString &a, const QString &b) const;
    static bool contextsClash(ActionContext a, ActionContext b);
    static bool actionsClash(const Action &a, const Action &b);
    QVector<QString> m_resolved;
    int m_conflicts = 0;
    bool m_anyCustomised = false;
    int m_revision = 0;
};
