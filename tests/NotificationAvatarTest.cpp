// v0.7.3: the initials disc a notification carries when an identity has no
// avatar.
//
// A sender without a picture produced a notification with no image hint at
// all, and the daemon substituted its own generic document glyph — reported
// from a real desktop, where a message from a user with no avatar looked
// like an unknown file attachment.
//
// The values asserted below were computed INDEPENDENTLY of the C++ (a
// separate implementation of AppTheme.qml's identityIndex, run over the same
// keys), so this suite pins agreement with the QML palette rather than
// restating whatever the C++ happens to do. If qml/AppTheme.qml's palette or
// hash changes, this fails — which is the point: the disc in a notification
// and the disc in the room list must be the same colour for the same person.

#include "notifications/FallbackAvatar.h"

#include <QImage>
#include <QtTest/QtTest>

using namespace lightning::notifications;

class NotificationAvatarTest : public QObject
{
    Q_OBJECT

private slots:
    void identityIndexMatchesTheQmlHash();
    void identityColourMatchesTheQmlPalette();
    void initialsFollowTheAvatarRule();
    void discIsDrawnWithTheIdentityColour();
    void colourKeyWinsOverTheDisplayName();
    void nothingToDrawYieldsNoImage();
};

void NotificationAvatarTest::identityIndexMatchesTheQmlHash()
{
    QCOMPARE(identityIndex(QStringLiteral("Mizerd")), 0);
    QCOMPARE(identityIndex(QStringLiteral("test")), 4);
    QCOMPARE(identityIndex(QStringLiteral("@test:mock.local")), 0);
    QCOMPARE(identityIndex(QStringLiteral("!abc:server.tld")), 2);
    QCOMPARE(identityIndex(QStringLiteral("alice")), 0);
    // Always inside the palette, including for input that drives the hash
    // negative — JavaScript's `| 0` wraps to int32 and Math.abs widens, so
    // the C++ must not narrow |INT32_MIN| back into an int.
    for (const QString &key : {QStringLiteral("ÿÿÿÿÿ"),
                               QStringLiteral("zzzzzzzzzzzzzzzz"),
                               QStringLiteral("中文名前")}) {
        const int index = identityIndex(key);
        QVERIFY2(index >= 0 && index < 9, qPrintable(key));
    }
}

void NotificationAvatarTest::identityColourMatchesTheQmlPalette()
{
    QCOMPARE(identityColor(QStringLiteral("Mizerd")).name().toUpper(),
             QStringLiteral("#D04339"));
    QCOMPARE(identityColor(QStringLiteral("test")).name().toUpper(),
             QStringLiteral("#2E8460"));
    QCOMPARE(identityColor(QStringLiteral("!abc:server.tld")).name().toUpper(),
             QStringLiteral("#8F7224"));
}

void NotificationAvatarTest::initialsFollowTheAvatarRule()
{
    // Matrix sigils stripped, so "@user:hs" reads as U rather than @.
    QCOMPARE(initialsFor(QStringLiteral("@mizerd:smetonis.net")),
             QStringLiteral("M"));
    QCOMPARE(initialsFor(QStringLiteral("#room:server")), QStringLiteral("R"));
    // Two words -> two initials.
    QCOMPARE(initialsFor(QStringLiteral("Rokas Smetonis")),
             QStringLiteral("RS"));
    QCOMPARE(initialsFor(QStringLiteral("  spaced   out  ")),
             QStringLiteral("SO"));
    QCOMPARE(initialsFor(QStringLiteral("lowercase")), QStringLiteral("L"));
    // Nothing to derive is an honest "?" rather than a blank disc.
    QCOMPARE(initialsFor(QString()), QStringLiteral("?"));
    QCOMPARE(initialsFor(QStringLiteral("   ")), QStringLiteral("?"));
    QCOMPARE(initialsFor(QStringLiteral("@")), QStringLiteral("?"));
}

void NotificationAvatarTest::discIsDrawnWithTheIdentityColour()
{
    const QImage image = fallbackAvatar(QStringLiteral("test"), QString(), 64);
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(64, 64));

    // Centre of the disc is the identity colour, or something drawn over it
    // (the initials are white) — sample a point inside the disc but away
    // from the glyph instead.
    const QColor expected = identityColor(QStringLiteral("test"));
    const QColor sample = image.pixelColor(32, 8);
    QCOMPARE(sample.name().toUpper(), expected.name().toUpper());
    // The corner is outside the circle and must stay transparent, so the
    // daemon composites a disc rather than a coloured square.
    QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    // Some white ink was actually drawn.
    bool sawInk = false;
    for (int y = 0; y < image.height() && !sawInk; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > 240 && pixel.green() > 240 && pixel.blue() > 240) {
                sawInk = true;
                break;
            }
        }
    }
    QVERIFY2(sawInk, "the initials were not drawn");
}

void NotificationAvatarTest::colourKeyWinsOverTheDisplayName()
{
    // Avatar.qml's _paletteKey: an explicit identity key is authoritative,
    // so two people who share a display name still get distinct discs.
    const QImage byKey =
        fallbackAvatar(QStringLiteral("test"), QStringLiteral("!abc:server.tld"), 64);
    QCOMPARE(byKey.pixelColor(32, 8).name().toUpper(),
             identityColor(QStringLiteral("!abc:server.tld")).name().toUpper());
    QVERIFY(byKey.pixelColor(32, 8).name().toUpper()
            != identityColor(QStringLiteral("test")).name().toUpper());
}

void NotificationAvatarTest::nothingToDrawYieldsNoImage()
{
    // A blank disc tells the user no more than the daemon's own placeholder,
    // so an identity with neither a name nor a key produces no image and the
    // notification keeps Lightning's ordinary icon.
    QVERIFY(fallbackAvatar(QString(), QString(), 64).isNull());
    QVERIFY(fallbackAvatar(QStringLiteral("test"), QString(), 0).isNull());
}

QTEST_MAIN(NotificationAvatarTest)
#include "NotificationAvatarTest.moc"
