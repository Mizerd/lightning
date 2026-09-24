// Custom-theme contract. The store sits between a hand-editable config file
// and the renderer, so most of this is about what it refuses; the rest pins
// the role table against AppTheme's palette keys and theme id 12 against
// AppTheme.qml's routing.

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

// Every key any palette object in AppTheme.qml defines. The custom palette
// merges over one of these, so an editable role must be one of them or its
// override paints nothing.
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

// The eleven shipped presets, resolved as AppTheme.paletteForTheme() does,
// so readability checks are calibrated against real themes. Named colour
// properties are collected first, then each `_theme` block is read through
// them. Computed entries (`Qt.darker(...)`) are not resolved; a check with a
// missing endpoint is skipped, and the preset sweep asserts how many checks
// it evaluated so a parser that resolves less fails.
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

    // The eleven palette blocks, by the theme id rawPaletteForTheme() maps.
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

// One `readonly property color _name: "#RRGGBB"` from AppTheme, read rather
// than hand-copied so the fixture follows AppTheme.
QString colorLiteral(const QString &qml, const QString &name)
{
    const QRegularExpression re(
        QStringLiteral("readonly\\s+property\\s+color\\s+%1:\\s*"
                       "\"(#[0-9A-Fa-f]{6})\"").arg(name));
    const auto m = re.match(qml);
    return m.hasMatch() ? m.captured(1).toUpper() : QString();
}

// One preset as paletteForTheme() hands it to QML: semantic names plus the
// same fallbacks, since the audit reads that object.
QVariantMap resolvedPalette(const QHash<QString, QString> &raw,
                            const QString &onAccent,
                            const QString &dangerFill = QString())
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
    // Only four presets name a `mention`; the rest take `_accentDanger`, as
    // paletteForTheme does. Absent (not empty) without a fallback, so the
    // check is skipped and counted as such.
    const QString mention = raw.value(QStringLiteral("mention"), dangerFill);
    if (!mention.isEmpty())
        p.insert(QStringLiteral("mentionBadge"), mention);
    // ownBubbleText is a literal (#FFFFFF) in AppTheme, not a palette entry.
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

    // `roles` is NOTIFY rolesChanged and builds labels with tr(); it must emit
    // on a language change, since engine.retranslate() does not reach strings
    // a C++ model already produced.
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

    // Every case starts from an empty config; a stored list would otherwise
    // leak into the next case.
    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // ---- what it refuses ----

    void onlyOpaqueSixDigitHexIsAccepted()
    {
        QVERIFY(CustomThemeStore::colorIsValid(QStringLiteral("#1D57FF")));
        QVERIFY(CustomThemeStore::colorIsValid(QStringLiteral("#abcdef")));

        // 8-digit ARGB is refused on purpose: a translucent shell surface makes
        // contrast unknowable, and every contrast rule assumes opaque colours.
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

    // The map reaches the renderer from a hand-editable config, so anything
    // but a known role with a valid colour is dropped.
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
        // Normalised on the way in, so comparisons never fail on case.
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

    // ---- the base theme ----

    // A custom theme based on itself is a cycle that QML resolves as an
    // undefined palette (the window paints black). System (0) is a resolution
    // mode, not a palette, so it is also invalid as a base.
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

    // A base written by a newer build or by hand must not leave the app with
    // an undefined palette.
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

    // ---- persistence and reset ----

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

        // resetAll keeps the base and name ("start over from this base"). The
        // theme itself still exists: a theme with no overrides is a real state.
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

    // ---- the collection ----

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

        // A duplicate carries colours and base, and becomes active.
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

        // Deleting the active theme selects a neighbour so theme id 12 still
        // resolves.
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
        // One line, so it survives being pasted into chat.
        QVERIFY(!shared.contains(QLatin1Char('\n')));

        QCOMPARE(store.importTheme(shared), QString());
        QCOMPARE(store.themes().size(), 2);
        QCOMPARE(store.baseTheme(), int(SettingsManager::MidnightBlueTheme));
        QCOMPARE(store.colors().value(QStringLiteral("sidebar")).toString(),
                 QStringLiteral("#445566"));

        // An imported theme is untrusted: unknown roles and malformed colours
        // are dropped, and the base is clamped to a real preset (12 would be a
        // cycle).
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
        // The pre-collection keys: an upgrade keeps the user's theme, once.
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
        // this, and a spurious change repaints the whole window.
        store.resetColor(QStringLiteral("accent"));
        QCOMPARE(changed.count(), 4);
        store.resetAll();
        QCOMPARE(changed.count(), 4);
        store.setBaseTheme(SettingsManager::NordTheme);
        QCOMPARE(changed.count(), 4);
        store.setName(QStringLiteral("x"));
        QCOMPARE(changed.count(), 4);
    }

    // ---- readability ----

    // The WCAG formula against published values (it is also copied in
    // AppTheme.qml and src/theme/IdentityColors.cpp). #767676 and #949494 are
    // the W3C's AA and AA-large boundary greys against white.
    void contrastMatchesPublishedReferenceValues()
    {
        const auto ratio = &CustomThemeStore::contrast;
        QVERIFY(qAbs(ratio(QStringLiteral("#000000"),
                           QStringLiteral("#FFFFFF")) - 21.0) < 0.001);
        QVERIFY(qAbs(ratio(QStringLiteral("#FFFFFF"),
                           QStringLiteral("#FFFFFF")) - 1.0) < 0.001);
        // Symmetric: the brighter colour always goes on top.
        QCOMPARE(ratio(QStringLiteral("#000000"), QStringLiteral("#FFFFFF")),
                 ratio(QStringLiteral("#FFFFFF"), QStringLiteral("#000000")));
        // The W3C's AA boundary grey on white: 4.54:1, just over 4.5.
        QVERIFY(qAbs(ratio(QStringLiteral("#767676"),
                           QStringLiteral("#FFFFFF")) - 4.54) < 0.01);
        // The AA-large boundary grey on white: 3.03:1, just over 3.
        QVERIFY(qAbs(ratio(QStringLiteral("#949494"),
                           QStringLiteral("#FFFFFF")) - 3.03) < 0.01);

        // QML passes a `color` as #AARRGGBB; the alpha must not be read as RGB.
        QCOMPARE(ratio(QStringLiteral("#FF000000"), QStringLiteral("#FFFFFF")),
                 ratio(QStringLiteral("#000000"), QStringLiteral("#FFFFFF")));

        // Unparseable input is 0, never a pass.
        QCOMPARE(ratio(QStringLiteral("nonsense"), QStringLiteral("#FFFFFF")),
                 0.0);

        // L*, at its two endpoints and at the CIE mid-grey.
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#000000")))
                < 0.001);
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#FFFFFF")) - 100.0)
                < 0.001);
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#777777")) - 50.0)
                < 0.5);
        // A near-black pins the 903.3 coefficient of L*'s linear segment,
        // which the other endpoints cannot (#0A0A0A has Y below 0.008856).
        // Dark custom themes' edge checks live here.
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#0A0A0A")) - 2.74)
                < 0.05);
    }

    // Every check must pass on all eleven shipped presets: a warning that
    // fires on a stock theme teaches people to ignore it. Checks removed for
    // that reason are recorded with the offending preset in the .cpp. Asserts
    // the number of comparisons made, so a parser that resolves less fails.
    void everyReadabilityCheckPassesOnEveryShippedPreset()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");
        const auto presets = presetPalettes(qml);
        QCOMPARE(presets.size(), 11);
        const QString dangerFill =
            colorLiteral(qml, QStringLiteral("_accentDanger"));
        QVERIFY2(!dangerFill.isEmpty(),
                 "_accentDanger is gone from AppTheme.qml, so the seven "
                 "presets without their own `mention` cannot be resolved");

        SettingsManager settings;
        CustomThemeStore store(&settings);

        int evaluated = 0;
        QStringList failures;
        for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
            const QVariantMap palette =
                resolvedPalette(it.value(), QStringLiteral("#FFFFFF"),
                                dangerFill);
            // Failures come from audit(); passes are derived from what did not
            // come back.
            const QVariantList bad = store.audit(palette);
            // An empty role narrows nothing, so this is the full table
            // including passes, which gives the comparison count.
            const QVariantList all = store.auditForRole(palette, QString());
            evaluated += all.size();

            // Every row, passing or failing, must name a role the editor can
            // open; a bad role on a passing row is otherwise never seen.
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

        // 11 presets x 27 checks = 297, minus exactly one: Storm's `hover` is
        // Qt.alpha(...), which the parser cannot resolve. `selectedHover` is
        // deliberately unchecked (see the .cpp). The bound is exact so a
        // renamed key or a parser regression cannot pass quietly.
        QCOMPARE(CustomThemeStore::readabilityCheckCount(), 27);
        QCOMPARE(evaluated, 11 * 27 - 1);

        // The unevaluated check is named, not just counted: auditSkipped says
        // which pair and why.
        int skipped = 0;
        for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
            const QVariantMap palette =
                resolvedPalette(it.value(), QStringLiteral("#FFFFFF"),
                                dangerFill);
            const QVariantList gaps = store.auditSkipped(palette, QString());
            skipped += gaps.size();
            for (const QVariant &row : gaps) {
                const QVariantMap m = row.toMap();
                QCOMPARE(it.key(), QStringLiteral("Storm"));
                QCOMPARE(m.value(QStringLiteral("fg")).toString(),
                         QStringLiteral("textPrimary"));
                QCOMPARE(m.value(QStringLiteral("bg")).toString(),
                         QStringLiteral("hover"));
                // "missing" because the fixture parser cannot resolve
                // Qt.alpha(); the running app gets a translucent QColor and says
                // "translucent". Both are skips, neither a pass.
                QCOMPARE(m.value(QStringLiteral("reason")).toString(),
                         QStringLiteral("missing"));
                QVERIFY2(!m.contains(QStringLiteral("passes")),
                         "a check that was never made must not carry a "
                         "verdict");
            }
        }
        QCOMPARE(skipped, 1);
    }

    // A translucent palette entry is not graded: it composites over whatever
    // is beneath, so its contrast is unknowable (the same reason 8-digit hex
    // is refused). The parser skips Storm's translucent hover, so this hands
    // the store a translucent value directly.
    void aTranslucentPaletteEntryIsSkippedRatherThanGuessedAt()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);

        // Opaque and failing: graded and reported.
        QVariantMap opaque{
            {QStringLiteral("textPrimary"), QStringLiteral("#6E7484")},
            {QStringLiteral("hover"), QStringLiteral("#3A3F4B")}};
        const QVariantList graded =
            store.auditForRole(opaque, QStringLiteral("hover"));
        QCOMPARE(graded.size(), 1);
        QCOMPARE(graded.first().toMap().value(QStringLiteral("passes")).toBool(),
                 false);

        // The same pair with a 22%-alpha hover, as Storm ships: not graded.
        QVariantMap translucent = opaque;
        translucent.insert(QStringLiteral("hover"),
                           QVariant::fromValue(QColor(0x3A, 0x3F, 0x4B, 56)));
        QCOMPARE(store.auditForRole(translucent, QStringLiteral("hover")).size(),
                 0);
        QCOMPARE(store.audit(translucent).size(), 0);

        // The same colour as 8-digit ARGB text, as a hand-edited config has it.
        QVariantMap asText = opaque;
        asText.insert(QStringLiteral("hover"), QStringLiteral("#383A3F4B"));
        QCOMPARE(store.audit(asText).size(), 0);

        // An opaque QColor is still graded; the guard keys on alpha.
        QVariantMap opaqueColor = opaque;
        opaqueColor.insert(QStringLiteral("hover"),
                           QVariant::fromValue(QColor(0x3A, 0x3F, 0x4B)));
        QCOMPARE(store.audit(opaqueColor).size(), 1);
    }

    // Every key the readability table reads must be a key paletteForTheme()
    // returns; a missing endpoint is skipped, so a rename in AppTheme would
    // silently stop grading. Reads the return literal (the resolved dialect);
    // `everyEditableRoleIsARealPaletteKey` covers the other direction.
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

    // Two themes whose contrast was measured on rendered pixels (Sand's room
    // list text at 3.08:1, its Send label at 1.62:1). The audit must name
    // them, and the finding count is pinned so a short report fails.
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

        const QString dangerFill =
            colorLiteral(qml, QStringLiteral("_accentDanger"));
        const auto merged = [&](const QString &base, const QVariantMap &over) {
            QVariantMap p = resolvedPalette(presets.value(base),
                                            QStringLiteral("#FFFFFF"),
                                            dangerFill);
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
        // Rendered "Send" button: 1.62:1; arithmetic: 1.63.
        QVERIFY(qAbs(findPair(sandReport, "accentText", "accent") - 1.63)
                < 0.01);
        // Rendered room-list name: 3.08:1; the audit grades the same pair.
        QVERIFY(qAbs(findPair(sandReport, "textPrimary", "sidebar") - 3.15)
                < 0.01);

        const QVariantList inkReport =
            store.audit(merged(QStringLiteral("Lightning Dark"), ink));
        // Ink darkens `accentText` but keeps the base `accentPressed`, so the
        // label is unreadable only while the button is held down.
        QCOMPARE(inkReport.size(), 20);
        QVERIFY(qAbs(findPair(inkReport, "accentText", "accent") - 2.68)
                < 0.01);
        QVERIFY(qAbs(findPair(inkReport, "accentText", "accentPressed") - 2.77)
                < 0.01);
        // The hovered state still passes, so the new pair is not just following
        // its neighbour.
        QCOMPARE(findPair(inkReport, "accentText", "accentHover"), -1.0);
        // `hover` was inherited, not overridden, and now clashes with the new
        // ink: only an audit over the resolved palette catches that.
        QVERIFY(qAbs(findPair(inkReport, "textPrimary", "hover") - 1.92)
                < 0.01);
        // Ink's hairline nearly vanishes: dL* 0.5 against its surface.
        QVERIFY(qAbs(findPair(inkReport, "border", "surface") - 0.50) < 0.02);

        // Every finding names a role the editor can open.
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

    // The live readout under an open picker keeps passes too, so a value
    // crossing its bar is visible.
    void theLiveReadoutForOneRoleKeepsItsPasses()
    {
        const QString qml = appTheme();
        const auto presets = presetPalettes(qml);
        SettingsManager settings;
        CustomThemeStore store(&settings);
        const QVariantMap storm =
            resolvedPalette(presets.value(QStringLiteral("Storm")),
                            QStringLiteral("#FFFFFF"));

        // Storm passes everything, so the narrowed report is all passes and
        // must not be empty.
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

    // A skipped check is named rather than silently dropped; otherwise every
    // consumer reads a skip as a pass. Invariant: graded + skipped equals the
    // whole table for any palette.
    void aSkippedCheckIsNamedRatherThanSilentlyDropped()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);

        const QVariantMap opaque{
            {QStringLiteral("textPrimary"), QStringLiteral("#6E7484")},
            {QStringLiteral("hover"), QStringLiteral("#3A3F4B")}};

        // Storm's shape: a 22%-alpha hover inherited from the base.
        QVariantMap translucent = opaque;
        translucent.insert(QStringLiteral("hover"),
                           QVariant::fromValue(QColor(0x3A, 0x3F, 0x4B, 56)));

        const QVariantList gaps =
            store.auditSkipped(translucent, QStringLiteral("hover"));
        QCOMPARE(gaps.size(), 1);
        const QVariantMap gap = gaps.first().toMap();
        QCOMPARE(gap.value(QStringLiteral("reason")).toString(),
                 QStringLiteral("translucent"));
        QCOMPARE(gap.value(QStringLiteral("bg")).toString(),
                 QStringLiteral("hover"));
        // Presentable: the editor prints the label and links to the role.
        QVERIFY(!gap.value(QStringLiteral("label")).toString().isEmpty());
        QVERIFY(CustomThemeStore::roleIsEditable(
            gap.value(QStringLiteral("role")).toString()));
        // No verdict: `passes: false` would read as a failure.
        QVERIFY2(!gap.contains(QStringLiteral("passes")),
                 "a check that was never made must not report a verdict");
        QVERIFY2(!gap.contains(QStringLiteral("value")),
                 "a check that was never made must not report a number");

        // The other reason: this build has no such key; not the user's doing.
        QVariantMap missing{
            {QStringLiteral("textPrimary"), QStringLiteral("#6E7484")}};
        const QVariantList absent =
            store.auditSkipped(missing, QStringLiteral("hover"));
        QCOMPARE(absent.size(), 1);
        QCOMPARE(absent.first().toMap().value(QStringLiteral("reason")).toString(),
                 QStringLiteral("missing"));

        // When both reasons apply, "missing" wins: it is our defect, and
        // blaming the user's translucent colour would be unfixable for them.
        QVariantMap both{
            {QStringLiteral("hover"), QColor(0x64, 0x69, 0xBF, 56)}};
        const QVariantList mixed =
            store.auditSkipped(both, QStringLiteral("hover"));
        QVERIFY(!mixed.isEmpty());
        for (const QVariant &row : mixed) {
            QCOMPARE(row.toMap().value(QStringLiteral("reason")).toString(),
                     QStringLiteral("missing"));
        }

        // Narrowing follows auditForRole: unknown role -> nothing, empty -> all.
        QCOMPARE(store.auditSkipped(translucent, QStringLiteral("nope")).size(),
                 0);

        // Every check is graded or skipped, never neither, on sparse,
        // translucent and real palettes.
        const QString qml = appTheme();
        const auto presets = presetPalettes(qml);
        const QVariantMap storm =
            resolvedPalette(presets.value(QStringLiteral("Storm")),
                            QStringLiteral("#FFFFFF"),
                            colorLiteral(qml, QStringLiteral("_accentDanger")));
        for (const QVariantMap &palette : {opaque, translucent, missing, storm}) {
            const int graded =
                store.auditForRole(palette, QString()).size();
            const int skipped = store.auditSkipped(palette, QString()).size();
            QCOMPARE(graded + skipped,
                     CustomThemeStore::readabilityCheckCount());
        }
    }

    // A check's graded palette key and its click-through role must name the
    // same colour, via the C++ alias map.
    void everyCheckGradesTheColourItsRoleWouldEdit()
    {
        const QVariantList checks = CustomThemeStore::readabilityChecks();
        // Not `checks.size() == readabilityCheckCount()` (same source, cannot
        // fail). Each entry must carry the six keys callers read; `fgRole` may
        // be empty but must be present.
        QVERIFY(!checks.isEmpty());
        for (const QVariant &entry : checks) {
            const QVariantMap m = entry.toMap();
            for (const char *key : { "fg", "fgRole", "bg", "bgRole",
                                     "label", "minimum", "kind" }) {
                QVERIFY2(m.contains(QLatin1String(key)),
                         qPrintable(QStringLiteral("a readability check is "
                                                   "missing '%1'")
                                        .arg(QLatin1String(key))));
            }
            QVERIFY(!m.value(QStringLiteral("label")).toString().isEmpty());
            QVERIFY(m.value(QStringLiteral("minimum")).toDouble() > 0.0);
            const QString kind = m.value(QStringLiteral("kind")).toString();
            QVERIFY2(kind == QStringLiteral("ink")
                     || kind == QStringLiteral("edge"),
                     qPrintable(QStringLiteral("unknown check kind '%1'")
                                    .arg(kind)));
        }

        SettingsManager settings;
        CustomThemeStore store(&settings);
        // roleAliases() is the invokable QML actually calls.
        QCOMPARE(store.roleAliases(), CustomThemeStore::paletteKeyAliases());
        QVERIFY(!store.roleAliases().isEmpty());
        const QStringList roles = CustomThemeStore::editableRoles();

        for (const QVariant &row : checks) {
            const QVariantMap m = row.toMap();
            const QString fg = m.value(QStringLiteral("fg")).toString();
            const QString bg = m.value(QStringLiteral("bg")).toString();
            const QString fgRole = m.value(QStringLiteral("fgRole")).toString();
            const QString bgRole = m.value(QStringLiteral("bgRole")).toString();
            const QString label = m.value(QStringLiteral("label")).toString();

            QVERIFY2(CustomThemeStore::roleIsEditable(bgRole),
                     qPrintable(QStringLiteral("'%1': the background role "
                                               "'%2' is not editable")
                                    .arg(label, bgRole)));
            QVERIFY2(store.paletteKeyForRole(bgRole) == bg,
                     qPrintable(QStringLiteral(
                                    "'%1' grades the palette key '%2' but "
                                    "sends a click to the role '%3', which "
                                    "is the colour '%4'")
                                    .arg(label, bg, bgRole,
                                         store.paletteKeyForRole(bgRole))));

            if (fgRole.isEmpty()) {
                // An ink AppTheme pins by literal: no editable role may resolve
                // to that key, or the report sends the user to the wrong side.
                for (const QString &role : roles) {
                    QVERIFY2(store.paletteKeyForRole(role) != fg,
                             qPrintable(QStringLiteral(
                                            "'%1' claims '%2' has no editable "
                                            "role, but '%3' resolves to it")
                                            .arg(label, fg, role)));
                }
                continue;
            }
            QVERIFY2(CustomThemeStore::roleIsEditable(fgRole),
                     qPrintable(QStringLiteral("'%1': the foreground role "
                                               "'%2' is not editable")
                                    .arg(label, fgRole)));
            QVERIFY2(store.paletteKeyForRole(fgRole) == fg,
                     qPrintable(QStringLiteral(
                                    "'%1' grades the palette key '%2' but "
                                    "sends a click to the role '%3', which "
                                    "is the colour '%4'")
                                    .arg(label, fg, fgRole,
                                         store.paletteKeyForRole(fgRole))));
        }

        // The alias map: roles whose palette spelling differs. The editor
        // resolves every swatch through it.
        const QVariantMap aliases = CustomThemeStore::paletteKeyAliases();
        QCOMPARE(aliases.size(), 3);
        QSet<QString> targets;
        for (auto it = aliases.constBegin(); it != aliases.constEnd(); ++it) {
            QVERIFY2(CustomThemeStore::roleIsEditable(it.key()),
                     qPrintable(QStringLiteral("'%1' is aliased but is not an "
                                               "editable role").arg(it.key())));
            const QString target = it.value().toString();
            QVERIFY2(target != it.key(),
                     "an alias that maps a role to its own name is noise");
            QVERIFY2(!targets.contains(target),
                     qPrintable(QStringLiteral("two roles both resolve to the "
                                               "palette key '%1'").arg(target)));
            targets.insert(target);
            QVERIFY2(CustomThemeStore::readabilityPaletteKeys().contains(target),
                     qPrintable(QStringLiteral(
                                    "'%1' is not a key the readability table "
                                    "reads, so nothing asserts it against "
                                    "paletteForTheme()").arg(target)));
            QCOMPARE(store.paletteKeyForRole(it.key()), target);
        }
        // Every other role is its own key; an unknown role is not invented.
        for (const QString &role : roles) {
            if (aliases.contains(role))
                continue;
            QCOMPARE(store.paletteKeyForRole(role), role);
        }
        QCOMPARE(store.paletteKeyForRole(QStringLiteral("nope")),
                 QStringLiteral("nope"));

        // The QML copy is gone; the dialog reads the map from the store.
        // AppTheme.qml sits beside the dialog, so its path is derived.
        const QString appThemePath = QStringLiteral(APPTHEME_QML_PATH);
        QFile dialog(appThemePath.left(appThemePath.lastIndexOf(QLatin1Char('/')) + 1)
                     + QStringLiteral("ThemeEditorDialog.qml"));
        QVERIFY2(dialog.open(QIODevice::ReadOnly | QIODevice::Text),
                 "ThemeEditorDialog.qml not readable");
        const QString source = QString::fromUtf8(dialog.readAll());
        QVERIFY2(source.contains(QStringLiteral("store.roleAliases()")),
                 "the editor no longer reads the alias map from the store");
        // Scoped to the shape, not the word: what must not return is a
        // `storeKeyAliases` whose right-hand side is an object literal.
        static const QRegularExpression literalAliases(
            QStringLiteral("storeKeyAliases\\s*:\\s*\\(?\\s*\\{"));
        QVERIFY2(!literalAliases.match(source).hasMatch(),
                 "ThemeEditorDialog.qml is keeping its own copy of the alias "
                 "map again");
    }

    // ---- couplings that would fail silently ----

    // AppTheme merges overrides straight over the base palette, so an editable
    // role that is not a palette key paints nothing.
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
        // Required regions: room list, spaces rail, header and the rest.
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
        // No duplicates: a second row would shadow the first.
        QCOMPARE(QSet<QString>(roles.begin(), roles.end()).size(), roles.size());
    }

    // New shell tokens reach custom themes by deriving from an editable role,
    // never by adding a required key to every palette.
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
            // ...and the source role is editable, so the editor moves the new
            // surfaces.
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

    // The id is duplicated between C++ and QML (QML cannot see the enum), so
    // it is asserted.
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
        // Second gate: this palette comes from a hand-editable file, and an
        // unparseable colour paints the shell transparent.
        QVERIFY2(qml.contains(QStringLiteral("/^#[0-9A-Fa-f]{6}$/.test(v)")),
                 "the QML-side override guard is gone");
    }

    // The custom theme is classified light or dark by its own background, not
    // its base, since a light palette can sit on a dark base.
    void theCustomThemeClassifiesItselfLightOrDark()
    {
        const QString qml = appTheme();
        QVERIFY2(qml.contains(QStringLiteral("effectiveTheme === 12")),
                 "`dark` no longer special-cases the custom palette");
        QVERIFY2(qml.contains(QStringLiteral("relativeLuminance(_p.background)")),
                 "the custom palette must be classified by its own background");
    }

    // ---- account scoping ----

    // The collection is stored per account but load() fills its cache once.
    // Without invalidation on account change, the new account sees the old
    // themes and its first write persists them over its own record.
    // Asserts both what is shown and what the first write persists.
    void aSwitchDropsTheOutgoingAccountsThemesInsteadOfSavingThemOverTheNext()
    {
        SettingsManager settings;
        settings.saveSession(kHs, kBob, QStringLiteral("BDEV"), QString());
        settings.saveSession(kHs, kAlice, QStringLiteral("ADEV"), QString());

        // Seeded through short-lived stores per account; one long-lived store
        // would itself be corrupted by the defect. discard() first because
        // appearanceValue mirrors writes into a device-global fallback.
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

        // The switch.
        settings.setActiveAccountUserId(kBob);

        // (a) what Bob is shown.
        QCOMPARE(themeNames(store), QStringList({ QStringLiteral("Bob only") }));

        // (b) what Bob's first write persists.
        store.setName(QStringLiteral("Bob renamed"));

        CustomThemeStore reread(&settings);
        QCOMPARE(themeNames(reread),
                 QStringList({ QStringLiteral("Bob renamed") }));

        // Alice keeps hers.
        settings.setActiveAccountUserId(kAlice);
        CustomThemeStore aliceAgain(&settings);
        QCOMPARE(themeNames(aliceAgain),
                 QStringList({ QStringLiteral("Alice one"),
                               QStringLiteral("Alice two") }));
    }

    // A sign-in writes the active-account pointer directly (saveSession) with
    // no switch, so invalidation hangs off sessionChanged.
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

    // Invalidation notifies: the Appearance page and AppTheme.qml re-read on
    // customThemeChanged.
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
    // Names in stored order; the whole list, since two lists can share a
    // length.
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
