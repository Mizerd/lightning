# Button and form audit (master prompt Part 6, section 2.4)

Started 2026-10-25. This is a **source inventory**: it records which button
primitive each surface family uses, what the round changed, and what is still
open. A row that says nothing about live rendering is a source finding, not a
visual PASS. A real-desktop pass over every family is **NOT TESTED**.

## The vocabulary (AppButton)

| Kind | Role | Resting state on a card (after this round) |
|---|---|---|
| `primary` | The commit action of its local task | Accent fill. |
| `secondary` (default) | A visible ordinary action | A tint of its own ink (`buttonSecondaryFill`, 6% `textPrimary`) inside the control-strength edge (`buttonSecondaryBorder` = `borderStrong`). Before this round it was transparent inside `border`, which measured 1.03:1 on Nord's cardElevated. `ThemeTokensTest::theSecondaryButtonEdgeReadsOnEveryCard` covers it. |
| `ghost` | Low emphasis, inside clear surrounding context | Borderless, no resting fill (unchanged). |
| `danger` / `dangerPrimary` | Quiet destructive action / committed confirmation | Unchanged. |
| `busy: true` | A submission this button started is in flight | Spinner before the label. The label and geometry stay. The owner keeps the button disabled. |

Disabled controls of every kind except ghost now take `buttonDisabledTint`
(8% of `buttonDisabledInk`) inside a 1px `buttonDisabledBorder`, with
`buttonDisabledInk` labels. This was found live on 2026-10-26: a disabled
filled button took `buttonDisabledFill`, which is `cardElevated`, the card the
Space Home Rename/Save sit on, so an unchanged Rename read as stray text.
`ControlSystemTest` asserts the new state. The Storm skin's secondary kind now
has a resting tint too (6% of `stormTextSecondary`).

## Inventory

| Surface family | Primitive(s) | Result this round |
|---|---|---|
| Space Home inline settings (TimelinePane) | `SpaceTextEditRow` (AppButton primary) | **Fixed.** It shares one draft/dirty/permission contract with Space settings: no re-send of an unchanged value, target re-checked at submission, a busy row, and Return takes the same guarded path. `space-text-edit-row-qml`. |
| Space settings dialog | `SpaceTextEditRow` (storm) | **Fixed**, the same component. |
| Invite People | AppButton primary with `iconName: "send"` and `busy` | **Fixed.** The custom `InvitePrimaryButton` (a literal 13 px font, an outset focus ring) is gone. |
| Settings navigation | ItemDelegate row plus its own chevron AbstractButton (24 px) | **Changed.** The chevron is a real control with an accessible name and an expanded state in its description. Row selection is separate from expansion. |
| Theme editor | `EditorButton` (an inline component in its own editor skin) | Kept custom, deliberately: the editor paints in its own `editor*` palette so it stays readable while the theme it edits is broken. New controls (Select elements, Link colours, Ungroup, Rename, Edit this element only) use it. |
| Settings pages (63 AppButtons) | AppButton secondary/primary | Inherit the new secondary affordance. Not visually re-checked per theme: NOT TESTED. |
| Home pane | Custom AbstractButton (the Space pills) using `buttonNeutral*` | Kept on purpose: it is a chip with an avatar, a pill radius and a "current Space" state, not a text button. It sits on the page, where `buttonNeutralFill` (cardElevated) does read. Seen live on 2026-10-26. |
| Media players, viewers, calls | IconButton, CallControlButton, VideoControlBar | Out of scope for the text-button fix. Owned by the media/calls work. |
| Composer and threads | IconButton (14 per delegate/panel) | Unchanged. |
| Pickers (emoji, GIF, sticker, file chooser) | IconButton and grid cells | Kept custom on purpose (grid choice cells are not buttons). Owned by the image-pack work. |
| Banners, toasts, dialogs | AppButton | Inherit the secondary change. NOT TESTED on coloured backgrounds. |

## Open

* A per-theme screenshot pass of enabled, hover, pressed, focus, disabled and
  busy on real surfaces (UI-060..063, UI-068): NOT TESTED.
* Compact preset and 200% text scale (UI-063): NOT TESTED.
