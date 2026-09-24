#include "profile/ProfileBadges.h"

#include <QCoreApplication>

namespace {

// The table: adding a badge is one row. User ids are compared exactly as
// written (localparts are case-sensitive); guessing at equivalence could give
// someone else's badge away.
const QVector<ProfileBadges::Badge> &table()
{
    static const QVector<ProfileBadges::Badge> kBadges = {
        {
            QStringLiteral("@romanticanimegerl:cutefunny.art"),
            QStringLiteral("idea master"),
            // Spelled out because readers may assume a tag is a permission or
            // verification state; it is neither.
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
