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

    // The Element X stall fix: an accepted incoming request must visibly
    // leave "requested" as soon as both sides are Ready, so the card stops
    // offering Accept and shows handshake progress instead of looking
    // identical to an unanswered request.
    void incomingRequestAdvancesThroughReadyToTheEmojiScreen()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        Q_EMIT rust->verificationRequestReceived(
            QStringLiteral("flow-in-1"), QStringLiteral("@self:example.org"),
            QStringLiteral("PHONEDEV"), true);
        QCOMPARE(app.verificationState(), QStringLiteral("requested"));
        QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-in-1"));
        QCOMPARE(app.verificationOtherDevice(), QStringLiteral("PHONEDEV"));

        // Accepting is a request to the SDK, not a state promotion: the
        // model must not claim progress the SDK has not reported.
        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.acceptVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QStringLiteral("requested"));

        // .ready from the SDK — the state the old build threw away.
        Q_EMIT rust->verificationReady(QStringLiteral("flow-in-1"));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(app.verificationState(), QStringLiteral("ready"));
        QVERIFY(app.verificationActive());
        QVERIFY(app.verificationEmojis().isEmpty());

        Q_EMIT rust->verificationSasReady(QStringLiteral("flow-in-1"),
                                          sampleEmojis(), QVariantList{});
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));
        QCOMPARE(app.verificationEmojis().size(), 1);
#endif
    }

    // A ready report may only move the flow forward. It must never be
    // able to rewind a live flow, resurrect a finished one, or leak
    // across flows.
    void readyOnlyAdvancesPreEmojiStatesAndIsFlowScoped()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachSasReady(app, rust, QStringLiteral("flow-in-2"));

        // Rewind attempt from the emoji screen.
        Q_EMIT rust->verificationReady(QStringLiteral("flow-in-2"));
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));

        app.confirmVerification();
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        Q_EMIT rust->verificationReady(QStringLiteral("flow-in-2"));
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));

        // A ready for some other flow never touches this one.
        Q_EMIT rust->verificationReady(QStringLiteral("other-flow"));
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-in-2"));

        // Terminal states stay terminal.
        Q_EMIT rust->verificationDone(QStringLiteral("flow-in-2"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
        Q_EMIT rust->verificationReady(QStringLiteral("flow-in-2"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
#endif
    }

    // The stall must always be escapable: whichever bounded Rust timeout
    // or peer cancellation fires, the UI leaves the in-flight state for a
    // terminal one carrying a reason. Never a silent freeze.
    void stallsExitReadyIntoAVisibleTerminalState()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        {
            // "Timed out waiting for SAS handshake." — the Rust accept
            // path's bounded exit.
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            Q_EMIT rust->verificationRequestReceived(
                QStringLiteral("flow-in-3"),
                QStringLiteral("@self:example.org"),
                QStringLiteral("PHONEDEV"), true);
            Q_EMIT rust->verificationReady(QStringLiteral("flow-in-3"));
            QCOMPARE(app.verificationState(), QStringLiteral("ready"));

            Q_EMIT rust->verificationFailed(
                QStringLiteral("flow-in-3"),
                QStringLiteral("Timed out waiting for SAS handshake."));
            QVERIFY(app.verificationState().startsWith(
                QStringLiteral("failed")));
            // The reason has to reach the surface, not be swallowed.
            QVERIFY(app.verificationState().contains(
                QStringLiteral("Timed out")));
        }
        {
            // Peer cancellation during the handshake.
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            Q_EMIT rust->verificationRequestReceived(
                QStringLiteral("flow-in-4"),
                QStringLiteral("@self:example.org"),
                QStringLiteral("PHONEDEV"), true);
            Q_EMIT rust->verificationReady(QStringLiteral("flow-in-4"));
            Q_EMIT rust->verificationCancelled(QStringLiteral("flow-in-4"),
                                               QStringLiteral("cancelled"));
            QCOMPARE(app.verificationState(), QStringLiteral("cancelled"));
            // A cancelled flow must not block the next attempt.
            QVERIFY(app.verificationState() != QStringLiteral("ready"));
        }
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

    // A failure raised BEFORE any flow id exists — no cross-signing identity,
    // request send failed, not signed in — must still be visible AND
    // dismissable. The card is shown for any non-empty state, so if Dismiss
    // could not clear a flow-id-less failure the user would be pinned to it
    // permanently: the start row stays hidden while a state is set.
    void failureWithoutAFlowIdStaysVisibleAndIsDismissable()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        // Nothing displayed yet: cancel is a no-op and must not fabricate one.
        app.cancelVerification();
        QCOMPARE(app.verificationState(), QString());

        Q_EMIT rust->verificationFailed(
            QString{}, QStringLiteral("no cross-signing identity"));
        QVERIFY(app.verificationState().startsWith(QStringLiteral("failed")));
        // No flow id, so the card cannot rely on verificationActive alone.
        QVERIFY(!app.verificationActive());

        app.cancelVerification();
        QCOMPARE(app.verificationState(), QString());
        QVERIFY(!app.verificationActive());
#endif
    }

    // "They do not match" is a report to the SDK, not a verdict the model
    // may render itself. Only the SDK's own cancellation moves the card —
    // otherwise the UI would claim an outcome the crypto layer never
    // reached.
    void mismatchNeedsAFlowAndNeverPromotesStateItself()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        // Nothing in flight: must not fabricate a flow.
        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.mismatchVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QString());

        reachSasReady(app, rust, QStringLiteral("flow-mismatch"));
        changed.clear();
        app.mismatchVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));

        // The SDK cancels with MismatchedSas; that is what terminates it.
        Q_EMIT rust->verificationCancelled(QStringLiteral("flow-mismatch"),
                                           QStringLiteral("MismatchedSas"));
        QCOMPARE(app.verificationState(), QStringLiteral("cancelled"));
#endif
    }

    // One flow at a time, enforced without wrecking the live one: a second
    // start must be refused outright, leaving the visible flow exactly as
    // it was.
    void startingASecondFlowIsRefusedAndLeavesTheLiveOneIntact()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachSasReady(app, rust, QStringLiteral("flow-live"));

        QSignalSpy errors(&app, &AppController::errorReported);
        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.startOwnVerification();

        QCOMPARE(errors.count(), 1);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));
        QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-live"));
        QCOMPARE(app.verificationEmojis().size(), 1);
#endif
    }

    // The complement: every TERMINAL state must allow a fresh attempt.
    // A finished, cancelled or failed flow that kept blocking new starts is
    // exactly the brick this pass removed on the Rust side too.
    void aTerminalFlowNeverBlocksTheNextAttempt()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        const QList<QString> terminal{QStringLiteral("done"),
                                      QStringLiteral("cancelled"),
                                      QStringLiteral("failed")};
        for (const QString &kind : terminal) {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            reachSasReady(app, rust, QStringLiteral("flow-term"));
            if (kind == QLatin1String("done"))
                Q_EMIT rust->verificationDone(QStringLiteral("flow-term"));
            else if (kind == QLatin1String("cancelled"))
                Q_EMIT rust->verificationCancelled(
                    QStringLiteral("flow-term"), QStringLiteral("User"));
            else
                Q_EMIT rust->verificationFailed(
                    QStringLiteral("flow-term"), QStringLiteral("sanitized"));

            QSignalSpy errors(&app, &AppController::errorReported);
            app.startOwnVerification();

            // Not refused. Without a live backend handle the attempt fails
            // immediately with a flow-id-less error, which is precisely the
            // path that must stay visible and dismissable.
            QCOMPARE(errors.count(), 0);
            QVERIFY2(app.verificationState().startsWith(
                         QStringLiteral("failed")),
                     qPrintable(QStringLiteral("state=%1 after %2")
                                    .arg(app.verificationState(), kind)));
            QVERIFY(app.verificationFlowId().isEmpty());
            QVERIFY(app.verificationEmojis().isEmpty());
        }
#endif
    }

    // The bridge now refuses to surface a second request while a flow is
    // live (it cancels the newcomer instead), so this should not happen in
    // practice. If it ever does, the model must adopt ONE flow cleanly
    // rather than blend two: no stale emoji, and the orphaned flow's
    // events must be inert.
    void aSecondIncomingRequestReplacesTheFlowWithoutMixingState()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachSasReady(app, rust, QStringLiteral("flow-first"));
        QCOMPARE(app.verificationEmojis().size(), 1);

        Q_EMIT rust->verificationRequestReceived(
            QStringLiteral("flow-second"), QStringLiteral("@self:example.org"),
            QStringLiteral("OTHERDEV"), true);
        QCOMPARE(app.verificationState(), QStringLiteral("requested"));
        QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-second"));
        QCOMPARE(app.verificationOtherDevice(), QStringLiteral("OTHERDEV"));
        // The previous flow's short auth string must not survive into a
        // different flow's card.
        QVERIFY(app.verificationEmojis().isEmpty());

        // Nothing the orphaned flow says may touch the visible one.
        Q_EMIT rust->verificationSasReady(QStringLiteral("flow-first"),
                                          sampleEmojis(), QVariantList{});
        QCOMPARE(app.verificationState(), QStringLiteral("requested"));
        Q_EMIT rust->verificationDone(QStringLiteral("flow-first"));
        QCOMPARE(app.verificationState(), QStringLiteral("requested"));
        QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-second"));
#endif
    }

    // Whatever reason the bridge reports — a peer cancel, a user cancel, or
    // the outgoing path's peer-wait timeout — the card must land in one
    // honest terminal state and never keep spinning.
    void everyCancellationReasonTerminatesTheFlow()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        const QList<QString> reasons{
            QStringLiteral("cancelled"),
            QStringLiteral("timed_out_waiting_for_peer"),
            QStringLiteral("User"),
            QStringLiteral("MismatchedSas"),
        };
        for (const QString &reason : reasons) {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            Q_EMIT rust->verificationRequestStarted(
                QStringLiteral("flow-cancel"),
                QStringLiteral("@self:example.org"), true);
            QCOMPARE(app.verificationState(),
                     QStringLiteral("waiting_for_other_session"));

            Q_EMIT rust->verificationCancelled(QStringLiteral("flow-cancel"),
                                               reason);
            QVERIFY2(app.verificationState() == QLatin1String("cancelled"),
                     qPrintable(QStringLiteral("state=%1 for reason=%2")
                                    .arg(app.verificationState(), reason)));
        }
#endif
    }

    // Poll batches deliver SDK state in order, but the model must not
    // depend on it: a missing intermediate report may never strand the
    // card, and a late one may never rewind a flow that moved on.
    void outOfOrderReportsNeverStrandOrRewindTheFlow()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        {
            // Emoji without a preceding ready.
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            Q_EMIT rust->verificationRequestStarted(
                QStringLiteral("flow-ooo1"),
                QStringLiteral("@self:example.org"), true);
            Q_EMIT rust->verificationSasReady(QStringLiteral("flow-ooo1"),
                                              sampleEmojis(), QVariantList{});
            QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));
            // A ready that arrives afterwards cannot pull it back.
            Q_EMIT rust->verificationReady(QStringLiteral("flow-ooo1"));
            QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));
        }
        {
            // Done without any emoji ever being shown. The SDK is the only
            // authority on success, so this still terminates as done.
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            Q_EMIT rust->verificationRequestStarted(
                QStringLiteral("flow-ooo2"),
                QStringLiteral("@self:example.org"), true);
            Q_EMIT rust->verificationDone(QStringLiteral("flow-ooo2"));
            QCOMPARE(app.verificationState(), QStringLiteral("done"));
            QVERIFY(app.verificationEmojis().isEmpty());
        }
        {
            // A duplicated terminal report is idempotent.
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            reachSasReady(app, rust, QStringLiteral("flow-ooo3"));
            Q_EMIT rust->verificationDone(QStringLiteral("flow-ooo3"));
            QCOMPARE(app.verificationState(), QStringLiteral("done"));
            // A duplicate terminal report is idempotent: re-notifying is
            // allowed, moving the flow is not. Also prove no OTHER terminal
            // report can overwrite a completed one — a late cancellation
            // must never turn a finished verification into a failed-looking
            // card.
            Q_EMIT rust->verificationDone(QStringLiteral("flow-ooo3"));
            QCOMPARE(app.verificationState(), QStringLiteral("done"));
            Q_EMIT rust->verificationCancelled(QStringLiteral("flow-ooo3"),
                                               QStringLiteral("Accepted"));
            QCOMPARE(app.verificationState(), QStringLiteral("done"));
            Q_EMIT rust->verificationFailed(QStringLiteral("flow-ooo3"),
                                            QStringLiteral("late failure"));
            QCOMPARE(app.verificationState(), QStringLiteral("done"));
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
