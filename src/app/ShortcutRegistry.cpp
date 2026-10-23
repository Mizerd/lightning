#include "app/ShortcutRegistry.h"

#include "app/SettingsManager.h"

#include <QKeyCombination>
#include <QKeySequence>

namespace {

/// A Global sequence must carry Ctrl, Alt or Meta. Shift alone is not enough:
/// Shift+A types a capital A. Only the first chord is checked, since that is
/// what Qt matches against a plain key press.
bool hasCommandModifier(const QKeySequence &seq)
{
    if (seq.count() <= 0)
        return false;
    const QKeyCombination combo = seq[0];
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
    return mods.testFlag(Qt::ControlModifier) || mods.testFlag(Qt::AltModifier)
           || mods.testFlag(Qt::MetaModifier);
}

/// What a capture control reports while the user is still reaching for the
/// real key. Storing it would produce a Shortcut that fires on Ctrl alone.
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

/// Keys that insert no text, so a Keys handler may take them bare.
bool isNonTextKey(const QKeySequence &seq)
{
    if (seq.count() <= 0)
        return false;
    const int key = seq[0].key();
    return key == Qt::Key_PageUp || key == Qt::Key_PageDown
           || (key >= Qt::Key_F1 && key <= Qt::Key_F35);
}

/// Keys the timeline and the focus chain own. Bare (or Shift-only) they are
/// refused for message selection: Tab/Backtab and F6 move focus, Return/Enter
/// and Space activate or page, the arrows move between rows, Backspace and
/// Escape go back. Binding one would swallow it from every other use.
bool isFocusOrNavigationKey(const QKeySequence &seq)
{
    if (seq.count() <= 0)
        return false;
    const QKeyCombination combo = seq[0];
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
    if (mods.testFlag(Qt::ControlModifier) || mods.testFlag(Qt::AltModifier)
        || mods.testFlag(Qt::MetaModifier))
        return false;
    switch (combo.key()) {
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_F6:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Backspace:
    case Qt::Key_Escape:
    case Qt::Key_Space:
        return true;
    default:
        return false;
    }
}

} // namespace

QString ShortcutRegistry::blockingReservedOwner(const QString &portable,
                                                ActionContext context,
                                                const QString &actionId) const
{
    const QString owner = reservedOwner(portable);
    if (owner.isEmpty())
        return {};
    // A menu's letter accelerator only exists while that menu owns focus; it
    // cannot collide with the timeline's Keys handler. One helper for
    // validationError() and conflictNote(), which once disagreed.
    if (context == MessageSelectionContext
        && (portable == QStringLiteral("R") || portable == QStringLiteral("T")
            || portable == QStringLiteral("E")
            || portable == QStringLiteral("Ctrl+C")))
        return {};
    // Dialog dismissal yields to native Copy of selected query text.
    if (context == DialogContext
        && actionId == QStringLiteral("dialog.close")
        && portable == QStringLiteral("Ctrl+C"))
        return {};
    return owner;
}

bool ShortcutRegistry::contextsClash(ActionContext a, ActionContext b)
{
    // Modal popup handlers cannot run on the main room surface.
    if (a == DialogContext || b == DialogContext)
        return a == b;

    // Selection claims ShortcutOverride only in the focused timeline, leaving
    // Global/Editor bindings available elsewhere (a shadow, not a clash).
    // Shared Timeline actions run in that same timeline, so they still
    // conflict with selection actions: the rule below already says so.
    // Global vs Editor is a designed shadow. A Timeline action has no such
    // arrangement: a Global Shortcut would take its key first, and an Editor
    // format would claim it in the same box.
    return a == b || a == TimelineContext || b == TimelineContext;
}

bool ShortcutRegistry::exclusive(const QString &a, const QString &b) const
{
    for (const auto &p : m_exclusive) {
        if ((p.first == a && p.second == b) || (p.first == b && p.second == a))
            return true;
    }
    return false;
}

ShortcutRegistry::ShortcutRegistry(SettingsManager *settings, QObject *parent)
    : QAbstractListModel(parent)
    , m_settings(settings)
{
    buildTables();
    m_resolved.resize(m_actions.size());
    reload();
}

void ShortcutRegistry::retranslate()
{
    // The tables are built in the constructor, which runs before main.cpp
    // installs the stored language, so every label would otherwise stay in
    // English for the life of the process.
    beginResetModel();
    buildTables();
    endResetModel();
    reload();
}

void ShortcutRegistry::buildTables()
{
    const QString appCat = tr("Application");
    const QString navCat = tr("Navigation");
    const QString viewCat = tr("View");
    const QString roomCat = tr("Conversation");
    const QString composerCat = tr("Message box");
    const QString composeCat = tr("Message formatting");
    const QString callCat = tr("Calls");
    const QString mediaCat = tr("Media");
    const QString dialogCat = tr("Dialogs");

    // Every row must be wired to a QML `Shortcut`: a registry entry whose QML
    // site was never migrated is a shortcut that reports a key and does
    // nothing, which is strictly worse than not offering it.
    m_actions = {
        // ── Application ─────────────────────────────────────────────────
        { QStringLiteral("app.quit"), appCat,
          tr("Quit Lightning"), QStringLiteral("Ctrl+Q"), GlobalContext },
        // Opens Settings or focuses its search field, via two Shortcuts on
        // mutually exclusive gates (MainScreen `app.currentScreen !== 2`,
        // SettingsScreen `root.visible`). Two enabled Shortcuts on one
        // sequence fire neither, so that exclusivity must be preserved.
        // Renamed from `app.settingsSearch`; old overrides fall back to the
        // default.
        { QStringLiteral("app.openSettings"), appCat,
          tr("Open Settings, or focus its search field"),
          QStringLiteral("Ctrl+,"), GlobalContext },
        // Navigates to the rebinding page rather than a separate cheat sheet
        // that could disagree with it.
        { QStringLiteral("app.shortcutsHelp"), appCat,
          tr("Show the keyboard shortcuts"), QStringLiteral("Ctrl+/"),
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
        // Opens the same dialog as Ctrl+Shift+N, on its DM tab.
        { QStringLiteral("nav.newDirectMessage"), navCat,
          tr("Start a direct message"), QStringLiteral("Ctrl+Shift+T"),
          GlobalContext },
        // Browser-style room history; the mouse's Back/Forward buttons do the
        // same (src/app/MouseNavigationFilter.h). Alt+Left/Right is free here;
        // the message box does not bind it (word movement is Ctrl+Left/Right,
        // and on macOS a focused text field still wins the keypress).
        { QStringLiteral("nav.historyBack"), navCat,
          tr("Go back to the previous conversation"), QStringLiteral("Alt+Left"),
          GlobalContext },
        { QStringLiteral("nav.historyForward"), navCat,
          tr("Go forward to the next conversation"), QStringLiteral("Alt+Right"),
          GlobalContext },
        // Not Ctrl+I: that is `composer.italic`, and one more Global/Editor
        // shadow was not worth it.
        { QStringLiteral("nav.activityCenter"), navCat,
          tr("Open the Activity Center"), QStringLiteral("Ctrl+Shift+I"),
          GlobalContext },

        // ── View / shell ────────────────────────────────────────────────
        { QStringLiteral("dialog.previousItem"), dialogCat,
          tr("Select the previous item in Activity or the quick switcher"),
          QStringLiteral("Ctrl+K"), DialogContext },
        { QStringLiteral("dialog.nextItem"), dialogCat,
          tr("Select the next item in Activity or the quick switcher"),
          QStringLiteral("Ctrl+J"), DialogContext },
        { QStringLiteral("dialog.close"), dialogCat,
          tr("Close Activity or the quick switcher"),
          QStringLiteral("Ctrl+C"), DialogContext },

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
        // Next/previous result of the find bar (Ctrl+F), like a browser. They
        // act only while the bar is open (TimelinePane gates on it), which is
        // also what lets find-previous share Ctrl+Shift+G with the GIF picker:
        // that Shortcut is disabled while a find is active (m_exclusive).
        { QStringLiteral("room.findNext"), roomCat,
          tr("Go to the next find result"), QStringLiteral("Ctrl+G"),
          GlobalContext },
        { QStringLiteral("room.findPrevious"), roomCat,
          tr("Go to the previous find result"), QStringLiteral("Ctrl+Shift+G"),
          GlobalContext },
        // Timeline movement. Keys handlers, not Shortcuts, so PgUp/PgDn can be
        // bare; they work from the timeline and from the message box (which
        // keeps them for itself while its caret can still move that way).
        // Home/End stay reserved: in the message box they move the caret.
        { QStringLiteral("timeline.selectLatestMessage"), roomCat,
          tr("Select the latest message"), QStringLiteral("Ctrl+Shift+Space"),
          GlobalContext },
        // Not Alt+<letter>: Option+letter types characters on Mac layouts
        // (German '@'), and Ctrl+Alt is AltGr on Windows. Every other
        // Ctrl+Shift+letter is taken; W is free.
        { QStringLiteral("timeline.resumeMessageSelection"), roomCat,
          tr("Resume message selection in the current view"),
          QStringLiteral("Ctrl+Shift+W"), GlobalContext },
        // Bare keys, nvim style. Safe only because the timeline's Keys
        // handler dispatches them while the timeline itself has focus, so no
        // text field is involved and no Global/Editor binding is shadowed.
        { QStringLiteral("timeline.olderMessage"), roomCat,
          tr("Select the older message"), QStringLiteral("K"),
          MessageSelectionContext },
        { QStringLiteral("timeline.newerMessage"), roomCat,
          tr("Select the newer message"), QStringLiteral("J"),
          MessageSelectionContext },
        { QStringLiteral("timeline.halfPageUp"), roomCat,
          tr("Scroll up half a page without changing selection"),
          QStringLiteral("U"), MessageSelectionContext },
        { QStringLiteral("timeline.halfPageDown"), roomCat,
          tr("Scroll down half a page without changing selection"),
          QStringLiteral("D"), MessageSelectionContext },
        { QStringLiteral("timeline.reply"), roomCat,
          tr("Reply to the selected message"), QStringLiteral("R"),
          MessageSelectionContext },
        // The context menu's other actions, on the selected message. Not D:
        // that is half-page down. Delete confirms exactly as the menu does.
        { QStringLiteral("timeline.forwardMessage"), roomCat,
          tr("Forward the selected message"), QStringLiteral("F"),
          MessageSelectionContext },
        { QStringLiteral("timeline.deleteMessage"), roomCat,
          tr("Delete the selected message"), QStringLiteral("Delete"),
          MessageSelectionContext },
        { QStringLiteral("timeline.copyMessageText"), roomCat,
          tr("Copy the selected message's text"), QStringLiteral("Ctrl+C"),
          MessageSelectionContext },
        { QStringLiteral("timeline.copyMessageLink"), roomCat,
          tr("Copy the selected message's link"), QStringLiteral("C"),
          MessageSelectionContext },
        { QStringLiteral("timeline.pageUp"), roomCat,
          tr("Scroll the conversation up by a page"), QStringLiteral("PgUp"),
          TimelineContext },
        { QStringLiteral("timeline.pageDown"), roomCat,
          tr("Scroll the conversation down by a page"),
          QStringLiteral("PgDown"), TimelineContext },
        { QStringLiteral("timeline.jumpToStart"), roomCat,
          tr("Jump to the beginning of the conversation"),
          QStringLiteral("Ctrl+Up"), TimelineContext },
        { QStringLiteral("timeline.jumpToEnd"), roomCat,
          tr("Jump to the latest message"), QStringLiteral("Ctrl+Down"),
          TimelineContext },
        { QStringLiteral("room.markRead"), roomCat,
          tr("Mark the open conversation as read"),
          QStringLiteral("Ctrl+Shift+M"), GlobalContext },
        // Shift+Esc is unavailable (Esc is reserved; Shift alone is not a
        // command modifier). Note Ctrl+Shift+A is QKeySequence::Deselect on
        // X11 and this Shortcut pre-empts it in text fields; accepted since
        // the row is rebindable.
        //
        // markAllRoomsRead() returns a count so a caller can tell "nothing
        // was unread" from "it worked".
        { QStringLiteral("room.markAllRead"), roomCat,
          tr("Mark every conversation as read"), QStringLiteral("Ctrl+Shift+A"),
          GlobalContext },
        // Room-scoped, unlike Discord's message-scoped Alt+Enter.
        //
        // Avoid Ctrl+Alt defaults: Windows delivers AltGr as Ctrl+Alt (German
        // reaches µ with AltGr+M, Polish uses many letters), and on macOS
        // Ctrl+Alt becomes Cmd+Option, which collides with system chords such
        // as "Hide Others". `call.returnToCall` is still Ctrl+Alt+A because
        // moving a shipped default rebinds every existing install.
        { QStringLiteral("room.markUnread"), roomCat,
          tr("Mark the open conversation as unread"),
          QStringLiteral("Ctrl+Shift+R"), GlobalContext },
        // Deliberate shadow: inside the message box Ctrl+U is still Underline
        // (`composer.underline`), like the Ctrl+B/Bold pair.
        { QStringLiteral("room.togglePeople"), roomCat,
          tr("Show or hide the people in this conversation"),
          QStringLiteral("Ctrl+U"), GlobalContext },
        // Not Ctrl+P, which is Print everywhere else.
        { QStringLiteral("room.togglePinned"), roomCat,
          tr("Show or hide pinned messages"), QStringLiteral("Ctrl+Shift+P"),
          GlobalContext },

        // ── Calls ───────────────────────────────────────────────────────
        //
        // Global so mute works while focus is elsewhere. Discord's
        // Ctrl+Shift+M/D are taken here (room.markRead, and the reserved
        // screenshot-demo controls), so these use U (unmute) and H (hear).
        // Inert when no call is running.
        { QStringLiteral("call.toggleMute"), callCat,
          tr("Mute or unmute the microphone"), QStringLiteral("Ctrl+Shift+U"),
          GlobalContext },
        { QStringLiteral("call.toggleDeafen"), callCat,
          tr("Deafen or undeafen"), QStringLiteral("Ctrl+Shift+H"),
          GlobalContext },
        // Camera exists only on the MatrixRTC lane; the legacy 1:1 lane is
        // audio-only, so the handler reports that rather than falling back.
        { QStringLiteral("call.toggleCamera"), callCat,
          tr("Turn the camera on or off"), QStringLiteral("Ctrl+Shift+V"),
          GlobalContext },
        // Returns to the call's room from anywhere, including Settings.
        { QStringLiteral("call.returnToCall"), callCat,
          tr("Return to the active call"), QStringLiteral("Ctrl+Alt+A"),
          GlobalContext },
        // Not Discord's Ctrl+', which is a dead key on several European
        // layouts.
        //
        // Gated, since it starts something: the gate is the timeline call
        // button's own expression in qml/MainScreen.qml, not canStartCall()
        // alone, which would let this key tear down a live call.
        { QStringLiteral("call.startCall"), callCat,
          tr("Start a call in this conversation"), QStringLiteral("Ctrl+Shift+C"),
          GlobalContext },
        // No mnemonic on purpose: Ctrl+Alt+H is macOS "Hide Others",
        // Ctrl+Shift+H is deafen, and Ctrl+Shift+W is the close-window reflex.
        // Leaving is irreversible, so it gets a chord nobody presses by habit.
        { QStringLiteral("call.leave"), callCat,
          tr("Leave the call"), QStringLiteral("Ctrl+Shift+Y"), GlobalContext },
        // Opens the portal or source picker, like the call bar's button; it
        // never starts sharing the desktop on its own.
        { QStringLiteral("call.toggleScreenShare"), callCat,
          tr("Share your screen, or stop sharing"),
          QStringLiteral("Ctrl+Shift+S"), GlobalContext },

        // ── Media ───────────────────────────────────────────────────────
        //
        // Keyboard access to the voice/audio player from anywhere: focus
        // lands on its play/pause (the docked strip's, the clip's row's) or
        // on the popped-out card, so Space plays or pauses and Tab walks its
        // controls. Inert while nothing is loaded. Not Ctrl+Alt (AltGr and
        // macOS, see room.markUnread); every other Ctrl+Shift letter that
        // reads as "player" is taken (P pinned, S share, M mark read), and
        // J is free in every context and in the reserved table.
        { QStringLiteral("media.focusPlayer"), mediaCat,
          tr("Focus the audio player"), QStringLiteral("Ctrl+Shift+J"),
          GlobalContext },

        // ── Message box surfaces (Global context) ───────────────────────
        //
        // Global, not Editor: Editor rows are routed through applyFormat() by
        // action id, and opening a picker is not a format. Ctrl+E is
        // `composer.code` and Ctrl+Shift+U is `call.toggleMute`, hence the
        // non-Discord keys.
        { QStringLiteral("composer.emojiPicker"), composerCat,
          tr("Open the emoji picker"), QStringLiteral("Ctrl+Shift+E"),
          GlobalContext },
        { QStringLiteral("composer.gifPicker"), composerCat,
          tr("Open the GIF and sticker picker"), QStringLiteral("Ctrl+Shift+G"),
          GlobalContext },
        { QStringLiteral("composer.attach"), composerCat,
          tr("Attach files"), QStringLiteral("Ctrl+Shift+O"), GlobalContext },

        // ── Message formatting (Editor context) ─────────────────────────
        // These drive the composer's applyFormat(). Ctrl+B and Ctrl+I shadow
        // the two panel toggles (see the header). Ctrl+S is not the
        // strikethrough default because it means Save everywhere else.
        { QStringLiteral("composer.focus"), composerCat,
          tr("Focus the message box"), QStringLiteral("Ctrl+L"), GlobalContext },

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
        // Underline has no markdown form, so it is a no-op in markdown mode.
        // Link is not Ctrl+K (the quick switcher); Ctrl+Shift+K is taken too.
        { QStringLiteral("composer.underline"), composeCat,
          tr("Underline"), QStringLiteral("Ctrl+U"), EditorContext },
        { QStringLiteral("composer.link"), composeCat,
          tr("Link"), QStringLiteral("Ctrl+Shift+L"), EditorContext },
    };

    // Hard-coded sequences with no row. Conflict detection must know them,
    // because two enabled Shortcuts on one sequence fire neither, whatever
    // declared them.
    m_reserved = {
        { QStringLiteral("Esc"),
          tr("closing the find bar, room information, a thread, Settings "
             "or the open conversation") },
        { QStringLiteral("Alt+V"),
          tr("the emoji picker's skin-tone selector") },
        { QStringLiteral("Ctrl+C"), tr("the message menu's Copy accelerator") },
        { QStringLiteral("R"), tr("the message menu's Reply accelerator") },
        { QStringLiteral("T"), tr("the message menu's Thread accelerator") },
        { QStringLiteral("E"), tr("the message menu's Edit accelerator") },
        { QStringLiteral("Ctrl+Shift+D"), tr("the screenshot-demo controls") },
        // Not Shortcuts: TimelinePane handles these in Keys.onPressed, which
        // a Shortcut would pre-empt.
        { QStringLiteral("Space"), tr("paging the timeline and the media grids") },
        // Plain PgUp/PgDn are the timeline.pageUp/pageDown rows now.
        { QStringLiteral("Shift+PgUp"),
          tr("jumping to the oldest unread message") },
        { QStringLiteral("Home"), tr("jumping to the earliest loaded message") },
        { QStringLiteral("End"), tr("jumping to the latest message") },
    };
    m_exclusive = {
        { QStringLiteral("room.findPrevious"),
          QStringLiteral("composer.gifPicker") },
    };
    // Normalize once so "Esc" and "Escape" compare equal.
    for (Reserved &r : m_reserved) {
        const QString portable = normalize(r.sequence);
        if (!portable.isEmpty())
            r.sequence = portable;
    }
}

int ShortcutRegistry::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_actions.size());
}

QHash<int, QByteArray> ShortcutRegistry::roleNames() const
{
    // Role names are prefixed because ShortcutRow declares properties named
    // actionId, description, currentSequence etc., and a component may not
    // redeclare them.
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
        // Normalized so this agrees with defaultSequenceFor() and Reset.
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
    // Keypad/GroupSwitch describe which physical key was used; keeping them
    // would bind only the numeric keypad.
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
    // An unparseable stored value falls back to the default: an inert
    // Shortcut would fail with no visible reason.
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
    // m_resolved holds the current binding, so a rebound action matches at
    // its new sequence only.
    for (int i = 0; i < m_actions.size(); ++i) {
        if (m_actions.at(i).context == EditorContext
            && m_resolved.at(i) == seq)
            return m_actions.at(i).id;
    }
    return {};
}

QString ShortcutRegistry::timelineActionForKey(int key, int modifiers) const
{
    const QString seq = sequenceFromKeyEvent(key, modifiers);
    if (seq.isEmpty())
        return {};
    for (int i = 0; i < m_actions.size(); ++i) {
        if (m_actions.at(i).context == TimelineContext
            && m_resolved.at(i) == seq)
            return m_actions.at(i).id;
    }
    return {};
}

QString ShortcutRegistry::messageSelectionActionForKey(int key, int modifiers) const
{
    const QString seq = sequenceFromKeyEvent(key, modifiers);
    if (seq.isEmpty())
        return {};
    for (int i = 0; i < m_actions.size(); ++i) {
        if (m_actions.at(i).context == MessageSelectionContext
            && m_resolved.at(i) == seq)
            return m_actions.at(i).id;
    }
    return {};
}

QString ShortcutRegistry::dialogActionForKey(int key, int modifiers,
                                            bool copyingSelectedText) const
{
    const QString seq = sequenceFromKeyEvent(key, modifiers);
    if (seq.isEmpty())
        return {};
    if (copyingSelectedText)
        return {};
    for (int i = 0; i < m_actions.size(); ++i) {
        if (m_actions.at(i).context == DialogContext && m_resolved.at(i) == seq)
            return m_actions.at(i).id;
    }
    return {};
}

QString ShortcutRegistry::editorOverrideFor(int key, int modifiers,
                                            bool hasSelection) const
{
    const QString action = editorActionForKey(key, modifiers);
    if (action.isEmpty() || hasSelection)
        return action;
    const QString seq = sequenceFromKeyEvent(key, modifiers);
    for (int i = 0; i < m_actions.size(); ++i) {
        if (m_actions.at(i).context == GlobalContext
            && m_resolved.at(i) == seq)
            return {};
    }
    return action;
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

    // A modifier-less sequence is refused for both contexts: globally it
    // takes the key from every text field, and in the editor typing it
    // inserts the character.
    // A Timeline action is delivered by a Keys handler, so a key that types
    // nothing (PgUp, PgDn, F-keys) may stay bare.
    const bool selectionOnly = action->context == MessageSelectionContext;
    const bool dialogOnly = action->context == DialogContext;
    if (dialogOnly && seq.count() != 1)
        return tr("Use a single key combination for dialog navigation.");

    if (selectionOnly && seq.count() != 1)
        return tr("Use a single key combination for message selection.");
    const bool bareOk = selectionOnly
                       || (action->context == TimelineContext && isNonTextKey(seq));
    if (!hasCommandModifier(seq) && !bareOk) {
        if (action->context == TimelineContext) {
            return tr("Use Ctrl, Alt or Super, or PgUp, PgDn or an F-key. "
                      "Any other bare key is a character you type in the "
                      "message box.");
        }
        if (dialogOnly)
            return tr("Use Ctrl, Alt or Super. Dialog shortcuts must leave "
                      "ordinary typing and navigation keys available.");
        return action->context == GlobalContext
                   ? tr("Use Ctrl, Alt or Super. A shortcut without one of "
                        "those is taken before any text field sees the key, "
                        "so it would stop you typing that character anywhere "
                        "in Lightning.")
                   : tr("Use Ctrl, Alt or Super. Without one, typing the "
                        "character in the message box is all that would "
                        "happen.");
    }

    if (selectionOnly && isFocusOrNavigationKey(seq)) {
        return tr("%1 moves focus or navigates the conversation, so it "
                  "cannot be used to select messages.")
            .arg(portable);
    }

    const QString owner = blockingReservedOwner(portable, action->context, actionId);
    if (!owner.isEmpty()) {
        return tr("%1 is already used for %2. Two shortcuts on one key make "
                  "Qt fire neither of them.")
            .arg(portable, owner);
    }

    for (int i = 0; i < m_actions.size(); ++i) {
        const Action &other = m_actions.at(i);
        if (other.id == actionId)
            continue;
        // Cross-context is a shadow, not a conflict; this is what lets
        // Ctrl+B be both Bold and the panel toggle.
        if (!contextsClash(other.context, action->context))
            continue;
        if (exclusive(other.id, actionId))
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
        // Binding a sequence back to its default clears the override, so the
        // account keeps following future default changes.
        m_settings->clearShortcutSequence(actionId);
    } else {
        m_settings->setShortcutSequence(actionId, portable);
    }
    reload();
    return {};
}

QString ShortcutRegistry::resetToDefault(const QString &actionId)
{
    const Action *action = find(actionId);
    if (!m_settings || !action)
        return {};
    const QString error = validationError(actionId, action->defaultSequence);
    if (!error.isEmpty())
        return error; // keep the working override rather than make a clash
    m_settings->clearShortcutSequence(actionId);
    reload();
    return {};
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
    const QString owner = blockingReservedOwner(mine, a.context, a.id);
    if (!owner.isEmpty())
        return tr("Also used for %1 — neither will work.").arg(owner);
    for (int i = 0; i < m_actions.size(); ++i) {
        if (i == row)
            continue;
        if (!contextsClash(m_actions.at(i).context, a.context))
            continue;
        if (exclusive(m_actions.at(i).id, a.id))
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
    if (a.context == DialogContext)
        return tr("Only while Activity or the quick switcher is open. "
                  "Copying selected text takes priority over closing.");
    const QString mine = m_resolved.at(row);
    if (mine.isEmpty())
        return {};
    QStringList notes;
    for (int i = 0; i < m_actions.size(); ++i) {
        if (i == row)
            continue;
        const Action &other = m_actions.at(i);
        if (contextsClash(other.context, a.context))
            continue;
        if (m_resolved.at(i) != mine)
            continue;
        if (other.context == DialogContext)
            notes.append(tr("While Activity or the quick switcher is open, "
                            "“%1” runs instead.")
                             .arg(other.description));
        else if (a.context == MessageSelectionContext)
            notes.append(tr("While the timeline has focus this runs instead of “%1”.")
                             .arg(other.description));
        else if (other.context == MessageSelectionContext)
            notes.append(tr("While the timeline has focus, “%1” runs instead.")
                             .arg(other.description));
        // The composer yields to a Global twin when nothing is selected
        // (editorOverrideFor), so the editor action wins only over a
        // selection. Say that, not "while the box has focus".
        else if (a.context == EditorContext && other.context == GlobalContext)
            notes.append(tr("With text selected in the message box this runs "
                            "instead of “%1”; with nothing selected, “%1” runs.")
                             .arg(other.description));
        else if (a.context == GlobalContext && other.context == EditorContext)
            notes.append(tr("While text is selected in the message box, “%1” "
                            "runs instead.")
                             .arg(other.description));
        else
            notes.append(a.context == EditorContext
                             ? tr("While the message box has focus this runs instead of "
                                  "“%1”.")
                                   .arg(other.description)
                             : tr("While the message box has focus, “%1” runs "
                                  "instead.")
                                   .arg(other.description));
    }
    return notes.join(QLatin1Char('\n'));
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
    // Emitted explicitly: sequenceFor() is a function call and creates no
    // QML binding dependency, so QML sites re-evaluate via bindingRevision.
    Q_EMIT bindingsChanged();
}
