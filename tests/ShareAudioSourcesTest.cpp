#include <QTest>
#include <QVariantMap>

#include "calls/ShareAudioSources.h"

using namespace lightning::shareaudio;

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
};

QTEST_APPLESS_MAIN(ShareAudioSourcesTest)
#include "ShareAudioSourcesTest.moc"
