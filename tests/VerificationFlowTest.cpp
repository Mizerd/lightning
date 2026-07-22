// v0.7.1: SAS verification UI state machine — the "They match" feedback
// chain. Boots a real AppController on the Rust backend (no session, no
// network: the FFI wrappers no-op without a live handle) and drives the
// RustSdkMatrixClient verification signals directly, exactly as the poll
// dispatcher would. Pins:
//   * sas_ready --confirm()--> confirming happens SYNCHRONOUSLY;
//   * verificationSasConfirmed moves confirming -> waiting_for_peer;
//   * verificationDone terminates the flow from the peer wait;
//   * confirm outside sas_ready (repeat clicks, terminal states) is a no-op;
//   * cancelled/failed terminate from the intermediate states;
//   * events for a stale/unknown flow id never mutate the visible state;
//   * a Confirmed report without a local confirm never advances the flow;
//   * logout (the path account switching passes through) clears everything.
// No credentials, tokens, or key material appear anywhere in this test.

#include "app/AppController.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

class VerificationFlowTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    static RustSdkMatrixClient *rustClient(AppController &app)
    {
        // The concrete client is parented to the AppController (makeClient
        // passes it as the QObject parent), mirroring how AppController
        // itself locates it via qobject_cast on m_client.
        return app.findChild<RustSdkMatrixClient *>();
    }

    static QVariantList sampleEmojis()
    {
        QVariantList emojis;
        QVariantMap emoji;
        emoji.insert(QStringLiteral("symbol"), QStringLiteral("E"));
        emoji.insert(QStringLiteral("description"), QStringLiteral("Emoji"));
        emojis.append(emoji);
        return emojis;
    }

    // Drive an outbound self-verification flow to the emoji screen using
    // the same signals the Rust event dispatcher emits.
    static void reachSasReady(AppController &app, RustSdkMatrixClient *rust,
                              const QString &flowId)
    {
        Q_EMIT rust->verificationRequestStarted(
            flowId, QStringLiteral("@self:example.org"), true);
        QCOMPARE(app.verificationState(),
                 QStringLiteral("waiting_for_other_session"));
        Q_EMIT rust->verificationSasReady(flowId, sampleEmojis(),
                                          QVariantList{});
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));
        QCOMPARE(app.verificationFlowId(), flowId);
    }
#endif

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("verification-flow-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void confirmIsSynchronousAndAdvancesThroughPeerWaitToDone()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachSasReady(app, rust, QStringLiteral("flow-1"));

        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.confirmVerification();
        // The press is acknowledged BEFORE any SDK round-trip reports back.
        QCOMPARE(changed.count(), 1);
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        QVERIFY(app.verificationActive());

        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-1"));
        QCOMPARE(app.verificationState(), QStringLiteral("waiting_for_peer"));

        Q_EMIT rust->verificationDone(QStringLiteral("flow-1"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
#endif
    }

    void confirmOutsideSasReadyIsNoOp()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        // Before the emoji screen: request sent, peer not ready yet.
        Q_EMIT rust->verificationRequestStarted(
            QStringLiteral("flow-2"), QStringLiteral("@self:example.org"),
            true);
        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.confirmVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(),
                 QStringLiteral("waiting_for_other_session"));

        // Repeat clicks while confirming / waiting for the peer.
        Q_EMIT rust->verificationSasReady(QStringLiteral("flow-2"),
                                          sampleEmojis(), QVariantList{});
        app.confirmVerification();
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        changed.clear();
        app.confirmVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-2"));
        changed.clear();
        app.confirmVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QStringLiteral("waiting_for_peer"));

        // Terminal state: confirm after done stays done.
        Q_EMIT rust->verificationDone(QStringLiteral("flow-2"));
        changed.clear();
        app.confirmVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
#endif
    }

    void staleOrUnknownFlowEventsDoNotMutateState()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachSasReady(app, rust, QStringLiteral("flow-3"));

        // A Confirmed report while sas_ready (no local confirm happened —
        // e.g. a stray poll observation) must not advance the flow.
        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-3"));
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));

        app.confirmVerification();
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));

        // Events for a different (stale) flow id never touch this flow.
        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("other-flow"));
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        Q_EMIT rust->verificationDone(QStringLiteral("other-flow"));
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        Q_EMIT rust->verificationCancelled(QStringLiteral("other-flow"),
                                           QStringLiteral("cancelled"));
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-3"));

        // A late Confirmed after the flow finished cannot resurrect it.
        Q_EMIT rust->verificationDone(QStringLiteral("flow-3"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-3"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
#endif
    }

    void cancelAndFailureTerminateTheIntermediateStates()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        {
            // Peer/SDK cancellation while we are confirming.
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            reachSasReady(app, rust, QStringLiteral("flow-4"));
            app.confirmVerification();
            QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
            Q_EMIT rust->verificationCancelled(QStringLiteral("flow-4"),
                                               QStringLiteral("cancelled"));
            QCOMPARE(app.verificationState(), QStringLiteral("cancelled"));
        }
        {
            // SDK failure while waiting for the peer's confirmation.
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            reachSasReady(app, rust, QStringLiteral("flow-5"));
            app.confirmVerification();
            Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-5"));
            QCOMPARE(app.verificationState(),
                     QStringLiteral("waiting_for_peer"));
            Q_EMIT rust->verificationFailed(QStringLiteral("flow-5"),
                                            QStringLiteral("sanitized"));
            QVERIFY(app.verificationState().startsWith(
                QStringLiteral("failed")));
        }
        {
            // Local cancel from the peer wait clears the flow entirely.
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            reachSasReady(app, rust, QStringLiteral("flow-6"));
            app.confirmVerification();
            Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-6"));
            app.cancelVerification();
            QVERIFY(!app.verificationActive());
            QCOMPARE(app.verificationState(), QString());
        }
#endif
    }

    void logoutClearsTheVerificationStateCache()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachSasReady(app, rust, QStringLiteral("flow-7"));
        app.confirmVerification();
        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-7"));
        QCOMPARE(app.verificationState(), QStringLiteral("waiting_for_peer"));

        // Account switching detaches the session through the same
        // loggedOut signal; the cache must never leak across sessions.
        Q_EMIT rust->loggedOut();
        QVERIFY(!app.verificationActive());
        QCOMPARE(app.verificationState(), QString());
        QCOMPARE(app.verificationFlowId(), QString());
        QVERIFY(app.verificationEmojis().isEmpty());

        // Late events from the dead flow stay rejected.
        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-7"));
        Q_EMIT rust->verificationDone(QStringLiteral("flow-7"));
        QCOMPARE(app.verificationState(), QString());
#endif
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(VerificationFlowTest)
#include "VerificationFlowTest.moc"
