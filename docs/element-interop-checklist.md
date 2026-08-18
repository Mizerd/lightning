# Element interoperability — scripted live-validation checklist

Purpose: convert the largest pile of unknown risk (the ~24 "NOT TESTED"
admissions across CLAUDE.md §7/§16) into either confidence or a bug list, in
one scripted pass. This is not exploratory clicking: run the steps in order,
record **PASS**, **FAIL**, or **NOT TESTED** per line, and keep the filled-in
copy. A FAIL needs one line of symptom next to it, nothing more — deeper
diagnosis happens later, with this sheet as the index.

Rules (CLAUDE.md §12 applies):

- Compilation, launch, or automated suites prove nothing here. Only the
  exercised interaction counts.
- Never mark a line PASS because the adjacent line passed.
- If a step cannot be run (no second device, no mic), mark it NOT TESTED —
  an honest gap beats a hopeful checkmark.
- Do not paste tokens, recovery keys, or event content into the results.

## Setup (record before starting)

| Field | Value |
|---|---|
| Date / Lightning version / commit | |
| Homeserver (e.g. matrix.org) | |
| Lightning account | |
| Element account (other side) | |
| Element client + version (Web/Desktop/X) | |
| Room used (encrypted, both joined) | |

Preparation:

1. Create or reuse one **encrypted** room with both accounts joined.
2. Verify the Lightning session (SAS or QR) so cross-signing is green on
   both sides before starting — key-recovery is tested separately at the end.
3. Keep Element visible on a second screen/device so both directions are
   observed as they happen.

## A. Encrypted send/receive, both directions

| # | Step | Result | Notes |
|---|---|---|---|
| A1 | Lightning → Element: plain text message decrypts and renders in Element | | |
| A2 | Element → Lightning: plain text message decrypts and renders in Lightning | | |
| A3 | Lightning → Element: markdown message (bold, code span, list) renders formatted in Element | | |
| A4 | Element → Lightning: formatted message renders formatted in Lightning | | |
| A5 | Lightning → Element: rich reply — Element shows the quoted target | | |
| A6 | Element → Lightning: rich reply — Lightning shows the quoted target | | |

## B. Threads

| # | Step | Result | Notes |
|---|---|---|---|
| B1 | Element creates a thread on a Lightning message; the reply appears in Lightning's thread panel, NOT as a standalone main-timeline row | | |
| B2 | Lightning replies in that thread; Element shows it inside the same thread | | |
| B3 | Lightning's main timeline shows the root's summary card with a correct reply count after both replies | | |
| B4 | Lightning sends an image attachment into the thread; Element shows it in-thread, decrypted | | |

## C. Media

| # | Step | Result | Notes |
|---|---|---|---|
| C1 | Lightning → Element: image in the encrypted room decrypts and displays | | |
| C2 | Lightning → Element: **video** — Element shows a poster thumbnail before playback (the send-side extracted poster) | | |
| C3 | Element plays that video; duration/dimensions look sane | | |
| C4 | Element → Lightning: video with thumbnail — Lightning shows the poster, then plays inline | | |
| C5 | Lightning → Element: **voice message** (room composer mic) — Element renders it as a voice message (waveform + duration), and it plays | | |
| C6 | Element → Lightning: voice message renders as voice (not a bare file) and plays | | |
| C7 | Lightning → Element: provider GIF (GIPHY/KLIPY picker) into the encrypted room — Element decrypts and animates it | | |

## D. Reactions, edits, redactions

| # | Step | Result | Notes |
|---|---|---|---|
| D1 | Lightning reacts to an Element message; the reaction appears in Element | | |
| D2 | Element reacts to a Lightning message; the chip appears in Lightning | | |
| D3 | Rapidly toggle the same reaction ~10× in Lightning; final state matches on both sides, and Lightning stays responsive (the 2026-08-18 freeze report — run with `LIGHTNING_GUI_STALL_TRACE=1` and attach any stall lines) | | |
| D4 | Lightning edits its own message; Element shows the edited body (and edit marker) | | |
| D5 | Element edits a message; Lightning replaces it in place, same row | | |
| D6 | Lightning redacts its own message; it tombstones on both sides | | |
| D7 | Element redacts a message Lightning has on screen; Lightning updates in place | | |

## E. Pins

| # | Step | Result | Notes |
|---|---|---|---|
| E1 | Lightning pins a message; Element's pinned-messages list shows it | | |
| E2 | Element pins a different message; Lightning's Room Info → Pinned updates **without a restart** (live `m.room.pinned_events` via the active-room subscription) | | |
| E3 | Lightning unpins; both sides converge | | |

## F. Read receipts and notifications (adjacent, cheap to check here)

| # | Step | Result | Notes |
|---|---|---|---|
| F1 | Element reads up to the latest message; Lightning shows Element's receipt chip on the right row | | |
| F2 | Lightning reads; Element's read indicator moves | | |
| F3 | With the room NOT open in Lightning, an Element message raises exactly one desktop notification; clicking it lands on the message | | |

## G. Key-recovery cycle after a fresh login

This is the E2EE claim that matters most and has never been live-proven.

| # | Step | Result | Notes |
|---|---|---|---|
| G1 | Confirm key backup exists (Settings → Privacy & security shows a usable backup) before signing out | | |
| G2 | Sign out of Lightning completely | | |
| G3 | Sign back in (fresh session). Pre-verification, the encrypted room's history shows undecryptable placeholders — not crashes | | |
| G4 | Verify the new session (SAS or QR against Element, either direction) | | |
| G5 | After verification + backup restore, the historical encrypted messages decrypt **in place, without restart or manual Retry** | | |
| G6 | New messages sent from Element to the fresh session decrypt immediately | | |
| G7 | QR path specifically (if not used in G4): Lightning shows the QR, Element scans it, flow completes and trust is reflected on both sides | | |

## Wrap-up

- File one issue (or note) per FAIL line, referencing its line ID.
- Save this filled sheet next to the release notes
  (`docs/validation/<date>-element-interop.md` is a fine home).
- Update the NOT TESTED claims in CLAUDE.md §7 for every line that is now
  PASS/FAIL — that is the whole point of the exercise.
