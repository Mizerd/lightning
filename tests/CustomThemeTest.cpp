// Custom-theme contract.
//
// The store is the gate between a config file a user can hand-edit and the
// renderer, so most of this is about what it REFUSES. The rest pins the two
// couplings that would fail silently: the role table against AppTheme's
// palette keys, and theme id 12 against the routing in AppTheme.qml.

#include "app/CustomThemeStore.h"
#include "app/SettingsManager.h"

#include <QFile>
#include <QRegularExpression>
#include <QSettings>
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

} // namespace

class CustomThemeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("custom-theme-test"));
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
        // the common intent, and re-picking both would be busywork.
        reopened.resetAll();
        QCOMPARE(reopened.overrideCount(), 0);
        QVERIFY(!reopened.exists());
        QCOMPARE(reopened.baseTheme(), int(SettingsManager::GraphiteTheme));
        QCOMPARE(reopened.name(), QStringLiteral("Mine"));

        // discard forgets the theme itself.
        reopened.setColor(QStringLiteral("accent"), QStringLiteral("#AA0000"));
        reopened.discard();
        QVERIFY(!reopened.exists());
        QVERIFY(reopened.name().isEmpty());
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

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(CustomThemeTest)
#include "CustomThemeTest.moc"
