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

} // namespace matrix::bridge
