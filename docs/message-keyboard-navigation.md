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
| Alt+L | `timeline.resumeMessageSelection` | Resume selection in the current viewport. |
| Ctrl+K | `timeline.olderMessage` | Select the older message. |
| Ctrl+J | `timeline.newerMessage` | Select the newer message. |
| Ctrl+U | `timeline.halfPageUp` | Immediately scroll up half a viewport. |
| Ctrl+D | `timeline.halfPageDown` | Immediately scroll down half a viewport. |
| Ctrl+R | `timeline.reply` | Reply to the selected message and focus compose. |

The last five actions run only while the room timeline itself has focus.
Tab and arrow navigation can instead focus an individual message row;
that existing row focus keeps its own actions and hides message selection.
Selection shortcuts do not act on the hidden selection in that state.
Use Alt+L to return to message selection before replying with Ctrl+R.
Redacted messages, local echoes, call cards and room-activity rows are
skipped as reply targets.
They may use plain letters, such as K/J/R (lowercase k/j/r; capital
letters require an explicit Shift modifier in the stored sequence).
The message editor keeps ordinary typing and formatting shortcuts.
Ctrl+K opens the quick switcher outside the timeline; inside it, the
default older-message action takes precedence. Rebind either action
in Settings if you prefer a different arrangement.

Half-pages move the viewport without changing the selected message.
After paging, the first older-message key selects the newest visible
message; the first newer-message key selects the oldest visible message.
That first key does not scroll or skip a message. Subsequent keys step
through messages, moving the viewport only as needed to expose them.

Ctrl+L hides the selection rectangle and remembers its target. Alt+L
restores that target when it is still visible, otherwise it selects
the newest visible message without jumping away. Drafts and reply/edit
targets survive this round trip; replying to another selected message
changes the reply target. Room changes clear remembered selection.

Existing Page Up/Page Down, jump-to-start/end, Space/Shift+Space and
composer caret behavior remain available. Escape keeps its contextual
dismissal and back-navigation behavior; use Alt+L to resume selection
without cancelling a reply or leaving the room.

## Cursor blinking

Settings → Appearance → Motion and time offers **Disable cursor
blinking**. It takes effect immediately in text inputs, including the
Markdown, rich-text and thread composers. The default is off, which
leaves Qt's system cursor timing in control. Turning it on requests a
steady cursor; turning it off removes that override. The preference is
stored on this device, independently of the active account.

On Linux, the QSettings file is normally
`~/.config/MatrixClient/matrix-client.conf`. Quit Lightning before
editing it. For example, this disables blinking and uses plain keys
for timeline selection/reply:

```ini
[ui]
disableCursorBlink=true

[shortcuts]
composer.focus=Ctrl+L
timeline.selectLatestMessage=Ctrl+Shift+Space
timeline.resumeMessageSelection=Alt+L
timeline.olderMessage=K
timeline.newerMessage=J
timeline.halfPageUp=Ctrl+U
timeline.halfPageDown=Ctrl+D
timeline.reply=R
```

Qt's stored `K`, `J` and `R` mean lowercase k/j/r without Shift.
Existing account-specific shortcut overrides take precedence over
this global fallback; use Settings → Keyboard shortcuts to update
the active account's bindings. Keep account/device/keyring records
local rather than replacing the whole settings file with an example.

Selecting latest also saves follow-latest intent for the room, so
returning after messages arrive shows the new latest message. Leaving
the room scrolled into history instead restores its historical view.
