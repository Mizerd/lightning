// Custom-theme contract.
//
// The store is the gate between a config file a user can hand-edit and the
// renderer, so most of this is about what it REFUSES. The rest pins the two
// couplings that would fail silently: the role table against AppTheme's
// palette keys, and theme id 12 against the routing in AppTheme.qml.

#include "app/CustomThemeStore.h"
#include "app/SettingsManager.h"

#include <QColor>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>
#include <QHash>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QString appTheme()
{
    QFile f(QStringLiteral(APPTHEME_QML_PATH));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

// Every key any palette object in AppTheme.qml defines. The custom palette is
// a merge over one of these, so a role the editor offers must be a key one of
// them uses — otherwise the override lands in the map and paints nothing.
QSet<QString> paletteKeys(const QString &qml)
{
    QSet<QString> keys;
    static const QRegularExpression block(
        QStringLiteral("readonly\\s+property\\s+var\\s+(_\\w+):\\s*\\(\\{(.*?)\\}\\)"),
        QRegularExpression::DotMatchesEverythingOption);
    auto it = block.globalMatch(qml);
    while (it.hasNext()) {
        const auto m = it.next();
        QString body = m.captured(2);
        body.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        static const QRegularExpression key(QStringLiteral("(\\w+)\\s*:"));
        auto kit = key.globalMatch(body);
        while (kit.hasNext())
            keys.insert(kit.next().captured(1));
    }
    return keys;
}

// The ELEVEN SHIPPED PRESETS, resolved the way AppTheme.paletteForTheme()
// resolves them, so the readability table can be calibrated against real
// themes rather than against an opinion.
//
// Two stages, because AppTheme writes its palettes as maps of NAMED colour
// properties rather than of literals: collect every
// `readonly property color _x: "#RRGGBB"` first, then read each
// `readonly property var _theme: ({ ... })` block through that map.
//
// WHAT THIS DOES NOT RESOLVE, and why that is safe: a handful of Storm
// entries are computed (`Qt.darker(...)`) rather than named, and this parser
// leaves those out. A check whose endpoint is missing is SKIPPED, never
// counted as a pass — and `everyReadabilityCheckPassesOnEveryShippedPreset`
// asserts the number of checks it actually evaluated, so the parser silently
// resolving less over time turns into a failure instead of into a quieter
// guarantee. That is the recurring shape §16 records: a sweep that comes back
// short is the defect, not its symptom.
QHash<QString, QHash<QString, QString>> presetPalettes(const QString &qml)
{
    QHash<QString, QString> literals;
    static const QRegularExpression lit(
        QStringLiteral("readonly\\s+property\\s+color\\s+(_\\w+):\\s*"
                       "\"(#[0-9A-Fa-f]{6})\""));
    auto lt = lit.globalMatch(qml);
    while (lt.hasNext()) {
        const auto m = lt.next();
        literals.insert(m.captured(1), m.captured(2).toUpper());
    }

    // The eleven palette blocks, by the theme id each one is switched to in
    // rawPaletteForTheme().
    const QHash<QString, QString> names{
        {QStringLiteral("_light"), QStringLiteral("Lightning Light")},
        {QStringLiteral("_dark"), QStringLiteral("Lightning Dark")},
        {QStringLiteral("_graphite"), QStringLiteral("Graphite")},
        {QStringLiteral("_midnight"), QStringLiteral("Midnight")},
        {QStringLiteral("_nord"), QStringLiteral("Nordic")},
        {QStringLiteral("_purple"), QStringLiteral("Purple Dusk")},
        {QStringLiteral("_warm"), QStringLiteral("Warm")},
        {QStringLiteral("_moss"), QStringLiteral("Moss Light")},
        {QStringLiteral("_indigo"), QStringLiteral("Indigo Night")},
        {QStringLiteral("_teal"), QStringLiteral("Deep Teal")},
        {QStringLiteral("_storm"), QStringLiteral("Storm")}};

    QHash<QString, QHash<QString, QString>> out;
    static const QRegularExpression block(
        QStringLiteral("readonly\\s+property\\s+var\\s+(_\\w+):\\s*\\(\\{"
                       "(.*?)\\n    \\}\\)"),
        QRegularExpression::DotMatchesEverythingOption);
    auto bt = block.globalMatch(qml);
    while (bt.hasNext()) {
        const auto m = bt.next();
        if (!names.contains(m.captured(1)))
            continue;
        QString body = m.captured(2);
        body.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        QHash<QString, QString> palette;
        static const QRegularExpression entry(
            QStringLiteral("(\\w+)\\s*:\\s*([_\\w]+|\"#[0-9A-Fa-f]{6}\")"));
        auto et = entry.globalMatch(body);
        while (et.hasNext()) {
            const auto e = et.next();
            const QString value = e.captured(2);
            if (value.startsWith(QLatin1Char('"')))
                palette.insert(e.captured(1), value.mid(1, 7).toUpper());
            else if (literals.contains(value))
                palette.insert(e.captured(1), literals.value(value));
        }
        out.insert(names.value(m.captured(1)), palette);
    }
    return out;
}

// One preset as the object AppTheme.paletteForTheme() hands QML: the block's
// own entries under their SEMANTIC names, plus the same fallbacks that
// function applies. The store's audit reads that object, not the raw block,
// so the test has to speak the same dialect.
QVariantMap resolvedPalette(const QHash<QString, QString> &raw,
                            const QString &onAccent)
{
    QVariantMap p;
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it)
        p.insert(it.key(), it.value());
    const auto fallback = [&](const char *key, const QString &value) {
        if (!p.contains(QLatin1String(key)) && !value.isEmpty())
            p.insert(QLatin1String(key), value);
    };
    // paletteForTheme's own spellings and defaults, in its order.
    if (raw.contains(QStringLiteral("inputBg")))
        p.insert(QStringLiteral("inputBackground"),
                 raw.value(QStringLiteral("inputBg")));
    p.insert(QStringLiteral("reactionBackground"),
             raw.value(QStringLiteral("reaction"),
                       raw.value(QStringLiteral("cardElevated"))));
    // ownBubbleText is a LITERAL in AppTheme (#FFFFFF), not a palette entry:
    // the only way to fix a failure against it is to move the bubble.
    p.insert(QStringLiteral("ownBubbleText"), QStringLiteral("#FFFFFF"));
    fallback("rail", raw.value(QStringLiteral("sidebar")));
    fallback("accentText", onAccent);
    fallback("link", raw.value(QStringLiteral("accent")));
    fallback("selectedText", raw.value(QStringLiteral("textPrimary")));
    fallback("otherBubble", raw.value(QStringLiteral("cardElevated")));
    return p;
}

const QString kAlice = QStringLiteral("@alice:one.example");
const QString kBob = QStringLiteral("@bob:one.example");
const QString kHs = QStringLiteral("https://one.example");

} // namespace

class CustomThemeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // A NOTIFY NOTHING EMITS IS A CONSTANT WEARING A SIGNAL'S CLOTHES.
    //
    // `roles` is declared NOTIFY rolesChanged, and its header says the
    // property is "re-read on a language change rather than being CONSTANT".
    // Nothing emitted rolesChanged anywhere in the tree, so that described an
    // intention and not the code: roles() builds its labels and group names
    // with tr(), QML's engine.retranslate() does not reach strings a C++
    // model has already turned into data, and the theme editor's labels kept
    // the old language until it was reopened.
    void roleLabelsAreAnnouncedOnALanguageChange()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        QVERIFY2(!store.roles().isEmpty(),
                 "fixture assumption: the store exposes editable roles");

        QSignalSpy spy(&store, &CustomThemeStore::rolesChanged);
        const QString before = settings.language();
        settings.setLanguage(before == QLatin1String("lt")
                                 ? QStringLiteral("en")
                                 : QStringLiteral("lt"));
        QCOMPARE(spy.count(), 1);
        settings.setLanguage(before);
    }

    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("custom-theme-test"));
    }

    // Every case starts from an empty config. Before the collection landed
    // each case happened to overwrite the same three keys, so leakage was
    // invisible; a stored LIST survives into the next case and made
    // aStoredBaseOutsideTheRangeFallsBack read the previous case's theme.
    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // ---- what it refuses ------------------------------------------------

    void onlyOpaqueSixDigitHexIsAccepted()
    {
        QVERIFY(CustomThemeStore::colorIsValid(QStringLiteral("#1D57FF")));
        QVERIFY(CustomThemeStore::colorIsValid(QStringLiteral("#abcdef")));

        // An 8-digit ARGB is refused deliberately, not by oversight: a
        // translucent SHELL surface composites over whatever is behind it,
        // which makes the resulting contrast unknowable, and every contrast
        // rule in this application is written against opaque values.
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("#801D57FF")));
        // Named colours would bypass the format entirely.
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("red")));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("transparent")));
        // Shorthand, missing hash, wrong length, injection attempts.
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("#FFF")));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("1D57FF")));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("#1D57F")));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("#1D57FFF")));
        QVERIFY(!CustomThemeStore::colorIsValid(QString()));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("#GGGGGG")));
        QVERIFY(!CustomThemeStore::colorIsValid(
            QStringLiteral("#000000\"; evil: 1")));
    }

    // The map reaches the renderer, and its source is a config file a user can
    // edit by hand. Anything that is not a known role carrying a known-good
    // colour must be dropped rather than passed along.
    void sanitizeDropsEverythingItDoesNotRecognise()
    {
        QVariantMap raw;
        raw.insert(QStringLiteral("background"), QStringLiteral("#101010"));
        raw.insert(QStringLiteral("accent"), QStringLiteral("#1d57ff"));
        raw.insert(QStringLiteral("notARole"), QStringLiteral("#FFFFFF"));
        raw.insert(QStringLiteral("border"), QStringLiteral("nonsense"));
        raw.insert(QStringLiteral("hover"), QStringLiteral("#80FFFFFF"));
        raw.insert(QStringLiteral("surface"), 42);

        const QVariantMap clean = CustomThemeStore::sanitize(raw);
        QCOMPARE(clean.size(), 2);
        QVERIFY(clean.contains(QStringLiteral("background")));
        // Normalised on the way in, so comparing an override against a
        // palette literal never fails on case alone.
        QCOMPARE(clean.value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#1D57FF"));
        QVERIFY(!clean.contains(QStringLiteral("notARole")));
        QVERIFY(!clean.contains(QStringLiteral("border")));
        QVERIFY(!clean.contains(QStringLiteral("hover")));
        QVERIFY(!clean.contains(QStringLiteral("surface")));
    }

    void aRefusedColorIsNeverStored()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        QVERIFY(!store.exists());

        QVERIFY(store.setColor(QStringLiteral("accent"), QStringLiteral("#123456")));
        QCOMPARE(store.overrideCount(), 1);

        QVERIFY(!store.setColor(QStringLiteral("accent"), QStringLiteral("bogus")));
        QVERIFY(!store.setColor(QStringLiteral("madeUpRole"),
                                QStringLiteral("#123456")));
        // The good value is still there and nothing else was added.
        QCOMPARE(store.overrideCount(), 1);
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#123456"));
    }

    // ---- the base theme -------------------------------------------------

    // A custom theme based on the custom theme is a cycle, and QML resolves
    // that as an undefined palette rather than as an error — every token comes
    // back transparent and the window paints black. System (0) is a resolution
    // MODE, not a palette, so it is equally invalid as a base.
    void theBaseMustBeARealPresetNeverTheCustomThemeItself()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);

        QCOMPARE(store.baseTheme(), int(SettingsManager::StormTheme));

        store.setBaseTheme(SettingsManager::MossLightTheme);
        QCOMPARE(store.baseTheme(), int(SettingsManager::MossLightTheme));

        store.setBaseTheme(CustomThemeStore::kCustomThemeId);
        QCOMPARE(store.baseTheme(), int(SettingsManager::MossLightTheme));
        store.setBaseTheme(SettingsManager::SystemTheme);
        QCOMPARE(store.baseTheme(), int(SettingsManager::MossLightTheme));
        store.setBaseTheme(999);
        QCOMPARE(store.baseTheme(), int(SettingsManager::MossLightTheme));
        store.setBaseTheme(-1);
        QCOMPARE(store.baseTheme(), int(SettingsManager::MossLightTheme));
    }

    // A value written by a newer build, or by hand, must not be able to leave
    // the running app resolving an undefined palette.
    void aStoredBaseOutsideTheRangeFallsBack()
    {
        {
            QSettings raw;
            raw.setValue(QStringLiteral("appearance/customThemeBase"),
                         CustomThemeStore::kCustomThemeId);
        }
        SettingsManager settings;
        CustomThemeStore store(&settings);
        QCOMPARE(store.baseTheme(), int(SettingsManager::StormTheme));
    }

    // ---- persistence and reset ------------------------------------------

    void overridesPersistAndResetIsGranular()
    {
        SettingsManager settings;
        {
            CustomThemeStore store(&settings);
            store.setColor(QStringLiteral("accent"), QStringLiteral("#AA0000"));
            store.setColor(QStringLiteral("background"), QStringLiteral("#0A0A0A"));
            store.setName(QStringLiteral("Mine"));
            store.setBaseTheme(SettingsManager::GraphiteTheme);
        }
        CustomThemeStore reopened(&settings);
        QCOMPARE(reopened.overrideCount(), 2);
        QCOMPARE(reopened.name(), QStringLiteral("Mine"));
        QCOMPARE(reopened.baseTheme(), int(SettingsManager::GraphiteTheme));
        QVERIFY(reopened.exists());

        reopened.resetColor(QStringLiteral("accent"));
        QCOMPARE(reopened.overrideCount(), 1);
        QVERIFY(reopened.colors().contains(QStringLiteral("background")));

        // resetAll keeps the base and the name: "start over from this base" is
        // the common intent, and re-picking both would be busywork. The theme
        // itself still EXISTS — since the collection landed, "a theme with
        // nothing overridden" is a real state, distinct from "no themes".
        reopened.resetAll();
        QCOMPARE(reopened.overrideCount(), 0);
        QVERIFY(reopened.exists());
        QCOMPARE(reopened.baseTheme(), int(SettingsManager::GraphiteTheme));
        QCOMPARE(reopened.name(), QStringLiteral("Mine"));

        // discard forgets every theme.
        reopened.setColor(QStringLiteral("accent"), QStringLiteral("#AA0000"));
        reopened.discard();
        QVERIFY(!reopened.exists());
        QVERIFY(reopened.themes().isEmpty());
        QVERIFY(reopened.name().isEmpty());
    }

    // ---- the collection --------------------------------------------------

    void themesAreACollectionWithOneActiveMember()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        QVERIFY(!store.exists());
        QVERIFY(store.themes().isEmpty());

        const QString first = store.createTheme(QStringLiteral("Ocean"));
        QVERIFY(!first.isEmpty());
        QCOMPARE(store.themes().size(), 1);
        QCOMPARE(store.activeThemeId(), first);
        QCOMPARE(store.name(), QStringLiteral("Ocean"));
        store.setColor(QStringLiteral("accent"), QStringLiteral("#112233"));

        // A duplicate carries the colours AND the base, and becomes active —
        // "edit this one into a new one" without a separate mode.
        store.setBaseTheme(SettingsManager::GraphiteTheme);
        const QString copy = store.duplicateActiveTheme(QStringLiteral("Ocean 2"));
        QVERIFY(!copy.isEmpty());
        QVERIFY(copy != first);
        QCOMPARE(store.activeThemeId(), copy);
        QCOMPARE(store.name(), QStringLiteral("Ocean 2"));
        QCOMPARE(store.baseTheme(), int(SettingsManager::GraphiteTheme));
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#112233"));

        // Editing the copy leaves the original alone.
        store.setColor(QStringLiteral("accent"), QStringLiteral("#445566"));
        store.setActiveThemeId(first);
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#112233"));

        // Deleting the active one selects a neighbour rather than leaving
        // theme id 12 resolving to nothing.
        store.deleteTheme(first);
        QCOMPARE(store.themes().size(), 1);
        QCOMPARE(store.activeThemeId(), copy);
        QVERIFY(store.exists());
    }

    void aSharedThemeSurvivesTheRoundTripAndIsRevalidated()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("Ocean"));
        store.setBaseTheme(SettingsManager::MidnightBlueTheme);
        store.setColor(QStringLiteral("accent"), QStringLiteral("#112233"));
        store.setColor(QStringLiteral("sidebar"), QStringLiteral("#445566"));

        const QString shared = store.exportTheme(store.activeThemeId());
        QVERIFY(!shared.isEmpty());
        QVERIFY(shared.contains(QStringLiteral("lightning_theme")));
        // One line, so it survives being pasted into a chat message.
        QVERIFY(!shared.contains(QLatin1Char('\n')));

        QCOMPARE(store.importTheme(shared), QString());
        QCOMPARE(store.themes().size(), 2);
        QCOMPARE(store.baseTheme(), int(SettingsManager::MidnightBlueTheme));
        QCOMPARE(store.colors().value(QStringLiteral("sidebar")).toString(),
                 QStringLiteral("#445566"));

        // A shared theme is untrusted input that gets to paint the whole
        // window, so it is re-validated rather than trusted: unknown roles and
        // malformed colours are dropped, and the base is clamped to a real
        // preset (12 would be a cycle).
        const QString hostile = QStringLiteral(
            "{\"lightning_theme\":1,\"name\":\"X\",\"base\":12,"
            "\"colors\":{\"accent\":\"#010203\",\"notARole\":\"#FFFFFF\","
            "\"sidebar\":\"#80FFFFFF\",\"border\":\"red\"}}");
        QCOMPARE(store.importTheme(hostile), QString());
        QCOMPARE(store.baseTheme(), int(SettingsManager::StormTheme));
        QCOMPARE(store.colors().size(), 1);
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#010203"));

        // Refusals are reported, never silently swallowed.
        QVERIFY(!store.importTheme(QStringLiteral("hello")).isEmpty());
        QVERIFY(!store.importTheme(QStringLiteral("{}")).isEmpty());
        QVERIFY(!store.importTheme(
                     QStringLiteral("{\"lightning_theme\":1,\"colors\":{}}"))
                     .isEmpty());
    }

    void anExistingSingleThemeSurvivesTheUpgradeExactlyOnce()
    {
        // The pre-collection keys. A user upgrading must not lose the theme
        // they built, and must not end up with two copies of it either.
        {
            QSettings raw;
            raw.setValue(QStringLiteral("appearance/customThemeColors"),
                         QStringLiteral("{\"accent\":\"#AA0000\"}"));
            raw.setValue(QStringLiteral("appearance/customThemeName"),
                         QStringLiteral("Legacy"));
            raw.setValue(QStringLiteral("appearance/customThemeBase"),
                         int(SettingsManager::NordTheme));
            raw.sync();
        }
        SettingsManager settings;
        {
            CustomThemeStore store(&settings);
            QCOMPARE(store.themes().size(), 1);
            QCOMPARE(store.name(), QStringLiteral("Legacy"));
            QCOMPARE(store.baseTheme(), int(SettingsManager::NordTheme));
            QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                     QStringLiteral("#AA0000"));
        }
        // Reopening must not migrate a second time.
        CustomThemeStore reopened(&settings);
        QCOMPARE(reopened.themes().size(), 1);
    }

    void everyMutationNotifies()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        QSignalSpy changed(&store, &CustomThemeStore::customThemeChanged);

        store.setColor(QStringLiteral("accent"), QStringLiteral("#AA0000"));
        QCOMPARE(changed.count(), 1);
        store.setBaseTheme(SettingsManager::NordTheme);
        QCOMPARE(changed.count(), 2);
        store.setName(QStringLiteral("x"));
        QCOMPARE(changed.count(), 3);
        store.resetColor(QStringLiteral("accent"));
        QCOMPARE(changed.count(), 4);

        // A no-op must not notify: Main.qml binds AppTheme.customOverrides to
        // this, and a spurious change repaints every consumer in the window.
        store.resetColor(QStringLiteral("accent"));
        QCOMPARE(changed.count(), 4);
        store.resetAll();
        QCOMPARE(changed.count(), 4);
        store.setBaseTheme(SettingsManager::NordTheme);
        QCOMPARE(changed.count(), 4);
        store.setName(QStringLiteral("x"));
        QCOMPARE(changed.count(), 4);
    }

    // ---- readability -----------------------------------------------------

    // The maths, against published values rather than against itself.
    //
    // This is a THIRD copy of the WCAG formula in this tree (AppTheme.qml and
    // src/theme/IdentityColors.cpp carry the other two), and the reason that
    // is affordable is that the formula is a W3C constant and these four
    // numbers pin it. Black on white is exactly 21, a colour on itself is
    // exactly 1, and #767676 / #949494 are the two greys the W3C's own
    // understanding-document uses as the AA and AA-large boundaries against
    // white.
    void contrastMatchesPublishedReferenceValues()
    {
        const auto ratio = &CustomThemeStore::contrast;
        QVERIFY(qAbs(ratio(QStringLiteral("#000000"),
                           QStringLiteral("#FFFFFF")) - 21.0) < 0.001);
        QVERIFY(qAbs(ratio(QStringLiteral("#FFFFFF"),
                           QStringLiteral("#FFFFFF")) - 1.0) < 0.001);
        // Symmetric: the brighter of the two always goes on top.
        QCOMPARE(ratio(QStringLiteral("#000000"), QStringLiteral("#FFFFFF")),
                 ratio(QStringLiteral("#FFFFFF"), QStringLiteral("#000000")));
        // The W3C's AA boundary grey on white: 4.54:1, just over 4.5.
        QVERIFY(qAbs(ratio(QStringLiteral("#767676"),
                           QStringLiteral("#FFFFFF")) - 4.54) < 0.01);
        // The AA-large boundary grey on white: 3.03:1, just over 3.
        QVERIFY(qAbs(ratio(QStringLiteral("#949494"),
                           QStringLiteral("#FFFFFF")) - 3.03) < 0.01);

        // QML hands a `color` across as #AARRGGBB. Reading that as an RGB
        // triple would make every live readout wrong by an alpha channel.
        QCOMPARE(ratio(QStringLiteral("#FF000000"), QStringLiteral("#FFFFFF")),
                 ratio(QStringLiteral("#000000"), QStringLiteral("#FFFFFF")));

        // Unparseable input is 0, never a silent pass. Nothing in the report
        // treats 0 as "fine", and a palette this build cannot read must not
        // be able to produce a clean bill of health.
        QCOMPARE(ratio(QStringLiteral("nonsense"), QStringLiteral("#FFFFFF")),
                 0.0);

        // L*, at its two endpoints and at the CIE mid-grey.
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#000000")))
                < 0.001);
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#FFFFFF")) - 100.0)
                < 0.001);
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#777777")) - 50.0)
                < 0.5);
        // AND ONE NEAR-BLACK, WHICH IS THE ONLY CASE THAT PINS 903.3.
        // L* has a linear segment below the CIE break, and every assertion
        // above it misses: at #000000 the luminance is 0, so `903.3 * 0` is
        // 0 for ANY coefficient, and the other two land above the break. A
        // custom DARK theme's edge checks live exactly here — #0A0A0A is
        // Y = 0.003035, below 0.008856, so this is the branch — and a wrong
        // constant there would misreport every one of them.
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#0A0A0A")) - 2.74)
                < 0.05);
    }

    // THE CALIBRATION GUARD, AND THE REASON THE WARNING IS WORTH HAVING.
    //
    // Every check in the table must pass on all ELEVEN shipped presets. A
    // person forks Moss Light, changes one colour, and must not be told their
    // theme has nine problems they did not create — a warning that fires on a
    // stock theme is one people learn to ignore, and then it is worth less
    // than nothing.
    //
    // This case is also what caught the first draft of the table. Five
    // reasonable-sounding checks fired on shipped themes and were removed with
    // the offending preset recorded beside each one in the .cpp: `selected`
    // against `sidebar` (Moss Light paints them the same colour), `hover` and
    // `cardElevated` separation (floors too low to bar anything), `accent`
    // against `background` at 3:1 (Indigo Night, 2.86) and `link` against
    // `background` at 4.5 (Moss Light, 4.47).
    //
    // It asserts the number of comparisons it actually MADE, not just that
    // nothing failed: a parser that quietly resolves less would otherwise
    // turn this guarantee into a vacuous pass.
    void everyReadabilityCheckPassesOnEveryShippedPreset()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");
        const auto presets = presetPalettes(qml);
        QCOMPARE(presets.size(), 11);

        SettingsManager settings;
        CustomThemeStore store(&settings);

        int evaluated = 0;
        QStringList failures;
        for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
            const QVariantMap palette =
                resolvedPalette(it.value(), QStringLiteral("#FFFFFF"));
            // auditForRole with an empty role would narrow to nothing, so the
            // full sweep goes through audit() and the pass count is derived
            // from what did NOT come back.
            const QVariantList bad = store.audit(palette);
            // auditForRole with an EMPTY role narrows nothing, so this is the
            // full table including its passes — which is what makes the
            // "how many did we actually compare" count possible.
            const QVariantList all = store.auditForRole(palette, QString());
            evaluated += all.size();

            // EVERY ROW MUST NAME A ROLE THE EDITOR CAN OPEN, on every
            // preset — not merely the rows two fixtures happen to fail.
            //
            // This assertion used to live only in the Sand/Ink case, over
            // `audit()`'s failing rows. That reached 37 rows out of 264, and
            // the 227 it never looked at included the one that was broken: a
            // table regenerated by script put the string "None" where a
            // nullptr belonged, so the own-bubble row pointed at a role that
            // does not exist and its click-through opened a picker that
            // silently discarded every colour. Both fixtures clear that pair
            // comfortably (6.22 and 7.76), so it was never emitted and the
            // whole suite stayed green. Checking the passes too is what
            // closes the class rather than the instance.
            for (const QVariant &row : all) {
                const QVariantMap m = row.toMap();
                const QString role = m.value(QStringLiteral("role")).toString();
                QVERIFY2(CustomThemeStore::roleIsEditable(role),
                         qPrintable(QStringLiteral(
                                        "%1: the check '%2' points at role "
                                        "'%3', which the editor cannot open")
                                        .arg(it.key(),
                                             m.value(QStringLiteral("label"))
                                                 .toString(),
                                             role)));
                QVERIFY2(!m.value(QStringLiteral("label")).toString().isEmpty(),
                         "a readability check has no written sentence");
            }
            for (const QVariant &row : bad) {
                const QVariantMap m = row.toMap();
                failures << QStringLiteral("%1: %2 on %3 = %4 (needs %5)")
                                .arg(it.key(),
                                     m.value(QStringLiteral("fg")).toString(),
                                     m.value(QStringLiteral("bg")).toString(),
                                     QString::number(
                                         m.value(QStringLiteral("value"))
                                             .toDouble(), 'f', 2),
                                     QString::number(
                                         m.value(QStringLiteral("minimum"))
                                             .toDouble(), 'f', 1));
            }
        }
        QVERIFY2(failures.isEmpty(),
                 qPrintable(QStringLiteral(
                                "the readability table fires on a SHIPPED "
                                "theme, which would make it noise:\n  %1")
                                .arg(failures.join(QStringLiteral("\n  ")))));

        // 11 presets x 24 checks = 264, minus EXACTLY ONE: Storm writes its
        // `hover` as Qt.alpha(_stoHover, 0.22) rather than as a named colour,
        // so the parser above cannot resolve it and textPrimary/hover is
        // skipped for Storm alone. Every other endpoint of every check
        // resolves on every preset.
        //
        // The bound is EXACT on purpose. A looser one is the same defect it
        // is meant to catch: if the parser stops resolving a palette, or a
        // key is renamed in AppTheme, the checks quietly stop being made and
        // a slack bound lets the case keep passing while guaranteeing less.
        QCOMPARE(CustomThemeStore::readabilityCheckCount(), 24);
        QCOMPARE(evaluated, 11 * 24 - 1);
    }

    // A TRANSLUCENT PALETTE ENTRY IS NOT GRADED, BECAUSE IT CANNOT BE.
    //
    // Found on the running editor, not here: a brand-new theme on the Storm
    // base with ZERO overrides reported "Main text on a hovered row 4.4:1 —
    // needs 4.5:1". Storm writes `hover: Qt.alpha(_stoHover, 0.22)`, and the
    // audit was grading that colour's raw RGB as though it were opaque. A
    // 22%-alpha fill composites over whatever is under it, so its contrast is
    // unknowable — which is the exact reason this class already refuses
    // 8-digit hex from the user.
    //
    // THE SUITE COULD NOT HAVE CAUGHT IT. `presetPalettes()` resolves named
    // colour literals only, so the one entry that misbehaves is the one entry
    // the parser skips — which is also why `evaluated` is 263 and not 264.
    // The fixture below therefore hands the store a translucent value
    // DIRECTLY rather than going through the parser.
    void aTranslucentPaletteEntryIsSkippedRatherThanGuessedAt()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);

        // Opaque and genuinely failing: graded, and reported.
        QVariantMap opaque{
            {QStringLiteral("textPrimary"), QStringLiteral("#6E7484")},
            {QStringLiteral("hover"), QStringLiteral("#3A3F4B")}};
        const QVariantList graded =
            store.auditForRole(opaque, QStringLiteral("hover"));
        QCOMPARE(graded.size(), 1);
        QCOMPARE(graded.first().toMap().value(QStringLiteral("passes")).toBool(),
                 false);

        // The SAME pair with the hover 22% transparent — the shape Storm
        // ships. Not graded at all: not a pass, not a failure, absent.
        QVariantMap translucent = opaque;
        translucent.insert(QStringLiteral("hover"),
                           QVariant::fromValue(QColor(0x3A, 0x3F, 0x4B, 56)));
        QCOMPARE(store.auditForRole(translucent, QStringLiteral("hover")).size(),
                 0);
        QCOMPARE(store.audit(translucent).size(), 0);

        // And the same colour written as 8-digit ARGB text, which is how a
        // hand-edited config would carry it.
        QVariantMap asText = opaque;
        asText.insert(QStringLiteral("hover"), QStringLiteral("#383A3F4B"));
        QCOMPARE(store.audit(asText).size(), 0);

        // A fully opaque QColor is still graded — the guard keys on alpha,
        // not on the value happening to arrive as a QColor.
        QVariantMap opaqueColor = opaque;
        opaqueColor.insert(QStringLiteral("hover"),
                           QVariant::fromValue(QColor(0x3A, 0x3F, 0x4B)));
        QCOMPARE(store.audit(opaqueColor).size(), 1);
    }

    // EVERY KEY THE TABLE READS MUST BE A KEY THE PALETTE ACTUALLY HAS.
    //
    // `gradePalette` SKIPS a check whose endpoint is missing rather than
    // failing it — which is right (a palette this build cannot read is not a
    // user's mistake) and is exactly why this case has to exist. Rename
    // `inputBackground` in AppTheme's `paletteForTheme` and the running app
    // silently stops grading two pairs, while every other case here stays
    // green because they build their palettes themselves.
    //
    // It reads the RETURN LITERAL of paletteForTheme, not the raw `_theme`
    // blocks: those use the palettes' own spelling (`inputBg`, `reaction`),
    // and the resolved object is what the editor hands in.
    // `everyEditableRoleIsARealPaletteKey` covers the other direction and the
    // other dialect; the two are not substitutes.
    void everyReadabilityKeyIsAKeyPaletteForThemeReturns()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");

        const int start = qml.indexOf(QStringLiteral("function paletteForTheme"));
        QVERIFY2(start >= 0, "paletteForTheme is gone from AppTheme.qml");
        const int open = qml.indexOf(QStringLiteral("return {"), start);
        QVERIFY2(open >= 0, "paletteForTheme no longer returns an object");
        const int close = qml.indexOf(QStringLiteral("\n        }"), open);
        QVERIFY2(close > open, "could not find the end of the returned object");
        QString body = qml.mid(open, close - open);
        body.remove(QRegularExpression(QStringLiteral("//[^\n]*")));

        QSet<QString> returned;
        static const QRegularExpression key(
            QStringLiteral("(?:^|\\n)\\s*(\\w+)\\s*:"));
        auto it = key.globalMatch(body);
        while (it.hasNext())
            returned.insert(it.next().captured(1));
        QVERIFY2(returned.size() > 25,
                 qPrintable(QStringLiteral("the return-object scan found only "
                                           "%1 keys — it has stopped working")
                                .arg(returned.size())));

        const QStringList keys = CustomThemeStore::readabilityPaletteKeys();
        QVERIFY2(!keys.isEmpty(), "the readability table reads no keys at all");
        for (const QString &k : keys) {
            QVERIFY2(returned.contains(k),
                     qPrintable(QStringLiteral(
                                    "the readability table grades '%1', which "
                                    "paletteForTheme does not return — that "
                                    "check is silently never made")
                                    .arg(k)));
        }
    }

    // THE TWO THEMES THAT WERE MEASURED ON REAL PIXELS.
    //
    // Both were authored in the running editor on 2026-09-19 and their
    // contrast was read off the RENDERED preview with an image tool, not
    // computed: the room name in Sand's room list measured 3.08:1 on screen,
    // and the white "Send" label on its mint accent measured 1.62:1, with
    // 1273 of that button's ~1456 pixels flat accent. Neither was contrived —
    // Sand is a soft warm palette and Ink a low-key dark one, the sort of
    // thing people actually build. The editor reported nothing about either.
    //
    // This case pins that the audit now names them, and pins the COUNT: a
    // report that comes back with one finding instead of eighteen is the
    // silent-short-count failure this project keeps meeting, and asserting
    // only "something was reported" cannot see it.
    void theAuditNamesWhatTheGuiMeasuredOnTwoRealThemes()
    {
        const QString qml = appTheme();
        const auto presets = presetPalettes(qml);
        SettingsManager settings;
        CustomThemeStore store(&settings);

        const QVariantMap sand{
            {QStringLiteral("background"), QStringLiteral("#FAF7F0")},
            {QStringLiteral("sidebar"), QStringLiteral("#F2EDE3")},
            {QStringLiteral("surface"), QStringLiteral("#FFFDF8")},
            {QStringLiteral("textPrimary"), QStringLiteral("#8A857B")},
            {QStringLiteral("textSecondary"), QStringLiteral("#A9A499")},
            {QStringLiteral("textMuted"), QStringLiteral("#BDB8AD")},
            {QStringLiteral("accent"), QStringLiteral("#A8D5BA")},
            {QStringLiteral("accentText"), QStringLiteral("#FFFFFF")},
            {QStringLiteral("border"), QStringLiteral("#F0EBE1")},
            {QStringLiteral("selected"), QStringLiteral("#EFEAE0")},
            {QStringLiteral("rail"), QStringLiteral("#EDE7DB")},
            {QStringLiteral("inputBackground"), QStringLiteral("#FFFDF8")}};
        const QVariantMap ink{
            {QStringLiteral("background"), QStringLiteral("#14161A")},
            {QStringLiteral("sidebar"), QStringLiteral("#101215")},
            {QStringLiteral("surface"), QStringLiteral("#1B1E24")},
            {QStringLiteral("textPrimary"), QStringLiteral("#6E7484")},
            {QStringLiteral("textSecondary"), QStringLiteral("#4E5462")},
            {QStringLiteral("textMuted"), QStringLiteral("#3E4450")},
            {QStringLiteral("accent"), QStringLiteral("#3A4CC0")},
            {QStringLiteral("accentText"), QStringLiteral("#101018")},
            {QStringLiteral("border"), QStringLiteral("#1A1D23")},
            {QStringLiteral("selected"), QStringLiteral("#20242C")},
            {QStringLiteral("rail"), QStringLiteral("#0C0E11")},
            {QStringLiteral("inputBackground"), QStringLiteral("#191C22")}};

        const auto merged = [&](const QString &base, const QVariantMap &over) {
            QVariantMap p = resolvedPalette(presets.value(base),
                                            QStringLiteral("#FFFFFF"));
            for (auto it = over.constBegin(); it != over.constEnd(); ++it)
                p.insert(it.key(), it.value());
            return p;
        };
        const auto findPair = [](const QVariantList &rows, const char *fg,
                                 const char *bg) {
            for (const QVariant &row : rows) {
                const QVariantMap m = row.toMap();
                if (m.value(QStringLiteral("fg")).toString()
                        == QLatin1String(fg)
                    && m.value(QStringLiteral("bg")).toString()
                        == QLatin1String(bg))
                    return m.value(QStringLiteral("value")).toDouble();
            }
            return -1.0;
        };

        const QVariantList sandReport =
            store.audit(merged(QStringLiteral("Moss Light"), sand));
        QCOMPARE(sandReport.size(), 18);
        // Measured on the rendered "Send" button: 1.62:1. Arithmetic: 1.63.
        QVERIFY(qAbs(findPair(sandReport, "accentText", "accent") - 1.63)
                < 0.01);
        // Measured on the rendered room-list room name: 3.08:1. The audit
        // grades the same ink against the same sidebar.
        QVERIFY(qAbs(findPair(sandReport, "textPrimary", "sidebar") - 3.15)
                < 0.01);

        const QVariantList inkReport =
            store.audit(merged(QStringLiteral("Lightning Dark"), ink));
        QCOMPARE(inkReport.size(), 19);
        QVERIFY(qAbs(findPair(inkReport, "accentText", "accent") - 2.68)
                < 0.01);
        // The one a person cannot see coming: `hover` was never overridden,
        // so it still comes from Lightning Dark and now clashes with the new
        // ink. An audit over the RESOLVED palette catches an interaction
        // between what the user changed and what they inherited; an audit
        // over the sparse override map could not.
        QVERIFY(qAbs(findPair(inkReport, "textPrimary", "hover") - 1.92)
                < 0.01);
        // Ink's hairline all but vanishes: dL* 0.5 against its own surface.
        QVERIFY(qAbs(findPair(inkReport, "border", "surface") - 0.50) < 0.02);

        // Every finding must name a role the editor can actually open, or the
        // click-through in the report is a dead end.
        for (const QVariantList &report : {sandReport, inkReport}) {
            for (const QVariant &row : report) {
                const QString role =
                    row.toMap().value(QStringLiteral("role")).toString();
                QVERIFY2(CustomThemeStore::roleIsEditable(role),
                         qPrintable(QStringLiteral("report row points at '%1', "
                                                   "which the editor cannot "
                                                   "open").arg(role)));
            }
        }
    }

    // The live readout under an open picker needs the PASSES too — a number
    // climbing past its bar as you drag is the thing that teaches; a warning
    // that merely disappears does not.
    void theLiveReadoutForOneRoleKeepsItsPasses()
    {
        const QString qml = appTheme();
        const auto presets = presetPalettes(qml);
        SettingsManager settings;
        CustomThemeStore store(&settings);
        const QVariantMap storm =
            resolvedPalette(presets.value(QStringLiteral("Storm")),
                            QStringLiteral("#FFFFFF"));

        // Storm passes everything, so a role-narrowed report over it is all
        // passes — and must not therefore be empty.
        const QVariantList rows =
            store.auditForRole(storm, QStringLiteral("background"));
        QVERIFY2(!rows.isEmpty(),
                 "a role with no problems must still report its numbers");
        for (const QVariant &row : rows) {
            const QVariantMap m = row.toMap();
            QVERIFY(m.value(QStringLiteral("passes")).toBool());
            QVERIFY(m.value(QStringLiteral("fg")).toString()
                        == QLatin1String("background")
                    || m.value(QStringLiteral("bg")).toString()
                        == QLatin1String("background"));
        }
        QCOMPARE(store.audit(storm).size(), 0);

        // An unknown role narrows to nothing rather than to everything.
        QCOMPARE(store.auditForRole(storm, QStringLiteral("nope")).size(), 0);
    }

    // ---- the couplings that would fail silently --------------------------

    // AppTheme merges the override map straight over the base palette, so a
    // role the editor offers that is NOT a palette key lands in the map,
    // paints nothing, and shows the user a swatch that does not work.
    void everyEditableRoleIsARealPaletteKey()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");
        const QSet<QString> keys = paletteKeys(qml);
        QVERIFY2(keys.size() > 15, "palette-key scan found almost nothing");

        for (const QString &role : CustomThemeStore::editableRoles()) {
            QVERIFY2(keys.contains(role),
                     qPrintable(QStringLiteral("the editor offers '%1', which "
                                               "is not a key any AppTheme "
                                               "palette defines").arg(role)));
        }
    }

    void theEditorCoversTheFourShellRegionsAndTheMessageSurfaces()
    {
        const QStringList roles = CustomThemeStore::editableRoles();
        // The user asked for "the room list, the menu with spaces on the left
        // and top part and all else" — these are the regions that maps to, so
        // losing one of them is a requirement regression, not a taste change.
        for (const char *required : { "rail", "sidebar", "background", "surface",
                                      "inputBg", "hover", "selected",
                                      "ownBubble", "otherBubble", "accent",
                                      "textPrimary", "border" }) {
            QVERIFY2(roles.contains(QLatin1String(required)),
                     qPrintable(QStringLiteral("the editor no longer offers %1")
                                    .arg(QLatin1String(required))));
        }
        QVERIFY(CustomThemeStore::roleIsEditable(QStringLiteral("rail")));
        QVERIFY(!CustomThemeStore::roleIsEditable(QStringLiteral("nope")));
        // No duplicates: two rows for one role would let the second silently
        // shadow the first in the editor.
        QCOMPARE(QSet<QString>(roles.begin(), roles.end()).size(), roles.size());
    }

    // Every new shell surface must reach a CUSTOM theme too, and the way it
    // does that here is by DERIVING from a role the editor already offers —
    // never by adding a required key to eleven palettes, which is how a theme
    // ends up with one undefined colour and a transparent row.
    //
    // So: each token added for the Channels layout and for the rail's folders
    // falls back to something the editor can actually change, and that
    // fallback role is an editable one. A token whose fallback were a private
    // literal would be invisible to the Theme Editor for ever.
    void theNewNavigationTokensFollowAnEditableRole()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");
        QString flat = qml;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        const QStringList roles = CustomThemeStore::editableRoles();

        // token -> the palette role it falls back to.
        const QList<QPair<QString, QString>> derived = {
            { QStringLiteral("channelCategoryText"), QStringLiteral("textMuted") },
            { QStringLiteral("channelText"), QStringLiteral("textSecondary") },
            { QStringLiteral("channelSelected"), QStringLiteral("selected") },
            { QStringLiteral("channelHover"), QStringLiteral("hover") },
            { QStringLiteral("channelUnreadMark"), QStringLiteral("accent") },
            { QStringLiteral("railFolderSurface"), QStringLiteral("cardElevated") },
        };
        for (const auto &[token, fallback] : derived) {
            const QString declaration =
                QStringLiteral("readonly property color ") + token + QStringLiteral(":");
            const int at = flat.indexOf(declaration);
            QVERIFY2(at >= 0, qPrintable(token + QStringLiteral(" is not declared")));
            const QString body = flat.mid(at, 240);
            QVERIFY2(body.contains(QStringLiteral("!== undefined")),
                     qPrintable(token + QStringLiteral(" does not fall back when "
                                                       "a palette omits it")));
            QVERIFY2(body.contains(fallback),
                     qPrintable(QStringLiteral("%1 no longer derives from %2, so "
                                               "a custom theme cannot reach it")
                                    .arg(token, fallback)));
            // ...and the role it derives from is one the editor offers, so a
            // Theme Editor change genuinely moves the new surfaces.
            const QString editable =
                fallback == QLatin1String("cardElevated")
                    ? QStringLiteral("cardElevated")
                    : fallback;
            QVERIFY2(roles.contains(editable)
                         || roles.contains(QStringLiteral("surface")),
                     qPrintable(QStringLiteral("%1 derives from %2, which the "
                                               "Theme Editor cannot change")
                                    .arg(token, fallback)));
        }
    }

    // The id is duplicated between C++ and QML by necessity (QML cannot see
    // the enum), so it is asserted rather than assumed.
    void theCustomThemeIdAgreesWithSettingsAndWithAppTheme()
    {
        QCOMPARE(int(CustomThemeStore::kCustomThemeId),
                 int(SettingsManager::CustomTheme));
        QCOMPARE(int(SettingsManager::kMaxThemeId),
                 int(SettingsManager::CustomTheme));

        const QString qml = appTheme();
        QVERIFY(qml.contains(QStringLiteral("case 12: return _custom")));
        QVERIFY2(qml.contains(QStringLiteral("readonly property var _custom")),
                 "AppTheme has no _custom palette");
        // The second gate. CustomThemeStore sanitises, but this is the one
        // palette whose contents reach a hand-editable config file, and an
        // unparseable colour there paints the shell transparent.
        QVERIFY2(qml.contains(QStringLiteral("/^#[0-9A-Fa-f]{6}$/.test(v)")),
                 "the QML-side override guard is gone");
    }

    // A user can build a LIGHT palette on a DARK base. Inheriting the base's
    // light/dark answer would leave shadows, scrims and overlay chrome
    // fighting the surface they sit on, so the custom theme is classified by
    // its own background.
    void theCustomThemeClassifiesItselfLightOrDark()
    {
        const QString qml = appTheme();
        QVERIFY2(qml.contains(QStringLiteral("effectiveTheme === 12")),
                 "`dark` no longer special-cases the custom palette");
        QVERIFY2(qml.contains(QStringLiteral("relativeLuminance(_p.background)")),
                 "the custom palette must be classified by its own background");
    }

    // ---- the cache is account-scoped ------------------------------------

    // DATA LOSS, not a stale list. The collection lives in
    // SettingsManager::appearanceValue, which is ACCOUNT-SCOPED, and load()
    // fills its cache exactly once per instance. Without an invalidation the
    // store keeps serving the outgoing account's themes after a switch — and
    // the incoming account's FIRST write (a rename here; setBaseTheme,
    // deleteTheme, importTheme and any colour are the same call) hands that
    // cached list to save(), which persists it over the incoming account's
    // own record. Their themes are gone, silently and permanently.
    //
    // Both halves are asserted: what the switched-to account is SHOWN, and
    // what its first write PERSISTS.
    void aSwitchDropsTheOutgoingAccountsThemesInsteadOfSavingThemOverTheNext()
    {
        SettingsManager settings;
        settings.saveSession(kHs, kBob, QStringLiteral("BDEV"), QString());
        settings.saveSession(kHs, kAlice, QStringLiteral("ADEV"), QString());

        // Seeded through SHORT-LIVED stores, one per account: a fixture built
        // with a single long-lived store would itself be corrupted by the
        // defect under test and could not distinguish anything.
        //
        // discard() first because appearanceValue mirrors every write into a
        // device-global fallback that an account with no record of its own
        // reads — deliberate for a theme, but it would make both accounts
        // start from the same list and hide the difference this case needs.
        settings.setActiveAccountUserId(kBob);
        {
            CustomThemeStore seed(&settings);
            seed.discard();
            QVERIFY(!seed.createTheme(QStringLiteral("Bob only")).isEmpty());
        }
        settings.setActiveAccountUserId(kAlice);
        {
            CustomThemeStore seed(&settings);
            seed.discard();
            QVERIFY(!seed.createTheme(QStringLiteral("Alice one")).isEmpty());
            QVERIFY(!seed.createTheme(QStringLiteral("Alice two")).isEmpty());
        }

        CustomThemeStore store(&settings);
        QCOMPARE(themeNames(store), QStringList({ QStringLiteral("Alice one"),
                                                  QStringLiteral("Alice two") }));

        // THE SWITCH.
        settings.setActiveAccountUserId(kBob);

        // (a) what Bob is shown.
        QCOMPARE(themeNames(store), QStringList({ QStringLiteral("Bob only") }));

        // (b) what Bob's first write persists. On the unfixed store this call
        // renames a theme belonging to Alice and writes her whole list into
        // Bob's record.
        store.setName(QStringLiteral("Bob renamed"));

        CustomThemeStore reread(&settings);
        QCOMPARE(themeNames(reread),
                 QStringList({ QStringLiteral("Bob renamed") }));

        // And Alice keeps hers. Her record is only reachable through her own
        // account key, so a write made under Bob must not have touched it.
        settings.setActiveAccountUserId(kAlice);
        CustomThemeStore aliceAgain(&settings);
        QCOMPARE(themeNames(aliceAgain),
                 QStringList({ QStringLiteral("Alice one"),
                               QStringLiteral("Alice two") }));
    }

    // The same cache survives the OTHER way an account becomes active, which
    // is why the fix hangs off sessionChanged rather than off a switch: a
    // sign-in writes the active-account pointer directly (saveSession), so
    // nothing "switched" and no switch-specific hook would fire. That is the
    // shape 0.9.4's e131aae fixed for a different store.
    void signingAnAccountInMakesTheNextReadConsultIt()
    {
        SettingsManager settings;
        settings.saveSession(kHs, kBob, QStringLiteral("BDEV"), QString());
        {
            CustomThemeStore seed(&settings);
            seed.discard();
            QVERIFY(!seed.createTheme(QStringLiteral("Bob only")).isEmpty());
        }
        settings.saveSession(kHs, kAlice, QStringLiteral("ADEV"), QString());
        {
            CustomThemeStore seed(&settings);
            seed.discard();
            QVERIFY(!seed.createTheme(QStringLiteral("Alice one")).isEmpty());
            QVERIFY(!seed.createTheme(QStringLiteral("Alice two")).isEmpty());
        }

        CustomThemeStore store(&settings);
        QCOMPARE(themeNames(store).size(), 2);

        // Bob signs in again: active NOW, with no switchToAccount involved.
        settings.saveSession(kHs, kBob, QStringLiteral("BDEV"), QString());
        QCOMPARE(themeNames(store), QStringList({ QStringLiteral("Bob only") }));
    }

    // The invalidation announces itself, because nothing polls this store:
    // the Appearance page and AppTheme.qml both re-read on customThemeChanged
    // and would otherwise keep painting the previous account's palette until
    // some unrelated edit happened to notify.
    void anAccountChangeNotifiesSoTheShellRepaints()
    {
        SettingsManager settings;
        settings.saveSession(kHs, kAlice, QStringLiteral("ADEV"), QString());
        CustomThemeStore store(&settings);
        store.discard();
        QVERIFY(!store.createTheme(QStringLiteral("Alice one")).isEmpty());

        QSignalSpy changed(&store, &CustomThemeStore::customThemeChanged);
        settings.saveSession(kHs, kBob, QStringLiteral("BDEV"), QString());
        QVERIFY2(changed.count() >= 1,
                 "an account change must announce that the theme list moved");
    }

private:
    // Names in stored order. Comparing the whole list rather than a size and
    // one name: the defect's signature is the WRONG ACCOUNT'S list, and two
    // lists can share a length.
    static QStringList themeNames(const CustomThemeStore &store)
    {
        QStringList out;
        const QVariantList themes = store.themes();
        for (const QVariant &entry : themes)
            out << entry.toMap().value(QStringLiteral("name")).toString();
        return out;
    }

    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(CustomThemeTest)
#include "CustomThemeTest.moc"
