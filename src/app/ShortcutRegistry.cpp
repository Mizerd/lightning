#include "app/ShortcutRegistry.h"

#include "app/SettingsManager.h"

#include <QKeyCombination>
#include <QKeySequence>

namespace {

/// A sequence is legal for a Global action only if it carries Ctrl, Alt or
/// Meta. Shift alone is NOT enough: Shift+A is how a capital A is typed, and
/// a Shortcut on it would pre-empt every text field in the application
/// (fact 1 in the header). The check inspects the FIRST chord only, which is
/// the one Qt matches against a plain key press.
bool hasCommandModifier(const QKeySequence &seq)
{
    if (seq.count() <= 0)
        return false;
    const QKeyCombination combo = seq[0];
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
    return mods.testFlag(Qt::ControlModifier) || mods.testFlag(Qt::AltModifier)
           || mods.testFlag(Qt::MetaModifier);
}

/// A sequence whose only content is a modifier is what a capture control
/// reports while the user is still reaching for the real key. Storing it
/// would produce a Shortcut that fires on Ctrl alone.
bool isBareModifier(const QKeySequence &seq)
{
    if (seq.count() <= 0)
        return true;
    const int key = seq[0].key();
    return key == Qt::Key_unknown || key == 0 || key == Qt::Key_Control
           || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta
           || key == Qt::Key_AltGr || key == Qt::Key_CapsLock
           || key == Qt::Key_NumLock || key == Qt::Key_ScrollLock;
}

} // namespace

ShortcutRegistry::ShortcutRegistry(SettingsManager *settings, QObject *parent)
    : QAbstractListModel(parent)
    , m_settings(settings)
{
    const QString appCat = tr("Application");
    const QString navCat = tr("Navigation");
    const QString viewCat = tr("View");
    const QString roomCat = tr("Conversation");
    const QString composeCat = tr("Message formatting");
    const QString callCat = tr("Calls");

    // THE SEED LIST IS THE CONTRACT. Every row here must be WIRED to
    // something — a registry entry whose QML site was never migrated is a
    // shortcut that reports a key and does nothing, which is strictly worse
    // than not offering it. When an action is added here, its `Shortcut`
    // must be migrated in the same change.
    m_actions = {
        // ── Application ─────────────────────────────────────────────────
        { QStringLiteral("app.quit"), appCat,
          tr("Quit Lightning"), QStringLiteral("Ctrl+Q"), GlobalContext },
        { QStringLiteral("app.settingsSearch"), appCat,
          tr("Focus the Settings search field"), QStringLiteral("Ctrl+,"),
          GlobalContext },

        // ── Navigation ──────────────────────────────────────────────────
        { QStringLiteral("nav.quickSwitcher"), navCat,
          tr("Open the quick switcher"), QStringLiteral("Ctrl+K"),
          GlobalContext },
        { QStringLiteral("nav.commandMode"), navCat,
          tr("Open the quick switcher in command mode"),
          QStringLiteral("Ctrl+Shift+K"), GlobalContext },
        { QStringLiteral("nav.messageSearch"), navCat,
          tr("Search message history"), QStringLiteral("Ctrl+Shift+F"),
          GlobalContext },
        { QStringLiteral("nav.newConversation"), navCat,
          tr("Create a room or Space"), QStringLiteral("Ctrl+Shift+N"),
          GlobalContext },

        // ── View / shell ────────────────────────────────────────────────
        { QStringLiteral("shell.toggleRoomList"), viewCat,
          tr("Show or hide the conversation list"), QStringLiteral("Ctrl+B"),
          GlobalContext },
        { QStringLiteral("shell.toggleSpacesRail"), viewCat,
          tr("Show or hide the Spaces rail"), QStringLiteral("Ctrl+Shift+B"),
          GlobalContext },
        { QStringLiteral("view.zoomIn"), viewCat,
          tr("Increase interface zoom"), QStringLiteral("Ctrl+="),
          GlobalContext },
        { QStringLiteral("view.zoomOut"), viewCat,
          tr("Decrease interface zoom"), QStringLiteral("Ctrl+-"),
          GlobalContext },
        { QStringLiteral("view.zoomReset"), viewCat,
          tr("Reset interface zoom"), QStringLiteral("Ctrl+0"), GlobalContext },

        // ── Conversation ────────────────────────────────────────────────
        { QStringLiteral("room.find"), roomCat,
          tr("Find in the loaded timeline"), QStringLiteral("Ctrl+F"),
          GlobalContext },
        { QStringLiteral("room.markRead"), roomCat,
          tr("Mark the open conversation as read"),
          QStringLiteral("Ctrl+Shift+M"), GlobalContext },

        // ── Calls ───────────────────────────────────────────────────────
        //
        // GlobalContext, and deliberately so: the whole value of a mute key
        // is that it works while you are doing something else — reading the
        // room, typing a note in another conversation — which is exactly
        // when you need to mute in a hurry. An EditorContext mute would be
        // unreachable at the one moment it matters.
        //
        // Discord's own keys are Ctrl+Shift+M and Ctrl+Shift+D, and NEITHER
        // was available: Ctrl+Shift+M is already room.markRead, and
        // Ctrl+Shift+D is in the RESERVED table below (the screenshot-demo
        // controls). Two actions cannot share a default — validationError()
        // refuses the conflict, and the rebinding page would open on an
        // unresolvable state the first time a user saw it — so both fall
        // back to free keys that keep the mnemonic: U for unmute, H for
        // hear. Anyone who wants Discord's keys can rebind, which is what
        // the rebinding feature is for.
        //
        // They are inert when no call is running, rather than absent: a key
        // that quietly does nothing outside a call is better than one that
        // takes the sequence away from something else while a call is up.
        { QStringLiteral("call.toggleMute"), callCat,
          tr("Mute or unmute the microphone"), QStringLiteral("Ctrl+Shift+U"),
          GlobalContext },
        { QStringLiteral("call.toggleDeafen"), callCat,
          tr("Deafen or undeafen"), QStringLiteral("Ctrl+Shift+H"),
          GlobalContext },

        // ── Message formatting (Editor context) ─────────────────────────
        // These call the composer's existing applyFormat(), which the
        // formatting toolbar already drives — so the key is the only new
        // part. Ctrl+B and Ctrl+I are the conventions every editor uses;
        // they SHADOW the two panel toggles rather than replacing them (see
        // the header). Ctrl+E is Element Web's inline-code key and is free
        // here because Lightning has no sticker picker to open with it.
        // Ctrl+S is deliberately NOT the strikethrough default the way it is
        // elsewhere: on a desktop every other application treats it as Save,
        // and a client that eats Ctrl+S is a client people distrust.
        { QStringLiteral("composer.bold"), composeCat,
          tr("Bold"), QStringLiteral("Ctrl+B"), EditorContext },
        { QStringLiteral("composer.italic"), composeCat,
          tr("Italic"), QStringLiteral("Ctrl+I"), EditorContext },
        { QStringLiteral("composer.strike"), composeCat,
          tr("Strikethrough"), QStringLiteral("Ctrl+Shift+X"), EditorContext },
        { QStringLiteral("composer.code"), composeCat,
          tr("Inline code"), QStringLiteral("Ctrl+E"), EditorContext },
        { QStringLiteral("composer.list"), composeCat,
          tr("Bulleted list"), QStringLiteral("Ctrl+Shift+8"), EditorContext },
        { QStringLiteral("composer.quote"), composeCat,
          tr("Quote"), QStringLiteral("Ctrl+Shift+9"), EditorContext },
        // v0.9 rich composer. Underline has no markdown form, so in markdown
        // mode this is a no-op; in rich mode it is the standard Ctrl+U.
        // Link is NOT Ctrl+K: that is the quick switcher everywhere in this
        // app, and Ctrl+Shift+K is taken globally too — Ctrl+Shift+L is the
        // free editor-context sequence.
        { QStringLiteral("composer.underline"), composeCat,
          tr("Underline"), QStringLiteral("Ctrl+U"), EditorContext },
        { QStringLiteral("composer.link"), composeCat,
          tr("Link"), QStringLiteral("Ctrl+Shift+L"), EditorContext },
    };

    // SEQUENCES THAT STAY HARD-CODED. They are not rows — none of them is a
    // sensible thing to rebind — but conflict detection has to KNOW about
    // them, because fact 2 does not care whether a Shortcut came from this
    // model or from a QML literal: two enabled Shortcuts on one sequence
    // fire NEITHER. Without this list the page would happily accept Escape
    // for "Quit" and quietly break closing a dialog at the same time.
    m_reserved = {
        { QStringLiteral("Esc"),
          tr("closing the find bar, room information, a thread or Settings") },
        { QStringLiteral("Alt+V"),
          tr("the emoji picker's skin-tone selector") },
        { QStringLiteral("Ctrl+C"), tr("the message menu's Copy accelerator") },
        { QStringLiteral("R"), tr("the message menu's Reply accelerator") },
        { QStringLiteral("T"), tr("the message menu's Thread accelerator") },
        { QStringLiteral("E"), tr("the message menu's Edit accelerator") },
        { QStringLiteral("Ctrl+Shift+D"), tr("the screenshot-demo controls") },
        // Not a Shortcut, but taken all the same: TimelinePane reads these
        // in Keys.onPressed, which a Shortcut would pre-empt (fact 1). They
        // are the exact keys commit 4c2317f was about.
        { QStringLiteral("Space"), tr("paging the timeline and the media grids") },
        { QStringLiteral("PgUp"), tr("paging the timeline") },
        { QStringLiteral("PgDown"), tr("paging the timeline") },
        { QStringLiteral("Home"), tr("jumping to the earliest loaded message") },
        { QStringLiteral("End"), tr("jumping to the latest message") },
    };
    // Normalize the reserved list ONCE, through the same round-trip every
    // candidate goes through — otherwise "Esc" and "Escape" compare unequal
    // and the whole list silently matches nothing.
    for (Reserved &r : m_reserved) {
        const QString portable = normalize(r.sequence);
        if (!portable.isEmpty())
            r.sequence = portable;
    }

    m_resolved.resize(m_actions.size());
    reload();
}

int ShortcutRegistry::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_actions.size());
}

QHash<int, QByteArray> ShortcutRegistry::roleNames() const
{
    // ROLE NAMES ARE DELIBERATELY PREFIXED. The delegate is a ShortcutRow,
    // which declares properties of its own called actionId, description,
    // currentSequence and so on — and a QML component instance may not
    // redeclare a property its type already has. Unprefixed role names would
    // therefore either collide outright or silently shadow the component's
    // own properties, which is the harder bug of the two to see.
    return {
        { IdRole, "shortcutId" },
        { CategoryRole, "shortcutCategory" },
        { DescriptionRole, "shortcutDescription" },
        { DefaultSequenceRole, "shortcutDefault" },
        { CurrentSequenceRole, "shortcutCurrent" },
        { IsDefaultRole, "shortcutIsDefault" },
        { ContextRole, "shortcutContext" },
        { ConflictsWithRole, "shortcutConflict" },
        { ShadowNoteRole, "shortcutShadow" },
    };
}

QVariant ShortcutRegistry::data(const QModelIndex &index, int role) const
{
    const int row = index.row();
    if (row < 0 || row >= m_actions.size())
        return {};
    const Action &a = m_actions.at(row);
    switch (role) {
    case IdRole:
        return a.id;
    case CategoryRole:
        return a.category;
    case DescriptionRole:
        return a.description;
    case DefaultSequenceRole:
        // NORMALIZED, like every other sequence that leaves this class. The
        // seed list is written in canonical form already, but a role that
        // returns the raw literal while defaultSequenceFor() returns the
        // round-tripped one is two answers to one question, and the "Reset
        // to %1" label would eventually disagree with what Reset does.
        return normalize(a.defaultSequence);
    case CurrentSequenceRole:
        return m_resolved.at(row);
    case IsDefaultRole:
        return m_resolved.at(row) == normalize(a.defaultSequence);
    case ContextRole:
        return static_cast<int>(a.context);
    case ConflictsWithRole:
        return conflictNote(row);
    case ShadowNoteRole:
        return shadowNote(row);
    default:
        return {};
    }
}

int ShortcutRegistry::conflictCount() const { return m_conflicts; }

bool ShortcutRegistry::anyCustomised() const { return m_anyCustomised; }

const ShortcutRegistry::Action *ShortcutRegistry::find(const QString &actionId) const
{
    for (const Action &a : m_actions) {
        if (a.id == actionId)
            return &a;
    }
    return nullptr;
}

QString ShortcutRegistry::normalize(const QString &sequence)
{
    const QString trimmed = sequence.trimmed();
    if (trimmed.isEmpty())
        return {};
    const QKeySequence seq =
        QKeySequence::fromString(trimmed, QKeySequence::PortableText);
    if (seq.isEmpty() || isBareModifier(seq))
        return {};
    return seq.toString(QKeySequence::PortableText);
}

QString ShortcutRegistry::sequenceFromKeyEvent(int key, int modifiers) const
{
    // KeypadModifier and GroupSwitchModifier describe WHICH physical key
    // produced the character, not what the user meant to bind — leaving them
    // in would store a sequence that only matches the numeric keypad.
    const auto mods = static_cast<Qt::KeyboardModifiers>(modifiers)
                      & ~Qt::KeypadModifier & ~Qt::GroupSwitchModifier;
    const QKeySequence seq(QKeyCombination(mods, static_cast<Qt::Key>(key)));
    if (seq.isEmpty() || isBareModifier(seq))
        return {};
    return seq.toString(QKeySequence::PortableText);
}

QString ShortcutRegistry::storedOverride(const QString &actionId) const
{
    if (!m_settings)
        return {};
    // An unparseable stored value is IGNORED rather than surfaced. A
    // Shortcut bound to an unparseable string is inert and looks exactly
    // like a shortcut that simply does not work, with nothing anywhere
    // saying why — so a corrupted or hand-edited settings file degrades to
    // the DEFAULT, which is a state the user can see and reason about.
    return normalize(m_settings->shortcutSequence(actionId));
}

QString ShortcutRegistry::currentSequence(const Action &action) const
{
    const QString stored = storedOverride(action.id);
    if (!stored.isEmpty())
        return stored;
    return normalize(action.defaultSequence);
}

QString ShortcutRegistry::sequenceFor(const QString &actionId) const
{
    for (int i = 0; i < m_actions.size(); ++i) {
        if (m_actions.at(i).id == actionId)
            return m_resolved.at(i);
    }
    return {};
}

QString ShortcutRegistry::editorActionForKey(int key, int modifiers) const
{
    const QString seq = sequenceFromKeyEvent(key, modifiers);
    if (seq.isEmpty())
        return {};
    // m_resolved holds the CURRENT binding, so a rebound Bold is matched at
    // its new sequence and a stale default is never matched at its old one.
    for (int i = 0; i < m_actions.size(); ++i) {
        if (m_actions.at(i).context == EditorContext
            && m_resolved.at(i) == seq)
            return m_actions.at(i).id;
    }
    return {};
}

QString ShortcutRegistry::defaultSequenceFor(const QString &actionId) const
{
    const Action *a = find(actionId);
    return a ? normalize(a->defaultSequence) : QString();
}

QString ShortcutRegistry::descriptionFor(const QString &actionId) const
{
    const Action *a = find(actionId);
    return a ? a->description : QString();
}

bool ShortcutRegistry::isReserved(const QString &sequence) const
{
    return !reservedOwner(sequence).isEmpty();
}

QString ShortcutRegistry::reservedOwner(const QString &sequence) const
{
    const QString portable = normalize(sequence);
    if (portable.isEmpty())
        return {};
    for (const Reserved &r : m_reserved) {
        if (r.sequence == portable)
            return r.owner;
    }
    return {};
}

QString ShortcutRegistry::validationError(const QString &actionId,
                                          const QString &sequence) const
{
    const Action *action = find(actionId);
    if (!action)
        return tr("Unknown action.");

    const QString portable = normalize(sequence);
    if (portable.isEmpty())
        return tr("That is not a key combination Lightning can store.");

    const QKeySequence seq =
        QKeySequence::fromString(portable, QKeySequence::PortableText);

    // Fact 1. A modifier-less global shortcut takes that key away from every
    // text field in the application — including the one you would use to
    // undo it. Refused for BOTH contexts: an Editor action bound to a bare
    // letter would be unreachable anyway, because typing the letter is what
    // the message box is for.
    if (!hasCommandModifier(seq)) {
        return action->context == GlobalContext
                   ? tr("Use Ctrl, Alt or Super. A shortcut without one of "
                        "those is taken before any text field sees the key, "
                        "so it would stop you typing that character anywhere "
                        "in Lightning.")
                   : tr("Use Ctrl, Alt or Super. Without one, typing the "
                        "character in the message box is all that would "
                        "happen.");
    }

    const QString owner = reservedOwner(portable);
    if (!owner.isEmpty()) {
        return tr("%1 is already used for %2. Two shortcuts on one key make "
                  "Qt fire neither of them.")
            .arg(portable, owner);
    }

    for (int i = 0; i < m_actions.size(); ++i) {
        const Action &other = m_actions.at(i);
        if (other.id == actionId)
            continue;
        // Cross-context is a SHADOW, not a conflict — the composer takes the
        // key while it has focus and the global action keeps it otherwise.
        // Reporting it as a conflict would forbid exactly the arrangement
        // that makes Ctrl+B work for both Bold and the panel toggle.
        if (other.context != action->context)
            continue;
        if (m_resolved.at(i) == portable) {
            return tr("%1 is already used for “%2”. Two shortcuts on "
                      "one key make Qt fire neither of them.")
                .arg(portable, other.description);
        }
    }
    return {};
}

QString ShortcutRegistry::setBinding(const QString &actionId,
                                     const QString &sequence)
{
    const QString error = validationError(actionId, sequence);
    if (!error.isEmpty())
        return error; // NOTHING is written on refusal.

    const Action *action = find(actionId);
    if (!action || !m_settings)
        return tr("Unknown action.");

    const QString portable = normalize(sequence);
    if (portable == normalize(action->defaultSequence)) {
        // Setting a binding back to its own default CLEARS the override
        // rather than storing it. Otherwise the account would carry a
        // pinned copy of today's default and would not follow a future
        // change to it — and "Reset" would appear to do nothing.
        m_settings->clearShortcutSequence(actionId);
    } else {
        m_settings->setShortcutSequence(actionId, portable);
    }
    reload();
    return {};
}

void ShortcutRegistry::resetToDefault(const QString &actionId)
{
    if (!m_settings || !find(actionId))
        return;
    m_settings->clearShortcutSequence(actionId);
    reload();
}

void ShortcutRegistry::resetAll()
{
    if (!m_settings)
        return;
    for (const Action &a : m_actions)
        m_settings->clearShortcutSequence(a.id);
    reload();
}

QStringList ShortcutRegistry::categories() const
{
    QStringList out;
    for (const Action &a : m_actions) {
        if (!out.contains(a.category))
            out.append(a.category);
    }
    return out;
}

QString ShortcutRegistry::conflictNote(int row) const
{
    if (row < 0 || row >= m_actions.size())
        return {};
    const Action &a = m_actions.at(row);
    const QString mine = m_resolved.at(row);
    if (mine.isEmpty())
        return {};
    const QString owner = reservedOwner(mine);
    if (!owner.isEmpty())
        return tr("Also used for %1 — neither will work.").arg(owner);
    for (int i = 0; i < m_actions.size(); ++i) {
        if (i == row)
            continue;
        if (m_actions.at(i).context != a.context)
            continue;
        if (m_resolved.at(i) == mine) {
            return tr("Also used for “%1” — neither will work.")
                .arg(m_actions.at(i).description);
        }
    }
    return {};
}

QString ShortcutRegistry::shadowNote(int row) const
{
    if (row < 0 || row >= m_actions.size())
        return {};
    const Action &a = m_actions.at(row);
    const QString mine = m_resolved.at(row);
    if (mine.isEmpty())
        return {};
    for (int i = 0; i < m_actions.size(); ++i) {
        if (i == row)
            continue;
        const Action &other = m_actions.at(i);
        if (other.context == a.context)
            continue;
        if (m_resolved.at(i) != mine)
            continue;
        return a.context == EditorContext
                   ? tr("While the message box has focus this runs instead of "
                        "“%1”.")
                         .arg(other.description)
                   : tr("While the message box has focus, “%1” runs "
                        "instead.")
                         .arg(other.description);
    }
    return {};
}

void ShortcutRegistry::recomputeSummary()
{
    int conflicts = 0;
    bool customised = false;
    for (int i = 0; i < m_actions.size(); ++i) {
        if (!conflictNote(i).isEmpty())
            ++conflicts;
        if (m_resolved.at(i) != normalize(m_actions.at(i).defaultSequence))
            customised = true;
    }
    if (conflicts != m_conflicts) {
        m_conflicts = conflicts;
        Q_EMIT conflictCountChanged();
    }
    if (customised != m_anyCustomised) {
        m_anyCustomised = customised;
        Q_EMIT anyCustomisedChanged();
    }
}

void ShortcutRegistry::announceAll()
{
    if (m_actions.isEmpty())
        return;
    Q_EMIT dataChanged(index(0), index(static_cast<int>(m_actions.size()) - 1));
}

void ShortcutRegistry::reload()
{
    m_resolved.resize(m_actions.size());
    for (int i = 0; i < m_actions.size(); ++i)
        m_resolved[i] = currentSequence(m_actions.at(i));
    recomputeSummary();
    ++m_revision;
    announceAll();
    // ANNOUNCED EXPLICITLY, not left to dataChanged. QML binds a Shortcut's
    // sequence through sequenceFor(), which is a function call and therefore
    // creates no binding dependency — the same trap the media-cache handlers
    // hit when they assigned Image.source imperatively and destroyed the
    // binding. Every QML site reads bindingRevision inside the binding, and
    // this signal is what makes that read fire.
    Q_EMIT bindingsChanged();
}
