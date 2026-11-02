
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
    QCOMPARE(identityColor(key, 9), identityColor(key, 9));
    QVERIFY(identityColor(key, 9) != identityColor(key, 7));
    QCOMPARE(identityColor(key, 9),
             lightning::theme::discColor(
                 lightning::theme::identityIndex(key),
                 lightning::theme::anchorForTheme(9)));
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
    QCOMPARE(initialsFor(QStringLiteral("@mizerd:smetonis.net")),
             QStringLiteral("M"));
    QCOMPARE(initialsFor(QStringLiteral("#room:server")), QStringLiteral("R"));
    QCOMPARE(initialsFor(QStringLiteral("Rokas Smetonis")),
             QStringLiteral("RS"));
    QCOMPARE(initialsFor(QStringLiteral("  spaced   out  ")),
             QStringLiteral("SO"));
    QCOMPARE(initialsFor(QStringLiteral("lowercase")), QStringLiteral("L"));
    QCOMPARE(initialsFor(QString()), QStringLiteral("?"));
    QCOMPARE(initialsFor(QStringLiteral("   ")), QStringLiteral("?"));
    QCOMPARE(initialsFor(QStringLiteral("@")), QStringLiteral("?"));
    QCOMPARE(initialsFor(QString::fromUtf8("emoji \xf0\x9f\x9a\x80 room")),
             QStringLiteral("ER"));
    QCOMPARE(initialsFor(QStringLiteral("Beta (test) [room]")),
             QStringLiteral("BT"));
    QCOMPARE(initialsFor(QStringLiteral("Regex .* ^$ \\d+")),
             QStringLiteral("RD"));
    QCOMPARE(initialsFor(QStringLiteral("Alice <b>Bold</b> 053")),
             QStringLiteral("AB"));
    QCOMPARE(initialsFor(QString::fromUcs4(U"\U00010428x")),
             QString::fromUcs4(U"\U00010400"));
    QCOMPARE(initialsFor(QString::fromUtf8("\xf0\x9f\x9a\x80")),
             QStringLiteral("?"));
    QCOMPARE(initialsFor(QStringLiteral("(( ))")), QStringLiteral("?"));
}

void NotificationAvatarTest::discIsDrawnWithTheIdentityColour()
{
    const QImage image =
        fallbackAvatar(QStringLiteral("test"), QString(), 64, 9);
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(64, 64));

    const QColor expected = identityColor(QStringLiteral("test"), 9);
    const QColor sample = image.pixelColor(32, 8);
    QCOMPARE(sample.name().toUpper(), expected.name().toUpper());
    QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
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
    const QImage byKey = fallbackAvatar(
        QStringLiteral("test"), QStringLiteral("!abc:server.tld"), 64, 9);
    QCOMPARE(byKey.pixelColor(32, 8).name().toUpper(),
             identityColor(QStringLiteral("!abc:server.tld"), 9).name().toUpper());
    QVERIFY(byKey.pixelColor(32, 8).name().toUpper()
            != identityColor(QStringLiteral("test"), 9).name().toUpper());
}

void NotificationAvatarTest::nothingToDrawYieldsNoImage()
{
    QVERIFY(fallbackAvatar(QString(), QString(), 64, 9).isNull());
    QVERIFY(fallbackAvatar(QStringLiteral("test"), QString(), 0, 9).isNull());
}

QTEST_MAIN(NotificationAvatarTest)
#include "NotificationAvatarTest.moc"
