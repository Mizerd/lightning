
#include "app/WindowFrameTheme.h"

#include <QColor>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QtTest>

using lightning::windowframe::prefersDarkFrame;

class WindowFrameThemeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void darkAndLightColoursAreTold();
    void everyPaletteGetsTheFrameItsTextNeeds();
    void theRequestAlwaysFollowsQtsOwn();
};

void WindowFrameThemeTest::darkAndLightColoursAreTold()
{
    QVERIFY(prefersDarkFrame(QColor(Qt::black)));
    QVERIFY(!prefersDarkFrame(QColor(Qt::white)));
    QVERIFY(prefersDarkFrame(QColor(QStringLiteral("#313338"))));
    QVERIFY(!prefersDarkFrame(QColor(QStringLiteral("#F2F3F5"))));
    QVERIFY(prefersDarkFrame(QColor(QStringLiteral("#1A237E"))));
    QVERIFY(!prefersDarkFrame(QColor(QStringLiteral("#FFEB3B"))));
    QVERIFY(prefersDarkFrame(QColor(QStringLiteral("#0000FF"))));
    QVERIFY(prefersDarkFrame(QColor(QStringLiteral("#737373"))));
    QVERIFY(!prefersDarkFrame(QColor(QStringLiteral("#7A7A7A"))));
}

void WindowFrameThemeTest::everyPaletteGetsTheFrameItsTextNeeds()
{
#ifndef APPTHEME_QML_PATH
    QSKIP("APPTHEME_QML_PATH not defined for this target");
#else
    QFile file(QStringLiteral(APPTHEME_QML_PATH));
    QVERIFY2(file.open(QIODevice::ReadOnly), "AppTheme.qml unreadable");
    const QString source = QString::fromUtf8(file.readAll());

    QHash<QString, QColor> tokens;
    const QRegularExpression token(QStringLiteral(
        R"re(readonly\s+property\s+color\s+(_\w+)\s*:\s*"(#[0-9A-Fa-f]{6,8})")re"));
    for (auto it = token.globalMatch(source); it.hasNext();) {
        const auto m = it.next();
        tokens.insert(m.captured(1), QColor(m.captured(2)));
    }
    QVERIFY2(tokens.size() > 50, "the colour token scan found too little");

    const QRegularExpression palette(QStringLiteral(
        R"re(readonly\s+property\s+var\s+(_\w+)\s*:\s*\(\{(.*?)\}\))re"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression background(
        QStringLiteral(R"re(\bbackground\s*:\s*(_\w+))re"));
    const QRegularExpression text(
        QStringLiteral(R"re(\btextPrimary\s*:\s*(_\w+))re"));
    int checked = 0;
    int dark = 0;
    QStringList names;
    for (auto it = palette.globalMatch(source); it.hasNext();) {
        const auto m = it.next();
        const QString body = m.captured(2);
        const auto bg = background.match(body);
        const auto fg = text.match(body);
        if (!bg.hasMatch() || !fg.hasMatch())
            continue;
        if (!tokens.contains(bg.captured(1)) || !tokens.contains(fg.captured(1)))
            continue;
        const bool darkFrame = prefersDarkFrame(tokens.value(bg.captured(1)));
        QVERIFY2(darkFrame != prefersDarkFrame(tokens.value(fg.captured(1))),
                 qPrintable(QStringLiteral("%1: background %2 and text %3 ask "
                                           "for the same frame")
                                .arg(m.captured(1), bg.captured(1),
                                     fg.captured(1))));
        ++checked;
        dark += darkFrame ? 1 : 0;
        names << m.captured(1);
    }
    QVERIFY2(checked >= 8,
             qPrintable(QStringLiteral("only %1 palettes resolved: %2")
                            .arg(checked)
                            .arg(names.join(QLatin1Char(' ')))));
    QVERIFY2(dark > 0 && dark < checked,
             qPrintable(QStringLiteral("%1 of %2 palettes dark")
                            .arg(dark)
                            .arg(checked)));
#endif
}

void WindowFrameThemeTest::theRequestAlwaysFollowsQtsOwn()
{
    using lightning::windowframe::Reapply;
    using lightning::windowframe::reapplyFor;
    const QList<QEvent::Type> afterQt{
        QEvent::ApplicationPaletteChange,
        QEvent::ThemeChange,
        QEvent::PlatformSurface,
        QEvent::Show,
        QEvent::WindowStateChange,
    };
    for (const QEvent::Type type : afterQt)
        QVERIFY2(reapplyFor(type) == Reapply::AfterQt,
                 qPrintable(QStringLiteral("event %1 does not reapply")
                                .arg(int(type))));
    for (const QEvent::Type type : { QEvent::MouseMove, QEvent::Paint,
                                     QEvent::Timer, QEvent::Expose,
                                     QEvent::Resize })
        QVERIFY(reapplyFor(type) == Reapply::Ignore);
    QCOMPARE(QLatin1String(lightning::windowframe::frameModeName(true)),
             QLatin1String("dark"));
    QCOMPARE(QLatin1String(lightning::windowframe::frameModeName(false)),
             QLatin1String("light"));

    QFile src(QStringLiteral(WINDOWFRAME_SOURCE_PATH));
    QVERIFY2(src.open(QIODevice::ReadOnly), "WindowFrameTheme.cpp unreadable");
    const QString text = QString::fromUtf8(src.readAll());
    QVERIFY2(text.contains(QStringLiteral("QTimer::singleShot(0, this")),
             "the request is no longer deferred past Qt's own");
    QVERIFY2(text.contains(QStringLiteral("\"lightning.window\"")),
             "the caption log category is gone");
    QVERIFY2(!text.contains(QStringLiteral("case QEvent::ApplicationPaletteChange:\n"
                                           "            // Sent to the application")),
             "the palette change is applied inline again");
}

QTEST_GUILESS_MAIN(WindowFrameThemeTest)
#include "WindowFrameThemeTest.moc"
