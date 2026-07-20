#pragma once

#include <QString>

#include <functional>

// v0.7.1: sanitizer for Matrix `org.matrix.custom.html` formatted bodies.
//
// Incoming formatted message bodies are untrusted HTML. This turns them into
// a small, safe Qt RichText subset that the timeline can render:
//   * only an allowlist of inline/block formatting tags survives; every
//     other tag is dropped (its text content is kept);
//   * <script>/<style>/<iframe>/<svg>/<mx-reply>/... are dropped WITH their
//     content, so nothing from them is rendered;
//   * all attributes are stripped, EXCEPT a validated href on <a>: only
//     http(s) links survive as external links, and matrix.to user links are
//     rewritten to an internal "mention:<user-id>" link whose visible text is
//     the resolved display name (never a bare MXID). Every other scheme
//     (javascript:, data:, …) is dropped and the link renders as plain text;
//   * <img> is dropped entirely, so a formatted body can never make the
//     client fetch a remote/tracking image.
//
// The result contains no attributes that could load remote content or carry
// active behaviour; combined with the timeline's onLinkActivated (which only
// opens validated http(s) URLs and routes mention: links to a profile), it
// renders untrusted content without executing anything. Fail-closed: unknown
// or malformed markup is dropped, not passed through.
namespace MessageHtml {

// resolveDisplayName maps a Matrix user id to a room display name (empty or
// the id itself when unknown — the sanitizer falls back to the localpart).
// ownUserId, when it matches a mention target, marks a self-mention (bold).
QString sanitize(
    const QString &html,
    const std::function<QString(const QString &userId)> &resolveDisplayName,
    const QString &ownUserId);

} // namespace MessageHtml
