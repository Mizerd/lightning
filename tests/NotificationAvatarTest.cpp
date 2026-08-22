// v0.7.3: the initials disc a notification carries when an identity has no
// avatar.
//
// A sender without a picture produced a notification with no image hint at
// all, and the daemon substituted its own generic document glyph — reported
// from a real desktop, where a message from a user with no avatar looked
// like an unknown file attachment.
//
// The hash indices below were computed INDEPENDENTLY of the C++ (a separate
// implementation of AppTheme.qml's identityIndex, run over the same keys), so
// that half of the agreement is pinned rather than restated.
//
// The COLOUR is no longer a fixed palette to restate: since 2026-08-22 the
// discs are derived from the active theme's accent, by the one implementation
// in lightning::theme that AppTheme.qml also calls. What is worth asserting
// about it is therefore behavioural — the disc follows the theme, it is
// stable within a theme, the painter uses the palette's own answer, and the
// initials are drawn in an ink the disc can actually carry.

#include "notifications/FallbackAvatar.h"
#include "theme/IdentityColors.h"

#include <QImage>
#include <QtTest/QtTest>

#include <cmath>

using namespace lightning::notifications;

class NotificationAvatarTest : public QObject
{
    Q_OBJECT

private slots:
    void identityIndexMatchesTheQmlHash();
    void identityColourFollowsTheTheme();
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

void NotificationAvatarTest::identityColourFollowsTheTheme()
{
    const QString key = QStringLiteral("!abc:server.tld");
    // Stable within a theme: the same person must not change colour between
    // two notifications.
    QCOMPARE(identityColor(key, 9), identityColor(key, 9));
    // ...and different between themes, which is the whole point of the
    // change: a warm disc on a deep indigo window was the report.
    QVERIFY(identityColor(key, 9) != identityColor(key, 7));
    // The notification painter and the palette agree. This is the assertion
    // the old hand-kept array copy existed to protect, and it now holds by
    // construction rather than by two files being edited together.
    QCOMPARE(identityColor(key, 9),
             lightning::theme::discColor(
                 lightning::theme::identityIndex(key),
                 lightning::theme::anchorForTheme(9)));
    // Every theme, every slot: an ink that clears 4.5:1 on its disc.
    for (int theme = 1; theme <= 11; ++theme) {
        const QColor anchor = lightning::theme::anchorForTheme(theme);
        for (int slot = 0; slot < lightning::theme::kIdentitySlots; ++slot) {
            const QColor disc = lightning::theme::discColor(slot, anchor);
            const QColor ink = lightning::theme::discInk(slot, anchor);
            const auto lum = [](const QColor &c) {
                const auto f = [](double v) {
                    return v <= 0.03928 ? v / 12.92
                                        : std::pow((v + 0.055) / 1.055, 2.4);
                };
                return 0.2126 * f(c.redF()) + 0.7152 * f(c.greenF())
                     + 0.0722 * f(c.blueF());
            };
            const double a = lum(disc);
            const double b = lum(ink);
            const double ratio =
                (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
            QVERIFY2(ratio >= 4.5,
                     qPrintable(QStringLiteral(
                         "theme %1 slot %2: %3 on %4 is %5:1")
                         .arg(theme).arg(slot).arg(ink.name(), disc.name())
                         .arg(ratio, 0, 'f', 2)));
        }
    }
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
    const QImage image =
        fallbackAvatar(QStringLiteral("test"), QString(), 64, 9);
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(64, 64));

    // Centre of the disc is the identity colour, or something drawn over it
    // (the initials are white) — sample a point inside the disc but away
    // from the glyph instead.
    const QColor expected = identityColor(QStringLiteral("test"), 9);
    const QColor sample = image.pixelColor(32, 8);
    QCOMPARE(sample.name().toUpper(), expected.name().toUpper());
    // The corner is outside the circle and must stay transparent, so the
    // daemon composites a disc rather than a coloured square.
    QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    // Ink was actually drawn — in the colour THIS disc carries, which is
    // not always white now that half the slots are pale.
    const QColor ink = lightning::theme::discInk(
        lightning::theme::identityIndex(QStringLiteral("test")),
        lightning::theme::anchorForTheme(9));
    bool sawInk = false;
    for (int y = 0; y < image.height() && !sawInk; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > 200
                && qAbs(pixel.red() - ink.red()) < 24
                && qAbs(pixel.green() - ink.green()) < 24
                && qAbs(pixel.blue() - ink.blue()) < 24) {
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
    const QImage byKey = fallbackAvatar(
        QStringLiteral("test"), QStringLiteral("!abc:server.tld"), 64, 9);
    QCOMPARE(byKey.pixelColor(32, 8).name().toUpper(),
             identityColor(QStringLiteral("!abc:server.tld"), 9).name().toUpper());
    QVERIFY(byKey.pixelColor(32, 8).name().toUpper()
            != identityColor(QStringLiteral("test"), 9).name().toUpper());
}

void NotificationAvatarTest::nothingToDrawYieldsNoImage()
{
    // A blank disc tells the user no more than the daemon's own placeholder,
    // so an identity with neither a name nor a key produces no image and the
    // notification keeps Lightning's ordinary icon.
    QVERIFY(fallbackAvatar(QString(), QString(), 64, 9).isNull());
    QVERIFY(fallbackAvatar(QStringLiteral("test"), QString(), 0, 9).isNull());
}

QTEST_MAIN(NotificationAvatarTest)
#include "NotificationAvatarTest.moc"
