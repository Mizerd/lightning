
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
        QCOMPARE(changed.count(), 1);
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        QVERIFY(app.verificationActive());

        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-1"));
        QCOMPARE(app.verificationState(), QStringLiteral("waiting_for_peer"));

        Q_EMIT rust->verificationDone(QStringLiteral("flow-1"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
#endif
    }

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

        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.acceptVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QStringLiteral("requested"));

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

    void readyOnlyAdvancesPreEmojiStatesAndIsFlowScoped()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachSasReady(app, rust, QStringLiteral("flow-in-2"));

        Q_EMIT rust->verificationReady(QStringLiteral("flow-in-2"));
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));

        app.confirmVerification();
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        Q_EMIT rust->verificationReady(QStringLiteral("flow-in-2"));
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));

        Q_EMIT rust->verificationReady(QStringLiteral("other-flow"));
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-in-2"));

        Q_EMIT rust->verificationDone(QStringLiteral("flow-in-2"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
        Q_EMIT rust->verificationReady(QStringLiteral("flow-in-2"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
#endif
    }

    void stallsExitReadyIntoAVisibleTerminalState()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        {
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
            QVERIFY(app.verificationState().contains(
                QStringLiteral("Timed out")));
        }
        {
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

        Q_EMIT rust->verificationRequestStarted(
            QStringLiteral("flow-2"), QStringLiteral("@self:example.org"),
            true);
        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.confirmVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(),
                 QStringLiteral("waiting_for_other_session"));

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

        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-3"));
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));

        app.confirmVerification();
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));

        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("other-flow"));
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        Q_EMIT rust->verificationDone(QStringLiteral("other-flow"));
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        Q_EMIT rust->verificationCancelled(QStringLiteral("other-flow"),
                                           QStringLiteral("cancelled"));
        QCOMPARE(app.verificationState(), QStringLiteral("confirming"));
        QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-3"));

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

    void failureWithoutAFlowIdStaysVisibleAndIsDismissable()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        app.cancelVerification();
        QCOMPARE(app.verificationState(), QString());

        Q_EMIT rust->verificationFailed(
            QString{}, QStringLiteral("no cross-signing identity"));
        QVERIFY(app.verificationState().startsWith(QStringLiteral("failed")));
        QVERIFY(!app.verificationActive());

        app.cancelVerification();
        QCOMPARE(app.verificationState(), QString());
        QVERIFY(!app.verificationActive());
#endif
    }

    void mismatchNeedsAFlowAndNeverPromotesStateItself()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.mismatchVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QString());

        reachSasReady(app, rust, QStringLiteral("flow-mismatch"));
        changed.clear();
        app.mismatchVerification();
        QCOMPARE(changed.count(), 0);
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));

        Q_EMIT rust->verificationCancelled(QStringLiteral("flow-mismatch"),
                                           QStringLiteral("MismatchedSas"));
        QCOMPARE(app.verificationState(), QStringLiteral("cancelled"));
#endif
    }

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
        QVERIFY(app.verificationEmojis().isEmpty());

        Q_EMIT rust->verificationSasReady(QStringLiteral("flow-first"),
                                          sampleEmojis(), QVariantList{});
        QCOMPARE(app.verificationState(), QStringLiteral("requested"));
        Q_EMIT rust->verificationDone(QStringLiteral("flow-first"));
        QCOMPARE(app.verificationState(), QStringLiteral("requested"));
        QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-second"));
#endif
    }

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

    void outOfOrderReportsNeverStrandOrRewindTheFlow()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        {
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            Q_EMIT rust->verificationRequestStarted(
                QStringLiteral("flow-ooo1"),
                QStringLiteral("@self:example.org"), true);
            Q_EMIT rust->verificationSasReady(QStringLiteral("flow-ooo1"),
                                              sampleEmojis(), QVariantList{});
            QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));
            Q_EMIT rust->verificationReady(QStringLiteral("flow-ooo1"));
            QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));
        }
        {
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
            AppController app(AppController::RustBackend);
            auto *rust = rustClient(app);
            QVERIFY(rust);
            reachSasReady(app, rust, QStringLiteral("flow-ooo3"));
            Q_EMIT rust->verificationDone(QStringLiteral("flow-ooo3"));
            QCOMPARE(app.verificationState(), QStringLiteral("done"));
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

    void qrIsShownThenScannedThenConfirmedThroughToDone()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachQrReady(app, rust, QStringLiteral("flow-qr-1"));

        QCOMPARE(app.verificationState(), QStringLiteral("ready"));
        QVERIFY(!app.verificationQrScanned());
        QVERIFY(!app.verificationQrConfirming());
        const QString url = app.verificationQrImage();
        QVERIFY(url.startsWith(QStringLiteral("image://lightning-qr/")));
        QVERIFY(!url.contains(QStringLiteral("flow-qr-1")));

        QSignalSpy changed(&app, &AppController::verificationStateChanged);
        app.confirmQrVerification();
        QCOMPARE(changed.count(), 0);
        QVERIFY(!app.verificationQrConfirming());

        Q_EMIT rust->verificationQrScanned(QStringLiteral("flow-qr-1"));
        QVERIFY(app.verificationQrScanned());
        QVERIFY(!app.verificationQrConfirming());

        changed.clear();
        app.confirmQrVerification();
        QCOMPARE(changed.count(), 1);
        QVERIFY(app.verificationQrConfirming());
        QVERIFY(app.verificationState() != QLatin1String("done"));

        changed.clear();
        app.confirmQrVerification();
        QCOMPARE(changed.count(), 0);

        Q_EMIT rust->verificationQrConfirmed(QStringLiteral("flow-qr-1"));
        QVERIFY(app.verificationQrConfirming());
        QVERIFY(app.verificationState() != QLatin1String("done"));

        Q_EMIT rust->verificationDone(QStringLiteral("flow-qr-1"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
        QVERIFY(!app.verificationQrAvailable());
        QVERIFY(app.verificationQrImage().isEmpty());
#endif
    }

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
            QCOMPARE(app.verificationState(), QStringLiteral("ready"));
            QCOMPARE(app.verificationFlowId(), QStringLiteral("flow-qr-fb"));

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

        Q_EMIT rust->verificationQrReady(QStringLiteral("other-flow"),
                                         kModules, sampleGrid());
        QCOMPARE(app.verificationQrImage(), mine);
        Q_EMIT rust->verificationQrScanned(QStringLiteral("other-flow"));
        QVERIFY(!app.verificationQrScanned());
        Q_EMIT rust->verificationQrConfirmed(QStringLiteral("other-flow"));
        QVERIFY(!app.verificationQrConfirming());
        Q_EMIT rust->verificationQrDismissed(QStringLiteral("other-flow"),
                                             QStringLiteral("not_scanned"));
        QCOMPARE(app.verificationQrImage(), mine);

        Q_EMIT rust->verificationDone(QStringLiteral("flow-qr-scope"));
        QCOMPARE(app.verificationState(), QStringLiteral("done"));
        Q_EMIT rust->verificationQrReady(QStringLiteral("flow-qr-scope"),
                                         kModules, sampleGrid());
        QVERIFY(!app.verificationQrAvailable());
        Q_EMIT rust->verificationQrScanned(QStringLiteral("flow-qr-scope"));
        QVERIFY(!app.verificationQrScanned());
#endif
    }

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

        Q_EMIT rust->verificationQrReady(QStringLiteral("flow-qr-bad"),
                                         kModules, QByteArray(4, '\0'));
        QVERIFY(!app.verificationQrAvailable());
        Q_EMIT rust->verificationQrReady(QStringLiteral("flow-qr-bad"), 0,
                                         QByteArray());
        QVERIFY(!app.verificationQrAvailable());
        QCOMPARE(app.verificationState(), QStringLiteral("ready"));
        Q_EMIT rust->verificationSasReady(QStringLiteral("flow-qr-bad"),
                                          sampleEmojis(), QVariantList{});
        QCOMPARE(app.verificationState(), QStringLiteral("sas_ready"));
#endif
    }

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

        Q_EMIT rust->loggedOut();
        QVERIFY(!app.verificationQrAvailable());
        QVERIFY(!app.verificationQrScanned());
        QVERIFY(app.verificationQrImage().isEmpty());

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

        Q_EMIT rust->loggedOut();
        QVERIFY(!app.verificationActive());
        QCOMPARE(app.verificationState(), QString());
        QCOMPARE(app.verificationFlowId(), QString());
        QVERIFY(app.verificationEmojis().isEmpty());

        Q_EMIT rust->verificationSasConfirmed(QStringLiteral("flow-7"));
        Q_EMIT rust->verificationDone(QStringLiteral("flow-7"));
        QCOMPARE(app.verificationState(), QString());
#endif
    }

    // B079: the SDK's cancel message is classified so a SAS mismatch reads
    // as a security warning rather than "cancelled". The real messages are
    // the Debug form of matrix-sdk-crypto's CancelInfo::reason().
    void theCancelReasonIsClassifiedFromTheSdksOwnWords()
    {
        const auto reason = &AppController::verificationCancelReasonFor;
        QCOMPARE(reason(QStringLiteral("\"The SAS did not match.\"")),
                 QStringLiteral("mismatch"));
        QCOMPARE(reason(QStringLiteral(
                     "\"The expected key did not match the verified one\"")),
                 QStringLiteral("mismatch"));
        QCOMPARE(reason(QStringLiteral("MismatchedSas")),
                 QStringLiteral("mismatch"));
        QCOMPARE(reason(QStringLiteral(
                     "\"The hash commitment did not match.\"")),
                 QStringLiteral("mismatch"));
        QCOMPARE(reason(QStringLiteral("MismatchedCommitment")),
                 QStringLiteral("mismatch"));
        QCOMPARE(reason(QStringLiteral(
                     "\"The user cancelled the verification.\"")),
                 QStringLiteral("user"));
        QCOMPARE(reason(QStringLiteral("cancelled")), QStringLiteral("user"));
        QCOMPARE(reason(QStringLiteral("User")), QStringLiteral("user"));
        QCOMPARE(reason(QStringLiteral(
                     "\"The verification process timed out.\"")),
                 QStringLiteral("timeout"));
        QCOMPARE(reason(QStringLiteral("timed_out_waiting_for_peer")),
                 QStringLiteral("timeout"));
        QCOMPARE(reason(QStringLiteral(
                     "\"The device received an unexpected message.\"")),
                 QString());
        QCOMPARE(reason(QString()), QString());
    }

    void aMismatchAndTheVerifiedSideReachTheCard()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("SAS verification exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);

        reachSasReady(app, rust, QStringLiteral("flow-b079"));
        Q_EMIT rust->verificationCancelled(
            QStringLiteral("flow-b079"),
            QStringLiteral("\"The SAS did not match.\""));
        QCOMPARE(app.verificationState(), QStringLiteral("cancelled"));
        QCOMPARE(app.verificationCancelReason(), QStringLiteral("mismatch"));

        app.cancelVerification();
        Q_EMIT rust->verificationRequestStarted(
            QStringLiteral("flow-b079-2"), QStringLiteral("@bob:example.org"),
            false);
        QCOMPARE(app.verificationRole(), QStringLiteral("other_user"));
        Q_EMIT rust->verificationCancelled(
            QStringLiteral("flow-b079-2"),
            QStringLiteral("\"The user cancelled the verification.\""));
        QCOMPARE(app.verificationCancelReason(), QStringLiteral("user"));

        app.cancelVerification();
        Q_EMIT rust->verificationRequestReceived(
            QStringLiteral("flow-b079-3"), QStringLiteral("@self:example.org"),
            QStringLiteral("DEVICE"), true);
        QCOMPARE(app.verificationRole(), QString());
#endif
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_GUILESS_MAIN(VerificationFlowTest)
#include "VerificationFlowTest.moc"
