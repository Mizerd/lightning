#include "profile/ProfileBadges.h"

#include <QCoreApplication>

namespace {

// THE TABLE. Adding a badge is a row here and nothing else.
//
// Matrix user ids are case-sensitive in their localpart, so these are compared
// exactly as written; no normalisation is attempted, because guessing at
// equivalence between two ids is how the wrong person gets somebody else's
// badge.
const QVector<ProfileBadges::Badge> &table()
{
    static const QVector<ProfileBadges::Badge> kBadges = {
        {
            QStringLiteral("@romanticanimegerl:cutefunny.art"),
            QStringLiteral("idea master"),
            // Spelled out because a decorative tag beside a name is exactly
            // the kind of thing a reader assumes is a permission or a
            // verification state. It is neither, and the accessible name says
            // so instead of relying on the visual treatment to imply it.
            QCoreApplication::translate(
                "ProfileBadges",
                "idea master — a thank-you badge for helping develop "
                "Lightning. Not a moderation role and not a verification "
                "status."),
        },
    };
    return kBadges;
}

} // namespace

ProfileBadges::ProfileBadges(QObject *parent)
    : QObject(parent)
{
}

const QVector<ProfileBadges::Badge> &ProfileBadges::badges()
{
    return table();
}

QString ProfileBadges::labelFor(const QString &userId) const
{
    if (userId.isEmpty())
        return {};
    for (const Badge &badge : table()) {
        if (badge.userId == userId)
            return badge.label;
    }
    return {};
}

QString ProfileBadges::descriptionFor(const QString &userId) const
{
    if (userId.isEmpty())
        return {};
    for (const Badge &badge : table()) {
        if (badge.userId == userId)
            return badge.description;
    }
    return {};
}

bool ProfileBadges::hasBadge(const QString &userId) const
{
    return !labelFor(userId).isEmpty();
}

QVariantMap ProfileBadges::badgeFor(const QString &userId) const
{
    if (userId.isEmpty())
        return {};
    for (const Badge &badge : table()) {
        if (badge.userId != userId)
            continue;
        return QVariantMap{
            { QStringLiteral("label"), badge.label },
            { QStringLiteral("description"), badge.description },
        };
    }
    return {};
}
