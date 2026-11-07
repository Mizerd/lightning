#include "app/ShortcutRegistry.h"

#include "app/SettingsManager.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyCombination>
#include <QKeyEvent>
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

bool isNonTextKey(const QKeySequence &seq)
{
    if (seq.count() <= 0)
        return false;
    const int key = seq[0].key();
    return key == Qt::Key_PageUp || key == Qt::Key_PageDown
           || (key >= Qt::Key_F1 && key <= Qt::Key_F35);
}

bool isSingleBareKey(const QKeySequence &seq)
{
    return seq.count() == 1
        && (seq[0].keyboardModifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
}

bool isNavigationOrEditingKey(int key)
{
    switch (key) {
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_F6:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Backspace:
    case Qt::Key_Delete:
    case Qt::Key_Insert:
    case Qt::Key_Escape:
    case Qt::Key_Space:
    case Qt::Key_Menu:
    case Qt::Key_Clear:
        return true;
    default:
        return false;
    }
}

// Qt keys below Key_Escape are Unicode characters. Text fields accept
// ShortcutOverride for these when unmodified (keypad included), so typing wins.
bool isPrintableKey(int key)
{
    return key > 0 && key < Qt::Key_Escape && key != Qt::Key_Space;
}

QString withoutKeypad(const QString &portable)
{
    const QKeySequence seq = QKeySequence::fromString(portable, QKeySequence::PortableText);
    if (seq.count() != 1 || !seq[0].keyboardModifiers().testFlag(Qt::KeypadModifier))
        return {};
    return QKeySequence(QKeyCombination(seq[0].keyboardModifiers() & ~Qt::KeypadModifier,
                                        seq[0].key()))
        .toString(QKeySequence::PortableText);
}

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

}

QString ShortcutRegistry::blockingReservedOwner(const QString &portable,
                                                ActionContext context,
                                                const QString &actionId) const
{
    const QString owner = reservedOwner(portable);
    if (owner.isEmpty())
        return {};
    if (context == MessageSelectionContext
        && (portable == QStringLiteral("R") || portable == QStringLiteral("T")
            || portable == QStringLiteral("E")
            || portable == QStringLiteral("Ctrl+C")))
        return {};
    if (context == DialogContext
        && actionId == QStringLiteral("dialog.close")
        && portable == QStringLiteral("Ctrl+C"))
        return {};
    return owner;
}

bool ShortcutRegistry::contextsClash(ActionContext a, ActionContext b)
{
    if (a == SettingsContext || b == SettingsContext)
        return a == b;
    if (a == DialogContext || b == DialogContext)
        return a == b;

    return a == b || a == TimelineContext || b == TimelineContext;
}

bool ShortcutRegistry::actionsClash(const Action &a, const Action &b)
{
    return contextsClash(a.context, b.context)
        || (a.context == SettingsContext && b.id == QStringLiteral("dialog.close"))
        || (b.context == SettingsContext && a.id == QStringLiteral("dialog.close"));
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

#if defined(Q_OS_WIN)
    m_keyboardPlatform = KeyboardPlatform::Windows;
#elif defined(Q_OS_MACOS)
    m_keyboardPlatform = KeyboardPlatform::MacOS;
#else
    // qGuiApp is a bare cast, so check the type before asking for the plugin.
    QCoreApplication *app = QCoreApplication::instance();
    if (app && app->inherits("QGuiApplication")) {
        const QString platform = QGuiApplication::platformName();
        if (platform.startsWith(QLatin1String("xcb"))
            || platform.startsWith(QLatin1String("wayland")))
            m_keyboardPlatform = KeyboardPlatform::Xkb;
    }
#endif
    if (m_keyboardPlatform != KeyboardPlatform::Unknown) {
        if (QCoreApplication *app = QCoreApplication::instance())
            app->installEventFilter(this);
    }
}

int ShortcutRegistry::physicalDigitKey(KeyboardPlatform platform,
                                       quint32 nativeScanCode,
                                       quint32 nativeVirtualKey)
{
    switch (platform) {
    case KeyboardPlatform::Xkb:
        // evdev KEY_1..KEY_0 are 2..11; xkb keycodes add 8.
        if (nativeScanCode >= 10 && nativeScanCode <= 19)
            return nativeScanCode == 19 ? Qt::Key_0
                                        : Qt::Key_1 + int(nativeScanCode - 10);
        return 0;
    case KeyboardPlatform::Windows:
        // Set-1 make codes 0x02..0x0B; the extended bit marks other keys.
        if (nativeScanCode >= 0x02 && nativeScanCode <= 0x0B)
            return nativeScanCode == 0x0B ? Qt::Key_0
                                          : Qt::Key_1 + int(nativeScanCode - 0x02);
        return 0;
    case KeyboardPlatform::MacOS:
        // kVK_ANSI_* codes, which are not in digit order.
        switch (nativeVirtualKey) {
        case 0x12: return Qt::Key_1;
        case 0x13: return Qt::Key_2;
        case 0x14: return Qt::Key_3;
        case 0x15: return Qt::Key_4;
        case 0x17: return Qt::Key_5;
        case 0x16: return Qt::Key_6;
        case 0x1A: return Qt::Key_7;
        case 0x1C: return Qt::Key_8;
        case 0x19: return Qt::Key_9;
        case 0x1D: return Qt::Key_0;
        default: return 0;
        }
    case KeyboardPlatform::Unknown:
        break;
    }
    return 0;
}

bool ShortcutRegistry::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress: {
        const auto *key = static_cast<const QKeyEvent *>(event);
        m_pressedKey = key->key();
        m_pressedModifiers = int(key->modifiers());
        m_pressedDigit = physicalDigitKey(m_keyboardPlatform, key->nativeScanCode(),
                                          key->nativeVirtualKey());
        if (event->type() == QEvent::KeyPress && watched->isWindowType()
            && runKeypadGlobalShortcut(watched, key))
            return true;
        break;
    }
    case QEvent::KeyRelease:
        m_pressedKey = 0;
        m_pressedDigit = 0;
        break;
    default:
        break;
    }
    return QAbstractListModel::eventFilter(watched, event);
}

// xkb strips the keypad modifier before Qt's shortcut map matches a press, so
// a bare Num+ binding never fires there; run it from the unclaimed key press.
bool ShortcutRegistry::runKeypadGlobalShortcut(QObject *window, const QKeyEvent *key)
{
    if (!key->modifiers().testFlag(Qt::KeypadModifier))
        return false;
    const QString seq = sequenceFromKeyEvent(key->key(), int(key->modifiers()));
    if (!seq.startsWith(QLatin1String("Num+")))
        return false;
    bool bound = false;
    for (int i = 0; i < m_actions.size() && !bound; ++i)
        bound = m_actions.at(i).context == GlobalContext && m_resolved.at(i) == seq;
    if (!bound)
        return false;
    // Whatever claims the key as an override (a text field, a capture chip) keeps it.
    if (QObject *focus = QGuiApplication::focusObject()) {
        QKeyEvent probe(QEvent::ShortcutOverride, key->key(), key->modifiers(),
                        key->nativeScanCode(), key->nativeVirtualKey(),
                        key->nativeModifiers(), key->text(), key->isAutoRepeat(),
                        key->count());
        probe.setAccepted(false);
        QCoreApplication::sendEvent(focus, &probe);
        if (probe.isAccepted())
            return false;
    }
    const QKeySequence target = QKeySequence::fromString(seq, QKeySequence::PortableText);
    bool fired = false;
    const auto objects = window->findChildren<QObject *>();
    for (QObject *object : objects) {
        if (!object->inherits("QQuickShortcut") || !object->property("enabled").toBool())
            continue;
        QVariantList sequences = object->property("sequences").toList();
        sequences << object->property("sequence");
        for (const QVariant &value : sequences) {
            if (QKeySequence::fromString(value.toString(), QKeySequence::PortableText)
                == target) {
                QMetaObject::invokeMethod(object, "activated");
                fired = true;
                break;
            }
        }
    }
    return fired;
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
    const QString settingsCat = tr("Settings navigation");

    // Every row must be wired to a QML `Shortcut`: a registry entry whose QML
    // site was never migrated is a shortcut that reports a key and does
    // nothing, which is strictly worse than not offering it.
    m_actions = {
        { QStringLiteral("app.quit"), appCat,
          tr("Quit Lightning"), QStringLiteral("Ctrl+Q"), GlobalContext },
        { QStringLiteral("app.openSettings"), appCat,
          tr("Open Settings, or focus its search field"),
          QStringLiteral("Ctrl+,"), GlobalContext },
        { QStringLiteral("app.shortcutsHelp"), appCat,
          tr("Show the keyboard shortcuts"), QStringLiteral("Ctrl+/"),
          GlobalContext },

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
        { QStringLiteral("nav.newDirectMessage"), navCat,
          tr("Start a direct message"), QStringLiteral("Ctrl+Shift+T"),
          GlobalContext },
        { QStringLiteral("nav.historyBack"), navCat,
          tr("Go back to the previous conversation"), QStringLiteral("Alt+Left"),
          GlobalContext },
        { QStringLiteral("nav.historyForward"), navCat,
          tr("Go forward to the next conversation"), QStringLiteral("Alt+Right"),
          GlobalContext },
        { QStringLiteral("nav.activityCenter"), navCat,
          tr("Open the Activity Center"), QStringLiteral("Ctrl+Shift+I"),
          GlobalContext },
        { QStringLiteral("nav.savedMessages"), navCat,
          tr("Open saved messages"), QStringLiteral("Ctrl+Alt+S"),
          GlobalContext },
        { QStringLiteral("nav.threadsActivity"), navCat,
          tr("Open Threads activity"), {}, GlobalContext },
        { QStringLiteral("view.toggleLayoutPreset"), viewCat,
          tr("Switch between Lightning and compact layouts"), {}, GlobalContext },
        { QStringLiteral("thread.togglePlacement"), viewCat,
          tr("Move the current thread between main and side views"),
          {}, GlobalContext },
        { QStringLiteral("thread.openRootInRoom"), navCat,
          tr("Open the current thread root in its room"), {}, GlobalContext },

        { QStringLiteral("dialog.previousItem"), dialogCat,
          tr("Select the previous item or Settings section"),
          QStringLiteral("Ctrl+K"), DialogContext },
        { QStringLiteral("dialog.nextItem"), dialogCat,
          tr("Select the next item or Settings section"),
          QStringLiteral("Ctrl+J"), DialogContext },
        { QStringLiteral("dialog.close"), dialogCat,
          tr("Dismiss Activity, the quick switcher or Settings"),
          QStringLiteral("Ctrl+C"), DialogContext },

        { QStringLiteral("settings.previousPage"), settingsCat,
          tr("Open the previous Settings page"), {}, SettingsContext },
        { QStringLiteral("settings.nextPage"), settingsCat,
          tr("Open the next Settings page"), {}, SettingsContext },
        { QStringLiteral("settings.previousSubsection"), settingsCat,
          tr("Jump to the previous Settings subsection"), {}, SettingsContext },
        { QStringLiteral("settings.nextSubsection"), settingsCat,
          tr("Jump to the next Settings subsection"), {}, SettingsContext },

        { QStringLiteral("settings.halfPageUp"), settingsCat,
          tr("Scroll Settings up half a page"), {}, SettingsContext },
        { QStringLiteral("settings.halfPageDown"), settingsCat,
          tr("Scroll Settings down half a page"), {}, SettingsContext },

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

        { QStringLiteral("room.find"), roomCat,
          tr("Find in the loaded timeline"), QStringLiteral("Ctrl+F"),
          GlobalContext },
        { QStringLiteral("room.findNext"), roomCat,
          tr("Go to the next find result"), QStringLiteral("Ctrl+G"),
          GlobalContext },
        { QStringLiteral("room.findPrevious"), roomCat,
          tr("Go to the previous find result"), QStringLiteral("Ctrl+Shift+G"),
          GlobalContext },
        { QStringLiteral("timeline.selectLatestMessage"), roomCat,
          tr("Select the latest message"), QStringLiteral("Ctrl+Shift+Space"),
          GlobalContext },
        { QStringLiteral("timeline.resumeMessageSelection"), roomCat,
          tr("Resume message selection in the current view"),
          QStringLiteral("Ctrl+Shift+W"), GlobalContext },
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
        { QStringLiteral("timeline.replyInThread"), roomCat,
          tr("Open the selected message's thread"), QStringLiteral("T"),
          MessageSelectionContext },
        { QStringLiteral("timeline.editMessage"), roomCat,
          tr("Edit the selected message"), QStringLiteral("E"),
          MessageSelectionContext },
        { QStringLiteral("timeline.reactToMessage"), roomCat,
          tr("React to the selected message"), QStringLiteral("A"),
          MessageSelectionContext },
        { QStringLiteral("timeline.pinMessage"), roomCat,
          tr("Pin or unpin the selected message"), QStringLiteral("P"),
          MessageSelectionContext },
        { QStringLiteral("timeline.forwardMessage"), roomCat,
          tr("Forward the selected message"), QStringLiteral("F"),
          MessageSelectionContext },
        { QStringLiteral("timeline.saveMessage"), roomCat,
          tr("Save or remove the selected message"), QStringLiteral("S"),
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
        { QStringLiteral("timeline.viewMessageSource"), roomCat,
          tr("View the selected message's source"), QStringLiteral("V"),
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
        { QStringLiteral("timeline.jumpToLatestAndCompose"), roomCat,
          tr("Jump to the latest message and focus the message box"), {},
          GlobalContext },
        { QStringLiteral("room.markRead"), roomCat,
          tr("Mark the open conversation as read"),
          QStringLiteral("Ctrl+Shift+M"), GlobalContext },
        { QStringLiteral("room.markAllRead"), roomCat,
          tr("Mark every conversation as read"), QStringLiteral("Ctrl+Shift+A"),
          GlobalContext },
        { QStringLiteral("room.markUnread"), roomCat,
          tr("Mark the open conversation as unread"),
          QStringLiteral("Ctrl+Shift+R"), GlobalContext },
        { QStringLiteral("room.togglePeople"), roomCat,
          tr("Show or hide the people in this conversation"),
          QStringLiteral("Ctrl+U"), GlobalContext },
        { QStringLiteral("room.togglePinned"), roomCat,
          tr("Show or hide pinned messages"), QStringLiteral("Ctrl+Shift+P"),
          GlobalContext },

        { QStringLiteral("call.toggleStreamInfo"), callCat,
          tr("Show or hide Stream Info"), QStringLiteral("Ctrl+F9"),
          GlobalContext },
        { QStringLiteral("call.toggleMute"), callCat,
          tr("Mute or unmute the microphone"), QStringLiteral("Ctrl+Shift+U"),
          GlobalContext },
        { QStringLiteral("call.toggleDeafen"), callCat,
          tr("Deafen or undeafen"), QStringLiteral("Ctrl+Shift+H"),
          GlobalContext },
        { QStringLiteral("call.toggleCamera"), callCat,
          tr("Turn the camera on or off"), QStringLiteral("Ctrl+Shift+V"),
          GlobalContext },
        { QStringLiteral("call.returnToCall"), callCat,
          tr("Return to the active call"), QStringLiteral("Ctrl+Alt+A"),
          GlobalContext },
        { QStringLiteral("call.startCall"), callCat,
          tr("Start a call in this conversation"), QStringLiteral("Ctrl+Shift+C"),
          GlobalContext },
        { QStringLiteral("call.leave"), callCat,
          tr("Leave the call"), QStringLiteral("Ctrl+Shift+Y"), GlobalContext },
        { QStringLiteral("call.toggleScreenShare"), callCat,
          tr("Share your screen, or stop sharing"),
          QStringLiteral("Ctrl+Shift+S"), GlobalContext },

        { QStringLiteral("media.focusPlayer"), mediaCat,
          tr("Focus the audio player"), QStringLiteral("Ctrl+Shift+J"),
          GlobalContext },

        { QStringLiteral("composer.emojiPicker"), composerCat,
          tr("Open the emoji picker"), QStringLiteral("Ctrl+Shift+E"),
          GlobalContext },
        { QStringLiteral("composer.gifPicker"), composerCat,
          tr("Open the GIF and sticker picker"), QStringLiteral("Ctrl+Shift+G"),
          GlobalContext },
        { QStringLiteral("composer.attach"), composerCat,
          tr("Attach files"), QStringLiteral("Ctrl+Shift+O"), GlobalContext },

        { QStringLiteral("composer.focus"), composerCat,
          tr("Focus the message box"), QStringLiteral("Ctrl+L"), GlobalContext },
        { QStringLiteral("composer.toggleHeight"), composerCat,
          tr("Expand or collapse the message box"), {}, EditorContext },

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
          tr("closing the find bar, room information, a thread or Settings") },
        { QStringLiteral("Alt+V"),
          tr("the emoji picker's skin-tone selector") },
        { QStringLiteral("Ctrl+C"), tr("the message menu's Copy accelerator") },
        { QStringLiteral("R"), tr("the message menu's Reply accelerator") },
        { QStringLiteral("T"), tr("the message menu's Thread accelerator") },
        { QStringLiteral("E"), tr("the message menu's Edit accelerator") },
        { QStringLiteral("Ctrl+Shift+D"), tr("the screenshot-demo controls") },
        { QStringLiteral("Space"), tr("paging the timeline and the media grids") },
        { QStringLiteral("Shift+PgUp"),
          tr("jumping to the oldest unread message") },
        { QStringLiteral("Home"), tr("jumping to the earliest loaded message") },
        { QStringLiteral("End"), tr("jumping to the latest message") },
    };
    m_exclusive = {
        { QStringLiteral("room.findPrevious"),
          QStringLiteral("composer.gifPicker") },
    };
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
    QKeySequence seq = QKeySequence::fromString(trimmed, QKeySequence::PortableText);
    if (seq.isEmpty() || isBareModifier(seq))
        return {};
    if (seq.count() == 1 && seq[0].key() == Qt::Key_Backtab)
        seq = QKeySequence(QKeyCombination(seq[0].keyboardModifiers() | Qt::ShiftModifier,
                                          Qt::Key_Tab));
    return seq.toString(QKeySequence::PortableText);
}

QString ShortcutRegistry::sequenceFromKeyEvent(int key, int modifiers) const
{
    auto mods = static_cast<Qt::KeyboardModifiers>(modifiers)
                      & ~Qt::GroupSwitchModifier;
    // The numpad is a key of its own only when pressed alone; Ctrl+Num+0 is
    // still Ctrl+0.
    if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        mods &= ~Qt::KeypadModifier;
    if (key == Qt::Key_Backtab) {
        key = Qt::Key_Tab;
        mods |= Qt::ShiftModifier;
    }
    const QKeySequence seq(QKeyCombination(mods, static_cast<Qt::Key>(key)));
    if (seq.isEmpty() || isBareModifier(seq))
        return {};
    return seq.toString(QKeySequence::PortableText);
}

QString ShortcutRegistry::storedOverride(const QString &actionId) const
{
    if (!m_settings)
        return {};
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

QString ShortcutRegistry::actionForKeyIn(ActionContext context, int key,
                                         int modifiers) const
{
    // Like Qt's shortcut map: an exact numpad binding first, then the same key
    // without the numpad, so a numpad Delete still runs a Delete binding.
    const QString seq = sequenceFromKeyEvent(key, modifiers);
    if (seq.isEmpty())
        return {};
    // A modified press on a main-row digit also answers that digit's binding,
    // whatever symbol the layout delivered, but only after the literal one.
    QString physical;
    const auto commandMods = Qt::ShiftModifier | Qt::ControlModifier
                             | Qt::AltModifier | Qt::MetaModifier;
    if (m_pressedDigit != 0 && m_pressedKey == key && m_pressedModifiers == modifiers
        && key != m_pressedDigit
        && (Qt::KeyboardModifiers(modifiers) & commandMods))
        physical = sequenceFromKeyEvent(m_pressedDigit, modifiers);
    for (const QString &candidate : { seq, withoutKeypad(seq), physical }) {
        if (candidate.isEmpty())
            continue;
        for (int i = 0; i < m_actions.size(); ++i) {
            if (m_actions.at(i).context == context && m_resolved.at(i) == candidate)
                return m_actions.at(i).id;
        }
    }
    return {};
}

QString ShortcutRegistry::editorActionForKey(int key, int modifiers) const
{
    return actionForKeyIn(EditorContext, key, modifiers);
}

QString ShortcutRegistry::timelineActionForKey(int key, int modifiers) const
{
    return actionForKeyIn(TimelineContext, key, modifiers);
}

QString ShortcutRegistry::messageSelectionActionForKey(int key, int modifiers) const
{
    return actionForKeyIn(MessageSelectionContext, key, modifiers);
}

QString ShortcutRegistry::dialogActionForKey(int key, int modifiers,
                                            bool copyingSelectedText) const
{
    if (copyingSelectedText)
        return {};
    return actionForKeyIn(DialogContext, key, modifiers);
}

QString ShortcutRegistry::settingsActionForKey(int key, int modifiers,
                                               bool copyingSelectedText) const
{
    if (copyingSelectedText)
        return {};
    const QString action = actionForKeyIn(SettingsContext, key, modifiers);
    if (!action.isEmpty())
        return action;
    return dialogActionForKey(key, modifiers, false);
}

QString ShortcutRegistry::editorOverrideFor(int key, int modifiers,
                                            bool hasSelection) const
{
    const QString action = editorActionForKey(key, modifiers);
    if (action.isEmpty() || hasSelection)
        return action;
    const QString seq = sequenceFor(action);
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

QString ShortcutRegistry::displayText(const QString &sequence)
{
    QString text = sequence;
    return text.replace(QLatin1String("Num+"), QLatin1String("Num "));
}

bool ShortcutRegistry::yieldsToTyping(const QString &sequence)
{
    const QString portable = normalize(sequence);
    if (portable.isEmpty())
        return false;
    const QKeySequence seq = QKeySequence::fromString(portable, QKeySequence::PortableText);
    return isSingleBareKey(seq) && isPrintableKey(seq[0].key());
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

    const bool selectionOnly = action->context == MessageSelectionContext;
    const bool dialogOnly = action->context == DialogContext;
    const bool settingsOnly = action->context == SettingsContext;
    const bool settingsPage = actionId == QStringLiteral("settings.previousPage")
                              || actionId == QStringLiteral("settings.nextPage");
    const auto combo = seq[0];
    const bool settingsTab = settingsPage && combo.key() == Qt::Key_Tab
        && (combo.keyboardModifiers() & ~Qt::ShiftModifier) == Qt::NoModifier;
    if (settingsOnly && seq.count() != 1)
        return tr("Use a single key combination for Settings navigation.");
    if (dialogOnly && seq.count() != 1)
        return tr("Use a single key combination for dialog navigation.");

    if (selectionOnly && seq.count() != 1)
        return tr("Use a single key combination for message selection.");
    const bool globalKey = action->context == GlobalContext && isSingleBareKey(seq);
    if (globalKey && isNavigationOrEditingKey(combo.key())) {
        return tr("%1 moves focus, navigates or edits text, so it cannot be a "
                  "shortcut on its own.")
            .arg(displayText(portable));
    }
    const bool bareOk = selectionOnly || settingsTab || globalKey
                       || (action->context == TimelineContext && isNonTextKey(seq));
    if (!hasCommandModifier(seq) && !bareOk) {
        if (action->context == TimelineContext) {
            return tr("Use Ctrl, Alt or Super, or PgUp, PgDn or an F-key. "
                      "Any other bare key is a character you type in the "
                      "message box.");
        }
        if (settingsOnly)
            return tr("Use Ctrl, Alt or Super. Settings page actions also accept Tab or Shift+Tab.");
        if (dialogOnly)
            return tr("Use Ctrl, Alt or Super. Dialog shortcuts must leave "
                      "ordinary typing and navigation keys available.");
        return action->context == GlobalContext
                   ? tr("Use Ctrl, Alt or Super, or a single key on its own. "
                        "Shift alone changes the character you type, so it "
                        "does not count.")
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
            .arg(displayText(portable), owner);
    }

    for (int i = 0; i < m_actions.size(); ++i) {
        const Action &other = m_actions.at(i);
        if (other.id == actionId)
            continue;
        if (!actionsClash(other, *action))
            continue;
        if (exclusive(other.id, actionId))
            continue;
        if (m_resolved.at(i) == portable) {
            return tr("%1 is already used for “%2”. Two shortcuts on "
                      "one key make Qt fire neither of them.")
                .arg(displayText(portable), other.description);
        }
    }
    return {};
}

QString ShortcutRegistry::setBinding(const QString &actionId,
                                     const QString &sequence)
{
    const QString error = validationError(actionId, sequence);
    if (!error.isEmpty())
        return error;

    const Action *action = find(actionId);
    if (!action || !m_settings)
        return tr("Unknown action.");

    const QString portable = normalize(sequence);
    if (portable == normalize(action->defaultSequence)) {
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
    const QString error = action->defaultSequence.isEmpty()
        ? QString() : validationError(actionId, action->defaultSequence);
    if (!error.isEmpty())
        return error;
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
        if (!actionsClash(m_actions.at(i), a))
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
    if (a.context == SettingsContext)
        return tr("Only in Settings. Overrides shared navigation; Tab bindings replace normal focus traversal.");
    if (a.context == DialogContext)
        return tr("Only in Activity, the quick switcher or Settings. "
                  "Copying selected text takes priority over closing.");
    const QString mine = m_resolved.at(row);
    if (mine.isEmpty())
        return {};
    QStringList notes;
    for (int i = 0; i < m_actions.size(); ++i) {
        if (i == row)
            continue;
        const Action &other = m_actions.at(i);
        if (actionsClash(other, a))
            continue;
        if (m_resolved.at(i) != mine)
            continue;
        if (other.context == SettingsContext)
            notes.append(tr("In Settings, “%1” runs instead.").arg(other.description));
        else if (other.context == DialogContext)
            notes.append(tr("In Activity, the quick switcher or Settings, "
                            "“%1” runs instead.")
                             .arg(other.description));
        else if (a.context == MessageSelectionContext)
            notes.append(tr("While the timeline has focus this runs instead of “%1”.")
                             .arg(other.description));
        else if (other.context == MessageSelectionContext)
            notes.append(tr("While the timeline has focus, “%1” runs instead.")
                             .arg(other.description));
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
    Q_EMIT bindingsChanged();
}
