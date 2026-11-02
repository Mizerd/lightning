
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
        QCOMPARE(link, QStringLiteral(
            "https://matrix.to/#/%23general:example.org"));
    }

    void fallsBackToRoomIdWhenAliasEmpty()
    {
        const QString link = RoomListModel::roomPermalink(
            QStringLiteral("!abc123:example.org"), QString());
        QCOMPARE(link, QStringLiteral(
            "https://matrix.to/#/!abc123:example.org"));
    }

    void fallsBackToRoomIdWhenAliasOmitted()
    {
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
        const QString link = RoomListModel::roomPermalink(
            QString(), QStringLiteral("#help:example.org"));
        QCOMPARE(link, QStringLiteral(
            "https://matrix.to/#/%23help:example.org"));
    }
};

QTEST_GUILESS_MAIN(RoomPermalinkTest)
#include "RoomPermalinkTest.moc"
