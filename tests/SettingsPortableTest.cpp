// Settings export / import and sync: the schema, the file codec, the sync
// merge rules. Every case pins one rule against the obvious naive version
// (noted per case) so it fails on code that lacks the rule.

#include "app/SettingsManager.h"
#include "settings/SettingsPortable.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaProperty>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace settingsport;

namespace {

QVariantMap sample()
{
    QVariantMap m;
    m.insert("theme", 5);
    m.insert("textScale", 120);
    m.insert("language", QStringLiteral("lt"));
    m.insert("reducedMotion", true);
    m.insert("hiddenComposerButtons", QStringList{ "gif", "poll" });
    return m;
}

Document docOf(const QVariantMap &settings)
{
    Document d;
    d.settings = settings;
    d.app = QStringLiteral("0.0.0-test");
    return d;
}

// A complete local view: every Sync field at its default.
QVariantMap allDefaults()
{
    QVariantMap m;
    for (const Field &f : fields()) {
        if (f.flags & Sync)
            m.insert(f.name, f.def);
    }
    return m;
}

QByteArray j(const char *text)
{
    return QByteArray(text).replace('\'', '"');
}

Entry E(const QVariant &v, qint64 t) { return Entry{ v, t }; }

ParsedBlob blobOf(const Blob &entries)
{
    ParsedBlob p;
    p.present = true;
    p.entries = entries;
    p.version = 1;
    return p;
}

} // namespace

class SettingsPortableTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("SettingsPortableTest"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_configHome.path());
    }

    // ---- schema ------------------------------------------------------------

    // A field name that is not a writable property would make export silently
    // drop it and import silently fail.
    void everyFieldIsAWritableSettingsProperty()
    {
        QSettings().clear();
        SettingsManager settings;
        const QMetaObject *mo = settings.metaObject();
        int checked = 0;
        for (const Field &f : fields()) {
            const int i = mo->indexOfProperty(f.name.toUtf8().constData());
            QVERIFY2(i >= 0, qPrintable(f.name));
            QVERIFY2(mo->property(i).isWritable(), qPrintable(f.name));
            ++checked;
        }
        QCOMPARE(checked, fields().size());
        QVERIFY(checked > 60);
    }

    // The schema is an allow-list: nothing that names a secret, an identity or
    // a place on disk may ever be listed.
    void noFieldNamesASecretAnIdentityOrAPath()
    {
        static const QRegularExpression bad(
            QStringLiteral("token|secret|password|recovery|passphrase|keyring|session|userId|deviceId|"
                           "homeserver|path|folder|geometry|microphoneId|speakerId|cameraId|accessKey|apiKey"),
            QRegularExpression::CaseInsensitiveOption);
        for (const Field &f : fields())
            QVERIFY2(!bad.match(f.name).hasMatch(), qPrintable(f.name));
        // Sync is a subset of Export.
        for (const Field &f : fields())
            if (f.flags & Sync)
                QVERIFY2(f.flags & Export, qPrintable(f.name));
        QVERIFY(find(QStringLiteral("downloadFolder")) == nullptr);
        QVERIFY(find(QStringLiteral("preferredMicrophoneId")) == nullptr);
        QVERIFY(find(QStringLiteral("homeserverUrl")) == nullptr);
        QVERIFY(find(QStringLiteral("verificationWarningDismissed")) == nullptr);
    }

    // Settings that widen what leaves the app, or that make a window or a
    // machine different from another, never travel through the server.
    void deviceAndSecurityRelevantSettingsDoNotSync()
    {
        for (const char *n : { "strictDeviceTrust", "loadPreviewsInEncryptedRooms", "notificationPreview",
                               "notificationPreviewEncrypted", "keepMediaOnDevice", "interfaceZoom",
                               "roomListWidth", "microphoneGain", "callSoundVolume", "shareMaxHeight",
                               "closeToTray", "startInTray", "smoothScrolling" }) {
            const Field *f = find(QString::fromLatin1(n));
            QVERIFY2(f, n);
            QVERIFY2(!(f->flags & Sync), n);
            QVERIFY2(f->flags & Export, n);
        }
        QVERIFY(names(Sync).contains(QStringLiteral("theme")));
        QVERIFY(names(Sync).size() >= 40);
    }

    // The default column is what SettingsManager really answers when nothing
    // was ever set; sync's "explicit only" rule depends on it.
    void defaultsMatchAFreshSettingsManager()
    {
        QSettings().clear();
        SettingsManager settings;
        const QVariantMap now = readProperties(&settings, Export);
        QStringList wrong;
        for (const Field &f : fields()) {
            if (!now.contains(f.name) || now.value(f.name) != f.def)
                wrong.append(QStringLiteral("%1 table=%2 real=%3")
                                 .arg(f.name, f.def.toString(), now.value(f.name).toString()));
        }
        if (qEnvironmentVariableIsSet("SETTINGS_PRINT_DEFAULTS")) {
            QJsonObject o;
            for (auto it = now.constBegin(); it != now.constEnd(); ++it)
                o.insert(it.key(), QJsonValue::fromVariant(it.value()));
            QFile out(qEnvironmentVariable("SETTINGS_PRINT_DEFAULTS"));
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write(QJsonDocument(o).toJson());
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join("; ")));
    }

    // ---- file codec --------------------------------------------------------

    void exportThenImportRoundTrips()
    {
        const QByteArray bytes = buildDocument(docOf(sample()), QStringLiteral("2026-10-13T00:00:00Z"));
        const ParseResult r = parseDocument(bytes);
        QVERIFY(r.ok);
        QCOMPARE(r.version, kFileVersion);
        QCOMPARE(r.doc.settings, sample());
        QVERIFY(r.rejected.isEmpty());
        QVERIFY(r.unknown.isEmpty());
    }

    // The naive exporter dumps whatever it is handed.
    void secretsAndIdentitiesAreNeverWritten()
    {
        QVariantMap m = sample();
        m.insert("accessToken", QStringLiteral("syt_SECRETTOKEN"));
        m.insert("recoveryKey", QStringLiteral("EsTc SECRET RECOVERY"));
        m.insert("deviceId", QStringLiteral("DEVICESECRETID"));
        m.insert("downloadFolder", QStringLiteral("/home/someone/private"));
        m.insert("preferredMicrophoneId", QStringLiteral("alsa_input.hw-secret"));
        m.insert("userId", QStringLiteral("@alice:example.org"));
        Document d = docOf(m);
        d.shortcuts.insert(QStringLiteral("send"), QStringLiteral("Ctrl+Return"));
        const QByteArray bytes = buildDocument(d, QStringLiteral("2026-10-13T00:00:00Z"));
        for (const char *needle : { "SECRET", "syt_", "DEVICE", "/home/", "alsa_", "@alice", "accessToken",
                                    "recoveryKey", "downloadFolder" })
            QVERIFY2(!bytes.contains(needle), needle);
        QVERIFY(bytes.contains("\"textScale\""));
    }

    // An import file is untrusted: a key the schema does not list is ignored
    // and reported, never applied, whatever its name.
    void importIgnoresUnknownAndSecretNamedKeys()
    {
        const QByteArray file = j("{'format':'lightning-settings','version':1,'settings':{ 'textScale':110,'accessToken':'syt_x','homeserverUrl':'evil.example', 'downloadFolder':'/etc','futureThing':7}}");
        const ParseResult r = parseDocument(file);
        QVERIFY(r.ok);
        QCOMPARE(r.doc.settings.keys(), QStringList{ "textScale" });
        QVERIFY(r.unknown.contains("accessToken"));
        QVERIFY(r.unknown.contains("homeserverUrl"));
        QVERIFY(r.unknown.contains("downloadFolder"));
        QVERIFY(r.unknown.contains("futureThing"));
    }

    // The naive importer clamps 9999 to 140; the contract is rejection per key
    // with a reason, the other keys still applying.
    void invalidValuesAreRejectedPerKeyNotClamped()
    {
        const QByteArray file = j("{'format':'lightning-settings','version':1,'settings':{ 'textScale':9999,'theme':'dark','reducedMotion':1,'gifPreferredProvider':'evil', 'language':'lt','hiddenComposerButtons':[1,2],'messageLayout':1.5,'clockFormat':2}}");
        const ParseResult r = parseDocument(file);
        QVERIFY(r.ok);
        QCOMPARE(r.rejected.value("textScale"), QStringLiteral("range"));
        QCOMPARE(r.rejected.value("theme"), QStringLiteral("type"));
        QCOMPARE(r.rejected.value("reducedMotion"), QStringLiteral("type"));
        QCOMPARE(r.rejected.value("gifPreferredProvider"), QStringLiteral("not_allowed"));
        QCOMPARE(r.rejected.value("hiddenComposerButtons"), QStringLiteral("type"));
        QCOMPARE(r.rejected.value("messageLayout"), QStringLiteral("type"));
        QCOMPARE(r.doc.settings.value("language").toString(), QStringLiteral("lt"));
        QCOMPARE(r.doc.settings.value("clockFormat").toInt(), 2);
        QVERIFY(!r.doc.settings.contains("textScale"));
        QCOMPARE(r.rejected.size(), 6);
    }

    void controlCharactersAndOversizedStringsAreRejected()
    {
        QJsonObject settings;
        settings.insert("uiFont", QStringLiteral("Bad\nFont"));
        settings.insert("monoFont", QString(500, QLatin1Char('a')));
        QJsonObject root{ { "format", "lightning-settings" }, { "version", 1 }, { "settings", settings } };
        const ParseResult r = parseDocument(QJsonDocument(root).toJson());
        QCOMPARE(r.rejected.value("uiFont"), QStringLiteral("control"));
        QCOMPARE(r.rejected.value("monoFont"), QStringLiteral("length"));
    }

    void futureVersionReadsKnownKeysAndSaysSo()
    {
        const QByteArray file = j("{'format':'lightning-settings','version':9,'settings':{'textScale':100,'newerKey':{'a':1}}}");
        const ParseResult r = parseDocument(file);
        QVERIFY(r.ok);
        QVERIFY(r.newerVersion);
        QCOMPARE(r.version, 9);
        QCOMPARE(r.doc.settings.value("textScale").toInt(), 100);
        QVERIFY(r.unknown.contains("newerKey"));
    }

    void notASettingsFileIsRefused()
    {
        QCOMPARE(parseDocument("").error, QStringLiteral("empty"));
        QCOMPARE(parseDocument("not json").error, QStringLiteral("not_json"));
        QCOMPARE(parseDocument("[1,2]").error, QStringLiteral("not_json"));
        QCOMPARE(parseDocument(j("{'format':'something-else','version':1}")).error, QStringLiteral("not_settings"));
        QCOMPARE(parseDocument(j("{'format':'lightning-settings'}")).error, QStringLiteral("not_settings"));
        QCOMPARE(parseDocument(j("{'format':'lightning-settings','version':0}")).error, QStringLiteral("not_settings"));
        QCOMPARE(parseDocument(QByteArray(kMaxFileBytes + 1, ' ')).error, QStringLiteral("empty"));
        QByteArray big = j("{'format':'lightning-settings','version':1,'pad':'");
        big += QByteArray(kMaxFileBytes, 'x');
        big += "\"}";
        QCOMPARE(parseDocument(big).error, QStringLiteral("too_large"));
    }

    void shortcutsAndThemesAreValidated()
    {
        const QByteArray file = j("{'format':'lightning-settings','version':1,'settings':{}, 'shortcuts':{'send':'Ctrl+Return','../bad':'x','ok.id':'Ctrl+K','long':'")
            + QByteArray(100, 'a') + j("'}, 'customThemes':[{'lightningTheme':1,'name':'A'},'not an object',7],'activeCustomTheme':0}");
        const ParseResult r = parseDocument(file);
        QVERIFY(r.ok);
        QCOMPARE(r.doc.shortcuts.size(), 2);
        QVERIFY(r.shortcutsRejected.contains("../bad"));
        QVERIFY(r.shortcutsRejected.contains("long"));
        QCOMPARE(r.doc.themes.size(), 1);
        QCOMPARE(r.themesDropped, 2);
        QCOMPARE(r.doc.activeTheme, 0);
    }

    void thePreviewListsOnlyWhatWouldChange()
    {
        QVariantMap current{ { "textScale", 100 }, { "theme", 2 }, { "reducedMotion", false } };
        QVariantMap incoming{ { "textScale", 120 }, { "theme", 2 }, { "language", QStringLiteral("lt") } };
        const QList<Change> changes = diff(current, incoming);
        QCOMPARE(changes.size(), 2);
        QStringList got;
        for (const Change &c : changes)
            got.append(c.name);
        got.sort();
        QCOMPARE(got, (QStringList{ "language", "textScale" }));
        QCOMPARE(changes.first().group.isEmpty(), false);
    }

    // The properties read back from a real SettingsManager survive the file and
    // are applied back; secrets stored next to them in the same QSettings never
    // appear in the file. Fails on an exporter that walks the QSettings keys.
    void aRealSettingsManagerRoundTripsAndLeaksNothing()
    {
        QSettings().clear();
        SettingsManager settings;
        settings.setHomeserverUrl(QStringLiteral("https://secret-homeserver.example"));
        settings.setDownloadFolder(QStringLiteral("/home/secret-user/Downloads"));
        settings.setPreferredMicrophoneId(QStringLiteral("alsa_input.secret-device"));
        settings.setTextScale(125);
        settings.setReducedMotion(true);
        settings.setLanguage(QStringLiteral("lt"));
        settings.setHiddenComposerButtons(QStringList{ "poll" });
        settings.setMessageLayout(2);

        const QVariantMap before = readProperties(&settings, Export);
        QCOMPARE(before.value("textScale").toInt(), 125);
        Document d;
        d.settings = before;
        const QByteArray bytes = buildDocument(d, QStringLiteral("x"));
        QVERIFY(!bytes.contains("secret"));
        QVERIFY(!bytes.contains("alsa_input"));

        settings.setTextScale(100);
        settings.setReducedMotion(false);
        settings.setMessageLayout(0);
        settings.setLanguage(QString());
        settings.setHiddenComposerButtons(QStringList());
        QVERIFY(readProperties(&settings, Export) != before);

        const ParseResult r = parseDocument(bytes);
        QVERIFY(r.ok);
        QVERIFY(writeProperties(&settings, r.doc.settings).isEmpty());
        QCOMPARE(readProperties(&settings, Export), before);
        QCOMPARE(settings.textScale(), 125);
        QCOMPARE(settings.messageLayout(), 2);
    }

    // ---- sync blob ---------------------------------------------------------

    void theBlobRoundTripsAsIntegersOnly()
    {
        Blob b;
        b.insert("textScale", E(120, 1760000000123LL));
        b.insert("reducedMotion", E(true, 1760000000124LL));
        b.insert("hiddenComposerButtons", E(QStringList{ "poll" }, 5));
        const QJsonObject o = composeBlob(b, QJsonObject{ { "futureKey", QJsonObject{ { "v", 1 }, { "t", 2 } } } });
        const QByteArray text = QJsonDocument(o).toJson(QJsonDocument::Compact);
        // Account data must be canonical JSON: no floats, no exponents.
        QVERIFY2(!QRegularExpression(QStringLiteral("[0-9]\\.[0-9]|[0-9][eE][+-]?[0-9]"))
                      .match(QString::fromUtf8(text)).hasMatch(),
                 text.constData());
        const ParsedBlob p = parseBlob(QJsonDocument::fromJson(text).object());
        QCOMPARE(p.entries.size(), 3);
        QCOMPARE(p.entries.value("textScale").t, 1760000000123LL);
        QCOMPARE(p.entries.value("textScale").v.toInt(), 120);
        QVERIFY(p.keep.contains("futureKey"));
        // The unknown entry is carried through a rewrite.
        QVERIFY(composeBlob(p.entries, p.keep).value("settings").toObject().contains("futureKey"));
    }

    // A blob is as untrusted as a file, and the server can write it: a
    // device-only or security-relevant key in it is never applied.
    void theBlobCannotCarryAKeyThatDoesNotSync()
    {
        const QJsonObject o = QJsonDocument::fromJson(j("{'version':1,'settings':{ 'strictDeviceTrust':{'v':false,'t':1},'loadPreviewsInEncryptedRooms':{'v':true,'t':1}, 'notificationPreview':{'v':0,'t':1},'textScale':{'v':9999,'t':1},'theme':{'v':3,'t':2}}}")).object();
        const ParsedBlob p = parseBlob(o);
        QCOMPARE(p.entries.keys(), QStringList{ "theme" });
        QVERIFY(p.rejected.contains("strictDeviceTrust"));
        QVERIFY(p.rejected.contains("loadPreviewsInEncryptedRooms"));
        QVERIFY(p.rejected.contains("notificationPreview"));
        QVERIFY(p.rejected.contains("textScale"));
    }

    void clearedMissingAndUnreadableBlobsAreDistinct()
    {
        QVERIFY(!parseBlob(QJsonObject(), false).present);
        const ParsedBlob cleared = parseBlob(QJsonObject());
        QVERIFY(cleared.present && cleared.readable && cleared.entries.isEmpty());
        QVERIFY(!parseBlob(QJsonObject{ { "junk", 1 } }).readable);
        QVERIFY(parseBlob(QJsonObject{ { "version", 7 }, { "settings", QJsonObject() } }).newerVersion);
    }

    void aFullBlobIsFarInsideTheAccountDataLimit()
    {
        Blob b;
        for (const Field &f : fields()) {
            if (!(f.flags & Sync))
                continue;
            QVariant v = f.def;
            if (f.kind == Kind::String)
                v = QString(f.maxLen, QLatin1Char('a'));
            if (f.kind == Kind::StringList)
                v = QStringList{ QString(f.maxLen, QLatin1Char('a')) };
            b.insert(f.name, E(v, 1760000000000LL));
        }
        const int size = QJsonDocument(composeBlob(b, QJsonObject())).toJson(QJsonDocument::Compact).size();
        QVERIFY2(size < kMaxBlobBytes / 4, qPrintable(QString::number(size)));
    }

    // ---- merge -------------------------------------------------------------

    // The remote value is adopted when nothing local is pending.
    void aLaterServerValueIsApplied()
    {
        QVariantMap local = allDefaults();
        State st;
        st.lastSynced.insert("textScale", 100);
        st.lastTs.insert("textScale", 10);
        local.insert("textScale", 100);
        const MergeResult r = merge(blobOf({ { "textScale", E(130, 20) } }), local, st, 1000);
        QCOMPARE(r.applyLocal.value("textScale").toInt(), 130);
        QVERIFY(!r.needsWrite);
        QVERIFY(r.lostLocal.isEmpty());
        QCOMPARE(r.afterApply.lastSynced.value("textScale").toInt(), 130);
    }

    // A local change not yet pushed is pushed, not overwritten by the server's
    // older value.
    void aNewerLocalChangeWinsAndIsPushed()
    {
        QVariantMap local = allDefaults();
        local.insert("textScale", 110);
        State st;
        st.lastSynced.insert("textScale", 100);
        st.lastTs.insert("textScale", 10);
        const MergeResult r = merge(blobOf({ { "textScale", E(100, 10) } }), local, st, 5000);
        QVERIFY(r.applyLocal.isEmpty());
        QVERIFY(r.needsWrite);
        QCOMPARE(r.toWrite.value("textScale").v.toInt(), 110);
        QCOMPARE(r.toWrite.value("textScale").t, 5000LL);
        QVERIFY(r.afterApply.pending.contains("textScale"));      // not yet acknowledged
        QVERIFY(!r.afterWrite.pending.contains("textScale"));     // acknowledged
        QCOMPARE(r.afterWrite.lastSynced.value("textScale").toInt(), 110);
    }

    // The naive last-writer-wins replaces an unsynced edit silently. Here the
    // later remote value does win, but the loss is REPORTED.
    void anUnsyncedLocalEditReplacedByALaterRemoteOneIsReported()
    {
        QVariantMap local = allDefaults();
        local.insert("textScale", 110);
        State st;
        st.lastSynced.insert("textScale", 100);
        st.lastTs.insert("textScale", 10);
        st.pending.insert("textScale", 2000);
        const MergeResult r = merge(blobOf({ { "textScale", E(135, 3000) } }), local, st, 4000);
        QCOMPARE(r.applyLocal.value("textScale").toInt(), 135);
        QCOMPARE(r.lostLocal, QStringList{ "textScale" });
        QVERIFY(!r.needsWrite);
        QVERIFY(r.afterApply.pending.isEmpty());
    }

    void perKeyMergeKeepsBothDevicesEdits()
    {
        QVariantMap local = allDefaults();
        local.insert("textScale", 110);      // edited here
        local.insert("reducedMotion", false);
        State st;
        st.lastSynced.insert("textScale", 100);
        st.lastTs.insert("textScale", 10);
        st.lastSynced.insert("reducedMotion", false);
        st.lastTs.insert("reducedMotion", 10);
        const ParsedBlob server = blobOf({ { "textScale", E(100, 10) }, { "reducedMotion", E(true, 99) } }); // edited there
        const MergeResult r = merge(server, local, st, 5000);
        QCOMPARE(r.applyLocal.value("reducedMotion").toBool(), true);
        QCOMPARE(r.toWrite.value("textScale").v.toInt(), 110);
        QCOMPARE(r.toWrite.value("reducedMotion").v.toBool(), true);
        QVERIFY(r.needsWrite);
    }

    // A concurrent writer replaced the blob with an older copy: we put back
    // what we had agreed.
    void aClobberedKeyIsReasserted()
    {
        QVariantMap local = allDefaults();
        local.insert("textScale", 120);
        State st;
        st.lastSynced.insert("textScale", 120);
        st.lastTs.insert("textScale", 500);
        const MergeResult gone = merge(blobOf({}), local, st, 9000);
        QVERIFY(gone.needsWrite);
        QCOMPARE(gone.toWrite.value("textScale").v.toInt(), 120);
        QCOMPARE(gone.toWrite.value("textScale").t, 500LL);
        const MergeResult older = merge(blobOf({ { "textScale", E(100, 100) } }), local, st, 9000);
        QVERIFY(older.needsWrite);
        QVERIFY(older.applyLocal.isEmpty());
        QCOMPARE(older.toWrite.value("textScale").v.toInt(), 120);
    }

    // A server clock far ahead must not make one value win for ever.
    void aTimestampFromTheFutureIsClamped()
    {
        QVariantMap local = allDefaults();
        local.insert("textScale", 110);
        State st;
        st.lastSynced.insert("textScale", 100);
        st.lastTs.insert("textScale", 10);
        st.pending.insert("textScale", 4000);
        // t = year 2100 would beat every later local edit; clamped to now.
        const MergeResult r = merge(blobOf({ { "textScale", E(135, 4102444800000LL) } }), local, st, 5000);
        QCOMPARE(r.afterApply.lastTs.value("textScale"), 5000LL);
    }

    void aNewerServerSchemaIsReadButNeverWritten()
    {
        QVariantMap local = allDefaults();
        local.insert("textScale", 110);
        ParsedBlob server = blobOf({ { "reducedMotion", E(true, 5) } });
        server.newerVersion = true;
        State st;
        st.lastSynced.insert("textScale", 100);
        st.lastTs.insert("textScale", 1);
        const MergeResult r = merge(server, local, st, 100);
        QVERIFY(r.readOnly);
        QVERIFY(!r.needsWrite);
        QCOMPARE(r.applyLocal.value("reducedMotion").toBool(), true);
    }

    // Untouched settings are not a choice: a new device's defaults never
    // overwrite the account's.
    void aFreshDeviceUploadsNothingItNeverChose()
    {
        const QVariantMap local = allDefaults();
        const ParsedBlob server = blobOf({ { "textScale", E(135, 5) } });
        const MergeResult r = uploadLocal(server, local, 1000);
        QVERIFY(!r.needsWrite);
        QCOMPARE(r.applyLocal.value("textScale").toInt(), 135);
    }

    void uploadMineMakesExplicitLocalValuesWin()
    {
        QVariantMap local = allDefaults();
        local.insert("textScale", 110);
        const ParsedBlob server = blobOf({ { "textScale", E(135, 5) }, { "reducedMotion", E(true, 5) } });
        const MergeResult r = uploadLocal(server, local, 1000);
        QVERIFY(r.needsWrite);
        QCOMPARE(r.toWrite.value("textScale").v.toInt(), 110);
        QVERIFY(!r.applyLocal.contains("textScale"));
        QCOMPARE(r.applyLocal.value("reducedMotion").toBool(), true);
    }

    void adoptServerChangesLocalAndUploadsNothing()
    {
        QVariantMap local = allDefaults();
        local.insert("textScale", 110);
        const ParsedBlob server = blobOf({ { "textScale", E(135, 5) } });
        const MergeResult r = adoptServer(server, local, 1000);
        QVERIFY(!r.needsWrite);
        QCOMPARE(r.applyLocal.value("textScale").toInt(), 135);
        QVERIFY(r.afterApply.pending.isEmpty());
        const FirstRunPlan plan = planFirstRun(server, local);
        QCOMPARE(plan.differing, QStringList{ "textScale" });
        QVERIFY(!plan.serverEmpty);
        QVERIFY(planFirstRun(blobOf({}), local).serverEmpty);
    }

    void changesBackToTheSyncedValueStopBeingPending()
    {
        QVariantMap local = allDefaults();
        local.insert("textScale", 100);
        State st;
        st.lastSynced.insert("textScale", 100);
        st.pending.insert("textScale", 5);
        const State out = detectLocalChanges(local, st, 99);
        QVERIFY(!out.pending.contains("textScale"));
        local.insert("textScale", 101);
        const State again = detectLocalChanges(local, out, 99);
        QCOMPARE(again.pending.value("textScale"), 99LL);
        // Already pending keeps its earlier time.
        QCOMPARE(detectLocalChanges(local, again, 500).pending.value("textScale"), 99LL);
    }

    void theStateSurvivesItsOwnJson()
    {
        State st;
        st.lastSynced.insert("textScale", 100);
        st.lastSynced.insert("hiddenComposerButtons", QStringList{ "gif" });
        st.lastTs.insert("textScale", 7);
        st.pending.insert("textScale", 9);
        st.lastSynced.insert("strictDeviceTrust", true); // not a Sync field: dropped on read
        QCOMPARE(stateFromJson(stateToJson(st)).lastSynced.keys(),
                 (QStringList{ "hiddenComposerButtons", "textScale" }));
        State clean = st;
        clean.lastSynced.remove("strictDeviceTrust");
        QVERIFY(stateFromJson(stateToJson(clean)) == clean);
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(SettingsPortableTest)
#include "SettingsPortableTest.moc"
