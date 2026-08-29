#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

// Decorative profile badges — a Discord-style role tag beside a name.
//
// WHAT THIS IS. A thank-you. A badge here says "this person helped build
// Lightning" and nothing else. It is awarded by the maintainer, it is the same
// for every viewer, and it is stored in ONE table (`kBadges` in the .cpp) so
// that adding the second one is a data change rather than a code change. There
// is deliberately no `if (userId == "@someone:example.org")` anywhere in the
// presentation layer.
//
// WHAT THIS IS NOT, and the distinction matters more than the feature does:
//
//   * It is NOT a security signal. It says nothing about device verification,
//     cross-signing, or whether a session is trusted. Those live on the
//     Sessions page and on the verification surfaces, they come from SDK state,
//     and nothing here may ever be mistaken for them. Badges therefore carry
//     no shield, no check mark, no lock, and none of the trust palette.
//   * It is NOT a moderation signal. It says nothing about power levels,
//     administrators or moderators. Those already have their own chips in the
//     profile card, they are computed from the room's real power levels, and a
//     decorative tag must not compete with them.
//   * It is NOT Matrix state. It is not fetched, it is not published, no
//     homeserver knows about it, and no other client will show it. It is
//     honest local decoration, and the accessible description says so out loud
//     rather than leaving a screen-reader user to guess what an unexplained
//     tag beside a name means.
//
// The TINT is the holder's own identity ink (AppTheme.userColor, hashed from
// the MXID — the same colour their name and fallback avatar already use), so a
// badge cannot introduce a new colour whose meaning a reader has to learn, and
// two badge holders are told apart the same way two senders already are.
class ProfileBadges : public QObject
{
    Q_OBJECT

public:
    explicit ProfileBadges(QObject *parent = nullptr);

    struct Badge {
        QString userId;
        // The tag text, rendered verbatim. Short by construction — this is a
        // pill beside a name, not a sentence.
        QString label;
        // Spoken instead of the bare label, because "idea master" on its own
        // tells a screen-reader user nothing about what kind of claim is being
        // made. Never fabricated per-user: it is part of the table.
        QString description;
    };

    // "" when this user has no badge — which is every user but a handful, so
    // the empty answer is the ordinary one and must render as nothing at all.
    // Pure read, safe in a QML binding: the table is immutable and there is no
    // revision to observe.
    Q_INVOKABLE QString labelFor(const QString &userId) const;
    Q_INVOKABLE QString descriptionFor(const QString &userId) const;
    Q_INVOKABLE bool hasBadge(const QString &userId) const;
    // { label, description } — one lookup for a delegate that needs both, so a
    // binding does not walk the table twice.
    Q_INVOKABLE QVariantMap badgeFor(const QString &userId) const;

    // The table itself, for tests. Callers must not assume an order.
    static const QVector<Badge> &badges();
};
