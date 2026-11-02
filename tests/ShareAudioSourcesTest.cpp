#include <QTest>
#include <QVariantMap>

#include "calls/RtpVp8Payloader.h"
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
}

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
    void macCaptureRequiresNativeSourceAndExcludesOurPlayback()
    {
        ExclusionElements elements;
        QVERIFY(!systemExclusionFor(Platform::MacOS, elements).possible);
        QCOMPARE(unavailabilityFor(Platform::MacOS, elements, false), Unavailability::MissingPlugin);
        QVERIFY(!systemCaptureFor(Platform::MacOS, false, false, 42).excludesUs);
        elements.screenCaptureKit = true;
        QVERIFY(systemExclusionFor(Platform::MacOS, elements).possible);
        QCOMPARE(unavailabilityFor(Platform::MacOS, elements, false), Unavailability::None);
        const auto capture = systemCaptureFor(Platform::MacOS, false, true, 42);
        QVERIFY(capture.excludesUs);
        QCOMPARE(capture.description, QStringLiteral("lightningmacsystemaudiosrc name=sharesrc"));
    }
    void ourOwnPlaybackIsNeverCaptured()
    {
        const qint64 ours = 4242;
        QVERIFY(!streamIsForeign(streamProps(ours, QStringLiteral("Firefox")),
                                 ours, QStringList{QStringLiteral("Lightning")}));
        QVERIFY(!streamIsForeign(streamProps(-1, QStringLiteral("Lightning")),
                                 ours, QStringList{QStringLiteral("Lightning")}));
        QVERIFY(!streamIsForeign(streamProps(-1, QStringLiteral("lightning")),
                                 ours, QStringList{QStringLiteral("Lightning")}));
    }

    void everySpellingOfOurOwnNameIsExcluded()
    {
        const QStringList ours{ QStringLiteral("matrix-client"),
                                QStringLiteral("lightning-matrix") };
        QVERIFY2(!streamIsForeign(
                     streamProps(-1, QStringLiteral("lightning-matrix")),
                     4242, ours),
                 "the binary name is what PipeWire actually records, and it "
                 "was not being matched");
        QVERIFY(!streamIsForeign(
            streamProps(-1, QStringLiteral("matrix-client")), 4242, ours));
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
        QVERIFY(streamIsForeign(streamProps(-1, QStringLiteral("mpv")), ours,
                                QStringList{QStringLiteral("Lightning")}));
    }

    void aStreamWithoutAUsableSerialIsRefused()
    {
        QVariantMap noSerial = streamProps(99, QStringLiteral("Firefox"));
        noSerial.remove(QStringLiteral("object.serial"));
        QVERIFY(!streamIsForeign(noSerial, 1, QStringList{QStringLiteral("Lightning")}));

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
        int feeders = 0;
        for (const QString &c : chains) {
            if (c.contains(QLatin1String("pipewiresrc"))
                || c.contains(QLatin1String("audiotestsrc"))) {
                QVERIFY(c.trimmed().endsWith(mixerElementName()
                                             + QLatin1Char('.')));
                ++feeders;
            }
        }
        QCOMPARE(feeders, 3);
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
        QVERIFY(branch.contains(QLatin1String("do-timestamp=true")));
        QVERIFY(branch.contains(QLatin1String("leaky=downstream")));
        QVERIFY(applicationBranchDescription(Stream{}, 0).isEmpty());
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
        apps.keys = { QStringLiteral("Firefox") };
        QVERIFY(apps.wants(firefox));
        QVERIFY2(!apps.wants(mpv), "an application nobody chose is captured");
        QVERIFY2(!apps.wants(keyless),
                 "a stream with no identity matched a choice");
        QVERIFY(apps.capturesAnything());

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

        Selection a;
        a.mode = Mode::Apps;
        a.keys = { QStringLiteral("x"), QStringLiteral("y") };
        Selection b = a;
        b.keys = { QStringLiteral("y"), QStringLiteral("x") };
        QVERIFY(a == b);
        b.keys = { QStringLiteral("x") };
        QVERIFY(a != b);
    }

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
        QVERIFY(apps.at(0).active);
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
        QVERIFY(branch.contains(QLatin1String("min-buffers=1")));
        QVERIFY(branch.contains(QLatin1String("target-object=9551")));
        QVERIFY(mixedSourceDescription({ s }, legacy)
                    .contains(QLatin1String("target-object=9551")));
        QVERIFY(!mixedSourceDescription({ s }, legacy)
                     .contains(QLatin1String("on-disconnect")));
    }

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

    void aWindowsBranchCapturesOneProcessTree()
    {
        Stream s;
        s.serial = QStringLiteral("4321");
        BranchOptions wasapi;
        wasapi.capture = Capture::WasapiProcess;
        wasapi.retireOnDisconnect = false;
        const QString branch = applicationBranchDescription(s, 2, wasapi);
        QVERIFY(branch.startsWith(QLatin1String(
            "wasapi2src name=shareapp2 loopback-mode=include-process-tree "
            "loopback-target-pid=4321")));
        QVERIFY(!branch.contains(QLatin1String("pipewiresrc")));
        QVERIFY(branch.contains(QLatin1String("volume name=shareappvol2")));
        Stream hostile;
        hostile.serial = QStringLiteral("4321 ! fakesink");
        QVERIFY(applicationBranchDescription(hostile, 0, wasapi).isEmpty());
    }

    void aProcessInsideAnotherChosenTreeIsNotCapturedTwice()
    {
        const QHash<qint64, qint64> parents{
            { 200, 100 }, { 300, 200 }, { 100, 1 }, { 400, 1 }, { 500, 99 },
        };
        QCOMPARE(dropDescendants({ 300, 100, 400, 500 }, parents),
                 (QList<qint64>{ 100, 400, 500 }));
        const QHash<qint64, qint64> cycle{ { 7, 8 }, { 8, 7 } };
        QCOMPARE(dropDescendants({ 7 }, cycle), (QList<qint64>{ 7 }));
        QCOMPARE(dropDescendants({ 9, 9 }, {}), (QList<qint64>{ 9 }));
    }

    void aMutedShareTrackIsBuiltWithItsValveClosedAfterTheMeter()
    {
        const QString src = QStringLiteral("audiotestsrc name=sharesrc");
        const QString open = encodedTrackDescription(src, 1, true);
        QVERIFY2(open.contains(QStringLiteral("valve name=%1 drop=false")
                                   .arg(shareValveElementName())),
                 qPrintable(open));
        const QString muted = encodedTrackDescription(src, 1, true, true);
        QVERIFY2(muted.contains(QStringLiteral("valve name=%1 drop=true")
                                    .arg(shareValveElementName())),
                 qPrintable(muted));
        QVERIFY(muted.indexOf(levelElementName())
                < muted.indexOf(shareValveElementName()));
        QVERIFY(muted.indexOf(shareValveElementName())
                < muted.indexOf(QStringLiteral("opusenc")));
        QVERIFY(muted.contains(QStringLiteral("ssrc=(uint)1")));
        QVERIFY(muted.contains(
            QStringLiteral("mtu=%1").arg(lightning::rtp::kRtpPayloadMtu)));
    }

    void theLevelMeterIsInTheTrackOnlyWhenAsked()
    {
        const QString src = QStringLiteral("audiotestsrc name=sharesrc");
        QVERIFY(!encodedTrackDescription(src, 1).contains(levelElementName()));
        const QString with = encodedTrackDescription(src, 1, true);
        QVERIFY(with.contains(QStringLiteral("level name=%1")
                                  .arg(levelElementName())));
        QVERIFY(with.indexOf(levelElementName())
                < with.indexOf(QLatin1String("opusenc")));
    }

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

        in.live = { stream(QStringLiteral("2"), QStringLiteral("b")) };
        in.records.insert(QStringLiteral("2"), built(1, QStringLiteral("b")));
        in.records[QStringLiteral("1")].muted = true;
        in.liveBranches = 2;
        plan = planScan(in);
        QCOMPARE(plan.retire, QStringList{ QStringLiteral("1") });
        QVERIFY(plan.add.isEmpty());
        QVERIFY(plan.mute.isEmpty());
    }

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

        in.live = {};
        in.running = { chrome.id() };
        ScanPlan plan = planScan(in);
        QVERIFY2(plan.retire.isEmpty(),
                 "a paused browser's capture was retired; it would never be "
                 "captured again");
        QVERIFY(plan.add.isEmpty());
        QCOMPARE(plan.carried, QStringList{ QStringLiteral("chrome.exe") });

        in.live = { chrome };
        plan = planScan(in);
        QVERIFY(plan.add.isEmpty() && plan.retire.isEmpty());

        in.live = {};
        in.running = {};
        plan = planScan(in);
        QCOMPARE(plan.retire, QStringList{ chrome.id() });

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
        in.liveBranches = 0;
        in.cap = 3;
        plan = planScan(in);
        QCOMPARE(plan.add.size(), 3);
        QVERIFY(!plan.limitReached);
    }

    void aProcessLightningRunsInsideIsNeverCaptured()
    {
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

        const QList<Application> apps = groupByApplication(listed);
        QCOMPARE(apps.size(), 3);
        QVERIFY(apps.at(0).containsUs);

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
        QCOMPARE(plan.add.size(), 1);
        QCOMPARE(plan.add.first().appKey, QStringLiteral("child.exe"));

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
        in.live = { stream(QStringLiteral("65"), QStringLiteral("toneb")) };
        plan = planScan(in);
        QVERIFY(plan.retire.isEmpty());
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

    void ourBinaryNameExcludesOurOwnStream()
    {
        QVariantMap p = streamProps(-1, QStringLiteral("Some Player"));
        p.insert(QStringLiteral("application.process.binary"),
                 QStringLiteral("lightning-matrix"));
        QVERIFY(!streamIsForeign(p, 4242,
                                 { QStringLiteral("lightning-matrix") }));
    }

    void theWholeSystemLeavesUsOutOnlyWithEveryPieceItNeeds()
    {
        ExclusionElements pipewire;
        pipewire.audiomixer = true;
        pipewire.pipewireTargetObject = true;
        pipewire.pipewireDeviceProvider = true;
        SystemExclusion v = systemExclusionFor(Platform::Linux, pipewire);
        QVERIFY(v.possible);
        QCOMPARE(v.how, QStringLiteral("per-application capture"));
        QVERIFY(v.missing.isEmpty());

        ExclusionElements appImage0101 = pipewire;
        appImage0101.audiomixer = false;
        v = systemExclusionFor(Platform::Linux, appImage0101);
        QVERIFY2(!v.possible,
                 "a package without audiomixer cannot build the capture that "
                 "leaves us out, and must not be reported as if it could");
        QCOMPARE(v.missing, QStringList{ QStringLiteral("element audiomixer") });

        const struct {
            bool ExclusionElements::*field;
            const char *named;
        } pieces[] = {
            { &ExclusionElements::audiomixer, "audiomixer" },
            { &ExclusionElements::pipewireTargetObject, "target-object" },
            { &ExclusionElements::pipewireDeviceProvider, "device provider" },
        };
        for (const auto &piece : pieces) {
            ExclusionElements without = pipewire;
            without.*piece.field = false;
            const SystemExclusion missing =
                systemExclusionFor(Platform::Linux, without);
            QVERIFY2(!missing.possible, piece.named);
            QCOMPARE(missing.missing.size(), 1);
            QVERIFY2(missing.missing.first().contains(
                         QLatin1String(piece.named)),
                     qPrintable(missing.missing.first()));
            QVERIFY(missing.how.isEmpty());
        }
        ExclusionElements onlyWasapi;
        onlyWasapi.wasapiProcessLoopback = true;
        QVERIFY(!systemExclusionFor(Platform::Linux, onlyWasapi).possible);
    }

    void windowsLeavesUsOutWithProcessLoopbackAlone()
    {
        ExclusionElements windows;
        windows.wasapiProcessLoopback = true;
        SystemExclusion v = systemExclusionFor(Platform::Windows, windows);
        QVERIFY(v.possible);
        QCOMPARE(v.how, QStringLiteral("process loopback"));

        v = systemExclusionFor(Platform::Windows, {});
        QVERIFY(!v.possible);
        QVERIFY(v.missing.join(QString()).contains(
            QLatin1String("process loopback")));
        ExclusionElements linuxOnly;
        linuxOnly.audiomixer = true;
        linuxOnly.pipewireTargetObject = true;
        linuxOnly.pipewireDeviceProvider = true;
        QVERIFY(!systemExclusionFor(Platform::Windows, linuxOnly).possible);

        ExclusionElements everything = linuxOnly;
        everything.wasapiProcessLoopback = true;
        QVERIFY(!systemExclusionFor(Platform::Other, everything).possible);
    }

    void theStatusLineSaysWhichWayItWent()
    {
        const QString pass = QStringLiteral(
            "screen-share sound exclusion capability: available");
        ExclusionElements pipewire;
        pipewire.audiomixer = true;
        pipewire.pipewireTargetObject = true;
        pipewire.pipewireDeviceProvider = true;
        const QString good = systemExclusionStatusLine(
            systemExclusionFor(Platform::Linux, pipewire));
        QVERIFY2(good.startsWith(pass), qPrintable(good));
        QVERIFY(good.contains(QLatin1String("per-application capture")));

        pipewire.audiomixer = false;
        const QString bad = systemExclusionStatusLine(
            systemExclusionFor(Platform::Linux, pipewire));
        QVERIFY2(!bad.contains(pass), qPrintable(bad));
        QVERIFY2(bad.startsWith(QLatin1String(
                     "screen-share sound exclusion capability: unavailable")),
                 qPrintable(bad));
        QVERIFY2(bad.contains(QLatin1String("audiomixer")), qPrintable(bad));
        QVERIFY(!bad.contains(QLatin1Char('\n')));
    }

    void theUnavailableReasonNamesWhatIsMissing()
    {
        ExclusionElements full;
        full.audiomixer = full.pipewireTargetObject
            = full.pipewireDeviceProvider = true;
        QCOMPARE(unavailabilityFor(Platform::Linux, full, true),
                 Unavailability::None);
        QCOMPARE(unavailabilityFor(Platform::Linux, full, false),
                 Unavailability::NoPipeWire);
        ExclusionElements noMixer = full;
        noMixer.audiomixer = false;
        QCOMPARE(unavailabilityFor(Platform::Linux, noMixer, true),
                 Unavailability::MissingPlugin);
        ExclusionElements win;
        QCOMPARE(unavailabilityFor(Platform::Windows, win, false),
                 Unavailability::MissingPlugin);
        win.wasapiSource = true;
        QCOMPARE(unavailabilityFor(Platform::Windows, win, false),
                 Unavailability::OldWindows);
        win.wasapiProcessLoopback = true;
        QCOMPARE(unavailabilityFor(Platform::Windows, win, false),
                 Unavailability::None);
        QCOMPARE(unavailabilityFor(Platform::Other, full, true),
                 Unavailability::NoCapture);
    }

    void systemCaptureNeverFallsBackToTheEndpointMix()
    {
        const auto linuxPlan = systemCaptureFor(Platform::Linux, true, false, 4242);
        QCOMPARE(linuxPlan.kind, SystemCapture::Kind::PerApplication);
        QVERIFY(linuxPlan.excludesUs);
        QVERIFY(linuxPlan.description.isEmpty());

        for (const auto platform : { Platform::Linux, Platform::Windows,
                                     Platform::Other }) {
            const auto unavailable = systemCaptureFor(platform, false, false, 4242);
            QCOMPARE(unavailable.kind, SystemCapture::Kind::Unavailable);
            QVERIFY(unavailable.description.isEmpty());
            QVERIFY(!unavailable.excludesUs);
        }
        const auto windows = systemCaptureFor(Platform::Windows, false, true, 4242);
        QCOMPARE(windows.kind, SystemCapture::Kind::ProcessLoopback);
        QVERIFY(windows.excludesUs);
        QVERIFY(windows.description.contains(
            QStringLiteral("loopback-mode=exclude-process-tree")));
        QVERIFY(windows.description.contains(QStringLiteral("loopback-target-pid=4242")));
        QVERIFY(!windows.description.contains(QStringLiteral("loopback=true")));
        for (qint64 pid : { qint64(-1), qint64(0), qint64(0x100000000ULL) }) {
            const auto invalid = systemCaptureFor(Platform::Windows, false, true, pid);
            QCOMPARE(invalid.kind, SystemCapture::Kind::Unavailable);
            QVERIFY(invalid.description.isEmpty());
        }
    }

    void ourPulsePlaybackStreamIsLeftOutByPid()
    {
        QVariantMap p;
        p.insert(QStringLiteral("media.class"),
                 QStringLiteral("Stream/Output/Audio"));
        p.insert(QStringLiteral("object.serial"), QStringLiteral("73"));
        p.insert(QStringLiteral("client.api"), QStringLiteral("pipewire-pulse"));
        p.insert(QStringLiteral("application.name"), QStringLiteral("Lightning"));
        p.insert(QStringLiteral("node.name"), QStringLiteral("Lightning"));
        p.insert(QStringLiteral("application.process.binary"),
                 QStringLiteral("lightning-matrix"));
        p.insert(QStringLiteral("application.process.id"),
                 QStringLiteral("2740077"));
        QVERIFY(!streamIsForeign(p, 2740077, {}));
        p.insert(QStringLiteral("application.name"), QStringLiteral("Player"));
        p.insert(QStringLiteral("node.name"), QStringLiteral("Player"));
        p.insert(QStringLiteral("application.process.binary"),
                 QStringLiteral("player"));
        QVERIFY(streamIsForeign(p, 4242,
                                { QStringLiteral("lightning-matrix") }));
    }
};

QTEST_APPLESS_MAIN(ShareAudioSourcesTest)
#include "ShareAudioSourcesTest.moc"
