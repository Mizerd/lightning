#include <QTest>
#include <QVariantMap>

#include "calls/ShareAudioSources.h"

using namespace lightning::shareaudio;

namespace {
Stream stream(const QString &serial, const QString &key, qint64 pid = -1,
              qint64 start = 0)
{
    Stream s;
    s.serial = serial;
    s.appKey = key;
    s.appName = key;
    s.pid = pid;
    s.startTime = start;
    return s;
}
BranchRecord built(int index, const QString &key, qint64 pid = -1,
                   qint64 start = 0, bool muted = false)
{
    BranchRecord r;
    r.index = index;
    r.appKey = key;
    r.pid = pid;
    r.startTime = start;
    r.muted = muted;
    return r;
}
} // namespace

// Which application streams a share with sound captures.
//
// Capturing the default sink's monitor would send Lightning's own playback of
// the other participants back to them, so each playing application is
// captured on its own and Lightning's own streams are excluded.
//
// Tested here: which streams are eligible and the shape of the pipeline
// description. Not tested anywhere: the rescan bookkeeping, branch cap and
// poll lifecycle (need a live PipeWire graph), and the echo removal itself
// (needs a far end).
class ShareAudioSourcesTest : public QObject
{
    Q_OBJECT

private:
    static QVariantMap streamProps(qint64 pid, const QString &appName,
                                   const QString &serial = QStringLiteral("42"))
    {
        QVariantMap p;
        p.insert(QStringLiteral("media.class"),
                 QStringLiteral("Stream/Output/Audio"));
        p.insert(QStringLiteral("object.serial"), serial);
        p.insert(QStringLiteral("application.name"), appName);
        p.insert(QStringLiteral("node.name"), appName);
        if (pid > 0)
            p.insert(QStringLiteral("application.process.id"),
                     QString::number(pid));
        return p;
    }

private slots:
    // Our own playback must never be captured, whichever way the node
    // identifies itself.
    void ourOwnPlaybackIsNeverCaptured()
    {
        const qint64 ours = 4242;
        QVERIFY(!streamIsForeign(streamProps(ours, QStringLiteral("Firefox")),
                                 ours, QStringList{QStringLiteral("Lightning")}));
        // No pid on the node — the name still has to save us.
        QVERIFY(!streamIsForeign(streamProps(-1, QStringLiteral("Lightning")),
                                 ours, QStringList{QStringLiteral("Lightning")}));
        QVERIFY(!streamIsForeign(streamProps(-1, QStringLiteral("lightning")),
                                 ours, QStringList{QStringLiteral("Lightning")}));
    }

    // The name check covers a node of ours with no `application.process.id`
    // (where the pid guard cannot fire). It must match every spelling of our
    // name: the application name ("matrix-client") and the binary name
    // ("lightning-matrix").
    void everySpellingOfOurOwnNameIsExcluded()
    {
        const QStringList ours{ QStringLiteral("matrix-client"),
                                QStringLiteral("lightning-matrix") };
        // No pid on the node, so only the name can save us. Both spellings
        // must, because which one an audio server records is not ours to
        // choose.
        QVERIFY2(!streamIsForeign(
                     streamProps(-1, QStringLiteral("lightning-matrix")),
                     4242, ours),
                 "the binary name is what PipeWire actually records, and it "
                 "was not being matched");
        QVERIFY(!streamIsForeign(
            streamProps(-1, QStringLiteral("matrix-client")), 4242, ours));
        // Case still does not matter, and a genuinely different application
        // is still captured.
        QVERIFY(!streamIsForeign(
            streamProps(-1, QStringLiteral("Lightning-Matrix")), 4242, ours));
        QVERIFY(streamIsForeign(
            streamProps(-1, QStringLiteral("Firefox")), 4242, ours));
    }

    void anotherApplicationIsCaptured()
    {
        const qint64 ours = 4242;
        QVERIFY(streamIsForeign(streamProps(99, QStringLiteral("Firefox")),
                                ours, QStringList{QStringLiteral("Lightning")}));
        // A node with no pid and a name that is not ours is still theirs.
        QVERIFY(streamIsForeign(streamProps(-1, QStringLiteral("mpv")), ours,
                                QStringList{QStringLiteral("Lightning")}));
    }

    // A stream we cannot target cannot be captured: `pipewiresrc` resolves a
    // stream node only by `object.serial` (given a name it carries silence),
    // so an absent or malformed serial is refused.
    void aStreamWithoutAUsableSerialIsRefused()
    {
        QVariantMap noSerial = streamProps(99, QStringLiteral("Firefox"));
        noSerial.remove(QStringLiteral("object.serial"));
        QVERIFY(!streamIsForeign(noSerial, 1, QStringList{QStringLiteral("Lightning")}));

        // A serial that is not a plain number never reaches the parse string,
        // where "42 ! fakesink" would be a different pipeline.
        for (const QString &bad : { QStringLiteral("42 ! fakesink"),
                                    QStringLiteral("4 2"),
                                    QStringLiteral("abc"),
                                    QStringLiteral("-1"),
                                    QStringLiteral("") }) {
            QVERIFY2(!streamIsForeign(
                         streamProps(99, QStringLiteral("Firefox"), bad), 1,
                         QStringList{QStringLiteral("Lightning")}),
                     qPrintable(QStringLiteral("accepted serial %1").arg(bad)));
        }
    }

    void onlyApplicationOutputStreamsQualify()
    {
        QVariantMap p = streamProps(99, QStringLiteral("Firefox"));
        // PipeWire's own internal split/convert nodes carry audio that some
        // application already produced; taking them as well sends it twice.
        p.insert(QStringLiteral("media.class"),
                 QStringLiteral("Stream/Output/Audio/Internal"));
        QVERIFY(!streamIsForeign(p, 1, QStringList{QStringLiteral("Lightning")}));
        p.insert(QStringLiteral("media.class"), QStringLiteral("Audio/Sink"));
        QVERIFY(!streamIsForeign(p, 1, QStringList{QStringLiteral("Lightning")}));
        p.insert(QStringLiteral("media.class"),
                 QStringLiteral("Stream/Input/Audio"));
        QVERIFY(!streamIsForeign(p, 1, QStringList{QStringLiteral("Lightning")}));
    }

    void loopbackPlumbingIsNotAnApplication()
    {
        QVariantMap p = streamProps(99, QStringLiteral("pw-loopback"));
        p.insert(QStringLiteral("node.link-group"),
                 QStringLiteral("loopback-1234"));
        QVERIFY(!streamIsForeign(p, 1, QStringList{QStringLiteral("Lightning")}));
    }

    // A share started before anything is playing must still hand the Opus
    // encoder a timeline, or the track publishes and carries nothing.
    void theDescriptionAlwaysCarriesASilenceFloor()
    {
        const QString empty = mixedSourceDescription({});
        QVERIFY(empty.contains(QLatin1String("audiotestsrc")));
        QVERIFY(empty.contains(QLatin1String("wave=silence")));
        QVERIFY(empty.contains(QLatin1String("audiomixer")));
        QVERIFY(!empty.contains(QLatin1String("pipewiresrc")));

        Stream s;
        s.serial = QStringLiteral("77");
        const QString one = mixedSourceDescription({ s });
        QVERIFY(one.contains(QLatin1String("wave=silence")));
        QVERIFY(one.contains(QLatin1String("target-object=77")));
    }

    // The mixer's output must be the last chain: the caller appends its
    // encoder with a bare `! `, and gst_parse continues the last-written
    // chain.
    void theMixerOutputIsTheLastChain()
    {
        Stream a;
        a.serial = QStringLiteral("1");
        Stream b;
        b.serial = QStringLiteral("2");
        const QString d = mixedSourceDescription({ a, b });
        const QStringList chains = d.split(QLatin1Char('\n'));
        QVERIFY(!chains.isEmpty());
        QCOMPARE(chains.last().trimmed(),
                 mixerElementName() + QLatin1Char('.'));
        // Every source feeds the mixer rather than dangling as a second
        // unlinked src pad, which would be ghosted out of the bin instead.
        int feeders = 0;
        for (const QString &c : chains) {
            if (c.contains(QLatin1String("pipewiresrc"))
                || c.contains(QLatin1String("audiotestsrc"))) {
                QVERIFY(c.trimmed().endsWith(mixerElementName()
                                             + QLatin1Char('.')));
                ++feeders;
            }
        }
        QCOMPARE(feeders, 3); // silence floor + two applications
    }

    // `min-buffers` is pinned, never inherited (the default changed from 8 to
    // 1 between gst-plugin-pipewire 1.4 and 1.6). `on-disconnect=eos` lets a
    // departing application retire its own branch.
    void everyApplicationBranchPinsWhatItMustNotInherit()
    {
        Stream s;
        s.serial = QStringLiteral("9551");
        const QString branch = applicationBranchDescription(s, 0);
        QVERIFY(branch.contains(QLatin1String("min-buffers=1")));
        QVERIFY(branch.contains(QLatin1String("on-disconnect=eos")));
        QVERIFY(branch.contains(QLatin1String("target-object=9551")));
        // `do-timestamp` puts the buffers on the pipeline's running time, and
        // a leaky queue lets a slow branch drop its own buffers instead of
        // back-pressuring the shared mixer.
        QVERIFY(branch.contains(QLatin1String("do-timestamp=true")));
        QVERIFY(branch.contains(QLatin1String("leaky=downstream")));
        QVERIFY(applicationBranchDescription(Stream{}, 0).isEmpty());
        // The serial is also checked here: these builders are public and take
        // a caller-built Stream.
        Stream hostile;
        hostile.serial = QStringLiteral("1 ! fakesink");
        QVERIFY(applicationBranchDescription(hostile, 0).isEmpty());
        QVERIFY(!mixedSourceDescription({ hostile })
                     .contains(QLatin1String("fakesink")));
    }

    void branchNamesAreUniquePerIndex()
    {
        Stream s;
        s.serial = QStringLiteral("5");
        QVERIFY(applicationBranchDescription(s, 0)
                != applicationBranchDescription(s, 1));
    }

    void propertiesAreReadIntoAStream()
    {
        const Stream s =
            streamFromProperties(streamProps(1234, QStringLiteral("mpv"),
                                             QStringLiteral("9551")));
        QCOMPARE(s.serial, QStringLiteral("9551"));
        QCOMPARE(s.appName, QStringLiteral("mpv"));
        QCOMPARE(s.pid, qint64(1234));
    }

    // ── Choosing applications (2026-10-07) ──

    // An application is chosen by one key every stream of it shares, so a
    // tab Firefox opens a minute later is in the share too. The application's
    // own name first (what pavucontrol shows), then its binary, then the
    // node; case does not split one application in two.
    void everyStreamOfAnApplicationSharesOneKey()
    {
        QVariantMap first = streamProps(10, QStringLiteral("Firefox"),
                                        QStringLiteral("70"));
        first.insert(QStringLiteral("node.name"),
                     QStringLiteral("Firefox stream 1"));
        QVariantMap second = streamProps(10, QStringLiteral("firefox"),
                                         QStringLiteral("71"));
        second.insert(QStringLiteral("node.name"),
                      QStringLiteral("Firefox stream 2"));
        QCOMPARE(streamFromProperties(first).appKey, QStringLiteral("firefox"));
        QCOMPARE(streamFromProperties(second).appKey,
                 streamFromProperties(first).appKey);

        // No application name: the binary, then the node name.
        QVariantMap bare;
        bare.insert(QStringLiteral("media.class"),
                    QStringLiteral("Stream/Output/Audio"));
        bare.insert(QStringLiteral("object.serial"), QStringLiteral("5"));
        bare.insert(QStringLiteral("application.process.binary"),
                    QStringLiteral("MPV"));
        bare.insert(QStringLiteral("node.name"), QStringLiteral("node-x"));
        QCOMPARE(streamFromProperties(bare).appKey, QStringLiteral("mpv"));
        bare.remove(QStringLiteral("application.process.binary"));
        QCOMPARE(streamFromProperties(bare).appKey, QStringLiteral("node-x"));
        QCOMPARE(applicationKeyFor(QString(), QString(), QString()), QString());

        // The icon is read when the node names one.
        QVariantMap icon = streamProps(10, QStringLiteral("Firefox"));
        icon.insert(QStringLiteral("application.icon-name"),
                    QStringLiteral("firefox"));
        QCOMPARE(streamFromProperties(icon).iconName, QStringLiteral("firefox"));
    }

    void aSelectionTakesExactlyWhatWasChosen()
    {
        Stream firefox;
        firefox.serial = QStringLiteral("1");
        firefox.appKey = QStringLiteral("firefox");
        Stream mpv;
        mpv.serial = QStringLiteral("2");
        mpv.appKey = QStringLiteral("mpv");
        Stream keyless;
        keyless.serial = QStringLiteral("3");

        Selection apps;
        apps.mode = Mode::Apps;
        apps.keys = { QStringLiteral("Firefox") };   // case-insensitive
        QVERIFY(apps.wants(firefox));
        QVERIFY2(!apps.wants(mpv), "an application nobody chose is captured");
        QVERIFY2(!apps.wants(keyless),
                 "a stream with no identity matched a choice");
        QVERIFY(apps.capturesAnything());

        // Applications mode with nothing chosen captures nothing: it must not
        // read as "everything".
        Selection none;
        none.mode = Mode::Apps;
        QVERIFY(!none.wants(firefox));
        QVERIFY(!none.capturesAnything());

        Selection system;
        system.mode = Mode::System;
        QVERIFY(system.wants(firefox) && system.wants(mpv));
        QVERIFY(system.capturesAnything());

        Selection off;
        off.mode = Mode::Off;
        QVERIFY(!off.wants(firefox));
        QVERIFY(!off.capturesAnything());

        // Order is not meaning.
        Selection a;
        a.mode = Mode::Apps;
        a.keys = { QStringLiteral("x"), QStringLiteral("y") };
        Selection b = a;
        b.keys = { QStringLiteral("y"), QStringLiteral("x") };
        QVERIFY(a == b);
        b.keys = { QStringLiteral("x") };
        QVERIFY(a != b);
    }

    // An enum is not a quantity: a value a newer build wrote must not be
    // clamped onto the nearest mode, and capturing nothing is the only safe
    // reading of a mode this build does not know.
    void anUnknownModeReadsAsOff()
    {
        QCOMPARE(modeFromInt(0), Mode::Off);
        QCOMPARE(modeFromInt(1), Mode::System);
        QCOMPARE(modeFromInt(2), Mode::Apps);
        QCOMPARE(modeFromInt(3), Mode::Off);
        QCOMPARE(modeFromInt(-1), Mode::Off);
    }

    void streamsAreGroupedPerApplication()
    {
        Stream a1;
        a1.serial = QStringLiteral("1");
        a1.appKey = QStringLiteral("firefox");
        a1.appName = QStringLiteral("Firefox");
        a1.active = false;
        Stream b;
        b.serial = QStringLiteral("2");
        b.appKey = QStringLiteral("mpv");
        b.binary = QStringLiteral("mpv");
        Stream a2 = a1;
        a2.serial = QStringLiteral("3");
        a2.active = true;
        a2.iconName = QStringLiteral("firefox");
        const QList<Application> apps = groupByApplication({ a1, b, a2 });
        QCOMPARE(apps.size(), 2);
        QCOMPARE(apps.at(0).key, QStringLiteral("firefox"));
        QCOMPARE(apps.at(0).label, QStringLiteral("Firefox"));
        QCOMPARE(apps.at(0).streams, 2);
        QVERIFY(apps.at(0).active);   // any of its streams playing
        QCOMPARE(apps.at(0).iconName, QStringLiteral("firefox"));
        QCOMPARE(apps.at(1).label, QStringLiteral("mpv"));
    }

    // Before PipeWire 1.6 pipewiresrc has no `on-disconnect`, and naming a
    // missing property fails the whole parse. Requiring it switched
    // per-application capture off in every package (the AppImage bundles
    // 1.4.2) and on every distribution older than 1.6.
    void aBranchIsBuiltWithoutOnDisconnectWhereThePluginLacksIt()
    {
        Stream s;
        s.serial = QStringLiteral("9551");
        BranchOptions legacy;
        legacy.retireOnDisconnect = false;
        const QString branch = applicationBranchDescription(s, 0, legacy);
        QVERIFY(!branch.isEmpty());
        QVERIFY2(!branch.contains(QLatin1String("on-disconnect")),
                 "a pre-1.6 branch names on-disconnect, which that "
                 "pipewiresrc does not have");
        // Still pinned: min-buffers and the serial.
        QVERIFY(branch.contains(QLatin1String("min-buffers=1")));
        QVERIFY(branch.contains(QLatin1String("target-object=9551")));
        QVERIFY(mixedSourceDescription({ s }, legacy)
                    .contains(QLatin1String("target-object=9551")));
        QVERIFY(!mixedSourceDescription({ s }, legacy)
                     .contains(QLatin1String("on-disconnect")));
    }

    // Deselecting an application mid-share mutes its branch: no linked pad is
    // ever unlinked on a live pipeline (the unpublish deadlock). So every
    // branch ends on its own named volume.
    void everyBranchEndsOnItsOwnVolume()
    {
        Stream s;
        s.serial = QStringLiteral("77");
        const QString live = applicationBranchDescription(s, 3);
        QVERIFY2(live.trimmed().endsWith(
                     QStringLiteral("volume name=%1 mute=false")
                         .arg(branchVolumeName(3))),
                 qPrintable(live));
        const QString muted = applicationBranchDescription(s, 3, {}, true);
        QVERIFY(muted.trimmed().endsWith(QLatin1String("mute=true")));
        QVERIFY(branchVolumeName(3) != branchVolumeName(4));
    }

    // Windows: one process-loopback capture per chosen application's session,
    // including its children (a browser plays from a child process).
    void aWindowsBranchCapturesOneProcessTree()
    {
        Stream s;
        s.serial = QStringLiteral("4321");   // the session's pid
        BranchOptions wasapi;
        wasapi.capture = Capture::WasapiProcess;
        wasapi.retireOnDisconnect = false;
        const QString branch = applicationBranchDescription(s, 2, wasapi);
        QVERIFY(branch.startsWith(QLatin1String(
            "wasapi2src name=shareapp2 loopback-mode=include-process-tree "
            "loopback-target-pid=4321")));
        QVERIFY(!branch.contains(QLatin1String("pipewiresrc")));
        QVERIFY(branch.contains(QLatin1String("volume name=shareappvol2")));
        // The pid is a parse-string value too.
        Stream hostile;
        hostile.serial = QStringLiteral("4321 ! fakesink");
        QVERIFY(applicationBranchDescription(hostile, 0, wasapi).isEmpty());
    }

    // include-process-tree on a parent already captures its children; a
    // second branch for a child would play it twice.
    void aProcessInsideAnotherChosenTreeIsNotCapturedTwice()
    {
        // 100 -> 200 -> 300; 400 unrelated; 500's parent is not listed.
        const QHash<qint64, qint64> parents{
            { 200, 100 }, { 300, 200 }, { 100, 1 }, { 400, 1 }, { 500, 99 },
        };
        QCOMPARE(dropDescendants({ 300, 100, 400, 500 }, parents),
                 (QList<qint64>{ 100, 400, 500 }));
        // A stale snapshot with a cycle must terminate.
        const QHash<qint64, qint64> cycle{ { 7, 8 }, { 8, 7 } };
        QCOMPARE(dropDescendants({ 7 }, cycle), (QList<qint64>{ 7 }));
        QCOMPARE(dropDescendants({ 9, 9 }, {}), (QList<qint64>{ 9 }));
    }

    // A share carrying silence must be distinguishable from one carrying
    // sound, so the track can carry a level meter, where the build has one.
    void theLevelMeterIsInTheTrackOnlyWhenAsked()
    {
        const QString src = QStringLiteral("audiotestsrc name=sharesrc");
        QVERIFY(!encodedTrackDescription(src, 1).contains(levelElementName()));
        const QString with = encodedTrackDescription(src, 1, true);
        QVERIFY(with.contains(QStringLiteral("level name=%1")
                                  .arg(levelElementName())));
        // Upstream of the encoder: it measures what is sent, not RTP.
        QVERIFY(with.indexOf(levelElementName())
                < with.indexOf(QLatin1String("opusenc")));
    }

    // ── A running share, as a plan (2026-10-07 review) ──

    void aStreamIsKeyedBySerialAndWherePidsRepeatByStartTime()
    {
        QCOMPARE(stream(QStringLiteral("42"), QStringLiteral("a")).id(),
                 QStringLiteral("42"));
        QCOMPARE(stream(QStringLiteral("42"), QStringLiteral("a"), 42, 777).id(),
                 QStringLiteral("42@777"));
        QVERIFY(stream(QStringLiteral("42"), QStringLiteral("a"), 42, 1).id()
                != stream(QStringLiteral("42"), QStringLiteral("a"), 42, 2).id());
        QCOMPARE(Stream{}.id(), QString());
    }

    // PipeWire: a branch is one stream node, gone when its stream is.
    void aPipeWireScanAddsChosenMutesDeselectedAndRetiresTheGone()
    {
        ScanInput in;
        in.options.capture = Capture::PipeWire;
        in.selection.mode = Mode::Apps;
        in.selection.keys = { QStringLiteral("a") };
        in.live = { stream(QStringLiteral("1"), QStringLiteral("a")),
                    stream(QStringLiteral("2"), QStringLiteral("b")) };
        ScanPlan plan = planScan(in);
        QCOMPARE(plan.add.size(), 1);
        QCOMPARE(plan.add.first().serial, QStringLiteral("1"));
        QCOMPARE(plan.carried, QStringList{ QStringLiteral("a") });
        QVERIFY(plan.retire.isEmpty());

        // Built; now b is chosen instead: a is muted (never unlinked), b added.
        in.records.insert(QStringLiteral("1"), built(0, QStringLiteral("a")));
        in.liveBranches = 1;
        in.selection.keys = { QStringLiteral("b") };
        plan = planScan(in);
        QCOMPARE(plan.mute.size(), 1);
        QCOMPARE(plan.mute.first().first, QStringLiteral("1"));
        QVERIFY(plan.mute.first().second);
        QCOMPARE(plan.add.size(), 1);
        QCOMPARE(plan.add.first().serial, QStringLiteral("2"));
        QCOMPARE(plan.carried, QStringList{ QStringLiteral("b") });

        // a's stream ends: retired, not muted again, not carried.
        in.live = { stream(QStringLiteral("2"), QStringLiteral("b")) };
        in.records.insert(QStringLiteral("2"), built(1, QStringLiteral("b")));
        in.records[QStringLiteral("1")].muted = true;
        in.liveBranches = 2;
        plan = planScan(in);
        QCOMPARE(plan.retire, QStringList{ QStringLiteral("1") });
        QVERIFY(plan.add.isEmpty());
        QVERIFY(plan.mute.isEmpty());
    }

    // H1: Windows process loopback follows the PROCESS. A session that
    // expires (a paused browser) is no reason to retire; only the process
    // exiting is, and a process that comes back gets a branch again.
    void aWindowsBranchLivesAsLongAsItsProcessNotItsSession()
    {
        ScanInput in;
        in.options.capture = Capture::WasapiProcess;
        in.options.followsProcess = true;
        in.selection.mode = Mode::Apps;
        in.selection.keys = { QStringLiteral("chrome.exe") };
        const Stream chrome =
            stream(QStringLiteral("500"), QStringLiteral("chrome.exe"), 500, 9);
        in.records.insert(chrome.id(),
                          built(3, QStringLiteral("chrome.exe"), 500, 9));
        in.liveBranches = 1;

        // The session expired; the process runs: nothing happens.
        in.live = {};
        in.running = { chrome.id() };
        ScanPlan plan = planScan(in);
        QVERIFY2(plan.retire.isEmpty(),
                 "a paused browser's capture was retired; it would never be "
                 "captured again");
        QVERIFY(plan.add.isEmpty());
        QCOMPARE(plan.carried, QStringList{ QStringLiteral("chrome.exe") });

        // The session is back: the same branch, no second one.
        in.live = { chrome };
        plan = planScan(in);
        QVERIFY(plan.add.isEmpty() && plan.retire.isEmpty());

        // The process exited: retired.
        in.live = {};
        in.running = {};
        plan = planScan(in);
        QCOMPARE(plan.retire, QStringList{ chrome.id() });

        // Its record is forgotten once reclaimed, so a returning process (or
        // the same pid reused, a different id) is captured again.
        in.records.clear();
        in.liveBranches = 0;
        in.live = { stream(QStringLiteral("500"), QStringLiteral("chrome.exe"),
                           500, 10) };
        plan = planScan(in);
        QCOMPARE(plan.add.size(), 1);
        QCOMPARE(plan.add.first().id(), QStringLiteral("500@10"));
    }

    void aFailedBranchIsNeverRetried()
    {
        ScanInput in;
        in.selection.mode = Mode::System;
        in.live = { stream(QStringLiteral("7"), QStringLiteral("x")) };
        BranchRecord failed;
        failed.appKey = QStringLiteral("x");
        failed.failed = true;
        in.records.insert(QStringLiteral("7"), failed);
        const ScanPlan plan = planScan(in);
        QVERIFY(plan.add.isEmpty());
        QVERIFY(plan.retire.isEmpty());
        QVERIFY(plan.carried.isEmpty());
    }

    void theBranchCapIsReportedNotSilent()
    {
        ScanInput in;
        in.selection.mode = Mode::System;
        in.cap = 2;
        in.live = { stream(QStringLiteral("1"), QStringLiteral("a")),
                    stream(QStringLiteral("2"), QStringLiteral("b")),
                    stream(QStringLiteral("3"), QStringLiteral("c")) };
        ScanPlan plan = planScan(in);
        QCOMPARE(plan.add.size(), 2);
        QVERIFY2(plan.limitReached, "an application was left out silently");
        QVERIFY(!plan.carried.contains(QStringLiteral("c")));
        // Branches taken out free their places.
        in.liveBranches = 0;
        in.cap = 3;
        plan = planScan(in);
        QCOMPARE(plan.add.size(), 3);
        QVERIFY(!plan.limitReached);
    }

    // H2: a process Lightning runs inside (explorer.exe, a terminal) is
    // listed but never captured: include-process-tree on it would capture the
    // call itself.
    void aProcessLightningRunsInsideIsNeverCaptured()
    {
        // explorer(10) -> terminal(20) -> lightning(30); chrome(40) under 10.
        const QHash<qint64, qint64> parents{
            { 20, 10 }, { 30, 20 }, { 40, 10 }, { 10, 4 },
        };
        QList<Stream> listed{
            stream(QStringLiteral("10"), QStringLiteral("explorer.exe"), 10),
            stream(QStringLiteral("20"), QStringLiteral("terminal.exe"), 20),
            stream(QStringLiteral("40"), QStringLiteral("chrome.exe"), 40),
        };
        markProcessesContainingUs(listed, parents, 30);
        QVERIFY(listed.at(0).containsUs);
        QVERIFY(listed.at(1).containsUs);
        QVERIFY(!listed.at(2).containsUs);
        QCOMPARE(ancestorsOf(30, parents),
                 (QSet<qint64>{ 20, 10, 4 }));

        // The listing keeps all three (so the UI can say why), marked.
        const QList<Application> apps = groupByApplication(listed);
        QCOMPARE(apps.size(), 3);
        QVERIFY(apps.at(0).containsUs);

        // Chosen or not, the ancestors get no branch; chrome, under explorer,
        // is NOT dropped as explorer's descendant, because explorer is never
        // captured.
        ScanInput in;
        in.options.capture = Capture::WasapiProcess;
        in.options.followsProcess = true;
        in.parents = parents;
        in.selection.mode = Mode::Apps;
        in.selection.keys = { QStringLiteral("explorer.exe"),
                              QStringLiteral("terminal.exe"),
                              QStringLiteral("chrome.exe") };
        in.live = listed;
        const ScanPlan plan = planScan(in);
        QCOMPARE(plan.add.size(), 1);
        QCOMPARE(plan.add.first().appKey, QStringLiteral("chrome.exe"));
        Selection system;
        system.mode = Mode::System;
        QVERIFY(!system.wants(listed.at(0)));
    }

    // dropDescendants applies among the CHOSEN only, at build time: a chosen
    // child under a chosen parent is captured once (by the parent), and a
    // standing child branch is muted when its parent becomes chosen.
    void aChosenChildOfAChosenParentIsCapturedOnce()
    {
        const QHash<qint64, qint64> parents{ { 200, 100 } };
        ScanInput in;
        in.options.capture = Capture::WasapiProcess;
        in.options.followsProcess = true;
        in.parents = parents;
        in.selection.mode = Mode::Apps;
        in.selection.keys = { QStringLiteral("child.exe") };
        const Stream child =
            stream(QStringLiteral("200"), QStringLiteral("child.exe"), 200, 1);
        const Stream parent =
            stream(QStringLiteral("100"), QStringLiteral("parent.exe"), 100, 1);
        in.live = { parent, child };
        ScanPlan plan = planScan(in);
        // Only the child is chosen: it is captured; the parent's presence in
        // the LISTING drops nothing.
        QCOMPARE(plan.add.size(), 1);
        QCOMPARE(plan.add.first().appKey, QStringLiteral("child.exe"));

        // Now the parent is chosen too: the standing child branch is muted,
        // the parent added.
        in.records.insert(child.id(),
                          built(0, QStringLiteral("child.exe"), 200, 1));
        in.liveBranches = 1;
        in.selection.keys << QStringLiteral("parent.exe");
        plan = planScan(in);
        QCOMPARE(plan.add.size(), 1);
        QCOMPARE(plan.add.first().appKey, QStringLiteral("parent.exe"));
        QCOMPARE(plan.mute.size(), 1);
        QVERIFY(plan.mute.first().second);
        QVERIFY(plan.carried.contains(QStringLiteral("child.exe")) == false);
    }

    // L-c: an ancestor the cap keeps out must not mute the chosen descendant
    // that is carrying its sound.
    void aCappedAncestorDoesNotMuteItsDescendant()
    {
        const QHash<qint64, qint64> parents{ { 200, 100 } };
        ScanInput in;
        in.options.capture = Capture::WasapiProcess;
        in.options.followsProcess = true;
        in.parents = parents;
        in.cap = 1;
        in.selection.mode = Mode::Apps;
        in.selection.keys = { QStringLiteral("child.exe"),
                              QStringLiteral("parent.exe") };
        const Stream child =
            stream(QStringLiteral("200"), QStringLiteral("child.exe"), 200, 1);
        const Stream parent =
            stream(QStringLiteral("100"), QStringLiteral("parent.exe"), 100, 1);
        in.live = { child, parent };
        in.records.insert(child.id(),
                          built(0, QStringLiteral("child.exe"), 200, 1));
        in.liveBranches = 1;
        const ScanPlan plan = planScan(in);
        QVERIFY(plan.add.isEmpty());
        QVERIFY(plan.limitReached);
        QVERIFY2(plan.mute.isEmpty(),
                 "the child was muted for a parent that got no branch: "
                 "silence");
        QCOMPARE(plan.carried, QStringList{ QStringLiteral("child.exe") });
    }

    // N2: a restarted PipeWire numbers its nodes again, so a serial a branch
    // was built for can come back naming ANOTHER application. That branch is
    // not this stream: it is retired, and the new one is captured once the
    // old record is gone.
    void aReusedSerialNamingAnotherAppIsNotTheOldBranch()
    {
        ScanInput in;
        in.selection.mode = Mode::System;
        in.records.insert(QStringLiteral("65"), built(0, QStringLiteral("toneb")));
        in.liveBranches = 1;
        in.live = { stream(QStringLiteral("65"), QStringLiteral("tonec")) };
        ScanPlan plan = planScan(in);
        QCOMPARE(plan.retire, QStringList{ QStringLiteral("65") });
        QVERIFY(plan.add.isEmpty());
        // Same application at the same serial: kept.
        in.live = { stream(QStringLiteral("65"), QStringLiteral("toneb")) };
        plan = planScan(in);
        QVERIFY(plan.retire.isEmpty());
        // Once taken out, the new stream is captured.
        in.records.clear();
        in.liveBranches = 0;
        in.live = { stream(QStringLiteral("65"), QStringLiteral("tonec")) };
        plan = planScan(in);
        QCOMPARE(plan.add.size(), 1);
    }

    void iconNamesAndBranchNamesAreChecked()
    {
        QCOMPARE(sanitizedIconName(QStringLiteral("org.mozilla.firefox")),
                 QStringLiteral("org.mozilla.firefox"));
        QCOMPARE(sanitizedIconName(QStringLiteral("../../etc/passwd")),
                 QString());
        QCOMPARE(sanitizedIconName(QStringLiteral("a b")), QString());
        QCOMPARE(sanitizedIconName(QString(65, QLatin1Char('a'))), QString());
        QVariantMap p = streamProps(1, QStringLiteral("X"));
        p.insert(QStringLiteral("application.icon-name"),
                 QStringLiteral("<img src=x>"));
        QCOMPARE(streamFromProperties(p).iconName, QString());

        QCOMPARE(branchIndexFromElementName(QStringLiteral("shareapp12")), 12);
        QCOMPARE(branchIndexFromElementName(QStringLiteral("shareappbin3")), 3);
        QCOMPARE(branchIndexFromElementName(QStringLiteral("shareappvol0")), 0);
        QCOMPARE(branchIndexFromElementName(QStringLiteral("sharesrc")), -1);
        QCOMPARE(branchIndexFromElementName(QStringLiteral("shareapp")), -1);
        QCOMPARE(branchSourceName(4), QStringLiteral("shareapp4"));
        QCOMPARE(branchBinName(4), QStringLiteral("shareappbin4"));
    }

    // PipeWire records a native client under its binary as well.
    void ourBinaryNameExcludesOurOwnStream()
    {
        QVariantMap p = streamProps(-1, QStringLiteral("Some Player"));
        p.insert(QStringLiteral("application.process.binary"),
                 QStringLiteral("lightning-matrix"));
        QVERIFY(!streamIsForeign(p, 4242,
                                 { QStringLiteral("lightning-matrix") }));
    }
};

QTEST_APPLESS_MAIN(ShareAudioSourcesTest)
#include "ShareAudioSourcesTest.moc"
