#pragma once

#include <QString>

// Recognising bridged conversations.
//
// Matrix has no first-class "network" field on a room, so this infers one
// for a unified inbox (WhatsApp, Signal, Telegram via mautrix-style bridges)
// from two identifiers the room list already holds: the DM partner's user id
// and the room's canonical alias.
//
// Conservative by design: ghosts follow a strict convention
// (`@whatsapp_<id>:server`, or `@_discord_<id>:server` for the
// matrix-appservice family) and are matched against a known network table, so
// an ordinary `@thomas_redstone:example.org` is not reported as a "thomas"
// bridge. Unrecognised prefixes return an empty id (no badge).
//
// Presentation only: never affects routing, sending, encryption or any
// protocol decision.
//
// Inference is effectively DM-only. `directUserId` comes from
// `Room::direct_targets()`, which matrix-sdk fills only from `m.direct`, and
// mautrix-family portals publish no canonical alias by default, so a bridged
// group has nothing to match on. Bridges do advertise themselves through
// MSC2346 `uk.half-shot.bridge` room state, but sliding sync does not carry
// that type, so reading it needs a raw /state request per room: fine on
// demand, not for a room-list badge. See AdvertisedBridgeLabel below and
// rust/src/bridges.rs.
namespace matrix::bridge {

// Canonical network id ("whatsapp", "signal", …) or an empty string when the
// identifier does not look like a bridge ghost. Accepts a full Matrix user
// id; a missing leading '@' is tolerated.
QString networkIdForUserId(const QString &userId);

// Same, for a room's canonical alias (`#whatsapp_<id>:server`). Empty when
// the alias is absent or unrecognised.
QString networkIdForAlias(const QString &alias);

// The room-level answer: the DM partner's id is the more specific signal and
// wins; the alias is the fallback for portal rooms and groups. Empty means
// not a bridged room as far as we can tell.
QString networkIdForRoom(const QString &directUserId,
                         const QString &canonicalAlias);

// Human-readable name for a canonical id ("whatsapp" -> "WhatsApp"). Empty
// for an unknown id, never a guess.
QString labelForNetworkId(const QString &networkId);

// Turns the MSC2346 answer into a badge, covering bridged groups that have no
// ghost mxid or portal alias to infer from. Rust reads and sanitises the
// state (rust/src/bridges.rs); this decides what may be shown.
//
// Precedence, which is security-relevant:
//   1. A protocol id the curated table knows gets our label; it must win.
//   2. Only an unknown id may show the bridge's own text, bounded to a chip.
//      That text is attacker-chosen (anyone able to send room state), which is
//      acceptable only because it sits beside the equally attacker-chosen room
//      name, renders as Text.PlainText (keep that in RoomDelegate.qml), is
//      stripped of control/bidi characters and names no user.
//   3. Neither: no label, no badge.
//
// `networkId` is the sanitised, lowercased protocol id, the same key the
// inference produces, so NetworkRole means one thing.
struct AdvertisedBridgeLabel {
    QString networkId;
    QString label;
};
AdvertisedBridgeLabel labelForAdvertisedBridge(const QString &protocolId,
                                               const QString &protocolName,
                                               const QString &networkName);

// Presentation repair for a bridged DM's computed name. A ghost localpart is
// machine identity, yet room naming degrades to it until the ghost's profile
// hydrates, and a bridged 1:1's hero summary counts bridge plumbing ("Sim,
// and 2 others").
//
//   .name         non-empty: show as-is (the human name without the member
//                 arithmetic, or the remote phone number when that is all
//                 the ghost id carries).
//   .networkLabel non-empty with .name empty: no humane name yet; the
//                 caller renders its own translated "<label> contact".
//
// Non-ghost names and non-bridged rooms pass through unchanged.
struct DmNamePresentation {
    QString name;
    QString networkLabel;
};
DmNamePresentation presentableDmName(const QString &computedName,
                                     const QString &directUserId);

} // namespace matrix::bridge
