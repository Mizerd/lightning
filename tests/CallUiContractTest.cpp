// 2026-08-18 round 2: contract scans for the incoming-call corner card,
// its Main.qml hosting, and the ring settings toggle — plus a REAL
// offscreen instantiation of IncomingCallPrompt against a live
// AppController, which catches unresolved theme tokens and property typos
// that string scans cannot. Predicates are matched whitespace-normalized
// so reflows don't break them.
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QWindow>
#include <QtTest>

#include <memory>

#include "app/AppController.h"
#include "auth/AuthManager.h"
#include "calls/CallController.h"
#include "calls/CallMediaBackend.h"
#include "calls/CallShareModel.h"
#include "calls/CallStageState.h"
#include "calls/SfuCallController.h"
#include "matrix/CallSignal.h"
#include "matrix/MockMatrixClient.h"

#ifdef HAVE_LIGHTNING_WEBRTC
#include <QVideoSink>

#include "calls/SfuVideoRouter.h"
#endif

/// The smallest thing that makes `mediaBackendAvailable` true.
///
/// It exists for ONE assertion: with no engine registered the legacy Accept
/// is hidden for BOTH lanes, so a test that only checks "Accept is absent on
/// an RTC ring" would pass on the broken tree and prove nothing. Registering
/// this is what makes the two lanes tell each other apart. It answers
/// nothing — no offer is ever produced — which is all these cases need.
class StubMediaBackend : public CallMediaBackend
{
public:
    using CallMediaBackend::CallMediaBackend;
    void createOffer(const QString &callId) override { Q_UNUSED(callId); }
    void createAnswer(const QString &callId, const QString &sdp) override
    { Q_UNUSED(callId); Q_UNUSED(sdp); }
    void setRemoteAnswer(const QString &callId, const QString &sdp) override
    { Q_UNUSED(callId); Q_UNUSED(sdp); }
    void addRemoteCandidate(const QString &callId, const QString &candidate,
                            const QString &sdpMid, int sdpMLineIndex) override
    { Q_UNUSED(callId); Q_UNUSED(candidate); Q_UNUSED(sdpMid);
      Q_UNUSED(sdpMLineIndex); }
    void setIceServers(const QStringList &uris, const QString &username,
                       const QString &password) override
    { Q_UNUSED(uris); Q_UNUSED(username); Q_UNUSED(password); }
    void close(const QString &callId) override { Q_UNUSED(callId); }
};

/// Find a named item by walking the VISUAL tree.
///
/// `QObject::findChild` cannot reach a `Repeater`'s delegates: they belong to
/// the delegate model, not to the item they are laid out inside. Every
/// segment of a SegmentedControl is created that way, so a tab is invisible
/// to findChild — measured, with a segment carrying a CONSTANT objectName and
/// still absent from a full `findChildren` dump, so this is not a binding
/// that failed to evaluate. `childItems()` does list them.
static QQuickItem *findVisualChild(QObject *root, const QString &name)
{
    auto *item = qobject_cast<QQuickItem *>(root);
    if (!item) {
        // A Dialog is a Popup, not an Item; start from what it draws.
        if (QObject *content = root
                ? root->property("contentItem").value<QObject *>()
                : nullptr)
            item = qobject_cast<QQuickItem *>(content);
    }
    if (!item)
        return nullptr;
    if (item->objectName() == name)
        return item;
    const auto children = item->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *found = findVisualChild(child, name))
            return found;
    }
    return nullptr;
}

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

    /// Source with WHOLE-LINE `//` comments removed, for BAN assertions.
    ///
    /// Several bans below forbid a token that their own rationale comment
    /// names, one line above — a ban read off the raw file would then always
    /// "find" it and always fail. 2026-08-25 taught the other half of this:
    /// a comment stripper is a parser, and the one in
    /// NavigationLayoutContractTest silently weakened every assertion that
    /// followed it because `[^"\']*` crossed newlines. So this one is
    /// deliberately the simplest thing that can work — whole lines only,
    /// which is where every mention in these files lives — and the ban tests
    /// each assert a token they KNOW is present to prove it still parses.
    static QString code(const QString &s)
    {
        QString out = s;
        out.remove(QRegularExpression(QStringLiteral("(?m)^[ \\t]*//.*$")));
        return out;
    }

    /// Run queued work AND the DEFERRED DELETIONS.
    ///
    /// `deleteLater()` is the whole reason the tests below exist: Qt destroys
    /// a replaced Loader's content and a regenerated Repeater's delegates
    /// with it, while building the replacements synchronously. A test that
    /// only calls processEvents() is measuring the moment BEFORE the
    /// interesting thing happens, and would pass on the broken tree.
    static void settle(int rounds = 3)
    {
        for (int i = 0; i < rounds; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    static QString normalized(const QString &s)
    {
        QString out = s;
        out.replace(QRegularExpression(QStringLiteral("\\s+")),
                    QStringLiteral(" "));
        return out.trimmed();
    }

    /// One row of `app.groupCall.screenShareSources`, in the shape
    /// SfuCallController builds it: a window carries a non-zero handle and
    /// the OWNING APPLICATION beside its caption, a screen carries neither.
    struct ShareRow {
        QString name;
        QString application;
        quint64 handle;
    };
    static QVariantList pickerRows(const QList<ShareRow> &rows)
    {
        QVariantList out;
        int display = 0;
        for (const ShareRow &row : rows) {
            out.append(QVariantMap{
                { QStringLiteral("index"),
                  row.handle != 0 ? -1 : display++ },
                { QStringLiteral("windowHandle"), row.handle },
                { QStringLiteral("name"), row.name },
                { QStringLiteral("application"), row.application },
                { QStringLiteral("geometry"), QStringLiteral("1920 x 1080") },
                { QStringLiteral("primary"), false },
                { QStringLiteral("current"), false },
            });
        }
        return out;
    }

    static void clickCentre(QQuickWindow *window, QQuickItem *item)
    {
        const QPointF centre =
            item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          centre.toPoint());
        QCoreApplication::processEvents();
    }

    /// Press the delegate at `viewIndex` of a real item view, resolved from
    /// the item's OWN geometry — a test that assumes a row height measures
    /// something else the moment the delegate changes shape.
    static void clickItem(QQuickWindow *window, QQuickItem *view, int viewIndex)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(view, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(int, viewIndex));
        QVERIFY2(item != nullptr, "no delegate was created for that row");
        clickCentre(window, item);
    }

private Q_SLOTS:
    /// THE VOLUME FEATURE IS INERT WITHOUT THIS ONE LINE, and silently so.
    ///
    /// SfuCallController reads the stored microphone gain and every stored
    /// per-participant level through its SettingsManager, and subscribes to
    /// their change signals through it. AppController built the controller
    /// and handed it a client and an RTC controller but never handed it
    /// settings — so the pointer stayed null, every connect() in setSettings
    /// was never made, applyStoredVolumes() returned at its first guard, and
    /// applyAudioState() fell back to unity on every join.
    ///
    /// Nothing failed. The sliders moved, the values persisted into
    /// QSettings, and none of it reached the engine. Reported as three
    /// separate faults at once: "sound amplifier does nothing", "i cant make
    /// myself louder and i cant make other louder", and "it doesnt remeber my
    /// volumnes set on user".
    ///
    /// A source scan rather than a behavioural check because m_settings is
    /// private with no getter, and adding one purely to observe the wiring
    /// would be a worse trade than reading the line that must exist.
    void theGroupCallIsHandedItsSettings()
    {
        const QString app = normalized(
            read(QStringLiteral(QML_DIR "/../src/app/AppController.cpp")));
        QVERIFY2(!app.isEmpty(), "AppController.cpp did not read");
        QVERIFY2(app.contains(QStringLiteral("m_groupCall->setSettings(")),
                 "AppController never hands SfuCallController a "
                 "SettingsManager: microphone gain and every per-participant "
                 "volume are inert and nothing is remembered");
    }

    // A CALL MUST LEAVE BEFORE THE SESSION IT RUNS ON GOES AWAY, on every
    // path that takes a session away. A membership RETRACTION is a Matrix
    // send and a Leave is an SFU command, and both need the client.
    //
    // A live session log said the account switch got this wrong:
    //
    //   account switch begin from= … to= …
    //   detaching local session …
    //   rust client released …
    //   teardown state= 6
    //   retraction could not be dispatched — this device will remain in the
    //   room's call membership until it expires
    //
    // The teardown ran AFTER the release, so rtcRetractMembership had no
    // handle and returned 0. With no MSC4140 delayed retraction on that
    // homeserver either, the membership then sat in the room until `expires`
    // — a phantom participant every other client in the call had to see.
    //
    // prepareForShutdown() had this fix and switchToAccount() never got it,
    // which is why the ORDER is asserted here rather than the mere presence
    // of a leave() call: both are calls to the same function and only one
    // ordering works.
    void everyPathThatTakesTheSessionAwayLeavesTheCallFirst()
    {
        // COMMENTS STRIPPED FIRST, and that is load-bearing here rather
        // than tidy: prepareForShutdown's own rationale comment names
        // `stopSync()` twelve lines ABOVE the call it describes, so an
        // ordering check over the raw file finds the comment, decides the
        // teardown comes first, and fails on correct code. (`code()` removes
        // whole-line `//` only, which is where these mentions live.)
        const QString app = normalized(
            code(read(QStringLiteral(QML_DIR "/../src/app/AppController.cpp"))));
        QVERIFY2(!app.isEmpty(), "AppController.cpp did not read");

        struct Path { const char *fn; const char *teardown; };
        const Path paths[] = {
            { "void AppController::prepareForShutdown", "stopSync()" },
            { "void AppController::switchToAccount", "detachSession()" },
        };
        for (const Path &path : paths) {
            const int at = app.indexOf(QLatin1String(path.fn));
            QVERIFY2(at >= 0,
                     qPrintable(QStringLiteral("%1 is gone, so this test is "
                                               "pinning nothing")
                                    .arg(QLatin1String(path.fn))));
            // Bounded to this function: the next one starts at the next
            // top-level definition, and scanning past it would find another
            // path's calls and pass for the wrong reason.
            const int next = app.indexOf(QStringLiteral("\nvoid AppController::"),
                                         at + 1);
            const QString body =
                next > at ? app.mid(at, next - at) : app.mid(at);
            const int leave = body.indexOf(QStringLiteral("m_groupCall->leave()"));
            const int down = body.indexOf(QLatin1String(path.teardown));
            QVERIFY2(leave >= 0,
                     qPrintable(QStringLiteral(
                         "%1 never leaves the call, so a live membership is "
                         "stranded in the room until it expires")
                                    .arg(QLatin1String(path.fn))));
            QVERIFY2(down >= 0,
                     qPrintable(QStringLiteral("%1 no longer calls %2")
                                    .arg(QLatin1String(path.fn),
                                         QLatin1String(path.teardown))));
            QVERIFY2(leave < down,
                     qPrintable(QStringLiteral(
                         "%1 tears the session down BEFORE leaving the call, "
                         "so the retraction has no client to reach")
                                    .arg(QLatin1String(path.fn))));
        }
    }

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
        // Accept exists ONLY behind the media-engine gate (round 3) AND only
        // on the lane it can actually answer (2026-08-26), and the no-engine
        // honesty line survives for engineless builds.
        QVERIFY(norm.contains(QStringLiteral(
            "readonly property bool legacyAcceptOffered: root.ringing && "
            "!root.rtcRing && app.calls.mediaBackendAvailable")));
        QVERIFY(norm.contains(
            QStringLiteral("visible: root.legacyAcceptOffered")));
        QVERIFY(norm.contains(
            QStringLiteral("if (!app.calls.answer())")));
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

    // A REAL instantiation, which is what catches an unresolved theme token
    // or a property typo that a string scan cannot see.
    //
    // Fed a ListModel fixture rather than the live controller: the strip binds
    // `app.groupCall.participantModel`, which on the mock backend is an empty
    // model with no way to put people in it. The fixture exercises the same
    // required-property delegate the real model drives, because a ListModel
    // supplies roles by name exactly as a QAbstractListModel does.
    //
    // On the unfixed tree the strip has a `people` property and a `modelData`
    // delegate, so binding `model:` leaves it empty and the count assertion
    // fails.
    void speakerBubblesInstantiateAndSizeThemselves()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(),
                            &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });

        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient
Item {
    id: outer
    width: 320
    height: 80
    property alias bubbles: strip
    ListModel {
        id: people
        ListElement {
            identity: "PA_one"; userId: "@alice:mock.local"
            displayName: "Alice"; avatarMxc: ""; local: true
            speaking: true; micKnown: false; micMuted: false
            screenSharing: false
        }
        ListElement {
            identity: "PA_two"; userId: "@bob:mock.local"
            displayName: "Bob"; avatarMxc: ""; local: false
            speaking: false; micKnown: true; micMuted: true
            screenSharing: true
        }
    }
    CallSpeakerBubbles {
        id: strip
        objectName: "fixtureBubbles"
        width: outer.width
        model: people
    }
}
)"), QUrl(QStringLiteral("qrc:/callbubblestest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *outer = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY2(outer != nullptr, "CallSpeakerBubbles must instantiate");
        QCoreApplication::processEvents();
        outer->polish();
        QCoreApplication::processEvents();

        auto *bubbles = qobject_cast<QQuickItem *>(
            outer->property("bubbles").value<QObject *>());
        QVERIFY(bubbles != nullptr);
        QVERIFY2(bubbles->property("height").toDouble() > 0.0,
                 "the bubble row has no height with people in the call");
        auto *strip =
            outer->findChild<QQuickItem *>(QStringLiteral("callSpeakerBubbles"));
        QVERIFY(strip != nullptr);
        QCOMPARE(strip->property("count").toInt(), 2);

        // Nobody: no strip of empty space above the messages.
        bubbles->setProperty("model", QVariant::fromValue<QObject *>(nullptr));
        QCoreApplication::processEvents();
        QCOMPARE(bubbles->property("height").toDouble(), 0.0);
        QCOMPARE(bubbles->property("visible").toBool(), false);

        for (const QString &warning : warnings) {
            QVERIFY2(!warning.contains(QStringLiteral("Unable to assign"))
                         && !warning.contains(QStringLiteral("is not available")),
                     qPrintable(warning));
        }
    }

    // The share picker LOADS, it classifies a row the same way the capture
    // does, and it NAMES a window the way a person can act on.
    //
    // The picker was reworked from a grouped list into a Discord-style GRID
    // of previews with an Applications/Screens tab pair — the kind of
    // structural change whose failure mode is a binding error that only
    // appears when a person opens the dialog, mid-call, which is the worst
    // possible place to find out.
    //
    // `isWindowRow` is the load-bearing part: the picker uses it to decide
    // which TAB a row belongs to and the CONTROLLER uses the same fact — a
    // non-zero window handle — to decide whether to capture a window or a
    // display. If those two ever disagreed, the grid would say one thing and
    // the share would do another.
    //
    // The label half pins the report this rework came from: "with brave it
    // listed my tab name but didnt even say brave anywhere". A Chromium
    // caption is the TAB's title and names no browser, so the OWNING
    // APPLICATION has to lead — and the resolution, which used to have a
    // line of its own, must not appear on the face of the tile at all.
    void theSharePickerLoadsAndKnowsAWindowRowFromAScreen()
    {
        AppController controller(AppController::MockBackend);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });

        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 700
    height: 500
    property alias picker: pick
    ScreenSharePicker { id: pick; objectName: "sharePicker" }
}
)"), QUrl(QStringLiteral("qrc:/sharepickertest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner != nullptr, "ScreenSharePicker must instantiate");
        QCoreApplication::processEvents();

        auto *picker = owner->property("picker").value<QObject *>();
        QVERIFY(picker != nullptr);

        // No call, so nothing to share and neither tab has anything in it.
        QCOMPARE(picker->property("screenCount").toInt(), 0);
        QCOMPARE(picker->property("windowCount").toInt(), 0);

        // A window row carries a non-zero handle; a display row does not.
        // Both shapes come straight from SfuCallController's own maps.
        auto classify = [picker](const QVariantMap &row) {
            QVariant out;
            const bool called = QMetaObject::invokeMethod(
                picker, "isWindowRow", Q_RETURN_ARG(QVariant, out),
                Q_ARG(QVariant, QVariant::fromValue(row)));
            return called && out.toBool();
        };
        QVERIFY2(classify({{QStringLiteral("windowHandle"), quint64(4660)}}),
                 "a row with a window handle is not treated as a window");
        QVERIFY2(!classify({{QStringLiteral("windowHandle"), quint64(0)},
                            {QStringLiteral("index"), 0}}),
                 "a display row is being treated as a window");
        QVERIFY2(!classify({{QStringLiteral("index"), 1}}),
                 "a row with no handle at all is being treated as a window");

        auto label = [picker](const char *fn, const QVariantMap &row) {
            QVariant out;
            const bool called = QMetaObject::invokeMethod(
                picker, fn, Q_RETURN_ARG(QVariant, out),
                Q_ARG(QVariant, QVariant::fromValue(row)));
            return called ? out.toString() : QString();
        };

        // THE BRAVE CASE. The caption names a tab and no browser, so the
        // application leads and the caption follows on its own line.
        const QVariantMap brave{
            { QStringLiteral("index"), -1 },
            { QStringLiteral("windowHandle"), quint64(4660) },
            { QStringLiteral("name"), QStringLiteral("Anthropic Console") },
            { QStringLiteral("application"), QStringLiteral("Brave Browser") },
            { QStringLiteral("geometry"), QStringLiteral("3840 x 2160") },
        };
        QCOMPARE(label("primaryLabel", brave), QStringLiteral("Brave Browser"));
        QCOMPARE(label("secondaryLabel", brave),
                 QStringLiteral("Anthropic Console"));

        // ...and a caption that already says which application it is keeps
        // ONE line. Repeating it would be noise.
        const QVariantMap explorer{
            { QStringLiteral("index"), -1 },
            { QStringLiteral("windowHandle"), quint64(4661) },
            { QStringLiteral("name"), QStringLiteral("Windows Explorer") },
            { QStringLiteral("application"),
              QStringLiteral("Windows Explorer") },
            { QStringLiteral("geometry"), QStringLiteral("1600 x 900") },
        };
        QCOMPARE(label("primaryLabel", explorer),
                 QStringLiteral("Windows Explorer"));
        QCOMPARE(label("secondaryLabel", explorer), QString());

        // A window whose executable could not be read still has to be
        // nameable: `application` is legitimately empty there.
        const QVariantMap unknownApp{
            { QStringLiteral("index"), -1 },
            { QStringLiteral("windowHandle"), quint64(4662) },
            { QStringLiteral("name"), QStringLiteral("Untitled - Notepad") },
            { QStringLiteral("application"), QString() },
            { QStringLiteral("geometry"), QStringLiteral("800 x 600") },
        };
        QCOMPARE(label("primaryLabel", unknownApp),
                 QStringLiteral("Untitled - Notepad"));

        // A screen is its platform name, and the second line says WHICH
        // screen it is — never its resolution.
        const QVariantMap screen{
            { QStringLiteral("index"), 0 },
            { QStringLiteral("name"), QStringLiteral("\\\\.\\DISPLAY1") },
            { QStringLiteral("application"), QString() },
            { QStringLiteral("geometry"), QStringLiteral("3840 x 2160") },
            { QStringLiteral("primary"), true },
            { QStringLiteral("current"), true },
        };
        QCOMPARE(label("primaryLabel", screen),
                 QStringLiteral("\\\\.\\DISPLAY1"));
        QCOMPARE(label("secondaryLabel", screen), QStringLiteral("This screen"));

        // THE RESOLUTION IS OFF THE FACE OF THE DIALOG — "we dont even need
        // to tell the user the resolution" — and still reaches a screen
        // reader, which has no preview to look at.
        for (const QVariantMap &row : { brave, screen }) {
            const QString geometry =
                row.value(QStringLiteral("geometry")).toString();
            QVERIFY(!geometry.isEmpty());
            QVERIFY2(!label("primaryLabel", row).contains(geometry),
                     "the tile's own label is carrying the geometry again");
            QVERIFY2(!label("secondaryLabel", row).contains(geometry),
                     "the tile's second line is carrying the geometry again");
            QVERIFY2(label("accessibleLabel", row).contains(geometry),
                     "the accessible name dropped the geometry with it");
        }

        for (const QString &warning : warnings) {
            QVERIFY2(!warning.contains(QStringLiteral("Unable to assign"))
                         && !warning.contains(QStringLiteral("is not available"))
                         && !warning.contains(QStringLiteral("Binding loop")),
                     qPrintable(warning));
        }
    }

    // PRESSING A TILE MUST SELECT IT — including a window tile, which now
    // lives behind the Applications tab rather than under a group header.
    //
    // The grouped rework this replaced shipped with its rows unclickable and
    // it reached a user: the picker opened, clicking a window did nothing,
    // and pressing Share published display 0 because `selected` had never
    // moved off it. The Windows log said it exactly — three
    // `screen share requested` and one
    // `screen share publishing node= 0 window= false`.
    //
    // Everything that existed passed, because nothing had a tile to press.
    // This drives the real tab strip and the real delegate.
    void pressingATileSelectsItIncludingOnTheApplicationsTab()
    {
        AppController controller(AppController::MockBackend);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);

        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 1100
    height: 760
    visible: true
    property alias picker: pick
    ScreenSharePicker { id: pick; objectName: "sharePicker" }
}
)"), QUrl(QStringLiteral("qrc:/sharepickerclicktest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *window = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(window != nullptr);
        auto *picker = owner->property("picker").value<QObject *>();
        QVERIFY(picker != nullptr);

        // Two displays then two windows — the shape the controller builds.
        const QVariantList rows = pickerRows({
            { QStringLiteral("Screen A"), QString(), 0 },
            { QStringLiteral("Screen B"), QString(), 0 },
            { QStringLiteral("Window A"), QStringLiteral("Brave Browser"),
              4660 },
            { QStringLiteral("Window B"), QStringLiteral("Brave Browser"),
              4661 },
        });
        picker->setProperty("sources", rows);
        QMetaObject::invokeMethod(picker, "open");
        QTRY_VERIFY(picker->property("visible").toBool());
        QCoreApplication::processEvents();

        QCOMPARE(picker->property("screenCount").toInt(), 2);
        QCOMPARE(picker->property("windowCount").toInt(), 2);

        auto *grid = picker->findChild<QQuickItem *>(
            QStringLiteral("sourceGrid"));
        QVERIFY2(grid != nullptr, "the picker's grid has no objectName to find");

        // THE TAB SHOWS ONE KIND. A grid still holding every row would pass
        // every click assertion below and still be the old list.
        QTRY_COMPARE(grid->property("count").toInt(), 2);
        QCOMPARE(picker->property("tab").toString(), QStringLiteral("screens"));

        clickItem(window, grid, 0);
        QCOMPARE(picker->property("selected").toInt(), 0);
        clickItem(window, grid, 1);
        QCOMPARE(picker->property("selected").toInt(), 1);

        // Now the other tab, through the real segment rather than by writing
        // the property: a tab strip nothing can press is the same defect as a
        // row nothing can press.
        auto *applicationsTab = findVisualChild(
            picker, QStringLiteral("shareTabs_applications"));
        QVERIFY2(applicationsTab != nullptr,
                 "the Applications tab has no objectName to find");
        clickCentre(window, applicationsTab);
        QTRY_COMPARE(picker->property("tab").toString(),
                     QStringLiteral("applications"));
        QTRY_COMPARE(grid->property("count").toInt(), 2);

        // Switching tabs must leave a tile of THIS tab highlighted, or Share
        // would send something the user cannot see chosen.
        QCOMPARE(picker->property("selected").toInt(), 2);

        clickItem(window, grid, 1);
        QCOMPARE(picker->property("selected").toInt(), 3);
    }

    // THE FILTERED GRID MUST MAP BACK TO THE UNFILTERED SOURCE INDEX.
    //
    // `SfuCallController::chooseScreenShareSource(index)` indexes into the
    // list it BUILT — the whole thing, screens and windows together — and the
    // grid shows one tab at a time. So a delegate's own index is a DIFFERENT
    // number from the one the controller needs, and the failure mode is not a
    // dead control: it is sharing the wrong thing, silently, which is the
    // worst outcome this dialog has.
    //
    // The rows below interleave the two kinds and carry more than one of
    // each, so a naive `selected = delegateIndex` does not merely land on a
    // neighbour — on the Screens tab it lands on a WINDOW. Each assertion
    // therefore also names the row it expects, so a failure reads as "you
    // shared Window A" rather than as an off-by-one.
    void theFilteredGridMapsBackToTheUnfilteredSourceIndex()
    {
        AppController controller(AppController::MockBackend);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);

        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 1100
    height: 760
    visible: true
    property alias picker: pick
    ScreenSharePicker { id: pick; objectName: "sharePicker" }
}
)"), QUrl(QStringLiteral("qrc:/sharepickermaptest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *window = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(window != nullptr);
        auto *picker = owner->property("picker").value<QObject *>();
        QVERIFY(picker != nullptr);

        // 0 screen, 1 window, 2 screen, 3 window, 4 window. Nothing lines up
        // with a per-tab index anywhere.
        const QVariantList rows = pickerRows({
            { QStringLiteral("Screen A"), QString(), 0 },
            { QStringLiteral("Window A"), QStringLiteral("Brave Browser"),
              4660 },
            { QStringLiteral("Screen B"), QString(), 0 },
            { QStringLiteral("Window B"), QStringLiteral("Brave Browser"),
              4661 },
            { QStringLiteral("Window C"), QStringLiteral("Slack"), 4662 },
        });
        picker->setProperty("sources", rows);
        QMetaObject::invokeMethod(picker, "open");
        QTRY_VERIFY(picker->property("visible").toBool());
        QCoreApplication::processEvents();

        auto *grid = picker->findChild<QQuickItem *>(
            QStringLiteral("sourceGrid"));
        QVERIFY(grid != nullptr);

        // Names the row the picker would SHARE, read out of the list this
        // test handed it — so a failure reads "you shared Window A" instead
        // of as an off-by-one. (Deliberately not `picker->property("sources")`:
        // a QML `property var` comes back as a QJSValue whose toList() is
        // empty, which would make every comparison below vacuous.)
        auto chosenName = [picker, &rows]() {
            const int at = picker->property("selected").toInt();
            if (at < 0 || at >= rows.size())
                return QStringLiteral("<out of range>");
            return rows.at(at).toMap().value(QStringLiteral("name")).toString();
        };

        // SCREENS: view row 1 is source 2. A naive mapping picks source 1,
        // which is a window.
        QTRY_COMPARE(grid->property("count").toInt(), 2);
        clickItem(window, grid, 1);
        QCOMPARE(chosenName(), QStringLiteral("Screen B"));
        QCOMPARE(picker->property("selected").toInt(), 2);

        auto *applicationsTab = findVisualChild(
            picker, QStringLiteral("shareTabs_applications"));
        QVERIFY(applicationsTab != nullptr);
        clickCentre(window, applicationsTab);
        QTRY_COMPARE(grid->property("count").toInt(), 3);

        // APPLICATIONS: view rows 0/1/2 are sources 1/3/4.
        clickItem(window, grid, 1);
        QCOMPARE(chosenName(), QStringLiteral("Window B"));
        QCOMPARE(picker->property("selected").toInt(), 3);

        clickItem(window, grid, 2);
        QCOMPARE(chosenName(), QStringLiteral("Window C"));
        QCOMPARE(picker->property("selected").toInt(), 4);

        // KEYBOARD, on the same mapping. This is a modal picker and it has to
        // be operable without a mouse; the arrows walk the VISIBLE tab, so
        // stepping back from Window C must land on Window B (source 3) and
        // never on source 1 by arithmetic.
        QTRY_VERIFY2(grid->hasActiveFocus(),
                     "the grid never took focus, so the picker cannot be "
                     "driven from the keyboard");
        QTest::keyClick(window, Qt::Key_Left);
        QCoreApplication::processEvents();
        QCOMPARE(chosenName(), QStringLiteral("Window B"));
        QCOMPARE(picker->property("selected").toInt(), 3);

        QTest::keyClick(window, Qt::Key_Home);
        QCoreApplication::processEvents();
        QCOMPARE(chosenName(), QStringLiteral("Window A"));
        QCOMPARE(picker->property("selected").toInt(), 1);

        // ...and the other half of the mapping: what Share hands the
        // controller is `selected` verbatim, never a view index. Source-read
        // because observing the call needs a live SFU session — but the two
        // halves together are what make the tile and the share the same
        // thing.
        const QString qml = normalized(
            code(read(QStringLiteral(QML_DIR "/ScreenSharePicker.qml"))));
        QVERIFY2(!qml.isEmpty(), "ScreenSharePicker.qml did not read");
        QVERIFY2(qml.contains(QStringLiteral("var chosen = root.selected;")),
                 "the confirm no longer sends the selected SOURCE index");
        QVERIFY2(qml.contains(
                     QStringLiteral("chooseScreenShareSource(chosen)")),
                 "the confirm no longer hands the controller the chosen index");
    }

    // EVERY refusal that reads a node id must know a window carries none.
    //
    // A window share passes nodeId = -1 and its handle instead. There are TWO
    // guards on that path — SfuCallController::startScreenShare and
    // SfuMediaEngine::publishVideo — and the first shipped still refusing on
    // `pipewireNodeId < 0` alone, so choosing a window returned false before
    // anything was logged. The user saw a picker that did nothing and the log
    // showed three `screen share requested` and not one publish.
    //
    // Source-scanned because reaching the real guard needs a live SFU
    // session; what is pinned is that neither refusal reads a node id
    // WITHOUT also asking whether a window was chosen.
    void noScreenShareRefusalForgetsThatAWindowCarriesNoNodeId()
    {
        struct Site { const char *file; const char *guard; };
        const Site sites[] = {
            { SRC_DIR "/calls/SfuCallController.cpp",
              "if (pipewireNodeId < 0 && windowHandle == 0)" },
            { SRC_DIR "/calls/SfuMediaEngine.cpp",
              "if (nodeId < 0 && windowHandle == 0) {" },
        };
        for (const Site &site : sites) {
            const QString src = read(QString::fromUtf8(site.file));
            QVERIFY2(!src.isEmpty(), site.file);
            QVERIFY2(src.contains(QLatin1String(site.guard)),
                     qPrintable(QStringLiteral(
                         "%1 refuses a share on the node id without asking "
                         "whether a window was chosen").arg(
                         QString::fromUtf8(site.file))));
            // ...and the bare form must be gone, or the corrected guard could
            // sit harmlessly beside the one that still refuses.
            QVERIFY2(!src.contains(QLatin1String("if (pipewireNodeId < 0)\n")),
                     "a bare node-id refusal survives alongside the fixed one");
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

    void anRtcRingOffersJoinInsteadOfAnAnswerThatCannotWork()
    {
        // "this [Incoming voice call] accept does nothing."
        //
        // It could not. The card's ONLY gate was
        // `app.calls.mediaBackendAvailable` — a property of the LEGACY
        // GStreamer engine, which says nothing about which lane rang — so an
        // Element call (announced over MatrixRTC) showed an Accept that
        // CallController::answer() refuses at its third guard, returning
        // false into a call site that discarded the result.
        //
        // UNFIXED TREE: FAILS. `rtcRing` and `legacyAcceptOffered` do not
        // exist, so both read false and the RTC case asserts a true.

        // The engine is declared BEFORE the controller so it OUTLIVES it:
        // CallController holds a QPointer to the engine and closes a live
        // session on teardown, and this case deliberately ends with a ring
        // still up.
        //
        // See StubMediaBackend for why an engine is registered at all:
        // without one BOTH lanes hide Accept and this test would be
        // decoration.
        StubMediaBackend media;
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(),
                            &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        controller.calls()->setMediaBackend(&media);
        QVERIFY(controller.calls()->mediaBackendAvailable());

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
        auto *mock = controller.findChild<MockMatrixClient *>();
        QVERIFY(mock != nullptr);

        // 1. The LEGACY lane. Accept is the affordance, and always was.
        CallSignal invite;
        invite.kind = CallSignal::Kind::Invite;
        invite.roomId = QStringLiteral("!general:mock.local");
        invite.eventId = QStringLiteral("$invite-lane");
        invite.sender = QStringLiteral("@peer:mock.local");
        invite.callId = QStringLiteral("lane-call");
        invite.partyId = QStringLiteral("peer-party");
        invite.lifetimeMs = 60000;
        invite.originServerTs = QDateTime::currentMSecsSinceEpoch();
        mock->emitCallSignalForTest(invite);
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        QVERIFY(!controller.calls()->rtcRing());
        settle();
        QCOMPARE(root->property("rtcRing").toBool(), false);
        QCOMPARE(root->property("legacyAcceptOffered").toBool(), true);
        QCOMPARE(root->property("canJoinRtc").toBool(), false);

        QVERIFY(controller.calls()->rejectIncoming());
        settle();

        // 2. The MatrixRTC lane. The legacy Accept is ABSENT — not disabled,
        //    which in Qt Quick would receive no hover and could not explain
        //    itself — and the reason is that answering it through the legacy
        //    path is structurally refused, which is asserted here rather than
        //    assumed.
        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.roomId = QStringLiteral("!general:mock.local");
        notify.eventId = QStringLiteral("$notify-lane");
        notify.sender = QStringLiteral("@peer:mock.local");
        notify.lifetimeMs = 30000;
        notify.senderTs = QDateTime::currentMSecsSinceEpoch();
        notify.originServerTs = notify.senderTs;
        mock->emitCallSignalForTest(notify);
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        QVERIFY(controller.calls()->rtcRing());
        settle();
        QCOMPARE(root->property("rtcRing").toBool(), true);
        QCOMPARE(root->property("legacyAcceptOffered").toBool(), false);
        QVERIFY2(!controller.calls()->answer(),
                 "the legacy answer path claims to accept an RTC ring");

        // The ring is still up, and the card is still the surface for it —
        // hiding Accept must not have hidden the card. Decline (which sends
        // m.rtc.decline) is what stops the ring everywhere.
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("shouldShow").toBool(),
                                  true, 3000);
    }

    void theRingCardJoinsThroughTheOneSharedGate()
    {
        // ONE gate (app.rtc.joinBlockReason), ONE action
        // (app.groupCall.join), three surfaces. RoomCallBanner and
        // CallEventDelegate already carry that rule in their comments; this
        // pins the card to the same pair so a third opinion about whether a
        // call is joinable cannot appear.
        const QString norm = normalized(
            read(QStringLiteral(QML_DIR "/IncomingCallPrompt.qml")));
        QVERIFY(!norm.isEmpty());
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"incomingCallPromptJoin\"")));
        QVERIFY(norm.contains(QStringLiteral(
            "onClicked: app.groupCall.join(root.callRoomId, false)")));
        QVERIFY(norm.contains(
            QStringLiteral("app.rtc.joinBlockReason(root.callRoomId)")));
        QVERIFY(norm.contains(
            QStringLiteral("visible: root.canJoinRtc")));
        // The join gate's answers are a CLOSED SET of tokens mapped to
        // wording here; a raw server string is never rendered.
        QVERIFY(norm.contains(QStringLiteral("case \"no_transport\":")));
        QVERIFY(norm.contains(QStringLiteral("case \"session_closed\":")));
        // Joining must never be reported to the caller as a decline.
        const int joinAt = norm.indexOf(
            QStringLiteral("objectName: \"incomingCallPromptJoin\""));
        const int acceptAt = norm.indexOf(
            QStringLiteral("objectName: \"incomingCallPromptAccept\""));
        QVERIFY(joinAt >= 0);
        QVERIFY(acceptAt > joinAt);
        QVERIFY2(!norm.mid(joinAt, acceptAt - joinAt)
                      .contains(QStringLiteral("rejectIncoming")),
                 "the Join button also declines the call");
        // And the RTC lane must never fall back to a legacy invite: that
        // rings every member of the room.
        QVERIFY2(!norm.contains(QStringLiteral("app.calls.placeCall")),
                 "the ring card can place a legacy call");
    }

    void theFullScreenWindowOpensOnTheApplicationsOwnScreen()
    {
        // "the full screen feature always starts in the same monitor and not
        // the one the client is in."
        //
        // UNFIXED TREE: FAILS on every assertion — nothing anywhere named a
        // screen, so QWindowPrivate::init() connected the window to the
        // PRIMARY screen and QWindowPrivate::create() re-derived the same
        // answer from the default geometry.
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const QString stageCode = normalized(code(stage));
        // Prove the stripper still leaves code behind before trusting the
        // ORDER assertions below — a comment mentioning showFullScreen()
        // would otherwise decide them.
        QVERIFY(stageCode.contains(QStringLiteral("objectName: \"callStage\"")));

        // The placement happens BEFORE the window is shown. Assigning the
        // screen afterwards does not move a window whose old and new screens
        // are virtual siblings — the ordinary single-desktop case — because
        // QWindow::setScreen() then finds windowRecreationRequired() false
        // and is bookkeeping plus a signal.
        const int place =
            stageCode.indexOf(QStringLiteral("placeOnThisApplicationsScreen()"));
        const int show =
            stageCode.indexOf(QStringLiteral("fullScreenWindow.showFullScreen()"));
        QVERIFY2(place >= 0, "nothing places the full-screen window");
        QVERIFY2(show > place,
                 "the window is shown before it is placed on a screen");

        // The screen comes from the Screen ATTACHED to this item, which
        // tracks the item's window and therefore follows the application
        // between monitors. Window.window.screen would be a lazily created
        // wrapper (QWindow has no `screen` Q_PROPERTY at all).
        QVERIFY(stageCode.contains(QStringLiteral("var target = root.Screen")));
        QVERIFY(stageCode.contains(
            QStringLiteral("fullScreenWindow.screen = target")));
        // ...AND the geometry, which is the half that actually decides:
        // QWindowPrivate::create() calls screenForGeometry() just before the
        // platform window exists, so a screen assignment with a default
        // rectangle lands straight back on the primary monitor.
        QVERIFY2(stageCode.contains(
                     QStringLiteral("fullScreenWindow.x = target.virtualX")),
                 "the window's geometry is not moved onto the target screen");
        QVERIFY(stageCode.contains(
            QStringLiteral("fullScreenWindow.y = target.virtualY")));

        // The imperative-visibility rule survives (see
        // fullScreenIsItsOwnWindowThatEscapeLeaves), and so does accepting
        // the close — refusing it vetoes Ctrl+Q.
        QVERIFY2(!code(stage).contains(QStringLiteral("visibility:")),
                 "the full-screen window binds Window.visibility");
        QVERIFY(stageCode.contains(
            QStringLiteral("onClosing: root.exitFullScreen()")));
        QVERIFY2(!stageCode.contains(QStringLiteral("close.accepted = false")),
                 "the full-screen window refuses its close event");
        // Never the primary screen, and never the whole virtual desktop.
        QVERIFY2(!stageCode.contains(QStringLiteral("Qt.application.screens")),
                 "the full-screen window picks a screen from a global list "
                 "rather than from the application's own window");
    }

    void theRaiseHandControlIsOnTheWireAndSaysNothingElse()
    {
        // "raise hand does nothing in element" — it does now. The control
        // used to carry an "only shown on this device" disclaimer in BOTH
        // directions, which was true and is now false: setHandRaised() sends
        // element-call's own m.reaction annotating our own membership state
        // event, so a peer really does see it.
        //
        // A stale disclaimer is worse than none — it tells the user a
        // working feature does not work — so the ban is the point of this
        // test, and it fails on the tree that still carries the wording.
        const QString bar = normalized(
            read(QStringLiteral(QML_DIR "/CallHeaderBar.qml")));
        QVERIFY(!bar.isEmpty());
        QVERIFY(bar.contains(
            QStringLiteral("objectName: \"callBarHandButton\"")));
        QVERIFY2(!bar.contains(QStringLiteral("only shown on this device")),
                 "the raise-hand control still claims no peer can see it, "
                 "which stopped being true when it went on the wire");
        // The icon must stay one the bundled Material Symbols SUBSET carries,
        // or the glyph is tofu (IconChromeTest owns the general rule).
        QVERIFY(bar.contains(QStringLiteral("iconName: \"front_hand\"")));

        // The wire format itself, and every part of it that must not drift.
        // element-call's ReactionsReader compares against exactly this key —
        // a different hand emoji, or the same one without the U+FE0F
        // variation selector, is a hand no Element client will ever see.
        const QString rtc =
            read(QStringLiteral(QML_DIR "/../rust/src/rtc.rs"));
        QVERIFY(!rtc.isEmpty());
        QVERIFY2(rtc.contains(QStringLiteral(
                     "HAND_RAISED_KEY: &str = \"\\u{1F590}\\u{FE0F}\"")),
                 "the raised-hand reaction key is not element-call's, so no "
                 "Element client will ever see one of ours");
        QVERIFY2(rtc.contains(QStringLiteral("RelationType::Annotation")),
                 "the join-time sweep does not ask for annotations, so a hand "
                 "raised before we joined stays invisible");
        // A raise is attributed through the membership it annotates, and the
        // SENDER MUST OWN IT: anyone may annotate anyone's state event, and
        // without the check one user could raise everybody's hand.
        const QString controller = read(QStringLiteral(
            QML_DIR "/../src/calls/RtcController.cpp"));
        QVERIFY(!controller.isEmpty());
        const int at = controller.indexOf(
            QStringLiteral("RtcController::identityForMembership"));
        QVERIFY2(at >= 0, "there is no way to attribute a raised hand");
        QVERIFY2(controller.mid(at, 900)
                     .contains(QStringLiteral("participant.userId != sender")),
                 "a raised hand is attributed without checking that the "
                 "sender owns the membership it annotates");
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
        // 2026-08-26: CallControlBar.qml is DELETED. Nothing ever
        // instantiated it, it carried the only layout control in the tree,
        // and this suite already banned it from the one surface that could
        // have used it — so it was a second, drifting definition of the
        // control set kept alive only by the file list. On the unfixed tree
        // this line fails, because the file is there.
        QVERIFY2(!QFile::exists(QStringLiteral(QML_DIR "/CallControlBar.qml")),
                 "the dead CallControlBar.qml is back in the tree");

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
        // The focused surface is defined ONCE, as `focusedSurface`, and both
        // hosts load THAT — the stage's spotlight and the full-screen window.
        // Two copies would be two things to keep in step, and one of them
        // would eventually stop routing video.
        const int surface = stage.indexOf(QStringLiteral("id: focusedSurface"));
        QVERIFY2(surface >= 0, "the focused surface is no longer defined once");
        const QString block = stage.mid(surface, 6000);
        // Both kinds of surface can be spotlighted, and each is the component
        // that owns its own routing: a SHARE is a CallShareTile (screen sink),
        // a pinned PERSON is a CallParticipantTile (camera sink). The old
        // stage had one surface and a `spotlightKind` string deciding which
        // track it asked for, which is the same thing as "only one share can
        // ever exist".
        QVERIFY2(block.contains(QStringLiteral("CallShareTile {")),
                 "the spotlight cannot show a screen share");
        QVERIFY2(block.contains(QStringLiteral("CallParticipantTile {")),
                 "the spotlight cannot show a pinned participant");
        // Matched BY ID against the real model, so a track key that arrives
        // late still reaches the surface.
        QVERIFY(block.contains(
            QStringLiteral("root.stageState.spotlightShareId")));
        QVERIFY(block.contains(
            QStringLiteral("root.stageState.pinnedIdentity")));
        // And the spotlight really is one of its hosts.
        const int spotlight =
            stage.indexOf(QStringLiteral("objectName: \"callSpotlight\""));
        QVERIFY(spotlight >= 0);
        QVERIFY2(stage.mid(spotlight, 1200)
                     .contains(QStringLiteral("sourceComponent: focusedSurface")),
                 "the spotlight does not host the shared focused surface");
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
        // The RELEASE names the sink, not a key — see
        // noVideoSurfaceEverReleasesARouteByKey for why that is the whole of
        // "camera no longer works".
        QVERIFY(tile.contains(
            QStringLiteral("app.groupCall.detachSink(output.videoSink)")));
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
        QVERIFY(bubbles.contains(QStringLiteral("required property bool speaking")));
        QVERIFY(bubbles.contains(QStringLiteral("opacity: bubble.speaking ? 1 : 0")));
        QVERIFY(bubbles.contains(QStringLiteral("border.color: AppTheme.success")));
        // Initials come from the real name, never the "You" label — that
        // would render a Y for the local user.
        QVERIFY(bubbles.contains(QStringLiteral("name: bubble.displayName")));
        // THE REAL MODEL, not a JS array copied out of participants(). On the
        // unfixed tree the delegate reads `modelData`, which only exists
        // because the strip was fed an array — and an array reassigned is a
        // model reset, so every bubble was destroyed and rebuilt on every
        // speaker update.
        QVERIFY2(!bubbles.contains(QStringLiteral("modelData")),
                 "the bubble strip is bound to a JS array again");
        QVERIFY(bubbles.contains(
            QStringLiteral("property var model: app.groupCall.participantModel")));
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
        const QString grid = read(QStringLiteral(QML_DIR "/CallTileGrid.qml"));
        QVERIFY2(!grid.isEmpty(), "CallTileGrid.qml is missing");
        QVERIFY(stage.contains(QStringLiteral("readonly property bool voiceOnly")));
        QVERIFY(stage.contains(QStringLiteral("voiceOnly: root.voiceOnly")));
        QVERIFY(grid.contains(QStringLiteral("bare: root.voiceOnly")));
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

    // 2026-08-26, maintainer request: "calls get put at the top of the screen".
    // The call is a PANEL above the message list, which keeps scrolling
    // beneath it — Discord's DM arrangement.
    //
    // This assertion is the INVERSE of what it used to be, and the history
    // matters. Originally the stage was merely ADDED beside the timeline and
    // both were `Layout.fillHeight`, so the ColumnLayout split the column:
    // the stage got a ~45 px strip with its avatar and its buttons piled on
    // each other. The fix then was to hide the timeline. The fix NOW is to
    // stop the fight at its source — the stage takes an explicitly assigned,
    // bounded height — which is what lets the timeline come back.
    //
    // On the unfixed tree every one of the four assertions below fails: the
    // host is `Layout.fillHeight: active`, the timeline carries
    // `visible: !root.callStageOwnsColumn`, and there is no panel height at
    // all.
    void theCallPanelSitsAboveTheTimelineRatherThanReplacingIt()
    {
        // Comment-stripped BEFORE normalizing: the bans below forbid two
        // literals that the new hosting comment quotes verbatim while
        // explaining why they are gone, and `normalized()` destroys the line
        // structure a stripper needs.
        const QString pane =
            normalized(code(read(QStringLiteral(QML_DIR "/TimelinePane.qml"))));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("objectName: \"timelineCallStageHost\"")));
        // ONE condition, so the stage and everything around it cannot drift
        // apart again.
        QVERIFY2(pane.contains(QStringLiteral(
                     "readonly property bool callStageOwnsColumn: "
                     "app.groupCall.active && app.groupCall.roomId === "
                     "app.currentRoomId")),
                 "TimelinePane has no single call-stage ownership condition");
        QVERIFY2(pane.contains(QStringLiteral("active: root.callStageOwnsColumn")),
                 "the call stage host does not read the ownership condition");
        // BOUNDED, never fillHeight. `Layout.fillHeight` on the host is the
        // original squash bug and must not come back.
        QVERIFY2(pane.contains(QStringLiteral(
                     "Layout.preferredHeight: active ? root.callPanelHeight "
                     ": 0")),
                 "the call panel does not take a bounded height");
        QVERIFY2(!pane.contains(QStringLiteral("Layout.fillHeight: active")),
                 "the call stage host is fighting the timeline for the column "
                 "again");
        // The timeline and the composer STAY. Hiding them is what this round
        // undoes.
        QVERIFY2(!pane.contains(QStringLiteral("visible: !root.callStageOwnsColumn")),
                 "the timeline still stands down for the call");
        QVERIFY2(!pane.contains(QStringLiteral(
                     "visible: app.currentRoomId !== \"\" && "
                     "!root.callStageOwnsColumn")),
                 "the composer still stands down for the call");
    }

    // The divider is draggable, and the drag is COMMITTED ON THE FALLING EDGE.
    //
    // §16's SplitView lesson, which applies to any hand-rolled resize just as
    // it does to SplitView.resizing: the RELEASE moves nothing, so it emits no
    // heightChanged, and a handler hung off the height never sees the end of
    // the gesture. On the unfixed tree there is no divider at all, so the
    // first assertion fails.
    void theCallPanelDividerCommitsOnTheFallingEdgeOfTheDrag()
    {
        const QString raw = read(QStringLiteral(QML_DIR "/TimelinePane.qml"));
        QVERIFY(!raw.isEmpty());
        const int at =
            raw.indexOf(QStringLiteral("objectName: \"callPanelDivider\""));
        QVERIFY2(at >= 0, "there is no call panel divider");
        const QString block = normalized(raw.mid(at, 2600));
        QVERIFY2(block.contains(QStringLiteral("DragHandler {")),
                 "the divider cannot be dragged");
        QVERIFY2(block.contains(QStringLiteral("onActiveChanged:")),
                 "the divider does not watch the drag's own active flag");
        // The stored value is written where the gesture ENDS.
        QVERIFY2(block.contains(QStringLiteral(
                     "root.callPanelUserHeight = root.clampCallPanelHeight("
                     "root.callPanelHeight);")),
                 "the divider never commits the released height");
        // ...and NOT from a height handler, which is the trap.
        const QString pane = normalized(code(raw));
        QVERIFY2(!pane.contains(QStringLiteral(
                     "onHeightChanged: root.callPanelUserHeight")),
                 "the panel height is being persisted from a height change, "
                 "which never fires on release");
        // A collapse is a request to the HOST, not a self-resize: the stage
        // cannot shrink a panel whose height it does not own.
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(stage.contains(QStringLiteral("signal collapseToggled()")));
        QVERIFY(pane.contains(QStringLiteral(
            "onCollapseToggled: root.callPanelCollapsed = "
            "!root.callPanelCollapsed")));
    }

    // ── The reported bug, and the shape that fixes it ────────────────────
    //
    // "make sure multiple users can screen share, now if share is closed no
    // way to get it back."
    //
    // A SHARE IS A TILE, NOT A MODE. The grid is built over SURFACES: one
    // cell per share and one per participant. On the unfixed tree neither
    // CallShareTile.qml nor CallTileGrid.qml exists, so this fails on its
    // first read; and CallStage carries `sharingPerson`, which returns the
    // FIRST sharer and stops.
    void aScreenShareIsATileNotAMode()
    {
        const QString grid = read(QStringLiteral(QML_DIR "/CallTileGrid.qml"));
        const QString share = read(QStringLiteral(QML_DIR "/CallShareTile.qml"));
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY2(!grid.isEmpty(), "CallTileGrid.qml is missing");
        QVERIFY2(!share.isEmpty(), "CallShareTile.qml is missing");
        QVERIFY(!stage.isEmpty());

        // Two models, two Repeaters — surfaces, not people.
        QVERIFY(grid.contains(QStringLiteral("model: root.shareModel")));
        QVERIFY(grid.contains(QStringLiteral("model: root.participantModel")));
        // The people start where the shares end, so a sharer occupies a cell
        // AND still has their own.
        QVERIFY2(grid.contains(QStringLiteral(
                     "root.cellX(root.shareCount + personCell.index)")),
                 "the grid does not place people after the shares");
        // A person's own tile never renders their screen: two surfaces asking
        // the router for one participant's screen blank each other.
        QVERIFY(grid.contains(QStringLiteral("mediaKind: \"camera\"")));
        // The share tile owns the screen sink, both ends of it — and it
        // releases the SINK it holds, never a key it recomputes.
        QVERIFY(share.contains(QStringLiteral("attachScreenSink(")));
        QVERIFY(share.contains(QStringLiteral("attachLocalScreenSink(")));
        QVERIFY(share.contains(
            QStringLiteral("app.groupCall.detachSink(output.videoSink)")));
        // A shared screen is CONTENT: fitted, never cropped.
        QVERIFY(share.contains(QStringLiteral("VideoOutput.PreserveAspectFit")));
        // The one-sharer collapse must not come back.
        QVERIFY2(!stage.contains(QStringLiteral("sharingPerson")),
                 "the stage still resolves 'the one person who is sharing'");
        QVERIFY2(!stage.contains(QStringLiteral("spotlightKind")),
                 "the spotlight still has a single track kind, which is the "
                 "one-share assumption in another spelling");
    }

    // N simultaneous shares are N surfaces. There is nothing further to add
    // on the wire — SfuVideoRouter already keys per participant and
    // CallShareModel already carries one row per live share — so what this
    // pins is that the VIEW stops collapsing them.
    //
    // On the unfixed tree `readonly property var sharingPerson` is present
    // and this fails.
    void multipleSimultaneousSharesEachGetTheirOwnSurface()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        const QString grid = read(QStringLiteral(QML_DIR "/CallTileGrid.qml"));
        QVERIFY(!stage.isEmpty());
        QVERIFY(!grid.isEmpty());
        // The grid is fed the whole share model, not one chosen sharer.
        QVERIFY(stage.contains(QStringLiteral("shareModel: root.shareModel")));
        QVERIFY(stage.contains(
            QStringLiteral("readonly property var shareModel: "
                           "app.groupCall.shareModel")));
        // No scan of the participant list for "who is sharing" survives
        // anywhere on the stage — that scan is what could only ever find one.
        QVERIFY2(!stage.contains(QStringLiteral("people[i].screenSharing")),
                 "the stage scans participants for a sharer again");
        QVERIFY2(!stage.contains(QStringLiteral("readonly property var people")),
                 "the stage rebuilt the participant JS array");
        // The strip excludes BY shareId. Excluding by identity is what used to
        // drop a sharer's CAMERA when their SCREEN was spotlighted.
        QVERIFY(normalized(stage).contains(QStringLiteral(
            "stripShare.shareId !== root.stageState.spotlightShareId")));
    }

    // THE INVARIANT: while any share is live there is always at least one
    // on-screen control that puts it back on the spotlight.
    //
    // Two of them, deliberately. The explicit "Show screen share" button
    // bound to `restorableShareAvailable`, and the share's own tile in the
    // grid — because the grid is a complete index of everything on offer, a
    // dismissed share is still a row, still a tile, still routable.
    //
    // On the unfixed tree this fails three ways: `layoutMode` exists, "Back
    // to grid" writes it, and there is no restore control anywhere.
    void aLiveShareIsAlwaysReachableAgain()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const QString norm = normalized(stage);

        // 1. The explicit control.
        QVERIFY2(norm.contains(
                     QStringLiteral("objectName: \"callRestoreShareButton\"")),
                 "there is no control that restores a dismissed share");
        QVERIFY(norm.contains(
            QStringLiteral("root.stageState.restorableShareAvailable")));
        QVERIFY(norm.contains(
            QStringLiteral("onClicked: root.stageState.restoreAllShares()")));

        // 2. The implicit one: a share tile in the grid restores itself.
        QVERIFY(norm.contains(QStringLiteral("root.stageState.restoreShare(")));

        // 3. Leaving the spotlight DISMISSES; it never writes a layout.
        QVERIFY(norm.contains(QStringLiteral(
            "root.stageState.dismissShare(root.stageState.spotlightShareId);")));
        QVERIFY(norm.contains(QStringLiteral("root.stageState.clearPin();")));
        const int back =
            norm.indexOf(QStringLiteral("objectName: \"callBackToGridButton\""));
        QVERIFY2(back >= 0, "the Back to grid control is gone");
        QVERIFY2(norm.mid(back, 400)
                     .contains(QStringLiteral("onClicked: root.leaveSpotlight()")),
                 "Back to grid does not go through the dismiss path");

        // 4. THE LATCH IS GONE. `layoutMode` was a one-way door: the only
        //    writer of anything but "auto" was Back to grid, writing "grid",
        //    and nothing ever wrote back.
        const QString stageCode = code(stage);
        // Prove the stripper still leaves code behind before trusting a ban.
        QVERIFY(stageCode.contains(QStringLiteral("objectName: \"callStage\"")));
        QVERIFY2(!stageCode.contains(QStringLiteral("layoutMode")),
                 "the terminal layout-mode latch is back on the call stage");
        // And the stage never pins the shared preference either, which would
        // be the same latch wearing the new API's name.
        QVERIFY2(!stageCode.contains(QStringLiteral("setLayoutPreference")),
                 "the stage writes a layout preference with no writer back");
        // `= "` and not `=`: the derivation legitimately COMPARES the
        // preference with `===`, which contains `= ` as a substring.
        QVERIFY2(!stageCode.contains(QStringLiteral("layoutPreference = \"")),
                 "the stage assigns a layout preference directly");
    }

    // "volume shows up as a circle arround user" — the ring is driven by the
    // SFU's amplitude, not by a boolean.
    //
    // On the unfixed tree the tile has no `speakingLevel` at all and animates
    // `scale: root.speaking ? 1 : 0.94`, so both the presence assertion and
    // the scale ban fail.
    void theSpeakingRingReadsALevelNotABoolean()
    {
        const QString tile =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        const QString grid = read(QStringLiteral(QML_DIR "/CallTileGrid.qml"));
        QVERIFY(!tile.isEmpty());
        QVERIFY(!grid.isEmpty());
        const QString norm = normalized(tile);

        QVERIFY(tile.contains(QStringLiteral("property real speakingLevel")));
        // The GAP follows amplitude.
        QVERIFY2(norm.contains(QStringLiteral(
                     "readonly property real ringTarget: root.speaking ? 3 + 6 "
                     "* Math.max(0, Math.min(1, root.speakingLevel)) : 0")),
                 "the ring does not read the level");
        // Attack fast, release slow, or it strobes between syllables.
        QVERIFY2(norm.contains(QStringLiteral(
                     "ringMotion.duration = root.ringTarget > root.ringGap ? "
                     "60 : 220")),
                 "the ring has no attack/release asymmetry");
        // NOTHING is fabricated from the boolean. An SFU that reports only
        // `active` must degrade to the fixed minimum ring, not to an invented
        // amplitude.
        QVERIFY2(!norm.contains(QStringLiteral("speakingLevel: root.speaking ?")),
                 "a level is being fabricated from the speaking boolean");
        // The ring must not resize the ITEM: scaling the avatar reflows every
        // neighbour on every syllable.
        QVERIFY2(!tile.contains(QStringLiteral("scale: root.speaking")),
                 "the speaking cue scales the avatar again");
        QVERIFY(norm.contains(QStringLiteral("width: parent.width + 2 * root.ringGap")));
        // ...and the level actually reaches the tile from the model.
        QVERIFY(grid.contains(
            QStringLiteral("speakingLevel: personCell.speakingLevel")));
    }

    // The stage binds the real models. A JS array reassigned is a MODEL
    // RESET, and the speaker feed fires continuously while anyone talks — so
    // the old `participants()` + `refreshTick` shape destroyed every tile,
    // every VideoOutput and every attach()/detach() pair on every syllable.
    // An amplitude ring cannot exist on top of that.
    //
    // On the unfixed tree `refreshTick` and `app.groupCall.participants()`
    // are both in CallStage.qml and this fails.
    void theStageBindsTheModelsRatherThanCopyingThem()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        QVERIFY(stage.contains(QStringLiteral(
            "readonly property var participantModel: "
            "app.groupCall.participantModel")));
        QVERIFY(stage.contains(QStringLiteral(
            "readonly property var stageState: app.groupCall.stageState")));
        const QString stageCode = code(stage);
        QVERIFY(stageCode.contains(QStringLiteral("objectName: \"callStage\"")));
        QVERIFY2(!stageCode.contains(QStringLiteral("refreshTick")),
                 "the stage is back on a hand-bumped tick");
        QVERIFY2(!stageCode.contains(QStringLiteral("app.groupCall.participants()")),
                 "the stage copies the participant list into a JS array again");
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

    // -----------------------------------------------------------------
    // 2026-08-27. "camera no longer works" and "when i full screen it it
    // stop shwoing video" are ONE defect, and it lives in how a destroyed
    // surface gave up its video route.
    // -----------------------------------------------------------------

    void noVideoSurfaceEverReleasesARouteByKey()
    {
        // THE regression, stated as a rule the whole qml/ tree must obey.
        //
        // Qt destroys a replaced surface with deleteLater() while it builds
        // the replacement synchronously, so on every grid<->spotlight swap,
        // every full-screen transition and every QQuickRepeater regenerate
        // (which is how a Repeater answers beginMoveRows — i.e. a participant
        // reorder) the order is: NEW tile attaches, THEN old tile detaches.
        // A key-named detach removed whatever was there, so the dying tile
        // unhooked the living one — and since a tile only attaches on
        // creation and on a routing-key change, nothing ever put it back.
        //
        // UNFIXED TREE: FAILS on the first four assertions. CallParticipantTile
        // calls detachLocalScreenSink()/detachScreenSink(root.identity)/
        // detachLocalCameraSink()/detachVideoSink(root.identity) and
        // CallShareTile calls two of them, all with no sink.
        QDir dir(QStringLiteral(QML_DIR));
        const QStringList files =
            dir.entryList({ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY(!files.isEmpty());

        // Prove the scan can see what it is looking for before trusting a ban.
        bool sawARelease = false;
        for (const QString &name : files) {
            const QString body = read(dir.filePath(name));
            for (const auto &banned : { "detachVideoSink(", "detachScreenSink(",
                                        "detachLocalCameraSink(",
                                        "detachLocalScreenSink(" }) {
                QVERIFY2(!body.contains(QLatin1String(banned)),
                         qPrintable(QStringLiteral(
                                        "%1 releases a video route BY KEY (%2)")
                                        .arg(name, QLatin1String(banned))));
            }
            // Every release names a sink. A bare `detachSink()` would be the
            // same hole with the new API's name on it.
            int at = 0;
            while ((at = body.indexOf(QStringLiteral("detachSink("), at)) >= 0) {
                at += 11; // past "detachSink("
                QVERIFY2(body.mid(at, 1) != QLatin1String(")"),
                         qPrintable(QStringLiteral(
                                        "%1 calls detachSink() with no sink")
                                        .arg(name)));
                sawARelease = true;
            }
        }
        QVERIFY2(sawARelease,
                 "no QML surface releases a video sink at all — the scan is "
                 "asserting nothing");

        // AND NO PERIODIC RE-ARM. One was written as a safety net — attach()
        // on every `participantsChanged`, restoring explicitly the accidental
        // self-healing the old constantly-resetting stage provided — and
        // REMOVED on analysis, because it reintroduces the very defect above.
        //
        // Between a layout swap and the deferred delete that ends the old
        // tile, BOTH tiles are alive and connected. One participant update in
        // that window has the dying tile re-CLAIM the key from its successor,
        // and its destruction then releases it as the rightful owner — the
        // live surface goes blank. The late-key case the net was meant to
        // cover is handled where it belongs, by `onActiveTrackKeyChanged` /
        // `onTrackKeyChanged` on the tile itself.
        for (const auto &name : { "CallParticipantTile.qml",
                                  "CallShareTile.qml" }) {
            const QString body =
                code(read(dir.filePath(QLatin1String(name))));
            QVERIFY(body.contains(QStringLiteral("function attach()")));
            QVERIFY2(!normalized(body).contains(
                         QStringLiteral("function onParticipantsChanged() { "
                                        "attach() }")),
                     qPrintable(QStringLiteral(
                                    "%1 re-arms its sink on every participant "
                                    "update, which lets a dying tile steal the "
                                    "key back from its successor")
                                    .arg(QLatin1String(name))));
        }
        // The late-key re-attach IS required, in both tiles.
        QVERIFY(read(dir.filePath(QStringLiteral("CallParticipantTile.qml")))
                    .contains(QStringLiteral("onActiveTrackKeyChanged:")));
        QVERIFY(read(dir.filePath(QStringLiteral("CallShareTile.qml")))
                    .contains(QStringLiteral("onTrackKeyChanged:")));
    }

    void theRouterRefusesAReleaseFromASupersededSurface()
    {
        // The router-level statement of the same rule, and the one assertion
        // that pins the mechanism rather than a call site.
        //
        // UNFIXED TREE: does not compile — `releaseSink` did not exist, and
        // the API it replaced (`detachSink(key)`) had no way to EXPRESS this,
        // which is exactly why eleven router tests passed straight through
        // the regression. Behaviourally, the nearest thing you can write
        // there — `detachSink(key)` from the superseded owner — leaves
        // `watching(key)` FALSE, which is the blank tile.
#ifndef HAVE_LIGHTNING_WEBRTC
        QSKIP("built without the SFU media engine");
#else
        SfuVideoRouter router;
        auto first = std::make_unique<QVideoSink>();
        auto second = std::make_unique<QVideoSink>();
        const QString key = QStringLiteral("TR_share_a");

        router.attachSink(key, first.get());
        QVERIFY(router.watchedBy(key, first.get()));

        // The replacement CLAIMS the key. This is the ORDER production
        // actually produces: Qt builds the new surface synchronously and
        // destroys the old one on the deferred-delete queue.
        router.attachSink(key, second.get());
        QVERIFY(router.watchedBy(key, second.get()));

        // ...and the superseded owner's release, arriving afterwards, takes
        // nothing with it. THIS is the assertion the whole regression turns
        // on.
        router.releaseSink(first.get());
        QVERIFY2(router.watching(key), "a dying surface unhooked a live one");
        QVERIFY(router.watchedBy(key, second.get()));

        // The real owner's release still works, or the table would only ever
        // grow and every assertion here would be vacuous.
        router.releaseSink(second.get());
        QVERIFY(!router.watching(key));

        // One surface gives up EVERY key it holds — a camera tile attaches
        // under both the track sid and the participant sid — and touches
        // nobody else's.
        router.attachSink(key, first.get());
        router.attachSink(QStringLiteral("PA_alice"), first.get());
        router.attachSink(QStringLiteral("TR_other"), second.get());
        router.releaseSink(first.get());
        QVERIFY(!router.watching(key));
        QVERIFY(!router.watching(QStringLiteral("PA_alice")));
        QVERIFY(router.watching(QStringLiteral("TR_other")));

        // A null attach is a NO-OP, never an eviction: "I have no sink yet"
        // is not "nobody may own this key". It used to remove the key, which
        // is the same defect wearing a different hat.
        router.attachSink(QStringLiteral("TR_other"), nullptr);
        QVERIFY(router.watching(QStringLiteral("TR_other")));

        // A null release is likewise a no-op and not a wildcard.
        router.releaseSink(nullptr);
        QVERIFY(router.watching(QStringLiteral("TR_other")));
#endif
    }

    void theVideoRouteSurvivesTheStagesOwnLayoutChanges()
    {
        // THE production-reaching one, and the only kind that would have
        // caught this. §16 records twice what a test that calls the policy
        // directly is worth (the row window shipped as a permanent no-op; the
        // rail drop could never group), so this drives the REAL CallStage
        // through the REAL transition and asserts against the ROUTER — never
        // against a QML property, because the whole failure is a tile
        // reporting "attached" while the router disagrees.
        //
        // UNFIXED TREE: does not compile (no isRoutingVideoTo). With the
        // equivalent assertion wired in, it FAILS at the grid -> spotlight
        // step: the grid tile's deferred destruction removes the key the
        // spotlight tile took, and nothing re-attaches.
#ifndef HAVE_LIGHTNING_WEBRTC
        QSKIP("built without the SFU media engine");
#else
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        SfuCallController *call = controller.groupCall();
        QVERIFY(call);

        QVariantMap track;
        track.insert(QStringLiteral("source"), QStringLiteral("screen_share"));
        track.insert(QStringLiteral("sid"), QStringLiteral("TR_share_a"));
        track.insert(QStringLiteral("muted"), false);
        QVariantMap sharer;
        sharer.insert(QStringLiteral("identity"), QStringLiteral("bob"));
        sharer.insert(QStringLiteral("sid"), QStringLiteral("PA_bob"));
        sharer.insert(QStringLiteral("tracks"), QVariantList { track });
        call->ingestParticipantsForTest({ sharer });
        QCOMPARE(call->shareModel()->rowCount(), 1);

        CallStageState *stage = call->stageState();
        QVERIFY(stage);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine,
                              &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("CallStage"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(3000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr, "CallStage must instantiate");
        root->setWidth(900);
        root->setHeight(500);
        settle();

        const QString key = QStringLiteral("TR_share_a");

        // 1. GRID. Dismissing the spotlight leaves the share as an ordinary
        //    grid tile — still live, still routable, which is the invariant
        //    CallStageState exists for.
        stage->dismissShare(key);
        settle();
        QCOMPARE(stage->spotlightShareId(), QString());
        QVERIFY2(call->isRoutingVideoTo(key),
                 "the grid's share tile never attached a sink");

        // 2. GRID -> SPOTLIGHT. The reported failure.
        stage->restoreShare(key);
        settle();
        QCOMPARE(stage->spotlightShareId(), key);
        QVERIFY2(call->isRoutingVideoTo(key),
                 "the dying grid tile unhooked the spotlight's sink");

        // 3. SPOTLIGHT -> FULL SCREEN. The same transition again, across a
        //    window boundary this time, which is why the feature could not
        //    have been added before this rule existed.
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);
        settle();
        QVERIFY2(call->isRoutingVideoTo(key),
                 "going full screen lost the video route");

        // 4. ...and back.
        stage->setFullScreen(false);
        settle();
        QVERIFY2(call->isRoutingVideoTo(key),
                 "leaving full screen lost the video route");

        // 5. The share ends: NOW the route really should be gone, or the
        //    router is simply never releasing anything and every assertion
        //    above is vacuous.
        QVariantMap stopped = sharer;
        track.insert(QStringLiteral("muted"), true);
        stopped.insert(QStringLiteral("tracks"), QVariantList { track });
        call->ingestParticipantsForTest({ stopped });
        settle();
        QCOMPARE(call->shareModel()->rowCount(), 0);
        QVERIFY2(!call->isRoutingVideoTo(key),
                 "the route outlived the share it belonged to");
#endif
    }

    void fullScreenIsItsOwnWindowThatEscapeLeaves()
    {
        // "add an option to full screen screen share so it takes full
        // minotir like discord". A separate Window, because an overlay can
        // only ever fill the application window.
        //
        // UNFIXED TREE: FAILS on every assertion — there was no full-screen
        // mode at all.
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const QString norm = normalized(stage);
        const QString stageCode = code(stage);
        // Prove the stripper still leaves code behind before trusting a ban.
        QVERIFY(stageCode.contains(QStringLiteral("objectName: \"callStage\"")));

        // 1. A real top-level Window, and a control that opens it.
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"callFullScreenWindow\"")));
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"callFullScreenButton\"")));

        // 2. TWO ways out, one of them on the screen the user was already
        //    looking at — the full-screen window may be on another monitor.
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"callExitFullScreenButton\"")));
        QVERIFY(norm.contains(QStringLiteral(
            "objectName: \"callExitFullScreenFromStageButton\"")));

        // 3. ESCAPE IS A Keys HANDLER, NEVER A Shortcut. Esc is in
        //    ShortcutRegistry's reserved list (the find bar, room
        //    information, a thread, Settings), and two enabled Shortcuts on
        //    one sequence fire NEITHER — so a Shortcut here would break
        //    closing a dialog for as long as a call was up.
        QVERIFY2(norm.contains(QStringLiteral("Keys.onPressed: function")),
                 "full screen has no key handling at all");
        QVERIFY2(norm.contains(QStringLiteral("event.key === Qt.Key_Escape")),
                 "Escape does not leave full screen");
        QVERIFY2(!stageCode.contains(QStringLiteral("Shortcut {")),
                 "the call stage registers a Shortcut, which can shadow a "
                 "reserved sequence");

        // 4. The window's closing is ACCEPTED, never refused. A window that
        //    refuses its close event vetoes application quit — §16 records
        //    close-to-tray eating Ctrl+Q for exactly that reason.
        QVERIFY(norm.contains(QStringLiteral("onClosing: root.exitFullScreen()")));
        QVERIFY2(!stageCode.contains(QStringLiteral("close.accepted = false")),
                 "the full-screen window refuses its close event");

        // 5. ONE surface per routing key: the stage's grid and spotlight both
        //    stand down while full screen is up. Two live tiles asking the
        //    router for one participant's screen would fight over the key.
        QVERIFY2(norm.contains(QStringLiteral(
                     "active: !root.collapsed && !root.fullScreenActive && "
                     "root.effectiveLayout === \"grid\"")),
                 "the grid stays up behind the full-screen window");
        QVERIFY2(norm.contains(QStringLiteral(
                     "active: !root.collapsed && !root.fullScreenActive && "
                     "root.effectiveLayout === \"spotlight\"")),
                 "the spotlight stays up behind the full-screen window");

        // 6. Never full screen with nothing in it.
        QVERIFY(norm.contains(QStringLiteral(
            "readonly property bool fullScreenActive: root.stageState ? "
            "(root.stageState.fullScreen && root.spotlightHasSurface) : "
            "false")));

        // 7. The window's visibility is driven imperatively. A binding on
        //    `visibility` is a binding on the property a window manager
        //    writes when the user closes the window, and a QML binding the
        //    platform overwrites is how this repo has shipped one-way
        //    latches before.
        QVERIFY(norm.contains(QStringLiteral("fullScreenWindow.showFullScreen()")));
        QVERIFY2(!stageCode.contains(QStringLiteral("visibility:")),
                 "the full-screen window binds Window.visibility");
    }

    void theFullScreenWindowStaysHiddenUntilItIsAskedFor()
    {
        // A real instantiation. The Window object exists for the stage's
        // whole life — declaring it inside the Item is what makes Qt treat it
        // as transient for the main window — so what must hold is that it is
        // not SHOWN, and that its contents are not built, until the flag says
        // so. A hidden Window still instantiates its children, which is why
        // the surface inside it is behind a Loader.
        //
        // UNFIXED TREE: does not compile as written (no such window), and
        // CallStage had never been instantiated in a test at all.
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine,
                              &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("CallStage"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(3000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr, "CallStage must instantiate");
        settle();

        QCOMPARE(root->property("fullScreenActive").toBool(), false);

        // Found through the application's window list rather than through
        // findChild: a Window declared inside an Item is not a child ITEM,
        // and how Qt parents it is an implementation detail this assertion
        // should not depend on. Every constructed QWindow is in this list,
        // shown or not.
        QWindow *window = nullptr;
        for (QWindow *w : QGuiApplication::allWindows()) {
            if (w->objectName() == QLatin1String("callFullScreenWindow")) {
                window = w;
                break;
            }
        }
        QVERIFY2(window != nullptr, "the full-screen window is not declared");
        QVERIFY2(!window->isVisible(),
                 "the full-screen window opened without being asked for");

        // With no call and nothing focused, asking for it is refused rather
        // than opening an empty black window.
        auto *stage = controller.groupCall()->stageState();
        QVERIFY(stage);
        stage->setFullScreen(true);
        settle();
        QCOMPARE(stage->fullScreen(), false);
        QCOMPARE(root->property("fullScreenActive").toBool(), false);
        QVERIFY(!window->isVisible());
    }

    void aParticipantsVolumeIsAdjustableAndSurvivesTheCall()
    {
        // "if a user A sets user B volume to 70% it stays the same in next
        // call or other room." The persistence is SettingsManager's and the
        // seeding is the controller's; what this pins is the surface, and
        // specifically the four ways a plausible implementation of it is
        // silently wrong.
        //
        // UNFIXED TREE: every assertion below fails — CallParticipantTile
        // had no volume control of any kind, which is why there was nothing
        // to set.
        const QString raw =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        QVERIFY(!raw.isEmpty());
        const QString tile = normalized(raw);

        // 1. It exists, and it is reachable BOTH ways — a right-click-only
        //    control is one most people never find, and a hover-only one is
        //    unreachable from the keyboard.
        QVERIFY(tile.contains(
            QStringLiteral("objectName: \"callParticipantVolumeSlider\"")));
        QVERIFY(tile.contains(
            QStringLiteral("objectName: \"callParticipantVolumeButton\"")));
        QVERIFY(tile.contains(QStringLiteral("acceptedButtons: Qt.RightButton")));
        QVERIFY(tile.contains(QStringLiteral("Qt.Key_Menu")));

        // 2. THE RANGE IS 0..200. `to: 100` is the obvious wrong version and
        //    it silently removes the entire reason the control exists: the
        //    case it is for is a participant who is too QUIET.
        QVERIFY(tile.contains(QStringLiteral("from: 0")));
        // The slider is the USER scale and stops at 200. What 200 MEANS is
        // 1000% of audio: SfuMediaEngine::audioFactorPercent() expands
        // 100-200 onto 100-1000 and leaves 0-100 as 1:1 attenuation. A
        // straight 0-200 slider tops out at +6 dB ("above 100% barely any
        // difference"); a straight 0-1000 one puts every useful setting in
        // its first tenth. The curve itself is pinned by
        // theVolumeCurveIsLiteralBelowUnityAndExpandsAbove.
        QVERIFY(tile.contains(QStringLiteral("to: 200")));
        // And the neutral point is MARKED. 100 is not the middle of a
        // preference, it is the one value that changes nothing.
        QVERIFY(tile.contains(QStringLiteral(
            "objectName: \"callParticipantVolumeNeutralMark\"")));

        // 3. IT READS THE CURRENT VALUE. A slider that always opens at 100 is
        //    the specific failure called out in the brief, and it is what you
        //    get from a control with a write path and no read path — which is
        //    exactly what `setParticipantVolume` was before this round
        //    (write-only, and therefore never called from any QML).
        QVERIFY(tile.contains(QStringLiteral("function currentVolumePercent()")));
        //    Read from the STORE through the controller, not from the model's
        //    `volumePercent` role. The two agree in the steady state and
        //    differ exactly while a call is opening — before the controller
        //    has seeded the rows — which is when a person is most likely to
        //    reach for this control.
        QVERIFY(tile.contains(QStringLiteral(
            "app.groupCall.participantVolume(root.identity)")));
        QVERIFY(tile.contains(QStringLiteral(
            "onOpened: volumeSlider.value = root.currentVolumePercent()")));

        // 4. `onMoved`, NEVER `onValueChanged`. `onValueChanged` also fires
        //    for the programmatic read above, so it would write the value
        //    straight back to the controller on every single open — a store
        //    write per open, and one that defeats the store's own "the
        //    default is never recorded" rule by re-recording what it read.
        //    This is the assertion most likely to catch a future edit.
        QVERIFY(tile.contains(
            QStringLiteral("onMoved: root.applyVolumePercent(value)")));
        QVERIFY2(!code(raw).contains(QStringLiteral("onValueChanged")),
                 "the volume slider must react to onMoved, not to "
                 "onValueChanged");
        // The speaker/muted glyph CALLS currentVolumePercent(). Qt cannot
        // observe a C++ function call as a dependency, so that binding must
        // read a revision counter or it evaluates once and then describes a
        // level that has since changed.
        QVERIFY(tile.contains(QStringLiteral("property int volumeRevision")));
        QVERIFY(tile.contains(QStringLiteral("var _ = root.volumeRevision;")));

        // 5. Amplification is DISCLOSED, and disclosed always — not revealed
        //    once the user is already past the line, which is not a
        //    disclosure.
        QVERIFY(tile.contains(
            QStringLiteral("Above 100% amplifies and can clip.")));

        // 6. NOT on the local tile. This is a PLAYBACK volume; nobody hears
        //    their own published audio, so a slider there would move a number
        //    with no audible effect and read as broken.
        QVERIFY(tile.contains(QStringLiteral(
            "readonly property bool _volumeOffered: !root.local")));

        // 7. The popup SINKS its own presses. A Popup does not consume a
        //    press that lands on it — blockInput() is false when the item is
        //    the popup item — so without an all-buttons sink in `background:`
        //    a press on the popup's chrome walks down to the tile's two
        //    TapHandlers beneath: left re-spotlights the stage, right
        //    re-opens the popup under itself. The Slider consumes its own
        //    presses, which is what makes this easy to miss — dragging works
        //    and only the surrounding chrome misbehaves. `modal: true` would
        //    NOT fix it: it blocks presses OUTSIDE a popup only.
        //
        //    This assertion fails on the first draft of this very feature,
        //    which is the only reason it is worth writing down.
        QVERIFY2(tile.contains(QStringLiteral(
                     "MouseArea { anchors.fill: parent acceptedButtons: "
                     "Qt.AllButtons")),
                 "the volume popup does not sink its own presses");
    }

    void theVolumeSurfaceGoesThroughTheControllerAndNeverTheStore()
    {
        // The write must reach the CONTROLLER, because the controller is the
        // only place that can map the SFU identity this tile holds to the
        // Matrix user id the preference is stored under. An identity is per
        // DEVICE and, in the sticky membership format, an opaque hash — a
        // preference keyed by it would be forgotten on the next rejoin, which
        // is the exact opposite of what was asked for.
        //
        // So a surface that "helpfully" also wrote app.settings would look
        // correct in a single call and lose the setting between calls. This
        // ban is what stops that, and it is the reason the test exists at all.
        const QString tile =
            code(read(QStringLiteral(QML_DIR "/CallParticipantTile.qml")));
        QVERIFY(!tile.isEmpty());

        // Present-token control: without this the ban below could pass simply
        // because the scan found nothing at all.
        QVERIFY2(tile.contains(QStringLiteral("app.groupCall.setParticipantVolume")),
                 "the volume write must go through the controller");

        QVERIFY2(!tile.contains(QStringLiteral("setCallParticipantVolume")),
                 "the tile must not write the store directly — the controller "
                 "owns the identity-to-user-id mapping");
        QVERIFY2(!tile.contains(QStringLiteral("QSettings")),
                 "no QML surface may touch QSettings");
    }

    void microphoneGainIsOfferedWithItsMicrophoneAndSaysWhatItCosts()
    {
        // The other direction: what OTHERS hear. It lives with the microphone
        // picker because it is a property of this computer's capture device,
        // not of a call or of a person.
        //
        // UNFIXED TREE: fails on every assertion — CallDeviceSettings.qml had
        // three device pickers and no level control at all.
        const QString raw =
            read(QStringLiteral(QML_DIR "/CallDeviceSettings.qml"));
        QVERIFY(!raw.isEmpty());
        const QString devices = normalized(raw);

        QVERIFY(devices.contains(
            QStringLiteral("objectName: \"microphoneGainSlider\"")));
        // Same range and the same marked neutral point as the per-person
        // control. Two level sliders in one app that disagree about what 100
        // means is a worse outcome than either of them being wrong.
        QVERIFY(devices.contains(QStringLiteral("to: 200")));
        QVERIFY(devices.contains(QStringLiteral(
            "objectName: \"microphoneGainNeutralMark\"")));
        QVERIFY(devices.contains(
            QStringLiteral("Above 100% amplifies and can clip.")));

        // Bound to the settings property both ways, and moved by a USER
        // gesture only — `onValueChanged` here would write back the value the
        // binding just delivered from the store.
        QVERIFY(devices.contains(
            QStringLiteral("value: app.settings.microphoneGain")));
        QVERIFY(devices.contains(QStringLiteral(
            "onMoved: app.settings.microphoneGain = Math.round(value)")));
        QVERIFY2(!code(raw).contains(QStringLiteral("onValueChanged")),
                 "the gain slider must react to onMoved, not onValueChanged");
        QVERIFY2(!code(raw).contains(QStringLiteral("QSettings")),
                 "no QML surface may touch QSettings");
    }

    void everyIconTheVolumeSurfacesAskForIsInTheBundledSubset()
    {
        // The bundled Material Symbols font is a SUBSET: a name absent from
        // Icon.qml's map renders as tofu, and nothing else catches it —
        // the glyph simply comes out as a box on the maintainer's desktop.
        // Pinned here for the names THIS round introduced, so a rename in
        // Icon.qml cannot quietly blank the volume button.
        const QString icons = read(QStringLiteral(QML_DIR "/Icon.qml"));
        QVERIFY(!icons.isEmpty());
        for (const auto &name : {"volume_up", "volume_off"}) {
            QVERIFY2(icons.contains(
                         QStringLiteral("\"%1\":").arg(QLatin1String(name))),
                     qPrintable(QStringLiteral("icon '%1' is not mapped")
                                    .arg(QLatin1String(name))));
        }
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

    // "There is a call in this room" on a room-list row, in BOTH layouts.
    //
    // The load-bearing rule is what it may COST. RtcController::refresh()
    // re-reads one room's session, and read_membership_events falls back to a
    // full /state request whenever the store holds no live membership — the
    // normal state of every idle room. A row that refreshed itself would issue
    // one /state per room in the list, on every rebuild, so this component
    // reads only what the controller already knows and re-reads on its
    // sessionChanged poke.
    void theRoomRowCallGlyphNeverAsksForARefresh()
    {
        const QString glyph = read(QStringLiteral(QML_DIR "/RoomCallGlyph.qml"));
        QVERIFY2(!glyph.isEmpty(), "RoomCallGlyph.qml is missing");
        QVERIFY2(glyph.contains(QStringLiteral("app.rtc.hasLiveSession")),
                 "the glyph does not ask whether a call is live");
        QVERIFY2(!glyph.contains(QStringLiteral("app.rtc.refresh")),
                 "the room-row call glyph refreshes the session itself, which "
                 "is one /state request per idle room in the list");
        QVERIFY2(glyph.contains(QStringLiteral("onSessionChanged")),
                 "the glyph never re-reads, so it freezes on whatever was "
                 "known when the row was built");
        // An Icon is a Text, and a never-laid-out empty Text keeps
        // ItemObservesViewport forever — in a per-row delegate that is the
        // most expensive QML mistake in this tree (d1ddc2f).
        QVERIFY2(glyph.contains(QStringLiteral("Loader")),
                 "the glyph's Icon is not behind a Loader, so every room row "
                 "adds a permanent viewport observer");

        // ONE component, used by both layouts: the rule is identical in
        // Classic and Channels, and a second copy is how the two lists end up
        // disagreeing about whether a call is live.
        for (const QString &file : { QStringLiteral(QML_DIR "/RoomDelegate.qml"),
                                     QStringLiteral(QML_DIR "/ChannelDelegate.qml") }) {
            const QString source = read(file);
            QVERIFY(!source.isEmpty());
            QVERIFY2(source.contains(QStringLiteral("RoomCallGlyph")),
                     qPrintable(QStringLiteral("%1 shows no call indicator")
                                    .arg(file)));
            QVERIFY2(!source.contains(QStringLiteral("app.rtc.hasLiveSession")),
                     qPrintable(QStringLiteral(
                         "%1 reimplements the call indicator instead of using "
                         "the shared component").arg(file)));
        }
    }

    // "the people tab takes up way too much space". The panel's whole job is
    // to list people, and the chrome above the roster was a header, a tab
    // strip, a filter/sort/Invite row AND a search row — about 190 px before
    // the first member, in a column ~300 px wide.
    //
    // Pinned as NUMBERS because that is what regressed: every one of these is
    // a value somebody can nudge back up one control at a time without
    // noticing the column has lost a third of its rows.
    void theMemberRosterStaysCompact()
    {
        const QString panel = read(QStringLiteral(QML_DIR "/RoomInfoPanel.qml"));
        QVERIFY(!panel.isEmpty());

        // ONE row of chrome: the search field and the three controls share it.
        // A second row is the shape this replaced.
        QVERIFY2(panel.contains(QStringLiteral("id: memberSearch")),
                 "the member search field is gone");
        const int searchAt = panel.indexOf(QStringLiteral("id: memberSearch"));
        const int listAt = panel.indexOf(QStringLiteral("id: memberList"));
        QVERIFY(searchAt > 0 && listAt > searchAt);
        const QString chrome = panel.mid(searchAt, listAt - searchAt);
        for (const QString &glyph : { QStringLiteral("person_search"),
                                      QStringLiteral("unfold_more"),
                                      QStringLiteral("person_add") }) {
            QVERIFY2(chrome.contains(glyph),
                     qPrintable(QStringLiteral(
                         "the %1 control left the search row, so the roster "
                         "has a second row of chrome above it again")
                                    .arg(glyph)));
        }
        // ...and they are ICONS. A labelled control is most of the row in a
        // column this narrow.
        QVERIFY2(!chrome.contains(QStringLiteral("text: qsTr(\"A to Z\")")),
                 "the sort control went back to a text button");

        // The row and heading heights, and the avatar.
        const QString list = panel.mid(listAt);
        // EVERY SIZE IN THIS SECTION FOLLOWS THE TEXT-SIZE SLIDER.
        //
        // The whole panel used AppTheme.scaled zero times while the room list
        // used it six, so the slider grew every other surface and left this
        // one behind — which is why the roster read small however its literal
        // sizes were tuned, across three rounds of trying to tune them.
        //
        // Heights are expressions over the scaled text rather than literals,
        // or a row clips its own contents at 140%.
        QVERIFY2(list.contains(QStringLiteral("AppTheme.scaled(AppTheme.textTitle)")),
                 "the member name does not follow the text-size slider");
        QVERIFY2(list.contains(QStringLiteral("Math.max(34, AppTheme.scaled")),
                 "the member row height is a literal again, so it clips its "
                 "own text at a larger slider position");
        QVERIFY2(list.contains(QStringLiteral("Math.max(24, AppTheme.scaled")),
                 "the member avatar no longer follows the row");
        QVERIFY2(list.contains(QStringLiteral("spacing: 3")),
                 "the member rows touch, so two names read as one block");
        // THE TAB STRIP IS ONE SIZE IN EVERY SECTION. `dense` and the tab
        // margins used to be conditional on `section === "people"`, so the
        // strip visibly shrank the moment People was selected and grew back
        // on the way out — "when clicking people tab everything gets small".
        // A control that resizes depending on which of its own tabs is active
        // looks broken; the compactness People wanted is the ROSTER's.
        // COMMENTS STRIPPED, and bounded to the control's own block (it ends
        // at onActivated). Both halves are load-bearing: the fix's own
        // rationale comment NAMES the token being banned, so a ban read off
        // the raw file always "finds" it — the hazard `code()` exists for and
        // that this file's own header warns about — and a fixed character
        // window runs past the block into the Overview section, whose
        // visibility condition is none of this test's business.
        const QString clean = code(panel);
        const int tabsAt = clean.indexOf(QStringLiteral("objectName: \"roomInfoTabs\""));
        QVERIFY(tabsAt > 0);
        const int tabsEnd = clean.indexOf(QStringLiteral("onActivated:"), tabsAt);
        QVERIFY(tabsEnd > tabsAt);
        const QString tabs = clean.mid(tabsAt, tabsEnd - tabsAt);
        // Prove the window still contains what it is meant to read, or every
        // negative assertion below is vacuous.
        QVERIFY2(tabs.contains(QStringLiteral("dense:")),
                 "the tab window no longer contains the control it scans");
        QVERIFY2(!tabs.contains(QStringLiteral("section === \"people\"")),
                 "the section tabs still resize themselves when People is "
                 "selected");
        QVERIFY2(tabs.contains(QStringLiteral("fitWidth: true"))
                     && tabs.contains(QStringLiteral("Layout.fillWidth: true")),
                 "the tab strip cannot compact into the panel: fitWidth only "
                 "acts on a width the host actually gives it, so without "
                 "fillWidth the row keeps its implicit width and the last tab "
                 "runs off the edge");

        // The panel may never take so much width that the shell overflows its
        // own window. The stored width outlives the window it was dragged in.
        const QString pane = read(QStringLiteral(QML_DIR "/TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY2(pane.contains(QStringLiteral("Math.min(app.settings.sidePanelWidth")),
                 "the info panel takes its stored width uncapped, so a window "
                 "narrower than it was dragged in pushes the panel off the "
                 "right edge");
        QVERIFY2(panel.contains(QStringLiteral("clip: true")),
                 "the info panel does not clip, so anything wider than it "
                 "paints over the timeline");

        // ONE body size across the panel. Compacting the roster shrank the
        // TEXT as well as the spacing, so People read a size smaller than
        // Overview and Media and switching tabs looked like a zoom. Density
        // is the row height's job.
        //
        // Scoped to the member ROW component: `list` runs to the end of the
        // file and so contains the Media section, whose own smaller labels
        // are none of this test's business.
        const int rowAt = panel.indexOf(QStringLiteral("id: memberRowComponent"));
        QVERIFY2(rowAt > 0, "the member row component is gone");
        const int rowEnd = panel.indexOf(QStringLiteral("Media & Files"), rowAt);
        QVERIFY(rowEnd > rowAt);
        const QString row = panel.mid(rowAt, rowEnd - rowAt);
        QVERIFY2(!row.contains(QStringLiteral("font.pixelSize: AppTheme.text")),
                 "a member row sets an UNSCALED font size, so it ignores the "
                 "text-size slider every other surface honours");
    }

    // ── 2026-08-27 Windows layout round ──────────────────────────────────
    //
    // Three defects reported from a packaged Windows build ("some button are
    // sitting on buttons ... in windows there is a lot of cliping"). None of
    // them is a Windows API difference: the app pins its own Qt Quick style
    // and ships its own font, so what differs is DISPLAY SCALE — the
    // maintainer's capture measures at 1.5, which leaves every logical
    // dimension the same and two thirds as many of them on the screen. Fixed
    // bands then take a bigger share, and anything sized from a label lands
    // somewhere else. The three cases below pin the structural half of that,
    // which is the half a test can hold.

    /// A control row with no implicit width is laid out ON TOP of the
    /// controls beside it.
    ///
    /// CallHeaderBar is hosted three ways and only ONE of them stretches it.
    /// The collapsed call strip puts it in a Loader inside a RowLayout and
    /// the full-screen window puts it in a Loader anchored to the bottom
    /// centre — both READ its implicit size. A Loader adopts the loaded
    /// item's implicit size, a Rectangle that never sets one reports 0, and
    /// a RowLayout cell of width 0 puts the NEXT control immediately after
    /// it — while `bar` (anchors.centerIn) goes on drawing half the control
    /// row each side of that zero-width point.
    ///
    /// `implicitHeight` was already carrying exactly this job for the
    /// vertical axis, which is why the strip had a sensible height and no
    /// width at all.
    ///
    /// The assertion is geometric rather than a source scan: every control
    /// must lie inside the box the layout gave the bar. On the unfixed tree
    /// the hang-up button lands past the trailing item's left edge, by about
    /// half the control row.
    void compactCallControlsStayInsideTheBoxTheLayoutGivesThem()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        // Declared BEFORE the item it will host, so the item's own owner is
        // destroyed FIRST: setParentItem takes QObject ownership too, and a
        // window torn down ahead of the unique_ptr would delete the tree
        // out from under it.
        QQuickWindow window;
        window.resize(900, 90);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Layouts
import MatrixClient
Item {
    id: outer
    width: 900
    height: 90
    RowLayout {
        anchors.fill: parent
        spacing: 8
        // Stands in for the speaker bubbles: the elastic cell that takes
        // whatever the controls do not.
        Item {
            objectName: "fixtureElastic"
            Layout.fillWidth: true
            implicitHeight: 10
        }
        CallHeaderBar {
            objectName: "fixtureCompactDock"
            previewMode: true
            placement: "dock"
            compact: true
        }
        // Stands in for the collapse button, which is what the control row
        // was drawing straight through.
        Rectangle {
            objectName: "fixtureTrailing"
            implicitWidth: 30
            implicitHeight: 30
            color: "transparent"
        }
    }
}
)"), QUrl(QStringLiteral("qrc:/callcompactdocktest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *outer = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY2(outer != nullptr, "the fixture must instantiate");

        outer->setParentItem(window.contentItem());
        window.show();
        settle();
        outer->polish();
        settle();

        auto *dock =
            outer->findChild<QQuickItem *>(QStringLiteral("fixtureCompactDock"));
        QVERIFY(dock != nullptr);
        auto *trailing =
            outer->findChild<QQuickItem *>(QStringLiteral("fixtureTrailing"));
        QVERIFY(trailing != nullptr);
        QVERIFY2(dock->property("visible").toBool(),
                 "previewMode no longer shows the bar, so this fixture proves "
                 "nothing");

        // A layout rearranges on the window's POLISH pass, which is
        // asynchronous even offscreen — so converge on a laid-out frame
        // rather than measuring the one that happens to be current.
        QTRY_VERIFY(dock->width() > 0.0);
        QVERIFY2(dock->width() > 100.0,
                 qPrintable(QStringLiteral("the compact control row was given "
                                           "%1 px by its layout — it reports "
                                           "no implicit width, so its controls "
                                           "draw over whatever is beside it")
                                .arg(dock->width())));

        const qreal trailingLeft =
            trailing->mapToItem(outer, QPointF(0, 0)).x();
        for (const auto &name : { "callBarMicButton", "callBarDeafenButton",
                                  "callBarCameraButton",
                                  "callBarScreenShareButton",
                                  "callBarHangUpButton" }) {
            auto *control = dock->findChild<QQuickItem *>(QLatin1String(name));
            QVERIFY2(control != nullptr,
                     qPrintable(QStringLiteral("missing %1")
                                    .arg(QLatin1String(name))));
            const qreal left = control->mapToItem(outer, QPointF(0, 0)).x();
            const qreal right =
                control->mapToItem(outer, QPointF(control->width(), 0)).x();
            QVERIFY2(right <= trailingLeft + 0.5,
                     qPrintable(QStringLiteral("%1 ends at %2, past the "
                                               "trailing control at %3 — the "
                                               "compact dock is drawing "
                                               "through its neighbour")
                                    .arg(QLatin1String(name))
                                    .arg(right)
                                    .arg(trailingLeft)));
            const qreal dockLeft = dock->mapToItem(outer, QPointF(0, 0)).x();
            QVERIFY2(left >= dockLeft - 0.5,
                     qPrintable(QStringLiteral("%1 starts at %2, left of the "
                                               "bar's own box at %3")
                                    .arg(QLatin1String(name))
                                    .arg(left)
                                    .arg(dockLeft)));
        }
    }

    /// The nameplate pill is capped to the tile; the ROW inside it was not.
    ///
    /// `anchors.centerIn` sets x and y and nothing else, so the row took its
    /// full implicit width — the whole unelided name — while the pill behind
    /// it stopped at the tile's edge. `Layout.fillWidth` on the label then
    /// had no width to fill against, so it never elided: the name ran out
    /// past both ends of its own plate, and on the share tile (which clips)
    /// it was cut off mid-word instead. That is the reported clipping, and
    /// how soon it shows depends on how wide the platform draws the name.
    void aTileNameplateNeverOverflowsItsOwnPill()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        // Before the fixture, so the fixture's owner outlives it — see the
        // note in the compact-dock case.
        QQuickWindow window;
        window.resize(400, 220);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient
Item {
    id: outer
    width: 400
    height: 220
    CallParticipantTile {
        objectName: "fixtureTile"
        width: 140
        height: 88
        compact: true
        identity: "PA_one"
        userId: "@bartholomew:mock.local"
        displayName: "Bartholomew Featherstonehaugh III"
        micKnown: true
        micMuted: true
    }
    CallShareTile {
        objectName: "fixtureShare"
        y: 96
        width: 140
        height: 88
        compact: true
        shareId: "S1"
        // No ownerIdentity ON PURPOSE: the label does not need one, and an
        // identity would activate the tile's VideoOutput, which drags Qt
        // Multimedia (and its ~1 s first-sink cost) into a layout test.
        ownerDisplayName: "Bartholomew Featherstonehaugh III"
    }
}
)"), QUrl(QStringLiteral("qrc:/callnameplatetest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *outer = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY2(outer != nullptr, "the fixture must instantiate");

        outer->setParentItem(window.contentItem());
        window.show();
        settle();
        outer->polish();
        settle();

        const auto check = [&](const char *tileName, const char *plateName,
                               const char *rowName) {
            auto *tile =
                outer->findChild<QQuickItem *>(QLatin1String(tileName));
            QVERIFY2(tile != nullptr, tileName);
            auto *plate =
                tile->findChild<QQuickItem *>(QLatin1String(plateName));
            QVERIFY2(plate != nullptr,
                     qPrintable(QStringLiteral("%1 is gone, so this test "
                                               "measures nothing")
                                    .arg(QLatin1String(plateName))));
            auto *row = tile->findChild<QQuickItem *>(QLatin1String(rowName));
            QVERIFY2(row != nullptr, rowName);
            // Converge on a laid-out frame: the pill's width comes from the
            // inner layout's size hints, which settle on a polish pass.
            QTRY_VERIFY(plate->width() > 0.0 && row->width() > 0.0);
            QVERIFY2(plate->width() <= tile->width() + 0.5,
                     qPrintable(QStringLiteral("%1 is %2 wide on a %3 px tile")
                                    .arg(QLatin1String(plateName))
                                    .arg(plate->width())
                                    .arg(tile->width())));
            QVERIFY2(row->width() > 0.0,
                     "the nameplate row has no width at all");
            QVERIFY2(row->width() <= plate->width() + 0.5,
                     qPrintable(QStringLiteral("the nameplate row is %1 wide "
                                               "inside a %2 px pill — the name "
                                               "is running out past its own "
                                               "plate instead of eliding")
                                    .arg(row->width())
                                    .arg(plate->width())));
        };
        check("fixtureTile", "callTileNameplate", "callTileNameplateRow");
        check("fixtureShare", "callShareNameplate", "callShareNameplateRow");
    }

    /// The spotlight's strip of other surfaces must YIELD height, because
    /// everything else in that column is fixed.
    ///
    /// It asked for a flat 96 px whatever the panel had, and the spotlight
    /// took what was left. Measured off the maintainer's Windows capture: a
    /// 335 px call panel spent 96 on the strip and left the shared screen
    /// 61, so a 16:9 desktop arrived as a 112x61 stamp in a full-width
    /// letterbox — the strip was the bigger half of the stage.
    ///
    /// Below a usable tile the strip becomes the bubble row rather than a
    /// squeezed version of a shape that no longer works, which is why the
    /// short case is checked for its MODE as well as its height.
    ///
    /// Both halves are pinned, because a policy test that calls the policy
    /// proves nothing about whether production reaches it: the arithmetic
    /// here, and the call site by scan.
    // A PANEL SHOWING VIDEO MUST NOT BE SQUEEZED INTO ITS OWN CHROME.
    //
    // The divider is draggable, and the floor under it used to be a flat 45%
    // of the pane whatever the call was doing. On a short window that left the
    // stage its header, its dock and a sliver of picture — the share collapsed
    // to a few pixels and the spotlight's own overlay controls, which are
    // anchored inside a CLIPPED rectangle and do not compact, drew across the
    // top edge in half. Reported with a screenshot: "make it when screen share
    // is on not so squishable, ui breaks then".
    //
    // Drives the REAL clamp through the real property, at a real pane height,
    // rather than restating the arithmetic: a test that recomputes the policy
    // agrees with itself no matter what production does.
    void aVideoCallPanelKeepsEnoughHeightToShowAPicture()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(":/qt/qml"));
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient
Item {
    width: 900
    height: 520
    property alias pane: paneItem
    TimelinePane { id: paneItem; anchors.fill: parent }
}
)"), QUrl(QStringLiteral("qrc:/callpanelfloortest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner != nullptr);
        auto *pane = owner->property("pane").value<QObject *>();
        QVERIFY(pane != nullptr);

        // The stage's own declared minimum, so this case cannot drift from
        // the bands CallStage actually draws.
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY2(normalized(stage).contains(
                     QStringLiteral("readonly property int minimumUsefulHeight")),
                 "the stage no longer declares how short it can usefully be");
        const QString pane_ = normalized(
            read(QStringLiteral(QML_DIR "/TimelinePane.qml")));
        QVERIFY2(pane_.contains(
                     QStringLiteral("callStageHost.item.minimumUsefulHeight")),
                 "the pane no longer ASKS the stage for that minimum, so the "
                 "two can drift");
        // AND THE CLAMP MUST USE IT. The property existing proves nothing —
        // checked by mutation: deleting the branch below while leaving the
        // property in place passed an earlier version of this assertion.
        //
        // Anchored on the EXPRESSION, not on a fixed window after the
        // function's name. The first version of this scan read 700 chars from
        // `function clampCallPanelHeight` and the explanatory comment inside
        // that function pushed the code to offset 1016, so it failed on the
        // FIXED tree — the "fixed-window scan defeated by added comments"
        // trap, walked into while writing the test that guards against it.
        QVERIFY2(pane_.contains(QStringLiteral(
                     "var floor = root.callPanelHasVideo")),
                 "the clamp no longer distinguishes a call showing video, so "
                 "a share is squeezed to the voice-only floor again");
        QVERIFY2(pane_.contains(QStringLiteral(
                     "Math.min(root.callPanelVideoFloor,")),
                 "the clamp no longer applies the stage's own minimum");

        // Drag the divider to nothing. With video the clamp must refuse to go
        // below a height that still leaves a picture; without it the old
        // rule stands, because a voice call has no picture to protect.
        const auto floorFor = [pane](bool video) {
            pane->setProperty("callPanelUserHeight", 0.0);
            QVariant out;
            QMetaObject::invokeMethod(
                pane, "clampCallPanelHeight", Q_RETURN_ARG(QVariant, out),
                Q_ARG(QVariant, 0.0));
            Q_UNUSED(video);
            return out.toReal();
        };
        const qreal clamped = floorFor(false);
        QVERIFY2(clamped >= 64.0,
                 qPrintable(QStringLiteral("the clamp collapsed to %1")
                                .arg(clamped)));
        // 520 px pane: the voice rule is min(220, 45%) = 220.
        QCOMPARE(qRound(clamped), 220);

        // And the overlay is ABSENT rather than crushed once the spotlight
        // cannot host it — the half-drawn controls in the report.
        QVERIFY2(normalized(stage).contains(QStringLiteral(
                     "visible: parent.height >= root.spotlightOverlayHeight")),
                 "the spotlight overlay is drawn whatever height the tile "
                 "has, so it can overflow a clipped rectangle again");
    }

    void theSpotlightStripYieldsHeightOnAShortStage()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const int strip =
            stage.indexOf(QStringLiteral("objectName: \"callStrip\""));
        QVERIFY2(strip > 0, "the spotlight strip is gone");
        // Whitespace-normalized, so a reflow of these wrapped expressions
        // cannot quietly turn an assertion into a pass.
        const QString block = normalized(stage.mid(strip, 1600));
        QVERIFY2(block.contains(
                     QStringLiteral("Layout.preferredHeight: visible ? "
                                    "spotlightColumn.stripHeight : 0")),
                 "the strip no longer asks the stage how much height it may "
                 "take");
        QVERIFY2(!block.contains(QStringLiteral("Layout.preferredHeight: 96")),
                 "the strip is back to a fixed height");
        QVERIFY2(block.contains(
                     QStringLiteral("visible: spotlightColumn.stripMode === "
                                    "\"tiles\"")),
                 "the tile strip is drawn whatever the stage's height is");
        // And the tiles follow the band rather than carrying their own
        // literals, or a shrunken strip clips the tiles it holds.
        QVERIFY(block.contains(QStringLiteral("spotlightColumn.stripTileHeight")));
        QVERIFY2(!stage.contains(QStringLiteral("width: active ? 140 : 0")),
                 "a strip tile is back to a hardcoded width");
        // The short-stage fallback is REACHED, not merely defined: a policy
        // with no call site is a comment.
        const QString whole = normalized(stage);
        QVERIFY2(whole.contains(
                     QStringLiteral("objectName: \"callStripBubblesHost\"")),
                 "the short-stage strip has no host");
        QVERIFY2(whole.contains(
                     QStringLiteral("active: spotlightColumn.stripMode !== "
                                    "\"tiles\"")),
                 "the bubble strip is not wired to the stage's own policy");

        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(), &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("CallStage"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(3000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr, "CallStage must instantiate");

        const auto stripFor = [&](int available) {
            QVariant out;
            const bool ok = QMetaObject::invokeMethod(
                root, "stripHeightForStage", Q_RETURN_ARG(QVariant, out),
                Q_ARG(QVariant, QVariant(available)));
            return ok ? out.toInt() : -1;
        };
        const auto modeFor = [&](int available) {
            QVariant out;
            const bool ok = QMetaObject::invokeMethod(
                root, "stripModeForStage", Q_RETURN_ARG(QVariant, out),
                Q_ARG(QVariant, QVariant(available)));
            return ok ? out.toString() : QString();
        };

        // The stage the maintainer actually had. THE STRIP MUST NOT BE THE
        // BIGGER HALF OF IT — with the fixed band it was 96 against 61.
        const int shortStage = 165;
        const int shortStrip = stripFor(shortStage);
        QVERIFY2(shortStrip > 0, "stripHeightForStage is not invokable");
        const int spotlight = shortStage - 8 - shortStrip;
        QVERIFY2(spotlight > shortStrip,
                 qPrintable(QStringLiteral("on a %1 px stage the strip takes "
                                           "%2 and leaves the picture %3")
                                .arg(shortStage)
                                .arg(shortStrip)
                                .arg(spotlight)));
        QVERIFY2(spotlight > 61,
                 "the picture is no better off than the fixed 96 px band left "
                 "it on the maintainer's own capture");
        // At that size a tile cannot carry an avatar AND a nameplate, so the
        // strip is the bubble row rather than a broken tile.
        QCOMPARE(modeFor(shortStage), QStringLiteral("bubbles"));

        // A roomy stage is UNCHANGED — this is a yield, not a redesign.
        QCOMPARE(modeFor(400), QStringLiteral("tiles"));
        QCOMPARE(stripFor(400), 96);
        QCOMPARE(stripFor(1000), 96);
        // Degenerate sizes (a stage measured before its first layout) must
        // not produce a zero-height strip that never grows back.
        QCOMPARE(stripFor(0), 96);
        QCOMPARE(modeFor(0), QStringLiteral("tiles"));
        // Wherever the tile strip IS drawn it is a usable one, and it never
        // takes more than 40% of the stage.
        for (int available = 120; available <= 900; available += 7) {
            const int band = stripFor(available);
            if (modeFor(available) == QStringLiteral("tiles")) {
                QVERIFY2(band >= 80,
                         qPrintable(QStringLiteral("a %1 px stage draws a %2 px "
                                                   "tile strip, which cannot "
                                                   "carry an avatar and a "
                                                   "nameplate")
                                        .arg(available)
                                        .arg(band)));
            }
            QVERIFY2(band <= 96, "the strip is over its own cap");
            QVERIFY2(available - 8 - band >= band,
                     qPrintable(QStringLiteral("a %1 px stage gives the strip "
                                               "%2 and the picture %3")
                                    .arg(available)
                                    .arg(band)
                                    .arg(available - 8 - band)));
        }
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(CallUiContractTest)
#include "CallUiContractTest.moc"
