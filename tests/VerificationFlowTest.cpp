// The SAS verification UI state machine. Boots a real AppController on the
// Rust backend (no session, no network: the FFI wrappers no-op without a live
// handle) and emits RustSdkMatrixClient's verification signals directly, as
// the poll dispatcher would. Pins:
//   * sas_ready --confirm()--> confirming happens synchronously;
//   * verificationSasConfirmed moves confirming -> waiting_for_peer;
//   * verificationDone ends the flow from the peer wait;
//   * confirm outside sas_ready (repeat clicks, terminal states) is a no-op;
//   * cancelled/failed end the flow from intermediate states;
//   * events for a stale or unknown flow id never change the visible state;
//   * a Confirmed report without a local confirm never advances the flow;
//   * logout (also the account-switch path) clears everything.
// No credentials, tokens or key material appear in this test.

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
        // The concrete client is parented to the AppController, as
        // AppController itself finds it via qobject_cast.
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

    // A synthetic, well-formed module grid, never a real QR payload:
    // matrix-sdk's QrVerification cannot be constructed from a test.
    static constexpr int kModules = 21;
    static QByteArray sampleGrid()
    {
        const int stride = (kModules + 7) / 8;
        return QByteArray(stride * kModules, '\x55');
    }

    // Drive an outbound flow to a displayed QR code.
    static void reachQrReady(AppController &app, RustSdkMatrixClient *rust,
                             const QString &flowId)
    {
        Q_EMIT rust->verificationRequestStarted(
            flowId, QStringLiteral("@self:example.org"), true);
        Q_EMIT rust->verificationReady(flowId);
        QCOMPARE(app.verificationState(), QStringLiteral("ready"));
        Q_EMIT rust->verificationQrReady(flowId, kModules, sampleGrid());
        QVERIFY(app.verificationQrAvailable());
    }

    // Drive an outbound self-verification flow to the emoji screen with the
    // signals the Rust dispatcher emits.
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
        // The press is acknowledged before any SDK round trip reports back.
        QCOMPARE(changed.count(), 1);
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        QVERIFY(app.verificationActive());

        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-1"));
        QCOMPARE(app.verificationState(), QStringLiteral("waiting_for_peer"));

        Q_EMIT rust->verificationDone(QStringLiteral("flow-1"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
#endif
    }

    // An accepted incoming request leaves "requested" as soon as both sides
    // are ready, so the card stops offering Accept and shows handshake
    // progress.
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

        // Accepting is a request to the SDK, not a state promotion.
        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.acceptVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QStringLiteral("requested"));

        // .ready from the SDK.
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

    // A ready report only moves the flow forward: it never rewinds a live
    // flow, resurrects a finished one, or leaks across flows.
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

    // A stall always ends: whichever bounded Rust timeout or peer cancellation
    // fires, the UI reaches a terminal state carrying a reason.
    void stallsExitReadyIntoAVisibleTerminalState()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        {
            // The Rust accept path's bounded timeout.
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
            // The reason reaches the surface.
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

        // A Confirmed report while sas_ready (no local confirm, e.g. a stray
        // poll observation) does not advance the flow.
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

    // A failure raised before any flow id exists (no cross-signing identity,
    // request send failed, not signed in) is visible and dismissable; the
    // start row stays hidden while any state is set.
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

    // "They do not match" is reported to the SDK; only the SDK's own
    // cancellation moves the card.
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

    // One flow at a time: a second start is refused and the visible flow is
    // left untouched.
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

    // Every terminal state allows a fresh attempt.
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

            // Not refused. Without a live backend handle the attempt fails at
            // once with a flow-id-less error, which must stay visible and
            // dismissable.
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

    // The bridge cancels a second incoming request while a flow is live, but
    // if one ever reaches the model it adopts one flow cleanly: no stale
    // emoji, and the orphaned flow's events are inert.
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
        // The previous flow's short auth string does not carry over.
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

    // Every cancellation reason (peer, user, outgoing peer-wait timeout) ends
    // in a terminal state, never a spinner.
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

    // Poll batches deliver SDK state in order, but the model does not rely on
    // it: a missing report never strands the card and a late one never
    // rewinds it.
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
            // Done without any emoji shown still ends as done: the SDK is the
            // authority on success.
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
            // Re-notifying is allowed, moving the flow is not, and a late
            // cancellation cannot overwrite a completed verification.
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

    // Show-QR leg. These drive the Rust dispatcher's signals and prove only
    // the UI state machine; the reciprocate handshake, a real phone scanning
    // and Element interoperability are not tested here.

    void qrIsShownThenScannedThenConfirmedThroughToDone()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachQrReady(app, rust, QStringLiteral("flow-qr-1"));

        // Showing a code does not move the SAS state machine: the QR is
        // another presentation of the same flow.
        QCOMPARE(app.verificationState(), QStringLiteral("ready"));
        QVERIFY(!app.verificationQrScanned());
        QVERIFY(!app.verificationQrConfirming());
        // The URL is opaque and carries no flow id.
        const QString url = app.verificationQrImage();
        QVERIFY(url.startsWith(QStringLiteral("image://lightning-qr/")));
        QVERIFY(!url.contains(QStringLiteral("flow-qr-1")));

        // Confirming before the peer scanned is a no-op.
        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.confirmQrVerification();
        QCOMPARE(changed.count(), 0);
        QVERIFY(!app.verificationQrConfirming());

        Q_EMIT rust->verificationQrScanned(QStringLiteral("flow-qr-1"));
        QVERIFY(app.verificationQrScanned());
        QVERIFY(!app.verificationQrConfirming());

        // The user's confirmation is acknowledged synchronously but is a
        // request to the SDK, not a success claim.
        changed.clear();
        app.confirmQrVerification();
        QCOMPARE(changed.count(), 1);
        QVERIFY(app.verificationQrConfirming());
        QVERIFY(app.verificationState() != QLatin1String("done"));

        // Repeat presses are inert.
        changed.clear();
        app.confirmQrVerification();
        QCOMPARE(changed.count(), 0);

        Q_EMIT rust->verificationQrConfirmed(QStringLiteral("flow-qr-1"));
        QVERIFY(app.verificationQrConfirming());
        QVERIFY(app.verificationState() != QLatin1String("done"));

        // Only the SDK's Done reports success, and it retires the code.
        Q_EMIT rust->verificationDone(QStringLiteral("flow-qr-1"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
        QVERIFY(!app.verificationQrAvailable());
        QVERIFY(app.verificationQrImage().isEmpty());
#endif
    }

    // "Peer cannot scan" fallback: the SDK moves the request to SAS, Rust
    // reports the QR dismissed, and the card returns to the emoji
    // presentation without disturbing the flow.
    void aDismissedQrFallsBackToTheSasFlowCleanly()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        for (const QString &reason : {QStringLiteral("peer_started_sas"),
                                      QStringLiteral("not_scanned")}) {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            reachQrReady(app, rust, QStringLiteral("flow-qr-fb"));

            Q_EMIT rust->verificationQrDismissed(
                QStringLiteral("flow-qr-fb"), reason);
            QVERIFY2(!app.verificationQrAvailable(), qPrintable(reason));
            QVERIFY(app.verificationQrImage().isEmpty());
            // The flow itself is untouched and still live.
            QCOMPARE(app.verificationState(), QStringLiteral("ready"));
            QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-qr-fb"));

            // SAS then proceeds exactly as it always has.
            Q_EMIT rust->verificationSasReady(QStringLiteral("flow-qr-fb"),
                                              sampleEmojis(), QVariantList{});
            QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));
            app.confirmVerification();
            QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
            Q_EMIT rust->verificationDone(QStringLiteral("flow-qr-fb"));
            QCOMPARE(app.verificationState(), QStringLiteral("done"));
        }
#endif
    }

    // Every terminal state retires the code, so nothing invites scanning a
    // code that can no longer verify.
    void everyTerminalStateRetiresTheQrCode()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        const QList<QString> kinds{QStringLiteral("done"),
                                   QStringLiteral("cancelled"),
                                   QStringLiteral("failed"),
                                   QStringLiteral("local-cancel")};
        for (const QString &kind : kinds) {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            reachQrReady(app, rust, QStringLiteral("flow-qr-term"));
            Q_EMIT rust->verificationQrScanned(QStringLiteral("flow-qr-term"));
            QVERIFY(app.verificationQrScanned());

            if (kind == QLatin1String("done"))
                Q_EMIT rust->verificationDone(QStringLiteral("flow-qr-term"));
            else if (kind == QLatin1String("cancelled"))
                Q_EMIT rust->verificationCancelled(
                    QStringLiteral("flow-qr-term"), QStringLiteral("User"));
            else if (kind == QLatin1String("failed"))
                Q_EMIT rust->verificationFailed(
                    QStringLiteral("flow-qr-term"), QStringLiteral("sanitized"));
            else
                app.cancelVerification();

            QVERIFY2(!app.verificationQrAvailable(), qPrintable(kind));
            QVERIFY2(!app.verificationQrScanned(), qPrintable(kind));
            QVERIFY2(!app.verificationQrConfirming(), qPrintable(kind));
            QVERIFY2(app.verificationQrImage().isEmpty(), qPrintable(kind));
        }
#endif
    }

    // A QR event for another flow, or after this flow finished, never puts a
    // code back on screen.
    void qrEventsAreFlowScopedAndCannotResurrectAFinishedFlow()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachQrReady(app, rust, QStringLiteral("flow-qr-scope"));
        const QString mine = app.verificationQrImage();

        // A stranger's grid must not replace the visible code.
        Q_EMIT rust->verificationQrReady(QStringLiteral("other-flow"),
                                         kModules, sampleGrid());
        QCOMPARE(app.verificationQrImage(), mine);
        Q_EMIT rust->verificationQrScanned(QStringLiteral("other-flow"));
        QVERIFY(!app.verificationQrScanned());
        Q_EMIT rust->verificationQrConfirmed(QStringLiteral("other-flow"));
        QVERIFY(!app.verificationQrConfirming());
        // A stranger's dismissal must not take our code away either.
        Q_EMIT rust->verificationQrDismissed(QStringLiteral("other-flow"),
                                             QStringLiteral("not_scanned"));
        QCOMPARE(app.verificationQrImage(), mine);

        // After the flow finishes, a late grid cannot repaint the card as
        // something still waiting to be scanned.
        Q_EMIT rust->verificationDone(QStringLiteral("flow-qr-scope"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
        Q_EMIT rust->verificationQrReady(QStringLiteral("flow-qr-scope"),
                                         kModules, sampleGrid());
        QVERIFY(!app.verificationQrAvailable());
        Q_EMIT rust->verificationQrScanned(QStringLiteral("flow-qr-scope"));
        QVERIFY(!app.verificationQrScanned());
#endif
    }

    // Malformed geometry shows no code rather than an unscannable picture.
    void aMalformedGridIsRefusedAndLeavesNoCodeOnScreen()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        Q_EMIT rust->verificationRequestStarted(
            QStringLiteral("flow-qr-bad"), QStringLiteral("@self:example.org"),
            true);
        Q_EMIT rust->verificationReady(QStringLiteral("flow-qr-bad"));

        // Byte count does not match the module count.
        Q_EMIT rust->verificationQrReady(QStringLiteral("flow-qr-bad"),
                                         kModules, QByteArray(4, '\0'));
        QVERIFY(!app.verificationQrAvailable());
        // Zero modules.
        Q_EMIT rust->verificationQrReady(QStringLiteral("flow-qr-bad"), 0,
                                         QByteArray());
        QVERIFY(!app.verificationQrAvailable());
        // The flow is unharmed and still finishes on SAS.
        QCOMPARE(app.verificationState(), QStringLiteral("ready"));
        Q_EMIT rust->verificationSasReady(QStringLiteral("flow-qr-bad"),
                                          sampleEmojis(), QVariantList{});
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));
#endif
    }

    // A new flow, in either direction, never inherits the previous code.
    void anewFlowNeverInheritsThePreviousCode()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            reachQrReady(app, rust, QStringLiteral("flow-qr-old"));
            Q_EMIT rust->verificationRequestReceived(
                QStringLiteral("flow-qr-new"),
                QStringLiteral("@self:example.org"),
                QStringLiteral("OTHERDEV"), true);
            QCOMPARE(app.verificationState(), QStringLiteral("requested"));
            QVERIFY(!app.verificationQrAvailable());
        }
        {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            reachQrReady(app, rust, QStringLiteral("flow-qr-old2"));
            Q_EMIT rust->verificationRequestStarted(
                QStringLiteral("flow-qr-new2"),
                QStringLiteral("@self:example.org"), true);
            QVERIFY(!app.verificationQrAvailable());
        }
#endif
    }

    void logoutClearsTheDisplayedQrCode()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachQrReady(app, rust, QStringLiteral("flow-qr-out"));
        Q_EMIT rust->verificationQrScanned(QStringLiteral("flow-qr-out"));
        QVERIFY(app.verificationQrScanned());

        // Account switching goes through the same loggedOut signal; a code
        // never survives into the next account.
        Q_EMIT rust->loggedOut();
        QVERIFY(!app.verificationQrAvailable());
        QVERIFY(!app.verificationQrScanned());
        QVERIFY(app.verificationQrImage().isEmpty());

        // Late events from the dead flow stay rejected.
        Q_EMIT rust->verificationQrReady(QStringLiteral("flow-qr-out"),
                                         kModules, sampleGrid());
        QVERIFY(!app.verificationQrAvailable());
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

        // Account switching goes through the same loggedOut signal; nothing
        // leaks across sessions.
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
