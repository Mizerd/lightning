// v0.6.5 (SPEC 1d): RoomListModel::roomPermalink is pure formatting — no
// server behavior — so it is unit-tested directly. It mirrors
// TimelineModel::messagePermalink's existing percent-encoding convention
// (! $ : @ excluded from encoding) so message and room links read
// consistently throughout the app.

#include <QtTest/QtTest>

#include "models/RoomListModel.h"

class RoomPermalinkTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aliasPreferredOverRoomId()
    {
        const QString link = RoomListModel::roomPermalink(
            QStringLiteral("!abc123:example.org"),
            QStringLiteral("#general:example.org"));
        // '#' is not in the !$:@ exclusion set, so it percent-encodes; ':'
        // and '.' are unaffected, matching messagePermalink's convention.
        QCOMPARE(link, QStringLiteral(
            "https://matrix.to/#/%23general:example.org"));
    }

    void fallsBackToRoomIdWhenAliasEmpty()
    {
        const QString link = RoomListModel::roomPermalink(
            QStringLiteral("!abc123:example.org"), QString());
        // '!' and ':' are both in the exclusion set, so a bare room id is
        // carried through unchanged apart from the matrix.to prefix.
        QCOMPARE(link, QStringLiteral(
            "https://matrix.to/#/!abc123:example.org"));
    }

    void fallsBackToRoomIdWhenAliasOmitted()
    {
        // The alias parameter defaults to empty when the caller has none.
        const QString link =
            RoomListModel::roomPermalink(QStringLiteral("!abc123:example.org"));
        QCOMPARE(link, QStringLiteral(
            "https://matrix.to/#/!abc123:example.org"));
    }

    void emptyRoomIdAndAliasProducesEmptyLink()
    {
        QVERIFY(RoomListModel::roomPermalink(QString(), QString()).isEmpty());
    }

    void emptyRoomIdWithAliasStillUsesAlias()
    {
        // A caller that only has the alias (no room id resolved yet) still
        // gets a usable link.
        const QString link = RoomListModel::roomPermalink(
            QString(), QStringLiteral("#help:example.org"));
        QCOMPARE(link, QStringLiteral(
            "https://matrix.to/#/%23help:example.org"));
    }
};

QTEST_GUILESS_MAIN(RoomPermalinkTest)
#include "RoomPermalinkTest.moc"
