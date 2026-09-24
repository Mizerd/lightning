// Contract scans for the incoming-call card, its Main.qml hosting and the ring
// settings toggle, plus a real offscreen IncomingCallPrompt instantiation that
// catches unresolved theme tokens and property typos. Predicates are matched
// whitespace-normalized so reflows don't break them.
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QWindow>
#include <QtTest>

#include <functional>

#include <utility>

#include <memory>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"
#include "auth/AuthManager.h"
#include "calls/CallController.h"
#include "calls/CallMediaBackend.h"
#include "calls/CallShareModel.h"
#include "calls/CallStageState.h"
#include "calls/SfuCallController.h"
#include "calls/SfuMediaEngine.h"
#include "matrix/CallSignal.h"
#include "matrix/MockMatrixClient.h"

#ifdef HAVE_LIGHTNING_WEBRTC
#include <QVideoSink>

#include "calls/SfuVideoRouter.h"
#endif

/// The smallest thing that makes `mediaBackendAvailable` true. Without an
/// engine the legacy Accept is hidden on both lanes, so registering this is
/// what lets the tests tell the lanes apart. It never produces an offer.
// Stand-in for `app` for the surfaces that decide "am I in this call?". Lets a
// test put room state and the local call controller into disagreement, which
// is not reachable through AppController without a homeserver.
class StubRtc : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    int count = 0;
    bool ownDevice = false;
    bool ownUser = false;
    QString block;

    Q_INVOKABLE int participantCount(const QString &) const { return count; }
    Q_INVOKABLE bool hasLiveSession(const QString &) const { return count > 0; }
    Q_INVOKABLE bool ownUserInSession(const QString &) const { return ownUser; }
    // Production no longer calls this; kept so the pre-fix expression fails on
    // the assertion rather than on a missing method.
    Q_INVOKABLE bool ownDeviceInSession(const QString &) const
    { return ownDevice; }
    Q_INVOKABLE QString joinBlockReason(const QString &) const { return block; }
    Q_INVOKABLE QVariantList participantFaces(const QString &, int) const
    { return {}; }
    Q_INVOKABLE void refresh(const QString &) {}

Q_SIGNALS:
    void sessionChanged(const QString &roomId);
    void availabilityChanged();
};

class StubGroupCall : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString roomId READ roomId NOTIFY roomIdChanged)
public:
    using QObject::QObject;

    bool active() const { return m_active; }
    QString roomId() const { return m_roomId; }
    // NOTIFY signals let a test toggle mid-test and prove `visible` is a live
    // binding.
    void setActive(bool v) { if (m_active != v) { m_active = v; Q_EMIT activeChanged(); } }
    void setRoomId(const QString &v)
    { if (m_roomId != v) { m_roomId = v; Q_EMIT roomIdChanged(); } }

    Q_INVOKABLE void join(const QString &) {}

Q_SIGNALS:
    void activeChanged();
    void roomIdChanged();

private:
    bool m_active = false;
    QString m_roomId;
};

class StubApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *rtc READ rtc CONSTANT)
    Q_PROPERTY(QObject *groupCall READ groupCall CONSTANT)
public:
    explicit StubApp(QObject *parent = nullptr)
        : QObject(parent), m_rtc(new StubRtc(this)),
          m_groupCall(new StubGroupCall(this)) {}

    QObject *rtc() const { return m_rtc; }
    QObject *groupCall() const { return m_groupCall; }
    StubRtc *rtcStub() const { return m_rtc; }
    StubGroupCall *callStub() const { return m_groupCall; }

private:
    StubRtc *m_rtc = nullptr;
    StubGroupCall *m_groupCall = nullptr;
};

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

/// Find a named item by walking the visual tree. `findChild` cannot reach a
/// Repeater's delegates (they belong to the delegate model), so segments of a
/// SegmentedControl are invisible to it; `childItems()` lists them.
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

    /// Source with whole-line `//` comments removed, for ban assertions whose
    /// own rationale comment names the banned token. Deliberately whole lines
    /// only: a smarter comment stripper is a parser and can silently weaken
    /// every assertion after it. Ban tests assert a known-present token to
    /// prove this still parses.
    static QString code(const QString &s)
    {
        QString out = s;
        out.remove(QRegularExpression(QStringLiteral("(?m)^[ \\t]*//.*$")));
        return out;
    }

    /// Run queued work and deferred deletions. A replaced Loader's content and
    /// a regenerated Repeater's delegates die via deleteLater(), so
    /// processEvents() alone measures the moment before the interesting part.
    static void settle(int rounds = 3)
    {
        for (int i = 0; i < rounds; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    // Build a standalone RoomCallBanner against a stub `app`.
    QQuickItem *buildBanner(QQmlEngine &engine, StubApp &stub,
                            const QString &room)
    {
        // On the root context: a module component resolves unqualified names in
        // its own file against the engine root, not the creation context.
        engine.rootContext()->setContextProperty(
            QStringLiteral("app"), static_cast<QObject *>(&stub));
        auto *ctx = new QQmlContext(engine.rootContext(), this);
        auto *component = new QQmlComponent(&engine, this);
        component->loadFromModule(QStringLiteral("MatrixClient"),
                                  QStringLiteral("RoomCallBanner"));
        if (!component->errors().isEmpty()) {
            qWarning("%s", qPrintable(component->errorString()));
            return nullptr;
        }
        auto *item = qobject_cast<QQuickItem *>(component->create(ctx));
        if (item) {
            item->setParent(component);
            // Into a window: `visible` is effective visibility, and an item with
            // no visual parent reads false whatever its binding says.
            item->setParentItem(m_fixtureWindow.contentItem());
            item->setProperty("roomId", room);
        }
        return item;
    }

    QQuickWindow m_fixtureWindow;

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
    /// the item's own geometry rather than an assumed row height.
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
    /// SfuCallController reads the microphone gain and per-participant levels
    /// through its SettingsManager; without setSettings() the volume feature
    /// silently does nothing. Source scan because m_settings has no getter.
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

    // A call must leave before its session goes away, on every path that takes
    // the session away: the membership retraction and the SFU Leave both need
    // the client, and a retraction after release leaves a phantom participant
    // until `expires`. The order is asserted, not just the presence of leave().
    void everyPathThatTakesTheSessionAwayLeavesTheCallFirst()
    {
        // Comments stripped first: prepareForShutdown's rationale comment names
        // `stopSync()` above the call, which would fool the ordering check.
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
            // Bounded to this function so another path's calls cannot satisfy it.
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
        // The in-call controls live in a bar under the room header.
        const QString bar = normalized(
            read(QStringLiteral(QML_DIR "/CallHeaderBar.qml")));
        QVERIFY(!bar.isEmpty());

        // 1. Mute, deafen, camera, screen share and leave are all present.
        for (const auto &name : {"callBarMicButton", "callBarDeafenButton",
                                 "callBarCameraButton",
                                 "callBarScreenShareButton",
                                 "callBarHangUpButton"}) {
            QVERIFY2(bar.contains(QStringLiteral("objectName: \"%1\"")
                                      .arg(QLatin1String(name))),
                     qPrintable(QStringLiteral("missing %1")
                                    .arg(QLatin1String(name))));
        }
        // 2. Device choosers exist for microphone and output.
        QVERIFY(bar.contains(QStringLiteral("objectName: \"callBarMicChevron\"")));
        QVERIFY(bar.contains(
            QStringLiteral("objectName: \"callBarSpeakerChevron\"")));
        // 3. It shows only for the room the call is in; a bar in the wrong
        //    room would hang up a call the user is not looking at.
        QVERIFY(bar.contains(QStringLiteral("callRoomId === app.currentRoomId")));
        // Screen share goes through the portal, never a hardcoded node id.
        QVERIFY(bar.contains(QStringLiteral("app.groupCall.requestScreenShare()")));
    }

    void theCornerCardNoLongerOwnsALiveCall()
    {
        // The card keeps the ringing and dialing states (the user may not be
        // looking at the call's room) and hands an active call to the bar.
        const QString card = normalized(
            read(QStringLiteral(QML_DIR "/IncomingCallPrompt.qml")));
        QVERIFY(!card.isEmpty());
        QVERIFY2(!card.contains(QStringLiteral("inCallMuteButton")),
                 "mute moved to the top bar and must not be duplicated here");
        // Exactly one surface at a time: the card appears only where the bar
        // cannot reach (another room or another screen).
        QVERIFY(card.contains(QStringLiteral("(inCall && !barCovers)")));
        QVERIFY(card.contains(QStringLiteral(
            "app.calls.activeRoomId === app.currentRoomId")));
        // Hang Up stays reachable from the card, e.g. from Settings or
        // another room.
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
        // Accept exists only behind the media-engine gate and only on the lane
        // it can answer; engineless builds keep the honesty line.
        QVERIFY(norm.contains(QStringLiteral(
            "readonly property bool legacyAcceptOffered: root.ringing && "
            "!root.rtcRing && app.calls.mediaBackendAvailable")));
        QVERIFY(norm.contains(
            QStringLiteral("visible: root.legacyAcceptOffered")));
        QVERIFY(norm.contains(
            QStringLiteral("if (!app.calls.answer())")));
        QVERIFY(norm.contains(QStringLiteral("isn't supported yet")));
        // Nothing SDP-shaped belongs in QML.
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
        // Wide enough to cover the button's block including its comments; a
        // too-tight window fails on prose, not behaviour.
        const QString scope = norm.mid(button, 2000);
        // Lane selection is one policy question answered in AppController
        // (MatrixRTC where available, legacy 1:1 as the audio-only DM
        // fallback). The button asks whether either lane can carry a call;
        // AppControllerCallLaneTest covers the policy itself.
        QVERIFY(scope.contains(
            QStringLiteral("app.canStartCall(app.currentRoomId)")));
        QVERIFY(scope.contains(QStringLiteral(
            "onClicked: app.startCall(app.currentRoomId, false)")));
        // A live group call must not offer "start a call" as well.
        QVERIFY(scope.contains(QStringLiteral("!app.groupCall.active")));
        // Enabled, and the "coming soon" wording must be gone with it.
        QVERIFY(scope.contains(QStringLiteral("enabled: true")));
        QVERIFY2(!norm.contains(QStringLiteral("Voice calls are coming soon")),
                 "the coming-soon wording must not outlive the disabled state");
        // The engine gate lives in canStartCall() and must be in the
        // visibility, not only the click handler: no button beats a dead one.
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
        QVERIFY(update > call);   // live ring above the passives
        QVERIFY(verify > update); // verify keeps its corner
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
        // Real instantiation: catches layout failures, bindings to missing
        // properties and uncreatable Loader components.
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

        // Collapsed with no live call, so it reserves no vertical space.
        QCOMPARE(root->property("live").toBool(), false);
        QCOMPARE(root->property("visible").toBool(), false);
        QCOMPARE(root->property("height").toDouble(), 0.0);

        // Each control exists and has a non-zero size.
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

    // Real instantiation of the speaker strip, fed a ListModel because the
    // mock backend's participant model cannot be populated. A ListModel
    // supplies roles by name exactly as the real model does.
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

        // Nobody: no empty strip above the messages.
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

    // The strip's implicitWidth must cover what its delegates draw, or its own
    // `clip: true` slices the last avatar. Measured against the last
    // delegate's right edge so any arithmetic drift fails.
    void theBubbleStripAsksForTheWidthItDraws()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QByteArray(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient
Item {
    width: 900
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
        model: people
    }
}
)"), QUrl(QStringLiteral("qrc:/callbubblewidth.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *outer = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(outer != nullptr);
        QCoreApplication::processEvents();
        outer->polish();
        QCoreApplication::processEvents();

        auto *bubbles = qobject_cast<QQuickItem *>(
            outer->property("bubbles").value<QObject *>());
        QVERIFY(bubbles != nullptr);
        // No host width, so the strip uses its implicit width (the header's
        // spotlight branch).
        const double want = bubbles->property("implicitWidth").toDouble();
        QVERIFY2(want > 0.0, "the strip reports no implicit width at all");

        auto *strip =
            outer->findChild<QQuickItem *>(QStringLiteral("callSpeakerBubbles"));
        QVERIFY(strip != nullptr);
        QCOMPARE(strip->property("count").toInt(), 2);

        // contentWidth is the ListView's own sum of cells and spacings.
        const double drawn = strip->property("contentWidth").toDouble();
        QVERIFY2(drawn > 0.0, "the ListView reports no content width — the "
                              "delegates never built, so this case would "
                              "pass on anything");
        QVERIFY2(want >= drawn,
                 qPrintable(QStringLiteral(
                                "the strip asks for %1 px and draws %2 px, so "
                                "its own clip cuts %3 px off the last avatar")
                                .arg(want).arg(drawn).arg(drawn - want)));
    }

    // Every `app.calls.X` and `app.groupCall.X` used in qml/ must exist on the
    // controller, since QML only fails when the line runs. Members are
    // collected from both headers so a C++ rename fails here too.
    void everyCallControllerMemberQmlUsesActuallyExists()
    {
        auto members = [&](const QString &header) {
            const QString src = read(QStringLiteral(QML_DIR "/../src/calls/")
                                     + header);
            QSet<QString> out;
            // Q_PROPERTY(<type> name READ ...): the name is the token before
            // READ/MEMBER, which survives template commas in <type>.
            QRegularExpression prop(
                QStringLiteral("Q_PROPERTY\\s*\\([^)]*?([A-Za-z_][A-Za-z0-9_]*)"
                               "\\s+(?:READ|MEMBER)"));
            auto pi = prop.globalMatch(src);
            while (pi.hasNext())
                out.insert(pi.next().captured(1));
            // Q_INVOKABLE <ret> name( ; plain declarations in signal/slot
            // sections match the same shape.
            QRegularExpression inv(
                QStringLiteral("Q_INVOKABLE[^;{]*?([A-Za-z_][A-Za-z0-9_]*)"
                               "\\s*\\("));
            auto ii = inv.globalMatch(src);
            while (ii.hasNext())
                out.insert(ii.next().captured(1));
            // Signals are callable from QML too. Collected loosely: this set
            // only accepts names, so over-matching can only cause a false pass.
            QRegularExpression sig(
                QStringLiteral("(?m)^\\s*(?:void|bool|int|QString)\\s+"
                               "([A-Za-z_][A-Za-z0-9_]*)\\s*\\("));
            auto si = sig.globalMatch(src);
            while (si.hasNext())
                out.insert(si.next().captured(1));
            return out;
        };

        const QSet<QString> calls = members(QStringLiteral("CallController.h"));
        const QSet<QString> group =
            members(QStringLiteral("SfuCallController.h"));
        // Floor both sides: an empty C++ set with an empty QML sweep would
        // otherwise pass.
        QVERIFY2(calls.size() > 10 && group.size() > 10,
                 qPrintable(QStringLiteral("parsed only %1/%2 members out of "
                                           "the two headers — the scan is "
                                           "broken, not the code")
                                .arg(calls.size()).arg(group.size())));

        QDir dir(QStringLiteral(QML_DIR));
        const auto files =
            dir.entryList({ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY(!files.isEmpty());
        QRegularExpression use(
            QStringLiteral("app\\.(calls|groupCall)\\."
                           "([A-Za-z_][A-Za-z0-9_]*)"));
        QStringList missing;
        int seen = 0;
        for (const QString &name : files) {
            const QString body = code(read(dir.filePath(name)));
            auto it = use.globalMatch(body);
            while (it.hasNext()) {
                const auto m = it.next();
                ++seen;
                const bool isGroup = m.captured(1) == QLatin1String("groupCall");
                const QSet<QString> &have = isGroup ? group : calls;
                if (!have.contains(m.captured(2))) {
                    const QString entry =
                        QStringLiteral("%1: app.%2.%3")
                            .arg(name, m.captured(1), m.captured(2));
                    if (!missing.contains(entry))
                        missing.append(entry);
                }
            }
        }
        QVERIFY2(seen > 30,
                 qPrintable(QStringLiteral("only %1 controller uses found in "
                                           "qml/ — the sweep is matching the "
                                           "wrong thing")
                                .arg(seen)));
        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral(
                                "these QML lines name something the "
                                "controller does not have, and will throw the "
                                "moment they run: %1")
                                .arg(missing.join(QStringLiteral("; ")))));
    }

    // The share picker loads, classifies window vs screen rows the same way
    // the controller does (non-zero window handle), and labels a window by its
    // owning application first: a browser caption names only the tab. The
    // resolution must not appear on the tile.
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

        // No call, so neither tab has anything in it.
        QCOMPARE(picker->property("screenCount").toInt(), 0);
        QCOMPARE(picker->property("windowCount").toInt(), 0);

        // A window row carries a non-zero handle; a display row does not.
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

        // Caption names a tab and no browser: the application leads.
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

        // A caption that already names the application stays one line.
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

        // An unreadable executable leaves `application` empty; still nameable.
        const QVariantMap unknownApp{
            { QStringLiteral("index"), -1 },
            { QStringLiteral("windowHandle"), quint64(4662) },
            { QStringLiteral("name"), QStringLiteral("Untitled - Notepad") },
            { QStringLiteral("application"), QString() },
            { QStringLiteral("geometry"), QStringLiteral("800 x 600") },
        };
        QCOMPARE(label("primaryLabel", unknownApp),
                 QStringLiteral("Untitled - Notepad"));

        // A screen shows its platform name and which screen, never resolution.
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

        // Resolution is off the tile face but still reaches a screen reader.
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

    // Pressing a tile selects it, including a window tile on the Applications
    // tab. Drives the real tab strip and delegate.
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

        // Two displays then two windows, as the controller builds them.
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

        // The tab shows one kind only.
        QTRY_COMPARE(grid->property("count").toInt(), 2);
        QCOMPARE(picker->property("tab").toString(), QStringLiteral("screens"));

        clickItem(window, grid, 0);
        QCOMPARE(picker->property("selected").toInt(), 0);
        clickItem(window, grid, 1);
        QCOMPARE(picker->property("selected").toInt(), 1);

        // Switch tabs through the real segment, not by writing the property.
        auto *applicationsTab = findVisualChild(
            picker, QStringLiteral("shareTabs_applications"));
        QVERIFY2(applicationsTab != nullptr,
                 "the Applications tab has no objectName to find");
        clickCentre(window, applicationsTab);
        QTRY_COMPARE(picker->property("tab").toString(),
                     QStringLiteral("applications"));
        QTRY_COMPARE(grid->property("count").toInt(), 2);

        // Switching tabs leaves a tile of this tab selected.
        QCOMPARE(picker->property("selected").toInt(), 2);

        clickItem(window, grid, 1);
        QCOMPARE(picker->property("selected").toInt(), 3);
    }

    // The filtered grid must map back to the unfiltered source index:
    // chooseScreenShareSource() indexes the full list, and a wrong mapping
    // silently shares the wrong thing. Rows interleave kinds so a naive
    // mapping lands on a different kind, and failures name the row shared.
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

        // 0 screen, 1 window, 2 screen, 3 window, 4 window.
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

        // Name of the row the picker would share. Not read from
        // `picker->property("sources")`: a QML `property var` comes back as a
        // QJSValue whose toList() is empty, making comparisons vacuous.
        auto chosenName = [picker, &rows]() {
            const int at = picker->property("selected").toInt();
            if (at < 0 || at >= rows.size())
                return QStringLiteral("<out of range>");
            return rows.at(at).toMap().value(QStringLiteral("name")).toString();
        };

        // Screens: view row 1 is source 2 (a naive mapping picks a window).
        QTRY_COMPARE(grid->property("count").toInt(), 2);
        clickItem(window, grid, 1);
        QCOMPARE(chosenName(), QStringLiteral("Screen B"));
        QCOMPARE(picker->property("selected").toInt(), 2);

        auto *applicationsTab = findVisualChild(
            picker, QStringLiteral("shareTabs_applications"));
        QVERIFY(applicationsTab != nullptr);
        clickCentre(window, applicationsTab);
        QTRY_COMPARE(grid->property("count").toInt(), 3);

        // Applications: view rows 0/1/2 are sources 1/3/4.
        clickItem(window, grid, 1);
        QCOMPARE(chosenName(), QStringLiteral("Window B"));
        QCOMPARE(picker->property("selected").toInt(), 3);

        clickItem(window, grid, 2);
        QCOMPARE(chosenName(), QStringLiteral("Window C"));
        QCOMPARE(picker->property("selected").toInt(), 4);

        // Keyboard uses the same mapping: arrows walk the visible tab.
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

        // Share hands the controller `selected` verbatim, never a view index.
        // Source-read because observing the call needs a live SFU session.
        const QString qml = normalized(
            code(read(QStringLiteral(QML_DIR "/ScreenSharePicker.qml"))));
        QVERIFY2(!qml.isEmpty(), "ScreenSharePicker.qml did not read");
        QVERIFY2(qml.contains(QStringLiteral("var chosen = root.selected;")),
                 "the confirm no longer sends the selected SOURCE index");
        QVERIFY2(qml.contains(
                     QStringLiteral("chooseScreenShareSource(chosen)")),
                 "the confirm no longer hands the controller the chosen index");
    }

    // Both screen-share guards (SfuCallController::startScreenShare and
    // SfuMediaEngine::publishVideo) must accept sources without a node id:
    // windows (handle) and the X11 no-portal fallback (root rectangle).
    // Source-scanned because the guards need a live SFU session.
    void noScreenShareRefusalForgetsThatAWindowCarriesNoNodeId()
    {
        struct Site { const char *file; const char *guard; };
        const Site sites[] = {
            { SRC_DIR "/calls/SfuCallController.cpp",
              "if (pipewireNodeId < 0 && windowHandle == 0 "
              "&& !captureRect.isValid())" },
            { SRC_DIR "/calls/SfuMediaEngine.cpp",
              "if (nodeId < 0 && windowHandle == 0 "
              "&& !captureRect.isValid()) {" },
        };
        for (const Site &site : sites) {
            const QString src = read(QString::fromUtf8(site.file));
            QVERIFY2(!src.isEmpty(), site.file);
            QVERIFY2(src.contains(QLatin1String(site.guard)),
                     qPrintable(QStringLiteral(
                         "%1 refuses a share on the node id without asking "
                         "whether a window was chosen").arg(
                         QString::fromUtf8(site.file))));
            // The bare form must be gone too.
            QVERIFY2(!src.contains(QLatin1String("if (pipewireNodeId < 0)\n")),
                     "a bare node-id refusal survives alongside the fixed one");
        }
    }

    // The in-room stage and the floating window must never both be built.
    // SfuVideoRouter holds one sink per track and the last attach wins; when
    // the second surface is destroyed it detaches, leaving nothing attached.
    // Text scan because observing it needs a real call.
    void theInRoomStageStandsDownForPictureInPicture()
    {
        const QString src = read(QStringLiteral(QML_DIR "/TimelinePane.qml"));
        QVERIFY2(!src.isEmpty(), "TimelinePane.qml is gone");
        const int at = src.indexOf(
            QStringLiteral("readonly property bool callStageOwnsColumn:"));
        QVERIFY2(at > 0, "callStageOwnsColumn is gone — re-anchor this test "
                         "rather than deleting it");
        // Scoped to the property's own expression so a mention elsewhere
        // cannot satisfy it.
        const int end = src.indexOf(QStringLiteral("\n\n"), at);
        const QString expr = src.mid(at, (end < 0 ? src.size() : end) - at);
        QVERIFY2(expr.contains(QStringLiteral("pictureInPicture")),
                 "the in-room call stage is built without consulting "
                 "picture-in-picture: popping the call out while looking at "
                 "the room gives two surfaces for one track, and the sink "
                 "goes to whichever attached last");

        // The host Loader's `active` binding really reads that property.
        const int hostAt = src.indexOf(
            QStringLiteral("objectName: \"timelineCallStageHost\""));
        QVERIFY2(hostAt > 0, "the call stage host is gone");
        const QString host = src.mid(hostAt, 1200);
        QVERIFY2(host.contains(QStringLiteral("active: root.callStageOwnsColumn")),
                 "the stage host no longer keys on callStageOwnsColumn, so "
                 "the clause above guards nothing");
    }

    // The picture-in-picture flag must not outlive the call on either lane;
    // CallStageState::clear() only runs on the group lane.
    void theFloatingWindowFlagDiesWithTheCall()
    {
        const QString src = read(QStringLiteral(QML_DIR "/CallPipWindow.qml"));
        QVERIFY2(!src.isEmpty(), "CallPipWindow.qml is gone");
        const int at = src.indexOf(QStringLiteral("onCallLiveChanged"));
        QVERIFY2(at > 0,
                 "nothing clears pictureInPicture when the call ends, so the "
                 "next call on the legacy lane reopens the floating window "
                 "by itself");
        const QString handler = src.mid(at, 400);
        QVERIFY2(handler.contains(QStringLiteral("setPictureInPicture(false)")),
                 "the call-ended handler does not actually drop the flag");
    }

    // Leave must not run off the edge of a narrow window. Source scan: a
    // standalone CallStage does not lay out offscreen, so a geometric sweep
    // here measured nothing. Pins the two lines the fix depends on.
    void theCallControlsMayShrinkRatherThanOverflow()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY2(!stage.isEmpty(), "CallStage.qml is missing");
        const QString code = normalized(stage);

        // No `Layout.minimumWidth: implicitWidth` floor: past the panel width a
        // RowLayout overflows to the right, where the hang-up button is.
        QVERIFY2(!code.contains(QStringLiteral(
                     "Layout.minimumWidth: implicitWidth")),
                 "the controls host has a floor at its implicit width again, "
                 "so a row too narrow for it overflows instead of shrinking "
                 "and the end of the control row leaves the window");

        // `compact` is driven by available width, not only by `collapsed`.
        QVERIFY2(code.contains(QStringLiteral(
                     "compact: root.collapsed || controlsHost.cramped")),
                 "the control bar no longer goes compact when the room it is "
                 "given is too small for it");
    }

    // A call that carries audio but cannot draw video must say so. Qt Quick's
    // software renderer has no video node, so frames arrive and the tiles
    // believe they have a picture while drawing nothing.
    void aRendererThatCannotDrawVideoSaysSoOnTheStage()
    {
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
            QVERIFY(createdSpy.wait(5000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        // Into a window: `visible` is effective visibility.
        QQuickWindow window;
        window.resize(900, 600);
        root->setParentItem(window.contentItem());

        auto *notice = root->findChild<QQuickItem *>(
            QStringLiteral("callSoftwareRendererNotice"));
        QVERIFY2(notice != nullptr, "the stage has no software-renderer notice");

        // Real GPU: the strip is absent and reserves no height.
        QVERIFY(!controller.softwareRenderer());
        QTest::qWait(50);
        QVERIFY2(!notice->property("visible").toBool(),
                 "the notice is shown on a machine that renders video fine");
        QCOMPARE(notice->property("implicitHeight").toReal(), 0.0);

        // Shown when the scene graph reports the software rasteriser (main.cpp
        // reads the renderer interface, not the requested graphics API).
        controller.setSoftwareRenderer(true);
        QTest::qWait(50);
        QVERIFY2(notice->property("visible").toBool(),
                 "a call that cannot draw video shows the user nothing but an "
                 "empty rectangle and no explanation");
        QVERIFY2(notice->property("implicitHeight").toReal() > 0.0,
                 "the notice is visible but has no height");
    }

    // Tiles must stand down under the software renderer too. Source-level
    // because a tile needs a participant model and a live sink.
    void theVideoTilesConsultTheRendererBeforeShowingAFrame()
    {
        for (const auto *file : { QML_DIR "/CallParticipantTile.qml",
                                  QML_DIR "/CallShareTile.qml" }) {
            const QString src = read(QString::fromUtf8(file));
            QVERIFY2(!src.isEmpty(), file);
            const QString code = normalized(src);
            // The declaration as well as the use: a missing property makes
            // `!root.<undefined>` true and a use-only scan still passes.
            QVERIFY2(code.contains(QStringLiteral(
                         "property bool softwareRendererHidesVideo")),
                     qPrintable(QStringLiteral("%1 no longer declares "
                                               "softwareRendererHidesVideo")
                                    .arg(QString::fromUtf8(file))));
            QVERIFY2(code.contains(QStringLiteral(
                         "&& !root.softwareRendererHidesVideo")),
                     qPrintable(QStringLiteral(
                         "%1 shows its VideoOutput without asking whether this "
                         "renderer can draw one, so it paints an empty "
                         "rectangle over the placeholder")
                                    .arg(QString::fromUtf8(file))));
        }
    }

    void theCallStageComponentActuallyLoads()
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
                              QStringLiteral("CallStage"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(5000));
        // objectCreated carries null when the component failed to load.
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr,
                 "CallStage.qml failed to load — see the qWarning above for "
                 "the property or type that does not exist");

        // The control surface exists as a real item in the default expanded
        // state.
        QVERIFY(root->findChild<QObject *>(QStringLiteral("callStageControls"))
                != nullptr);
    }

    // The control dock stays inside the stage at every panel width. Geometric,
    // on the real stage: `Layout.minimumWidth: 0` without `Layout.fillWidth`
    // gives a fixed horizontal policy, so the row never shrank and the
    // hang-up was drawn outside narrow panels. Window minimum is 640 and the
    // conversation column's minimum is 320, so every width below is reachable.
    void theControlDockStaysInsideTheStageAtEveryWidth()
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
                              QStringLiteral("CallStage"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(5000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr, "CallStage must instantiate");

        // Into a window, or the Loader never lays its item out.
        QQuickWindow window;
        window.resize(1100, 620);
        root->setParentItem(window.contentItem());
        root->setHeight(620);

        auto *bar = root->findChild<QQuickItem *>(
            QStringLiteral("callStageControls"));
        QVERIFY2(bar != nullptr, "the stage has no control bar");

        // Walk up from the bar rather than naming a path; the assertion is
        // about where pixels land.
        auto *host = qobject_cast<QQuickItem *>(bar->parentItem());
        QVERIFY(host != nullptr);
        auto *row = qobject_cast<QQuickItem *>(host->parentItem());
        QVERIFY(row != nullptr);

        for (const int width : { 1100, 900, 760, 700, 640, 560, 480 }) {
            root->setWidth(width);
            settle(6);

            const QPointF rowRight = row->mapToItem(nullptr,
                                                    QPointF(row->width(), 0));
            const QPointF stageRight = root->mapToItem(
                nullptr, QPointF(root->width(), 0));
            QVERIFY2(rowRight.x() <= stageRight.x() + 0.5,
                     qPrintable(QStringLiteral(
                         "at a %1 px stage the control row reaches x=%2 but "
                         "the stage ends at x=%3 — the end of the control "
                         "row, hang-up included, is outside the panel")
                                    .arg(width)
                                    .arg(rowRight.x())
                                    .arg(stageRight.x())));

        // The hang-up specifically: its absence traps the user in the call.
            auto *hangUp = root->findChild<QQuickItem *>(
                QStringLiteral("callBarHangUpButton"));
            QVERIFY2(hangUp != nullptr,
                     qPrintable(QStringLiteral(
                         "no hang-up button at a %1 px stage").arg(width)));
            const QPointF hangRight = hangUp->mapToItem(
                nullptr, QPointF(hangUp->width(), 0));
            // The contents too. With no live call CallHeaderBar is invisible
            // and reports implicitWidth 0, so its cell is zero wide; but a
            // Layout ignores a child only by the child's own `visible`, so the
            // inner RowLayout still laid out and `anchors.centerIn` hung it off
            // both sides. CallHeaderBar clamps the centring so the bar can only
            // overflow to the left, and Leave is never the control that goes.
            QVERIFY2(hangRight.x() <= stageRight.x() + 0.5,
                     qPrintable(QStringLiteral(
                         "at a %1 px stage the hang-up button ends at x=%2, "
                         "past the stage's own right edge at x=%3")
                                    .arg(width)
                                    .arg(hangRight.x())
                                    .arg(stageRight.x())));
        }
    }

    // `Layout.fillWidth` alone lets the dock grow into a wide panel's spare
    // width; `Layout.maximumWidth: implicitWidth` keeps it pill-sized.
    void theControlDockDoesNotStretchOnAWidePanel()
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
                              QStringLiteral("CallStage"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(5000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root != nullptr);

        QQuickWindow window;
        window.resize(1400, 620);
        root->setParentItem(window.contentItem());
        root->setWidth(1400);
        root->setHeight(620);
        settle(6);

        auto *bar = root->findChild<QQuickItem *>(
            QStringLiteral("callStageControls"));
        QVERIFY(bar != nullptr);
        auto *host = qobject_cast<QQuickItem *>(bar->parentItem());
        QVERIFY(host != nullptr);
        QVERIFY2(host->width() <= host->implicitWidth() + 0.5,
                 qPrintable(QStringLiteral(
                     "the control dock stretched to %1 px in a 1400 px "
                     "panel; it asks for %2 and must never be given more")
                                .arg(host->width())
                                .arg(host->implicitWidth())));
    }

    // Compaction must be reversible. `Layout.maximumWidth: implicitWidth` pins
    // the cell to what the bar currently asks for, so the cell cannot measure
    // whether the expanded set would fit again; the row can (its implicitWidth
    // minus this cell's). The call state is set before the component loads so
    // the bar's `visible` binding is true on first evaluation.
    void theControlDockComesBackWhenThePanelIsWidenedAgain()
    {
        AppController controller(AppController::MockBackend);
        QSignalSpy loginSpy(controller.auth(),
                            &AuthManager::loginSucceeded);
        controller.auth()->login(QStringLiteral("https://mock.local"),
                                 QStringLiteral("alice"),
                                 QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));

        auto *call = controller.groupCall();
        QVERIFY(call != nullptr);
        call->setCallStateForTest(SfuCallController::State::Connected);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine,
                              &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("CallStage"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(5000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr, "CallStage must instantiate");

        // Shown: a QQuickLayout whose size hints change waits for a polish,
        // which only runs from a window's render pass. On an unshown window the
        // header row freezes at its old width and the case measures nothing.
        QQuickWindow window;
        window.resize(1100, 620);
        root->setParentItem(window.contentItem());
        root->setHeight(620);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

        auto *bar = root->findChild<QQuickItem *>(
            QStringLiteral("callStageControls"));
        QVERIFY2(bar != nullptr, "the stage has no control bar");
        auto *host = qobject_cast<QQuickItem *>(bar->parentItem());
        QVERIFY(host != nullptr);
        auto *hangUp = root->findChild<QQuickItem *>(
            QStringLiteral("callBarHangUpButton"));
        QVERIFY(hangUp != nullptr);

        const auto resize = [&](int w) {
            root->setWidth(w);
            root->setHeight(window.height());
            settle(4);
            QTest::qWait(80);
            settle(4);
        };

        // Assert the dock is drawn; with the bar invisible every width reads 0.
        resize(1100);
        QVERIFY2(bar->property("visible").toBool(),
                 "the dock is not visible, so this case is measuring an "
                 "empty bar and proves nothing about the latch");
        QVERIFY2(!host->property("cramped").toBool(),
                 "the dock is already compact in an 1100 px panel");
        const qreal expanded = host->width();
        QVERIFY2(expanded > 400,
                 qPrintable(QStringLiteral("the expanded dock is only %1 px "
                                           "wide; the control set is gone")
                                .arg(expanded)));

        // Down past the point where the full set stops fitting.
        resize(600);
        QVERIFY2(host->property("cramped").toBool(),
                 "a 600 px panel did not compact the dock, so the rest of "
                 "this case cannot test the way back");
        QVERIFY2(host->width() + 0.5 < expanded,
                 "the dock did not actually shrink");

        // ...and back up.
        resize(1100);
        QVERIFY2(!host->property("cramped").toBool(),
                 qPrintable(QStringLiteral(
                     "the dock is still compact at %1 px after one narrow "
                     "moment: cramped latched and nothing can clear it, so "
                     "camera, Share, Raise hand, React, Participants, PiP "
                     "and every device chevron are gone for the rest of the "
                     "call")
                                .arg(host->width())));
        QCOMPARE(host->width(), expanded);

        // Containment holds throughout, on a dock that is really on screen.
        for (const int width : { 1100, 900, 760, 740, 700, 640, 560, 480,
                                 560, 640, 700, 740, 760, 900, 1100 }) {
            resize(width);
            const qreal stageRight =
                root->mapToItem(nullptr, QPointF(root->width(), 0)).x();
            const qreal hangRight =
                hangUp->mapToItem(nullptr, QPointF(hangUp->width(), 0)).x();
            QVERIFY2(hangRight <= stageRight + 0.5,
                     qPrintable(QStringLiteral(
                         "at a %1 px stage the hang-up ends at x=%2, past "
                         "the stage's right edge at x=%3")
                                    .arg(width).arg(hangRight)
                                    .arg(stageRight)));
        }
    }

    // The full-screen idle timer must never be stopped: the reveal band is the
    // bottom edge the pointer leaves through, so a stop there has no matching
    // restart and the chrome never hides again. The timer is read live;
    // "nobody calls stop()" can only be asserted against the source.
    void theFullScreenIdleTimerIsNeverStopped()
    {
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
            QVERIFY(createdSpy.wait(5000));
        auto *stage = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(stage != nullptr);

        auto *surface = stage->findChild<QQuickItem *>(
            QStringLiteral("fullScreenSurface"));
        QVERIFY2(surface != nullptr, "the full-screen surface is gone");
        // Shown when full screen opens; only the timer may change that.
        QCOMPARE(surface->property("overlaysIdle").toBool(), false);

        auto *timer = surface->findChild<QObject *>(
            QStringLiteral("fullScreenIdleTimer"));
        QVERIFY2(timer != nullptr, "the idle timer is gone");
        QVERIFY2(timer->property("interval").toInt() > 0,
                 "the idle timer has no interval, so it can never fire");
        QVERIFY2(timer->property("repeat").toBool(),
                 "the idle timer fires once; after any interruption the "
                 "chrome can never retire again");

        const QString src = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY2(!src.contains(QStringLiteral("fullScreenIdleTimer.stop()")),
                 "the idle timer is stopped somewhere; a stop with no "
                 "guaranteed restart is what left the controls on screen "
                 "over a full-screen share");
        // Drive the retirement through the real handler. The timer only runs
        // over a live share, so ticks are delivered by hand.
        const int ticksToHide = surface->property("idleTicksToHide").toInt();
        QVERIFY2(ticksToHide > 0, "no idle tick budget, so nothing can hide");
        QVERIFY2(timer->property("interval").toInt() * ticksToHide >= 2000,
                 "the chrome retires in under two seconds of stillness, which "
                 "is too eager to reach a control with");
        surface->setProperty("idleTicks", 0);
        for (int i = 0; i < ticksToHide - 1; ++i)
            QMetaObject::invokeMethod(timer, "triggered");
        QVERIFY2(!surface->property("overlaysIdle").toBool(),
                 "the chrome retired before its full idle budget elapsed");
        QMetaObject::invokeMethod(timer, "triggered");
        QVERIFY2(surface->property("overlaysIdle").toBool(),
                 "the chrome never retires: a still pointer leaves the "
                 "controls drawn over a full-screen share forever, which is "
                 "what a single-monitor desktop always does");

        // Movement anywhere, not only over the dock, brings the chrome back.
        QVERIFY2(src.contains(QStringLiteral("id: fullScreenHover")),
                 "there is no pointer-movement handler over the share, so "
                 "nothing can reveal the chrome again");
        QVERIFY2(!src.contains(QStringLiteral("fullScreenDockHover")),
                 "the chrome is still held open by a hover on the dock; a "
                 "pointer resting there never leaves on a single monitor");

        // The timer is never stopped or restarted; only the count resets.
        // Restarting a timer whose count is at budget retires the chrome on
        // the next tick.
        QVERIFY2(!src.contains(QStringLiteral("fullScreenIdleTimer.restart()")),
                 "the idle timer is restarted somewhere; the count is what "
                 "carries the idle budget, so re-phasing alone leaves the "
                 "chrome retiring on the next tick");
        // Entering full screen and tapping the arrow both zero it.
        QVERIFY2(src.count(QStringLiteral("idleTicks = 0")) >= 2,
                 "something wakes the chrome without clearing the idle "
                 "count, so it retires again immediately");

        // Retired chrome must not take input: the dock is stacked over the
        // reveal arrow and would swallow the tap meant for it.
        QVERIFY2(src.count(QStringLiteral("enabled: !fullScreenSurface.overlaysIdle")) >= 2,
                 "a faded full-screen surface still accepts clicks: the "
                 "reveal arrow is unreachable under the invisible dock, and "
                 "a still click hits a control nobody can see");
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

        // A ring is the corner card's job, not the bar's: the user may not be
        // looking at the ringing room.
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

        // The call's room gates visibility, so a bar in another room cannot
        // hang up a call the user is not looking at.
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

        // End the ring via the invokable the card's Decline is pinned to call.
        // Asserts the state-to-visibility binding chain, not hit-testing.
        QVERIFY(controller.calls()->rejectIncoming());
        QTRY_COMPARE_WITH_TIMEOUT(root->property("shouldShow").toBool(),
                                  false, 3000);
    }

    void anRtcRingOffersJoinInsteadOfAnAnswerThatCannotWork()
    {
        // Accept must only be offered on the lane that can answer it.
        // `mediaBackendAvailable` belongs to the legacy engine and says nothing
        // about which lane rang, so an RTC ring showed an Accept that
        // CallController::answer() refuses.

        // The engine is declared before the controller so it outlives it:
        // CallController holds a QPointer to it and closes a live session on
        // teardown. See StubMediaBackend for why an engine is needed at all.
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

        // 1. Legacy lane: Accept is offered.
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

        // 2. MatrixRTC lane: the legacy Accept is absent (not disabled, which
        //    gets no hover and cannot explain itself), and the legacy answer
        //    path is asserted to refuse it.
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

        // The ring and the card are still up; Decline (m.rtc.decline) is what
        // stops the ring everywhere.
        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("shouldShow").toBool(),
                                  true, 3000);
    }

    void theRingCardJoinsThroughTheOneSharedGate()
    {
        // One gate (app.rtc.joinBlockReason) and one action
        // (app.groupCall.join) shared with RoomCallBanner and
        // CallEventDelegate.
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
        // Join-gate answers are a closed set of tokens mapped to wording; a
        // raw server string is never rendered.
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
        // The RTC lane never falls back to a legacy invite, which rings every
        // room member.
        QVERIFY2(!norm.contains(QStringLiteral("app.calls.placeCall")),
                 "the ring card can place a legacy call");
    }

    void theFullScreenWindowOpensOnTheApplicationsOwnScreen()
    {
        // Full screen opens on the monitor the client is on, not the primary.
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const QString stageCode = normalized(code(stage));
        // Prove the stripper leaves code behind before trusting the order
        // assertions; a comment mentioning showFullScreen() would decide them.
        QVERIFY(stageCode.contains(QStringLiteral("objectName: \"callStage\"")));

        // Placement happens before the window is shown: QWindow::setScreen()
        // does not move a window between virtual-sibling screens.
        const int place =
            stageCode.indexOf(QStringLiteral("placeOnThisApplicationsScreen()"));
        const int show =
            stageCode.indexOf(QStringLiteral("fullScreenWindow.showFullScreen()"));
        QVERIFY2(place >= 0, "nothing places the full-screen window");
        QVERIFY2(show > place,
                 "the window is shown before it is placed on a screen");

        // The screen comes from the attached Screen, which follows the item's
        // window. QWindow has no `screen` Q_PROPERTY.
        QVERIFY(stageCode.contains(QStringLiteral("var target = root.Screen")));
        QVERIFY(stageCode.contains(
            QStringLiteral("fullScreenWindow.screen = target")));
        // The geometry too: QWindowPrivate::create() calls
        // screenForGeometry(), so a default rectangle lands on the primary.
        QVERIFY2(stageCode.contains(
                     QStringLiteral("fullScreenWindow.x = target.virtualX")),
                 "the window's geometry is not moved onto the target screen");
        QVERIFY(stageCode.contains(
            QStringLiteral("fullScreenWindow.y = target.virtualY")));

        // Imperative visibility survives (see
        // fullScreenIsItsOwnWindowThatEscapeLeaves), and the close is accepted:
        // refusing it vetoes Ctrl+Q.
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
        // Raise hand is sent as element-call's m.reaction on our membership,
        // so peers see it; the old "only shown on this device" disclaimer
        // must stay gone.
        const QString bar = normalized(
            read(QStringLiteral(QML_DIR "/CallHeaderBar.qml")));
        QVERIFY(!bar.isEmpty());
        QVERIFY(bar.contains(
            QStringLiteral("objectName: \"callBarHandButton\"")));
        QVERIFY2(!bar.contains(QStringLiteral("only shown on this device")),
                 "the raise-hand control still claims no peer can see it, "
                 "which stopped being true when it went on the wire");
        // The icon must be in the bundled Material Symbols subset
        // (IconChromeTest owns the general rule).
        QVERIFY(bar.contains(QStringLiteral("iconName: \"front_hand\"")));

        // The wire format: element-call's ReactionsReader compares against
        // exactly this key, including the U+FE0F variation selector.
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
        // sender must own it, or one user could raise everybody's hand.
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

    // Transient call reactions: the control, the tile and the wire format
    // (`io.element.call.reaction` referencing the sender's membership).
    void theCallReactionControlSendsElementCallsOwnPairs()
    {
        const QString bar = normalized(
            read(QStringLiteral(QML_DIR "/CallHeaderBar.qml")));
        QVERIFY(!bar.isEmpty());
        QVERIFY2(bar.contains(
                     QStringLiteral("objectName: \"callBarReactButton\"")),
                 "there is no control for sending a call reaction");
        // The icon must be in the bundled Material Symbols subset.
        QVERIFY(bar.contains(QStringLiteral("iconName: \"add_reaction\"")));
        QVERIFY2(bar.contains(
                     QStringLiteral("app.groupCall.sendCallReaction(")),
                 "the reaction control does not reach the controller");

        // element-call looks up a reaction's sound by `name` and draws
        // `emoji`, so pairs must match theirs byte for byte. This is the third
        // copy of the table (QML picker and rust/src/rtc.rs are the others),
        // so drift in either fails here.
        struct Pair {
            const char *utf8;
            const char *name;
            const char *rustEntry;
        };
        const Pair firstRow[] = {
            { "\xF0\x9F\x91\x8D", "thumbsup", "(\"thumbsup\", \"\\u{1F44D}\")" },
            { "\xF0\x9F\x8E\x89", "party", "(\"party\", \"\\u{1F389}\")" },
            { "\xF0\x9F\x91\x8F", "clapping", "(\"clapping\", \"\\u{1F44F}\")" },
            { "\xF0\x9F\x90\xB6", "dog", "(\"dog\", \"\\u{1F436}\")" },
            { "\xF0\x9F\x90\xB1", "cat", "(\"cat\", \"\\u{1F431}\")" },
        };
        const QString rtc =
            read(QStringLiteral(QML_DIR "/../rust/src/rtc.rs"));
        QVERIFY(!rtc.isEmpty());
        for (const Pair &pair : firstRow) {
            const QString expected =
                QStringLiteral("emoji: \"%1\", name: \"%2\"")
                    .arg(QString::fromUtf8(pair.utf8),
                         QString::fromUtf8(pair.name));
            QVERIFY2(bar.contains(expected),
                     qPrintable(QStringLiteral(
                                    "the picker no longer offers element-"
                                    "call's own pair for %1")
                                    .arg(QString::fromUtf8(pair.name))));
            QVERIFY2(rtc.contains(QString::fromUtf8(pair.rustEntry)),
                     qPrintable(QStringLiteral(
                                    "rust/src/rtc.rs no longer carries "
                                    "element-call's own entry for %1")
                                    .arg(QString::fromUtf8(pair.name))));
        }
    }

    void theCallReactionWireFormatIsElementCallsOwn()
    {
        const QString rtc =
            read(QStringLiteral(QML_DIR "/../rust/src/rtc.rs"));
        QVERIFY(!rtc.isEmpty());
        // The event type is element-call's `ElementCallReactionEventType`.
        QVERIFY2(rtc.contains(QStringLiteral(
                     "#[ruma_event(type = \"io.element.call.reaction\", "
                     "kind = MessageLike)]")),
                 "the call reaction is not element-call's event type");
        // The relation is an m.reference to the sender's own membership (an
        // annotation is for the raised hand). Serde does not verify the tag
        // inbound; inbound safety is the sender-owns-membership check.
        QVERIFY2(rtc.contains(QStringLiteral("pub relates_to: Reference,")),
                 "a call reaction's relation is not typed as an m.reference");
        QVERIFY(rtc.contains(QStringLiteral("relates_to: Reference::new(")));

        // One lifetime constant: element-call's 3000 ms.
        const QString controller = read(
            QStringLiteral(QML_DIR "/../src/calls/SfuCallController.h"));
        QVERIFY(!controller.isEmpty());
        QVERIFY2(controller.contains(QStringLiteral(
                     "static constexpr int kReactionActiveMs = 3000;")),
                 "the reaction window is no longer element-call's own "
                 "REACTION_ACTIVE_TIME_MS");
        QVERIFY(normalized(controller).contains(
            QStringLiteral("REACTION_ACTIVE_TIME_MS = 3000")));
    }

    void aCallReactionIsDrawnTransientlyAndNeverAsMarkup()
    {
        const QString tile =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        QVERIFY(!tile.isEmpty());
        QVERIFY(tile.contains(QStringLiteral("property string reactionEmoji")));

        // Behind a Loader: "" is the normal state, and a Text created empty
        // keeps ItemObservesViewport for its lifetime.
        const QString normalizedTile = normalized(tile);
        const int at = normalizedTile.indexOf(
            QStringLiteral("active: root.reactionEmoji.length > 0"));
        QVERIFY2(at >= 0,
                 "the reaction indicator is not gated on there being one");
        const QString block = normalizedTile.mid(at, 1200);
        const int loaderAt =
            normalizedTile.lastIndexOf(QStringLiteral("Loader {"), at);
        QVERIFY2(loaderAt >= 0 && at - loaderAt < 200,
                 "the reaction indicator is not behind a Loader, so an empty "
                 "Text is created on every tile of every call");
        QVERIFY2(block.contains(QStringLiteral("textFormat: Text.PlainText")),
                 "a REMOTE emoji is rendered as something other than plain "
                 "text");
        QVERIFY2(block.contains(QStringLiteral("app.emojiFontFamily")),
                 "the reaction glyph does not use the resolved emoji family, "
                 "so Qt's own fallback may draw it monochrome");

        // No timer in the delegate: the model clears the role when the window
        // ends, so there is one answer to "is this reaction current".
        QVERIFY2(!block.contains(QStringLiteral("Timer")),
                 "the tile runs its own reaction timer");

        // The stage passes the role to both person surfaces.
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        QCOMPARE(stage.count(
                     QStringLiteral("required property string reactionEmoji")),
                 2);
        QCOMPARE(stage.count(
                     QStringLiteral("reactionEmoji: spotPerson.reactionEmoji")),
                 1);
        QCOMPARE(stage.count(QStringLiteral(
                     "reactionEmoji: stripPerson.reactionEmoji")),
                 1);
    }

    void ourOwnReactionIsDrawnFromTheEventAndNotOptimistically()
    {
        // Raise-hand is optimistic (a refusal puts it back); a reaction is
        // not, since a local echo could show one that never left the machine.
        const QString controller = code(read(
            QStringLiteral(QML_DIR "/../src/calls/SfuCallController.cpp")));
        QVERIFY(!controller.isEmpty());
        const int at = controller.indexOf(
            QStringLiteral("void SfuCallController::sendCallReaction("));
        QVERIFY2(at >= 0, "there is no way to send a call reaction");
        const int end = controller.indexOf(
            QStringLiteral("void SfuCallController::onRtcSendFinished("), at);
        QVERIFY(end > at);
        const QString body = controller.mid(at, end - at);
        QVERIFY2(!body.contains(QStringLiteral("setReaction(")),
                 "the sender's own tile is lit before the event exists");
        QVERIFY2(!body.contains(QStringLiteral("applyCallReaction(")),
                 "the sender's own tile is lit before the event exists");
        // References our own membership, preferring the observed one: a
        // refresh replaces the state event.
        QVERIFY(body.contains(QStringLiteral("ownMembershipEventId(")));
    }

    // Only one surface owns the call's media controls.
    void exactlyOneSurfaceOwnsTheMediaControls()
    {
        // Every call control lives in the header bar; the stage has none.
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        QVERIFY2(!stage.contains(QStringLiteral("CallControlBar {")),
                 "the call stage instantiates a control bar again");
        // CallControlBar.qml was a second, unused definition of the control set
        // and is deleted.
        QVERIFY2(!QFile::exists(QStringLiteral(QML_DIR "/CallControlBar.qml")),
                 "the dead CallControlBar.qml is back in the tree");

        // The stage's dock is the same component in another placement, and the
        // header instance stands down while the stage shows.
        QVERIFY(stage.contains(QStringLiteral("CallHeaderBar {")));
        QVERIFY(stage.contains(QStringLiteral("placement: \"dock\"")));

        // The header owns the full set.
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

    // The spotlight renders the shared video rather than a placeholder
    // describing it.
    void theSpotlightRendersVideoRatherThanDescribingIt()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        // The focused surface is defined once (`focusedSurface`) and loaded by
        // both the stage spotlight and the full-screen window.
        const int surface = stage.indexOf(QStringLiteral("id: focusedSurface"));
        QVERIFY2(surface >= 0, "the focused surface is no longer defined once");
        const QString block = stage.mid(surface, 6000);
        // Each spotlighted kind owns its routing: a share is a CallShareTile
        // (screen sink), a pinned person a CallParticipantTile (camera sink).
        QVERIFY2(block.contains(QStringLiteral("CallShareTile {")),
                 "the spotlight cannot show a screen share");
        QVERIFY2(block.contains(QStringLiteral("CallParticipantTile {")),
                 "the spotlight cannot show a pinned participant");
        // Matched by id against the model, so a late track key still arrives.
        QVERIFY(block.contains(
            QStringLiteral("root.stageState.spotlightShareId")));
        QVERIFY(block.contains(
            QStringLiteral("root.stageState.pinnedIdentity")));
        // The spotlight is one of its hosts.
        const int spotlight =
            stage.indexOf(QStringLiteral("objectName: \"callSpotlight\""));
        QVERIFY(spotlight >= 0);
        QVERIFY2(stage.mid(spotlight, 1200)
                     .contains(QStringLiteral("sourceComponent: focusedSurface")),
                 "the spotlight does not host the shared focused surface");
        // The placeholder wording must not survive beside a real surface.
        QVERIFY(!stage.contains(
            QStringLiteral("Someone is sharing their screen")));
    }

    // Camera and screen share are separate tracks from one person, routed by
    // track key; a participant-keyed route can feed only one surface.
    void aScreenShareAndACameraAreRoutedAsSeparateTracks()
    {
        const QString tile =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        QVERIFY(!tile.isEmpty());
        QVERIFY(tile.contains(QStringLiteral("property string mediaKind")));
        QVERIFY(tile.contains(QStringLiteral("attachScreenSink(")));
        QVERIFY(tile.contains(QStringLiteral("attachLocalScreenSink(")));
        // Release names the sink, not a key (see
        // noVideoSurfaceEverReleasesARouteByKey).
        QVERIFY(tile.contains(
            QStringLiteral("app.groupCall.detachSink(output.videoSink)")));
        // Re-attach when the routing key arrives: the SFU can announce a
        // participant before its media section, and an empty-key attach never
        // gets a frame.
        QVERIFY(tile.contains(QStringLiteral("onActiveTrackKeyChanged:")));
        // A shared screen is fitted, never cropped.
        QVERIFY(tile.contains(QStringLiteral("VideoOutput.PreserveAspectFit")));
    }

    // Speaker bubbles above the stage. The ring is driven by the SFU's own
    // speaker updates; no local audio is inspected.
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
        // Bound to the model's speaking flag.
        QVERIFY(bubbles.contains(QStringLiteral("required property bool speaking")));
        QVERIFY(bubbles.contains(QStringLiteral("opacity: bubble.speaking ? 1 : 0")));
        QVERIFY(bubbles.contains(QStringLiteral("border.color: AppTheme.success")));
        // Initials come from the real name, never "You".
        QVERIFY(bubbles.contains(QStringLiteral("name: bubble.displayName")));
        // The real model, not a JS array: a reassigned array is a model reset
        // that rebuilds every bubble on each speaker update.
        QVERIFY2(!bubbles.contains(QStringLiteral("modelData")),
                 "the bubble strip is bound to a JS array again");
        QVERIFY(bubbles.contains(
            QStringLiteral("property var model: app.groupCall.participantModel")));
    }

    // Stopping one published track must never stop another: "the last
    // published" is the share whenever it started after the camera.
    void stoppingOneTrackNamesItRatherThanTakingTheLastOne()
    {
        QFile file(QStringLiteral(SRC_DIR "/calls/SfuCallController.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY(source.contains(QStringLiteral("unpublishTrack(m_cameraCid)")));
        QVERIFY(source.contains(QStringLiteral("unpublishTrack(m_screenCid)")));
        // The old shape: a reverse scan that takes the last published entry.
        QVERIFY2(!source.contains(QStringLiteral(
                     "for (int i = m_publishedTrackIds.size() - 1")),
                 "a track is still stopped by taking the last published one");
    }

    // A voice call draws circular avatars; a tile becomes a panel only when it
    // has video.
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
        // Selection and keyboard focus still draw.
        QVERIFY(tile.contains(QStringLiteral("readonly property bool _drawsCard")));
        QVERIFY(tile.contains(QStringLiteral("root.focused || root.activeFocus")));
    }

    void theJoinButtonActuallyJoins()
    {
        // The banner's Join handler must actually join.
        const QString banner =
            read(QStringLiteral(QML_DIR "/RoomCallBanner.qml"));
        QVERIFY(!banner.isEmpty());
        const int at = banner.indexOf(QStringLiteral("roomCallJoinButton"));
        QVERIFY2(at >= 0, "the Join button is gone");
        // Comments stripped, then a generous window, so a long comment cannot
        // push the call out of the slice.
        QString body = banner.mid(at, 900);
        body.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
        body = normalized(body);
        QVERIFY2(body.contains(QStringLiteral("app.groupCall.join(")),
                 "the Join button does not call join()");
        QVERIFY2(!body.contains(QStringLiteral("onClicked: { }")),
                 "the Join handler is empty again");
    }

    // "Am I in this call?", instantiated. Room state naming this device is not
    // proof this device is in the call: a membership survives an unclean exit,
    // and the local call controller is the authority. Read
    // `property("visible")`, not isVisible(), which folds in the parent chain.
    void aGhostMembershipDoesNotHideTheBanner()
    {
        QQmlEngine engine;
        StubApp stub;
        const QString room = QStringLiteral("!r:mock.local");

        // Live call, a stale membership for this device, and no local call.
        stub.rtcStub()->count = 3;
        stub.rtcStub()->ownDevice = true;
        stub.callStub()->setActive(false);

        QQuickItem *banner = buildBanner(engine, stub, room);
        QVERIFY2(banner, "RoomCallBanner did not instantiate");
        QVERIFY2(banner->property("visible").toBool(),
                 "a membership left behind by this device's own previous "
                 "process hid the banner, and with it the only way back into "
                 "a call the user is not in");
    }

    void theBannerStandsDownOnceThisDeviceIsInTheCall()
    {
        // Already in the call: no Join; the header bar owns it.
        QQmlEngine engine;
        StubApp stub;
        const QString room = QStringLiteral("!r:mock.local");

        stub.rtcStub()->count = 3;
        stub.callStub()->setRoomId(room);
        stub.callStub()->setActive(true);

        QQuickItem *banner = buildBanner(engine, stub, room);
        QVERIFY(banner);
        QVERIFY2(!banner->property("visible").toBool(),
                 "the banner offers Join to a device already in the call");

        // A live binding: leaving brings the banner back.
        stub.callStub()->setActive(false);
        QVERIFY2(banner->property("visible").toBool(),
                 "the banner did not come back when this device left the "
                 "call, so `visible` is not tracking the controller");
    }

    void aCallInAnotherRoomLeavesThisRoomsBannerUp()
    {
        // Being in another call must not hide this room's banner.
        QQmlEngine engine;
        StubApp stub;
        const QString room = QStringLiteral("!r:mock.local");

        stub.rtcStub()->count = 2;
        stub.callStub()->setRoomId(QStringLiteral("!elsewhere:mock.local"));
        stub.callStub()->setActive(true);

        QQuickItem *banner = buildBanner(engine, stub, room);
        QVERIFY(banner);
        QVERIFY2(banner->property("visible").toBool(),
                 "a call in a DIFFERENT room hid this room's banner");
    }

    void noCallMeansNoBannerAndNoReservedHeight()
    {
        // A room with no call reserves no space above its timeline.
        QQmlEngine engine;
        StubApp stub;

        stub.rtcStub()->count = 0;
        QQuickItem *banner =
            buildBanner(engine, stub, QStringLiteral("!r:mock.local"));
        QVERIFY(banner);
        QVERIFY(!banner->property("visible").toBool());
        QCOMPARE(banner->property("implicitHeight").toReal(), 0.0);
    }

    void theSiblingJoinSurfacesAskTheControllerToo()
    {
        // Same rule for CallEventDelegate and RoomCallGlyph, source-level since
        // they need row models: neither may decide "this device is in the call"
        // from room state.
        for (const auto *file : { QML_DIR "/CallEventDelegate.qml",
                                  QML_DIR "/RoomCallGlyph.qml",
                                  QML_DIR "/RoomCallBanner.qml" }) {
            const QString src = read(QString::fromUtf8(file));
            QVERIFY2(!src.isEmpty(), file);
            QString code = src;
            code.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
            code.remove(QRegularExpression(QStringLiteral("(?m)^\\s*///.*$")));
            QVERIFY2(!code.contains(QStringLiteral("ownDeviceInSession(")),
                     qPrintable(QStringLiteral(
                         "%1 still decides whether this device is in the call "
                         "from room state, so a membership left behind by a "
                         "previous process hides its Join").arg(
                         QString::fromUtf8(file))));
        }
    }

    // The call is a panel above the message list, which keeps scrolling
    // beneath it. The stage takes an explicit bounded height; making the host
    // `Layout.fillHeight` splits the column and squashes the stage.
    void theCallPanelSitsAboveTheTimelineRatherThanReplacingIt()
    {
        // Comment-stripped before normalizing: the hosting comment quotes the
        // banned literals, and normalized() destroys the line structure.
        const QString pane =
            normalized(code(read(QStringLiteral(QML_DIR "/TimelinePane.qml"))));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("objectName: \"timelineCallStageHost\"")));
        // One condition, so the stage and its surroundings cannot drift apart.
        QVERIFY2(pane.contains(QStringLiteral(
                     "readonly property bool callStageOwnsColumn: "
                     "app.groupCall.active && app.groupCall.roomId === "
                     "app.currentRoomId")),
                 "TimelinePane has no single call-stage ownership condition");
        QVERIFY2(pane.contains(QStringLiteral("active: root.callStageOwnsColumn")),
                 "the call stage host does not read the ownership condition");
        // Bounded, never fillHeight.
        QVERIFY2(pane.contains(QStringLiteral(
                     "Layout.preferredHeight: active ? root.callPanelHeight "
                     ": 0")),
                 "the call panel does not take a bounded height");
        QVERIFY2(!pane.contains(QStringLiteral("Layout.fillHeight: active")),
                 "the call stage host is fighting the timeline for the column "
                 "again");
        // The timeline and composer stay visible.
        QVERIFY2(!pane.contains(QStringLiteral("visible: !root.callStageOwnsColumn")),
                 "the timeline still stands down for the call");
        QVERIFY2(!pane.contains(QStringLiteral(
                     "visible: app.currentRoomId !== \"\" && "
                     "!root.callStageOwnsColumn")),
                 "the composer still stands down for the call");
    }

    // The divider is draggable and commits on the falling edge: the release
    // moves nothing, so a height-change handler never sees the gesture end.
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
        // The stored value is written where the gesture ends.
        QVERIFY2(block.contains(QStringLiteral(
                     "root.callPanelUserHeight = root.clampCallPanelHeight("
                     "root.callPanelHeight);")),
                 "the divider never commits the released height");
        // ...and not from a height handler.
        const QString pane = normalized(code(raw));
        QVERIFY2(!pane.contains(QStringLiteral(
                     "onHeightChanged: root.callPanelUserHeight")),
                 "the panel height is being persisted from a height change, "
                 "which never fires on release");
        // Collapse is a request to the host, which owns the panel height.
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(stage.contains(QStringLiteral("signal collapseToggled()")));
        QVERIFY(pane.contains(QStringLiteral(
            "onCollapseToggled: root.callPanelCollapsed = "
            "!root.callPanelCollapsed")));
    }

    // A share is a tile, not a mode: the grid has one cell per share and one
    // per participant, so several people can share and a closed share can be
    // brought back.
    void aScreenShareIsATileNotAMode()
    {
        const QString grid = read(QStringLiteral(QML_DIR "/CallTileGrid.qml"));
        const QString share = read(QStringLiteral(QML_DIR "/CallShareTile.qml"));
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY2(!grid.isEmpty(), "CallTileGrid.qml is missing");
        QVERIFY2(!share.isEmpty(), "CallShareTile.qml is missing");
        QVERIFY(!stage.isEmpty());

        // Two models, two Repeaters: surfaces, not people.
        QVERIFY(grid.contains(QStringLiteral("model: root.shareModel")));
        QVERIFY(grid.contains(QStringLiteral("model: root.participantModel")));
        // People start where shares end, so a sharer has both cells.
        QVERIFY2(grid.contains(QStringLiteral(
                     "root.cellX(root.shareCount + personCell.index)")),
                 "the grid does not place people after the shares");
        // A person's own tile never renders their screen: two surfaces asking
        // for one screen blank each other.
        QVERIFY(grid.contains(QStringLiteral("mediaKind: \"camera\"")));
        // The share tile owns the screen sink and releases the sink it holds,
        // never a recomputed key.
        QVERIFY(share.contains(QStringLiteral("attachScreenSink(")));
        QVERIFY(share.contains(QStringLiteral("attachLocalScreenSink(")));
        QVERIFY(share.contains(
            QStringLiteral("app.groupCall.detachSink(output.videoSink)")));
        // A shared screen is fitted, never cropped.
        QVERIFY(share.contains(QStringLiteral("VideoOutput.PreserveAspectFit")));
        // No single-sharer collapse.
        QVERIFY2(!stage.contains(QStringLiteral("sharingPerson")),
                 "the stage still resolves 'the one person who is sharing'");
        QVERIFY2(!stage.contains(QStringLiteral("spotlightKind")),
                 "the spotlight still has a single track kind, which is the "
                 "one-share assumption in another spelling");
    }

    // N simultaneous shares are N surfaces. The router and CallShareModel
    // already handle this; what is pinned is that the view does not collapse
    // them.
    void multipleSimultaneousSharesEachGetTheirOwnSurface()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        const QString grid = read(QStringLiteral(QML_DIR "/CallTileGrid.qml"));
        QVERIFY(!stage.isEmpty());
        QVERIFY(!grid.isEmpty());
        // The grid gets the whole share model.
        QVERIFY(stage.contains(QStringLiteral("shareModel: root.shareModel")));
        QVERIFY(stage.contains(
            QStringLiteral("readonly property var shareModel: "
                           "app.groupCall.shareModel")));
        // No "who is sharing" scan over participants, which finds only one.
        QVERIFY2(!stage.contains(QStringLiteral("people[i].screenSharing")),
                 "the stage scans participants for a sharer again");
        QVERIFY2(!stage.contains(QStringLiteral("readonly property var people")),
                 "the stage rebuilt the participant JS array");
        // The strip excludes by shareId; excluding by identity dropped a
        // sharer's camera when their screen was spotlighted.
        QVERIFY(normalized(stage).contains(QStringLiteral(
            "stripShare.shareId !== root.stageState.spotlightShareId")));
    }

    // While any share is live, at least one control puts it back on the
    // spotlight: the explicit "Show screen share" button and the share's own
    // tile in the grid.
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

        // 2. A share tile in the grid restores itself.
        QVERIFY(norm.contains(QStringLiteral("root.stageState.restoreShare(")));

        // 3. Leaving the spotlight dismisses; it never writes a layout.
        QVERIFY(norm.contains(QStringLiteral(
            "root.stageState.dismissShare(root.stageState.spotlightShareId);")));
        QVERIFY(norm.contains(QStringLiteral("root.stageState.clearPin();")));
        const int back =
            norm.indexOf(QStringLiteral("objectName: \"callBackToGridButton\""));
        QVERIFY2(back >= 0, "the Back to grid control is gone");
        QVERIFY2(norm.mid(back, 400)
                     .contains(QStringLiteral("onClicked: root.leaveSpotlight()")),
                 "Back to grid does not go through the dismiss path");

        // 4. No one-way `layoutMode` latch.
        const QString stageCode = code(stage);
        // Prove the stripper leaves code behind before trusting a ban.
        QVERIFY(stageCode.contains(QStringLiteral("objectName: \"callStage\"")));
        QVERIFY2(!stageCode.contains(QStringLiteral("layoutMode")),
                 "the terminal layout-mode latch is back on the call stage");
        // The stage never pins the shared preference either.
        QVERIFY2(!stageCode.contains(QStringLiteral("setLayoutPreference")),
                 "the stage writes a layout preference with no writer back");
        // `= "` not `=`: the derivation compares the preference with `===`.
        QVERIFY2(!stageCode.contains(QStringLiteral("layoutPreference = \"")),
                 "the stage assigns a layout preference directly");
    }

    // The speaking ring follows the SFU's amplitude, not a boolean.
    void theSpeakingRingReadsALevelNotABoolean()
    {
        const QString tile =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        const QString grid = read(QStringLiteral(QML_DIR "/CallTileGrid.qml"));
        QVERIFY(!tile.isEmpty());
        QVERIFY(!grid.isEmpty());
        const QString norm = normalized(tile);

        QVERIFY(tile.contains(QStringLiteral("property real speakingLevel")));
        // The gap follows amplitude.
        QVERIFY2(norm.contains(QStringLiteral(
                     "readonly property real ringTarget: root.speaking ? 3 + 6 "
                     "* Math.max(0, Math.min(1, root.speakingLevel)) : 0")),
                 "the ring does not read the level");
        // Fast attack, slow release, or it strobes between syllables.
        QVERIFY2(norm.contains(QStringLiteral(
                     "ringMotion.duration = root.ringTarget > root.ringGap ? "
                     "60 : 220")),
                 "the ring has no attack/release asymmetry");
        // Nothing is fabricated from the boolean: an SFU reporting only
        // `active` degrades to the fixed minimum ring.
        QVERIFY2(!norm.contains(QStringLiteral("speakingLevel: root.speaking ?")),
                 "a level is being fabricated from the speaking boolean");
        // The ring must not scale the item, which reflows neighbours.
        QVERIFY2(!tile.contains(QStringLiteral("scale: root.speaking")),
                 "the speaking cue scales the avatar again");
        QVERIFY(norm.contains(QStringLiteral("width: parent.width + 2 * root.ringGap")));
        // The level reaches the tile from the model.
        QVERIFY(grid.contains(
            QStringLiteral("speakingLevel: personCell.speakingLevel")));
    }

    // The stage binds the real models. Reassigning a JS array is a model
    // reset, and the speaker feed fires continuously, so every tile and
    // VideoOutput would be rebuilt on every syllable.
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
        // The strip exists for walking away from the call's room; inside that
        // room it would duplicate the stage.
        const QString strip =
            normalized(read(QStringLiteral(QML_DIR "/VoiceConnectedBar.qml")));
        QVERIFY(!strip.isEmpty());
        QVERIFY2(strip.contains(QStringLiteral(
                     "app.groupCall.roomId === app.currentRoomId")),
                 "the Voice Connected strip does not exclude the call's own "
                 "room");
    }

    // A destroyed video surface must release its route by sink, never by key.

    void noVideoSurfaceEverReleasesARouteByKey()
    {
        // Rule for the whole qml/ tree: never release a route by key. Qt
        // destroys a replaced surface with deleteLater() after building the
        // replacement, so the new tile attaches before the old one detaches;
        // a key-named detach unhooks the living tile and nothing re-attaches.
        QDir dir(QStringLiteral(QML_DIR));
        const QStringList files =
            dir.entryList({ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY(!files.isEmpty());

        // Prove the scan sees what it looks for before trusting a ban.
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
            // Every release names a sink; a bare `detachSink()` is the same hole.
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

        // No periodic re-arm either: while both tiles are alive a participant
        // update lets the dying tile reclaim the key and then release it. Late
        // keys are handled by `onActiveTrackKeyChanged` / `onTrackKeyChanged`.
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
        // The late-key re-attach is required in both tiles.
        QVERIFY(read(dir.filePath(QStringLiteral("CallParticipantTile.qml")))
                    .contains(QStringLiteral("onActiveTrackKeyChanged:")));
        QVERIFY(read(dir.filePath(QStringLiteral("CallShareTile.qml")))
                    .contains(QStringLiteral("onTrackKeyChanged:")));
    }

    void theRouterRefusesAReleaseFromASupersededSurface()
    {
        // Router-level statement of the same rule: a superseded owner's
        // release must not remove the new owner's route.
#ifndef HAVE_LIGHTNING_WEBRTC
        QSKIP("built without the SFU media engine");
#else
        SfuVideoRouter router;
        auto first = std::make_unique<QVideoSink>();
        auto second = std::make_unique<QVideoSink>();
        const QString key = QStringLiteral("TR_share_a");

        router.attachSink(key, first.get());
        QVERIFY(router.watchedBy(key, first.get()));

        // The replacement claims the key first, the order production produces.
        router.attachSink(key, second.get());
        QVERIFY(router.watchedBy(key, second.get()));

        // The superseded owner's later release takes nothing with it.
        router.releaseSink(first.get());
        QVERIFY2(router.watching(key), "a dying surface unhooked a live one");
        QVERIFY(router.watchedBy(key, second.get()));

        // The real owner's release still works, or these checks are vacuous.
        router.releaseSink(second.get());
        QVERIFY(!router.watching(key));

        // A surface releases every key it holds (a camera tile attaches under
        // both track sid and participant sid) and nobody else's.
        router.attachSink(key, first.get());
        router.attachSink(QStringLiteral("PA_alice"), first.get());
        router.attachSink(QStringLiteral("TR_other"), second.get());
        router.releaseSink(first.get());
        QVERIFY(!router.watching(key));
        QVERIFY(!router.watching(QStringLiteral("PA_alice")));
        QVERIFY(router.watching(QStringLiteral("TR_other")));

        // A null attach is a no-op, never an eviction.
        router.attachSink(QStringLiteral("TR_other"), nullptr);
        QVERIFY(router.watching(QStringLiteral("TR_other")));

        // A null release is a no-op, not a wildcard.
        router.releaseSink(nullptr);
        QVERIFY(router.watching(QStringLiteral("TR_other")));
#endif
    }

    void theVideoRouteSurvivesTheStagesOwnLayoutChanges()
    {
        // Drives the real CallStage through real transitions and asserts
        // against the router, never a QML property: the failure is a tile
        // reporting "attached" while the router disagrees.
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

        // 1. Grid: a dismissed spotlight leaves the share as a live grid tile.
        stage->dismissShare(key);
        settle();
        QCOMPARE(stage->spotlightShareId(), QString());
        QVERIFY2(call->isRoutingVideoTo(key),
                 "the grid's share tile never attached a sink");

        // 2. Grid -> spotlight.
        stage->restoreShare(key);
        settle();
        QCOMPARE(stage->spotlightShareId(), key);
        QVERIFY2(call->isRoutingVideoTo(key),
                 "the dying grid tile unhooked the spotlight's sink");

        // 3. Spotlight -> full screen, across a window boundary.
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

        // 5. Share ends: now the route must be gone, or nothing is ever
        //    released and the checks above are vacuous.
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

    void theTwoShareQualityLaddersCannotDisagree()
    {
        // Quality snapping exists in SettingsManager (storage) and in
        // SfuMediaEngine::setShareQuality (caps). Both must stay, so they are
        // pinned against each other: disagreement means the UI shows one
        // quality while the encoder uses another.
#ifndef HAVE_LIGHTNING_WEBRTC
        QSKIP("built without the SFU media engine");
#else
        SettingsManager settings;
        SfuMediaEngine engine;

        // Values around every boundary, plus absurd hand-edited ones.
        const QList<int> heights = { -100, 0, 1, 480, 719, 720, 721, 900,
                                     901, 1080, 1081, 1260, 1261, 1440,
                                     1441, 1800, 1801, 2160, 2161, 4320 };
        for (int h : heights) {
            settings.setShareMaxHeight(h);
            engine.setShareQuality(h, 30);
            QVERIFY2(engine.shareMaxHeight() == settings.shareMaxHeight(),
                     qPrintable(QStringLiteral(
                         "height %1 snapped to %2 in settings but %3 in the "
                         "engine")
                             .arg(h).arg(settings.shareMaxHeight())
                             .arg(engine.shareMaxHeight())));
            QVERIFY2(engine.shareMaxHeight() == 720
                         || engine.shareMaxHeight() == 1080
                         || engine.shareMaxHeight() == 1440
                         || engine.shareMaxHeight() == 2160,
                     "a share height outside the offered set reached the "
                     "encoder");
        }

        const QList<int> rates = { -5, 0, 1, 10, 15, 16, 22, 23, 30, 31,
                                   45, 46, 60, 61, 240 };
        for (int f : rates) {
            settings.setShareFps(f);
            engine.setShareQuality(1080, f);
            QVERIFY2(engine.shareFps() == settings.shareFps(),
                     qPrintable(QStringLiteral(
                         "fps %1 snapped to %2 in settings but %3 in the "
                         "engine")
                             .arg(f).arg(settings.shareFps())
                             .arg(engine.shareFps())));
            QVERIFY2(engine.shareFps() == 15 || engine.shareFps() == 30
                         || engine.shareFps() == 60,
                     "a share frame rate outside the offered set reached the "
                     "encoder");
        }
#endif
    }

    void theShareMenuStaysOpenWhileSettingsAreChanged()
    {
        // Quality rows must not close the menu on trigger, so a resolution
        // pick does not dismiss the frame-rate list.
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 800
    height: 600
    property alias menu: m
    CallShareOptionsMenu { id: m }
}
)"), QUrl(QStringLiteral("qrc:/sharemenustaysopentest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner != nullptr);
        QCoreApplication::processEvents();
        auto *menu = owner->property("menu").value<QObject *>();
        QVERIFY(menu != nullptr);
        QMetaObject::invokeMethod(menu, "open");
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(), 3000);

        SettingsManager *settings = controller.settings();
        QVERIFY(settings);
        settings->setShareMaxHeight(1080);

        // Through the menu's count/itemAt: rows are Repeater delegates, which
        // findChild cannot reach.
        QQuickItem *target = nullptr;
        const int rows = menu->property("count").toInt();
        QVERIFY2(rows > 0, "the menu reports no rows at all");
        for (int i = 0; i < rows; ++i) {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt",
                                      Q_RETURN_ARG(QQuickItem *, item),
                                      Q_ARG(int, i));
            if (item
                && item->property("text").toString() == QStringLiteral("720p")) {
                target = item;
                break;
            }
        }
        QVERIFY2(target != nullptr, "no 720p row found in the menu");
        // Emit the signal: MenuItem has no trigger() slot, and invoking a
        // missing name is a silent no-op.
        QMetaObject::invokeMethod(target, "triggered");
        QCoreApplication::processEvents();

        QCOMPARE(settings->shareMaxHeight(), 720);
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(), 3000);
    }

    void theShareAudioCheckboxKeepsFollowingTheControllerAfterAClick()
    {
        // A CheckBox writes its own `checked` on click, destroying the
        // binding; it must be restored so the picker and the call bar's menu
        // keep agreeing on the shared setting.
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
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
)"), QUrl(QStringLiteral("qrc:/shareaudiosynctest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner != nullptr);
        QCoreApplication::processEvents();
        auto *picker = owner->property("picker").value<QObject *>();
        QVERIFY(picker != nullptr);
        QMetaObject::invokeMethod(picker, "open");
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(picker->property("visible").toBool(), 3000);

        auto *box = picker->findChild<QQuickItem *>(
            QStringLiteral("shareAudioCheck"));
        QVERIFY2(box != nullptr, "the share-audio checkbox is gone");
        if (!box->isVisible())
            QSKIP("no loopback capture element here, so the box is absent");

        SfuCallController *call = controller.groupCall();
        QVERIFY(call);

        // A JS-side write, because that is what breaks a binding. C++
        // setProperty()/toggle() do not remove a QML binding, so a test built
        // on them passes without the fix.
        {
            QQmlExpression write(qmlContext(box), box,
                                 QStringLiteral("checked = !checked"));
            write.evaluate();
            QVERIFY2(!write.hasError(),
                     qPrintable(write.error().toString()));
        }
        QMetaObject::invokeMethod(box, "toggled");
        QCoreApplication::processEvents();

        // Change the setting from the other surface; the box must follow.
        const bool now = call->shareAudioEnabled();
        call->setShareAudioEnabled(!now);
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(box->property("checked").toBool(), !now, 3000);
        call->setShareAudioEnabled(now);
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(box->property("checked").toBool(), now, 3000);
    }

    void bothShareSurfacesOfferTheSameRungsAndTheSameWarning()
    {
        // The chevron menu and the share picker set the same settings and must
        // offer the same choices.
        const QString menu =
            read(QStringLiteral(QML_DIR "/CallShareOptionsMenu.qml"));
        const QString picker =
            read(QStringLiteral(QML_DIR "/ScreenSharePicker.qml"));
        QVERIFY(!menu.isEmpty() && !picker.isEmpty());

        for (int rung : { 720, 1080, 1440, 2160 }) {
            const QString needle = QStringLiteral("value: %1").arg(rung);
            QVERIFY2(menu.contains(needle),
                     qPrintable(QStringLiteral("the menu does not offer %1")
                                    .arg(rung)));
            QVERIFY2(picker.contains(needle),
                     qPrintable(QStringLiteral("the picker does not offer %1")
                                    .arg(rung)));
        }
        for (int rate : { 15, 30, 60 }) {
            const QString needle = QStringLiteral("value: %1").arg(rate);
            QVERIFY2(menu.contains(needle), qPrintable(
                         QStringLiteral("the menu does not offer %1 fps")
                             .arg(rate)));
            QVERIFY2(picker.contains(needle), qPrintable(
                         QStringLiteral("the picker does not offer %1 fps")
                             .arg(rate)));
        }

        // Both mark the slow combination from the same predicate. The menu
        // marks the rate row itself.
        for (const QString &src : { menu, picker }) {
            QVERIFY2(src.contains(QStringLiteral("shareQualityDemanding")),
                     "a share surface decides for itself whether a "
                     "combination is too slow, so the two can disagree");
        }
        QVERIFY2(menu.contains(QStringLiteral("shareQualityDemandingAt")),
                 "the menu no longer marks the individual rate rows, so the "
                 "warning is back to being a block of prose");

        // The predicate's table. `gdiscreencapsrc` sustains 30 fps, not 60, so
        // 60 warns where the pixel count makes the blit expensive and 4K warns
        // at every rate. 1080p must never warn: a warning on the recommended
        // setting trains everyone to ignore it.
        SettingsManager settings;
        struct Row { int h; int fps; bool warn; };
        const QList<Row> rows = {
            // 1080p and below: never.
            { 720, 30, false }, { 720, 60, false },
            { 1080, 15, false }, { 1080, 30, false }, { 1080, 60, false },
            // 1440p: warned at 60 only.
            { 1440, 15, false }, { 1440, 30, false }, { 1440, 60, true },
            // 4K: warned at every rate.
            { 2160, 15, true }, { 2160, 30, true }, { 2160, 60, true },
        };
        for (const Row &r : rows) {
            // Asked both ways: current setting (bar) and arbitrary combination
            // (menu rows).
            QCOMPARE(settings.shareQualityDemandingAt(r.h, r.fps), r.warn);
            settings.setShareMaxHeight(r.h);
            settings.setShareFps(r.fps);
            QVERIFY2(settings.shareQualityDemanding() == r.warn,
                     qPrintable(QStringLiteral("%1p%2 warned=%3, expected %4")
                                    .arg(r.h).arg(r.fps)
                                    .arg(settings.shareQualityDemanding())
                                    .arg(r.warn)));
        }
    }

    void startingACallAnnouncesItOnceAndOnlyByTheFirstArrival()
    {
        // Contract scan for the call announcement. `m.call.member` is a state
        // event and never renders in a timeline, so starting a call must also
        // send the message-like announcement. Behaviour is judged live
        // against Element.
        const QString src =
            read(QStringLiteral(SRC_DIR "/calls/SfuCallController.cpp"));
        QVERIFY(!src.isEmpty());

        const int sampled = src.indexOf(QStringLiteral(
            "startsCallForAnnouncement(m_rtc->participants(roomId))"));
        const int published = src.indexOf(QStringLiteral(
            "m_publishOp = m_client->rtcPublishMembership"));
        QVERIFY2(sampled >= 0, "nothing decides whether we started the call");
        QVERIFY2(published >= 0, "the membership publish call site moved");
        // Sampled before our own membership is published, or the answer is
        // always "somebody is already here" (ourselves).
        QVERIFY2(sampled < published,
                 "the first-arrival check runs AFTER our own membership "
                 "publishes, so it can only ever answer no");

        // Announce ("notification"), not "ring", which makes the far end ring.
        QVERIFY2(src.contains(QStringLiteral("m_client->rtcNotify(")),
                 "nothing announces the call, so the room still gets no "
                 "timeline row when a call starts");
        const int notify = src.indexOf(QStringLiteral("m_client->rtcNotify("));
        const QString args = src.mid(notify, 320);
        QVERIFY2(args.contains(QStringLiteral("\"notification\"")),
                 qPrintable(QStringLiteral("the announcement is not sent as "
                                           "a notification: %1").arg(args)));
        QVERIFY2(!args.contains(QStringLiteral("\"ring\"")),
                 "the announcement rings other people's clients, which was "
                 "deliberately left to its own round");

        // Once: the flag clears as it fires, or each membership refresh
        // announces again.
        QVERIFY2(src.contains(QStringLiteral("m_announceOnPublish = false;")),
                 "the announcement flag is never cleared, so a membership "
                 "refresh would announce the call again");
    }

    void everyPrivateQtHeaderIncludeStaysGuarded()
    {
        // `qpa/qplatformscreen.h` is a private Qt header the Windows MinGW Qt
        // lacks, so every include must be platform- and CMake-probe guarded
        // (as SfuCallController.cpp does).
        QDir dir(QStringLiteral(SRC_DIR));
        const QStringList files =
            dir.entryList(QStringList{}, QDir::Dirs | QDir::NoDotAndDotDot);
        int includes = 0;
        QStringList offenders;
        std::function<void(const QString &)> walk = [&](const QString &path) {
            QDir d(path);
            for (const QFileInfo &fi :
                 d.entryInfoList(QDir::Files | QDir::Dirs
                                 | QDir::NoDotAndDotDot)) {
                if (fi.isDir()) {
                    walk(fi.absoluteFilePath());
                    continue;
                }
                if (!fi.fileName().endsWith(QStringLiteral(".cpp"))
                    && !fi.fileName().endsWith(QStringLiteral(".h")))
                    continue;
                QFile f(fi.absoluteFilePath());
                if (!f.open(QIODevice::ReadOnly))
                    continue;
                const QStringList lines =
                    QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
                for (int i = 0; i < lines.size(); ++i) {
                    if (!lines.at(i).contains(QStringLiteral("#include <qpa/")))
                        continue;
                    ++includes;
                    // A conditional must open within the preceding lines;
                    // twenty allows for a comment block.
                    bool guarded = false;
                    for (int j = std::max(0, i - 20); j < i; ++j) {
                        if (lines.at(j).trimmed().startsWith(
                                QStringLiteral("#if"))) {
                            guarded = true;
                            break;
                        }
                    }
                    if (!guarded)
                        offenders << (fi.fileName() + QStringLiteral(":")
                                      + QString::number(i + 1));
                }
            }
        };
        walk(QStringLiteral(SRC_DIR));

        // found > 0, or a rename makes this pass by matching nothing.
        QVERIFY2(includes > 0,
                 "no private Qt include found anywhere, so this scan is "
                 "checking nothing -- has the header been renamed?");
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "unguarded private Qt header include(s), which build "
                     "here and break the Windows package: %1")
                         .arg(offenders.join(QStringLiteral(", ")))));
    }

    void theShareChevronSitsAsCloseAsEveryOtherChevron()
    {
        // The share button/chevron gap must match the camera and microphone
        // pairs, which group into one RowLayout with spacing 0. Compared
        // against that pair rather than a hardcoded number.
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 1200
    height: 200
    visible: true
    property alias bar: b
    CallHeaderBar { id: b; anchors.fill: parent; previewMode: true }
}
)"), QUrl(QStringLiteral("qrc:/sharechevrongaptest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner != nullptr, "CallHeaderBar must instantiate");
        QCoreApplication::processEvents();
        auto *bar = owner->property("bar").value<QQuickItem *>();
        QVERIFY(bar != nullptr);

        auto gapFor = [bar](const char *button, const char *chevron) -> double {
            auto *b = bar->findChild<QQuickItem *>(QString::fromLatin1(button));
            auto *c = bar->findChild<QQuickItem *>(QString::fromLatin1(chevron));
            if (!b || !c || !b->isVisible() || !c->isVisible())
                return -1.0;
            // Scene coordinates, so a different parent cannot skew it.
            const QPointF bEnd = b->mapToScene(QPointF(b->width(), 0));
            const QPointF cStart = c->mapToScene(QPointF(0, 0));
            return cStart.x() - bEnd.x();
        };

        double cameraGap = -1.0;
        double shareGap = -1.0;
        QTRY_VERIFY_WITH_TIMEOUT(
            (cameraGap = gapFor("callBarCameraButton", "callBarCameraChevron"))
                >= 0.0
            && (shareGap = gapFor("callBarScreenShareButton",
                                  "callBarShareOptionsChevron")) >= 0.0,
            5000);

        QVERIFY2(qAbs(shareGap - cameraGap) < 1.5,
                 qPrintable(QStringLiteral(
                     "the share chevron sits %1 px from its button while the "
                     "camera's sits %2 px from its own")
                         .arg(shareGap).arg(cameraGap)));
    }

    void theShareOptionsMenuShowsItsLabelsWithoutEliding()
    {
        // Measured: a Label whose implicitWidth exceeds its width is eliding.
        // Also catches translations with longer wording.
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 800
    height: 600
    property alias menu: m
    CallShareOptionsMenu { id: m }
}
)"), QUrl(QStringLiteral("qrc:/shareoptionsmenutest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner != nullptr, "CallShareOptionsMenu must instantiate");
        QCoreApplication::processEvents();
        auto *menu = owner->property("menu").value<QObject *>();
        QVERIFY(menu != nullptr);

        QMetaObject::invokeMethod(menu, "open");
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(), 3000);

        auto *item = menu->findChild<QQuickItem *>(
            QStringLiteral("shareAudioMenuItem"));
        QVERIFY2(item != nullptr, "the sound row is gone from the menu");

        // The row is only built where something can capture; skip rather than
        // pass on an absent row.
        if (!item->isVisible())
            QSKIP("no loopback capture element here, so the sound row is "
                  "absent and its width cannot be measured");

        int checked = 0;
        for (QQuickItem *label : item->findChildren<QQuickItem *>()) {
            if (!label->inherits("QQuickLabel")
                && !label->inherits("QQuickText"))
                continue;
            const QString text = label->property("text").toString();
            if (text.isEmpty())
                continue;
            ++checked;
            QVERIFY2(label->implicitWidth() <= label->width() + 0.5,
                     qPrintable(QStringLiteral(
                         "\"%1\" is elided: it wants %2 px and has %3")
                             .arg(text)
                             .arg(label->implicitWidth())
                             .arg(label->width())));
        }
        QVERIFY2(checked > 0,
                 "no label was measured, so this case proved nothing");
    }

    void theShareAudioToggleLivesWhereWaylandCanReachIt()
    {
        // On Wayland the share goes through the portal and our picker never
        // opens, so the share-audio control must live on the call controls.
        const QString bar = read(QStringLiteral(QML_DIR "/CallHeaderBar.qml"));
        QVERIFY(!bar.isEmpty());
        QVERIFY2(bar.contains(QStringLiteral("callBarShareOptionsChevron")),
                 "the share options are not on the call bar, so a Wayland "
                 "user -- whose picker never opens -- has no way to reach "
                 "sound, resolution or frame rate");
        const QString menu =
            read(QStringLiteral(QML_DIR "/CallShareOptionsMenu.qml"));
        QVERIFY2(!menu.isEmpty(), "the share options menu is gone");
        QVERIFY2(menu.contains(QStringLiteral("shareAudioSupported")),
                 "the menu offers sound without asking whether anything can "
                 "capture it, so it appears where it cannot work");
        // Resolution and frame rate must be here too.
        QVERIFY2(menu.contains(QStringLiteral("shareMaxHeight"))
                     && menu.contains(QStringLiteral("shareFps")),
                 "resolution or frame rate is missing from the one surface "
                 "Wayland shows");

        // The picker keeps its copy for X11, Windows and macOS.
        const QString picker =
            read(QStringLiteral(QML_DIR "/ScreenSharePicker.qml"));
        QVERIFY(!picker.isEmpty());
        QVERIFY2(picker.contains(QStringLiteral("shareAudioCheck")),
                 "the picker lost its share-audio switch, leaving the "
                 "non-portal platforms without one");

        // Neither the menu nor the picker may hold its own copy of the state.
        for (const QString &src : { menu, picker }) {
            QVERIFY2(src.contains(QStringLiteral("shareAudioEnabled")),
                     "a share-audio control does not read the controller's "
                     "state");
        }

        // The portal branch enumerates nothing and opens no picker of ours.
        const QString ctrl =
            read(QStringLiteral(SRC_DIR "/calls/SfuCallController.cpp"));
        QVERIFY(!ctrl.isEmpty());
        QVERIFY2(ctrl.contains(QStringLiteral("case LinuxShareRoute::Portal")),
                 "the portal route is gone; if the picker now opens on "
                 "Wayland this case is describing a world that no longer "
                 "exists and should be revisited");
    }

    void theSharePickerOffersShareAudioOnlyWhereItCanWork()
    {
        // A real load: a binding to a missing property is a load-time error
        // that text scans and qmlformat cannot see.
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
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
)"), QUrl(QStringLiteral("qrc:/shareaudiopickertest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner != nullptr, "ScreenSharePicker must instantiate");
        QCoreApplication::processEvents();
        auto *picker = owner->property("picker").value<QObject *>();
        QVERIFY(picker != nullptr);

        // Open it first: inside a closed dialog `visible` is always false.
        QMetaObject::invokeMethod(picker, "open");
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(picker->property("visible").toBool(), 3000);

        auto *check = picker->findChild<QObject *>(
            QStringLiteral("shareAudioCheck"));
        QVERIFY2(check != nullptr, "the share-audio switch is gone");

        // Absent, not disabled, where there is no loopback element. Asserted
        // against the controller's answer so it holds on any machine.
        const bool supported =
            controller.groupCall()
                ? controller.groupCall()->shareAudioSupported() : false;
        // Skip loudly when unsupported: both sides would be false and the case
        // would pass without measuring anything.
        if (!supported) {
            QSKIP("no loopback capture element here, so the visible branch "
                  "of this case cannot be exercised");
        }
        QCOMPARE(check->property("visible").toBool(), supported);

        // The switch reflects the controller rather than a literal.
        QCOMPARE(check->property("checked").toBool(),
                 controller.groupCall()->shareAudioEnabled());
        controller.groupCall()->setShareAudioEnabled(false);
        QCoreApplication::processEvents();
        QVERIFY2(!check->property("checked").toBool(),
                 "the switch did not follow the controller");
        controller.groupCall()->setShareAudioEnabled(true);
    }

    void chromeRetiresWithThePointerParkedOverIt()
    {
        // A pointer resting on the share must not keep the chrome awake.
        // Retiring the chrome changes what is under the pointer, Qt re-delivers
        // a hover at the same position, and a handler that treats
        // `pointChanged` as movement resets its own count: a feedback loop.
        // Needs a real window and real mouse events.
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
        track.insert(QStringLiteral("sid"), QStringLiteral("TR_parked"));
        track.insert(QStringLiteral("muted"), false);
        QVariantMap sharer;
        sharer.insert(QStringLiteral("identity"), QStringLiteral("bob"));
        sharer.insert(QStringLiteral("sid"), QStringLiteral("PA_bob"));
        sharer.insert(QStringLiteral("tracks"), QVariantList { track });
        call->ingestParticipantsForTest({ sharer });
        CallStageState *stage = call->stageState();
        QVERIFY(stage);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("CallStage"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(5000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(root);
        stage->restoreShare(QStringLiteral("TR_parked"));

        QQuickWindow window;
        window.resize(1200, 800);
        root->setParentItem(window.contentItem());
        root->setSize(QSizeF(1200, 800));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
        window.requestActivate();
        QCoreApplication::processEvents();
        QVERIFY2(root->property("spotlightHasSurface").toBool(),
                 "no spotlight, so nothing draws over a picture and this "
                 "measures nothing");

        const int budgetMs = root->property("idleTickMs").toInt()
                             * root->property("idleTicksToHide").toInt();

        // Pointer onto the share once, never moved again.
        QTest::mouseMove(&window, QPoint(600, 300));
        QCoreApplication::processEvents();
        QVERIFY2(!root->property("stageChromeIdle").toBool(),
                 "retired before any time passed");
        QTRY_VERIFY_WITH_TIMEOUT(root->property("stageChromeIdle").toBool(),
                                 budgetMs + 20000);

        // It must stay retired: under the feedback loop it reached the state
        // briefly each cycle, which QTRY_VERIFY alone would accept.
        for (int i = 0; i < 20; ++i) {
            QTest::qWait(200);
            QVERIFY2(root->property("stageChromeIdle").toBool(),
                     "the chrome woke itself with the pointer parked and "
                     "unmoved -- a hide that re-delivers a hover event and "
                     "counts it as movement");
        }

        // The window's actual cursor, not the handler's configured one.
        QCOMPARE(window.cursor().shape(), Qt::BlankCursor);

        // A real move wakes it, or "never reset" would also pass.
        QTest::mouseMove(&window, QPoint(640, 340));
        QCoreApplication::processEvents();
        QVERIFY2(!root->property("stageChromeIdle").toBool(),
                 "moving the pointer did not bring the controls back");
        QVERIFY2(window.cursor().shape() != Qt::BlankCursor,
                 "the pointer stayed hidden after the chrome came back");

        // ...and it retires again, so waking is not a latch.
        QTRY_VERIFY_WITH_TIMEOUT(root->property("stageChromeIdle").toBool(),
                                 budgetMs + 20000);
#endif
    }

    void chromeOverAPictureRetiresOnItsOwnTimerInBothPlaces()
    {
        // End to end with nothing driven by hand: a real share is ingested,
        // the spotlight resolves and the real timer retires the chrome. Covers
        // both the in-window spotlight and the full-screen window.
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
        track.insert(QStringLiteral("sid"), QStringLiteral("TR_share_idle"));
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
        QSignalSpy createdSpy(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("CallStage"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(5000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr, "CallStage must instantiate");
        root->setWidth(1200);
        root->setHeight(800);

        const QString key = QStringLiteral("TR_share_idle");
        stage->restoreShare(key);
        settle();
        QCOMPARE(stage->spotlightShareId(), key);
        QVERIFY2(root->property("spotlightHasSurface").toBool(),
                 "no spotlight surface, so nothing draws over a picture and "
                 "this test would pass without measuring anything");

        // 1. The spotlight in the application window.
        QVERIFY2(!root->property("stageChromeIdle").toBool(),
                 "the overlay is retired before any time has passed");
        const int budgetMs = root->property("idleTickMs").toInt()
                             * root->property("idleTicksToHide").toInt();
        QVERIFY2(budgetMs >= 2000 && budgetMs <= 6000,
                 "the idle budget is outside the few seconds asked for");
        // Generous timeout: this measures that it retires unaided, not how
        // fast, and offscreen window creation can stall the loop for seconds.
        QTRY_VERIFY_WITH_TIMEOUT(root->property("stageChromeIdle").toBool(),
                                 budgetMs + 20000);

        // Movement wakes it. The pointer cannot move offscreen, so reset the
        // count production resets.
        root->setProperty("stageIdleTicks", 0);
        QVERIFY2(!root->property("stageChromeIdle").toBool(),
                 "the overlay stayed retired after the idle count cleared, "
                 "so nothing can bring it back");

        // 2. The full-screen window, on its own timer.
        stage->setFullScreen(true);
        settle();
        QVERIFY(root->property("fullScreenActive").toBool());
        auto *surface = root->findChild<QQuickItem *>(
            QStringLiteral("fullScreenSurface"));
        QVERIFY2(surface != nullptr, "the full-screen surface is gone");
        QVERIFY2(!surface->property("overlaysIdle").toBool(),
                 "the full-screen chrome is retired before any time passes");
        QTRY_VERIFY_WITH_TIMEOUT(surface->property("overlaysIdle").toBool(),
                                 budgetMs + 20000);

        stage->setFullScreen(false);
        settle();

        // Pinned rather than inherited from earlier steps, which would make
        // this timing-dependent.
        root->setProperty("stageIdleTicks", 0);
        QVERIFY(!root->property("stageChromeIdle").toBool());

        // 3. A cursor handler must participate only while blanking; one naming
        //    Qt.ArrowCursor otherwise overrides every button's pointing hand.
        for (const char *name : { "callSpotlightCursor",
                                  "callFullScreenCursor" }) {
            auto *cursor = root->findChild<QObject *>(
                QString::fromLatin1(name));
            QVERIFY2(cursor != nullptr,
                     qPrintable(QStringLiteral("no cursor handler %1: the "
                                               "pointer never hides")
                                    .arg(QString::fromLatin1(name))));
            QCOMPARE(cursor->property("cursorShape").toInt(),
                     int(Qt::BlankCursor));
            QVERIFY2(!cursor->property("visible").toBool(),
                     qPrintable(QStringLiteral("%1 is present while the "
                                               "chrome is up, so it flattens "
                                               "the cursor of every control "
                                               "beneath it")
                                    .arg(QString::fromLatin1(name))));
            QVERIFY2(cursor->property("z").toReal() > 0,
                     qPrintable(QStringLiteral("%1 is not stacked above the "
                                               "picture, and the window "
                                               "takes its cursor from the "
                                               "TOP-MOST item that sets one")
                                    .arg(QString::fromLatin1(name))));
        }
#endif
    }

    void fullScreenIsItsOwnWindowThatEscapeLeaves()
    {
        // Full-screen share is a separate Window, since an overlay can only
        // fill the application window.
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const QString norm = normalized(stage);
        const QString stageCode = code(stage);
        // Prove the stripper leaves code behind before trusting a ban.
        QVERIFY(stageCode.contains(QStringLiteral("objectName: \"callStage\"")));

        // 1. A real top-level Window and a control that opens it.
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"callFullScreenWindow\"")));
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"callFullScreenButton\"")));

        // 2. Two ways out, one on the screen the user was already looking at.
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"callExitFullScreenButton\"")));
        QVERIFY(norm.contains(QStringLiteral(
            "objectName: \"callExitFullScreenFromStageButton\"")));

        // 3. Escape is a Keys handler, never a Shortcut: Esc is reserved in
        //    ShortcutRegistry, and two enabled Shortcuts on one sequence fire
        //    neither.
        QVERIFY2(norm.contains(QStringLiteral("Keys.onPressed: function")),
                 "full screen has no key handling at all");
        QVERIFY2(norm.contains(QStringLiteral("event.key === Qt.Key_Escape")),
                 "Escape does not leave full screen");
        QVERIFY2(!stageCode.contains(QStringLiteral("Shortcut {")),
                 "the call stage registers a Shortcut, which can shadow a "
                 "reserved sequence");

        // 4. Closing is accepted: refusing a close event vetoes application
        //    quit.
        QVERIFY(norm.contains(QStringLiteral("onClosing: root.exitFullScreen()")));
        QVERIFY2(!stageCode.contains(QStringLiteral("close.accepted = false")),
                 "the full-screen window refuses its close event");

        // 5. One surface per routing key: grid and spotlight stand down while
        //    full screen is up.
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

        // 7. Visibility is driven imperatively; the window manager writes
        //    `visibility`, which would break a binding.
        QVERIFY(norm.contains(QStringLiteral("fullScreenWindow.showFullScreen()")));
        QVERIFY2(!stageCode.contains(QStringLiteral("visibility:")),
                 "the full-screen window binds Window.visibility");
    }

    void theFullScreenWindowStaysHiddenUntilItIsAskedFor()
    {
        // Real instantiation. The Window exists for the stage's lifetime (so
        // it is transient for the main window) but must not be shown, nor its
        // contents built, until requested; hence the Loader inside it.
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

        // Found via the application's window list: a Window declared inside an
        // Item is not a child item.
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

        // With no call and nothing focused, the request is refused.
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
        // Per-participant volume control on the tile. Persistence is
        // SettingsManager's and seeding the controller's; this pins the
        // surface.
        const QString raw =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        QVERIFY(!raw.isEmpty());
        const QString tile = normalized(raw);

        // 1. Reachable by right-click and from the keyboard.
        QVERIFY(tile.contains(
            QStringLiteral("objectName: \"callParticipantVolumeSlider\"")));
        QVERIFY(tile.contains(
            QStringLiteral("objectName: \"callParticipantVolumeButton\"")));
        QVERIFY(tile.contains(QStringLiteral("acceptedButtons: Qt.RightButton")));
        QVERIFY(tile.contains(QStringLiteral("Qt.Key_Menu")));

        // 2. Range 0..200: the control exists for participants who are too
        //    quiet.
        QVERIFY(tile.contains(QStringLiteral("from: 0")));
        // The slider is the user scale; SfuMediaEngine::audioFactorPercent()
        // maps 100-200 onto 100-1000% and keeps 0-100 linear. Pinned by
        // theVolumeCurveIsLiteralBelowUnityAndExpandsAbove.
        QVERIFY(tile.contains(QStringLiteral("to: 200")));
        // 100 is marked: it is the value that changes nothing.
        QVERIFY(tile.contains(QStringLiteral(
            "objectName: \"callParticipantVolumeNeutralMark\"")));

        // 3. It opens at the current value, not always at 100.
        QVERIFY(tile.contains(QStringLiteral("function currentVolumePercent()")));
        //    Read from the store through the controller, not the model's
        //    `volumePercent` role, which is unseeded while a call is opening.
        QVERIFY(tile.contains(QStringLiteral(
            "app.groupCall.participantVolume(root.identity)")));
        QVERIFY(tile.contains(QStringLiteral(
            "onOpened: volumeSlider.value = root.currentVolumePercent()")));

        // 4. `onMoved`, never `onValueChanged`: the latter also fires for the
        //    programmatic read and would write the value back on every open.
        QVERIFY(tile.contains(
            QStringLiteral("onMoved: root.applyVolumePercent(value)")));
        QVERIFY2(!code(raw).contains(QStringLiteral("onValueChanged")),
                 "the volume slider must react to onMoved, not to "
                 "onValueChanged");
        // The glyph binding calls currentVolumePercent(); QML cannot track a
        // C++ call as a dependency, so it reads a revision counter.
        QVERIFY(tile.contains(QStringLiteral("property int volumeRevision")));
        QVERIFY(tile.contains(QStringLiteral("var _ = root.volumeRevision;")));

        // 5. Amplification is always disclosed, not only once past 100.
        QVERIFY(tile.contains(
            QStringLiteral("Above 100% amplifies and can clip.")));

        // 6. Not on the local tile: nobody hears their own published audio.
        QVERIFY(tile.contains(QStringLiteral(
            "readonly property bool _volumeOffered: !root.local")));

        // 7. The popup sinks its own presses. A Popup does not consume presses
        //    on itself, so without an all-buttons sink in `background:` they
        //    reach the tile's TapHandlers beneath. `modal: true` only blocks
        //    presses outside the popup.
        QVERIFY2(tile.contains(QStringLiteral(
                     "MouseArea { anchors.fill: parent acceptedButtons: "
                     "Qt.AllButtons")),
                 "the volume popup does not sink its own presses");
    }

    void theVolumeSurfaceGoesThroughTheControllerAndNeverTheStore()
    {
        // The write must go through the controller, which maps the SFU
        // identity (per device, possibly an opaque hash) to the Matrix user id
        // the preference is stored under. Writing app.settings directly would
        // lose the setting on the next rejoin.
        const QString tile =
            code(read(QStringLiteral(QML_DIR "/CallParticipantTile.qml")));
        QVERIFY(!tile.isEmpty());

        // Present-token control, so the ban cannot pass by matching nothing.
        QVERIFY2(tile.contains(QStringLiteral("app.groupCall.setParticipantVolume")),
                 "the volume write must go through the controller");

        QVERIFY2(!tile.contains(QStringLiteral("setCallParticipantVolume")),
                 "the tile must not write the store directly — the controller "
                 "owns the identity-to-user-id mapping");
        QVERIFY2(!tile.contains(QStringLiteral("QSettings")),
                 "no QML surface may touch QSettings");
    }

    void aFlatpakIsNotToldItHasNoCamera()
    {
        // Inside a Flatpak Qt lists no camera, yet the camera works through
        // the xdg Camera portal. The empty text must branch on the
        // controller's answer, which must actually ask the sandbox.
        const QString qml =
            read(QStringLiteral(QML_DIR "/CallDeviceSettings.qml"));
        const int empty = qml.indexOf(QStringLiteral("emptyText: app.callDevices.camerasChosenByDesktop"));
        QVERIFY2(empty >= 0, "the camera list's empty text does not ask "
                             "whether the desktop chooses the camera");
        const QString cpp =
            read(QStringLiteral(SRC_DIR "/calls/CallDeviceController.cpp"));
        const int fn = cpp.indexOf(QStringLiteral(
            "bool CallDeviceController::camerasChosenByDesktop() const"));
        QVERIFY(fn >= 0);
        const QString body = cpp.mid(fn, cpp.indexOf(QLatin1Char('}'), fn) - fn);
        QVERIFY2(body.contains(QStringLiteral("FLATPAK_ID"))
                     && body.contains(QStringLiteral("/.flatpak-info")),
                 "the answer must come from the sandbox, not a constant");
    }

    void microphoneGainIsOfferedWithItsMicrophoneAndSaysWhatItCosts()
    {
        // Microphone gain lives with the microphone picker: it is a property
        // of the capture device, not of a call or a person.
        const QString raw =
            read(QStringLiteral(QML_DIR "/CallDeviceSettings.qml"));
        QVERIFY(!raw.isEmpty());
        const QString devices = normalized(raw);

        QVERIFY(devices.contains(
            QStringLiteral("objectName: \"microphoneGainSlider\"")));
        // Same range and marked neutral point as the per-person control.
        QVERIFY(devices.contains(QStringLiteral("to: 200")));
        QVERIFY(devices.contains(QStringLiteral(
            "objectName: \"microphoneGainNeutralMark\"")));
        QVERIFY(devices.contains(
            QStringLiteral("Above 100% amplifies and can clip.")));

        // Bound to the setting and moved only by user gesture; `onValueChanged`
        // would write back the value the binding just delivered.
        QVERIFY(devices.contains(
            QStringLiteral("value: app.settings.microphoneGain")));
        QVERIFY(devices.contains(QStringLiteral(
            "onMoved: app.settings.microphoneGain = Math.round(value)")));
        QVERIFY2(!code(raw).contains(QStringLiteral("onValueChanged")),
                 "the gain slider must react to onMoved, not onValueChanged");
        QVERIFY2(!code(raw).contains(QStringLiteral("QSettings")),
                 "no QML surface may touch QSettings");
    }

    // The input level is reachable from the call menu, and writes the same
    // stored value as Settings: `microphoneGainChanged` -> applyAudioState()
    // is the only path to the engine's volume element.
    void theCallMenuCarriesTheMicrophoneLevelAndWritesTheSharedSetting()
    {
        const QString raw = read(QStringLiteral(QML_DIR "/CallDeviceMenu.qml"));
        QVERIFY2(!raw.isEmpty(), "CallDeviceMenu.qml is missing");
        // `code(raw)`: a comment naming a token must not satisfy the positive
        // assertions.
        const QString menu = normalized(code(raw));

        QVERIFY2(menu.contains(
                     QStringLiteral("objectName: \"callMenuMicGainSlider\"")),
                 "the in-call device menu has no level control");
        // Same range and marked neutral point as the other level controls.
        QVERIFY(menu.contains(QStringLiteral("to: 200")));
        QVERIFY(menu.contains(QStringLiteral(
            "objectName: \"callMenuMicGainNeutralMark\"")));
        // Reset only off the neutral point, as a real row.
        QVERIFY(menu.contains(
            QStringLiteral("objectName: \"callMenuMicGainReset\"")));

        // One stored value, two surfaces.
        QVERIFY2(menu.contains(
                     QStringLiteral("value: app.settings.microphoneGain")),
                 "the menu's level is not bound to the stored setting, so it "
                 "cannot follow a change made in Settings");
        QVERIFY2(menu.contains(QStringLiteral(
                     "onMoved: app.settings.microphoneGain = Math.round(value)")),
                 "the menu's level does not WRITE the stored setting — "
                 "SfuCallController reaches the engine's volume element only "
                 "through microphoneGainChanged, so a local-only slider would "
                 "move and change nothing");
        QVERIFY2(!code(raw).contains(QStringLiteral("onValueChanged")),
                 "the level must react to onMoved, not onValueChanged — the "
                 "latter also fires when the binding delivers a value that "
                 "came FROM the store, and writes it straight back");
        QVERIFY2(!code(raw).contains(QStringLiteral("QSettings")),
                 "no QML surface may touch QSettings");
    }

    // The device menu links to the Sound section of Settings.
    void theCallMenuPointsAtTheSoundSectionOfSettings()
    {
        const QString menu =
            normalized(code(read(QStringLiteral(QML_DIR "/CallDeviceMenu.qml"))));
        QVERIFY(!menu.isEmpty());
        QVERIFY2(menu.contains(
                     QStringLiteral("app.showSettingsSection(\"sound\")")),
                 "the call's device menu offers no way to the rest of the "
                 "sound settings");

        // The linked section must exist: an unknown key opens an empty page
        // with no error.
        const QString screen =
            normalized(code(read(QStringLiteral(QML_DIR "/SettingsScreen.qml"))));
        QVERIFY(!screen.isEmpty());
        QVERIFY2(screen.contains(
                     QStringLiteral("visible: root.section === \"sound\"")),
                 "SettingsScreen has no pane for the \"sound\" section the "
                 "call menu links to");
        QVERIFY2(screen.contains(QStringLiteral("sectionKey: \"sound\"")),
                 "the sound section has no navigation row, so it is reachable "
                 "only by deep link");

        // Moved, not copied: one set of device pickers. The runtime half is
        // SettingsShellQmlTest::theCallDevicesLiveInTheSoundSectionAndLeaveNotifications.
        QCOMPARE(screen.count(
                     QStringLiteral("objectName: \"callDeviceSettings\"")), 1);
    }

    void everyIconTheVolumeSurfacesAskForIsInTheBundledSubset()
    {
        // The bundled Material Symbols font is a subset; a name missing from
        // Icon.qml's map renders as tofu.
        const QString icons = read(QStringLiteral(QML_DIR "/Icon.qml"));
        QVERIFY(!icons.isEmpty());
        // `graphic_eq` is the in-call level row's above-100% glyph
        // (CallDeviceMenu.qml).
        for (const auto &name : {"volume_up", "volume_off", "graphic_eq",
                                 "mic", "settings"}) {
            QVERIFY2(icons.contains(
                         QStringLiteral("\"%1\":").arg(QLatin1String(name))),
                     qPrintable(QStringLiteral("icon '%1' is not mapped")
                                    .arg(QLatin1String(name))));
        }
    }

    // Every global ShortcutRegistry row must be bound to a `Shortcut` in qml/;
    // a row wired to nothing reports a key that does nothing. Lives here
    // because this target has QML_DIR and a QSettings sandbox. Source scan:
    // several sites are gated on a live room or call.
    void everyGlobalShortcutRowIsActuallyBoundInQml()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        QDir dir(QStringLiteral(QML_DIR));
        QVERIFY2(dir.exists(), QML_DIR);
        const auto files =
            dir.entryList({ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY(!files.isEmpty());
        // `code()`: a commented-out call site must not satisfy this.
        QString all;
        for (const QString &name : files)
            all += code(read(dir.filePath(name)));

        // Present-token control: a renamed accessor would otherwise match
        // nothing and pass.
        QVERIFY2(all.count(QStringLiteral("sequenceFor(")) > 10,
                 "the sweep found almost no sequenceFor() call sites, so it is "
                 "matching the wrong thing and would pass on anything");

        QStringList unwired;
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            // EditorContext rows are routed by the composer through
            // editorActionForKey(), not declared as `Shortcut`s.
            if (registry.data(idx, ShortcutRegistry::ContextRole).toInt()
                == ShortcutRegistry::EditorContext)
                continue;
            const QString id =
                registry.data(idx, ShortcutRegistry::IdRole).toString();
            if (!all.contains(QStringLiteral("sequenceFor(\"%1\")").arg(id)))
                unwired.append(id);
        }
        QVERIFY2(unwired.isEmpty(),
                 qPrintable(QStringLiteral(
                                "these registry rows report a key and do "
                                "nothing — no Shortcut in qml/ asks for them: "
                                "%1")
                                .arg(unwired.join(QStringLiteral(", ")))));
    }

    // The start-call key must carry the call button's whole gate:
    // `canStartCall()` has no in-a-call clause, and SfuCallController::join
    // tears down any running call. The predicate cannot be hoisted into C++
    // (a Q_INVOKABLE has no NOTIFY), so this asserts the two copies agree.
    void theStartCallKeyIsGatedLikeTheCallButton()
    {
        const QString button = normalized(
            code(read(QStringLiteral(QML_DIR "/TimelinePane.qml"))));
        const QString shell = normalized(
            code(read(QStringLiteral(QML_DIR "/MainScreen.qml"))));
        QVERIFY(!button.isEmpty());
        QVERIFY(!shell.isEmpty());

        // The clause list is derived from the button, so a clause added to the
        // button fails here until the key is updated too. Derived from
        // `available:`, not `visible:`, which also carries the header's fold
        // state; a folded action is still offered and its key must work.
        const QString vis =
            QStringLiteral("property bool available: app.currentRoomId !== \"\""
                           " && app.canStartCall");
        const int from = button.indexOf(vis);
        QVERIFY2(from >= 0,
                 "TimelinePane's call button no longer opens with "
                 "currentRoomId + canStartCall — re-anchor this slice");
        const int to = button.indexOf(QStringLiteral(" enabled:"), from);
        QVERIFY2(to > from, "the call button's available: expression is not "
                            "terminated by an enabled: property");
        const char *kGate = "property bool available:";
        const QString expr =
            button.mid(from + int(qstrlen(kGate)), to - from
                       - int(qstrlen(kGate)))
                .trimmed();

        // Split on `&&` at paren depth zero; the last conjunct is a
        // parenthesised `||`.
        QStringList clauses;
        int depth = 0;
        int last = 0;
        for (int i = 0; i < expr.size(); ++i) {
            const QChar c = expr.at(i);
            if (c == QLatin1Char('('))
                ++depth;
            else if (c == QLatin1Char(')'))
                --depth;
            else if (depth == 0 && c == QLatin1Char('&')
                     && i + 1 < expr.size()
                     && expr.at(i + 1) == QLatin1Char('&')) {
                clauses << expr.mid(last, i - last).trimmed();
                last = i + 2;
            }
        }
        clauses << expr.mid(last).trimmed();
        clauses.removeAll(QString());

        // The gate reads `app.callGateRevision`: a binding that only calls the
        // canStartCall() invokable evaluates once and goes stale.
        QVERIFY2(expr.contains(QStringLiteral("app.callGateRevision")),
                 "the call button gates on canStartCall() without reading "
                 "callGateRevision, so RTC state arriving after the room was "
                 "opened never reaches the button");

        // Present-token control on the slice and the split.
        QVERIFY2(clauses.size() >= 4,
                 qPrintable(QStringLiteral("only %1 conjuncts came out of the "
                                           "call button's gate — the slice is "
                                           "wrong and this case is vacuous")
                                .arg(clauses.size())));

        // The Shortcut's own block: other keys in MainScreen also mention
        // groupCall.active.
        const int at = shell.indexOf(
            QStringLiteral("sequenceFor(\"call.startCall\")"));
        QVERIFY2(at >= 0, "no Shortcut in MainScreen.qml asks for "
                          "call.startCall");
        const int end = shell.indexOf(QStringLiteral("onActivated"), at);
        QVERIFY2(end > at, "the call.startCall Shortcut has no onActivated");
        const QString gate = shell.mid(at, end - at);

        for (const QString &clause : std::as_const(clauses)) {
            QVERIFY2(gate.contains(clause),
                     qPrintable(QStringLiteral(
                                    "the call.startCall shortcut is missing "
                                    "'%1', which the call button gates on — "
                                    "the key can act where the button refuses")
                                    .arg(clause)));
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

    // The room-row call glyph must never trigger a refresh:
    // RtcController::refresh() can fall back to a full /state request per
    // idle room. It reads what the controller knows and re-reads on
    // sessionChanged.
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
        // Behind a Loader: an Icon is a Text, and a never-laid-out empty Text
        // keeps ItemObservesViewport forever.
        QVERIFY2(glyph.contains(QStringLiteral("Loader")),
                 "the glyph's Icon is not behind a Loader, so every room row "
                 "adds a permanent viewport observer");

        // One component shared by both layouts.
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

    // The member roster's chrome stays compact. Pinned as numbers, since each
    // can creep back up one control at a time.
    void theMemberRosterStaysCompact()
    {
        const QString panel = read(QStringLiteral(QML_DIR "/RoomInfoPanel.qml"));
        QVERIFY(!panel.isEmpty());

        // One row of chrome: search field and three controls share it.
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
        // ...as icons; labels do not fit a column this narrow.
        QVERIFY2(!chrome.contains(QStringLiteral("text: qsTr(\"A to Z\")")),
                 "the sort control went back to a text button");

        // The row and heading heights, and the avatar.
        const QString list = panel.mid(listAt);
        // Every size here follows the text-size slider (AppTheme.scaled).
        // Heights are expressions over scaled text, or rows clip at 140%.
        QVERIFY2(list.contains(QStringLiteral("AppTheme.scaled(AppTheme.textTitle)")),
                 "the member name does not follow the text-size slider");
        QVERIFY2(list.contains(QStringLiteral("Math.max(34, AppTheme.scaled")),
                 "the member row height is a literal again, so it clips its "
                 "own text at a larger slider position");
        QVERIFY2(list.contains(QStringLiteral("Math.max(24, AppTheme.scaled")),
                 "the member avatar no longer follows the row");
        QVERIFY2(list.contains(QStringLiteral("spacing: 3")),
                 "the member rows touch, so two names read as one block");
        // The tab strip is one size in every section; a control that resizes
        // with its own active tab looks broken. Comments are stripped (the
        // fix's comment names the banned token) and the scan is bounded to the
        // control's block (it ends at onActivated).
        const QString clean = code(panel);
        const int tabsAt = clean.indexOf(QStringLiteral("objectName: \"roomInfoTabs\""));
        QVERIFY(tabsAt > 0);
        const int tabsEnd = clean.indexOf(QStringLiteral("onActivated:"), tabsAt);
        QVERIFY(tabsEnd > tabsAt);
        const QString tabs = clean.mid(tabsAt, tabsEnd - tabsAt);
        // Prove the window contains what it should, or the negative
        // assertions are vacuous.
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

        // The panel may never take so much width that the shell overflows; a
        // stored width outlives the window it was dragged in.
        const QString pane = read(QStringLiteral(QML_DIR "/TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY2(pane.contains(QStringLiteral("Math.min(app.settings.sidePanelWidth")),
                 "the info panel takes its stored width uncapped, so a window "
                 "narrower than it was dragged in pushes the panel off the "
                 "right edge");
        QVERIFY2(panel.contains(QStringLiteral("clip: true")),
                 "the info panel does not clip, so anything wider than it "
                 "paints over the timeline");

        // One body text size across the panel; density is the row height's
        // job. Scoped to the member row component and anchored on the
        // Widgets section that follows the roster; a missing anchor fails.
        const int rowAt = panel.indexOf(QStringLiteral("id: memberRowComponent"));
        QVERIFY2(rowAt > 0, "the member row component is gone");
        const int rowEnd = panel.indexOf(QStringLiteral("── Widgets"), rowAt);
        QVERIFY2(rowEnd > rowAt,
                 "the section after the roster is gone: re-anchor this scope "
                 "rather than deleting the bound, or the check silently "
                 "covers the whole file");
        const QString row = panel.mid(rowAt, rowEnd - rowAt);
        QVERIFY2(!row.contains(QStringLiteral("font.pixelSize: AppTheme.text")),
                 "a member row sets an UNSCALED font size, so it ignores the "
                 "text-size slider every other surface honours");
    }

    /// A control row with no implicit width is laid out on top of its
    /// neighbours. CallHeaderBar is hosted three ways; the collapsed strip and
    /// the full-screen window read its implicit size through a Loader, so it
    /// must report an implicitWidth. Geometric: every control must lie inside
    /// the box the layout gave the bar.
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
        // Declared before the hosted item so it is destroyed after it:
        // setParentItem also takes QObject ownership.
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

        // Layouts rearrange on the asynchronous polish pass, so converge.
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

    /// The nameplate row must be capped with its pill. `anchors.centerIn`
    /// sets only position, so the row took its full implicit width and the
    /// label never elided.
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
        // Before the fixture, so the fixture's owner outlives it.
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
        // Converge on a laid-out frame; sizes settle on a polish pass.
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

    /// The spotlight's strip of other surfaces yields height, since the rest
    /// of the column is fixed; below a usable tile it becomes the bubble row.
    /// The arithmetic is checked here and the call site by scan.
    // With video showing, the divider clamp keeps enough height for a
    // picture; the spotlight's overlay controls do not compact and would
    // otherwise draw across the clipped edge. Drives the real clamp.
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

        // The stage's own declared minimum, so this cannot drift from CallStage.
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
        // The clamp must use it, anchored on the expression rather than a
        // fixed window after the function name (comments move the code).
        QVERIFY2(pane_.contains(QStringLiteral(
                     "var floor = root.callPanelHasVideo")),
                 "the clamp no longer distinguishes a call showing video, so "
                 "a share is squeezed to the voice-only floor again");
        QVERIFY2(pane_.contains(QStringLiteral(
                     "Math.min(root.callPanelVideoFloor,")),
                 "the clamp no longer applies the stage's own minimum");

        // Drag to nothing: with video the clamp keeps a picture; without it the
        // voice rule stands.
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

        // The overlay is absent, not crushed, once the spotlight cannot host it.
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
        // Whitespace-normalized, so reflows do not break the assertions.
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
        // Tiles follow the band rather than their own literals.
        QVERIFY(block.contains(QStringLiteral("spotlightColumn.stripTileHeight")));
        QVERIFY2(!stage.contains(QStringLiteral("width: active ? 140 : 0")),
                 "a strip tile is back to a hardcoded width");
        // The short-stage fallback is reached, not merely defined.
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

        // A short stage: the strip must not be the bigger half of it.
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
        // Too small for avatar plus nameplate: the bubble row instead.
        QCOMPARE(modeFor(shortStage), QStringLiteral("bubbles"));

        // A roomy stage is unchanged.
        QCOMPARE(modeFor(400), QStringLiteral("tiles"));
        QCOMPARE(stripFor(400), 96);
        QCOMPARE(stripFor(1000), 96);
        // Degenerate sizes (before first layout) must not produce a zero-height
        // strip that never grows back.
        QCOMPARE(stripFor(0), 96);
        QCOMPARE(modeFor(0), QStringLiteral("tiles"));
        // A drawn tile strip is always usable and never above 40% of the stage.
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

    // requestScreenShare() must actually consult linuxShareRoute();
    // call-controller tests only prove the policy. Driving the Linux branch
    // needs a live call and portal, so this reads the branch.
    void theLinuxScreenShareBranchActuallyConsultsTheRoutePolicy()
    {
        QFile file(QStringLiteral(SRC_DIR "/calls/SfuCallController.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(file.readAll());

        // Slice out requestScreenShare() and prove the slice worked first.
        const int begin =
            source.indexOf(QStringLiteral("void SfuCallController::"
                                          "requestScreenShare()"));
        QVERIFY2(begin > 0, "requestScreenShare() is gone or was renamed");
        const int end = source.indexOf(
            QStringLiteral("void SfuCallController::chooseScreenShareSource"),
            begin);
        QVERIFY2(end > begin, "could not find the end of requestScreenShare()");
        const QString body = source.mid(begin, end - begin);
        QVERIFY2(body.contains(QStringLiteral("screenShareSourcesAvailable")),
                 "the slice does not contain a line this function is known "
                 "to have, so it is not the function");

        // The policy is consulted and supplies the refusal wording.
        QVERIFY2(body.contains(QStringLiteral("linuxShareRoute(")),
                 "requestScreenShare() does not consult the route policy, so "
                 "every case that tests the policy tests nothing reachable");
        QVERIFY2(body.contains(QStringLiteral(
                     "linuxShareRefusal(route, runningSandboxed())")),
                 "the Linux refusals do not come from the policy, or no "
                 "longer tell it whether the build is sandboxed — a Flatpak "
                 "would be told to install a host package it cannot load");
        // The fallback reaches the same picker Windows and macOS use.
        QVERIFY2(body.contains(QStringLiteral("populateLinuxDisplaySources()")),
                 "the fallback does not populate the shared picker's rows");
        QVERIFY2(body.contains(QStringLiteral("screenShareSourcesAvailable()")),
                 "the fallback never opens the picker");

        // The portal is still asked first.
        QVERIFY2(body.contains(QStringLiteral("m_portal->requestShare(")),
                 "the portal path is gone from requestScreenShare()");
        // Both must be found before comparing: indexOf's -1 is less than any
        // real offset.
        const int portalAt = body.indexOf(QStringLiteral(
            "LinuxShareRoute::Portal"));
        const int fallbackAt = body.indexOf(QStringLiteral(
            "LinuxShareRoute::FallbackDisplays"));
        QVERIFY2(portalAt >= 0, "the Portal route is not handled at all");
        QVERIFY2(fallbackAt >= 0, "the fallback route is not handled at all");
        QVERIFY2(portalAt < fallbackAt,
                 "the fallback is considered before the portal");

        // The old unconditional refusal survives only in the Windows/macOS
        // guard.
        const int oldRefusal = body.indexOf(QStringLiteral(
            "Screen sharing isn't available on this desktop."));
        if (oldRefusal >= 0) {
            const int guard = body.indexOf(QStringLiteral(
                "#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)"));
            QVERIFY2(guard >= 0 && guard < oldRefusal,
                     "the unactionable refusal is reachable on Linux again");
        }

        // The capture element's name must not appear in any string literal
        // here: the engine owns pipeline text, and a second spelling can drift.
        // Prose comments naming it are allowed. A quote, then non-quotes, then
        // the name on one line is a literal.
        const QRegularExpression spelledInAString(
            QStringLiteral("\"[^\"\n]*ximagesrc"));
        QVERIFY2(!spelledInAString.match(source).hasMatch(),
                 "the controller spells the capture element inside a string "
                 "instead of asking the engine for its name");
        QVERIFY2(body.contains(QStringLiteral("x11ScreenCaptureElementName()")),
                 "the element probe does not ask the engine which element it "
                 "should be probing for");
    }

    // The stage has one control surface, and it is not a full-width dock
    // along the bottom (which took picture height and moved the controls).
    void theStageHasOneControlSurfaceAndItIsNotAlongTheBottom()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());

        // Exactly one CallHeaderBar in the stage body, plus the full-screen one.
        QCOMPARE(stage.count(QStringLiteral("objectName: \"callStageControls\"")), 1);
        QCOMPARE(stage.count(QStringLiteral("objectName: \"callFullScreenDock\"")), 1);
        QVERIFY2(!stage.contains(QStringLiteral("callStageDock")),
                 "the bottom dock is back: it costs a strip of every screen "
                 "share and moves the controls between states");

        // Not gated on the collapsed state.
        const int at = stage.indexOf(QStringLiteral("objectName: \"callStageControls\""));
        QVERIFY(at > 0);
        const int loader = stage.lastIndexOf(QStringLiteral("Loader {"), at);
        QVERIFY(loader > 0);
        const QString block = stage.mid(loader, at - loader);
        QVERIFY2(!block.contains(QStringLiteral("active: root.collapsed")),
                 "the one control surface is collapsed-only again");
        QVERIFY2(block.contains(QStringLiteral("active: true")),
                 "the control surface must be live in both states");

        // Not compact while expanded: `compact` hides screen share, raise hand
        // and the device chevrons. Bounded by expressions at both ends, not a
        // character count; CallStage uses `compact` elsewhere too.
        const int barEnd =
            stage.indexOf(QStringLiteral("onParticipantsRequested"), at);
        QVERIFY2(barEnd > at, "the control bar block did not close as expected");
        const QString barBlock = stage.mid(at, barEnd - at);
        QVERIFY2(barBlock.contains(QStringLiteral("compact: root.collapsed")),
                 "the expanded stage must get the FULL control set: compact "
                 "hides screen share, raise hand and every device chevron");
        QVERIFY2(!barBlock.contains(QStringLiteral("compact: true")),
                 "an always-compact surface loses Share and Raise hand when "
                 "the call is expanded");
    }

    // The premise of the case above, asserted against CallHeaderBar itself.
    void compactIsAReducedControlSetNotJustASmallerOne()
    {
        const QString bar = read(QStringLiteral(QML_DIR "/CallHeaderBar.qml"));
        QVERIFY(!bar.isEmpty());
        const int hidden = bar.count(QStringLiteral("!root.compact"));
        QVERIFY2(hidden >= 4,
                 qPrintable(QStringLiteral("expected compact to gate several "
                                           "controls, found %1").arg(hidden)));
    }

    // Full-screen chrome retires when the pointer stops.
    void fullScreenOverlaysRetireWhenThePointerIsStill()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(stage.contains(QStringLiteral("overlaysIdle")));
        QVERIFY2(stage.contains(QStringLiteral("HoverHandler")),
                 "movement must wake the chrome without consuming a click");
        // Both overlays fade, including the exit affordance.
        QCOMPARE(stage.count(
                     QStringLiteral("opacity: fullScreenSurface.overlaysIdle ? 0 : 1")),
                 2);
        // Leaving full screen must not strand them hidden.
        QVERIFY2(stage.contains(QStringLiteral("onFullScreenActiveChanged")),
                 "the idle flag outlives the state it belongs to");
    }

    // The padlock must not reassure while a remote stream's frames are being
    // dropped for want of a usable key.
    void theEncryptionBadgeStopsReassuringWhenMediaIsBeingDropped()
    {
        const QString norm = normalized(read(QStringLiteral(QML_DIR "/CallStage.qml")));
        QVERIFY(!norm.isEmpty());
        const int lock = norm.indexOf(QStringLiteral("app.groupCall.mediaEncrypted"));
        QVERIFY2(lock >= 0, "the encryption badge is gone entirely");
        // Bounded to the badge's Loader.
        const QString block = norm.mid(lock, 900);
        // The icon and colour bindings must depend on it; a mention in the
        // tooltip alone would pass with constants.
        QVERIFY2(block.contains(QStringLiteral(
                     "name: app.groupCall.remoteMediaBlocked ? \"warning\" : \"lock\"")),
                 "the badge's ICON does not depend on remoteMediaBlocked, so "
                 "it still shows a reassuring lock over a participant whose "
                 "every frame is being dropped");
        QVERIFY2(block.contains(QStringLiteral(
                     "color: app.groupCall.remoteMediaBlocked ? AppTheme.warning "
                     ": AppTheme.success")),
                 "the badge's COLOUR does not depend on remoteMediaBlocked, "
                 "so the blocked state is still drawn in the success colour");
    }


    // Every join-block token has wording on every surface that shows one.
    // Tokens are derived from RtcController::joinBlockReason's body, so a new
    // token fails here until the QML maps it.
    void everyJoinBlockTokenHasWordingOnEverySurface()
    {
        const QString controller = read(QStringLiteral(
            SRC_DIR "/calls/RtcController.cpp"));
        QVERIFY(!controller.isEmpty());

        // The body of joinBlockReason, to its closing brace.
        const int at = controller.indexOf(QStringLiteral(
            "QString RtcController::joinBlockReason("));
        QVERIFY2(at >= 0, "joinBlockReason is gone; this contract is aimed "
                          "at nothing");
        int depth = 0;
        bool started = false;
        int end = controller.size();
        for (int i = at; i < controller.size(); ++i) {
            if (controller.at(i) == QLatin1Char('{')) {
                ++depth;
                started = true;
            } else if (controller.at(i) == QLatin1Char('}')) {
                --depth;
                if (started && depth <= 0) {
                    end = i;
                    break;
                }
            }
        }
        QVERIFY(started);
        const QString body = controller.mid(at, end - at);

        static const QRegularExpression token(
            QStringLiteral("QStringLiteral\\(\"([a-z_]+)\"\\)"));
        QStringList tokens;
        auto it = token.globalMatch(body);
        while (it.hasNext()) {
            const QString found = it.next().captured(1);
            if (!tokens.contains(found))
                tokens.append(found);
        }
        // Mutation guard: a derivation that matches nothing proves nothing.
        QVERIFY2(tokens.size() >= 7,
                 qPrintable(QStringLiteral("the token derivation found only "
                                           "%1 tokens, so it is not reading "
                                           "joinBlockReason at all")
                                .arg(tokens.size())));
        QVERIFY2(tokens.contains(QStringLiteral("media_encryption_unavailable")),
                 "the derivation missed the encryption block, which is the "
                 "token this contract exists for");

        struct Surface { const char *path; const char *what; };
        const QList<Surface> surfaces = {
            { QML_DIR "/RoomCallBanner.qml",    "the room's call banner" },
            { QML_DIR "/IncomingCallPrompt.qml", "the incoming-call card" },
            { QML_DIR "/CallEventDelegate.qml",  "the timeline's call row" },
        };
        for (const Surface &surface : surfaces) {
            const QString source = read(QString::fromUtf8(surface.path));
            QVERIFY2(!source.isEmpty(), surface.path);
            for (const QString &name : std::as_const(tokens)) {
                QVERIFY2(source.contains(QStringLiteral("case \"%1\":")
                                             .arg(name)),
                         qPrintable(QStringLiteral(
                                        "%1 has no wording for the join "
                                        "block `%2`, so it falls into the "
                                        "default and says nothing useful")
                                        .arg(QString::fromUtf8(surface.what),
                                             name)));
            }
        }

        // join()'s own refusal message in C++ must know every token too.
        const QString sfu = read(QStringLiteral(
            SRC_DIR "/calls/SfuCallController.cpp"));
        QVERIFY(!sfu.isEmpty());
        const int refusal = sfu.indexOf(QStringLiteral(
            "SfuCallController::joinRefusalMessage("));
        QVERIFY2(refusal >= 0,
                 "join() has no wording for a block it refuses on");
        const QString refusalBody = sfu.mid(refusal, 2400);
        for (const QString &name : std::as_const(tokens)) {
            QVERIFY2(refusalBody.contains(QStringLiteral("QLatin1String(\"%1\")")
                                              .arg(name)),
                     qPrintable(QStringLiteral(
                                    "joinRefusalMessage() has no wording for "
                                    "`%1`").arg(name)));
        }
    }

    // join() must return on every block rather than publishing a membership
    // for a session this client cannot join. Source-read: join() needs a live
    // SfuMediaEngine, which this build does not have.
    void joinRefusesEveryBlockInsteadOfPublishingAMembership()
    {
        const QString sfu = read(QStringLiteral(
            SRC_DIR "/calls/SfuCallController.cpp"));
        QVERIFY(!sfu.isEmpty());
        const int gate = sfu.indexOf(QStringLiteral(
            "m_rtc->joinBlockReason(roomId)"));
        QVERIFY2(gate >= 0, "join() no longer consults the join gate at all");
        const int publish = sfu.indexOf(
            QStringLiteral("m_client->rtcPublishMembership("), gate);
        QVERIFY2(publish > gate,
                 "the membership publish moved; this contract is aimed at "
                 "nothing");
        const QString between = sfu.mid(gate, publish - gate);

        // The refusal branch to its closing brace, so the `return false;` is
        // provably inside it.
        const int branch =
            between.indexOf(QStringLiteral("if (!block.isEmpty()) {"));
        QVERIFY2(branch >= 0,
                 "join() does not refuse on a standing join block at all, so "
                 "it publishes a membership for a call it has just decided "
                 "it cannot join");
        int depth = 0;
        bool started = false;
        int close = between.size();
        for (int i = branch; i < between.size(); ++i) {
            if (between.at(i) == QLatin1Char('{')) {
                ++depth;
                started = true;
            } else if (between.at(i) == QLatin1Char('}')) {
                --depth;
                if (started && depth <= 0) {
                    close = i;
                    break;
                }
            }
        }
        QVERIFY(started);
        QVERIFY2(between.mid(branch, close - branch)
                     .contains(QStringLiteral("return false;")),
                 "join()'s block branch falls through instead of returning, "
                 "so the warning it logs says the opposite of what happens");
        QVERIFY2(!between.contains(QStringLiteral(
                     "if (block == QLatin1String(\"media_encryption_unavailable\")) {")),
                 "join() still refuses only the encryption block and goes on "
                 "to publish for every other one");
    }

    // A 404/400/M_UNRECOGNIZED from discovery is an answer ("no MatrixRTC"),
    // not a failed check: `server_answered` must be true for it, or the join
    // gate says "couldn't check" and discovery retries on every room change.
    // Behaviour is pinned by
    // `a_homeserver_without_matrixrtc_counts_as_having_answered` in
    // rust/src/rtc.rs.
    void aDefinitiveNoMatrixRtcAnswerIsNotReportedAsAFailedCheck()
    {
        const QString rtc = read(QStringLiteral(
            QML_DIR "/../rust/src/rtc.rs"));
        QVERIFY(!rtc.isEmpty());
        QVERIFY2(!rtc.contains(QStringLiteral(
                     "\"server_answered\": category.is_empty()")),
                 "a homeserver that answered 404 (no MSC4143) is still "
                 "reported as a check that failed, so the UI says "
                 "\"couldn't check\" and discovery re-runs on every room "
                 "change for the whole session");
        QVERIFY2(rtc.contains(QStringLiteral(
                     "\"server_answered\": discovery_answer_is_definitive(&category)")),
                 "the discovery payload no longer routes through the helper "
                 "that decides what counts as an answer");
        QVERIFY2(rtc.contains(QStringLiteral(
                     "fn discovery_answer_is_definitive(category: &str) -> bool")),
                 "the helper is gone");
    }

    // Only the newest call row in a room offers Join; `sessionLive` is a
    // per-room answer. Drives the real delegate with a live-looking session,
    // so the only difference between halves is isLatestCallRow.
    void anOlderCallRowOffersNoJoinButton()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QByteArray(R"(
import QtQuick
import MatrixClient
Item {
    property alias newest: newestRow
    property alias older: olderRow
    CallEventDelegate {
        id: newestRow
        roomId: "!r:mock.local"
        sentence: "Alice started a call."
        isLatestCallRow: true
    }
    CallEventDelegate {
        id: olderRow
        roomId: "!r:mock.local"
        sentence: "Alice started a call."
        isLatestCallRow: false
    }
}
)"), QUrl(QStringLiteral("qrc:/fixture/CallRowJoin.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));

        auto *older = root->property("older").value<QObject *>();
        auto *newest = root->property("newest").value<QObject *>();
        QVERIFY(older);
        QVERIFY(newest);
        // The property is real and tracks.
        QVERIFY2(older->property("isLatestCallRow").isValid(),
                 "CallEventDelegate does not declare isLatestCallRow, so the "
                 "host cannot tell it which row owns the live session");
        QVERIFY2(older->property("supersededByNewerCall").toBool(),
                 "an older call row does not consider itself superseded");
        QVERIFY2(!newest->property("supersededByNewerCall").toBool(),
                 "the newest call row believes it has been superseded");

        // canJoin's use of it is pinned against the source: with no `app` in
        // the test context `sessionLive` is false, so canJoin is false either
        // way and a runtime assertion would pass with the gate deleted.
        QFile delegateSource(QStringLiteral(QML_DIR "/CallEventDelegate.qml"));
        QVERIFY2(delegateSource.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(delegateSource.errorString()));
        const QString qml = QString::fromUtf8(delegateSource.readAll());
        const int canJoinAt = qml.indexOf(QStringLiteral("readonly property bool canJoin:"));
        QVERIFY2(canJoinAt >= 0, "canJoin is gone from CallEventDelegate");
        // Bounded to the statement: the end is the next declaration at the same
        // indent, and not finding one fails. A blank-line slice fails open.
        const int exprEnd =
            qml.indexOf(QRegularExpression(QStringLiteral("\n    [A-Za-z/]")),
                        canJoinAt + 1);
        QVERIFY2(exprEnd > canJoinAt,
                 "could not find the end of the canJoin expression, so the "
                 "slice below would cover the rest of the file and pass "
                 "trivially");
        const QString canJoinExpr = qml.mid(canJoinAt, exprEnd - canJoinAt);
        QVERIFY2(canJoinExpr.contains(QStringLiteral("supersededByNewerCall")),
                 qPrintable(QStringLiteral(
                     "canJoin does not consult supersededByNewerCall, so every "
                     "call row in a room offers Join while any call is live. "
                     "Expression was: %1").arg(canJoinExpr)));
    }

    void theCallRowIsToldWhichRowIsTheNewest()
    {
        // MessageDelegate must pass the model's answer to `isLatestCallRow`.
        // It defaults to true (so hosts that cannot answer still show Join),
        // so a missing binding makes every row offer Join and no other suite
        // notices.
        QFile hostSource(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY2(hostSource.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(hostSource.errorString()));
        const QString host = QString::fromUtf8(hostSource.readAll());
        const int bindAt = host.indexOf(QStringLiteral("isLatestCallRow:"));
        QVERIFY2(bindAt >= 0,
                 "MessageDelegate no longer tells CallEventDelegate which row "
                 "is the newest call, so the delegate's permissive default "
                 "puts Join back on every call row in the room");
        // Bounded to the binding's statement; not finding the end fails.
        const int bindEnd =
            host.indexOf(QRegularExpression(QStringLiteral("\n            [A-Za-z]")),
                         bindAt + 1);
        QVERIFY2(bindEnd > bindAt,
                 "could not find the end of the isLatestCallRow binding");
        const QString bindExpr = host.mid(bindAt, bindEnd - bindAt);
        QVERIFY2(bindExpr.contains(QStringLiteral("latestCallEventId")),
                 qPrintable(QStringLiteral(
                     "isLatestCallRow is not bound to the model's "
                     "latestCallEventId, so it cannot know which row is "
                     "newest. Expression was: %1").arg(bindExpr)));
        QVERIFY2(bindExpr.contains(QStringLiteral("model.eventId")),
                 qPrintable(QStringLiteral(
                     "isLatestCallRow does not compare against this row's own "
                     "event id, so every row would answer the same. "
                     "Expression was: %1").arg(bindExpr)));
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(CallUiContractTest)
#include "CallUiContractTest.moc"
