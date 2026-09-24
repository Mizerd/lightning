#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

// Decorative profile badges: a role tag beside a name, thanking people who
// helped build Lightning. Awarded by the maintainer, identical for every
// viewer, and stored in one table (`kBadges` in the .cpp); no user id is ever
// special-cased in the presentation layer.
//
// What it is not:
//   * a security signal: nothing about verification or trust, so no shield,
//     check mark, lock or trust colours;
//   * a moderation signal: power levels have their own chips;
//   * Matrix state: not fetched or published, invisible to other clients. The
//     accessible description says so.
//
// The tint is the holder's identity colour (AppTheme.userColor), so badges
// introduce no new colour meaning.
class ProfileBadges : public QObject
{
    Q_OBJECT

public:
    explicit ProfileBadges(QObject *parent = nullptr);

    struct Badge {
        QString userId;
        // Rendered verbatim; a short pill, not a sentence.
        QString label;
        // Spoken instead of the bare label, which alone says nothing about the
        // kind of claim. Part of the table, never per user.
        QString description;
    };

    // "" for almost every user, rendering as nothing. Pure read; the table is
    // immutable.
    Q_INVOKABLE QString labelFor(const QString &userId) const;
    Q_INVOKABLE QString descriptionFor(const QString &userId) const;
    Q_INVOKABLE bool hasBadge(const QString &userId) const;
    // { label, description } in one lookup.
    Q_INVOKABLE QVariantMap badgeFor(const QString &userId) const;

    // The table, for tests; unordered.
    static const QVector<Badge> &badges();
};
