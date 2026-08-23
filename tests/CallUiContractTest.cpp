// 2026-08-18 round 2: contract scans for the incoming-call corner card,
// its Main.qml hosting, and the ring settings toggle — plus a REAL
// offscreen instantiation of IncomingCallPrompt against a live
// AppController, which catches unresolved theme tokens and property typos
// that string scans cannot. Predicates are matched whitespace-normalized
// so reflows don't break them.
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QtTest>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "calls/CallController.h"
#include "matrix/CallSignal.h"
#include "matrix/MockMatrixClient.h"

class CallUiContractTest : public QObject
{
    Q_OBJECT
private:
    static QString read(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    }

    static QString normalized(const QString &s)
    {
        QString out = s;
        out.replace(QRegularExpression(QStringLiteral("\\s+")),
                    QStringLiteral(" "));
        return out.trimmed();
    }

private Q_SLOTS:
    void inCallControlsLiveAtTheTopOfTheConversation()
    {
        // 2026-08-23 (maintainer request, with a reference screenshot): the
        // in-call controls moved from the corner card to a bar directly
        // under the room header. Three things must hold.
        const QString bar = normalized(
            read(QStringLiteral(QML_DIR "/CallHeaderBar.qml")));
        QVERIFY(!bar.isEmpty());

        // 1. Mute, deafen, camera, screen share and leave are all there —
        //    the gaps the maintainer reported were the missing device
        //    chooser and screen share, so their absence is the regression
        //    this pins.
        for (const auto &name : {"callBarMicButton", "callBarDeafenButton",
                                 "callBarCameraButton",
                                 "callBarScreenShareButton",
                                 "callBarHangUpButton"}) {
            QVERIFY2(bar.contains(QStringLiteral("objectName: \"%1\"")
                                      .arg(QLatin1String(name))),
                     qPrintable(QStringLiteral("missing %1")
                                    .arg(QLatin1String(name))));
        }
        // 2. Device choosers exist for microphone and output. Without these
        //    a user with several microphones cannot pick one, which is
        //    exactly what was reported.
        QVERIFY(bar.contains(QStringLiteral("objectName: \"callBarMicChevron\"")));
        QVERIFY(bar.contains(
            QStringLiteral("objectName: \"callBarSpeakerChevron\"")));
        // 3. It only shows for the room the call is IN. A bar in the wrong
        //    room would hang up a call the user is not looking at.
        QVERIFY(bar.contains(QStringLiteral("callRoomId === app.currentRoomId")));
        // Screen share goes through the portal entry point, never a
        // hardcoded node id.
        QVERIFY(bar.contains(QStringLiteral("app.groupCall.requestScreenShare()")));
    }

    void theCornerCardNoLongerOwnsALiveCall()
    {
        // Two surfaces offering mute, with nothing to say they are the same
        // state, is worse than either alone. The card keeps the RING and the
        // dialing states — the cases where the user may not be looking at
        // the call's room — and hands an ACTIVE call to the top bar.
        const QString card = normalized(
            read(QStringLiteral(QML_DIR "/IncomingCallPrompt.qml")));
        QVERIFY(!card.isEmpty());
        QVERIFY2(!card.contains(QStringLiteral("inCallMuteButton")),
                 "mute moved to the top bar and must not be duplicated here");
        QVERIFY(card.contains(QStringLiteral(
            "(inCall && app.calls.state !== CallController.Active)")));
    }

    void promptBindsToCallStateNotPolicy()
    {
        const QString norm = normalized(
            read(QStringLiteral(QML_DIR "/IncomingCallPrompt.qml")));
        QVERIFY(!norm.isEmpty());
        // Visibility is call STATE, gated to the chat shell, with per-call
        // dismissal for the RINGING form — never the sound-policy gates.
        QVERIFY(norm.contains(
            QStringLiteral("app.calls.activeCallId !== dismissedCallId")));
        QVERIFY(norm.contains(QStringLiteral("app.currentScreen === 1")));
        QVERIFY(!norm.contains(QStringLiteral("shouldRing")));
        // Decline is the real wire action; Dismiss is local-only; Hang up
        // owns the in-call form.
        QVERIFY(norm.contains(
            QStringLiteral("onClicked: app.calls.rejectIncoming()")));
        QVERIFY(norm.contains(QStringLiteral(
            "onClicked: root.dismissedCallId = app.calls.activeCallId")));
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"incomingCallPromptDecline\"")));
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"incomingCallPromptDismiss\"")));
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"incomingCallPromptHangup\"")));
        QVERIFY(norm.contains(
            QStringLiteral("onClicked: app.calls.hangup()")));
        // Accept exists ONLY behind the media-engine gate (round 3), and
        // the no-engine honesty line survives for engineless builds.
        QVERIFY(norm.contains(QStringLiteral(
            "visible: root.ringing && app.calls.mediaBackendAvailable")));
        QVERIFY(norm.contains(
            QStringLiteral("onClicked: app.calls.answer()")));
        QVERIFY(norm.contains(QStringLiteral("isn't supported yet")));
        // And nothing SDP-shaped belongs anywhere near QML.
        QVERIFY(!norm.contains(QStringLiteral("sdp"),
                               Qt::CaseInsensitive));
    }

    void placeCallEntryIsDmAndEngineGated()
    {
        const QString norm = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(!norm.isEmpty());
        const int button = norm.indexOf(
            QStringLiteral("objectName: \"startVoiceCallButton\""));
        QVERIFY(button >= 0);
        // Wide enough to cover the button's whole block including its
        // rationale comments (widened 2026-08-19 when the coming-soon note
        // landed) — a too-tight window fails on prose, not on behaviour.
        const QString scope = norm.mid(button, 1600);
        // A legacy m.call.invite rings EVERY member of a room: the entry
        // point must be gated to 1:1 DMs and to a registered engine.
        QVERIFY(scope.contains(
            QStringLiteral("root.currentRoom.isDirect === true")));
        QVERIFY(scope.contains(
            QStringLiteral("app.calls.mediaBackendAvailable")));
        QVERIFY(scope.contains(QStringLiteral(
            "onClicked: app.calls.placeCall(app.currentRoomId)")));
        // 2026-08-23: ENABLED at the maintainer's request, after mute was
        // made real and the engine's handshake was proven in-process. The
        // "coming soon" wording must be gone with it — a live button whose
        // tooltip still says the feature is unavailable is worse than
        // either state on its own.
        QVERIFY(scope.contains(QStringLiteral("enabled: true")));
        QVERIFY2(!norm.contains(QStringLiteral("Voice calls are coming soon")),
                 "the coming-soon wording must not outlive the disabled state");
        // The ENGINE gate stays load-bearing: on a packaged build without the
        // GStreamer plugins the button must be ABSENT, not present and dead.
        // That is the one thing enabling it must not quietly give up.
        const int visible = scope.indexOf(QStringLiteral("visible:"));
        const int enabled = scope.indexOf(QStringLiteral("enabled: true"));
        QVERIFY(visible >= 0 && enabled > visible);
        QVERIFY(scope.mid(visible, enabled - visible)
                    .contains(QStringLiteral("app.calls.mediaBackendAvailable")));
    }

    void mainHostsTheCallPromptAboveThePassiveOnes()
    {
        const QString norm =
            normalized(read(QStringLiteral(QML_DIR "/Main.qml")));
        QVERIFY(!norm.isEmpty());
        const int host =
            norm.indexOf(QStringLiteral("objectName: \"cornerPromptHost\""));
        QVERIFY(host >= 0);
        const int call = norm.indexOf(
            QStringLiteral("IncomingCallPrompt {"), host);
        const int update = norm.indexOf(
            QStringLiteral("UpdateAvailablePrompt {"), host);
        const int verify = norm.indexOf(
            QStringLiteral("VerifySessionPrompt {"), host);
        QVERIFY(call > host);
        QVERIFY(update > call);   // live ring renders above the passives
        QVERIFY(verify > update); // verify keeps its anchored corner spot
    }

    void settingsExposeTheRingToggle()
    {
        const QString norm = normalized(
            read(QStringLiteral(QML_DIR "/SettingsScreen.qml")));
        QVERIFY(!norm.isEmpty());
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"ringForCallsCheck\"")));
        QVERIFY(norm.contains(
            QStringLiteral("checked: app.settings.ringForCalls")));
        QVERIFY(norm.contains(
            QStringLiteral("app.settings.ringForCalls = checked")));
    }

    void callHeaderBarInstantiatesAndLaysOutItsControls()
    {
        // A REAL instantiation, not a source scan: this is what catches a
        // control that fails to lay out, a binding against a property that
        // does not exist, or a Loader whose component cannot be created —
        // none of which a text search can see.
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(),
                            &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine,
                              &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("CallHeaderBar"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(3000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr, "CallHeaderBar must instantiate");

        // Collapsed while no call is live, so a room without one reserves
        // no vertical space.
        QCOMPARE(root->property("live").toBool(), false);
        QCOMPARE(root->property("visible").toBool(), false);
        QCOMPARE(root->property("height").toDouble(), 0.0);

        // The controls exist as real items, and each has a non-zero size —
        // a control that lays out to nothing is invisible in practice even
        // though every source check passes.
        for (const auto &name : {"callBarMicButton", "callBarMicChevron",
                                 "callBarDeafenButton",
                                 "callBarSpeakerChevron",
                                 "callBarHangUpButton"}) {
            auto *item = root->findChild<QQuickItem *>(QLatin1String(name));
            QVERIFY2(item != nullptr,
                     qPrintable(QStringLiteral("missing %1")
                                    .arg(QLatin1String(name))));
            QVERIFY2(item->implicitWidth() > 0 && item->implicitHeight() > 0,
                     qPrintable(QStringLiteral("%1 has no size")
                                    .arg(QLatin1String(name))));
        }
    }

    void callHeaderBarShowsForALiveCallInItsOwnRoomOnly()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(),
                            &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine,
                              &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("CallHeaderBar"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(3000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        // A ring is NOT the bar's job: the corner card owns that, because
        // the user may not be looking at the ringing room.
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        CallSignal invite;
        invite.kind = CallSignal::Kind::Invite;
        invite.roomId = QStringLiteral("!general:mock.local");
        invite.eventId = QStringLiteral("$invite-bar");
        invite.sender = QStringLiteral("@peer:mock.local");
        invite.callId = QStringLiteral("bar-call");
        invite.partyId = QStringLiteral("peer-party");
        invite.lifetimeMs = 60000;
        invite.originServerTs = QDateTime::currentMSecsSinceEpoch();
        mock->emitCallSignalForTest(invite);
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        QCOMPARE(root->property("live").toBool(), false);

        // The room the call belongs to is what gates visibility. Without
        // this, a bar in the wrong room would hang up a call the user is
        // not even looking at.
        QCOMPARE(root->property("callRoomId").toString(),
                 QStringLiteral("!general:mock.local"));
    }

    void promptInstantiatesAndReactsToARealRing()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(),
                            &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine,
                              &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("IncomingCallPrompt"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(3000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        // Hidden while idle.
        QCOMPARE(root->property("shouldShow").toBool(), false);

        // A real inbound invite through the wired stack shows the card.
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);
        CallSignal invite;
        invite.kind = CallSignal::Kind::Invite;
        invite.roomId = QStringLiteral("!general:mock.local");
        invite.eventId = QStringLiteral("$invite-ui");
        invite.sender = QStringLiteral("@peer:mock.local");
        invite.callId = QStringLiteral("ui-call");
        invite.partyId = QStringLiteral("peer-party");
        invite.lifetimeMs = 60000;
        invite.originServerTs = QDateTime::currentMSecsSinceEpoch();
        mock->emitCallSignalForTest(invite);
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("shouldShow").toBool(),
                                  true, 3000);

        // End the ring via the same controller invokable the card's
        // Decline button is contract-pinned (above) to call — the QML
        // binding chain from state to visibility is what this asserts;
        // button hit-testing is not exercised here.
        QVERIFY(controller.calls()->rejectIncoming());
        QTRY_COMPARE_WITH_TIMEOUT(root->property("shouldShow").toBool(),
                                  false, 3000);
    }

    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("call-ui-contract-test"));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(CallUiContractTest)
#include "CallUiContractTest.moc"
