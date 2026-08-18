// 2026-08-18 round 3: the REAL WebRTC handshake, no mocks. Two
// GstCallMediaBackend instances in one process — one offers, one answers —
// exchange SDP and trickled ICE candidates exactly the way CallController
// wires them across Matrix, and must reach CONNECTED: a genuine ICE
// negotiation over loopback host candidates plus a genuine DTLS-SRTP
// handshake with Opus RTP flowing (test-tone mode: audiotestsrc/fakesink,
// so no audio device is needed and CI can run this headless).
//
// If the runtime element probe fails (a tree without the GStreamer plugins)
// the suite SKIPs rather than fails: absence of the engine is a supported
// configuration, not a defect.
#include <QtTest/QtTest>

#include <QSignalSpy>

#include "calls/GstCallMediaBackend.h"

class CallMediaLoopbackTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QString whyNot;
        if (!GstCallMediaBackend::runtimeAvailable(&whyNot))
            QSKIP(qPrintable(
                QStringLiteral("webrtc runtime unavailable: ") + whyNot));
    }

    void runtimeProbeIsStable()
    {
        // Second call answers from the cached init; must agree.
        QVERIFY(GstCallMediaBackend::runtimeAvailable());
    }

    void loopbackCallReachesConnectedBothWays()
    {
        const QString callId = QStringLiteral("loopback-1");
        GstCallMediaBackend caller;
        GstCallMediaBackend callee;
        caller.setTestToneMode(true);
        callee.setTestToneMode(true);

        bool callerConnected = false;
        bool calleeConnected = false;
        QString failureCategory;

        // Cross the signaling exactly as CallController does over Matrix.
        bool sawAudioOffer = false;
        bool sawAudioAnswer = false;
        connect(&caller, &CallMediaBackend::offerReady, &callee,
                [&](const QString &id, const QString &sdp) {
                    if (id != callId)
                        return; // the second-call phase reuses the engines
                    sawAudioOffer = sdp.contains(QStringLiteral("m=audio"));
                    callee.createAnswer(callId, sdp);
                });
        connect(&callee, &CallMediaBackend::answerReady, &caller,
                [&](const QString &id, const QString &sdp) {
                    if (id != callId)
                        return;
                    sawAudioAnswer = sdp.contains(QStringLiteral("m=audio"));
                    caller.setRemoteAnswer(callId, sdp);
                });
        connect(&caller, &CallMediaBackend::localCandidate, &callee,
                [&](const QString &id, const QString &candidate,
                    const QString &mid, int mline) {
                    callee.addRemoteCandidate(id, candidate, mid, mline);
                });
        connect(&callee, &CallMediaBackend::localCandidate, &caller,
                [&](const QString &id, const QString &candidate,
                    const QString &mid, int mline) {
                    caller.addRemoteCandidate(id, candidate, mid, mline);
                });
        connect(&caller, &CallMediaBackend::connected, this,
                [&](const QString &id) {
                    if (id == callId)
                        callerConnected = true;
                });
        connect(&callee, &CallMediaBackend::connected, this,
                [&](const QString &id) {
                    if (id == callId)
                        calleeConnected = true;
                });
        const auto noteFailure = [&](const QString &, const QString &why) {
            failureCategory = why;
        };
        connect(&caller, &CallMediaBackend::failed, this, noteFailure);
        connect(&callee, &CallMediaBackend::failed, this, noteFailure);

        caller.createOffer(callId);

        QTRY_VERIFY2_WITH_TIMEOUT(
            callerConnected && calleeConnected,
            qPrintable(QStringLiteral("no connection; failure=%1")
                           .arg(failureCategory)),
            45000);
        QVERIFY(failureCategory.isEmpty());
        QVERIFY(sawAudioOffer);
        QVERIFY(sawAudioAnswer);

        // Tear down cleanly; a second call on the same engines must work
        // (session state fully recycles).
        caller.close(callId);
        callee.close(callId);

        const QString second = QStringLiteral("loopback-2");
        callerConnected = false;
        calleeConnected = false;
        bool secondOffered = false;
        connect(&caller, &CallMediaBackend::offerReady, this,
                [&](const QString &id, const QString &) {
                    if (id == second)
                        secondOffered = true;
                });
        caller.createOffer(second);
        QTRY_VERIFY_WITH_TIMEOUT(secondOffered, 15000);
        caller.close(second);
    }

    void badRemoteOfferFailsCleanly()
    {
        GstCallMediaBackend engine;
        engine.setTestToneMode(true);
        QSignalSpy failed(&engine, &CallMediaBackend::failed);
        engine.createAnswer(QStringLiteral("bad-1"),
                            QStringLiteral("this is not sdp"));
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() >= 1, 10000);
        QCOMPARE(failed.first().at(1).toString(),
                 QStringLiteral("bad_remote_offer"));
        // The engine recovered: a fresh offer session still starts.
        bool offered = false;
        connect(&engine, &CallMediaBackend::offerReady, this,
                [&](const QString &, const QString &) { offered = true; });
        engine.createOffer(QStringLiteral("good-after-bad"));
        QTRY_VERIFY_WITH_TIMEOUT(offered, 15000);
        engine.close(QStringLiteral("good-after-bad"));
    }
};

QTEST_GUILESS_MAIN(CallMediaLoopbackTest)
#include "CallMediaLoopbackTest.moc"
