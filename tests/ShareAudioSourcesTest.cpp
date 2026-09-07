#include <QTest>
#include <QVariantMap>

#include "calls/ShareAudioSources.h"

using namespace lightning::shareaudio;

// The decision that removes the echo.
//
// Sharing with sound used to capture the default sink's MONITOR — the whole
// post-mix output — so Lightning's own playback of the other participants
// went straight back out and they heard themselves (tester report
// 2026-09-06 §3, confirmed twice). A monitor cannot leave a contributor out;
// the fix is to capture each playing application on its own and simply not
// capture ourselves.
//
// What is tested here is the DECISION and the pipeline SHAPE: which streams
// are eligible, and what the description built from them contains. What is
// NOT tested here, and is not tested anywhere: the dynamic half — the rescan
// bookkeeping, the branch cap, the poll's own start and stop — which needs a
// live PipeWire graph, and the echo removal itself, which needs a far end.
// Both are reported NOT TESTED rather than implied by this file's existence.
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
    // THE CASE THE DEFECT IS. Our own playback must never be captured,
    // whichever way the node identifies itself.
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

    // THE BELT HAD THE WRONG NAME ON IT, AND SO COULD NEVER FASTEN.
    //
    // The name check exists for one case: a node of ours that carries no
    // `application.process.id`, where the pid guard cannot fire. It used to
    // be handed QCoreApplication::applicationName() alone, which is
    // "matrix-client" (src/main.cpp), while a live share on 2026-09-07
    // logged its own sources as `app= "lightning-matrix"` — the BINARY name.
    // So the one process the belt existed to exclude was the one name it did
    // not have, and had the pid ever gone missing the echo would have come
    // straight back with a check in place that looked like it was working.
    //
    // FAIL-ON-OLD: with the parameter narrowed back to a single name, the
    // binary-name case below captures our own playback.
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

    // A stream we cannot TARGET is not a stream we can capture. `pipewiresrc`
    // resolves a stream node by `object.serial` and by nothing else — given a
    // name it runs happily and carries digital silence (measured) — so an
    // absent or malformed serial must be refused rather than guessed at.
    void aStreamWithoutAUsableSerialIsRefused()
    {
        QVariantMap noSerial = streamProps(99, QStringLiteral("Firefox"));
        noSerial.remove(QStringLiteral("object.serial"));
        QVERIFY(!streamIsForeign(noSerial, 1, QStringList{QStringLiteral("Lightning")}));

        // And a serial that is not a plain number never reaches a parse
        // string: it is interpolated into gst_parse_bin_from_description,
        // where "42 ! fakesink" would be a different pipeline, not a bad
        // target.
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

    // THE FLOOR. A share started before anything is playing — "share, then
    // press play" — must still hand the Opus encoder a timeline, or the
    // track publishes and then carries nothing, which is worse than the echo
    // it replaces.
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

    // The mixer's own output has to be the LAST chain in the description,
    // because the caller appends its encoder with a bare `! `: gst_parse
    // continues whichever chain was written last, so a source written after
    // the mixer would quietly swallow the encoder.
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

    // §16: `min-buffers` is PINNED, never inherited — the default moved from
    // 8 to 1 between gst-plugin-pipewire 1.4 and 1.6 and 8 cannot negotiate
    // against a source offering fewer. And `on-disconnect=eos` is what lets a
    // departing application retire its own branch instead of leaving a silent
    // pad the mixer waits on for the rest of the share.
    void everyApplicationBranchPinsWhatItMustNotInherit()
    {
        Stream s;
        s.serial = QStringLiteral("9551");
        const QString branch = applicationBranchDescription(s, 0);
        QVERIFY(branch.contains(QLatin1String("min-buffers=1")));
        QVERIFY(branch.contains(QLatin1String("on-disconnect=eos")));
        QVERIFY(branch.contains(QLatin1String("target-object=9551")));
        // Both load-bearing for a live source joining a running aggregator:
        // `do-timestamp` puts the buffers on the pipeline's running time (an
        // application that started ten minutes into the share has its own
        // idea of time), and a leaky queue means one slow branch drops its
        // own buffers instead of back-pressuring the mixer everyone shares.
        QVERIFY(branch.contains(QLatin1String("do-timestamp=true")));
        QVERIFY(branch.contains(QLatin1String("leaky=downstream")));
        QVERIFY(applicationBranchDescription(Stream{}, 0).isEmpty());
        // AND THE SERIAL IS CHECKED HERE, not only where a Stream is read
        // from PipeWire. These builders are public and take a caller-built
        // Stream, so a value that would change the parse string rather than
        // the target must be refused at the point it is interpolated.
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
};

QTEST_APPLESS_MAIN(ShareAudioSourcesTest)
#include "ShareAudioSourcesTest.moc"
