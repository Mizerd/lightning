// The Windows title bar follows the app theme (live Windows test 2026-10-16:
// the caption stayed white under a dark theme). DWM itself cannot run here;
// what can is the decision it is given: whether the application palette's
// window colour (AppTheme.background, pushed by Main.qml's
// syncControlPalette()) asks for the dark frame. Checked against fixed
// colours and against every palette AppTheme.qml defines, read from source.

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
    // Saturated colours go by luminance, not by HSL lightness.
    QVERIFY(prefersDarkFrame(QColor(QStringLiteral("#1A237E"))));
    QVERIFY(!prefersDarkFrame(QColor(QStringLiteral("#FFEB3B"))));
    // Pure blue has HSL lightness 0.5 and luminance 0.07: dark.
    QVERIFY(prefersDarkFrame(QColor(QStringLiteral("#0000FF"))));
    // Either side of L* 50 (#777777 is L* 50.0, #7A7A7A 51.2).
    QVERIFY(prefersDarkFrame(QColor(QStringLiteral("#737373"))));
    QVERIFY(!prefersDarkFrame(QColor(QStringLiteral("#7A7A7A"))));
}

// A palette's background must ask for the frame its own primary text is NOT:
// dark text on a light ground wants the light caption, and the reverse. Read
// from AppTheme.qml so a new or retuned palette is covered without editing
// this test.
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
    // Assert what varied: palettes on both sides of the line.
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

// Live FAIL on pipeline 326: the request was made inside an application event
// filter, which runs before Qt's Windows plugin answers the same event with the
// SYSTEM scheme (QWindowsWindow::windowEvent on ApplicationPaletteChange), so a
// light system under a dark theme kept a light caption. The cure is that every
// event after which Qt may rewrite the caption schedules the request AFTER Qt.
// DWM cannot run here; the decision table is what can be checked.
void WindowFrameThemeTest::theRequestAlwaysFollowsQtsOwn()
{
    using lightning::windowframe::Reapply;
    using lightning::windowframe::reapplyFor;
    const QList<QEvent::Type> afterQt{
        QEvent::ApplicationPaletteChange, // Qt sets the frame from the system
        QEvent::ThemeChange,              // ... and when the system changes
        QEvent::PlatformSurface,          // ... and at native creation
        QEvent::Show,
        QEvent::WindowStateChange,
    };
    for (const QEvent::Type type : afterQt)
        QVERIFY2(reapplyFor(type) == Reapply::AfterQt,
                 qPrintable(QStringLiteral("event %1 does not reapply")
                                .arg(int(type))));
    // The decision is deliberately narrow: pointer, paint and timer traffic
    // must not each cost a DWM call.
    for (const QEvent::Type type : { QEvent::MouseMove, QEvent::Paint,
                                     QEvent::Timer, QEvent::Expose,
                                     QEvent::Resize })
        QVERIFY(reapplyFor(type) == Reapply::Ignore);
    QCOMPARE(QLatin1String(lightning::windowframe::frameModeName(true)),
             QLatin1String("dark"));
    QCOMPARE(QLatin1String(lightning::windowframe::frameModeName(false)),
             QLatin1String("light"));

    // The Windows branch must not apply inline from the event filter: the
    // only application of the request outside SurfaceCreated is the timer.
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
