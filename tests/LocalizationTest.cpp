// Localization contract.
//
// Three separate things are pinned here and they fail for different reasons:
//   * the pure locale-matching policy (no Qt objects, no files);
//   * the CATALOGS on disk — that every shipped language has one, that the
//     build lists it, and that no translator has broken a placeholder;
//   * that a compiled catalog actually loads and answers, including Qt's
//     plural forms, which are the reason an English catalog exists at all.

#include "app/SettingsManager.h"
#include "i18n/LocalizationManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTranslator>
#include <QXmlStreamReader>
#include <QtTest>

namespace {

QString repoFile(const QString &relative)
{
    return QStringLiteral(LIGHTNING_SOURCE_DIR "/") + relative;
}

QString readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

// %1..%9 and %n, as a set. Order is deliberately NOT compared: a translator
// may legitimately reorder them, which is half the point of numbered
// placeholders. Losing or inventing one is the defect.
QSet<QString> placeholders(const QString &s)
{
    QSet<QString> out;
    static const QRegularExpression re(QStringLiteral("%(\\d|n)"));
    auto it = re.globalMatch(s);
    while (it.hasNext())
        out.insert(it.next().captured(0));
    return out;
}

int tokenOccurrences(const QString &text, const QString &token)
{
    const QRegularExpression re(
        QStringLiteral("(?<![A-Za-z0-9_])%1(?![A-Za-z0-9_])")
            .arg(QRegularExpression::escape(token)));
    int count = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

QSet<QString> catalogKeys(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QSet<QString> out;
    QXmlStreamReader xml(&file);
    QString context;
    QString source;
    QString comment;
    bool inMessage = false;
    bool numerus = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QLatin1String("name") && !inMessage) {
                context = xml.readElementText();
            } else if (xml.name() == QLatin1String("message")) {
                inMessage = true;
                numerus = xml.attributes().value(QLatin1String("numerus"))
                              == QLatin1String("yes");
                source.clear();
                comment.clear();
            } else if (inMessage && xml.name() == QLatin1String("source")) {
                source = xml.readElementText();
            } else if (inMessage && xml.name() == QLatin1String("comment")) {
                comment = xml.readElementText();
            }
        } else if (xml.isEndElement()
                   && xml.name() == QLatin1String("message")) {
            const QChar separator(0x1f);
            out.insert(context + separator + source + separator + comment
                       + separator + (numerus ? QLatin1Char('1')
                                              : QLatin1Char('0')));
            inMessage = false;
        }
    }
    return xml.hasError() ? QSet<QString>() : out;
}

QString summarizeKeys(const QSet<QString> &keys)
{
    QStringList sorted = keys.values();
    sorted.sort();
    if (sorted.size() > 8)
        sorted = sorted.mid(0, 8) << QStringLiteral("…");
    for (QString &key : sorted)
        key.replace(QChar(0x1f), QStringLiteral(" | "));
    return sorted.join(QLatin1Char('\n'));
}

} // namespace

class LocalizationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("localization-test"));
    }

    // ---- policy ---------------------------------------------------------

    void regionalVariantsCollapseOntoTheirLanguage()
    {
        // The exact cases the requirement names, plus both spellings Qt uses
        // ('-' from uiLanguages(), '_' from QLocale::name()).
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("es_ES")),
                 QStringLiteral("es"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("es_MX")),
                 QStringLiteral("es"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("pt-BR")),
                 QStringLiteral("pt"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("pt_PT")),
                 QStringLiteral("pt"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("fr_CA")),
                 QStringLiteral("fr"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("fr_FR")),
                 QStringLiteral("fr"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("ru_RU")),
                 QStringLiteral("ru"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("ar_EG")),
                 QStringLiteral("ar"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("id_ID")),
                 QStringLiteral("id"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("hi_IN")),
                 QStringLiteral("hi"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("bn_BD")),
                 QStringLiteral("bn"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("lt_LT")),
                 QStringLiteral("lt"));
    }

    // Chinese is the one language where the SCRIPT decides. Simplified and
    // Traditional are different written forms, and Lightning ships only
    // Simplified — so a Traditional locale must reach the English fallback
    // rather than be quietly served a script it did not ask for.
    void chineseIsMatchedByScriptNotByRegion()
    {
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("zh_CN")),
                 QStringLiteral("zh_CN"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("zh_SG")),
                 QStringLiteral("zh_CN"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("zh-Hans-CN")),
                 QStringLiteral("zh_CN"));
        QCOMPARE(LocalizationManager::matchLanguageTag(QStringLiteral("zh")),
                 QStringLiteral("zh_CN"));

        QVERIFY(LocalizationManager::matchLanguageTag(QStringLiteral("zh_TW")).isEmpty());
        QVERIFY(LocalizationManager::matchLanguageTag(QStringLiteral("zh_HK")).isEmpty());
        QVERIFY(LocalizationManager::matchLanguageTag(QStringLiteral("zh-Hant-TW")).isEmpty());
    }

    void unsupportedLanguagesFallBackToEnglish()
    {
        for (const char *tag : { "de_DE", "ja", "sw", "", "!!", "xx_YY" })
            QVERIFY2(LocalizationManager::matchLanguageTag(QLatin1String(tag)).isEmpty(),
                     tag);
        QCOMPARE(LocalizationManager::matchPreferenceList(
                     { QStringLiteral("de_DE"), QStringLiteral("ja") }),
                 QStringLiteral("en"));
        QCOMPARE(LocalizationManager::matchPreferenceList({}),
                 QStringLiteral("en"));
    }

    // uiLanguages() is an ORDERED preference list, not one locale. A desktop
    // set to "Japanese, then French, then English" must reach French, not
    // English — taking only the first entry would drop the user's real
    // second choice on the floor.
    void thePreferenceListIsWalkedInOrder()
    {
        QCOMPARE(LocalizationManager::matchPreferenceList(
                     { QStringLiteral("ja_JP"), QStringLiteral("fr_CA"),
                       QStringLiteral("en_GB") }),
                 QStringLiteral("fr"));
        QCOMPARE(LocalizationManager::matchPreferenceList(
                     { QStringLiteral("pt_BR"), QStringLiteral("es_MX") }),
                 QStringLiteral("pt"));
    }

    void arabicIsTheOnlyRightToLeftLanguage()
    {
        const QStringList codes = LocalizationManager::supportedCodes();
        QVERIFY(codes.contains(QStringLiteral("ar")));
        for (const QString &code : codes) {
            QCOMPARE(LocalizationManager::isRightToLeft(code),
                     code == QStringLiteral("ar"));
        }
        // An unknown code must not claim a direction.
        QVERIFY(!LocalizationManager::isRightToLeft(QStringLiteral("he")));
    }

    void everySupportedCodeNamesItself()
    {
        for (const QString &code : LocalizationManager::supportedCodes()) {
            QVERIFY2(!LocalizationManager::endonym(code).isEmpty(),
                     qPrintable(code));
            QVERIFY2(!LocalizationManager::englishName(code).isEmpty(),
                     qPrintable(code));
        }
        QVERIFY(LocalizationManager::endonym(QStringLiteral("nope")).isEmpty());
    }

    // ---- persistence ----------------------------------------------------

    // "system" is a POLICY, not a language. Storing the RESOLVED code instead
    // would freeze a user's language the first time they opened Settings, and
    // moving the machine to another desktop locale would stop following.
    void theStoredPolicySurvivesAndIsValidated()
    {
        SettingsManager settings;
        LocalizationManager loc(&settings);

        // Nothing stored: follow the desktop.
        QCOMPARE(loc.language(), QStringLiteral("system"));

        loc.setLanguage(QStringLiteral("fr"));
        QCOMPARE(loc.language(), QStringLiteral("fr"));
        QCOMPARE(settings.language(), QStringLiteral("fr"));

        // A fresh manager over the same store reads it back.
        LocalizationManager reopened(&settings);
        QCOMPARE(reopened.language(), QStringLiteral("fr"));

        // A retired or corrupted value is not a language. It must fall back
        // to the POLICY, so the desktop still decides — falling back to "en"
        // would silently pin a user to English forever.
        settings.setLanguage(QStringLiteral("tlh"));
        LocalizationManager rubbish(&settings);
        QCOMPARE(rubbish.language(), QStringLiteral("system"));

        // Writing rubbish through the manager is refused the same way.
        loc.setLanguage(QStringLiteral("klingon"));
        QCOMPARE(loc.language(), QStringLiteral("system"));
    }

    void switchingLanguageAsksForARetranslate()
    {
        SettingsManager settings;
        settings.setLanguage(QStringLiteral("system"));
        LocalizationManager loc(&settings);
        QSignalSpy retranslate(&loc, &LocalizationManager::retranslateRequested);
        QSignalSpy changed(&loc, &LocalizationManager::languageChanged);

        loc.setLanguage(QStringLiteral("es"));
        QCOMPARE(retranslate.count(), 1);
        QCOMPARE(changed.count(), 1);
        // effectiveLanguage reports what is actually LOADED, which is the
        // honest thing for it to report: a language whose catalog is missing
        // degrades to English rather than claiming a translation it does not
        // have. Both outcomes are correct here — what must not happen is a
        // third value.
        QVERIFY2(loc.effectiveLanguage() == QStringLiteral("es")
                     || loc.effectiveLanguage() == QStringLiteral("en"),
                 qPrintable(loc.effectiveLanguage()));
        QCOMPARE(loc.language(), QStringLiteral("es"));

        // Setting the same value again is not a change and must not churn
        // the whole UI through a retranslate.
        loc.setLanguage(QStringLiteral("es"));
        QCOMPARE(retranslate.count(), 1);
    }

    // ---- catalogs on disk ------------------------------------------------

    void everySupportedLanguageHasACatalogAndTheBuildListsIt()
    {
        const QString cmake = readAll(repoFile(QStringLiteral("CMakeLists.txt")));
        QVERIFY(!cmake.isEmpty());
        const QRegularExpression codesRe(
            QStringLiteral("set\\(LIGHTNING_LANGUAGE_CODES ([^)]*)\\)"));
        const auto m = codesRe.match(cmake);
        QVERIFY2(m.hasMatch(), "CMakeLists has no LIGHTNING_LANGUAGE_CODES");
        const QStringList built =
            m.captured(1).split(QRegularExpression(QStringLiteral("\\s+")),
                                Qt::SkipEmptyParts);

        for (const QString &code : LocalizationManager::supportedCodes()) {
            QVERIFY2(built.contains(code),
                     qPrintable(QStringLiteral("%1 is offered in the language "
                                               "picker but CMake never "
                                               "compiles its catalog")
                                    .arg(code)));
            const QString ts =
                repoFile(QStringLiteral("i18n/lightning_%1.ts").arg(code));
            QVERIFY2(QFile::exists(ts), qPrintable(ts));
        }
        // And nothing is compiled that the picker does not offer, which would
        // ship dead weight in every package.
        for (const QString &code : built) {
            QVERIFY2(LocalizationManager::isSupported(code),
                     qPrintable(QStringLiteral("CMake builds a catalog for "
                                               "'%1', which is not a "
                                               "selectable language").arg(code)));
        }
    }

    // A catalog can agree with every other catalog and still be stale. This
    // is what happened when all ten files remained internally consistent at
    // 1,922 messages while the source had grown to 2,545: every newly added
    // UI string silently fell back to English. Extract the current source to
    // a temporary catalog and compare translation lookup keys in both
    // directions so missing and removed messages are both visible.
    void catalogsMatchTheCurrentSource()
    {
#ifndef LIGHTNING_LUPDATE_EXECUTABLE
        QSKIP("configured without Qt Linguist tools — source extraction unavailable");
#else
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString fresh = temp.filePath(QStringLiteral("lightning_en_US.ts"));

        QProcess lupdate;
        lupdate.setWorkingDirectory(QStringLiteral(LIGHTNING_SOURCE_DIR));
        lupdate.setProcessChannelMode(QProcess::MergedChannels);
        lupdate.start(QStringLiteral(LIGHTNING_LUPDATE_EXECUTABLE), {
            QStringLiteral("-silent"),
            QStringLiteral("-locations"), QStringLiteral("none"),
            QStringLiteral("-no-obsolete"),
            QStringLiteral("-target-language"), QStringLiteral("en_US"),
            QStringLiteral("src"), QStringLiteral("qml"),
            QStringLiteral("-ts"), fresh,
        });
        QVERIFY2(lupdate.waitForFinished(30'000),
                 "lupdate did not finish within 30 seconds");
        const QString diagnostics = QString::fromUtf8(lupdate.readAll());
        QCOMPARE(lupdate.exitStatus(), QProcess::NormalExit);
        QVERIFY2(lupdate.exitCode() == 0, qPrintable(diagnostics));
        QVERIFY2(!diagnostics.contains(QStringLiteral("cannot be called without context")),
                 qPrintable(diagnostics));

        const QSet<QString> extracted = catalogKeys(fresh);
        QVERIFY2(extracted.size() > 2'000,
                 "lupdate extracted implausibly few source messages");
        for (const QString &code : LocalizationManager::supportedCodes()) {
            const QString path =
                repoFile(QStringLiteral("i18n/lightning_%1.ts").arg(code));
            const QSet<QString> catalog = catalogKeys(path);
            QVERIFY2(!catalog.isEmpty(), qPrintable(path));
            const QSet<QString> missing = extracted - catalog;
            const QSet<QString> stale = catalog - extracted;
            QVERIFY2(missing.isEmpty() && stale.isEmpty(),
                     qPrintable(QStringLiteral(
                         "%1 is out of sync with the source: %2 missing, %3 stale\n"
                         "missing:\n%4\nstale:\n%5")
                         .arg(code)
                         .arg(missing.size())
                         .arg(stale.size())
                         .arg(summarizeKeys(missing), summarizeKeys(stale))));
        }
#endif
    }

    // A translation that drops %1 loses the room name; one that invents %2
    // renders a literal "%2" to the user. Both are silent — lrelease accepts
    // them — so they are caught here instead.
    void translationsPreserveTheirPlaceholders()
    {
        int checked = 0;
        for (const QString &code : LocalizationManager::supportedCodes()) {
            const QString ts =
                repoFile(QStringLiteral("i18n/lightning_%1.ts").arg(code));
            const QString xml = readAll(ts);
            QVERIFY2(!xml.isEmpty(), qPrintable(ts));

            static const QRegularExpression msgRe(
                QStringLiteral("<message[^>]*>(.*?)</message>"),
                QRegularExpression::DotMatchesEverythingOption);
            auto it = msgRe.globalMatch(xml);
            while (it.hasNext()) {
                const QString body = it.next().captured(1);
                static const QRegularExpression srcRe(
                    QStringLiteral("<source>(.*?)</source>"),
                    QRegularExpression::DotMatchesEverythingOption);
                const auto sm = srcRe.match(body);
                if (!sm.hasMatch())
                    continue;
                const QSet<QString> want = placeholders(sm.captured(1));

                // Only FINISHED translations are shipped (-nounfinished), so
                // only those are held to the contract.
                static const QRegularExpression trRe(
                    QStringLiteral("<translation(?![^>]*type=\"unfinished\")[^>]*>"
                                   "(.*?)</translation>"),
                    QRegularExpression::DotMatchesEverythingOption);
                const auto tm = trRe.match(body);
                if (!tm.hasMatch())
                    continue;
                const QString translated = tm.captured(1);
                if (translated.trimmed().isEmpty())
                    continue;

                // A numerus translation carries several forms; every one of
                // them has to satisfy the contract on its own.
                static const QRegularExpression formRe(
                    QStringLiteral("<numerusform>(.*?)</numerusform>"),
                    QRegularExpression::DotMatchesEverythingOption);
                QStringList forms;
                auto fit = formRe.globalMatch(translated);
                while (fit.hasNext())
                    forms << fit.next().captured(1);
                const bool numerus = !forms.isEmpty();
                if (forms.isEmpty())
                    forms << translated;

                for (const QString &form : std::as_const(forms)) {
                    QSet<QString> got = placeholders(form);
                    QSet<QString> expected = want;
                    // A PLURAL form may drop %n, and only %n, and only a
                    // plural form may do it: a zero form idiomatically spells
                    // the count out rather than printing 0 — Arabic's is "لا
                    // أعضاء" (no members), which reads far better than "0
                    // members" and is exactly what the six-form plural rule
                    // exists to allow. Every OTHER placeholder still has to
                    // survive in every form, and a non-plural message may drop
                    // nothing at all: that is where a missing %1 silently
                    // loses the room name and an extra one prints "%2" at a
                    // user.
                    if (numerus && !got.contains(QStringLiteral("%n")))
                        expected.remove(QStringLiteral("%n"));
                    QVERIFY2(got == expected,
                             qPrintable(QStringLiteral(
                                 "%1: placeholders differ\n  source: %2\n"
                                 "  translation: %3")
                                    .arg(code, sm.captured(1), form)));
                    ++checked;
                }
            }
        }
        // Guards against the check silently passing because the regex stopped
        // matching anything at all.
        QVERIFY2(checked > 0, "no finished translations were examined");
    }

    // Product, protocol, library and file-format names are identifiers, not
    // prose. Translating Lightning to "Rayo" or Rust to the word for oxidised
    // iron renames the thing being described; transliterating PNG leaves a
    // user looking for a file type that is not in their file picker.
    void translationsPreserveTechnicalNames()
    {
        const QStringList protectedNames = {
            QStringLiteral("Lightning"), QStringLiteral("MatrixRTC"),
            QStringLiteral("Matrix"), QStringLiteral("LiveKit"),
            QStringLiteral("Element"), QStringLiteral("GStreamer"),
            QStringLiteral("PipeWire"), QStringLiteral("Flatpak"),
            QStringLiteral("Snap"), QStringLiteral("GIPHY"),
            QStringLiteral("KLIPY"), QStringLiteral("JPEG XL"),
            QStringLiteral("Electron"), QStringLiteral("Secret Service"),
            QStringLiteral("QSettings"), QStringLiteral("KWallet"),
            QStringLiteral("libsecret"), QStringLiteral("gnome-keyring"),
            QStringLiteral("xdg-desktop-portal"),
            QStringLiteral("matrix-client"),
            QStringLiteral("WebP"), QStringLiteral("PNG"),
            QStringLiteral("JPEG"), QStringLiteral("GIF"),
            QStringLiteral("BMP"), QStringLiteral("SDK"),
            QStringLiteral("SFU"), QStringLiteral("OIDC"),
            QStringLiteral("OAuth"), QStringLiteral("SAS"),
            QStringLiteral("QML"), QStringLiteral("Qt"),
            QStringLiteral("Rust"),
        };

        int checked = 0;
        for (const QString &code : LocalizationManager::supportedCodes()) {
            const QString path =
                repoFile(QStringLiteral("i18n/lightning_%1.ts").arg(code));
            const QString xml = readAll(path);
            QVERIFY2(!xml.isEmpty(), qPrintable(path));

            static const QRegularExpression msgRe(
                QStringLiteral("<message[^>]*>(.*?)</message>"),
                QRegularExpression::DotMatchesEverythingOption);
            auto it = msgRe.globalMatch(xml);
            while (it.hasNext()) {
                const QString body = it.next().captured(1);
                static const QRegularExpression srcRe(
                    QStringLiteral("<source>(.*?)</source>"),
                    QRegularExpression::DotMatchesEverythingOption);
                const auto sm = srcRe.match(body);
                if (!sm.hasMatch())
                    continue;

                static const QRegularExpression trRe(
                    QStringLiteral("<translation(?![^>]*type=\"unfinished\")[^>]*>"
                                   "(.*?)</translation>"),
                    QRegularExpression::DotMatchesEverythingOption);
                const auto tm = trRe.match(body);
                if (!tm.hasMatch() || tm.captured(1).trimmed().isEmpty())
                    continue;

                static const QRegularExpression formRe(
                    QStringLiteral("<numerusform>(.*?)</numerusform>"),
                    QRegularExpression::DotMatchesEverythingOption);
                QStringList forms;
                auto fit = formRe.globalMatch(tm.captured(1));
                while (fit.hasNext())
                    forms << fit.next().captured(1);
                if (forms.isEmpty())
                    forms << tm.captured(1);

                for (const QString &name : protectedNames) {
                    if (tokenOccurrences(sm.captured(1), name) == 0)
                        continue;
                    for (const QString &form : std::as_const(forms)) {
                        QVERIFY2(tokenOccurrences(form, name) > 0,
                                 qPrintable(QStringLiteral(
                                     "%1 drops or translates the technical name %2\n"
                                     "source: %3\ntranslation: %4")
                                     .arg(code, name, sm.captured(1), form)));
                        ++checked;
                    }
                }
            }
        }
        QVERIFY2(checked > 100, "implausibly few technical names were checked");
    }

    // ---- the catalog actually loads --------------------------------------

    // English is the SOURCE language and still needs a catalog, because a
    // "%n room(s)" source string renders its "(s)" LITERALLY when nothing is
    // loaded. This is the assertion that would fail if someone decided the
    // English catalog was redundant and deleted it.
    void theCompiledEnglishCatalogSuppliesPluralForms()
    {
#ifndef LIGHTNING_HAS_TRANSLATIONS
        QSKIP("configured without Qt Linguist tools — no catalog is compiled");
#endif
        const QString qm = QStringLiteral(LIGHTNING_QM_DIR "/lightning_en.qm");
        QVERIFY2(QFile::exists(qm), qPrintable(qm));

        QTranslator t;
        QVERIFY2(t.load(qm), "the compiled English catalog did not load");

        const QString one =
            t.translate("TimelinePane", "%n room(s)", "rooms inside a Space", 1);
        const QString many =
            t.translate("TimelinePane", "%n room(s)", "rooms inside a Space", 4);
        QCOMPARE(one, QStringLiteral("%n room"));
        QCOMPARE(many, QStringLiteral("%n rooms"));
        QVERIFY2(!one.contains(QStringLiteral("(s)")),
                 "the English UI would render a literal \"(s)\"");
    }

    // Smoke-test the new catalog through the same compiled artifact the app
    // loads, and pin the Matrix-specific terminology that generic machine
    // translation most readily turns into unrelated everyday words.
    void theCompiledLithuanianCatalogUsesMatrixTerminology()
    {
#ifndef LIGHTNING_HAS_TRANSLATIONS
        QSKIP("configured without Qt Linguist tools — no catalog is compiled");
#endif
        const QString qm = QStringLiteral(LIGHTNING_QM_DIR "/lightning_lt.qm");
        QVERIFY2(QFile::exists(qm), qPrintable(qm));

        QTranslator t;
        QVERIFY2(t.load(qm), "the compiled Lithuanian catalog did not load");
        QCOMPARE(t.translate("DiscoverJoinDialog", "Space"),
                 QStringLiteral("Erdvė"));
        QCOMPARE(t.translate("AccountMenu", "Accounts"),
                 QStringLiteral("Paskyros"));
        QCOMPARE(t.translate("ThreadPanel", "Thread"),
                 QStringLiteral("Gija"));
        QCOMPARE(t.translate("SettingsScreen", "Backend"),
                 QStringLiteral("Posistemė"));
    }

    // ---- source discipline ------------------------------------------------

    // Cheap and precise on purpose: it flags ONLY a user-visible property
    // bound to a bare quoted literal, which is unambiguous. Anything richer
    // (a literal inside a ternary, an identifier comparison) is left alone,
    // because a noisy version of this test gets disabled and then protects
    // nothing.
    void userVisibleQmlPropertiesAreTranslatable()
    {
        static const QRegularExpression bare(
            QStringLiteral("^\\s*(text|placeholderText|ToolTip\\.text|"
                           "Accessible\\.name|Accessible\\.description)"
                           "\\s*:\\s*\"([^\"]{2,})\"\\s*$"));

        // Values that are deliberately NOT translated, each for a stated
        // reason. Keyed by VALUE rather than by file:line so moving a line
        // does not silently re-open a hole, and so adding an entry is a
        // decision about a STRING rather than about a location.
        const QSet<QString> allowed = {
            // The product name. Translating it would rename the application.
            QStringLiteral("Lightning"),
            // An example homeserver URL. URLs are not prose (requirement:
            // never translate URLs), and this one is a real, reachable server.
            QStringLiteral("https://matrix.org"),
            // A file-format name shown as a badge on an animated image. "GIF"
            // is the format's name in every language, like "PNG" or "MP4".
            QStringLiteral("GIF"),
            // A keycap. It is the legend physically printed on the key, so it
            // matches what the user is looking at whatever the UI language.
            QStringLiteral("ESC"),
        };

        QDir dir(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml"));
        const QStringList files = dir.entryList({ QStringLiteral("*.qml") },
                                                QDir::Files, QDir::Name);
        QVERIFY(files.size() > 40);
        QStringList offenders;
        for (const QString &name : files) {
            const QString src = readAll(dir.filePath(name));
            const QStringList lines = src.split(QLatin1Char('\n'));
            for (int i = 0; i < lines.size(); ++i) {
                const auto m = bare.match(lines.at(i));
                if (!m.hasMatch())
                    continue;
                const QString value = m.captured(2);
                // An empty-ish or symbol-only value is not prose.
                static const QRegularExpression prose(
                    QStringLiteral("[A-Za-z]{2,}"));
                if (!prose.match(value).hasMatch())
                    continue;
                if (allowed.contains(value))
                    continue;
                offenders << QStringLiteral("%1:%2  %3")
                                 .arg(name).arg(i + 1).arg(lines.at(i).trimmed());
            }
        }
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral("user-visible QML text is not "
                                           "translatable:\n  %1")
                                .arg(offenders.join(QStringLiteral("\n  ")))));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(LocalizationTest)
#include "LocalizationTest.moc"
