# Message keyboard navigation

Keyboard shortcuts can be rebound in Settings → Keyboard shortcuts.
Sequences below are the defaults; Shift is written when required.

## Focus compose

Ctrl+L (`composer.focus`) focuses the active Markdown or rich-text
message editor without changing the draft or reply/edit target. It
does nothing when compose is hidden, read-only, or a thread owns
the keyboard. The quick switcher keeps its keyboard focus while open.

## Select and reply

| Default | Action ID | Behavior |
| --- | --- | --- |
| Ctrl+Shift+Space | `timeline.selectLatestMessage` | Jump to and select the latest message. |
| Ctrl+Shift+W | `timeline.resumeMessageSelection` | Resume selection in the current viewport. |
| K | `timeline.olderMessage` | Select the older message. |
| J | `timeline.newerMessage` | Select the newer message. |
| U | `timeline.halfPageUp` | Immediately scroll up half a viewport. |
| D | `timeline.halfPageDown` | Immediately scroll down half a viewport. |
| R | `timeline.reply` | Reply to the selected message and focus compose. |

The plain letters (lowercase k/j/u/d/r, nvim style) run only while the room
timeline itself has focus, which a click on the timeline gives it. No text
field is involved, so typing is unaffected: the message editor never receives
them, and the composer keeps every ordinary typing and formatting shortcut.
They shadow no global or composer binding, which is why they are bare keys:
Ctrl+K (quick switcher) and Ctrl+U (people panel) stay what they are while the
timeline has focus. `R` is also the message menu's Reply accelerator, but that
exists only while the menu is open and owns focus, so the two never meet.
Capital letters need an explicit Shift modifier in the stored sequence.

Message selection refuses keys that move focus or navigate: Tab, Shift+Tab,
F6, Shift+F6, Return/Enter, the arrows, Backspace, Escape and Space (with no
modifier or Shift only). The resume shortcut is Ctrl+Shift+W because every
Alt+letter types a character on some Mac layout and every other
Ctrl+Shift+letter is taken.

Tab and arrow navigation can instead focus an individual message row;
that existing row focus keeps its own actions and hides message selection.
Selection shortcuts do not act on the hidden selection in that state.
Use Ctrl+Shift+W to return to message selection before replying with R.
Redacted messages, local echoes, call cards and room-activity rows are
skipped as reply targets. Rebind any action in Settings if you prefer a
different arrangement.

Half-pages move the viewport without changing the selected message.
After paging, the first older-message key selects the newest visible
message; the first newer-message key selects the oldest visible message.
That first key does not scroll or skip a message. Subsequent keys step
through messages, moving the viewport only as needed to expose them.

Ctrl+L hides the selection rectangle and remembers its target. Ctrl+Shift+W
restores that target when it is still visible, otherwise it selects
the newest visible message without jumping away. Drafts and reply/edit
targets survive this round trip; replying to another selected message
changes the reply target. Room changes clear remembered selection.

Existing Page Up/Page Down, jump-to-start/end, Space/Shift+Space and
composer caret behavior remain available. Escape keeps its contextual
dismissal and back-navigation behavior; use Ctrl+Shift+W to resume selection
without cancelling a reply or leaving the room.

## Activity, the quick switcher and Settings

The Activity Center is a modal dialog; the quick switcher is a modal
popup. Settings is an in-shell screen. All three use these actions,
listed under **Dialogs** in Settings → Keyboard shortcuts:

| Default | Action ID | Behavior |
| --- | --- | --- |
| Ctrl+K | `dialog.previousItem` | Select the previous item or Settings section. |
| Ctrl+J | `dialog.nextItem` | Select the next item or Settings section. |
| Ctrl+C | `dialog.close` | Dismiss the current surface or Settings layer. |

Activity focuses its list on opening. In the switcher, selection moves
while the query keeps focus, including in command mode. The shortcuts
scroll the selected row into view and stop at the ends of the list.
They apply only inside these surfaces; main-window and timeline
bindings can reuse the same combinations.

Copying selected query text takes priority over closing: Ctrl+C copies
while text is selected, and closes when nothing is selected. A rebound
close action such as Alt+L closes directly. Arrows, Enter, Tab and Escape
keep their existing behavior. Use a single combination with Ctrl, Alt
or Super for these actions; plain letters stay available for searching.

In Settings, previous/next follow the visible sidebar sections and stop
at either end. Search filtering hides sections from this navigation.
Tab and Shift+Tab still move between individual controls. The close action
uses the existing Escape behavior: clear the search, close an inline
picker, leave a content editor, then return to the room. Selected-text
Copy, shortcut capture and nested popups keep their own key handling.

Settings also offers six unbound actions under **Settings navigation**:
`settings.previousPage`, `settings.nextPage`,
`settings.previousSubsection`, `settings.nextSubsection`,
`settings.halfPageUp` and `settings.halfPageDown`. Subsection
navigation immediately reveals the heading's controls and focuses its
sidebar link, without requiring Space or Enter. It skips headings whose
controls are hidden, opens a collapsed outline, and stops at either end.
A search query is cleared to reveal the current page before jumping.

For example, to use Tab for pages and Ctrl+J/K for subsections:

```ini
[shortcuts]
settings.nextPage=Tab
settings.previousPage=Shift+Tab
settings.nextSubsection=Ctrl+J
settings.previousSubsection=Ctrl+K
settings.halfPageUp=Ctrl+U
settings.halfPageDown=Ctrl+D
```

These Settings-only overrides take priority over shared dialog navigation.
Tab/Shift+Tab replace ordinary focus traversal only when explicitly bound
to page actions; resetting them restores the normal Tab behavior. The
Activity and quick-switcher bindings remain independent. Half-page actions
scroll the controls viewport immediately without moving focus or switching
pages. Page Up/Down retain their existing full-page behavior. Ctrl+, focuses
the Settings search field (`app.openSettings`).

## Cursor blinking

Settings → Appearance → Motion and time offers **Disable cursor
blinking**. It takes effect immediately in text inputs, including the
Markdown, rich-text and thread composers. The default is off, which
leaves Qt's system cursor timing in control. Turning it on requests a
steady cursor; turning it off removes that override. The preference is
stored on this device, independently of the active account.

On Linux, the QSettings file is normally
`~/.config/MatrixClient/matrix-client.conf`; in the Flatpak it is
`~/.var/app/org.lightning_matrix.Lightning/config/MatrixClient/matrix-client.conf`.
Quit Lightning before editing it. For example, this disables blinking and
spells out the default timeline selection/reply keys:

```ini
[ui]
disableCursorBlink=true

[shortcuts]
composer.focus=Ctrl+L
dialog.previousItem=Ctrl+K
dialog.nextItem=Ctrl+J
dialog.close=Ctrl+C
timeline.selectLatestMessage=Ctrl+Shift+Space
timeline.resumeMessageSelection=Ctrl+Shift+W
timeline.olderMessage=K
timeline.newerMessage=J
timeline.halfPageUp=U
timeline.halfPageDown=D
timeline.reply=R
```

Qt's stored `K`, `J`, `U`, `D` and `R` mean lowercase letters without Shift.
Existing account-specific shortcut overrides take precedence over
this global fallback; use Settings → Keyboard shortcuts to update
the active account's bindings. Keep account/device/keyring records
local rather than replacing the whole settings file with an example.

Ctrl+Shift+Space goes through the timeline's own jump-to-latest bookkeeping
(live edge exposed, follow-latest saved, read receipts re-evaluated), with
the row window bounded so a distant jump does not instantiate the skipped
history.

Selecting latest also saves follow-latest intent for the room, so
returning after messages arrive shows the new latest message. Leaving
the room scrolled into history instead restores its historical view.

## Thread panel

While the thread panel holds a thread, `composer.focus`,
`timeline.selectLatestMessage` and `timeline.resumeMessageSelection` act on
the panel (its composer, its reply list), and the reply list takes the same
message-selection bindings (`timeline.olderMessage`, `timeline.newerMessage`,
`timeline.halfPageUp`, `timeline.halfPageDown`, `timeline.reply`). Under the
inline Thread style (Settings > Appearance) thread replies are rows of the room
timeline, so the room bindings reach them directly.
