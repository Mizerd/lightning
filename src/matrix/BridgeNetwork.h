#pragma once

#include <QString>

// Recognising bridged conversations.
//
// A Matrix client that is being used as a unified inbox — WhatsApp, Signal,
// Telegram and friends arriving through mautrix-style bridges — needs to
// tell the user which network a chat actually belongs to. Matrix itself
// carries no first-class "network" field on a room, so this derives one from
// the two identifiers the room list already holds: the other party's user id
// in a DM, and the room's canonical alias for a portal room.
//
// Deliberately conservative. Bridge ghosts follow a strict convention
// (`@whatsapp_<id>:server`, or `@_discord_<id>:server` for the
// matrix-appservice family), and matching is against a KNOWN network table
// rather than "anything before the first underscore" — otherwise a perfectly
// ordinary `@thomas_redstone:example.org` would be reported as a bridged
// account on the "thomas" network. An unrecognised prefix returns an empty
// id, which the UI renders as no badge at all.
//
// This is presentation metadata only. It never affects routing, sending,
// encryption, or any protocol decision, and a wrong answer costs a wrong
// label and nothing else.
//
// WHY THE BADGE IS EFFECTIVELY DM-ONLY (established 2026-09-06 from a tester
// report: "it only shows on DMs for me"). It is not written as a DM test —
// it is written as "ghost mxid first, canonical alias second" — but only one
// of those two inputs is ever populated in practice:
//
//   * `directUserId` comes from `Room::direct_targets()`, which matrix-sdk
//     fills ONLY from the `m.direct` global account data. So the ghost mxid
//     — the reliable signal — reaches this function for a room the account
//     has marked as a direct chat, and for nothing else. A bridged GROUP has
//     ghosts all through its member list and this never looks there.
//   * `canonicalAlias` is populated for every room, and the alias branch
//     works (it is unit-tested), but mautrix-family bridges do not publish a
//     canonical alias for portal rooms by default, so it is empty.
//
// A bridged room that is not a DM therefore has nothing to match on. It is
// NOT that the bridge fails to advertise itself: it advertises through
// MSC2346 `uk.half-shot.bridge` room state, which carries the protocol id and
// display name explicitly and is what Element reads for its Bridge Info
// panel. Lightning has never read that event.
//
// Reading it is a real change rather than a line: `Room::get_state_events` is
// STORE-ONLY in matrix-sdk 0.18 and `uk.half-shot.bridge` is not in sliding
// sync's `required_state` (which `RoomListService::subscribe_to_rooms` gives
// no way to extend), so the store answer is empty for every room and it would
// need a raw `/state` read — exactly the shape `widgets.rs` and `banner.rs`
// already have. One request per room is fine on demand in the room info
// panel; it is not fine for a room-list badge, which is where the badge
// currently lives (`qml/RoomDelegate.qml`, and nowhere else — the Channels
// navigation layout's `ChannelDelegate.qml` has no network tag either).
namespace matrix::bridge {

// Canonical network id ("whatsapp", "signal", …) or an empty string when the
// identifier does not look like a bridge ghost. Accepts a full Matrix user
// id; a missing leading '@' is tolerated.
QString networkIdForUserId(const QString &userId);

// Same, for a room's canonical alias (`#whatsapp_<id>:server`). Empty when
// the alias is absent or unrecognised.
QString networkIdForAlias(const QString &alias);

// The room-level answer: the DM partner's id wins, because it is the more
// specific signal, and the alias is the fallback for portal rooms and
// groups. Empty means "not a bridged room as far as we can tell", which is
// also what a native Matrix room returns.
QString networkIdForRoom(const QString &directUserId,
                         const QString &canonicalAlias);

// Human-readable name for a canonical id ("whatsapp" -> "WhatsApp"). Returns
// an empty string for an unknown id, never a guess: a badge reading
// "Whatsapp" or "Gvoice" is worse than no badge.
QString labelForNetworkId(const QString &networkId);

// The MSC2346 answer, turned into a badge.
//
// This is what closes the DM-only gap described above: a bridged GROUP has no
// ghost mxid and no portal alias to infer from, but it does carry
// `uk.half-shot.bridge` room state naming the protocol. The Rust side reads
// and sanitises that (rust/src/bridges.rs); this decides what it is allowed
// to say.
//
// PRECEDENCE, and it is security-relevant:
//
//   1. A protocol id the curated table above knows gets OUR label. That is
//      the high-confidence path and it must win — "whatsapp" reads as
//      "WhatsApp" whatever the bridge would have liked it to say.
//   2. ONLY for an id the table does not know may the bridge's own text be
//      shown, bounded to a chip's worth. That text is written by anyone with
//      the power level to send room state, so it is attacker-chosen — which
//      is acceptable here for one specific reason: it renders in a muted chip
//      beside the room NAME, which is equally attacker-chosen and far more
//      prominent, and it goes through `Text.PlainText` (RoomDelegate.qml
//      already sets it; keep it). It is bounded, stripped of control and
//      bidi characters in Rust, and it names no user.
//   3. Neither available: no label, which the UI renders as no badge.
//
// `networkId` is the sanitised, lowercased protocol id — the same key the
// inference produces for a known network, so NetworkRole means one thing.
struct AdvertisedBridgeLabel {
    QString networkId;
    QString label;
};
AdvertisedBridgeLabel labelForAdvertisedBridge(const QString &protocolId,
                                               const QString &protocolName,
                                               const QString &networkName);

// Presentation repair for a bridged DM's computed name.
//
// A ghost localpart is machine identity, never a human name — but it is
// exactly what the room-naming algorithm degrades to while the ghost's
// profile has not hydrated ("linkedin___a_co_a_a…"), and the hero summary
// of a bridged 1:1 additionally counts the bridge plumbing ("Sim, and 2
// others"). This returns what the row should actually say:
//
//   .name        non-empty: show it as-is — the human name with the
//                membership arithmetic stripped, or the remote phone number
//                when that is all the ghost id carries and it reads as one.
//   .networkLabel non-empty (with .name empty): no humane name exists yet;
//                the caller renders its own "<label> contact" placeholder
//                (the caller owns the translation).
//
// A name that is not a ghost id passes through untouched, and a
// non-bridged directUserId returns the input unchanged — native Matrix
// rooms cannot be affected.
struct DmNamePresentation {
    QString name;
    QString networkLabel;
};
DmNamePresentation presentableDmName(const QString &computedName,
                                     const QString &directUserId);

} // namespace matrix::bridge
