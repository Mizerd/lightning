// User-selectable and user-importable fonts.
//
// Three things this suite exists to hold:
//   * the FALLBACK rule. A family that is not on this host renders as the
//     bundled default and the STORED CHOICE IS NOT REWRITTEN. Getting that
//     backwards would destroy a preference every time a font was temporarily
//     absent, and nothing about the UI would say so;
//   * the IMPORT gates, on real bytes: a real font file is accepted, and
//     every wrong shape is refused for the right reason. The refusals are
//     asserted by CATEGORY, so a future change that swaps two of them shows
//     up here rather than in a dialog;
//   * the copy discipline. The path the user picked is never stored and never
//     read again; the recorded name is content-addressed and lives in
//     Lightning's own directory.
//
// The test target links Qt, SettingsManager and the app-data paths and NOTHING
// else — no MatrixClient, no network. That link list is itself the assertion
// that nothing remote can name a font file.
#include "app/FontManager.h"
#include "app/SettingsManager.h"
#include "storage/AppDataPaths.h"

#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QTemporaryDir>
#include <QtTest>

namespace {

// SOURCE_DIR, never an absolute path typed in: a hardcoded
// /home/<someone>/... only works on the machine it was written on and fails
// on CI and on every other checkout. The target defines it, exactly as
// desktop-integration-test does for the .desktop file it reads.
const char *kRealFont = SOURCE_DIR "/data/fonts/Manrope[wght].ttf";

QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

} // namespace

class FontManagerTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_root;
    QString m_fontsSource;

    QString write(const QString &name, const QByteArray &bytes) const
    {
        const QString path = m_root->path() + QLatin1String("/src/") + name;
        QDir().mkpath(m_root->path() + QLatin1String("/src"));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(bytes);
        f.close();
        return path;
    }

    static QUrl url(const QString &path) { return QUrl::fromLocalFile(path); }

private Q_SLOTS:
    void initTestCase()
    {
        m_root = std::make_unique<QTemporaryDir>();
        QVERIFY(m_root->isValid());
        qputenv("XDG_CONFIG_HOME", m_root->path().toUtf8());
        qputenv("XDG_DATA_HOME", m_root->path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("LightningFontTest"));
        QCoreApplication::setApplicationName(QStringLiteral("font-manager"));
        // A real font, so "QFontDatabase actually accepted it" is a real
        // answer and not a stub's.
        m_fontsSource = QString::fromLatin1(kRealFont);
        if (!QFileInfo::exists(m_fontsSource))
            m_fontsSource.clear();
    }

    // ---- pure validators -------------------------------------------------

    void sfntSignaturesAreAcceptedAndTheRestAreNot()
    {
        QVERIFY(FontManager::looksLikeSfnt(
            QByteArray::fromHex("0001000000090080")));
        QVERIFY(FontManager::looksLikeSfnt(QByteArrayLiteral("OTTO....")));
        QVERIFY(FontManager::looksLikeSfnt(QByteArrayLiteral("true....")));
        // A web transport wrapper and a collection are deliberately refused.
        QVERIFY(!FontManager::looksLikeSfnt(QByteArrayLiteral("wOFF....")));
        QVERIFY(!FontManager::looksLikeSfnt(QByteArrayLiteral("wOF2....")));
        QVERIFY(!FontManager::looksLikeSfnt(QByteArrayLiteral("ttcf....")));
        // Nothing else is a font.
        QVERIFY(!FontManager::looksLikeSfnt(QByteArrayLiteral("\x89PNG")));
        QVERIFY(!FontManager::looksLikeSfnt(QByteArrayLiteral("<svg")));
        QVERIFY(!FontManager::looksLikeSfnt(QByteArrayLiteral("#!/bin/sh")));
        QVERIFY(!FontManager::looksLikeSfnt(QByteArray()));
        QVERIFY(!FontManager::looksLikeSfnt(QByteArrayLiteral("\x00\x01")));
    }

    void onlyTtfAndOtfNamesAreOffered()
    {
        QVERIFY(FontManager::hasFontExtension(QStringLiteral("a.ttf")));
        QVERIFY(FontManager::hasFontExtension(QStringLiteral("a.OTF")));
        QVERIFY(!FontManager::hasFontExtension(QStringLiteral("a.ttc")));
        QVERIFY(!FontManager::hasFontExtension(QStringLiteral("a.woff2")));
        QVERIFY(!FontManager::hasFontExtension(QStringLiteral("a.so")));
        QVERIFY(!FontManager::hasFontExtension(QStringLiteral("a.ttf.exe")));
        QVERIFY(!FontManager::hasFontExtension(QStringLiteral("ttf")));
    }

    // A family name is stored verbatim when it is a plausible name, and
    // refused when it carries anything a markup or style reader downstream
    // would have to escape.
    void familyNamesAreValidatedSyntacticallyAndNotSemantically()
    {
        QCOMPARE(SettingsManager::acceptableFontFamily(
                     QStringLiteral("  Noto Sans CJK JP  ")),
                 QStringLiteral("Noto Sans CJK JP"));
        // Not installed anywhere, and still perfectly storable: existence is
        // FontManager's question, not this one's.
        QCOMPARE(SettingsManager::acceptableFontFamily(
                     QStringLiteral("Comic Sans MS")),
                 QStringLiteral("Comic Sans MS"));
        for (const QString &bad : { QStringLiteral(""),
                                    QStringLiteral("   "),
                                    QStringLiteral("a<b"),
                                    QStringLiteral("a\"b"),
                                    QStringLiteral("a;b"),
                                    QStringLiteral("a{b}"),
                                    QStringLiteral("a/b"),
                                    QStringLiteral("a\\b"),
                                    QStringLiteral("a\nb"),
                                    QString(200, QLatin1Char('x')) })
            QVERIFY2(SettingsManager::acceptableFontFamily(bad).isEmpty(),
                     qPrintable(bad.left(20)));
    }

    // ---- persistence -----------------------------------------------------

    void bothFacesPersistPerAccountAndSurviveARestart()
    {
        SettingsManager settings;
        const QString alice = QStringLiteral("@fa:example.org");
        const QString bob = QStringLiteral("@fb:example.org");
        settings.saveSession(QStringLiteral("https://example.org"), alice,
                             QStringLiteral("A"), QStringLiteral("t1"));
        settings.saveSession(QStringLiteral("https://example.org"), bob,
                             QStringLiteral("B"), QStringLiteral("t2"));

        QCOMPARE(settings.uiFont(), QStringLiteral("Manrope"));
        QCOMPARE(settings.monoFont(), QStringLiteral("JetBrains Mono"));

        settings.setActiveAccountUserId(alice);
        QSignalSpy uiSpy(&settings, &SettingsManager::uiFontChanged);
        QSignalSpy monoSpy(&settings, &SettingsManager::monoFontChanged);
        settings.setUiFont(QStringLiteral("Inter"));
        settings.setMonoFont(QStringLiteral("Fira Code"));
        QCOMPARE(uiSpy.count(), 1);
        QCOMPARE(monoSpy.count(), 1);
        QCOMPARE(settings.uiFont(), QStringLiteral("Inter"));
        // Not installed here; stored anyway. That is the feature.
        QCOMPARE(settings.monoFont(), QStringLiteral("Fira Code"));

        settings.setActiveAccountUserId(bob);
        QCOMPARE(settings.uiFont(), QStringLiteral("Inter")); // global fallback
        settings.setUiFont(QStringLiteral("Source Sans 3"));
        settings.setActiveAccountUserId(alice);
        QCOMPARE(settings.uiFont(), QStringLiteral("Inter"));

        // A fresh manager restores the ACTIVE account (alice) and with it
        // her two faces, not the last value any account happened to write.
        SettingsManager reopened;
        QCOMPARE(reopened.uiFont(), QStringLiteral("Inter"));
        QCOMPARE(reopened.monoFont(), QStringLiteral("Fira Code"));
    }

    void arefusedFamilyFallsBackToTheDefaultRatherThanBeingStored()
    {
        SettingsManager settings;
        settings.setUiFont(QStringLiteral("Inter"));
        QCOMPARE(settings.uiFont(), QStringLiteral("Inter"));
        settings.setUiFont(QStringLiteral("evil\"; }"));
        QCOMPARE(settings.uiFont(), QStringLiteral("Manrope"));
    }

    // ---- resolution and fallback ----------------------------------------

    // The rule the whole feature turns on.
    void aMissingFamilyRendersTheBundledFaceAndKeepsTheStoredChoice()
    {
        SettingsManager settings;
        FontManager fonts(&settings);
        settings.setUiFont(QStringLiteral("A Font Nobody Has 12345"));
        settings.setMonoFont(QStringLiteral("Another Absent Face 67890"));

        QCOMPARE(fonts.uiFamily(), QStringLiteral("Manrope"));
        QCOMPARE(fonts.monospaceFamily(), QStringLiteral("JetBrains Mono"));
        QVERIFY(!fonts.uiFamilyAvailable());
        QVERIFY(!fonts.monospaceFamilyAvailable());
        // NOT rewritten — this is what lets a reinstalled font come back, and
        // what keeps the picker honest about what the user asked for.
        QCOMPARE(fonts.storedUiFamily(),
                 QStringLiteral("A Font Nobody Has 12345"));
        QCOMPARE(settings.uiFont(), QStringLiteral("A Font Nobody Has 12345"));
        QCOMPARE(settings.monoFont(),
                 QStringLiteral("Another Absent Face 67890"));
    }

    void anInstalledFamilyIsUsedAsChosen()
    {
        SettingsManager settings;
        FontManager fonts(&settings);
        const QStringList installed = QFontDatabase::families();
        QVERIFY(!installed.isEmpty());
        const QString real = installed.first();
        if (SettingsManager::acceptableFontFamily(real).isEmpty())
            QSKIP("this host's first family is not a storable name");
        settings.setUiFont(real);
        QVERIFY(fonts.uiFamilyAvailable());
        QCOMPARE(fonts.uiFamily(), real);
    }

    void theFamilyListsAreUsableAndBundledFacesLeadTheUiList()
    {
        SettingsManager settings;
        FontManager fonts(&settings);
        const QStringList ui = fonts.uiFamilies();
        QVERIFY(!ui.isEmpty());
        // Never a duplicate: the bundled block is merged with the host's.
        QSet<QString> seen;
        for (const QString &f : ui) {
            QVERIFY2(!seen.contains(f.toLower()), qPrintable(f));
            seen.insert(f.toLower());
        }
        // An icon subset is not a UI face.
        QVERIFY(!ui.contains(QStringLiteral("Material Symbols Rounded")));
        const QStringList mono = fonts.monospaceFamilies();
        QVERIFY(!mono.isEmpty());
        for (const QString &f : mono)
            QVERIFY(!f.startsWith(QLatin1Char('.')));
    }

    // ---- import ----------------------------------------------------------

    void arealFontFileIsAcceptedCopiedAndRegistered()
    {
        if (m_fontsSource.isEmpty())
            QSKIP("bundled font source not present");
        SettingsManager settings;
        FontManager fonts(&settings);
        const QByteArray bytes = readAll(m_fontsSource);
        QVERIFY(!bytes.isEmpty());
        const QString picked = write(QStringLiteral("Picked Name.ttf"), bytes);
        QVERIFY(!picked.isEmpty());

        QSignalSpy importedSpy(&fonts, &FontManager::importedFontsChanged);
        QVERIFY(fonts.importFontFile(url(picked)));
        QCOMPARE(fonts.lastImportError(), QString());
        QCOMPARE(importedSpy.count(), 1);

        const QVariantList entries = fonts.importedFonts();
        QCOMPARE(entries.size(), 1);
        const QVariantMap entry = entries.first().toMap();
        QVERIFY(entry.value(QStringLiteral("available")).toBool());
        QVERIFY(!entry.value(QStringLiteral("families")).toStringList().isEmpty());

        // The recorded name is ours, content-addressed, and carries nothing
        // the user typed.
        const QString recorded = entry.value(QStringLiteral("fileName")).toString();
        QCOMPARE(recorded.size(), 68);
        QVERIFY(recorded.endsWith(QStringLiteral(".ttf")));
        QVERIFY(!recorded.contains(QStringLiteral("Picked")));
        QCOMPARE(settings.importedFontFiles(), QStringList{ recorded });

        // The copy is inside Lightning's own directory, and the picked file
        // is never referenced again.
        const QString copy = matrix::app_data::primaryRoot()
            + QLatin1String("/fonts/") + recorded;
        QVERIFY(QFileInfo(copy).isFile());
        QCOMPARE(QFileInfo(copy).size(), qint64(bytes.size()));
        for (const QString &stored : settings.importedFontFiles())
            QVERIFY(!stored.contains(QLatin1Char('/')));

        // Importing the same bytes again is one entry, not two.
        const QString again = write(QStringLiteral("Other Name.ttf"), bytes);
        QVERIFY(!fonts.importFontFile(url(again)));
        QCOMPARE(fonts.lastImportError(), QStringLiteral("already_imported"));
        QCOMPARE(fonts.importedFonts().size(), 1);

        // Removing it deletes the copy and forgets the record.
        QVERIFY(fonts.removeImportedFont(recorded));
        QVERIFY(!QFileInfo::exists(copy));
        QVERIFY(fonts.importedFonts().isEmpty());
        QVERIFY(settings.importedFontFiles().isEmpty());
    }

    void everyWrongShapeIsRefusedForItsOwnReason()
    {
        SettingsManager settings;
        FontManager fonts(&settings);
        struct Case { QString path; QString category; const char *what; };

        const QString notAFont =
            write(QStringLiteral("lie.ttf"), QByteArrayLiteral("\x89PNG\r\n\x1a\n padding padding"));
        const QString empty = write(QStringLiteral("empty.ttf"), QByteArray());
        const QString wrongExt =
            write(QStringLiteral("real.woff2"), QByteArrayLiteral("wOF2 padding padding"));
        QByteArray huge(int(FontManager::kMaxFontFileBytes) + 16, 'x');
        huge[0] = 0x00; huge[1] = 0x01; huge[2] = 0x00; huge[3] = 0x00;
        const QString oversize = write(QStringLiteral("huge.ttf"), huge);

        const QList<Case> cases = {
            { notAFont, QStringLiteral("not_a_font"), "png wearing a .ttf name" },
            { empty, QStringLiteral("empty"), "zero bytes" },
            { wrongExt, QStringLiteral("unsupported_extension"), "woff2" },
            { oversize, QStringLiteral("too_large"), "over the byte cap" },
            { m_root->path() + QStringLiteral("/does-not-exist.ttf"),
              QStringLiteral("not_a_file"), "absent" },
        };
        for (const Case &c : cases) {
            QVERIFY2(!fonts.importFontFile(url(c.path)), c.what);
            QVERIFY2(fonts.lastImportError() == c.category, c.what);
            QVERIFY2(fonts.importedFonts().isEmpty(), c.what);
        }

        // Not a local file at all: there is no download path here.
        QVERIFY(!fonts.importFontFile(QUrl(QStringLiteral("https://x.example/a.ttf"))));
        QCOMPARE(fonts.lastImportError(), QStringLiteral("not_a_local_file"));
        QVERIFY(!fonts.importFontFile(QUrl()));
        QCOMPARE(fonts.lastImportError(), QStringLiteral("not_a_local_file"));
    }

    // A hand-edited config must not be able to make this class read, register
    // or delete a path of somebody else's choosing.
    void aStoredNameThatIsNotOneOfOursIsIgnored()
    {
        SettingsManager settings;
        settings.setImportedFontFiles(
            { QStringLiteral("../../../etc/passwd"),
              QStringLiteral("/usr/share/fonts/x.ttf"),
              QStringLiteral("evil.ttf"),
              QStringLiteral("Manrope[wght].ttf") });
        FontManager fonts(&settings);
        fonts.loadImportedFonts();
        QVERIFY(fonts.importedFonts().isEmpty());
        QVERIFY(!fonts.removeImportedFont(QStringLiteral("../../../etc/passwd")));
        QVERIFY(!fonts.removeImportedFont(QStringLiteral("evil.ttf")));
    }

    // A record whose copy is gone reports unavailable; it is not silently
    // dropped, so the user can see why their font stopped applying.
    void arecordWhoseFileVanishedReportsUnavailable()
    {
        if (m_fontsSource.isEmpty())
            QSKIP("bundled font source not present");
        SettingsManager settings;
        FontManager fonts(&settings);
        const QString picked =
            write(QStringLiteral("gone.ttf"), readAll(m_fontsSource));
        QVERIFY(fonts.importFontFile(url(picked)));
        const QString recorded =
            fonts.importedFonts().first().toMap()
                .value(QStringLiteral("fileName")).toString();
        QFile::remove(matrix::app_data::primaryRoot()
                      + QLatin1String("/fonts/") + recorded);

        FontManager reopened(&settings);
        reopened.loadImportedFonts();
        QCOMPARE(reopened.importedFonts().size(), 1);
        QVERIFY(!reopened.importedFonts().first().toMap()
                     .value(QStringLiteral("available")).toBool());
        QVERIFY(reopened.removeImportedFont(recorded));
    }

    void theStoreRefusesRatherThanEvictingWhenItIsFull()
    {
        if (m_fontsSource.isEmpty())
            QSKIP("bundled font source not present");
        SettingsManager settings;
        // Pre-fill the record to the cap with well-formed names of ours.
        QStringList full;
        for (int i = 0; i < FontManager::kMaxImportedFonts; ++i) {
            full.append(QString(64, QLatin1Char('a')).replace(
                            0, 2, QString::asprintf("%02x", i))
                        + QStringLiteral(".ttf"));
        }
        settings.setImportedFontFiles(full);
        FontManager fonts(&settings);
        const QString picked =
            write(QStringLiteral("one-too-many.ttf"), readAll(m_fontsSource));
        QVERIFY(!fonts.importFontFile(url(picked)));
        QCOMPARE(fonts.lastImportError(), QStringLiteral("store_full"));
        // Nothing was discarded to make room.
        QCOMPARE(settings.importedFontFiles().size(),
                 FontManager::kMaxImportedFonts);
    }
};

QTEST_MAIN(FontManagerTest)
#include "FontManagerTest.moc"
