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
        // EXACTLY ONE surface at a time. Gating on call STATE was wrong —
        // during Inviting/Connecting both the bar and the card were visible,
        // which is what the maintainer reported. The card now appears only
        // where the bar cannot reach: another room, or another screen.
        QVERIFY(card.contains(QStringLiteral("(inCall && !barCovers)")));
        QVERIFY(card.contains(QStringLiteral(
            "app.calls.activeRoomId === app.currentRoomId")));
        // Hang Up must still be reachable from the card, so leaving a call
        // works from Settings or another room.
        QVERIFY(card.contains(QStringLiteral("incomingCallPromptHangup")));
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
        // 2026-08-23: lane selection is ONE policy question, answered in
        // AppController — MatrixRTC where available, the legacy 1:1 lane as
        // the audio-only DM fallback. The button asks whether either lane
        // can carry a call rather than re-deriving that rule in QML, so a
        // homeserver with no MatrixRTC and a non-DM room shows no button.
        //
        // The DM restriction still EXISTS; it moved to where it belongs.
        // AppControllerCallLaneTest covers it against the real policy.
        QVERIFY(scope.contains(
            QStringLiteral("app.canStartCall(app.currentRoomId)")));
        QVERIFY(scope.contains(QStringLiteral(
            "onClicked: app.startCall(app.currentRoomId, false)")));
        // A live group call must not offer "start a call" as well.
        QVERIFY(scope.contains(QStringLiteral("!app.groupCall.active")));
        // 2026-08-23: ENABLED at the maintainer's request, after mute was
        // made real and the engine's handshake was proven in-process. The
        // "coming soon" wording must be gone with it — a live button whose
        // tooltip still says the feature is unavailable is worse than
        // either state on its own.
        QVERIFY(scope.contains(QStringLiteral("enabled: true")));
        QVERIFY2(!norm.contains(QStringLiteral("Voice calls are coming soon")),
                 "the coming-soon wording must not outlive the disabled state");
        // The ENGINE gate stays load-bearing, but it now lives inside
        // canStartCall(): on a packaged build with no GStreamer plugins
        // neither lane is available, so the button is ABSENT rather than
        // present and dead. What this asserts is that the gate is in the
        // VISIBILITY, not merely in the click handler — a button that
        // appears and then refuses is the failure mode being prevented.
        const int visible = scope.indexOf(QStringLiteral("visible:"));
        const int enabled = scope.indexOf(QStringLiteral("enabled: true"));
        QVERIFY(visible >= 0 && enabled > visible);
        QVERIFY(scope.mid(visible, enabled - visible)
                    .contains(QStringLiteral("app.canStartCall(")));
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

    // A REAL instantiation of the bubble row with a real participant list,
    // because a source scan cannot see a binding against a token that does not
    // exist, a delegate that lays out to nothing, or a row that reserves space
    // while the call is still connecting. (An undeclared AppTheme token is
    // exactly the class of mistake that shipped hundreds of "Unable to assign
    // [undefined]" warnings past every offscreen gate once already.)
    void speakerBubblesInstantiateAndSizeThemselves()
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
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        QSignalSpy createdSpy(&engine,
                              &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("CallSpeakerBubbles"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(3000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr, "CallSpeakerBubbles must instantiate");

        // Nobody yet: no strip of empty space above the stage.
        QCOMPARE(root->property("height").toDouble(), 0.0);
        QCOMPARE(root->property("visible").toBool(), false);

        // Two people, one of them speaking and muted-unknown.
        QVariantList people;
        QVariantMap one;
        one.insert(QStringLiteral("identity"), QStringLiteral("PA_one"));
        one.insert(QStringLiteral("userId"), QStringLiteral("@alice:mock.local"));
        one.insert(QStringLiteral("displayName"), QStringLiteral("Alice"));
        one.insert(QStringLiteral("speaking"), true);
        one.insert(QStringLiteral("local"), true);
        people.append(one);
        QVariantMap two;
        two.insert(QStringLiteral("identity"), QStringLiteral("PA_two"));
        two.insert(QStringLiteral("userId"), QStringLiteral("@bob:mock.local"));
        two.insert(QStringLiteral("displayName"), QStringLiteral("Bob"));
        two.insert(QStringLiteral("micKnown"), true);
        two.insert(QStringLiteral("micMuted"), true);
        two.insert(QStringLiteral("screenSharing"), true);
        people.append(two);
        root->setProperty("people", people);
        root->setWidth(300);
        QCoreApplication::processEvents();
        root->polish();
        QCoreApplication::processEvents();

        QVERIFY2(root->property("height").toDouble() > 0.0,
                 "the bubble row has no height with people in the call");
        auto *strip =
            root->findChild<QQuickItem *>(QStringLiteral("callSpeakerBubbles"));
        QVERIFY(strip != nullptr);
        QCOMPARE(strip->property("count").toInt(), 2);
        for (const QString &warning : warnings) {
            QVERIFY2(!warning.contains(QStringLiteral("Unable to assign"))
                         && !warning.contains(QStringLiteral("is not available")),
                     qPrintable(warning));
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

    // 2026-08-23 reporter round: FOUR call surfaces were on screen at once
    // for one call — the header bar, the stage's own control bar, the Voice
    // Connected strip, and a "You are in a call" banner still offering Join.
    // Each of these pins one of them shut.
    void exactlyOneSurfaceOwnsTheMediaControls()
    {
        // EVERY call control lives in the header bar. The stage carries none.
        //
        // This started as "the stage may keep the controls that have nowhere
        // else to live" — layout toggle, raise hand, participants — and that
        // was wrong: once the media controls moved up, what was left on the
        // stage were two orphan buttons floating under the call UI, which is
        // exactly how it was reported. So the assertion is now the stronger
        // one: no control bar on the stage at all.
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        QVERIFY2(!stage.contains(QStringLiteral("CallControlBar {")),
                 "the call stage instantiates a control bar again");

        // The stage's dock is the SAME component in another placement, not a
        // second control bar — that distinction is the whole lesson here. And
        // the header instance stands down while the stage is showing, so the
        // controls are never drawn twice.
        QVERIFY(stage.contains(QStringLiteral("CallHeaderBar {")));
        QVERIFY(stage.contains(QStringLiteral("placement: \"dock\"")));

        // ...and the header owns the full set.
        const QString header =
            read(QStringLiteral(QML_DIR "/CallHeaderBar.qml"));
        QVERIFY(!header.isEmpty());
        QVERIFY(header.contains(QStringLiteral("stageOwnsControls")));
        QVERIFY(header.contains(QStringLiteral("root.dock || !root.stageOwnsControls")));
        for (const QString &button : { QStringLiteral("callBarMicButton"),
                                       QStringLiteral("callBarDeafenButton"),
                                       QStringLiteral("callBarCameraButton"),
                                       QStringLiteral("callBarScreenShareButton"),
                                       QStringLiteral("callBarHandButton"),
                                       QStringLiteral("callBarParticipantsButton"),
                                       QStringLiteral("callBarHangUpButton") }) {
            QVERIFY2(header.contains(button),
                     qPrintable(button + " is missing from the header bar"));
        }
    }

    // The spotlight is where a screen share lands, and it used to draw a glyph
    // and the words "someone is sharing their screen" over an empty rectangle
    // — announcing a share it never rendered. Reported as "I couldn't see
    // their screenshare"; the receive path was only half the cause.
    void theSpotlightRendersVideoRatherThanDescribingIt()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const int spotlight =
            stage.indexOf(QStringLiteral("objectName: \"callSpotlight\""));
        QVERIFY(spotlight >= 0);
        const QString block = stage.mid(spotlight, 3000);
        QVERIFY2(block.contains(QStringLiteral("CallParticipantTile {")),
                 "the spotlight draws no video surface at all");
        // It shows the SHARE when there is one, and it says which track it is
        // asking for — a surface that asked for the camera would render a face
        // where the user asked for a screen.
        QVERIFY(block.contains(QStringLiteral("mediaKind: root.spotlightKind")));
        QVERIFY(stage.contains(QStringLiteral("readonly property string spotlightKind")));
        QVERIFY(stage.contains(QStringLiteral("\"screen\" : \"camera\"")));
        // The old placeholder wording must not survive next to a real surface,
        // or the stage claims a share is unviewable while showing it.
        QVERIFY(!stage.contains(
            QStringLiteral("Someone is sharing their screen")));
    }

    // A camera and a screen share are two tracks from one person, and one
    // surface can only render one of them. The tile therefore says which, and
    // routing goes through the TRACK's key, not the participant's — a
    // participant-keyed route can only ever feed one surface.
    void aScreenShareAndACameraAreRoutedAsSeparateTracks()
    {
        const QString tile =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        QVERIFY(!tile.isEmpty());
        QVERIFY(tile.contains(QStringLiteral("property string mediaKind")));
        QVERIFY(tile.contains(QStringLiteral("attachScreenSink(")));
        QVERIFY(tile.contains(QStringLiteral("attachLocalScreenSink(")));
        QVERIFY(tile.contains(QStringLiteral("detachScreenSink(")));
        QVERIFY(tile.contains(QStringLiteral("detachLocalScreenSink(")));
        // Re-attached when the routing key arrives: the SFU can announce a
        // participant before it says which media section their tracks landed
        // on, and an attach made under an empty key never gets a frame.
        QVERIFY(tile.contains(QStringLiteral("onActiveTrackKeyChanged:")));
        // A shared screen is content: it is fitted, never cropped, or the
        // edges of what the other person is showing are hidden.
        QVERIFY(tile.contains(QStringLiteral("VideoOutput.PreserveAspectFit")));
    }

    // Discord's bubble row: who is here and who is talking, above the stage.
    // The ring is driven by the SFU's OWN speaker updates — no local audio is
    // inspected to produce it.
    void theCallStageCarriesSpeakerBubbles()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        const QString bubbles =
            read(QStringLiteral(QML_DIR "/CallSpeakerBubbles.qml"));
        QVERIFY(!stage.isEmpty());
        QVERIFY(!bubbles.isEmpty());
        QVERIFY(stage.contains(QStringLiteral("CallSpeakerBubbles {")));
        QVERIFY(bubbles.contains(QStringLiteral("objectName: \"callSpeakerBubbles\"")));
        QVERIFY(bubbles.contains(QStringLiteral("Avatar {")));
        // The ring is bound to the model's speaking flag, never to anything
        // measured here.
        QVERIFY(bubbles.contains(QStringLiteral("modelData.speaking === true")));
        QVERIFY(bubbles.contains(QStringLiteral("border.color: AppTheme.success")));
        // Initials come from the real name, never the "You" label — that
        // would render a Y for the local user.
        QVERIFY(bubbles.contains(QStringLiteral("name: bubble.modelData.displayName")));
    }

    // Stopping one published track must never stop another. "Unpublish the
    // last track we published" is the screen share whenever the share started
    // after the camera, so turning the camera off killed the share and left
    // the camera live — the LED being the user's only honest indicator.
    void stoppingOneTrackNamesItRatherThanTakingTheLastOne()
    {
        QFile file(QStringLiteral(SRC_DIR "/calls/SfuCallController.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY(source.contains(QStringLiteral("unpublishTrack(m_cameraCid)")));
        QVERIFY(source.contains(QStringLiteral("unpublishTrack(m_screenCid)")));
        // The old shape: a reverse scan of the published list that breaks on
        // the first entry, i.e. "the last one published".
        QVERIFY2(!source.contains(QStringLiteral(
                     "for (int i = m_publishedTrackIds.size() - 1")),
                 "a track is still stopped by taking the last published one");
    }

    // A voice call is circular avatars on the canvas, not a grid of empty
    // bordered panels — the shape the maintainer asked for, with a reference
    // screenshot. A tile only becomes a panel when it has video to hold.
    void aVoiceOnlyCallDrawsAvatarsNotPanels()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        const QString tile =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        QVERIFY(!stage.isEmpty());
        QVERIFY(!tile.isEmpty());
        QVERIFY(stage.contains(QStringLiteral("readonly property bool voiceOnly")));
        QVERIFY(stage.contains(QStringLiteral("bare: root.voiceOnly")));
        QVERIFY(tile.contains(QStringLiteral("property bool bare")));
        // Selection and keyboard focus still draw: those are states the user
        // caused and must be able to see.
        QVERIFY(tile.contains(QStringLiteral("readonly property bool _drawsCard")));
        QVERIFY(tile.contains(QStringLiteral("root.focused || root.activeFocus")));
    }

    void theJoinButtonActuallyJoins()
    {
        // THE reported defect. This handler was an empty block with a
        // comment saying no join path existed yet, long after one did: the
        // button rendered, looked enabled, and did nothing.
        const QString banner =
            read(QStringLiteral(QML_DIR "/RoomCallBanner.qml"));
        QVERIFY(!banner.isEmpty());
        const int at = banner.indexOf(QStringLiteral("roomCallJoinButton"));
        QVERIFY2(at >= 0, "the Join button is gone");
        // Comments stripped, then a generous window: the explanatory comment
        // above this handler is longer than the code and pushed the call out
        // of a fixed-size slice.
        QString body = banner.mid(at, 900);
        body.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
        body = normalized(body);
        QVERIFY2(body.contains(QStringLiteral("app.groupCall.join(")),
                 "the Join button does not call join()");
        QVERIFY2(!body.contains(QStringLiteral("onClicked: { }")),
                 "the Join handler is empty again");
    }

    void theBannerStandsDownOnceThisDeviceIsInTheCall()
    {
        // Offering "Join" to someone already in the call is nonsense, and
        // the header bar owns the call at that point.
        const QString banner =
            normalized(read(QStringLiteral(QML_DIR "/RoomCallBanner.qml")));
        QVERIFY2(banner.contains(QStringLiteral(
                     "visible: hasCall && !ownDeviceHere && !locallyInCall")),
                 "the banner does not stand down for this device's own call");
        // ownDEVICE, not ownUser: the same account on another device is a
        // real other participant and this device should still be offered a
        // way in.
        QVERIFY2(banner.contains(QStringLiteral("ownDeviceInSession(")),
                 "the banner keys on the account rather than the device");
    }

    // The stage is documented as REPLACING the timeline, and it was only
    // ever ADDED beside it. Both are `Layout.fillHeight` in one ColumnLayout,
    // so the column was SPLIT between them: the stage got roughly half, its
    // spotlight collapsed to a ~45px strip with the avatar and the "Back to
    // grid" button piled on top of each other, and message rows kept drawing
    // underneath the call's control dock. Every reported "the call UI is
    // broken" screenshot is this.
    void theCallStageOwnsTheColumnRatherThanSharingIt()
    {
        const QString pane =
            normalized(read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY(!pane.isEmpty());
        // ONE condition, so the stage and everything it replaces cannot
        // drift apart again.
        QVERIFY2(pane.contains(QStringLiteral(
                     "readonly property bool callStageOwnsColumn: "
                     "app.groupCall.active && app.groupCall.roomId === "
                     "app.currentRoomId")),
                 "TimelinePane has no single call-stage ownership condition");
        QVERIFY2(pane.contains(QStringLiteral("active: root.callStageOwnsColumn")),
                 "the call stage host does not read the ownership condition");
        QVERIFY2(pane.contains(QStringLiteral("visible: !root.callStageOwnsColumn")),
                 "the timeline does not stand down while the call stage owns "
                 "the column, so the two share its height");
        QVERIFY2(pane.contains(QStringLiteral(
                     "visible: app.currentRoomId !== \"\" && "
                     "!root.callStageOwnsColumn")),
                 "the composer does not stand down under the call stage");
    }

    void theVoiceStripOnlyShowsWhenTheCallIsElsewhere()
    {
        // Its whole purpose is surviving a walk away from the call's room.
        // Inside that room it was a third copy of the same call.
        const QString strip =
            normalized(read(QStringLiteral(QML_DIR "/VoiceConnectedBar.qml")));
        QVERIFY(!strip.isEmpty());
        QVERIFY2(strip.contains(QStringLiteral(
                     "app.groupCall.roomId === app.currentRoomId")),
                 "the Voice Connected strip does not exclude the call's own "
                 "room");
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
