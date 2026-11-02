
#include "app/CustomThemeStore.h"
#include "app/SettingsManager.h"
#include "support/BadgeColourRules.h"

#include <QColor>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>
#include <QHash>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtTest>

#include <functional>

namespace {

QString appTheme()
{
    QFile f(QStringLiteral(APPTHEME_QML_PATH));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

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

QHash<QString, QString> elementGroupsTable(const QString &qml)
{
    QHash<QString, QString> out;
    static const QRegularExpression block(
        QStringLiteral("readonly\\s+property\\s+var\\s+elementGroups:\\s*\\(\\{(.*?)\\}\\)"),
        QRegularExpression::DotMatchesEverythingOption);
    const auto m = block.match(qml);
    if (!m.hasMatch())
        return out;
    static const QRegularExpression pair(
        QStringLiteral("(\\w+)\\s*:\\s*\"(\\w+)\""));
    auto it = pair.globalMatch(m.captured(1));
    while (it.hasNext()) {
        const auto p = it.next();
        out.insert(p.captured(1), p.captured(2));
    }
    return out;
}

QSet<QString> resolverKeys(const QString &qml)
{
    QSet<QString> keys;
    const int fn = qml.indexOf(QStringLiteral("function paletteForTheme("));
    if (fn < 0)
        return keys;
    const QString open = QStringLiteral("var out = {");
    const int ret = qml.indexOf(open, fn);
    const int end = qml.indexOf(QStringLiteral("\n        }\n"), ret);
    if (ret < 0 || end < 0)
        return keys;
    QString body = qml.mid(ret + open.size(), end - ret - open.size());
    body.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
    static const QRegularExpression key(
        QStringLiteral("^\\s*(\\w+)\\s*:"),
        QRegularExpression::MultilineOption);
    auto it = key.globalMatch(body);
    while (it.hasNext())
        keys.insert(it.next().captured(1));
    const QHash<QString, QString> elements = elementGroupsTable(qml);
    for (auto e = elements.constBegin(); e != elements.constEnd(); ++e)
        keys.insert(e.key());
    return keys;
}

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

QString colorLiteral(const QString &qml, const QString &name)
{
    const QRegularExpression re(
        QStringLiteral("readonly\\s+property\\s+color\\s+%1:\\s*"
                       "\"(#[0-9A-Fa-f]{6})\"").arg(name));
    const auto m = re.match(qml);
    return m.hasMatch() ? m.captured(1).toUpper() : QString();
}

void fillElements(QVariantMap &p, const QVariantMap &own, const QString &qml)
{
    QHash<QString, QString> groups = elementGroupsTable(qml);
    groups.insert(QStringLiteral("embedSurface"), QStringLiteral("cardElevated"));
    groups.insert(QStringLiteral("roomSelected"), QStringLiteral("selected"));
    groups.insert(QStringLiteral("popoverSurface"), QStringLiteral("surface"));
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        if (own.contains(it.key()))
            continue;
        if (p.contains(it.value()))
            p.insert(it.key(), p.value(it.value()));
    }
    badgerules::addDerived(p, own);
}

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
    if (raw.contains(QStringLiteral("inputBg")))
        p.insert(QStringLiteral("inputBackground"),
                 raw.value(QStringLiteral("inputBg")));
    p.insert(QStringLiteral("reactionBackground"),
             raw.value(QStringLiteral("reaction"),
                       raw.value(QStringLiteral("cardElevated"))));
    const QString mention = raw.value(QStringLiteral("mention"), dangerFill);
    if (!mention.isEmpty())
        p.insert(QStringLiteral("mentionBadge"), mention);
    p.insert(QStringLiteral("ownBubbleText"), QStringLiteral("#FFFFFF"));
    fallback("rail", raw.value(QStringLiteral("sidebar")));
    fallback("accentText", onAccent);
    fallback("link", raw.value(QStringLiteral("accent")));
    fallback("selectedText", raw.value(QStringLiteral("textPrimary")));
    fallback("otherBubble", raw.value(QStringLiteral("cardElevated")));
    fallback("unreadBadge", raw.value(QStringLiteral("accent")));
    fillElements(p, QVariantMap(), appTheme());
    return p;
}

const QString kAlice = QStringLiteral("@alice:one.example");
const QString kBob = QStringLiteral("@bob:one.example");
const QString kHs = QStringLiteral("https://one.example");

}

class CustomThemeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

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

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void onlyOpaqueSixDigitHexIsAccepted()
    {
        QVERIFY(CustomThemeStore::colorIsValid(QStringLiteral("#1D57FF")));
        QVERIFY(CustomThemeStore::colorIsValid(QStringLiteral("#abcdef")));

        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("#801D57FF")));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("red")));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("transparent")));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("#FFF")));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("1D57FF")));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("#1D57F")));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("#1D57FFF")));
        QVERIFY(!CustomThemeStore::colorIsValid(QString()));
        QVERIFY(!CustomThemeStore::colorIsValid(QStringLiteral("#GGGGGG")));
        QVERIFY(!CustomThemeStore::colorIsValid(
            QStringLiteral("#000000\"; evil: 1")));
    }

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
        QCOMPARE(store.overrideCount(), 1);
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#123456"));
    }

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

        reopened.resetAll();
        QCOMPARE(reopened.overrideCount(), 0);
        QVERIFY(reopened.exists());
        QCOMPARE(reopened.baseTheme(), int(SettingsManager::GraphiteTheme));
        QCOMPARE(reopened.name(), QStringLiteral("Mine"));

        reopened.setColor(QStringLiteral("accent"), QStringLiteral("#AA0000"));
        reopened.discard();
        QVERIFY(!reopened.exists());
        QVERIFY(reopened.themes().isEmpty());
        QVERIFY(reopened.name().isEmpty());
    }

    void aColourGroupLinksUnrelatedRolesInOneStep()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("Groups"));
        store.setColor(QStringLiteral("accent"), QStringLiteral("#112233"));
        store.sealUndoStep();
        store.setColor(QStringLiteral("unreadBadge"), QStringLiteral("#445566"));
        store.sealUndoStep();
        const QVariantMap made = store.createColorGroup(
            QStringLiteral("Highlights"),
            { QStringLiteral("accent"), QStringLiteral("unreadBadge") },
            QStringLiteral("#aa0000"), false);
        QVERIFY2(made.value(QStringLiteral("ok")).toBool(),
                 qPrintable(made.value(QStringLiteral("error")).toString()));
        const QString id = made.value(QStringLiteral("id")).toString();
        QCOMPARE(store.groupOfRole(QStringLiteral("accent")), id);
        QCOMPARE(store.groupOfRole(QStringLiteral("unreadBadge")), id);
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#AA0000"));
        QCOMPARE(store.colors().value(QStringLiteral("unreadBadge")).toString(),
                 QStringLiteral("#AA0000"));
        QCOMPARE(store.colorGroups().size(), 1);

        store.setColor(QStringLiteral("unreadBadge"), QStringLiteral("#00AA00"));
        store.sealUndoStep();
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#00AA00"));

        QVERIFY(store.undo());
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#AA0000"));
        QVERIFY(store.undo());
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#112233"));
        QCOMPARE(store.colors().value(QStringLiteral("unreadBadge")).toString(),
                 QStringLiteral("#445566"));
        QVERIFY(store.colorGroups().isEmpty());
        QVERIFY(store.redo());
        QCOMPARE(store.groupOfRole(QStringLiteral("accent")), id);
    }

    void aGroupColourDragIsOneStep()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("Drag"));
        const QString id = store.createColorGroup(
            QStringLiteral("Pair"),
            { QStringLiteral("accent"), QStringLiteral("link") },
            QStringLiteral("#101010"), false).value(QStringLiteral("id")).toString();
        QVERIFY(!id.isEmpty());
        const QString undoBefore = store.undoLabel();
        for (int i = 0; i < 30; ++i) {
            const QString hex = QStringLiteral("#%1%1%1")
                .arg(0x20 + i * 4, 2, 16, QLatin1Char('0')).toUpper();
            QVERIFY(store.setGroupColor(id, hex));
            QCOMPARE(store.colors().value(QStringLiteral("accent")),
                     store.colors().value(QStringLiteral("link")));
        }
        store.sealUndoStep();
        QVERIFY(store.undoLabel() != undoBefore);
        QVERIFY(store.undo());
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#101010"));
        QCOMPARE(store.colors().value(QStringLiteral("link")).toString(),
                 QStringLiteral("#101010"));
        QCOMPARE(store.undoLabel(), undoBefore);
    }

    void detachAndUngroupKeepTheAppearance()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("Detach"));
        const QString id = store.createColorGroup(
            QStringLiteral("Three"),
            { QStringLiteral("accent"), QStringLiteral("link"),
              QStringLiteral("focusRing") },
            QStringLiteral("#123456"), false).value(QStringLiteral("id")).toString();
        QVERIFY(store.detachFromColorGroup(QStringLiteral("focusRing")));
        QCOMPARE(store.colors().value(QStringLiteral("focusRing")).toString(),
                 QStringLiteral("#123456"));
        QVERIFY(store.groupOfRole(QStringLiteral("focusRing")).isEmpty());
        store.setGroupColor(id, QStringLiteral("#654321"));
        store.sealUndoStep();
        QCOMPARE(store.colors().value(QStringLiteral("focusRing")).toString(),
                 QStringLiteral("#123456"));
        QCOMPARE(store.colors().value(QStringLiteral("link")).toString(),
                 QStringLiteral("#654321"));

        QVERIFY(store.deleteColorGroup(id));
        QVERIFY(store.colorGroups().isEmpty());
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#654321"));
        QVERIFY(store.undo());
        QCOMPARE(store.groupOfRole(QStringLiteral("link")), id);

        QVERIFY(store.detachFromColorGroup(QStringLiteral("link")));
        QVERIFY(store.colorGroups().isEmpty());
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#654321"));
    }

    void movingARoleBetweenGroupsIsExplicit()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("Move"));
        const QString a = store.createColorGroup(
            QStringLiteral("A"), { QStringLiteral("accent"), QStringLiteral("link"),
                                   QStringLiteral("focusRing") },
            QStringLiteral("#111111"), false).value(QStringLiteral("id")).toString();
        const QString b = store.createColorGroup(
            QStringLiteral("B"), { QStringLiteral("success"), QStringLiteral("warning") },
            QStringLiteral("#222222"), false).value(QStringLiteral("id")).toString();
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        QVariantMap refused = store.addToColorGroup(b, QStringLiteral("link"), false);
        QVERIFY(!refused.value(QStringLiteral("ok")).toBool());
        QCOMPARE(store.groupOfRole(QStringLiteral("link")), a);
        QVariantMap moved = store.addToColorGroup(b, QStringLiteral("link"), true);
        QVERIFY(moved.value(QStringLiteral("ok")).toBool());
        QCOMPARE(store.groupOfRole(QStringLiteral("link")), b);
        QCOMPARE(store.colors().value(QStringLiteral("link")).toString(),
                 QStringLiteral("#222222"));
        QVERIFY(!store.createColorGroup(QStringLiteral("C"),
                    { QStringLiteral("accent"), QStringLiteral("danger") },
                    QStringLiteral("#333333"), false).value(QStringLiteral("ok")).toBool());
        QVERIFY(store.undo());
        QCOMPARE(store.groupOfRole(QStringLiteral("link")), a);
    }

    void impactGradientsAndResetsAreHonest()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("Impact"));
        const QVariantMap impact = store.colorGroupImpact(
            { QStringLiteral("selected"), QStringLiteral("accent") });
        QStringList dependents;
        for (const QVariant &d : impact.value(QStringLiteral("dependents")).toList())
            dependents << d.toMap().value(QStringLiteral("role")).toString();
        QVERIFY2(dependents.contains(QStringLiteral("roomSelected")),
                 qPrintable(dependents.join(QLatin1Char(','))));
        QVERIFY(!dependents.contains(QStringLiteral("accent")));

        QVariantMap grad;
        grad.insert(QStringLiteral("type"), QStringLiteral("linear"));
        grad.insert(QStringLiteral("angle"), 180);
        grad.insert(QStringLiteral("stops"),
                    QVariantList{ QStringLiteral("#000000"), QStringLiteral("#FFFFFF") });
        QVERIFY(store.setGradient(QStringLiteral("background"), grad));
        const QVariantMap refused = store.createColorGroup(
            QStringLiteral("Bad"),
            { QStringLiteral("background"), QStringLiteral("textPrimary") },
            QStringLiteral("#444444"), false);
        QVERIFY(!refused.value(QStringLiteral("ok")).toBool());
        QVERIFY(!refused.value(QStringLiteral("error")).toString().isEmpty());
        QVERIFY(store.gradients().contains(QStringLiteral("background")));

        const QString id = store.createColorGroup(
            QStringLiteral("Surfaces"),
            { QStringLiteral("surface"), QStringLiteral("sidebar"),
              QStringLiteral("roomList") },
            QStringLiteral("#202020"), false).value(QStringLiteral("id")).toString();
        QVERIFY(!id.isEmpty());
        QVERIFY(store.setGradient(QStringLiteral("sidebar"), grad));
        QVERIFY(store.groupOfRole(QStringLiteral("sidebar")).isEmpty());
        store.resetColor(QStringLiteral("roomList"));
        QVERIFY(store.groupOfRole(QStringLiteral("roomList")).isEmpty());
        QVERIFY(!store.colors().contains(QStringLiteral("roomList")));
        QVERIFY(store.colorGroups().isEmpty());
        QCOMPARE(store.colors().value(QStringLiteral("surface")).toString(),
                 QStringLiteral("#202020"));
    }

    void groupsSurviveRestartDuplicateAndExport()
    {
        SettingsManager settings;
        QString payload;
        {
            CustomThemeStore store(&settings);
            store.createTheme(QStringLiteral("Keep"));
            QVERIFY(store.createColorGroup(QStringLiteral("Pair"),
                        { QStringLiteral("accent"), QStringLiteral("link") },
                        QStringLiteral("#ABCDEF"), false).value(QStringLiteral("ok")).toBool());
            payload = store.exportTheme(store.activeThemeId());
        }
        CustomThemeStore reopened(&settings);
        QCOMPARE(reopened.colorGroups().size(), 1);
        QVERIFY(!reopened.groupOfRole(QStringLiteral("link")).isEmpty());
        reopened.duplicateActiveTheme(QStringLiteral("Copy"));
        QCOMPARE(reopened.colorGroups().size(), 1);

        const QJsonObject shared = QJsonDocument::fromJson(payload.toUtf8()).object();
        QCOMPARE(shared.value(QStringLiteral("colors")).toObject()
                     .value(QStringLiteral("link")).toString(),
                 QStringLiteral("#ABCDEF"));
        QCOMPARE(shared.value(QStringLiteral("groups")).toArray().size(), 1);
        QVERIFY(reopened.importTheme(payload).isEmpty());
        QCOMPARE(reopened.colorGroups().size(), 1);
        QCOMPARE(reopened.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#ABCDEF"));
    }

    void untrustedGroupsAreBoundedAndRepaired()
    {
        QVariantMap colors{ { QStringLiteral("accent"), QStringLiteral("#010101") },
                            { QStringLiteral("link"), QStringLiteral("#020202") } };
        QVariantList none;
        QVariantMap untouched = colors;
        QCOMPARE(CustomThemeStore::normalizeColorGroups(&none, &untouched, {}), 0);
        QCOMPARE(untouched, colors);

        QVariantList groups;
        for (int i = 0; i < 40; ++i) {
            groups << QVariantMap{
                { QStringLiteral("id"), QStringLiteral("g%1").arg(i) },
                { QStringLiteral("name"), QStringLiteral("n") },
                { QStringLiteral("color"), QStringLiteral("#0000FF") },
                { QStringLiteral("members"),
                  QVariantList{ QStringLiteral("accent"), QStringLiteral("link") } } };
        }
        groups.prepend(QVariantMap{
            { QStringLiteral("id"), QStringLiteral("<script>") },
            { QStringLiteral("name"), QString(500, QLatin1Char('x')) },
            { QStringLiteral("color"), QStringLiteral("#00FF00") },
            { QStringLiteral("members"),
              QVariantList{ QStringLiteral("accent"), QStringLiteral("nonsense"),
                            QStringLiteral("textPrimary"), QStringLiteral("accent") } } });
        groups.prepend(QVariantMap{ { QStringLiteral("color"), QStringLiteral("red") } });
        QVariantMap c = colors;
        QVERIFY(CustomThemeStore::normalizeColorGroups(&groups, &c, {}) > 0);
        QCOMPARE(groups.size(), 1);
        const QVariantMap g = groups.first().toMap();
        QCOMPARE(g.value(QStringLiteral("members")).toStringList(),
                 (QStringList{ QStringLiteral("accent"), QStringLiteral("textPrimary") }));
        QVERIFY(g.value(QStringLiteral("name")).toString().size() <= CustomThemeStore::kMaxNameLength);
        QVERIFY(g.value(QStringLiteral("id")).toString() != QStringLiteral("<script>"));
        QCOMPARE(c.value(QStringLiteral("textPrimary")).toString(), QStringLiteral("#00FF00"));
        QCOMPARE(c.value(QStringLiteral("link")).toString(), QStringLiteral("#020202"));
    }

    void theFirstThemeStartsFromTheActiveBuiltInPalette()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        settings.setTheme(SettingsManager::IndigoNightTheme);
        const QString id = store.createTheme(QString());
        QCOMPARE(store.baseTheme(), int(SettingsManager::IndigoNightTheme));
        store.deleteTheme(id);
        QVERIFY(!store.exists());
        settings.setTheme(SettingsManager::SystemTheme);
        store.createTheme(QString());
        QCOMPARE(store.baseTheme(), int(SettingsManager::StormTheme));
    }

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

        store.setBaseTheme(SettingsManager::GraphiteTheme);
        const QString copy = store.duplicateActiveTheme(QStringLiteral("Ocean 2"));
        QVERIFY(!copy.isEmpty());
        QVERIFY(copy != first);
        QCOMPARE(store.activeThemeId(), copy);
        QCOMPARE(store.name(), QStringLiteral("Ocean 2"));
        QCOMPARE(store.baseTheme(), int(SettingsManager::GraphiteTheme));
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#112233"));

        store.setColor(QStringLiteral("accent"), QStringLiteral("#445566"));
        store.setActiveThemeId(first);
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#112233"));

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
        QVERIFY(!shared.contains(QLatin1Char('\n')));

        QCOMPARE(store.importTheme(shared), QString());
        QCOMPARE(store.themes().size(), 2);
        QCOMPARE(store.baseTheme(), int(SettingsManager::MidnightBlueTheme));
        QCOMPARE(store.colors().value(QStringLiteral("sidebar")).toString(),
                 QStringLiteral("#445566"));

        const QString hostile = QStringLiteral(
            "{\"lightning_theme\":1,\"name\":\"X\",\"base\":12,"
            "\"colors\":{\"accent\":\"#010203\",\"notARole\":\"#FFFFFF\","
            "\"sidebar\":\"#80FFFFFF\",\"border\":\"red\"}}");
        QCOMPARE(store.importTheme(hostile), QString());
        QCOMPARE(store.baseTheme(), int(SettingsManager::StormTheme));
        QCOMPARE(store.colors().size(), 1);
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#010203"));

        QVERIFY(!store.importTheme(QStringLiteral("hello")).isEmpty());
        QVERIFY(!store.importTheme(QStringLiteral("{}")).isEmpty());
        QVERIFY(!store.importTheme(
                     QStringLiteral("{\"lightning_theme\":1,\"colors\":{}}"))
                     .isEmpty());
    }

    void anExistingSingleThemeSurvivesTheUpgradeExactlyOnce()
    {
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

        store.resetColor(QStringLiteral("accent"));
        QCOMPARE(changed.count(), 4);
        store.resetAll();
        QCOMPARE(changed.count(), 4);
        store.setBaseTheme(SettingsManager::NordTheme);
        QCOMPARE(changed.count(), 4);
        store.setName(QStringLiteral("x"));
        QCOMPARE(changed.count(), 4);
    }

    void nudgeToPassMovesOnlyLightnessAndOnlyAsFarAsNeeded()
    {
        const auto nudge = &CustomThemeStore::nudgeToPass;
        const QString ground = QStringLiteral("#202020");

        QCOMPARE(nudge(QStringLiteral("#f0f0f0"), ground, 4.5, false),
                 QStringLiteral("#F0F0F0"));

        const QString lifted = nudge(QStringLiteral("#303030"), ground, 4.5, false);
        QVERIFY2(!lifted.isEmpty(), "a reachable bar returned nothing");
        QVERIFY(CustomThemeStore::contrast(lifted, ground) >= 4.5);
        QVERIFY(CustomThemeStore::lstar(lifted) > CustomThemeStore::lstar(QStringLiteral("#303030")));
        QVERIFY(CustomThemeStore::lstar(lifted) < 90.0);

        const QColor blue = QColor::fromString(
            nudge(QStringLiteral("#1a2a60"), ground, 4.5, false));
        QVERIFY(blue.isValid());
        QVERIFY(blue.blue() > blue.red() && blue.blue() > blue.green());

        const QString darker = nudge(QStringLiteral("#d0d0d0"),
                                     QStringLiteral("#ffffff"), 4.5, false);
        QVERIFY(!darker.isEmpty());
        QVERIFY(CustomThemeStore::contrast(darker, QStringLiteral("#ffffff")) >= 4.5);
        QVERIFY(CustomThemeStore::lstar(darker) < CustomThemeStore::lstar(QStringLiteral("#d0d0d0")));

        const QString edge = nudge(QStringLiteral("#252525"), ground, 8.0, true);
        QVERIFY(!edge.isEmpty());
        QVERIFY(qAbs(CustomThemeStore::lstar(edge) - CustomThemeStore::lstar(ground)) >= 8.0);

        QVERIFY(nudge(QStringLiteral("#80303030"), ground, 4.5, false).isEmpty());
        QVERIFY(nudge(QStringLiteral("#303030"), QStringLiteral("nonsense"), 4.5, false).isEmpty());
        QVERIFY(nudge(QStringLiteral("nonsense"), ground, 4.5, false).isEmpty());
        QVERIFY(nudge(QStringLiteral("#303030"), ground, 30.0, false).isEmpty());
    }

    void readableColorIsTheSameAnswerAsNudgeToPass()
    {
        CustomThemeStore store(nullptr);
        QCOMPARE(store.readableColor(QStringLiteral("#303030"),
                                     QStringLiteral("#202020"), 4.5, false),
                 CustomThemeStore::nudgeToPass(QStringLiteral("#303030"),
                                               QStringLiteral("#202020"), 4.5, false));
    }

    void contrastMatchesPublishedReferenceValues()
    {
        const auto ratio = &CustomThemeStore::contrast;
        QVERIFY(qAbs(ratio(QStringLiteral("#000000"),
                           QStringLiteral("#FFFFFF")) - 21.0) < 0.001);
        QVERIFY(qAbs(ratio(QStringLiteral("#FFFFFF"),
                           QStringLiteral("#FFFFFF")) - 1.0) < 0.001);
        QCOMPARE(ratio(QStringLiteral("#000000"), QStringLiteral("#FFFFFF")),
                 ratio(QStringLiteral("#FFFFFF"), QStringLiteral("#000000")));
        QVERIFY(qAbs(ratio(QStringLiteral("#767676"),
                           QStringLiteral("#FFFFFF")) - 4.54) < 0.01);
        QVERIFY(qAbs(ratio(QStringLiteral("#949494"),
                           QStringLiteral("#FFFFFF")) - 3.03) < 0.01);

        QCOMPARE(ratio(QStringLiteral("#FF000000"), QStringLiteral("#FFFFFF")),
                 ratio(QStringLiteral("#000000"), QStringLiteral("#FFFFFF")));

        QCOMPARE(ratio(QStringLiteral("nonsense"), QStringLiteral("#FFFFFF")),
                 0.0);

        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#000000")))
                < 0.001);
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#FFFFFF")) - 100.0)
                < 0.001);
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#777777")) - 50.0)
                < 0.5);
        QVERIFY(qAbs(CustomThemeStore::lstar(QStringLiteral("#0A0A0A")) - 2.74)
                < 0.05);
    }

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
            const QVariantList bad = store.audit(palette);
            const QVariantList all = store.auditForRole(palette, QString());
            evaluated += all.size();

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

        QCOMPARE(CustomThemeStore::readabilityCheckCount(), 35);
        QCOMPARE(evaluated, 11 * 35 - 1);

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
                QCOMPARE(m.value(QStringLiteral("reason")).toString(),
                         QStringLiteral("missing"));
                QVERIFY2(!m.contains(QStringLiteral("passes")),
                         "a check that was never made must not carry a "
                         "verdict");
            }
        }
        QCOMPARE(skipped, 1);
    }

    void aTranslucentPaletteEntryIsSkippedRatherThanGuessedAt()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);

        QVariantMap opaque{
            {QStringLiteral("textPrimary"), QStringLiteral("#6E7484")},
            {QStringLiteral("hover"), QStringLiteral("#3A3F4B")}};
        const QVariantList graded =
            store.auditForRole(opaque, QStringLiteral("hover"));
        QCOMPARE(graded.size(), 1);
        QCOMPARE(graded.first().toMap().value(QStringLiteral("passes")).toBool(),
                 false);

        QVariantMap translucent = opaque;
        translucent.insert(QStringLiteral("hover"),
                           QVariant::fromValue(QColor(0x3A, 0x3F, 0x4B, 56)));
        QCOMPARE(store.auditForRole(translucent, QStringLiteral("hover")).size(),
                 0);
        QCOMPARE(store.audit(translucent).size(), 0);

        QVariantMap asText = opaque;
        asText.insert(QStringLiteral("hover"), QStringLiteral("#383A3F4B"));
        QCOMPARE(store.audit(asText).size(), 0);

        QVariantMap opaqueColor = opaque;
        opaqueColor.insert(QStringLiteral("hover"),
                           QVariant::fromValue(QColor(0x3A, 0x3F, 0x4B)));
        QCOMPARE(store.audit(opaqueColor).size(), 1);
    }

    void everyReadabilityKeyIsAKeyPaletteForThemeReturns()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");

        const int start = qml.indexOf(QStringLiteral("function paletteForTheme"));
        QVERIFY2(start >= 0, "paletteForTheme is gone from AppTheme.qml");
        const int open = qml.indexOf(QStringLiteral("var out = {"), start);
        QVERIFY2(open >= 0, "paletteForTheme no longer builds an object");
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
        const QHash<QString, QString> elements = elementGroupsTable(qml);
        QVERIFY2(elements.size() > 50, "the elementGroups scan found almost nothing");
        for (auto e = elements.constBegin(); e != elements.constEnd(); ++e)
            returned.insert(e.key());
        const int fnEnd = qml.indexOf(QStringLiteral("\n    }"), close);
        static const QRegularExpression assigned(QStringLiteral("\\bout\\.(\\w+)\\s*="));
        auto a = assigned.globalMatch(qml.mid(close, fnEnd - close));
        int derivedSeen = 0;
        const QVariantMap derived = CustomThemeStore::derivedPaletteKeys();
        while (a.hasNext()) {
            const QString k = a.next().captured(1);
            returned.insert(k);
            if (derived.contains(k))
                ++derivedSeen;
        }
        QCOMPARE(derivedSeen, derived.size());
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
            fillElements(p, over, qml);
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
        QCOMPARE(sandReport.size(), 24);
        QVERIFY(qAbs(findPair(sandReport, "accentText", "accent") - 1.63)
                < 0.01);
        QVERIFY(qAbs(findPair(sandReport, "roomNameUnread", "roomList") - 3.15)
                < 0.01);

        const QVariantList inkReport =
            store.audit(merged(QStringLiteral("Lightning Dark"), ink));
        QCOMPARE(inkReport.size(), 26);
        QCOMPARE(findPair(inkReport, "unreadCountInk", "unreadCountFill"),
                 -1.0);
        QVERIFY(qAbs(findPair(inkReport, "accentText", "accent") - 2.68)
                < 0.01);
        QVERIFY(qAbs(findPair(inkReport, "buttonPrimaryInk", "accentPressed") - 2.77)
                < 0.01);
        QCOMPARE(findPair(inkReport, "buttonPrimaryInk", "accentHover"), -1.0);
        QVERIFY(qAbs(findPair(inkReport, "textPrimary", "hover") - 1.92)
                < 0.01);
        QVERIFY(qAbs(findPair(inkReport, "border", "surface") - 0.50) < 0.02);

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

    void theLiveReadoutForOneRoleKeepsItsPasses()
    {
        const QString qml = appTheme();
        const auto presets = presetPalettes(qml);
        SettingsManager settings;
        CustomThemeStore store(&settings);
        const QVariantMap storm =
            resolvedPalette(presets.value(QStringLiteral("Storm")),
                            QStringLiteral("#FFFFFF"));

        const QVariantList rows =
            store.auditForRole(storm, QStringLiteral("timelineBackground"));
        QVERIFY2(!rows.isEmpty(),
                 "a role with no problems must still report its numbers");
        for (const QVariant &row : rows) {
            const QVariantMap m = row.toMap();
            QVERIFY(m.value(QStringLiteral("passes")).toBool());
            QVERIFY(m.value(QStringLiteral("fg")).toString()
                        == QLatin1String("timelineBackground")
                    || m.value(QStringLiteral("bg")).toString()
                        == QLatin1String("timelineBackground"));
        }
        QCOMPARE(store.audit(storm).size(), 0);

        QCOMPARE(store.auditForRole(storm, QStringLiteral("nope")).size(), 0);
    }

    void aSkippedCheckIsNamedRatherThanSilentlyDropped()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);

        const QVariantMap opaque{
            {QStringLiteral("textPrimary"), QStringLiteral("#6E7484")},
            {QStringLiteral("hover"), QStringLiteral("#3A3F4B")}};

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
        QVERIFY(!gap.value(QStringLiteral("label")).toString().isEmpty());
        QVERIFY(CustomThemeStore::roleIsEditable(
            gap.value(QStringLiteral("role")).toString()));
        QVERIFY2(!gap.contains(QStringLiteral("passes")),
                 "a check that was never made must not report a verdict");
        QVERIFY2(!gap.contains(QStringLiteral("value")),
                 "a check that was never made must not report a number");

        QVariantMap missing{
            {QStringLiteral("textPrimary"), QStringLiteral("#6E7484")}};
        const QVariantList absent =
            store.auditSkipped(missing, QStringLiteral("hover"));
        QCOMPARE(absent.size(), 1);
        QCOMPARE(absent.first().toMap().value(QStringLiteral("reason")).toString(),
                 QStringLiteral("missing"));

        QVariantMap both{
            {QStringLiteral("hover"), QColor(0x64, 0x69, 0xBF, 56)}};
        const QVariantList mixed =
            store.auditSkipped(both, QStringLiteral("hover"));
        QVERIFY(!mixed.isEmpty());
        for (const QVariant &row : mixed) {
            QCOMPARE(row.toMap().value(QStringLiteral("reason")).toString(),
                     QStringLiteral("missing"));
        }

        QCOMPARE(store.auditSkipped(translucent, QStringLiteral("nope")).size(),
                 0);

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

    void everyCheckGradesTheColourItsRoleWouldEdit()
    {
        const QVariantList checks = CustomThemeStore::readabilityChecks();
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
            const QVariantMap derived = CustomThemeStore::derivedPaletteKeys();
            QVERIFY2(store.paletteKeyForRole(bgRole) == bg
                         || derived.value(bg).toString() == bgRole,
                     qPrintable(QStringLiteral(
                                    "'%1' grades the palette key '%2' but "
                                    "sends a click to the role '%3', which "
                                    "is the colour '%4'")
                                    .arg(label, bg, bgRole,
                                         store.paletteKeyForRole(bgRole))));

            if (fgRole.isEmpty()) {
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
            QVERIFY2(store.paletteKeyForRole(fgRole) == fg
                         || derived.value(fg).toString() == fgRole,
                     qPrintable(QStringLiteral(
                                    "'%1' grades the palette key '%2' but "
                                    "sends a click to the role '%3', which "
                                    "is the colour '%4'")
                                    .arg(label, fg, fgRole,
                                         store.paletteKeyForRole(fgRole))));
        }

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
            bool readThroughDerived = false;
            const QVariantMap derivedKeys = CustomThemeStore::derivedPaletteKeys();
            for (auto d = derivedKeys.constBegin(); d != derivedKeys.constEnd(); ++d) {
                if (d.value().toString() == it.key()
                    && CustomThemeStore::readabilityPaletteKeys().contains(d.key()))
                    readThroughDerived = true;
            }
            QVERIFY2(CustomThemeStore::readabilityPaletteKeys().contains(target)
                         || readThroughDerived,
                     qPrintable(QStringLiteral(
                                    "'%1' is not a key the readability table "
                                    "reads, so nothing asserts it against "
                                    "paletteForTheme()").arg(target)));
            QCOMPARE(store.paletteKeyForRole(it.key()), target);
        }
        for (const QString &role : roles) {
            if (aliases.contains(role))
                continue;
            QCOMPARE(store.paletteKeyForRole(role), role);
        }
        QCOMPARE(store.paletteKeyForRole(QStringLiteral("nope")),
                 QStringLiteral("nope"));

        const QString appThemePath = QStringLiteral(APPTHEME_QML_PATH);
        QFile dialog(appThemePath.left(appThemePath.lastIndexOf(QLatin1Char('/')) + 1)
                     + QStringLiteral("ThemeEditorDialog.qml"));
        QVERIFY2(dialog.open(QIODevice::ReadOnly | QIODevice::Text),
                 "ThemeEditorDialog.qml not readable");
        const QString source = QString::fromUtf8(dialog.readAll());
        QVERIFY2(source.contains(QStringLiteral("store.roleAliases()")),
                 "the editor no longer reads the alias map from the store");
        static const QRegularExpression literalAliases(
            QStringLiteral("storeKeyAliases\\s*:\\s*\\(?\\s*\\{"));
        QVERIFY2(!literalAliases.match(source).hasMatch(),
                 "ThemeEditorDialog.qml is keeping its own copy of the alias "
                 "map again");
    }

    void theHeaderButtonClusterScrollsInsteadOfClipping()
    {
        const QString appThemePath = QStringLiteral(APPTHEME_QML_PATH);
        QFile dialog(appThemePath.left(appThemePath.lastIndexOf(QLatin1Char('/')) + 1)
                     + QStringLiteral("ThemeEditorDialog.qml"));
        QVERIFY(dialog.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString src = QString::fromUtf8(dialog.readAll());
        const int fl = src.indexOf(QStringLiteral("objectName: \"themeHeaderActions\""));
        QVERIFY2(fl > 0, "the header action cluster is not in a Flickable");
        const int row = src.indexOf(QStringLiteral("id: headerActionRow"), fl);
        const int done = src.indexOf(QStringLiteral("objectName: \"themeEditorDoneButton\""), fl);
        const int apply = src.indexOf(QStringLiteral("objectName: \"themeApplyButton\""), fl);
        QVERIFY(row > fl);
        QVERIFY2(apply > row && done > apply,
                 "Use this theme / Done are not inside the scrolling cluster");
        QVERIFY(src.mid(fl - 120, 120).contains(QStringLiteral("Flickable {")));
        QVERIFY(src.mid(fl, 900).contains(QStringLiteral("HorizontalFlick")));
        QVERIFY2(src.mid(fl, 2500).contains(QStringLiteral("onActiveFocusItemChanged")),
                 "a focused clipped header button is not scrolled into view");
    }

    void aCleanThemeEditorClosesOnEscapeWhateverHasFocus()
    {
        const QString appThemePath = QStringLiteral(APPTHEME_QML_PATH);
        QFile dialog(appThemePath.left(appThemePath.lastIndexOf(QLatin1Char('/')) + 1)
                     + QStringLiteral("ThemeEditorDialog.qml"));
        QVERIFY(dialog.open(QIODevice::ReadOnly | QIODevice::Text));
        QString source = QString::fromUtf8(dialog.readAll());
        source.replace(QRegularExpression(QStringLiteral("\\s+")),
                       QStringLiteral(" "));
        QVERIFY2(source.contains(QStringLiteral("closePolicy: Popup.NoAutoClose ")),
                 "the last Escape layer is left to the focus-dependent close "
                 "policy again");
        QVERIFY2(source.contains(QStringLiteral(
                     "sequence: \"Escape\" enabled: root.opened "
                     "onActivated: root.peelEscape()")),
                 "Escape is not one always-live shortcut");
        const int peel = source.indexOf(QStringLiteral("function peelEscape()"));
        QVERIFY(peel > 0);
        const QString body = source.mid(peel, 400);
        QVERIFY2(body.contains(QStringLiteral("root.dismissTransient()"))
                     && body.contains(QStringLiteral("root.close()")),
                 "peelEscape() does not end with closing the editor");
        QVERIFY2(body.indexOf(QStringLiteral("root.dismissTransient()"))
                     < body.indexOf(QStringLiteral("root.close()")),
                 "a transient layer must close before the editor does");
    }

    void everyEditableRoleIsARealPaletteKey()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");
        QSet<QString> keys = paletteKeys(qml);
        QVERIFY2(keys.size() > 15, "palette-key scan found almost nothing");
        const QSet<QString> resolved = resolverKeys(qml);
        QVERIFY2(resolved.size() > 30,
                 "the paletteForTheme key scan found almost nothing");
        keys.unite(resolved);

        for (const QString &role : CustomThemeStore::editableRoles()) {
            QVERIFY2(keys.contains(role),
                     qPrintable(QStringLiteral("the editor offers '%1', which "
                                               "is not a key any AppTheme "
                                               "palette defines").arg(role)));
        }
    }

    void everyEditableRoleIsReadByAppThemeAndResolvedForTheEditor()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");
        const QSet<QString> resolved = resolverKeys(qml);
        const QStringList roles = CustomThemeStore::editableRoles();
        const QVariantMap aliases = CustomThemeStore::paletteKeyAliases();
        int checked = 0;
        for (const QString &role : roles) {
            const QString key = aliases.value(role, role).toString();
            QVERIFY2(resolved.contains(key),
                     qPrintable(QStringLiteral("paletteForTheme() does not "
                                               "return '%1', so the editor "
                                               "cannot show the colour of "
                                               "'%2' before it is changed")
                                    .arg(key, role)));
            const QRegularExpression read(
                QStringLiteral("\\b_p\\.%1\\b").arg(role));
            QVERIFY2(qml.contains(read),
                     qPrintable(QStringLiteral("nothing in AppTheme.qml reads "
                                               "_p.%1, so editing '%1' would "
                                               "paint nothing").arg(role)));
            ++checked;
        }
        QCOMPARE(checked, roles.size());
    }

    void aSplitRoleFallsBackToTheTokenItWasSplitFrom()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");
        QString flat = qml;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));

        const QList<QPair<QString, QString>> splits = {
            { QStringLiteral("popoverSurface"), QStringLiteral("stormPanel") },
            { QStringLiteral("focusRing"), QStringLiteral("_p.accent") },
            { QStringLiteral("scrollbarHandle"), QStringLiteral("borderStrong") },
            { QStringLiteral("reactionSelectedBackground"),
              QStringLiteral("accentSoft") },
            { QStringLiteral("ownBubbleText"),
              QStringLiteral("_ownBubbleTextDefault") },
            { QStringLiteral("presenceOnline"), QStringLiteral("success") },
            { QStringLiteral("unreadBadge"), QStringLiteral("accent") },
            { QStringLiteral("dangerTint"), QStringLiteral("mentionBadge") },
            { QStringLiteral("messageHighlight"), QStringLiteral("selected") },
            { QStringLiteral("textSelection"), QStringLiteral("selectedHover") },
            { QStringLiteral("paletteHighlight"), QStringLiteral("selected") },
            { QStringLiteral("roomSelected"), QStringLiteral("selected") },
            { QStringLiteral("roomHover"), QStringLiteral("hover") },
            { QStringLiteral("channelSelected"), QStringLiteral("selected") },
            { QStringLiteral("channelHover"), QStringLiteral("hover") },
            { QStringLiteral("menuHighlight"), QStringLiteral("stormSelection") },
            { QStringLiteral("buttonGhostHover"), QStringLiteral("hover") },
            { QStringLiteral("timestampInk"), QStringLiteral("textMuted") },
            { QStringLiteral("placeholderInk"), QStringLiteral("stormTextMuted") },
            { QStringLiteral("settingsPage"), QStringLiteral("stormDeep") },
            { QStringLiteral("settingsNav"), QStringLiteral("stormCanvas") },
        };
        const QStringList roles = CustomThemeStore::editableRoles();
        for (const auto &[role, fallback] : splits) {
            QVERIFY2(roles.contains(role),
                     qPrintable(role + QStringLiteral(" is not editable")));
            const QString declaration =
                QStringLiteral("readonly property color ") + role
                + QStringLiteral(":");
            const int at = flat.indexOf(declaration);
            QVERIFY2(at >= 0, qPrintable(role + QStringLiteral(" is not declared")));
            const QString body = flat.mid(at, 260);
            QVERIFY2(body.contains(QStringLiteral("_p.") + role
                                   + QStringLiteral(" !== undefined")),
                     qPrintable(role + QStringLiteral(" ignores a palette that "
                                                      "sets it")));
            QVERIFY2(body.contains(fallback),
                     qPrintable(QStringLiteral("%1 no longer falls back to %2, "
                                               "so existing themes would "
                                               "change colour")
                                    .arg(role, fallback)));
        }
        QCOMPARE(colorLiteral(qml, QStringLiteral("_ownBubbleTextDefault")),
                 QStringLiteral("#FFFFFF"));
    }

    void anOldSharedFileLoadsAndANewKeyRoundTrips()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("old"));
        store.setColor(QStringLiteral("surface"), QStringLiteral("#102030"));
        const QString oldFile = store.exportTheme(store.activeThemeId());
        QVERIFY(!oldFile.isEmpty());
        QVERIFY2(!oldFile.contains(QStringLiteral("popoverSurface")),
                 "an untouched split role must not be written into the file");

        QCOMPARE(store.importTheme(oldFile), QString());
        QCOMPARE(store.colors().value(QStringLiteral("surface")).toString(),
                 QStringLiteral("#102030"));
        QVERIFY(!store.colors().contains(QStringLiteral("popoverSurface")));

        store.setColor(QStringLiteral("popoverSurface"), QStringLiteral("#AA5500"));
        store.setColor(QStringLiteral("ownBubbleText"), QStringLiteral("#101010"));
        const QString newFile = store.exportTheme(store.activeThemeId());
        QCOMPARE(store.importTheme(newFile), QString());
        QCOMPARE(store.colors().value(QStringLiteral("popoverSurface")).toString(),
                 QStringLiteral("#AA5500"));
        QCOMPARE(store.colors().value(QStringLiteral("ownBubbleText")).toString(),
                 QStringLiteral("#101010"));
        QCOMPARE(store.colors().value(QStringLiteral("surface")).toString(),
                 QStringLiteral("#102030"));
    }

    void elementRolesAreSparseAndSurviveTheShareRoundTrip()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("elements"));
        QVERIFY(store.isEditableRole(QStringLiteral("roomHeader")));
        QCOMPARE(CustomThemeStore::roleFollows(QStringLiteral("roomHeader")),
                 QStringLiteral("sidebar"));
        QCOMPARE(CustomThemeStore::roleFollows(QStringLiteral("sidePanel")),
                 QStringLiteral("sidebar"));
        QVERIFY(store.setColor(QStringLiteral("roomHeader"), QStringLiteral("#123456")));
        QVERIFY(store.setColor(QStringLiteral("menuItemText"), QStringLiteral("#ABCDEF")));
        QVERIFY(store.isGradientRole(QStringLiteral("timelineBackground")));
        QVERIFY(store.setGradient(QStringLiteral("timelineBackground"),
            { { QStringLiteral("type"), QStringLiteral("linear") },
              { QStringLiteral("angle"), 90 },
              { QStringLiteral("stops"),
                QStringList{ QStringLiteral("#101010"), QStringLiteral("#202020") } } }));
        QVERIFY(!store.setGradient(QStringLiteral("roomHeader"),
            { { QStringLiteral("stops"),
                QStringList{ QStringLiteral("#101010"), QStringLiteral("#202020") } } }));

        QCOMPARE(store.colors().size(), 2);
        QVERIFY(!store.colors().contains(QStringLiteral("sidebar")));
        QVERIFY(!store.colors().contains(QStringLiteral("sidePanel")));

        const QString file = store.exportTheme(store.activeThemeId());
        QVERIFY(file.contains(QStringLiteral("\"roomHeader\":\"#123456\"")));
        QVERIFY(!file.contains(QStringLiteral("sidePanel")));
        QCOMPARE(store.importTheme(file), QString());
        QCOMPARE(store.colors().size(), 2);
        QCOMPARE(store.colors().value(QStringLiteral("roomHeader")).toString(),
                 QStringLiteral("#123456"));
        QCOMPARE(store.colors().value(QStringLiteral("menuItemText")).toString(),
                 QStringLiteral("#ABCDEF"));
        QCOMPARE(store.gradients().value(QStringLiteral("timelineBackground"))
                     .toMap().value(QStringLiteral("angle")).toInt(), 90);

        QString newer = file;
        newer.replace(QStringLiteral("\"roomHeader\""),
                      QStringLiteral("\"someFutureElement\":\"#FF0000\",\"roomHeader\""));
        QVERIFY(newer.contains(QStringLiteral("someFutureElement")));
        QCOMPARE(store.importTheme(newer), QString());
        QVERIFY(!store.colors().contains(QStringLiteral("someFutureElement")));
        QCOMPARE(store.colors().value(QStringLiteral("roomHeader")).toString(),
                 QStringLiteral("#123456"));
    }

    void applyToGroupRecoloursTheGroupAndFreesItsElements()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("apply"));
        const QStringList members = store.groupMembers(QStringLiteral("sidebar"));
        QVERIFY2(members.contains(QStringLiteral("roomHeader"))
                     && members.contains(QStringLiteral("sidePanel"))
                     && members.contains(QStringLiteral("roomList")),
                 qPrintable(members.join(QStringLiteral(", "))));
        store.setColor(QStringLiteral("roomHeader"), QStringLiteral("#111111"));
        store.setColor(QStringLiteral("sidePanel"), QStringLiteral("#222222"));
        store.setColor(QStringLiteral("menuItemText"), QStringLiteral("#333333"));
        store.sealUndoStep();
        const QVariantMap before = store.colors();

        QVERIFY(!store.applyToGroup(QStringLiteral("textPrimary"), QStringLiteral("#444444")));
        QVERIFY(!store.applyToGroup(QStringLiteral("roomHeader"), QStringLiteral("red")));
        QCOMPARE(store.colors(), before);

        QVERIFY(store.applyToGroup(QStringLiteral("roomHeader"), QStringLiteral("#abcdef")));
        QCOMPARE(store.colors().value(QStringLiteral("sidebar")).toString(),
                 QStringLiteral("#ABCDEF"));
        QVERIFY(!store.colors().contains(QStringLiteral("roomHeader")));
        QVERIFY(!store.colors().contains(QStringLiteral("sidePanel")));
        QCOMPARE(store.colors().value(QStringLiteral("menuItemText")).toString(),
                 QStringLiteral("#333333"));

        QVERIFY(store.undo());
        QCOMPARE(store.colors(), before);
    }

    void everyEditableRoleIsReadByAView()
    {
        const QString themePath = QStringLiteral(APPTHEME_QML_PATH);
        const QString dir = themePath.left(themePath.lastIndexOf(QLatin1Char('/')) + 1);
        QString all;
        int files = 0;
        QDirIterator it(dir, { QStringLiteral("*.qml") }, QDir::Files);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString name = QFileInfo(path).fileName();
            if (name == QLatin1String("AppTheme.qml")
                || name.startsWith(QLatin1String("ThemeEditor"))
                || name.startsWith(QLatin1String("ThemePreview")))
                continue;
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                all += QString::fromUtf8(f.readAll());
                ++files;
            }
        }
        QVERIFY2(files > 100, "the view scan found almost no QML files");
        const QVariantMap aliases = CustomThemeStore::paletteKeyAliases();
        QFile themeFile(themePath);
        QVERIFY(themeFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString theme = QString::fromUtf8(themeFile.readAll());
        const QHash<QString, QStringList> derivedFrom = {
            { QStringLiteral("roomUnreadBadge"),
              { QStringLiteral("unreadCountFill"), QStringLiteral("unreadDot") } },
            { QStringLiteral("unreadBadgeText"), { QStringLiteral("unreadCountInk") } },
            { QStringLiteral("mentionBadgeText"), { QStringLiteral("mentionBadgeInk") } },
        };
        const auto derives = [&theme](const QString &token, const QString &role) {
            const qsizetype at = theme.indexOf(
                QStringLiteral("readonly property color %1:").arg(token));
            return at >= 0 && theme.mid(at, 400).contains(QRegularExpression(
                       QStringLiteral("\\b%1\\b").arg(role)));
        };
        const auto read = [&](const QString &role) {
            const QString key = aliases.value(role, role).toString();
            if (all.contains(QRegularExpression(
                    QStringLiteral("AppTheme\\.%1\\b").arg(key))))
                return true;
            for (const QString &token : derivedFrom.value(key)) {
                if (derives(token, key)
                    && all.contains(QRegularExpression(
                        QStringLiteral("AppTheme\\.%1\\b").arg(token))))
                    return true;
            }
            return false;
        };
        QHash<QString, QStringList> children;
        for (const QString &role : CustomThemeStore::editableRoles()) {
            const QString parent = CustomThemeStore::roleFollows(role);
            if (!parent.isEmpty())
                children[parent] << role;
        }
        std::function<bool(const QString &, int)> readThrough =
            [&](const QString &role, int depth) {
            if (read(role))
                return true;
            for (const QString &child : children.value(role)) {
                if (depth < 6 && readThrough(child, depth + 1))
                    return true;
            }
            return false;
        };
        QStringList unread;
        int direct = 0;
        for (const QString &role : CustomThemeStore::editableRoles()) {
            if (read(role))
                ++direct;
            else if (!readThrough(role, 0))
                unread << role;
        }
        QVERIFY2(direct > 100,
                 qPrintable(QStringLiteral("only %1 roles are read by a view")
                                .arg(direct)));
        QVERIFY2(unread.isEmpty(),
                 qPrintable(QStringLiteral("no view reads the live token for: "
                                           "%1").arg(unread.join(QStringLiteral(", ")))));
    }

    void everyEditableRoleHasARegionInThePreview()
    {
        const QString themePath = QStringLiteral(APPTHEME_QML_PATH);
        QFile preview(themePath.left(themePath.lastIndexOf(QLatin1Char('/')) + 1)
                      + QStringLiteral("ThemePreviewDemo.qml"));
        QVERIFY2(preview.open(QIODevice::ReadOnly | QIODevice::Text),
                 "ThemePreviewDemo.qml not readable");
        QString src = QString::fromUtf8(preview.readAll());
        src.remove(QRegularExpression(QStringLiteral("//[^\n]*")));

        const auto named = [&src](const QString &role) {
            const QRegularExpression re(
                QStringLiteral("\\b(?:role|inkRole):[^}]{0,500}?\"%1\"").arg(role),
                QRegularExpression::DotMatchesEverythingOption);
            return src.contains(re);
        };
        QHash<QString, QStringList> children;
        for (const QString &role : CustomThemeStore::editableRoles()) {
            const QString parent = CustomThemeStore::roleFollows(role);
            if (!parent.isEmpty())
                children[parent] << role;
        }
        std::function<bool(const QString &, int)> paintedThrough =
            [&](const QString &role, int depth) {
            if (named(role))
                return true;
            if (depth > 6)
                return false;
            for (const QString &child : children.value(role)) {
                if (paintedThrough(child, depth + 1))
                    return true;
            }
            return false;
        };
        QStringList missing;
        int checked = 0;
        int direct = 0;
        for (const QString &role : CustomThemeStore::editableRoles()) {
            if (named(role))
                ++direct;
            else if (!paintedThrough(role, 0))
                missing << role;
            ++checked;
        }
        QVERIFY2(checked > 100, "the role table shrank");
        QVERIFY2(direct > 90,
                 qPrintable(QStringLiteral("only %1 roles have a Region")
                                .arg(direct)));
        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral("these roles can be edited but the "
                                           "preview paints no region for "
                                           "them: %1")
                                .arg(missing.join(QStringLiteral(", ")))));
    }

    void everyFollowingRoleFallsBackToTheRoleItNames()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");
        QString flat = qml;
        flat.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));

        const int fn = flat.indexOf(QStringLiteral("function paletteForTheme("));
        const int ret = flat.indexOf(QStringLiteral("var out = {"), fn);
        QVERIFY2(fn >= 0 && ret > fn, "paletteForTheme() not found");
        const QString resolver = flat.mid(ret);

        QHash<QString, QStringList> tokens{
            {QStringLiteral("sidebar"), {QStringLiteral("sidebar")}},
            {QStringLiteral("surface"),
             {QStringLiteral("surface"), QStringLiteral("stormPanel")}},
            {QStringLiteral("background"),
             {QStringLiteral("background"), QStringLiteral("stormDeep"),
              QStringLiteral("stormCanvas")}},
            {QStringLiteral("selected"), {QStringLiteral("selected")}},
            {QStringLiteral("hover"),
             {QStringLiteral("hover"), QStringLiteral("stormSelection")}},
            {QStringLiteral("selectedHover"), {QStringLiteral("selectedHover")}},
            {QStringLiteral("accentSoft"), {QStringLiteral("accentSoft")}},
            {QStringLiteral("accent"), {QStringLiteral("accent")}},
            {QStringLiteral("borderStrong"), {QStringLiteral("borderStrong")}},
            {QStringLiteral("textMuted"),
             {QStringLiteral("textMuted"), QStringLiteral("stormTextMuted")}},
            {QStringLiteral("success"), {QStringLiteral("success")}},
            {QStringLiteral("mention"),
             {QStringLiteral("mentionBadge"), QStringLiteral("mentionBadgeFill")}},
            {QStringLiteral("border"),
             {QStringLiteral("border"), QStringLiteral("stormBorder")}},
            {QStringLiteral("cardElevated"), {QStringLiteral("cardElevated")}},
            {QStringLiteral("unreadBadge"), {QStringLiteral("unreadBadge")}},
            {QStringLiteral("accentText"), {QStringLiteral("accentText")}},
            {QStringLiteral("textPrimary"),
             {QStringLiteral("textPrimary"), QStringLiteral("stormText")}},
            {QStringLiteral("textSecondary"),
             {QStringLiteral("textSecondary"),
              QStringLiteral("stormTextSecondary")}},
            {QStringLiteral("selectedText"), {QStringLiteral("selectedText")}},
            {QStringLiteral("textDisabled"), {QStringLiteral("textDisabled")}},
            {QStringLiteral("presenceOnline"), {QStringLiteral("presenceOnline")}},
            {QStringLiteral("warning"), {QStringLiteral("warning")}},
            {QStringLiteral("dangerTint"), {QStringLiteral("dangerTint")}},
            {QStringLiteral("danger"),
             {QStringLiteral("danger"), QStringLiteral("stormDanger")}},
        };
        tokens[QStringLiteral("borderStrong")] << QStringLiteral("stormBorderStrong");
        tokens[QStringLiteral("textMuted")] << QStringLiteral("sectionLabelColor");
        const QHash<QString, QString> elementTable = elementGroupsTable(qml);
        QVERIFY2(elementTable.size() > 50, "the elementGroups scan found almost nothing");
        const QVariantMap aliases = CustomThemeStore::paletteKeyAliases();
        const auto namesParent = [&](const QString &fallback,
                                     const QString &parent) {
            for (const QString &t : tokens.value(parent)) {
                if (fallback.contains(QRegularExpression(
                        QStringLiteral("\\b%1\\b").arg(t))))
                    return true;
            }
            return false;
        };

        SettingsManager settings;
        CustomThemeStore store(&settings);
        const QStringList editable = CustomThemeStore::editableRoles();
        QStringList wrong;
        int following = 0;
        QHash<QString, QString> follows;
        for (const QVariant &v : store.roles()) {
            const QVariantMap entry = v.toMap();
            const QString role = entry.value(QStringLiteral("key")).toString();
            const QString parent = entry.value(QStringLiteral("follows")).toString();
            if (parent.isEmpty())
                continue;
            follows.insert(role, parent);
            ++following;
            if (!editable.contains(parent) || parent == role) {
                wrong << role + QStringLiteral(" -> ") + parent
                             + QStringLiteral(" (not an editable role)");
                continue;
            }
            if (!tokens.contains(parent)) {
                wrong << role + QStringLiteral(" -> ") + parent
                             + QStringLiteral(" (no token list in this test)");
                continue;
            }

            const QString decl = QStringLiteral("readonly property color ")
                                 + role + QStringLiteral(":");
            const int at = flat.indexOf(decl);
            const int next = flat.indexOf(QStringLiteral("readonly property"),
                                          at + decl.size());
            const QString live = at < 0 ? QString()
                                        : flat.mid(at, next < 0 ? -1 : next - at);
            const QString liveHead = QStringLiteral("_p.") + role
                                     + QStringLiteral(" !== undefined ? _p.")
                                     + role;
            const int lh = live.indexOf(liveHead);
            if (lh < 0 || !namesParent(live.mid(lh + liveHead.size()), parent))
                wrong << role + QStringLiteral(" -> ") + parent
                             + QStringLiteral(" (live property: ")
                             + live.left(160) + QStringLiteral(")");

            if (elementTable.contains(role)) {
                const QString want = aliases.value(parent, parent).toString();
                if (elementTable.value(role) != want)
                    wrong << role + QStringLiteral(" -> ") + parent
                                 + QStringLiteral(" (elementGroups names ")
                                 + elementTable.value(role) + QStringLiteral(")");
                continue;
            }
            const QRegularExpression key(
                QStringLiteral("[{,] %1: (p\\.%1 !== undefined \\? p\\.%1)(.*?)"
                               "(?=, \\w+:|\\} )").arg(role));
            const auto m = key.match(resolver);
            if (!m.hasMatch() || !namesParent(m.captured(2), parent))
                wrong << role + QStringLiteral(" -> ") + parent
                             + QStringLiteral(" (paletteForTheme)");
        }
        for (auto e = elementTable.constBegin(); e != elementTable.constEnd(); ++e) {
            const QString parent = CustomThemeStore::roleFollows(e.key());
            if (aliases.value(parent, parent).toString() != e.value())
                wrong << e.key() + QStringLiteral(" (elementGroups says ")
                             + e.value() + QStringLiteral(", the store says ")
                             + parent + QStringLiteral(")");
        }
        QVERIFY2(following >= 80,
                 qPrintable(QStringLiteral("only %1 roles declare what they "
                                           "follow").arg(following)));
        QVERIFY2(wrong.isEmpty(),
                 qPrintable(QStringLiteral("a role's `follows` is not the role "
                                           "AppTheme falls back to:\n  %1")
                                .arg(wrong.join(QStringLiteral("\n  ")))));

        for (const QString &role : editable) {
            QString p = follows.value(role);
            int hops = 0;
            while (!p.isEmpty() && hops < 10) {
                QVERIFY2(p != role,
                         qPrintable(role + QStringLiteral(" follows itself")));
                p = follows.value(p);
                ++hops;
            }
            QVERIFY2(hops < 10, qPrintable(role + QStringLiteral(" has a cycle")));
        }
    }

    void undoMergesADragAndStepsBackThroughEveryKindOfEdit()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("Undo"));
        const QString accent = QStringLiteral("accent");
        const auto canUndo = [&] { return store.property("canUndo").toBool(); };
        const auto canRedo = [&] { return store.property("canRedo").toBool(); };
        const auto call = [&](const char *method) {
            bool result = false;
            const bool ok = QMetaObject::invokeMethod(
                &store, method, Q_RETURN_ARG(bool, result));
            return ok && result;
        };
        const auto seal = [&] {
            QVERIFY2(QMetaObject::invokeMethod(&store, "sealUndoStep"),
                     "the store has no sealUndoStep()");
        };
        const auto value = [&](const QString &role) {
            return store.colors().value(role).toString();
        };
        QVERIFY(!canUndo());
        QSignalSpy history(&store, SIGNAL(historyChanged()));
        QVERIFY2(history.isValid(), "the store has no historyChanged()");

        store.setColor(accent, QStringLiteral("#111111"));
        store.setColor(accent, QStringLiteral("#222222"));
        store.setColor(accent, QStringLiteral("#333333"));
        QVERIFY(canUndo());
        QVERIFY(call("undo"));
        QVERIFY2(!store.colors().contains(accent),
                 "undo stepped back one sample of a drag, not the drag");
        QVERIFY(!canUndo());
        QVERIFY(canRedo());
        QVERIFY(call("redo"));
        QCOMPARE(value(accent), QStringLiteral("#333333"));

        seal();
        store.setColor(accent, QStringLiteral("#444444"));
        seal();
        store.setColor(accent, QStringLiteral("#555555"));
        QVERIFY(call("undo"));
        QCOMPARE(value(accent), QStringLiteral("#444444"));
        QVERIFY(call("undo"));
        QCOMPARE(value(accent), QStringLiteral("#333333"));

        QVERIFY(canRedo());
        store.setColor(QStringLiteral("background"), QStringLiteral("#0A0A0A"));
        QVERIFY(!canRedo());

        QVERIFY(call("undo"));
        QVERIFY(!store.colors().contains(QStringLiteral("background")));
        QCOMPARE(value(accent), QStringLiteral("#333333"));

        store.resetColor(accent);
        QVERIFY(!store.colors().contains(accent));
        QVERIFY(call("undo"));
        QCOMPARE(value(accent), QStringLiteral("#333333"));

        store.setColor(QStringLiteral("surface"), QStringLiteral("#123456"));
        store.resetAll();
        QCOMPARE(store.overrideCount(), 0);
        QVERIFY(call("undo"));
        QCOMPARE(value(accent), QStringLiteral("#333333"));
        QCOMPARE(value(QStringLiteral("surface")), QStringLiteral("#123456"));

        const int baseBefore = store.baseTheme();
        const int other = baseBefore == int(SettingsManager::GraphiteTheme)
                              ? int(SettingsManager::NordTheme)
                              : int(SettingsManager::GraphiteTheme);
        store.setBaseTheme(other);
        QVERIFY(call("undo"));
        QCOMPARE(store.baseTheme(), baseBefore);
        QVERIFY(call("redo"));
        QCOMPARE(store.baseTheme(), other);
        QVERIFY(history.count() > 0);

        QVERIFY(canUndo());
        const QString first = store.activeThemeId();
        const QString second = store.createTheme(QStringLiteral("Other"));
        QVERIFY(!second.isEmpty());
        QVERIFY(!canUndo());
        QVERIFY(!canRedo());
        QVERIFY(!call("undo"));
        store.setColor(accent, QStringLiteral("#ABCDEF"));
        QVERIFY(canUndo());
        store.setActiveThemeId(first);
        QVERIFY(!canUndo());
        QCOMPARE(value(accent), QStringLiteral("#333333"));
    }

    void undoStepsAreNamedAndGradientsAreInTheHistory()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("Named"));
        const auto undoLabel = [&] { return store.property("undoLabel").toString(); };
        const auto redoLabel = [&] { return store.property("redoLabel").toString(); };
        QVERIFY2(store.metaObject()->indexOfProperty("undoLabel") >= 0,
                 "the store has no undoLabel");
        QCOMPARE(undoLabel(), QString());

        store.setColor(QStringLiteral("roomHeader"), QStringLiteral("#111111"));
        QVERIFY2(undoLabel().contains(CustomThemeStore::roleLabel(
                                          QStringLiteral("roomHeader"))),
                 qPrintable(undoLabel()));
        store.setColor(QStringLiteral("roomHeader"), QStringLiteral("#222222"));
        const QString dragLabel = undoLabel();
        store.sealUndoStep();

        const QVariantMap spec{ { QStringLiteral("type"), QStringLiteral("linear") },
                                { QStringLiteral("angle"), 180 },
                                { QStringLiteral("stops"),
                                  QStringList{ QStringLiteral("#102030"),
                                               QStringLiteral("#203040") } } };
        QVERIFY(store.setGradient(QStringLiteral("background"), spec));
        QVERIFY(undoLabel().contains(CustomThemeStore::roleLabel(
                                         QStringLiteral("background"))));
        QVERIFY(undoLabel() != dragLabel);
        const QString gradientLabel = undoLabel();
        QVERIFY(store.undo());
        QVERIFY2(!store.gradients().contains(QStringLiteral("background")),
                 "undo left the gradient in place");
        QCOMPARE(redoLabel(), gradientLabel);
        QCOMPARE(undoLabel(), dragLabel);
        QVERIFY(store.redo());
        QVERIFY(store.gradients().contains(QStringLiteral("background")));
        QVERIFY(undoLabel() == gradientLabel);

        QVariantMap moved = spec;
        moved.insert(QStringLiteral("stops"),
                     QStringList{ QStringLiteral("#112233"), QStringLiteral("#203040") });
        store.sealUndoStep();
        QVERIFY(store.setGradient(QStringLiteral("background"), moved));
        moved.insert(QStringLiteral("stops"),
                     QStringList{ QStringLiteral("#445566"), QStringLiteral("#203040") });
        QVERIFY(store.setGradient(QStringLiteral("background"), moved));
        QVERIFY(store.undo());
        QCOMPARE(store.gradients().value(QStringLiteral("background")).toMap()
                     .value(QStringLiteral("stops")).toStringList().value(0),
                 QStringLiteral("#102030"));

        store.resetGradient(QStringLiteral("background"));
        QVERIFY(!store.gradients().contains(QStringLiteral("background")));
        QVERIFY(store.undo());
        QVERIFY(store.gradients().contains(QStringLiteral("background")));

        store.sealUndoStep();
        QVERIFY(QMetaObject::invokeMethod(&store, "nameNextStep",
                                          Q_ARG(QString, QStringLiteral("Make it pop"))));
        store.setColor(QStringLiteral("accent"), QStringLiteral("#ABCDEF"));
        QCOMPARE(undoLabel(), QStringLiteral("Make it pop"));
        store.sealUndoStep();
        store.setColor(QStringLiteral("link"), QStringLiteral("#ABCDEF"));
        QVERIFY(undoLabel() != QStringLiteral("Make it pop"));

        store.sealUndoStep();
        QVERIFY(store.setColor(QStringLiteral("sidebar"), QStringLiteral("#010101")));
        store.sealUndoStep();
        const QVariantMap colorsBefore = store.colors();
        const QVariantMap gradientsBefore = store.gradients();
        QVERIFY(QMetaObject::invokeMethod(&store, "beginUndoGroup",
                                          Q_ARG(QString, QStringLiteral("Unlink Room list"))));
        QVERIFY(store.setGradient(QStringLiteral("roomList"), spec));
        store.setColor(QStringLiteral("roomList"), QStringLiteral("#050505"));
        store.resetColor(QStringLiteral("sidebar"));
        QVERIFY(QMetaObject::invokeMethod(&store, "endUndoGroup"));
        QCOMPARE(undoLabel(), QStringLiteral("Unlink Room list"));
        QVERIFY(store.undo());
        QCOMPARE(store.colors(), colorsBefore);
        QCOMPARE(store.gradients(), gradientsBefore);
        QVERIFY(store.redo());
        QVERIFY(store.gradients().contains(QStringLiteral("roomList")));

        store.resetAll();
        QCOMPARE(store.gradients().size(), 0);
        QVERIFY(store.undo());
        QVERIFY(store.gradients().contains(QStringLiteral("roomList")));
        QVERIFY(store.gradients().contains(QStringLiteral("background")));
    }

    void importPreviewChangesNothingAndReplaceKeepsTheNameAndIsUndoable()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("Source"));
        QVERIFY(store.setColor(QStringLiteral("sidebar"), QStringLiteral("#123456")));
        QVERIFY(store.setColor(QStringLiteral("accent"), QStringLiteral("#FF8800")));
        QVERIFY(store.setGradient(
            QStringLiteral("background"),
            { { QStringLiteral("type"), QStringLiteral("linear") },
              { QStringLiteral("angle"), 180 },
              { QStringLiteral("stops"), QStringList{ QStringLiteral("#102030"),
                                                      QStringLiteral("#203040") } } }));
        const QString code = store.exportTheme(store.activeThemeId());
        QVERIFY(!code.isEmpty());

        const QString target = store.createTheme(QStringLiteral("Target"));
        QVERIFY(!target.isEmpty());
        const int themesBefore = store.themes().size();
        const QVariantMap colorsBefore = store.colors();

        QVariantMap pv;
        QVERIFY2(QMetaObject::invokeMethod(&store, "previewImport",
                                           Q_RETURN_ARG(QVariantMap, pv),
                                           Q_ARG(QString, code)),
                 "the store has no previewImport()");
        QCOMPARE(pv.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(pv.value(QStringLiteral("name")).toString(), QStringLiteral("Source"));
        QCOMPARE(pv.value(QStringLiteral("colourCount")).toInt(), 2);
        QCOMPARE(pv.value(QStringLiteral("gradientCount")).toInt(), 1);
        QCOMPARE(pv.value(QStringLiteral("colors")).toMap()
                     .value(QStringLiteral("sidebar")).toString(),
                 QStringLiteral("#123456"));
        QCOMPARE(pv.value(QStringLiteral("atLimit")).toBool(), false);
        QCOMPARE(store.themes().size(), themesBefore);
        QCOMPARE(store.colors(), colorsBefore);
        QCOMPARE(store.activeThemeId(), target);

        for (const QString &bad : { QStringLiteral("nonsense"), QStringLiteral("{}"),
                                    QStringLiteral("{\"lightning_theme\":1,\"colors\":{}}") }) {
            QVariantMap refused;
            QVERIFY(QMetaObject::invokeMethod(&store, "previewImport",
                                              Q_RETURN_ARG(QVariantMap, refused),
                                              Q_ARG(QString, bad)));
            QCOMPARE(refused.value(QStringLiteral("ok")).toBool(), false);
            QVERIFY(!refused.value(QStringLiteral("error")).toString().isEmpty());
        }

        QCOMPARE(store.name(), QStringLiteral("Target"));
        QString reason = QStringLiteral("unset");
        QVERIFY(QMetaObject::invokeMethod(&store, "replaceActiveTheme",
                                          Q_RETURN_ARG(QString, reason),
                                          Q_ARG(QString, code)));
        QVERIFY2(reason.isEmpty(), qPrintable(reason));
        QCOMPARE(store.themes().size(), themesBefore);
        QCOMPARE(store.activeThemeId(), target);
        QCOMPARE(store.name(), QStringLiteral("Target"));
        QCOMPARE(store.colors().value(QStringLiteral("sidebar")).toString(),
                 QStringLiteral("#123456"));
        QVERIFY(store.gradients().contains(QStringLiteral("background")));

        QVERIFY(store.canUndo());
        QVERIFY(store.property("undoLabel").toString().length() > 0);
        QVERIFY(store.undo());
        QCOMPARE(store.colors(), colorsBefore);
        QVERIFY(!store.gradients().contains(QStringLiteral("background")));
        QVERIFY(store.redo());
        QCOMPARE(store.colors().value(QStringLiteral("accent")).toString(),
                 QStringLiteral("#FF8800"));

        const QVariantMap colorsNow = store.colors();
        QVERIFY(QMetaObject::invokeMethod(&store, "replaceActiveTheme",
                                          Q_RETURN_ARG(QString, reason),
                                          Q_ARG(QString, QStringLiteral("nonsense"))));
        QVERIFY(!reason.isEmpty());
        QCOMPARE(store.colors(), colorsNow);

        while (store.themes().size() < CustomThemeStore::kMaxThemes)
            QVERIFY(!store.createTheme(QStringLiteral("Filler")).isEmpty());
        QVERIFY(QMetaObject::invokeMethod(&store, "previewImport",
                                          Q_RETURN_ARG(QVariantMap, pv),
                                          Q_ARG(QString, code)));
        QCOMPARE(pv.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(pv.value(QStringLiteral("atLimit")).toBool(), true);
    }

    void aDroppedThemeFileIsReadOnlyWhenItIsSmallAndLocal()
    {
        SettingsManager settings;
        CustomThemeStore store(&settings);
        store.createTheme(QStringLiteral("Drop"));
        QVERIFY(store.setColor(QStringLiteral("accent"), QStringLiteral("#FF8800")));
        const QString code = store.exportTheme(store.activeThemeId());

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto write = [&](const QString &name, const QByteArray &bytes) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly))
                return QUrl();
            f.write(bytes);
            return QUrl::fromLocalFile(f.fileName());
        };
        const auto read = [&](const QUrl &url) {
            QString text;
            const bool ok = QMetaObject::invokeMethod(
                &store, "readThemeFile", Q_RETURN_ARG(QString, text),
                Q_ARG(QUrl, url));
            return ok ? text : QStringLiteral("<no readThemeFile()>");
        };

        QCOMPARE(read(write(QStringLiteral("t.json"), code.toUtf8())), code);
        QCOMPARE(read(write(QStringLiteral("big.json"),
                            QByteArray(int(CustomThemeStore::kMaxShareFileBytes) + 1, 'x'))),
                 QString());
        QCOMPARE(read(write(QStringLiteral("empty.json"), QByteArray())), QString());
        QCOMPARE(read(QUrl(QStringLiteral("https://example.com/theme.json"))), QString());
        QCOMPARE(read(QUrl::fromLocalFile(dir.path())), QString());
        QCOMPARE(read(QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.json")))),
                 QString());
    }

    void theEditorCoversTheFourShellRegionsAndTheMessageSurfaces()
    {
        const QStringList roles = CustomThemeStore::editableRoles();
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
        QCOMPARE(QSet<QString>(roles.begin(), roles.end()).size(), roles.size());
    }

    void theNewNavigationTokensFollowAnEditableRole()
    {
        const QString qml = appTheme();
        QVERIFY2(!qml.isEmpty(), "AppTheme.qml not readable");
        QString flat = qml;
        flat.replace(QRegularExpression(QStringLiteral("\\s+")),
                     QStringLiteral(" "));
        const QStringList roles = CustomThemeStore::editableRoles();

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
        QVERIFY2(qml.contains(QStringLiteral("/^#[0-9A-Fa-f]{6}$/.test(v)")),
                 "the QML-side override guard is gone");
    }

    void theCustomThemeClassifiesItselfLightOrDark()
    {
        const QString qml = appTheme();
        QVERIFY2(qml.contains(QStringLiteral("effectiveTheme === 12")),
                 "`dark` no longer special-cases the custom palette");
        QVERIFY2(qml.contains(QStringLiteral("relativeLuminance(_asColor(_p.background))")),
                 "the custom palette must be classified by its own background");
    }

    void aSwitchDropsTheOutgoingAccountsThemesInsteadOfSavingThemOverTheNext()
    {
        SettingsManager settings;
        settings.saveSession(kHs, kBob, QStringLiteral("BDEV"), QString());
        settings.saveSession(kHs, kAlice, QStringLiteral("ADEV"), QString());

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

        settings.setActiveAccountUserId(kBob);

        QCOMPARE(themeNames(store), QStringList({ QStringLiteral("Bob only") }));

        store.setName(QStringLiteral("Bob renamed"));

        CustomThemeStore reread(&settings);
        QCOMPARE(themeNames(reread),
                 QStringList({ QStringLiteral("Bob renamed") }));

        settings.setActiveAccountUserId(kAlice);
        CustomThemeStore aliceAgain(&settings);
        QCOMPARE(themeNames(aliceAgain),
                 QStringList({ QStringLiteral("Alice one"),
                               QStringLiteral("Alice two") }));
    }

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

        settings.saveSession(kHs, kBob, QStringLiteral("BDEV"), QString());
        QCOMPARE(themeNames(store), QStringList({ QStringLiteral("Bob only") }));
    }

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
