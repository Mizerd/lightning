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

class VolumeGroupCall : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE int participantVolume(const QString &) const { return stored; }
    Q_INVOKABLE void setParticipantVolume(const QString &id, int percent)
    {
        lastId = id;
        stored = percent;
        ++writes;
    }
    int stored = 100;
    int writes = 0;
    QString lastId;
};

class VolumeApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *groupCall READ groupCall CONSTANT)
public:
    explicit VolumeApp(QObject *parent = nullptr)
        : QObject(parent), m_call(new VolumeGroupCall(this)) {}
    QObject *groupCall() const { return m_call; }
    VolumeGroupCall *call() const { return m_call; }

private:
    VolumeGroupCall *m_call = nullptr;
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

static QQuickItem *findVisualChild(QObject *root, const QString &name)
{
    auto *item = qobject_cast<QQuickItem *>(root);
    if (!item) {
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

    static QString code(const QString &s)
    {
        QString out = s;
        out.remove(QRegularExpression(QStringLiteral("(?m)^[ \\t]*//.*$")));
        return out;
    }

    static void settle(int rounds = 3)
    {
        for (int i = 0; i < rounds; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    QQuickItem *buildBanner(QQmlEngine &engine, StubApp &stub,
                            const QString &room)
    {
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

    void everyPathThatTakesTheSessionAwayLeavesTheCallFirst()
    {
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
        const QString bar = normalized(
            read(QStringLiteral(QML_DIR "/CallHeaderBar.qml")));
        QVERIFY(!bar.isEmpty());

        for (const auto &name : {"callBarMicButton", "callBarDeafenButton",
                                 "callBarCameraButton",
                                 "callBarScreenShareButton",
                                 "callBarHangUpButton"}) {
            QVERIFY2(bar.contains(QStringLiteral("objectName: \"%1\"")
                                      .arg(QLatin1String(name))),
                     qPrintable(QStringLiteral("missing %1")
                                    .arg(QLatin1String(name))));
        }
        QVERIFY(bar.contains(QStringLiteral("objectName: \"callBarMicChevron\"")));
        QVERIFY(bar.contains(
            QStringLiteral("objectName: \"callBarSpeakerChevron\"")));
        QVERIFY(bar.contains(QStringLiteral("callRoomId === app.currentRoomId")));
        QVERIFY(bar.contains(QStringLiteral("app.groupCall.requestScreenShare()")));
    }

    void theCornerCardNoLongerOwnsALiveCall()
    {
        const QString card = normalized(
            read(QStringLiteral(QML_DIR "/IncomingCallPrompt.qml")));
        QVERIFY(!card.isEmpty());
        QVERIFY2(!card.contains(QStringLiteral("inCallMuteButton")),
                 "mute moved to the top bar and must not be duplicated here");
        QVERIFY(card.contains(QStringLiteral("(inCall && !barCovers)")));
        QVERIFY(card.contains(QStringLiteral(
            "app.calls.activeRoomId === app.currentRoomId")));
        QVERIFY(card.contains(QStringLiteral("incomingCallPromptHangup")));
    }

    void promptBindsToCallStateNotPolicy()
    {
        const QString norm = normalized(
            read(QStringLiteral(QML_DIR "/IncomingCallPrompt.qml")));
        QVERIFY(!norm.isEmpty());
        QVERIFY(norm.contains(
            QStringLiteral("app.calls.activeCallId !== dismissedCallId")));
        QVERIFY(norm.contains(QStringLiteral("app.currentScreen === 1")));
        QVERIFY(!norm.contains(QStringLiteral("shouldRing")));
        QVERIFY(norm.contains(
            QStringLiteral("onClicked: app.calls.rejectIncoming()")));
        QVERIFY(norm.contains(QStringLiteral("onClicked: root.dismiss()")));
        QVERIFY(norm.contains(QStringLiteral(
            "root.dismissedCallId = id")));
        QVERIFY(norm.contains(QStringLiteral("app.callSounds.dismissRing(id)")));
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"incomingCallPromptDecline\"")));
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"incomingCallPromptDismiss\"")));
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"incomingCallPromptHangup\"")));
        QVERIFY(norm.contains(
            QStringLiteral("onClicked: app.calls.hangup()")));
        QVERIFY(norm.contains(QStringLiteral(
            "readonly property bool legacyAcceptOffered: root.ringing && "
            "!root.rtcRing && app.calls.mediaBackendAvailable")));
        QVERIFY(norm.contains(
            QStringLiteral("visible: root.legacyAcceptOffered")));
        QVERIFY(norm.contains(
            QStringLiteral("if (!app.calls.answer(true))")));
        QVERIFY(norm.contains(QStringLiteral("isn't supported yet")));
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
        const QString scope = norm.mid(button, 2000);
        QVERIFY(scope.contains(
            QStringLiteral("app.canStartCall(app.currentRoomId)")));
        QVERIFY(scope.contains(QStringLiteral(
            "onClicked: app.startCall(app.currentRoomId, false)")));
        QVERIFY(scope.contains(QStringLiteral("!app.groupCall.active")));
        QVERIFY(scope.contains(QStringLiteral("enabled: true")));
        QVERIFY2(!norm.contains(QStringLiteral("Voice calls are coming soon")),
                 "the coming-soon wording must not outlive the disabled state");
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
        QVERIFY(update > call);
        QVERIFY(verify > update);
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

        QCOMPARE(root->property("live").toBool(), false);
        QCOMPARE(root->property("visible").toBool(), false);
        QCOMPARE(root->property("height").toDouble(), 0.0);

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

    void theHeaderPlacementFoldsInsteadOfOverflowingANarrowPane()
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
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient
Item {
    id: outer
    width: 1000
    height: 80
    property alias bar: hb
    CallHeaderBar {
        id: hb
        objectName: "fixtureHeader"
        previewMode: true
        width: outer.width
    }
}
)"),
                          QUrl());
        QVERIFY2(component.isReady(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> holder(component.create());
        auto *outer = qobject_cast<QQuickItem *>(holder.get());
        QVERIFY(outer != nullptr);
        QQuickWindow window;
        window.resize(1000, 200);
        outer->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        settle(6);

        auto *bar = outer->findChild<QQuickItem *>(
            QStringLiteral("fixtureHeader"));
        QVERIFY(bar != nullptr);
        QVERIFY(bar->isVisible());
        const auto fits = [&](int width) -> QString {
            outer->setWidth(width);
            QTest::qWait(80);
            settle(6);
            auto *row = bar->findChild<QQuickItem *>(
                QStringLiteral("callHeaderBarRow"));
            if (!row)
                return QStringLiteral("row missing");
            const qreal left = row->mapToItem(outer, QPointF(0, 0)).x();
            const qreal right =
                row->mapToItem(outer, QPointF(row->width(), 0)).x();
            if (left < -0.5 || right > width + 0.5)
                return QStringLiteral("at %1 px the control row spans %2..%3 "
                                      "(narrow=%4 need=%5)")
                    .arg(width).arg(left).arg(right)
                    .arg(bar->property("narrow").toBool())
                    .arg(bar->property("fullNeed").toDouble());
            return QString();
        };
        QCOMPARE(fits(1000), QString());
        QVERIFY(!bar->property("narrow").toBool());
        const QString narrowResult = fits(420);
        QVERIFY2(narrowResult.isEmpty(), qPrintable(narrowResult));
        QVERIFY(bar->property("narrow").toBool());
        QCOMPARE(fits(1000), QString());
        QVERIFY(!bar->property("narrow").toBool());
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
        const double want = bubbles->property("implicitWidth").toDouble();
        QVERIFY2(want > 0.0, "the strip reports no implicit width at all");

        auto *strip =
            outer->findChild<QQuickItem *>(QStringLiteral("callSpeakerBubbles"));
        QVERIFY(strip != nullptr);
        QCOMPARE(strip->property("count").toInt(), 2);

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

    void everyCallControllerMemberQmlUsesActuallyExists()
    {
        auto members = [&](const QString &header) {
            const QString src = read(QStringLiteral(QML_DIR "/../src/calls/")
                                     + header);
            QSet<QString> out;
            QRegularExpression prop(
                QStringLiteral("Q_PROPERTY\\s*\\([^)]*?([A-Za-z_][A-Za-z0-9_]*)"
                               "\\s+(?:READ|MEMBER)"));
            auto pi = prop.globalMatch(src);
            while (pi.hasNext())
                out.insert(pi.next().captured(1));
            QRegularExpression inv(
                QStringLiteral("Q_INVOKABLE[^;{]*?([A-Za-z_][A-Za-z0-9_]*)"
                               "\\s*\\("));
            auto ii = inv.globalMatch(src);
            while (ii.hasNext())
                out.insert(ii.next().captured(1));
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

        QCOMPARE(picker->property("screenCount").toInt(), 0);
        QCOMPARE(picker->property("windowCount").toInt(), 0);

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

        const QVariantMap unknownApp{
            { QStringLiteral("index"), -1 },
            { QStringLiteral("windowHandle"), quint64(4662) },
            { QStringLiteral("name"), QStringLiteral("Untitled - Notepad") },
            { QStringLiteral("application"), QString() },
            { QStringLiteral("geometry"), QStringLiteral("800 x 600") },
        };
        QCOMPARE(label("primaryLabel", unknownApp),
                 QStringLiteral("Untitled - Notepad"));

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

        QTRY_COMPARE(grid->property("count").toInt(), 2);
        QCOMPARE(picker->property("tab").toString(), QStringLiteral("screens"));

        clickItem(window, grid, 0);
        QCOMPARE(picker->property("selected").toInt(), 0);
        clickItem(window, grid, 1);
        QCOMPARE(picker->property("selected").toInt(), 1);

        auto *applicationsTab = findVisualChild(
            picker, QStringLiteral("shareTabs_applications"));
        QVERIFY2(applicationsTab != nullptr,
                 "the Applications tab has no objectName to find");
        clickCentre(window, applicationsTab);
        QTRY_COMPARE(picker->property("tab").toString(),
                     QStringLiteral("applications"));
        QTRY_COMPARE(grid->property("count").toInt(), 2);

        QCOMPARE(picker->property("selected").toInt(), 2);

        clickItem(window, grid, 1);
        QCOMPARE(picker->property("selected").toInt(), 3);
    }

    void thePreviewImageIsNeverRequestedOffWindows()
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
)"), QUrl(QStringLiteral("qrc:/sharepickerpreviewtest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *window = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(window != nullptr);
        auto *picker = owner->property("picker").value<QObject *>();
        QVERIFY(picker != nullptr);

        const QVariantList screenRows = pickerRows({
            { QStringLiteral("Screen A"), QString(), 0 },
        });
        picker->setProperty("sources", screenRows);
        QMetaObject::invokeMethod(picker, "open");
        QTRY_VERIFY(picker->property("visible").toBool());
        QCoreApplication::processEvents();

        auto *screenPreview = findVisualChild(
            picker, QStringLiteral("sharePickerPreview_s0"));
        QVERIFY2(screenPreview != nullptr,
                 "the screen tile's preview Image has no matching objectName");
        QVERIFY2(screenPreview->property("source").toUrl().isEmpty(),
                 "a screen tile still asks image://lightning-sharesource off "
                 "Windows");

        QMetaObject::invokeMethod(picker, "close");

        const QVariantList windowRows = pickerRows({
            { QStringLiteral("Window A"), QStringLiteral("Brave Browser"),
              4660 },
        });
        picker->setProperty("sources", windowRows);
        picker->setProperty("tab", QStringLiteral("applications"));
        QMetaObject::invokeMethod(picker, "open");
        QTRY_VERIFY(picker->property("visible").toBool());
        QCoreApplication::processEvents();

        auto *windowPreview = findVisualChild(
            picker, QStringLiteral("sharePickerPreview_w4660"));
        QVERIFY2(windowPreview != nullptr,
                 "the window tile's preview Image has no matching objectName");
        QVERIFY2(windowPreview->property("source").toUrl().isEmpty(),
                 "a window tile still asks image://lightning-sharesource off "
                 "Windows");
    }

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

        auto chosenName = [picker, &rows]() {
            const int at = picker->property("selected").toInt();
            if (at < 0 || at >= rows.size())
                return QStringLiteral("<out of range>");
            return rows.at(at).toMap().value(QStringLiteral("name")).toString();
        };

        QTRY_COMPARE(grid->property("count").toInt(), 2);
        clickItem(window, grid, 1);
        QCOMPARE(chosenName(), QStringLiteral("Screen B"));
        QCOMPARE(picker->property("selected").toInt(), 2);

        auto *applicationsTab = findVisualChild(
            picker, QStringLiteral("shareTabs_applications"));
        QVERIFY(applicationsTab != nullptr);
        clickCentre(window, applicationsTab);
        QTRY_COMPARE(grid->property("count").toInt(), 3);

        clickItem(window, grid, 1);
        QCOMPARE(chosenName(), QStringLiteral("Window B"));
        QCOMPARE(picker->property("selected").toInt(), 3);

        clickItem(window, grid, 2);
        QCOMPARE(chosenName(), QStringLiteral("Window C"));
        QCOMPARE(picker->property("selected").toInt(), 4);

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

        const QString qml = normalized(
            code(read(QStringLiteral(QML_DIR "/ScreenSharePicker.qml"))));
        QVERIFY2(!qml.isEmpty(), "ScreenSharePicker.qml did not read");
        QVERIFY2(qml.contains(QStringLiteral("var chosen = root.selected;")),
                 "the confirm no longer sends the selected SOURCE index");
        QVERIFY2(qml.contains(
                     QStringLiteral("chooseScreenShareSource(chosen)")),
                 "the confirm no longer hands the controller the chosen index");
    }

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
            QVERIFY2(!src.contains(QLatin1String("if (pipewireNodeId < 0)\n")),
                     "a bare node-id refusal survives alongside the fixed one");
        }
    }

    void theInRoomStageStandsDownForPictureInPicture()
    {
        const QString src = read(QStringLiteral(QML_DIR "/TimelinePane.qml"));
        QVERIFY2(!src.isEmpty(), "TimelinePane.qml is gone");
        const int at = src.indexOf(
            QStringLiteral("readonly property bool callStageOwnsColumn:"));
        QVERIFY2(at > 0, "callStageOwnsColumn is gone — re-anchor this test "
                         "rather than deleting it");
        const int end = src.indexOf(QStringLiteral("\n\n"), at);
        const QString expr = src.mid(at, (end < 0 ? src.size() : end) - at);
        QVERIFY2(expr.contains(QStringLiteral("pictureInPicture")),
                 "the in-room call stage is built without consulting "
                 "picture-in-picture: popping the call out while looking at "
                 "the room gives two surfaces for one track, and the sink "
                 "goes to whichever attached last");

        const int hostAt = src.indexOf(
            QStringLiteral("objectName: \"timelineCallStageHost\""));
        QVERIFY2(hostAt > 0, "the call stage host is gone");
        const QString host = src.mid(hostAt, 1200);
        QVERIFY2(host.contains(QStringLiteral("active: root.callStageOwnsColumn")),
                 "the stage host no longer keys on callStageOwnsColumn, so "
                 "the clause above guards nothing");
    }

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

    void theCallControlsMayShrinkRatherThanOverflow()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY2(!stage.isEmpty(), "CallStage.qml is missing");
        const QString code = normalized(stage);

        QVERIFY2(!code.contains(QStringLiteral(
                     "Layout.minimumWidth: implicitWidth")),
                 "the controls host has a floor at its implicit width again, "
                 "so a row too narrow for it overflows instead of shrinking "
                 "and the end of the control row leaves the window");

        QVERIFY2(code.contains(QStringLiteral(
                     "compact: root.collapsed || controlsHost.cramped")),
                 "the control bar no longer goes compact when the room it is "
                 "given is too small for it");
    }

    void aVideoSurfaceOutsideTheCallExplainsTheSoftwareRendererToo()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient

Item {
    width: 640
    height: 360
    property alias big: big
    property alias small: small
    property bool playing: true
    SoftwareVideoNotice { id: big; anchors.fill: parent; shown: parent.playing }
    SoftwareVideoNotice { id: small; width: 120; height: 40 }
}
)"), QUrl(QStringLiteral("qrc:/softwarevideonoticetest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner != nullptr);
        QQuickWindow window;
        window.resize(640, 360);
        auto *rootItem = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(rootItem);
        rootItem->setParentItem(window.contentItem());
        auto *big = owner->property("big").value<QQuickItem *>();
        auto *small = owner->property("small").value<QQuickItem *>();
        QVERIFY(big && small);

        QVERIFY(!controller.softwareRenderer());
        QVERIFY2(!big->isVisible(),
                 "the notice covers video on a machine that draws it");
        controller.setSoftwareRenderer(true);
        QTRY_VERIFY(big->isVisible());
        auto *text = big->findChild<QQuickItem *>(
            QStringLiteral("softwareVideoNoticeText"));
        QVERIFY(text && text->isVisible());
        QVERIFY(text->property("text").toString().contains(
            QStringLiteral("graphics acceleration")));
        auto *plate = big->findChild<QQuickItem *>(
            QStringLiteral("softwareVideoNoticePlate"));
        QVERIFY(plate);
        const QRectF inBig = plate->mapRectToItem(big, plate->boundingRect());
        QVERIFY2(QRectF(0, 0, big->width(), big->height()).contains(inBig),
                 "the notice draws outside the video it explains");

        owner->setProperty("playing", false);
        QTRY_VERIFY(!big->isVisible());

        QVERIFY(small->isVisible());
        auto *smallText = small->findChild<QQuickItem *>(
            QStringLiteral("softwareVideoNoticeText"));
        QVERIFY(smallText && !smallText->isVisible());
        auto *smallPlate = small->findChild<QQuickItem *>(
            QStringLiteral("softwareVideoNoticePlate"));
        QVERIFY(smallPlate);
        QVERIFY(smallPlate->width() <= small->width());
        QVERIFY(smallPlate->height() <= small->height());
    }

    void theVideoCardAndItsViewerExplainTheSoftwareRenderer()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient

Item {
    width: 640
    height: 360
    property alias card: card
    property alias viewer: viewer
    property QtObject fakePlayer: QtObject {}
    VideoPlayerCard {
        id: card
        width: 480
        height: 270
        mediaKey: "fixture-software-video"
        ownerKey: "fixture-owner"
    }
    VideoViewerOverlay { id: viewer }
}
)"), QUrl(QStringLiteral("qrc:/videosoftwarenoticetest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner != nullptr);
        auto *card = owner->property("card").value<QQuickItem *>();
        auto *viewer = owner->property("viewer").value<QObject *>();
        QVERIFY(card && viewer);

        auto *cardNotice = card->findChild<QQuickItem *>(
            QStringLiteral("videoCardSoftwareNotice"));
        QVERIFY2(cardNotice != nullptr,
                 "the inline video card has no software-renderer notice");
        QCOMPARE(cardNotice->width(), card->width());
        QCOMPARE(cardNotice->height(), card->height());
        const auto agrees = [&] {
            return cardNotice->property("shown").toBool()
                == card->property("pictureExpected").toBool();
        };
        QVERIFY(agrees());
        QObject *player = nullptr;
        for (QObject *o : card->findChildren<QObject *>()) {
            if (o->inherits("QMediaPlayer"))
                player = o;
        }
        QVERIFY2(player != nullptr, "the card has no MediaPlayer");
        card->setProperty("fetchState", QStringLiteral("idle"));
        player->setProperty("source",
                            QUrl::fromLocalFile(QStringLiteral(
                                QML_DIR "/../data/sounds/connected.wav")));
        if (!QTest::qWaitFor(
                [card] { return card->property("pictureExpected").toBool(); },
                10000))
            QSKIP("this Qt Multimedia could not load the fixture source");
        QVERIFY(agrees());
        QVERIFY2(!cardNotice->property("active").toBool(),
                 "the notice covers video on a machine that draws it");
        controller.setSoftwareRenderer(true);
        QVERIFY2(cardNotice->property("active").toBool(),
                 "a card expecting a picture on the software renderer says "
                 "nothing");
        controller.setSoftwareRenderer(false);

        card->setProperty("fetchState", QStringLiteral("failed"));
        QVERIFY(!card->property("pictureExpected").toBool());
        QVERIFY(agrees());
        controller.setSoftwareRenderer(true);
        QVERIFY2(!cardNotice->property("active").toBool(),
                 "the notice speaks over a card that is showing a failure");

        QObject *viewerNotice = nullptr;
        for (QObject *o : viewer->findChildren<QObject *>()) {
            if (o->objectName() == QLatin1String("videoViewerSoftwareNotice"))
                viewerNotice = o;
        }
        if (!viewerNotice) {
            if (auto *content = viewer->property("contentItem")
                                    .value<QQuickItem *>()) {
                viewerNotice = content->findChild<QQuickItem *>(
                    QStringLiteral("videoViewerSoftwareNotice"));
            }
        }
        QVERIFY2(viewerNotice != nullptr,
                 "the full-window video viewer has no software-renderer "
                 "notice");
        QVERIFY(!viewerNotice->property("active").toBool());
        viewer->setProperty("player", QVariant::fromValue(
                                          owner->property("fakePlayer")
                                              .value<QObject *>()));
        QVERIFY(viewerNotice->property("active").toBool());
        controller.setSoftwareRenderer(false);
        QVERIFY2(!viewerNotice->property("active").toBool(),
                 "the notice covers video on a machine that draws it");
        viewer->setProperty("player", QVariant());
    }

    void theComposerVideoChipKeepsItsGlyphOnTheSoftwareRenderer()
    {
        const QString src = read(QStringLiteral(QML_DIR "/MessageComposerBar.qml"));
        const int loader = src.indexOf(QStringLiteral("id: chipVideoLoader"));
        QVERIFY2(loader > 0, "the composer's video chip Loader moved");
        const int activeAt = src.indexOf(QStringLiteral("active:"), loader);
        const int end = src.indexOf(QStringLiteral("property bool posterFailed"),
                                    activeAt);
        QVERIFY(activeAt > 0 && end > activeAt);
        const QString active = src.mid(activeAt, end - activeAt);
        QVERIFY2(active.contains(QStringLiteral("app.softwareRenderer")),
                 qPrintable(QStringLiteral("the chip's poster player ignores "
                                           "the software renderer: %1")
                                .arg(active)));
        QVERIFY2(src.contains(QStringLiteral("!chipVideoLoader.active")),
                 "the videocam glyph no longer shows for an inactive poster");
    }

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

        QQuickWindow window;
        window.resize(900, 600);
        root->setParentItem(window.contentItem());

        auto *notice = root->findChild<QQuickItem *>(
            QStringLiteral("callSoftwareRendererNotice"));
        QVERIFY2(notice != nullptr, "the stage has no software-renderer notice");

        QVERIFY(!controller.softwareRenderer());
        QTest::qWait(50);
        QVERIFY2(!notice->property("visible").toBool(),
                 "the notice is shown on a machine that renders video fine");
        QCOMPARE(notice->property("implicitHeight").toReal(), 0.0);

        controller.setSoftwareRenderer(true);
        QTest::qWait(50);
        QVERIFY2(notice->property("visible").toBool(),
                 "a call that cannot draw video shows the user nothing but an "
                 "empty rectangle and no explanation");
        QVERIFY2(notice->property("implicitHeight").toReal() > 0.0,
                 "the notice is visible but has no height");
    }

    void theVideoTilesConsultTheRendererBeforeShowingAFrame()
    {
        for (const auto *file : { QML_DIR "/CallParticipantTile.qml",
                                  QML_DIR "/CallShareTile.qml" }) {
            const QString src = read(QString::fromUtf8(file));
            QVERIFY2(!src.isEmpty(), file);
            const QString code = normalized(src);
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
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr,
                 "CallStage.qml failed to load — see the qWarning above for "
                 "the property or type that does not exist");

        QVERIFY(root->findChild<QObject *>(QStringLiteral("callStageControls"))
                != nullptr);
    }

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
        auto *row = qobject_cast<QQuickItem *>(host->parentItem());
        QVERIFY(row != nullptr);

        for (const int width : { 1100, 900, 760, 700, 640, 560, 480 }) {
            root->setWidth(width);
            settle(4);
            QTest::qWait(80);
            settle(4);

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

            auto *hangUp = root->findChild<QQuickItem *>(
                QStringLiteral("callBarHangUpButton"));
            QVERIFY2(hangUp != nullptr,
                     qPrintable(QStringLiteral(
                         "no hang-up button at a %1 px stage").arg(width)));
            const QPointF hangRight = hangUp->mapToItem(
                nullptr, QPointF(hangUp->width(), 0));
            QVERIFY2(hangRight.x() <= stageRight.x() + 0.5,
                     qPrintable(QStringLiteral(
                         "at a %1 px stage the hang-up button ends at x=%2, "
                         "past the stage's own right edge at x=%3")
                                    .arg(width)
                                    .arg(hangRight.x())
                                    .arg(stageRight.x())));
        }
    }

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

        resize(600);
        QVERIFY2(host->property("cramped").toBool(),
                 "a 600 px panel did not compact the dock, so the rest of "
                 "this case cannot test the way back");
        QVERIFY2(host->width() + 0.5 < expanded,
                 "the dock did not actually shrink");

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

        QVERIFY2(src.contains(QStringLiteral("id: fullScreenHover")),
                 "there is no pointer-movement handler over the share, so "
                 "nothing can reveal the chrome again");
        QVERIFY2(!src.contains(QStringLiteral("fullScreenDockHover")),
                 "the chrome is still held open by a hover on the dock; a "
                 "pointer resting there never leaves on a single monitor");

        QVERIFY2(!src.contains(QStringLiteral("fullScreenIdleTimer.restart()")),
                 "the idle timer is restarted somewhere; the count is what "
                 "carries the idle budget, so re-phasing alone leaves the "
                 "chrome retiring on the next tick");
        QVERIFY2(src.count(QStringLiteral("idleTicks = 0")) >= 2,
                 "something wakes the chrome without clearing the idle "
                 "count, so it retires again immediately");

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

        QCOMPARE(root->property("shouldShow").toBool(), false);

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

        QVERIFY(controller.calls()->rejectIncoming());
        QTRY_COMPARE_WITH_TIMEOUT(root->property("shouldShow").toBool(),
                                  false, 3000);
    }

    void anRtcRingOffersJoinInsteadOfAnAnswerThatCannotWork()
    {

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

        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.notificationType = QStringLiteral("ring");
        notify.mentionsRoom = true;
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

        QCOMPARE(controller.calls()->state(),
                 CallController::State::Ringing);
        QTRY_COMPARE_WITH_TIMEOUT(root->property("shouldShow").toBool(),
                                  true, 3000);
    }

    void theRingCardJoinsThroughTheOneSharedGate()
    {
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
        QVERIFY(norm.contains(QStringLiteral("case \"no_transport\":")));
        QVERIFY(norm.contains(QStringLiteral("case \"session_closed\":")));
        const int joinAt = norm.indexOf(
            QStringLiteral("objectName: \"incomingCallPromptJoin\""));
        const int acceptAt = norm.indexOf(
            QStringLiteral("objectName: \"incomingCallPromptAccept\""));
        QVERIFY(joinAt >= 0);
        QVERIFY(acceptAt > joinAt);
        QVERIFY2(!norm.mid(joinAt, acceptAt - joinAt)
                      .contains(QStringLiteral("rejectIncoming")),
                 "the Join button also declines the call");
        QVERIFY2(!norm.contains(QStringLiteral("app.calls.placeCall")),
                 "the ring card can place a legacy call");
    }

    void theFullScreenWindowOpensOnTheApplicationsOwnScreen()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const QString stageCode = normalized(code(stage));
        QVERIFY(stageCode.contains(QStringLiteral("objectName: \"callStage\"")));

        const int place =
            stageCode.indexOf(QStringLiteral("placeOnThisApplicationsScreen()"));
        const int show =
            stageCode.indexOf(QStringLiteral("fullScreenWindow.showFullScreen()"));
        QVERIFY2(place >= 0, "nothing places the full-screen window");
        QVERIFY2(show > place,
                 "the window is shown before it is placed on a screen");

        QVERIFY(stageCode.contains(QStringLiteral("var target = root.Screen")));
        QVERIFY(stageCode.contains(
            QStringLiteral("fullScreenWindow.screen = target")));
        QVERIFY2(stageCode.contains(
                     QStringLiteral("fullScreenWindow.x = target.virtualX")),
                 "the window's geometry is not moved onto the target screen");
        QVERIFY(stageCode.contains(
            QStringLiteral("fullScreenWindow.y = target.virtualY")));

        QVERIFY2(!code(stage).contains(QStringLiteral("visibility:")),
                 "the full-screen window binds Window.visibility");
        QVERIFY(stageCode.contains(
            QStringLiteral("onClosing: root.exitFullScreen()")));
        QVERIFY2(!stageCode.contains(QStringLiteral("close.accepted = false")),
                 "the full-screen window refuses its close event");
        QVERIFY2(!stageCode.contains(QStringLiteral("Qt.application.screens")),
                 "the full-screen window picks a screen from a global list "
                 "rather than from the application's own window");
    }

    void theRaiseHandControlIsOnTheWireAndSaysNothingElse()
    {
        const QString bar = normalized(
            read(QStringLiteral(QML_DIR "/CallHeaderBar.qml")));
        QVERIFY(!bar.isEmpty());
        QVERIFY(bar.contains(
            QStringLiteral("objectName: \"callBarHandButton\"")));
        QVERIFY2(!bar.contains(QStringLiteral("only shown on this device")),
                 "the raise-hand control still claims no peer can see it, "
                 "which stopped being true when it went on the wire");
        QVERIFY(bar.contains(QStringLiteral("iconName: \"front_hand\"")));

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

    void theCallReactionControlSendsElementCallsOwnPairs()
    {
        const QString bar = normalized(
            read(QStringLiteral(QML_DIR "/CallHeaderBar.qml")));
        QVERIFY(!bar.isEmpty());
        QVERIFY2(bar.contains(
                     QStringLiteral("objectName: \"callBarReactButton\"")),
                 "there is no control for sending a call reaction");
        QVERIFY(bar.contains(QStringLiteral("iconName: \"add_reaction\"")));
        QVERIFY2(bar.contains(
                     QStringLiteral("app.groupCall.sendCallReaction(")),
                 "the reaction control does not reach the controller");

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

    void theCallReactionMoreChoiceUsesTheUnicodePickerAndSameSendPath()
    {
        const QString bar = normalized(
            read(QStringLiteral(QML_DIR "/CallHeaderBar.qml")));
        const QString picker = normalized(
            read(QStringLiteral(QML_DIR "/EmojiPicker.qml")));
        QVERIFY(bar.contains(QStringLiteral("objectName: \"callReactionMore\"")));
        QVERIFY(bar.contains(QStringLiteral("EmojiPicker {")));
        QVERIFY(bar.contains(QStringLiteral("mode: \"reaction\"")));
        QVERIFY(bar.contains(QStringLiteral("allowCustomEmoji: false")));
        QVERIFY(bar.contains(QStringLiteral("callEmojiPicker.openExpanded = true")));
        QVERIFY(bar.contains(QStringLiteral("callEmojiPicker.open()")));
        QVERIFY(bar.contains(QStringLiteral(
            "onEmojiChosen: (emoji) => app.groupCall.sendCallReaction(emoji, \"\")")));
        QVERIFY(picker.contains(QStringLiteral("app.emojiCatalog.recordUse(emoji)")));
    }

    void theCallReactionWireFormatIsElementCallsOwn()
    {
        const QString rtc =
            read(QStringLiteral(QML_DIR "/../rust/src/rtc.rs"));
        QVERIFY(!rtc.isEmpty());
        QVERIFY2(rtc.contains(QStringLiteral(
                     "#[ruma_event(type = \"io.element.call.reaction\", "
                     "kind = MessageLike)]")),
                 "the call reaction is not element-call's event type");
        QVERIFY2(rtc.contains(QStringLiteral("pub relates_to: Reference,")),
                 "a call reaction's relation is not typed as an m.reference");
        QVERIFY(rtc.contains(QStringLiteral("relates_to: Reference::new(")));

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

        QVERIFY2(!block.contains(QStringLiteral("Timer")),
                 "the tile runs its own reaction timer");

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
        QVERIFY(body.contains(QStringLiteral("ownMembershipEventId(")));
    }

    void exactlyOneSurfaceOwnsTheMediaControls()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        QVERIFY2(!stage.contains(QStringLiteral("CallControlBar {")),
                 "the call stage instantiates a control bar again");
        QVERIFY2(!QFile::exists(QStringLiteral(QML_DIR "/CallControlBar.qml")),
                 "the dead CallControlBar.qml is back in the tree");

        QVERIFY(stage.contains(QStringLiteral("CallHeaderBar {")));
        QVERIFY(stage.contains(QStringLiteral("placement: \"dock\"")));

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

    void theSpotlightRendersVideoRatherThanDescribingIt()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const int surface = stage.indexOf(QStringLiteral("id: focusedSurface"));
        QVERIFY2(surface >= 0, "the focused surface is no longer defined once");
        const QString block = stage.mid(surface, 6000);
        QVERIFY2(block.contains(QStringLiteral("CallShareTile {")),
                 "the spotlight cannot show a screen share");
        QVERIFY2(block.contains(QStringLiteral("CallParticipantTile {")),
                 "the spotlight cannot show a pinned participant");
        QVERIFY(block.contains(
            QStringLiteral("root.stageState.spotlightShareId")));
        QVERIFY(block.contains(
            QStringLiteral("root.stageState.pinnedIdentity")));
        const int spotlight =
            stage.indexOf(QStringLiteral("objectName: \"callSpotlight\""));
        QVERIFY(spotlight >= 0);
        QVERIFY2(stage.mid(spotlight, 1200)
                     .contains(QStringLiteral("sourceComponent: focusedSurface")),
                 "the spotlight does not host the shared focused surface");
        QVERIFY(!stage.contains(
            QStringLiteral("Someone is sharing their screen")));
    }

    void aScreenShareAndACameraAreRoutedAsSeparateTracks()
    {
        const QString tile =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        QVERIFY(!tile.isEmpty());
        QVERIFY(tile.contains(QStringLiteral("property string mediaKind")));
        QVERIFY(tile.contains(QStringLiteral("attachScreenSink(")));
        QVERIFY(tile.contains(QStringLiteral("attachLocalScreenSink(")));
        QVERIFY(tile.contains(
            QStringLiteral("app.groupCall.detachSink(output.videoSink)")));
        QVERIFY(tile.contains(QStringLiteral("onActiveTrackKeyChanged:")));
        QVERIFY(tile.contains(QStringLiteral("VideoOutput.PreserveAspectFit")));
    }

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
        QVERIFY(bubbles.contains(QStringLiteral("required property bool speaking")));
        QVERIFY(bubbles.contains(QStringLiteral("opacity: bubble.speaking ? 1 : 0")));
        QVERIFY(bubbles.contains(QStringLiteral("border.color: AppTheme.success")));
        QVERIFY(bubbles.contains(QStringLiteral("name: bubble.displayName")));
        QVERIFY2(!bubbles.contains(QStringLiteral("modelData")),
                 "the bubble strip is bound to a JS array again");
        QVERIFY(bubbles.contains(
            QStringLiteral("property var model: app.groupCall.participantModel")));
    }

    void stoppingOneTrackNamesItRatherThanTakingTheLastOne()
    {
        QFile file(QStringLiteral(SRC_DIR "/calls/SfuCallController.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY(source.contains(QStringLiteral("unpublishTrack(m_cameraCid)")));
        QVERIFY(source.contains(QStringLiteral("unpublishTrack(m_screenCid)")));
        QVERIFY2(!source.contains(QStringLiteral(
                     "for (int i = m_publishedTrackIds.size() - 1")),
                 "a track is still stopped by taking the last published one");
    }

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
        QVERIFY(tile.contains(QStringLiteral("readonly property bool _drawsCard")));
        QVERIFY(tile.contains(QStringLiteral("root.focused || root.activeFocus")));
    }

    void theJoinButtonActuallyJoins()
    {
        const QString banner =
            read(QStringLiteral(QML_DIR "/RoomCallBanner.qml"));
        QVERIFY(!banner.isEmpty());
        const int at = banner.indexOf(QStringLiteral("roomCallJoinButton"));
        QVERIFY2(at >= 0, "the Join button is gone");
        QString body = banner.mid(at, 900);
        body.remove(QRegularExpression(QStringLiteral("(?m)^\\s*//.*$")));
        body = normalized(body);
        QVERIFY2(body.contains(QStringLiteral("app.groupCall.join(")),
                 "the Join button does not call join()");
        QVERIFY2(!body.contains(QStringLiteral("onClicked: { }")),
                 "the Join handler is empty again");
    }

    void aGhostMembershipDoesNotHideTheBanner()
    {
        QQmlEngine engine;
        StubApp stub;
        const QString room = QStringLiteral("!r:mock.local");

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

        stub.callStub()->setActive(false);
        QVERIFY2(banner->property("visible").toBool(),
                 "the banner did not come back when this device left the "
                 "call, so `visible` is not tracking the controller");
    }

    void aCallInAnotherRoomLeavesThisRoomsBannerUp()
    {
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

    void theCallPanelSitsAboveTheTimelineRatherThanReplacingIt()
    {
        const QString pane =
            normalized(code(read(QStringLiteral(QML_DIR "/TimelinePane.qml"))));
        QVERIFY(!pane.isEmpty());
        QVERIFY(pane.contains(QStringLiteral("objectName: \"timelineCallStageHost\"")));
        QVERIFY2(pane.contains(QStringLiteral(
                     "readonly property bool callStageOwnsColumn: "
                     "app.groupCall.active && app.groupCall.roomId === "
                     "app.currentRoomId")),
                 "TimelinePane has no single call-stage ownership condition");
        QVERIFY2(pane.contains(QStringLiteral("active: root.callStageOwnsColumn")),
                 "the call stage host does not read the ownership condition");
        QVERIFY2(pane.contains(QStringLiteral(
                     "Layout.preferredHeight: active ? root.callPanelHeight "
                     ": 0")),
                 "the call panel does not take a bounded height");
        QVERIFY2(!pane.contains(QStringLiteral("Layout.fillHeight: active")),
                 "the call stage host is fighting the timeline for the column "
                 "again");
        QVERIFY2(!pane.contains(QStringLiteral("visible: !root.callStageOwnsColumn")),
                 "the timeline still stands down for the call");
        QVERIFY2(!pane.contains(QStringLiteral(
                     "visible: app.currentRoomId !== \"\" && "
                     "!root.callStageOwnsColumn")),
                 "the composer still stands down for the call");
    }

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
        QVERIFY2(block.contains(QStringLiteral(
                     "root.callPanelUserHeight = root.clampCallPanelHeight("
                     "root.callPanelHeight);")),
                 "the divider never commits the released height");
        const QString pane = normalized(code(raw));
        QVERIFY2(!pane.contains(QStringLiteral(
                     "onHeightChanged: root.callPanelUserHeight")),
                 "the panel height is being persisted from a height change, "
                 "which never fires on release");
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(stage.contains(QStringLiteral("signal collapseToggled()")));
        QVERIFY(pane.contains(QStringLiteral(
            "onCollapseToggled: root.callPanelCollapsed = "
            "!root.callPanelCollapsed")));
    }

    void aScreenShareIsATileNotAMode()
    {
        const QString grid = read(QStringLiteral(QML_DIR "/CallTileGrid.qml"));
        const QString share = read(QStringLiteral(QML_DIR "/CallShareTile.qml"));
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY2(!grid.isEmpty(), "CallTileGrid.qml is missing");
        QVERIFY2(!share.isEmpty(), "CallShareTile.qml is missing");
        QVERIFY(!stage.isEmpty());

        QVERIFY(grid.contains(QStringLiteral("model: root.shareModel")));
        QVERIFY(grid.contains(QStringLiteral("model: root.participantModel")));
        QVERIFY2(grid.contains(QStringLiteral(
                     "root.cellX(root.shareCount + personCell.index)")),
                 "the grid does not place people after the shares");
        QVERIFY(grid.contains(QStringLiteral("mediaKind: \"camera\"")));
        QVERIFY(share.contains(QStringLiteral("attachScreenSink(")));
        QVERIFY(share.contains(QStringLiteral("attachLocalScreenSink(")));
        QVERIFY(share.contains(
            QStringLiteral("app.groupCall.detachSink(output.videoSink)")));
        QVERIFY(share.contains(QStringLiteral("VideoOutput.PreserveAspectFit")));
        QVERIFY2(!stage.contains(QStringLiteral("sharingPerson")),
                 "the stage still resolves 'the one person who is sharing'");
        QVERIFY2(!stage.contains(QStringLiteral("spotlightKind")),
                 "the spotlight still has a single track kind, which is the "
                 "one-share assumption in another spelling");
    }

    void multipleSimultaneousSharesEachGetTheirOwnSurface()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        const QString grid = read(QStringLiteral(QML_DIR "/CallTileGrid.qml"));
        QVERIFY(!stage.isEmpty());
        QVERIFY(!grid.isEmpty());
        QVERIFY(stage.contains(QStringLiteral("shareModel: root.shareModel")));
        QVERIFY(stage.contains(
            QStringLiteral("readonly property var shareModel: "
                           "app.groupCall.shareModel")));
        QVERIFY2(!stage.contains(QStringLiteral("people[i].screenSharing")),
                 "the stage scans participants for a sharer again");
        QVERIFY2(!stage.contains(QStringLiteral("readonly property var people")),
                 "the stage rebuilt the participant JS array");
        QVERIFY(normalized(stage).contains(QStringLiteral(
            "stripShare.shareId !== root.stageState.spotlightShareId")));
    }

    void aLiveShareIsAlwaysReachableAgain()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const QString norm = normalized(stage);

        QVERIFY2(norm.contains(
                     QStringLiteral("objectName: \"callRestoreShareButton\"")),
                 "there is no control that restores a dismissed share");
        QVERIFY(norm.contains(
            QStringLiteral("root.stageState.restorableShareAvailable")));
        QVERIFY(norm.contains(
            QStringLiteral("onClicked: root.stageState.restoreAllShares()")));

        QVERIFY(norm.contains(QStringLiteral("root.stageState.restoreShare(")));

        QVERIFY(norm.contains(QStringLiteral(
            "root.stageState.dismissShare(root.stageState.spotlightShareId);")));
        QVERIFY(norm.contains(QStringLiteral("root.stageState.clearPin();")));
        const int back =
            norm.indexOf(QStringLiteral("objectName: \"callBackToGridButton\""));
        QVERIFY2(back >= 0, "the Back to grid control is gone");
        QVERIFY2(norm.mid(back, 400)
                     .contains(QStringLiteral("onClicked: root.leaveSpotlight()")),
                 "Back to grid does not go through the dismiss path");

        const QString stageCode = code(stage);
        QVERIFY(stageCode.contains(QStringLiteral("objectName: \"callStage\"")));
        QVERIFY2(!stageCode.contains(QStringLiteral("layoutMode")),
                 "the terminal layout-mode latch is back on the call stage");
        QVERIFY2(!stageCode.contains(QStringLiteral("setLayoutPreference")),
                 "the stage writes a layout preference with no writer back");
        QVERIFY2(!stageCode.contains(QStringLiteral("layoutPreference = \"")),
                 "the stage assigns a layout preference directly");
    }

    void theSpeakingRingReadsALevelNotABoolean()
    {
        const QString tile =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        const QString grid = read(QStringLiteral(QML_DIR "/CallTileGrid.qml"));
        QVERIFY(!tile.isEmpty());
        QVERIFY(!grid.isEmpty());
        const QString norm = normalized(tile);

        QVERIFY(tile.contains(QStringLiteral("property real speakingLevel")));
        QVERIFY2(norm.contains(QStringLiteral(
                     "readonly property real ringTarget: root.speaking ? 3 + 6 "
                     "* Math.max(0, Math.min(1, root.speakingLevel)) : 0")),
                 "the ring does not read the level");
        QVERIFY2(norm.contains(QStringLiteral(
                     "ringMotion.duration = root.ringTarget > root.ringGap ? "
                     "60 : 220")),
                 "the ring has no attack/release asymmetry");
        QVERIFY2(!norm.contains(QStringLiteral("speakingLevel: root.speaking ?")),
                 "a level is being fabricated from the speaking boolean");
        QVERIFY2(!tile.contains(QStringLiteral("scale: root.speaking")),
                 "the speaking cue scales the avatar again");
        QVERIFY(norm.contains(QStringLiteral("width: parent.width + 2 * root.ringGap")));
        QVERIFY(grid.contains(
            QStringLiteral("speakingLevel: personCell.speakingLevel")));
    }

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
        const QString strip =
            normalized(read(QStringLiteral(QML_DIR "/VoiceConnectedBar.qml")));
        QVERIFY(!strip.isEmpty());
        QVERIFY2(strip.contains(QStringLiteral(
                     "app.groupCall.roomId === app.currentRoomId")),
                 "the Voice Connected strip does not exclude the call's own "
                 "room");
    }

    void noVideoSurfaceEverReleasesARouteByKey()
    {
        QDir dir(QStringLiteral(QML_DIR));
        const QStringList files =
            dir.entryList({ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY(!files.isEmpty());

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
            int at = 0;
            while ((at = body.indexOf(QStringLiteral("detachSink("), at)) >= 0) {
                at += 11;
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
        QVERIFY(read(dir.filePath(QStringLiteral("CallParticipantTile.qml")))
                    .contains(QStringLiteral("onActiveTrackKeyChanged:")));
        QVERIFY(read(dir.filePath(QStringLiteral("CallShareTile.qml")))
                    .contains(QStringLiteral("onTrackKeyChanged:")));
    }

    void theRouterRefusesAReleaseFromASupersededSurface()
    {
#ifndef HAVE_LIGHTNING_WEBRTC
        QSKIP("built without the SFU media engine");
#else
        SfuVideoRouter router;
        auto first = std::make_unique<QVideoSink>();
        auto second = std::make_unique<QVideoSink>();
        const QString key = QStringLiteral("TR_share_a");

        router.attachSink(key, first.get());
        QVERIFY(router.watchedBy(key, first.get()));

        router.attachSink(key, second.get());
        QVERIFY(router.watchedBy(key, second.get()));

        router.releaseSink(first.get());
        QVERIFY2(router.watching(key), "a dying surface unhooked a live one");
        QVERIFY(router.watchedBy(key, second.get()));

        router.releaseSink(second.get());
        QVERIFY(!router.watching(key));

        router.attachSink(key, first.get());
        router.attachSink(QStringLiteral("PA_alice"), first.get());
        router.attachSink(QStringLiteral("TR_other"), second.get());
        router.releaseSink(first.get());
        QVERIFY(!router.watching(key));
        QVERIFY(!router.watching(QStringLiteral("PA_alice")));
        QVERIFY(router.watching(QStringLiteral("TR_other")));

        router.attachSink(QStringLiteral("TR_other"), nullptr);
        QVERIFY(router.watching(QStringLiteral("TR_other")));

        router.releaseSink(nullptr);
        QVERIFY(router.watching(QStringLiteral("TR_other")));
#endif
    }

    void theVideoRouteSurvivesTheStagesOwnLayoutChanges()
    {
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

        stage->dismissShare(key);
        settle();
        QCOMPARE(stage->spotlightShareId(), QString());
        QVERIFY2(call->isRoutingVideoTo(key),
                 "the grid's share tile never attached a sink");

        stage->restoreShare(key);
        settle();
        QCOMPARE(stage->spotlightShareId(), key);
        QVERIFY2(call->isRoutingVideoTo(key),
                 "the dying grid tile unhooked the spotlight's sink");

        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);
        settle();
        QVERIFY2(call->isRoutingVideoTo(key),
                 "going full screen lost the video route");

        stage->setFullScreen(false);
        settle();
        QVERIFY2(call->isRoutingVideoTo(key),
                 "leaving full screen lost the video route");

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

    void aLiveShareFollowsTheQualitySettings()
    {
        const QString src = read(QStringLiteral(QML_DIR "/../src/calls/SfuCallController.cpp"));
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral(
                     "&SettingsManager::shareQualityChanged")),
                 "the controller no longer listens for share quality changes");
        const int at = src.indexOf(QStringLiteral(
            "void SfuCallController::applyShareQualityToLiveShare()"));
        QVERIFY(at >= 0);
        const QString body = src.mid(at, 1400);
        QVERIFY2(body.contains(QStringLiteral("applyShareQualityLive(")),
                 "a quality change is not applied to the running share");
        QVERIFY2(!body.contains(QStringLiteral("stopScreenShare"))
                     && !body.contains(QStringLiteral("startScreenShare")),
                 "a quality change must not restart the share");
    }

    void theTwoShareQualityLaddersCannotDisagree()
    {
#ifndef HAVE_LIGHTNING_WEBRTC
        QSKIP("built without the SFU media engine");
#else
        SettingsManager settings;
        SfuMediaEngine engine;

        const QList<int> heights = { -100, 1, 480, 719, 720, 721, 900,
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

        const QList<int> rates = { -5, 1, 10, 15, 16, 22, 23, 30, 31,
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
        QMetaObject::invokeMethod(target, "triggered");
        QCoreApplication::processEvents();

        QCOMPARE(settings->shareMaxHeight(), 720);
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(), 3000);
    }

    void theShareOptionsMenuExplainsWhyShareSoundIsOff()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        SfuCallController *call = controller.groupCall();
        QVERIFY(call);
        call->setShareAudioUnavailableForTest(
            SfuCallController::ShareAudioUnavailable::NoPipeWire);
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
)"), QUrl(QStringLiteral("qrc:/sharesoundoffmenutest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner != nullptr);
        auto *menu = owner->property("menu").value<QObject *>();
        QVERIFY(menu != nullptr);
        QMetaObject::invokeMethod(menu, "open");
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(), 3000);

        QVERIFY(!call->shareAudioSupported());
        auto *status = menu->findChild<QQuickItem *>(
            QStringLiteral("shareAudioStatusLabel"));
        auto *system = menu->findChild<QQuickItem *>(
            QStringLiteral("shareAudioMenuItem"));
        auto *apps = menu->findChild<QQuickItem *>(
            QStringLiteral("shareAudioAppsItem"));
        QVERIFY(status && system && apps);
        QVERIFY2(status->isVisible(),
                 "share sound is off and the menu does not say why");
        QVERIFY(status->height() > 0);
        QCOMPARE(status->property("text").toString(), call->shareAudioStatus());
        QVERIFY2(status->property("text").toString().contains(
                     QStringLiteral("PipeWire")),
                 qPrintable(status->property("text").toString()));
        QCOMPARE(status->property("textFormat").toInt(),
                 int(Qt::PlainText));
        QVERIFY2(system->isVisible() && !system->isEnabled(),
                 "the choice is hidden or still usable where nothing can "
                 "capture");
        QVERIFY2(!apps->isVisible(), "chosen apps offered with no capture");
    }

    void theSharePickerExplainsWhyShareSoundIsOff()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        SfuCallController *call = controller.groupCall();
        QVERIFY(call);
        call->setShareAudioUnavailableForTest(
            SfuCallController::ShareAudioUnavailable::MissingPlugin);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 900
    height: 700
    property alias picker: pick
    ScreenSharePicker { id: pick; objectName: "sharePicker" }
}
)"), QUrl(QStringLiteral("qrc:/shareaudiooffpickertest.qml")));
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

        QVERIFY(!call->shareAudioSupported());
        auto *section = picker->findChild<QQuickItem *>(
            QStringLiteral("shareAudioSection"));
        auto *combo = picker->findChild<QQuickItem *>(
            QStringLiteral("shareAudioCombo"));
        auto *status = picker->findChild<QQuickItem *>(
            QStringLiteral("shareAudioStatus"));
        QVERIFY(section && combo && status);
        QVERIFY2(section->isVisible() && status->isVisible(),
                 "share sound is off and the picker says nothing about it");
        QVERIFY2(!combo->isEnabled(),
                 "the sound choice is usable where nothing can capture");
        QCOMPARE(status->property("text").toString(), call->shareAudioStatus());
        QVERIFY2(status->property("text").toString().contains(
                     QStringLiteral("plugins")),
                 qPrintable(status->property("text").toString()));
        QCOMPARE(status->property("textFormat").toInt(), int(Qt::PlainText));
    }

    void theShareSoundChoiceKeepsFollowingTheControllerAfterAPick()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 900
    height: 700
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

        auto *combo = picker->findChild<QQuickItem *>(
            QStringLiteral("shareAudioCombo"));
        QVERIFY2(combo != nullptr, "the share sound choice is gone");
        auto *section = picker->findChild<QQuickItem *>(
            QStringLiteral("shareAudioSection"));
        QVERIFY(section != nullptr);
        if (!combo->isEnabled())
            QSKIP("no excluding capture here, so the sound choice is "
                  "disabled (theSharePickerExplainsWhyShareSoundIsOff)");

        SfuCallController *call = controller.groupCall();
        QVERIFY(call);

        combo->setProperty("currentIndex", 0);
        QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 0));
        QCoreApplication::processEvents();
        QCOMPARE(call->shareAudioMode(), 0);

        call->setShareAudioMode(1);
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentValue").toInt(), 1,
                                  3000);
        call->setShareAudioMode(0);
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentValue").toInt(), 0,
                                  3000);
        call->setShareAudioMode(1);

        auto *status = picker->findChild<QQuickItem *>(
            QStringLiteral("shareAudioStatus"));
        QVERIFY(status != nullptr);
        QCOMPARE(status->property("text").toString(), call->shareAudioStatus());
    }

    void theCallBarOffersNoSoundTheSystemOrChosenApps()
    {
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
    property int asked: 0
    CallShareOptionsMenu { id: m; onChooseAppsRequested: asked = asked + 1 }
}
)"), QUrl(QStringLiteral("qrc:/sharesoundmenutest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner != nullptr);
        auto *menu = owner->property("menu").value<QObject *>();
        QVERIFY(menu != nullptr);
        QMetaObject::invokeMethod(menu, "open");
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(), 3000);

        SfuCallController *call = controller.groupCall();
        QVERIFY(call);
        auto *off = menu->findChild<QQuickItem *>(
            QStringLiteral("shareAudioOffItem"));
        auto *system = menu->findChild<QQuickItem *>(
            QStringLiteral("shareAudioMenuItem"));
        auto *apps = menu->findChild<QQuickItem *>(
            QStringLiteral("shareAudioAppsItem"));
        auto *status = menu->findChild<QQuickItem *>(
            QStringLiteral("shareAudioStatusLabel"));
        QVERIFY2(off && system && apps && status,
                 "a sound row or the status line is gone from the menu");
        if (!call->shareAudioSupported())
            QSKIP("no excluding capture here, so the sound choices are "
                  "disabled (theShareOptionsMenuExplainsWhyShareSoundIsOff)");
        QVERIFY(off->isVisible());
        QCOMPARE(apps->isVisible(), call->shareAudioCanChooseApps());

        QMetaObject::invokeMethod(off, "triggered");
        QCoreApplication::processEvents();
        QCOMPARE(call->shareAudioMode(), 0);
        QVERIFY(off->property("radioSelected").toBool());
        QMetaObject::invokeMethod(system, "triggered");
        QCoreApplication::processEvents();
        QCOMPARE(call->shareAudioMode(), 1);
        QVERIFY(system->property("radioSelected").toBool());
        QCOMPARE(status->property("text").toString(), call->shareAudioStatus());

        if (call->shareAudioCanChooseApps()) {
            QMetaObject::invokeMethod(apps, "triggered");
            QCoreApplication::processEvents();
            QCOMPARE(owner->property("asked").toInt(), 1);
            QCOMPARE(call->shareAudioMode(), 1);
        }
    }

    void theCallBarMutesTheShareSoundWhileASharePlaysIt()
    {
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
)"), QUrl(QStringLiteral("qrc:/sharesoundmutetest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner != nullptr);
        auto *menu = owner->property("menu").value<QObject *>();
        QVERIFY(menu != nullptr);
        QMetaObject::invokeMethod(menu, "open");
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(), 3000);

        SfuCallController *call = controller.groupCall();
        QVERIFY(call);
        auto *mute = menu->findChild<QQuickItem *>(
            QStringLiteral("shareAudioMuteItem"));
        QVERIFY2(mute != nullptr, "the share-sound mute row is missing");
        if (!call->shareAudioSupported())
            QSKIP("no loopback capture element here, so the sound rows are "
                  "absent");
        call->setShareAudioMode(1);
        QVERIFY2(!mute->isVisible(), "a mute row with no share running");
        call->setScreenSharingForTest(true);
        QTRY_VERIFY(mute->isVisible());
        const QString unmutedText = mute->property("text").toString();

        QMetaObject::invokeMethod(mute, "triggered");
        QCoreApplication::processEvents();
        QVERIFY(call->shareAudioMuted());
        QVERIFY2(!call->microphoneMuted(),
                 "the share-sound row muted the microphone");
        QVERIFY2(mute->property("text").toString() != unmutedText,
                 "the row does not say the sound is now muted");
        QMetaObject::invokeMethod(mute, "triggered");
        QCoreApplication::processEvents();
        QVERIFY(!call->shareAudioMuted());

        call->setShareAudioCanChooseAppsForTest(true);
        QVERIFY(call->shareAudioApps().isEmpty());
        call->setShareAudioMode(2);
        QCOMPARE(call->shareAudioMode(), 2);
        QTRY_VERIFY2(!mute->isVisible(),
                     "a mute row for a share that carries no app");
        call->setShareAudioMode(1);
        QTRY_VERIFY(mute->isVisible());

        call->setShareAudioMode(0);
        QTRY_VERIFY(!mute->isVisible());
        call->setScreenSharingForTest(false);
    }

    void theApplicationListTicksWhatIsChosenAndUntickingReachesTheController()
    {
        AppController controller(AppController::MockBackend);
        SfuCallController *call = controller.groupCall();
        QVERIFY(call);
        call->setShareAudioCanChooseAppsForTest(true);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 500
    height: 400
    visible: true
    property alias list: l
    ShareAudioAppList { id: l; width: 460 }
}
)"), QUrl(QStringLiteral("qrc:/shareaudioapplisttest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY(owner != nullptr);
        call->setShareAudioAppChosen(QStringLiteral("NotPlayingApp"),
                                     QStringLiteral("Not Playing App"), true);
        QCOMPARE(call->shareAudioMode(), 2);
        auto *list = owner->property("list").value<QQuickItem *>();
        QVERIFY(list != nullptr);
        std::function<QQuickItem *(QQuickItem *)> walk =
            [&](QQuickItem *at) -> QQuickItem * {
            if (at->objectName() == QLatin1String("shareAudioAppCheck")
                && at->property("appKey").toString()
                       == QLatin1String("notplayingapp"))
                return at;
            for (QQuickItem *child : at->childItems()) {
                if (QQuickItem *hit = walk(child))
                    return hit;
            }
            return nullptr;
        };
        auto findTick = [&]() -> QQuickItem * { return walk(list); };
        QTRY_VERIFY_WITH_TIMEOUT(findTick() != nullptr, 3000);
        QQuickItem *tick = findTick();
        QVERIFY(tick->property("checked").toBool());
        {
            QQmlExpression write(qmlContext(tick), tick,
                                 QStringLiteral("checked = false"));
            write.evaluate();
            QVERIFY2(!write.hasError(), qPrintable(write.error().toString()));
        }
        QMetaObject::invokeMethod(tick, "toggled");
        QCoreApplication::processEvents();
        QVERIFY2(!call->shareAudioApps().contains(QStringLiteral("notplayingapp")),
                 "unticking did not reach the controller");
        QTRY_VERIFY_WITH_TIMEOUT(findTick() == nullptr, 3000);
        call->setShareAudioMode(1);
    }

    void bothShareSurfacesOfferTheSameRungsAndTheSameWarning()
    {
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

        for (const QString &src : { menu, picker }) {
            QVERIFY2(src.contains(QStringLiteral("shareQualityDemanding")),
                     "a share surface decides for itself whether a "
                     "combination is too slow, so the two can disagree");
        }
        QVERIFY2(menu.contains(QStringLiteral("shareQualityDemandingAt")),
                 "the menu no longer marks the individual rate rows, so the "
                 "warning is back to being a block of prose");

        SettingsManager settings;
        struct Row { int h; int fps; bool warn; };
        const QList<Row> rows = {
            { 720, 30, false }, { 720, 60, false },
            { 1080, 15, false }, { 1080, 30, false }, { 1080, 60, false },
            { 1440, 15, false }, { 1440, 30, false }, { 1440, 60, true },
            { 2160, 15, true }, { 2160, 30, true }, { 2160, 60, true },
        };
        for (const Row &r : rows) {
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
        const QString src =
            read(QStringLiteral(SRC_DIR "/calls/SfuCallController.cpp"));
        QVERIFY(!src.isEmpty());

        const int sampled = src.indexOf(QStringLiteral(
            "startsCallForAnnouncement(m_rtc->participants(roomId))"));
        const int published = src.indexOf(QStringLiteral(
            "m_publishOp = m_client->rtcPublishMembership"));
        QVERIFY2(sampled >= 0, "nothing decides whether we started the call");
        QVERIFY2(published >= 0, "the membership publish call site moved");
        QVERIFY2(sampled < published,
                 "the first-arrival check runs AFTER our own membership "
                 "publishes, so it can only ever answer no");

        QVERIFY2(src.contains(QStringLiteral("m_client->rtcNotify(")),
                 "nothing announces the call, so the room still gets no "
                 "timeline row when a call starts");
        const int notify = src.indexOf(QStringLiteral("m_client->rtcNotify("));
        const int typed = src.indexOf(QStringLiteral(
            "announcementTypeFor(m_client->roomInfo(m_roomId))"));
        QVERIFY2(typed >= 0 && typed < notify,
                 "the announcement's type is not decided from the room");
        const QString args = src.mid(notify, 320);
        QVERIFY2(!args.contains(QStringLiteral("\"ring\"")),
                 qPrintable(QStringLiteral("the announcement rings whatever "
                                           "the room: %1").arg(args)));

        QVERIFY2(src.contains(QStringLiteral("m_announceOnPublish = false;")),
                 "the announcement flag is never cleared, so a membership "
                 "refresh would announce the call again");
    }

    void everyPrivateQtHeaderIncludeStaysGuarded()
    {
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

        QVERIFY2(includes > 0,
                 "no private Qt include found anywhere, so this scan is "
                 "checking nothing -- has the header been renamed?");
        QVERIFY2(offenders.isEmpty(),
                 qPrintable(QStringLiteral(
                     "unguarded private Qt header include(s), which build "
                     "here and break the Windows package: %1")
                         .arg(offenders.join(QStringLiteral(", ")))));
    }

    void compactCallBarKeepsShareOptionsReachable()
    {
        AppController controller(AppController::MockBackend);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        QByteArray fixture(R"(
import QtQuick
import QtQuick.Controls
import MatrixClient
ApplicationWindow {
    width: 600
    height: 200
    visible: true
    property alias bar: b
    CallHeaderBar { id: b; anchors.fill: parent; previewMode: true; placement: "dock"; compact: true }
}
)" );
        const QString copyDir = qEnvironmentVariable("LIGHTNING_REGRESSION_QML_COPY");
        if (!copyDir.isEmpty()) {
            fixture.replace("import MatrixClient", QByteArray("import MatrixClient\nimport \"")
                            + QUrl::fromLocalFile(copyDir).toEncoded() + "\" as Reverted");
            fixture.replace("CallHeaderBar {", "Reverted.CallHeaderBar {");
        }
        component.setData(fixture, QUrl(QStringLiteral("qrc:/regressionfixture.qml")));
        std::unique_ptr<QObject> owner(component.create());
        QVERIFY2(owner, qPrintable(component.errorString()));
        auto *bar = owner->property("bar").value<QQuickItem *>();
        QVERIFY(bar);
        auto *chevron = bar->findChild<QQuickItem *>(
            QStringLiteral("callBarShareOptionsChevron"));
        QVERIFY(chevron);
        QTRY_VERIFY(chevron->isVisible());
        QTRY_VERIFY(chevron->width() > 0);
        const QPointF start = chevron->mapToItem(bar, QPointF());
        QVERIFY(start.x() >= 0);
        QVERIFY(start.x() + chevron->width() <= bar->width());
        QVERIFY(QMetaObject::invokeMethod(chevron, "clicked"));
        QObject *menu = nullptr;
        for (auto *child : chevron->findChildren<QObject *>()) {
            if (child->metaObject()->indexOfSignal("chooseAppsRequested()") >= 0)
                menu = child;
        }
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(menu->findChild<QObject *>(QStringLiteral("shareAudioOffItem")));
        QVERIFY(menu->findChild<QObject *>(QStringLiteral("shareAudioMenuItem")));
    }

    void theShareChevronSitsAsCloseAsEveryOtherChevron()
    {
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

        if (!item->isVisible())
            QSKIP("no loopback capture element here, so the sound row is "
                  "absent and its width cannot be measured");

        QList<QQuickItem *> rows{ item };
        for (const char *name : { "shareAudioOffItem", "shareAudioAppsItem" }) {
            if (auto *row = menu->findChild<QQuickItem *>(
                    QString::fromLatin1(name));
                row && row->isVisible())
                rows << row;
        }
        int checked = 0;
        for (QQuickItem *row : rows) {
            for (QQuickItem *label : row->findChildren<QQuickItem *>()) {
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
        }
        QVERIFY2(checked >= 2,
                 "fewer than two sound labels were measured, so this case "
                 "proved little");
    }

    void theShareAudioToggleLivesWhereWaylandCanReachIt()
    {
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
        QVERIFY2(menu.contains(QStringLiteral("shareMaxHeight"))
                     && menu.contains(QStringLiteral("shareFps")),
                 "resolution or frame rate is missing from the one surface "
                 "Wayland shows");

        const QString picker =
            read(QStringLiteral(QML_DIR "/ScreenSharePicker.qml"));
        QVERIFY(!picker.isEmpty());
        QVERIFY2(picker.contains(QStringLiteral("shareAudioCombo")),
                 "the picker lost its share-sound choice, leaving the "
                 "non-portal platforms without one");

        for (const QString &src : { menu, picker }) {
            QVERIFY2(src.contains(QStringLiteral("shareAudioMode")),
                     "a share-sound control does not read the controller's "
                     "state");
        }
        QVERIFY2(menu.contains(QStringLiteral("chooseAppsRequested"))
                     && bar.contains(QStringLiteral("ShareAudioAppsDialog")),
                 "the call bar cannot open the application chooser, so a "
                 "Wayland user cannot pick which apps are heard");

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

        QMetaObject::invokeMethod(picker, "open");
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(picker->property("visible").toBool(), 3000);

        auto *section = picker->findChild<QObject *>(
            QStringLiteral("shareAudioSection"));
        auto *combo = picker->findChild<QObject *>(
            QStringLiteral("shareAudioCombo"));
        QVERIFY2(section != nullptr && combo != nullptr,
                 "the share-sound choice is gone");

        const bool supported =
            controller.groupCall()
                ? controller.groupCall()->shareAudioSupported() : false;
        if (!supported) {
            QSKIP("no loopback capture element here, so the visible branch "
                  "of this case cannot be exercised");
        }
        QCOMPARE(section->property("visible").toBool(), supported);

        QCOMPARE(combo->property("currentValue").toInt(),
                 controller.groupCall()->shareAudioMode());
        QCOMPARE(combo->property("count").toInt(),
                 controller.groupCall()->shareAudioCanChooseApps() ? 3 : 2);
        controller.groupCall()->setShareAudioEnabled(false);
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(combo->property("currentValue").toInt(), 0,
                                  3000);
        controller.groupCall()->setShareAudioEnabled(true);
        auto *list = picker->findChild<QObject *>(
            QStringLiteral("shareAudioAppsScroll"));
        QVERIFY(list != nullptr);
        QVERIFY(!list->property("visible").toBool());
    }

    void chromeRetiresWithThePointerParkedOverIt()
    {
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

        QTest::mouseMove(&window, QPoint(600, 300));
        QCoreApplication::processEvents();
        QVERIFY2(!root->property("stageChromeIdle").toBool(),
                 "retired before any time passed");
        QTRY_VERIFY_WITH_TIMEOUT(root->property("stageChromeIdle").toBool(),
                                 budgetMs + 20000);

        for (int i = 0; i < 20; ++i) {
            QTest::qWait(200);
            QVERIFY2(root->property("stageChromeIdle").toBool(),
                     "the chrome woke itself with the pointer parked and "
                     "unmoved -- a hide that re-delivers a hover event and "
                     "counts it as movement");
        }

        QCOMPARE(window.cursor().shape(), Qt::BlankCursor);

        QTest::mouseMove(&window, QPoint(640, 340));
        QCoreApplication::processEvents();
        QVERIFY2(!root->property("stageChromeIdle").toBool(),
                 "moving the pointer did not bring the controls back");
        QVERIFY2(window.cursor().shape() != Qt::BlankCursor,
                 "the pointer stayed hidden after the chrome came back");

        QTRY_VERIFY_WITH_TIMEOUT(root->property("stageChromeIdle").toBool(),
                                 budgetMs + 20000);
#endif
    }

    void chromeOverAPictureRetiresOnItsOwnTimerInBothPlaces()
    {
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

        QVERIFY2(!root->property("stageChromeIdle").toBool(),
                 "the overlay is retired before any time has passed");
        const int budgetMs = root->property("idleTickMs").toInt()
                             * root->property("idleTicksToHide").toInt();
        QVERIFY2(budgetMs >= 2000 && budgetMs <= 6000,
                 "the idle budget is outside the few seconds asked for");
        QTRY_VERIFY_WITH_TIMEOUT(root->property("stageChromeIdle").toBool(),
                                 budgetMs + 20000);

        root->setProperty("stageIdleTicks", 0);
        QVERIFY2(!root->property("stageChromeIdle").toBool(),
                 "the overlay stayed retired after the idle count cleared, "
                 "so nothing can bring it back");

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

        root->setProperty("stageIdleTicks", 0);
        QVERIFY(!root->property("stageChromeIdle").toBool());

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
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());
        const QString norm = normalized(stage);
        const QString stageCode = code(stage);
        QVERIFY(stageCode.contains(QStringLiteral("objectName: \"callStage\"")));

        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"callFullScreenWindow\"")));
        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"callFullScreenButton\"")));

        QVERIFY(norm.contains(
            QStringLiteral("objectName: \"callExitFullScreenButton\"")));
        QVERIFY(norm.contains(QStringLiteral(
            "objectName: \"callExitFullScreenFromStageButton\"")));

        QVERIFY2(norm.contains(QStringLiteral("Keys.onPressed: function")),
                 "full screen has no key handling at all");
        QVERIFY2(norm.contains(QStringLiteral("event.key === Qt.Key_Escape")),
                 "Escape does not leave full screen");
        QVERIFY2(!stageCode.contains(QStringLiteral("Shortcut {")),
                 "the call stage registers a Shortcut, which can shadow a "
                 "reserved sequence");

        QVERIFY(norm.contains(QStringLiteral("onClosing: root.exitFullScreen()")));
        QVERIFY2(!stageCode.contains(QStringLiteral("close.accepted = false")),
                 "the full-screen window refuses its close event");

        QVERIFY2(norm.contains(QStringLiteral(
                     "active: !root.collapsed && !root.fullScreenActive && "
                     "root.effectiveLayout === \"grid\"")),
                 "the grid stays up behind the full-screen window");
        QVERIFY2(norm.contains(QStringLiteral(
                     "active: !root.collapsed && !root.fullScreenActive && "
                     "root.effectiveLayout === \"spotlight\"")),
                 "the spotlight stays up behind the full-screen window");

        QVERIFY(norm.contains(QStringLiteral(
            "readonly property bool fullScreenActive: root.stageState ? "
            "(root.stageState.fullScreen && root.spotlightHasSurface) : "
            "false")));

        QVERIFY(norm.contains(QStringLiteral("fullScreenWindow.showFullScreen()")));
        QVERIFY2(!stageCode.contains(QStringLiteral("visibility:")),
                 "the full-screen window binds Window.visibility");
    }

    void theFullScreenWindowStaysHiddenUntilItIsAskedFor()
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
            QVERIFY(createdSpy.wait(3000));
        auto *root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY2(root != nullptr, "CallStage must instantiate");
        settle();

        QCOMPARE(root->property("fullScreenActive").toBool(), false);

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
        const QString raw =
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml"));
        QVERIFY(!raw.isEmpty());
        const QString tile = normalized(raw);

        QVERIFY(tile.contains(
            QStringLiteral("objectName: \"callParticipantVolumeSlider\"")));
        QVERIFY(tile.contains(
            QStringLiteral("objectName: \"callParticipantVolumeButton\"")));
        QVERIFY(tile.contains(QStringLiteral("acceptedButtons: Qt.RightButton")));
        QVERIFY(tile.contains(QStringLiteral("Qt.Key_Menu")));

        QVERIFY(tile.contains(QStringLiteral("from: 0")));
        QVERIFY(tile.contains(QStringLiteral("to: 200")));
        QVERIFY(tile.contains(QStringLiteral(
            "objectName: \"callParticipantVolumeNeutralMark\"")));

        QVERIFY(tile.contains(QStringLiteral("function currentVolumePercent()")));
        QVERIFY(tile.contains(QStringLiteral(
            "app.groupCall.participantVolume(root.identity)")));
        QVERIFY(tile.contains(QStringLiteral(
            "onOpened: volumeSlider.value = root.currentVolumePercent()")));

        QVERIFY(tile.contains(
            QStringLiteral("onMoved: root.applyVolumePercent(value)")));
        QVERIFY2(!code(raw).contains(QStringLiteral("onValueChanged")),
                 "the volume slider must react to onMoved, not to "
                 "onValueChanged");
        QVERIFY(tile.contains(QStringLiteral("property int volumeRevision")));
        QVERIFY(tile.contains(QStringLiteral("var _ = root.volumeRevision;")));

        QVERIFY(tile.contains(
            QStringLiteral("Above 100% amplifies and can clip.")));

        QVERIFY(tile.contains(QStringLiteral(
            "readonly property bool _volumeOffered: !root.local")));

        QVERIFY2(tile.contains(QStringLiteral(
                     "MouseArea { anchors.fill: parent acceptedButtons: "
                     "Qt.AllButtons")),
                 "the volume popup does not sink its own presses");
    }

    void theVolumeSurfaceGoesThroughTheControllerAndNeverTheStore()
    {
        const QString tile =
            code(read(QStringLiteral(QML_DIR "/CallParticipantTile.qml")));
        QVERIFY(!tile.isEmpty());

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
        QVERIFY2(body.contains(QStringLiteral("sandboxenv::isFlatpak()")),
                 "the answer must come from the sandbox, not a constant");
        const QString detector =
            read(QStringLiteral(SRC_DIR "/app/SandboxEnvironment.h"));
        QVERIFY(detector.contains(QStringLiteral("\"FLATPAK_ID\"")));
        QVERIFY(detector.contains(QStringLiteral("/.flatpak-info")));
        QVERIFY2(!detector.contains(QStringLiteral("exists(flatpakInfoPath")),
                 "never gate /.flatpak-info on exists()");
    }

    void microphoneGainIsOfferedWithItsMicrophoneAndSaysWhatItCosts()
    {
        const QString raw =
            read(QStringLiteral(QML_DIR "/CallDeviceSettings.qml"));
        QVERIFY(!raw.isEmpty());
        const QString devices = normalized(raw);

        QVERIFY(devices.contains(
            QStringLiteral("objectName: \"microphoneGainSlider\"")));
        QVERIFY(devices.contains(QStringLiteral("to: 200")));
        QVERIFY(devices.contains(QStringLiteral(
            "objectName: \"microphoneGainNeutralMark\"")));
        QVERIFY(devices.contains(
            QStringLiteral("Above 100% amplifies and can clip.")));

        QVERIFY(devices.contains(
            QStringLiteral("value: app.settings.microphoneGain")));
        QVERIFY(devices.contains(QStringLiteral(
            "onMoved: app.settings.microphoneGain = Math.round(value)")));
        QVERIFY2(!code(raw).contains(QStringLiteral("onValueChanged")),
                 "the gain slider must react to onMoved, not onValueChanged");
        QVERIFY2(!code(raw).contains(QStringLiteral("QSettings")),
                 "no QML surface may touch QSettings");
    }

    void noiseSuppressionIsOneChoiceThatReachesTheEngine()
    {
        const QString settings =
            normalized(read(QStringLiteral(QML_DIR "/SettingsScreen.qml")));
        QVERIFY(settings.contains(QStringLiteral("NoiseSuppressionSelector {")));
        QVERIFY2(settings.contains(QStringLiteral(
                     "selected: app.settings.noiseSuppressionMode")),
                 "the selector must show the stored mode");
        QVERIFY2(settings.contains(QStringLiteral(
                     "app.settings.noiseSuppressionMode = key")),
                 "a choice must be written to the setting, nowhere else");
        QVERIFY(settings.contains(
            QStringLiteral("app.groupCall.noiseSuppressionChoices()")));
        QVERIFY(settings.contains(
            QStringLiteral("app.groupCall.noiseSuppressionFailedMode")));

        const QString selector =
            read(QStringLiteral(QML_DIR "/NoiseSuppressionSelector.qml"));
        QVERIFY(!selector.isEmpty());
        QVERIFY(selector.contains(QStringLiteral("Accessible.RadioButton")));
        QVERIFY2(!code(selector).contains(QStringLiteral("CheckBox"))
                     && !code(selector).contains(QStringLiteral("Switch {")),
                 "a toggle per engine would let two run at once");
        QVERIFY2(!code(selector).contains(QStringLiteral("QSettings")),
                 "no QML surface may touch QSettings");

        const QString controller =
            read(QStringLiteral(SRC_DIR "/calls/SfuCallController.cpp"));
        const int connectAt = controller.indexOf(
            QStringLiteral("&SettingsManager::noiseSuppressionModeChanged"));
        QVERIFY2(connectAt >= 0, "nothing listens to the setting");
        QVERIFY2(controller.mid(connectAt, 160).contains(
                     QStringLiteral("applyAudioState()")),
                 "a change must reach a running call");
        const int fn = controller.indexOf(
            QStringLiteral("void SfuCallController::applyAudioState()"));
        QVERIFY(fn >= 0);
        const QString body =
            controller.mid(fn, controller.indexOf(QStringLiteral("\n}\n"), fn) - fn);
        QVERIFY2(body.contains(QStringLiteral("m_engine->setNoiseSuppressionMode(")),
                 "applyAudioState() must hand the mode to the engine");
        QVERIFY(body.contains(QStringLiteral("m_settings->noiseSuppressionMode()")));
    }

    void noiseSuppressionChoicesSayWhyAModeIsMissing()
    {
        SfuCallController call;
        const QVariantList choices = call.noiseSuppressionChoices();
        const QStringList keys = {QStringLiteral("off"), QStringLiteral("webrtc"),
                                  QStringLiteral("rnnoise"),
                                  QStringLiteral("deepfilternet")};
        QCOMPARE(choices.size(), keys.size());
        for (int i = 0; i < keys.size(); ++i) {
            const QVariantMap choice = choices.at(i).toMap();
            QCOMPARE(choice.value(QStringLiteral("key")).toString(), keys.at(i));
            const bool off = i == 0;
            QCOMPARE(choice.value(QStringLiteral("available")).toBool(), off);
            QCOMPARE(choice.value(QStringLiteral("reason")).toString(),
                     off ? QString() : QStringLiteral("no-call-engine"));
        }
        QVERIFY(call.noiseSuppressionFailedMode().isEmpty());
    }

    void theNoiseSuppressionSelectorDisablesWhatCannotRun()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient

NoiseSuppressionSelector {
    width: 480
    selected: "webrtc"
    choices: [
        { key: "off", available: true, reason: "" },
        { key: "webrtc", available: true, reason: "" },
        { key: "rnnoise", available: false, reason: "not-in-build" },
        { key: "deepfilternet", available: true, reason: "" }
    ]
}
)"), QUrl(QStringLiteral("qrc:/noiseselectortest.qml")));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *item = qobject_cast<QQuickItem *>(root.data());
        QVERIFY(item);
        settle();
        std::function<QQuickItem *(QQuickItem *, const QString &)> find =
            [&find](QQuickItem *from, const QString &name) -> QQuickItem * {
            for (QQuickItem *child : from->childItems()) {
                if (child->objectName() == name)
                    return child;
                if (QQuickItem *deeper = find(child, name))
                    return deeper;
            }
            return nullptr;
        };
        QQuickItem *rnnoise =
            find(item, QStringLiteral("noiseSuppressionOption_rnnoise"));
        QQuickItem *dfn =
            find(item, QStringLiteral("noiseSuppressionOption_deepfilternet"));
        QQuickItem *webrtc =
            find(item, QStringLiteral("noiseSuppressionOption_webrtc"));
        QVERIFY(rnnoise && dfn && webrtc);
        QVERIFY(!rnnoise->isEnabled());
        QVERIFY(dfn->isEnabled());
        QVERIFY(webrtc->property("current").toBool());
        QVERIFY(!dfn->property("current").toBool());
        QQuickItem *reason =
            find(item, QStringLiteral("noiseSuppressionReason_rnnoise"));
        QVERIFY(reason);
        QVERIFY(reason->property("visible").toBool());
        QCOMPARE(reason->property("text").toString(),
                 QStringLiteral("Not included in this build."));
        QQuickItem *noReason =
            find(item, QStringLiteral("noiseSuppressionReason_deepfilternet"));
        QVERIFY(noReason && !noReason->property("visible").toBool());

        QSignalSpy chosen(root.data(), SIGNAL(chosen(QString)));
        QVERIFY(QMetaObject::invokeMethod(dfn, "clicked"));
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(chosen.first().at(0).toString(), QStringLiteral("deepfilternet"));
    }

    void theNoiseSelectorIsOneRadioGroupForTheKeyboard()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient

NoiseSuppressionSelector {
    width: 480
    selected: "webrtc"
    choices: [
        { key: "off", available: true, reason: "" },
        { key: "webrtc", available: true, reason: "" },
        { key: "rnnoise", available: false, reason: "not-in-build" },
        { key: "deepfilternet", available: true, reason: "" }
    ]
}
)"), QUrl(QStringLiteral("qrc:/noisekeys.qml")));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        settle();
        auto *item = qobject_cast<QQuickItem *>(root.data());
        QVERIFY(item);
        std::function<QQuickItem *(QQuickItem *, const QString &)> find =
            [&find](QQuickItem *from, const QString &name) -> QQuickItem * {
            for (QQuickItem *child : from->childItems()) {
                if (child->objectName() == name)
                    return child;
                if (QQuickItem *deeper = find(child, name))
                    return deeper;
            }
            return nullptr;
        };
        int strong = 0;
        for (const char *key : {"off", "webrtc", "rnnoise", "deepfilternet"}) {
            QQuickItem *o = find(item, QStringLiteral("noiseSuppressionOption_")
                                           + QLatin1String(key));
            QVERIFY(o);
            if (o->property("focusPolicy").toInt() == Qt::StrongFocus)
                ++strong;
        }
        QCOMPARE(strong, 1);
        QCOMPARE(find(item, QStringLiteral("noiseSuppressionOption_webrtc"))
                     ->property("focusPolicy").toInt(),
                 int(Qt::StrongFocus));

        QSignalSpy chosen(root.data(), SIGNAL(chosen(QString)));
        QVERIFY(QMetaObject::invokeMethod(root.data(), "step",
                                          Q_ARG(QVariant, 1), Q_ARG(QVariant, 1)));
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(chosen.last().at(0).toString(), QStringLiteral("deepfilternet"));
        QVERIFY(QMetaObject::invokeMethod(root.data(), "step",
                                          Q_ARG(QVariant, 0), Q_ARG(QVariant, -1)));
        QCOMPARE(chosen.last().at(0).toString(), QStringLiteral("deepfilternet"));
    }

    void theMicrophoneSliderIsKeyboardCompleteAndMirrorsItsFill()
    {
        AppController controller(AppController::MockBackend);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &controller);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient
Item {
    id: outer
    width: 520
    height: 900
    property bool rtl: false
    LayoutMirroring.enabled: rtl
    LayoutMirroring.childrenInherit: true
    CallDeviceSettings { id: s; width: 480 }
}
)"), QUrl(QStringLiteral("qrc:/micslider.qml")));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *outer = qobject_cast<QQuickItem *>(root.data());
        QVERIFY(outer);
        QQuickWindow window;
        window.resize(520, 900);
        outer->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        settle();

        std::function<QQuickItem *(QQuickItem *, const QString &)> find =
            [&find](QQuickItem *from, const QString &name) -> QQuickItem * {
            for (QQuickItem *child : from->childItems()) {
                if (child->objectName() == name)
                    return child;
                if (QQuickItem *deeper = find(child, name))
                    return deeper;
            }
            return nullptr;
        };
        QQuickItem *slider = find(outer, QStringLiteral("microphoneGainSlider"));
        QVERIFY(slider);
        slider->forceActiveFocus();
        QVERIFY(slider->hasActiveFocus());
        SettingsManager *settings = controller.settings();

        auto press = [&](Qt::Key key, int from, int expected) {
            settings->setMicrophoneGain(from);
            settle();
            QTest::keyClick(&window, key);
            settle();
            QCOMPARE(settings->microphoneGain(), expected);
            QVERIFY(slider->hasActiveFocus());
        };
        const int to = slider->property("to").toInt();
        press(Qt::Key_Home, 80, int(slider->property("from").toDouble()));
        press(Qt::Key_End, 80, to);
        press(Qt::Key_PageUp, 80, 90);
        press(Qt::Key_PageUp, to - 3, to);
        press(Qt::Key_PageDown, 80, 70);
        press(Qt::Key_PageDown, 5, 0);
        press(Qt::Key_Down, 0, 0);
        press(Qt::Key_Up, 0, 1);
        press(Qt::Key_Down, 50, 49);

        settings->setMicrophoneGain(to * 3 / 4);
        settle();
        QQuickItem *fill = find(outer, QStringLiteral("microphoneGainFill"));
        QVERIFY(fill);
        const qreal track = fill->parentItem()->width();
        QVERIFY(track > 50);
        QVERIFY2(qAbs(fill->width() - track * 0.75) < 2.0,
                 qPrintable(QString::number(fill->width())));
        QVERIFY(qAbs(fill->x()) < 1.0);

        root->setProperty("rtl", true);
        settle();
        QVERIFY2(qAbs(fill->width() - track * 0.75) < 2.0,
                 qPrintable(QString::number(fill->width())));
        QVERIFY2(qAbs(fill->x() - (track - fill->width())) < 2.0,
                 qPrintable(QString::number(fill->x())));
    }

    void theCallMenuCarriesTheMicrophoneLevelAndWritesTheSharedSetting()
    {
        const QString raw = read(QStringLiteral(QML_DIR "/CallDeviceMenu.qml"));
        QVERIFY2(!raw.isEmpty(), "CallDeviceMenu.qml is missing");
        const QString menu = normalized(code(raw));

        QVERIFY2(menu.contains(
                     QStringLiteral("objectName: \"callMenuMicGainSlider\"")),
                 "the in-call device menu has no level control");
        QVERIFY(menu.contains(QStringLiteral("to: 200")));
        QVERIFY(menu.contains(QStringLiteral(
            "objectName: \"callMenuMicGainNeutralMark\"")));
        QVERIFY(menu.contains(
            QStringLiteral("objectName: \"callMenuMicGainReset\"")));

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

    void theCallMenuPointsAtTheSoundSectionOfSettings()
    {
        const QString menu =
            normalized(code(read(QStringLiteral(QML_DIR "/CallDeviceMenu.qml"))));
        QVERIFY(!menu.isEmpty());
        QVERIFY2(menu.contains(
                     QStringLiteral("app.showSettingsSection(\"sound\")")),
                 "the call's device menu offers no way to the rest of the "
                 "sound settings");

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

        QCOMPARE(screen.count(
                     QStringLiteral("objectName: \"callDeviceSettings\"")), 1);
    }

    void everyIconTheVolumeSurfacesAskForIsInTheBundledSubset()
    {
        const QString icons = read(QStringLiteral(QML_DIR "/Icon.qml"));
        QVERIFY(!icons.isEmpty());
        for (const auto &name : {"volume_up", "volume_off", "graphic_eq",
                                 "mic", "settings"}) {
            QVERIFY2(icons.contains(
                         QStringLiteral("\"%1\":").arg(QLatin1String(name))),
                     qPrintable(QStringLiteral("icon '%1' is not mapped")
                                    .arg(QLatin1String(name))));
        }
    }

    void everyGlobalShortcutRowIsActuallyBoundInQml()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);

        QDir dir(QStringLiteral(QML_DIR));
        QVERIFY2(dir.exists(), QML_DIR);
        const auto files =
            dir.entryList({ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY(!files.isEmpty());
        QString all;
        for (const QString &name : files)
            all += code(read(dir.filePath(name)));

        QVERIFY2(all.count(QStringLiteral("sequenceFor(")) > 10,
                 "the sweep found almost no sequenceFor() call sites, so it is "
                 "matching the wrong thing and would pass on anything");

        QVERIFY2(all.contains(QStringLiteral("timelineActionForKey(")),
                 "no view resolves timeline keys, so the TimelineContext "
                 "rows below cannot be wired");
        QVERIFY2(all.contains(QStringLiteral("messageSelectionActionForKey(")),
                 "no view resolves message-selection keys, so the "
                 "MessageSelectionContext rows below cannot be wired");

        QStringList unwired;
        int timelineRows = 0;
        int selectionRows = 0;
        int dialogRows = 0;
        int settingsRows = 0;
        const QStringList dialogFiles = {
            QStringLiteral("ActivityCenterPanel.qml"),
            QStringLiteral("QuickSwitcher.qml"),
        };
        for (int row = 0; row < registry.rowCount(); ++row) {
            const QModelIndex idx = registry.index(row);
            const int context =
                registry.data(idx, ShortcutRegistry::ContextRole).toInt();
            if (context == ShortcutRegistry::EditorContext)
                continue;
            const QString id =
                registry.data(idx, ShortcutRegistry::IdRole).toString();
            if (context == ShortcutRegistry::SettingsContext) {
                ++settingsRows;
                const QString source = code(read(dir.filePath(QStringLiteral("SettingsScreen.qml"))));
                if (!source.contains(QStringLiteral("action === \"%1\"").arg(id)))
                    unwired.append(id);
                continue;
            }
            if (context == ShortcutRegistry::DialogContext) {
                ++dialogRows;
                for (const QString &file : dialogFiles) {
                    const QString source = code(read(dir.filePath(file)));
                    if (!source.contains(QStringLiteral("dialogActionForKey("))
                        || !source.contains(QStringLiteral("action === \"%1\"").arg(id)))
                        unwired.append(id + QStringLiteral(" in ") + file);
                }
                continue;
            }
            if (context == ShortcutRegistry::MessageSelectionContext) {
                ++selectionRows;
                if (!all.contains(QStringLiteral("case \"%1\":").arg(id)))
                    unwired.append(id);
                continue;
            }
            if (context == ShortcutRegistry::TimelineContext) {
                ++timelineRows;
                if (!all.contains(QStringLiteral("case \"%1\":").arg(id)))
                    unwired.append(id);
                continue;
            }
            if (!all.contains(QStringLiteral("sequenceFor(\"%1\")").arg(id)))
                unwired.append(id);
        }
        QVERIFY2(timelineRows >= 4, "no TimelineContext rows were checked");
        QCOMPARE(dialogRows, 3);
        QCOMPARE(settingsRows, 6);
        const QString settingsSource = code(read(dir.filePath(QStringLiteral("SettingsScreen.qml"))));
        QVERIFY(settingsSource.contains(QStringLiteral("installSettingsNavigation(root)")));
        for (const QString &id : {QStringLiteral("dialog.previousItem"),
                                 QStringLiteral("dialog.nextItem"), QStringLiteral("dialog.close")})
            QVERIFY2(settingsSource.contains(QStringLiteral("action === \"%1\"").arg(id)), qPrintable(id));
        QVERIFY2(selectionRows >= 5,
                 "no MessageSelectionContext rows were checked");
        QVERIFY2(unwired.isEmpty(),
                 qPrintable(QStringLiteral(
                                "these registry rows report a key and do "
                                "nothing — no Shortcut in qml/ asks for them: "
                                "%1")
                                .arg(unwired.join(QStringLiteral(", ")))));
    }

    void theStartCallKeyIsGatedLikeTheCallButton()
    {
        const QString button = normalized(
            code(read(QStringLiteral(QML_DIR "/TimelinePane.qml"))));
        const QString shell = normalized(
            code(read(QStringLiteral(QML_DIR "/MainScreen.qml"))));
        QVERIFY(!button.isEmpty());
        QVERIFY(!shell.isEmpty());

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

        QVERIFY2(expr.contains(QStringLiteral("app.callGateRevision")),
                 "the call button gates on canStartCall() without reading "
                 "callGateRevision, so RTC state arriving after the room was "
                 "opened never reaches the button");

        QVERIFY2(clauses.size() >= 4,
                 qPrintable(QStringLiteral("only %1 conjuncts came out of the "
                                           "call button's gate — the slice is "
                                           "wrong and this case is vacuous")
                                .arg(clauses.size())));

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
        QVERIFY2(glyph.contains(QStringLiteral("Loader")),
                 "the glyph's Icon is not behind a Loader, so every room row "
                 "adds a permanent viewport observer");

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

    void theMemberRosterStaysCompact()
    {
        const QString panel = read(QStringLiteral(QML_DIR "/RoomInfoPanel.qml"));
        QVERIFY(!panel.isEmpty());

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
        QVERIFY2(!chrome.contains(QStringLiteral("text: qsTr(\"A to Z\")")),
                 "the sort control went back to a text button");

        const QString list = panel.mid(listAt);
        QVERIFY2(list.contains(QStringLiteral("AppTheme.scaled(AppTheme.textTitle)")),
                 "the member name does not follow the text-size slider");
        QVERIFY2(list.contains(QStringLiteral("Math.max(34, AppTheme.scaled")),
                 "the member row height is a literal again, so it clips its "
                 "own text at a larger slider position");
        QVERIFY2(list.contains(QStringLiteral("Math.max(24, AppTheme.scaled")),
                 "the member avatar no longer follows the row");
        QVERIFY2(list.contains(QStringLiteral("spacing: 3")),
                 "the member rows touch, so two names read as one block");
        const QString clean = code(panel);
        const int tabsAt = clean.indexOf(QStringLiteral("objectName: \"roomInfoTabs\""));
        QVERIFY(tabsAt > 0);
        const int tabsEnd = clean.indexOf(QStringLiteral("onActivated:"), tabsAt);
        QVERIFY(tabsEnd > tabsAt);
        const QString tabs = clean.mid(tabsAt, tabsEnd - tabsAt);
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

        const QString pane = read(QStringLiteral(QML_DIR "/TimelinePane.qml"));
        QVERIFY(!pane.isEmpty());
        QVERIFY2(pane.contains(QStringLiteral("Math.min(app.settings.sidePanelWidth")),
                 "the info panel takes its stored width uncapped, so a window "
                 "narrower than it was dragged in pushes the panel off the "
                 "right edge");
        QVERIFY2(panel.contains(QStringLiteral("clip: true")),
                 "the info panel does not clip, so anything wider than it "
                 "paints over the timeline");

        const int rowAt = panel.indexOf(QStringLiteral("id: memberRowComponent"));
        QVERIFY2(rowAt > 0, "the member row component is gone");
        const int rowEnd = panel.indexOf(QStringLiteral("visible: root.section === \"widgets\""), rowAt);
        QVERIFY2(rowEnd > rowAt,
                 "the section after the roster is gone: re-anchor this scope "
                 "rather than deleting the bound, or the check silently "
                 "covers the whole file");
        const QString row = panel.mid(rowAt, rowEnd - rowAt);
        QVERIFY2(!row.contains(QStringLiteral("font.pixelSize: AppTheme.text")),
                 "a member row sets an UNSCALED font size, so it ignores the "
                 "text-size slider every other surface honours");
    }

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
        QVERIFY2(pane_.contains(QStringLiteral(
                     "var floor = root.callPanelHasVideo")),
                 "the clamp no longer distinguishes a call showing video, so "
                 "a share is squeezed to the voice-only floor again");
        QVERIFY2(pane_.contains(QStringLiteral(
                     "Math.min(root.callPanelVideoFloor,")),
                 "the clamp no longer applies the stage's own minimum");

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
        QCOMPARE(qRound(clamped), 220);

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
        QVERIFY(block.contains(QStringLiteral("spotlightColumn.stripTileHeight")));
        QVERIFY2(!stage.contains(QStringLiteral("width: active ? 140 : 0")),
                 "a strip tile is back to a hardcoded width");
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
        QCOMPARE(modeFor(shortStage), QStringLiteral("bubbles"));

        QCOMPARE(modeFor(400), QStringLiteral("tiles"));
        QCOMPARE(stripFor(400), 96);
        QCOMPARE(stripFor(1000), 96);
        QCOMPARE(stripFor(0), 96);
        QCOMPARE(modeFor(0), QStringLiteral("tiles"));
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

    void theLinuxScreenShareBranchActuallyConsultsTheRoutePolicy()
    {
        QFile file(QStringLiteral(SRC_DIR "/calls/SfuCallController.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(file.readAll());

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

        QVERIFY2(body.contains(QStringLiteral("linuxShareRoute(")),
                 "requestScreenShare() does not consult the route policy, so "
                 "every case that tests the policy tests nothing reachable");
        QVERIFY2(body.contains(QStringLiteral(
                     "linuxShareRefusal(route, runningSandboxed())")),
                 "the Linux refusals do not come from the policy, or no "
                 "longer tell it whether the build is sandboxed — a Flatpak "
                 "would be told to install a host package it cannot load");
        QVERIFY2(body.contains(QStringLiteral("offerLinuxDisplayPicker(true)")),
                 "the no-portal route does not offer Lightning's own picker");
        const int helperBegin = source.indexOf(
            QStringLiteral("void SfuCallController::offerLinuxDisplayPicker("));
        QVERIFY2(helperBegin > 0, "offerLinuxDisplayPicker() is gone");
        const QString helper = source.mid(helperBegin, 1600);
        QVERIFY2(helper.contains(QStringLiteral("populateLinuxDisplaySources()")),
                 "the fallback does not populate the shared picker's rows");
        QVERIFY2(helper.contains(QStringLiteral("screenShareSourcesAvailable()")),
                 "the fallback never opens the picker");

        QVERIFY2(body.contains(QStringLiteral("m_portal->requestShare(")),
                 "the portal path is gone from requestScreenShare()");
        const int portalAt = body.indexOf(QStringLiteral(
            "LinuxShareRoute::Portal"));
        const int fallbackAt = body.indexOf(QStringLiteral(
            "LinuxShareRoute::FallbackDisplays"));
        QVERIFY2(portalAt >= 0, "the Portal route is not handled at all");
        QVERIFY2(fallbackAt >= 0, "the fallback route is not handled at all");
        QVERIFY2(portalAt < fallbackAt,
                 "the fallback is considered before the portal");

        const int oldRefusal = body.indexOf(QStringLiteral(
            "Screen sharing isn't available on this desktop."));
        if (oldRefusal >= 0) {
            const int guard = body.indexOf(QStringLiteral(
                "#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)"));
            QVERIFY2(guard >= 0 && guard < oldRefusal,
                     "the unactionable refusal is reachable on Linux again");
        }

        const QRegularExpression spelledInAString(
            QStringLiteral("\"[^\"\n]*ximagesrc"));
        QVERIFY2(!spelledInAString.match(source).hasMatch(),
                 "the controller spells the capture element inside a string "
                 "instead of asking the engine for its name");
        QVERIFY2(body.contains(QStringLiteral("x11ScreenCaptureElementName()")),
                 "the element probe does not ask the engine which element it "
                 "should be probing for");
    }

    void theStageHasOneControlSurfaceAndItIsNotAlongTheBottom()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(!stage.isEmpty());

        QCOMPARE(stage.count(QStringLiteral("objectName: \"callStageControls\"")), 1);
        QCOMPARE(stage.count(QStringLiteral("objectName: \"callFullScreenDock\"")), 1);
        QVERIFY2(!stage.contains(QStringLiteral("callStageDock")),
                 "the bottom dock is back: it costs a strip of every screen "
                 "share and moves the controls between states");

        const int at = stage.indexOf(QStringLiteral("objectName: \"callStageControls\""));
        QVERIFY(at > 0);
        const int loader = stage.lastIndexOf(QStringLiteral("Loader {"), at);
        QVERIFY(loader > 0);
        const QString block = stage.mid(loader, at - loader);
        QVERIFY2(!block.contains(QStringLiteral("active: root.collapsed")),
                 "the one control surface is collapsed-only again");
        QVERIFY2(block.contains(QStringLiteral("active: true")),
                 "the control surface must be live in both states");

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

    void compactIsAReducedControlSetNotJustASmallerOne()
    {
        const QString bar = read(QStringLiteral(QML_DIR "/CallHeaderBar.qml"));
        QVERIFY(!bar.isEmpty());
        QVERIFY(bar.contains(QStringLiteral("readonly property bool tight: root.compact || root.narrow")));
        const int hidden = bar.count(QStringLiteral("!root.compact"))
            + bar.count(QStringLiteral("!root.tight"));
        QVERIFY2(hidden >= 4,
                 qPrintable(QStringLiteral("expected compact to gate several "
                                           "controls, found %1").arg(hidden)));
    }

    void fullScreenOverlaysRetireWhenThePointerIsStill()
    {
        const QString stage = read(QStringLiteral(QML_DIR "/CallStage.qml"));
        QVERIFY(stage.contains(QStringLiteral("overlaysIdle")));
        QVERIFY2(stage.contains(QStringLiteral("HoverHandler")),
                 "movement must wake the chrome without consuming a click");
        QCOMPARE(stage.count(
                     QStringLiteral("opacity: fullScreenSurface.overlaysIdle ? 0 : 1")),
                 2);
        QVERIFY2(stage.contains(QStringLiteral("onFullScreenActiveChanged")),
                 "the idle flag outlives the state it belongs to");
    }

    void theEncryptionBadgeStopsReassuringWhenMediaIsBeingDropped()
    {
        const QString norm = normalized(read(QStringLiteral(QML_DIR "/CallStage.qml")));
        QVERIFY(!norm.isEmpty());
        const int lock = norm.indexOf(QStringLiteral("app.groupCall.mediaEncrypted"));
        QVERIFY2(lock >= 0, "the encryption badge is gone entirely");
        const QString block = norm.mid(lock, 900);
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

    void everyJoinBlockTokenHasWordingOnEverySurface()
    {
        const QString controller = read(QStringLiteral(
            SRC_DIR "/calls/RtcController.cpp"));
        QVERIFY(!controller.isEmpty());

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
        QVERIFY2(older->property("isLatestCallRow").isValid(),
                 "CallEventDelegate does not declare isLatestCallRow, so the "
                 "host cannot tell it which row owns the live session");
        QVERIFY2(older->property("supersededByNewerCall").toBool(),
                 "an older call row does not consider itself superseded");
        QVERIFY2(!newest->property("supersededByNewerCall").toBool(),
                 "the newest call row believes it has been superseded");

        QFile delegateSource(QStringLiteral(QML_DIR "/CallEventDelegate.qml"));
        QVERIFY2(delegateSource.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(delegateSource.errorString()));
        const QString qml = QString::fromUtf8(delegateSource.readAll());
        const int canJoinAt = qml.indexOf(QStringLiteral("readonly property bool canJoin:"));
        QVERIFY2(canJoinAt >= 0, "canJoin is gone from CallEventDelegate");
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
        QFile hostSource(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY2(hostSource.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(hostSource.errorString()));
        const QString host = QString::fromUtf8(hostSource.readAll());
        const int bindAt = host.indexOf(QStringLiteral("isLatestCallRow:"));
        QVERIFY2(bindAt >= 0,
                 "MessageDelegate no longer tells CallEventDelegate which row "
                 "is the newest call, so the delegate's permissive default "
                 "puts Join back on every call row in the room");
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

    void theCallMicMenuSwitchesNoiseSuppressionLive()
    {
        const QString menu = normalized(
            read(QStringLiteral(QML_DIR "/CallDeviceMenu.qml")));
        QVERIFY(menu.contains(QStringLiteral("objectName: \"callMenuNoiseHeading\"")));
        QVERIFY(menu.contains(
            QStringLiteral("app.groupCall.noiseSuppressionChoices()")));
        QVERIFY(menu.contains(QStringLiteral(
            "app.settings.noiseSuppressionMode = modeKey")));
        QVERIFY(menu.contains(QStringLiteral(
            "app.groupCall.retryNoiseSuppression()")));
        QVERIFY(menu.contains(QStringLiteral(
            "enabled: choice !== null && choice.available")));
        for (const char *key : { "off", "webrtc", "rnnoise", "deepfilternet" })
            QVERIFY(menu.contains(QStringLiteral("NoiseModeItem { modeKey: \"%1\" }")
                                      .arg(QLatin1String(key))));
        QVERIFY(menu.contains(QStringLiteral(
            "onTriggered: app.showSettingsSection(\"labs\")")));
        const QString controller = read(
            QStringLiteral(QML_DIR "/../src/calls/SfuCallController.cpp"));
        QVERIFY(controller.contains(QStringLiteral("setNoiseSuppressionMode(")));
    }

    void everyTileStateBadgeNamesItselfOnHover()
    {
        const QString tile = normalized(
            read(QStringLiteral(QML_DIR "/CallParticipantTile.qml")));
        const QString badge = normalized(
            read(QStringLiteral(QML_DIR "/CallTileBadge.qml")));
        QVERIFY(!tile.isEmpty() && !badge.isEmpty());
        QVERIFY(badge.contains(QStringLiteral("ToolTip.text: root.tooltip")));
        for (const char *tip : { "Poor connection", "Hand raised",
                                 "Sharing their screen", "Camera off",
                                 "Microphone muted" }) {
            QVERIFY2(tile.contains(QStringLiteral("tooltip: qsTr(\"%1\")")
                                       .arg(QLatin1String(tip)))
                         || tile.contains(QStringLiteral("ToolTip.text: qsTr(\"%1\")")
                                              .arg(QLatin1String(tip))),
                     tip);
        }
    }

    void aParticipantsSessionShowsOnlyWithDeveloperTools()
    {
        AppController app(AppController::MockBackend);
        QSignalSpy loginSpy(app.auth(), &AuthManager::loginSucceeded);
        app.auth()->login(QStringLiteral("https://mock.local"),
                          QStringLiteral("alice"), QStringLiteral("unused"));
        QVERIFY(loginSpy.wait(3000));
        app.settings()->setDeveloperTools(false);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &app);
        QQuickWindow window;
        window.resize(400, 300);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient
Item {
    width: 400
    height: 300
    CallParticipantTile {
        objectName: "infoTile"
        x: 20; y: 20
        width: 300
        height: 180
        identity: "PA_remote"
        userId: "@bob:mock.local"
        deviceId: "BOBDEVICE"
        displayName: "Bob"
    }
}
)"), QUrl(QStringLiteral("qrc:/callclientinfotest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *outer = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(outer != nullptr);
        outer->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *tile = outer->findChild<QQuickItem *>(QStringLiteral("infoTile"));
        QVERIFY(tile != nullptr);
        QMetaObject::invokeMethod(tile, "openVolumeControl");
        settle(6);
        auto *popup = tile->findChild<QObject *>(
            QStringLiteral("callParticipantVolumePopup"));
        QVERIFY(popup != nullptr);
        QTRY_VERIFY(popup->property("opened").toBool());
        auto *host = tile->findChild<QObject *>(
            QStringLiteral("callParticipantClientInfo"));
        QVERIFY(host != nullptr);
        QVERIFY2(!host->property("active").toBool(),
                 "the switch is off and the tile built the session view anyway");

        app.settings()->setDeveloperTools(true);
        QTRY_VERIFY(host->property("active").toBool());
        auto *info = qvariant_cast<QObject *>(host->property("item"));
        QVERIFY(info != nullptr);
        QCOMPARE(info->property("userId").toString(),
                 QStringLiteral("@bob:mock.local"));
        QCOMPARE(info->property("deviceId").toString(),
                 QStringLiteral("BOBDEVICE"));
        QVERIFY(info->property("visible").toBool());
        app.settings()->setDeveloperTools(false);
        QTRY_VERIFY(!host->property("active").toBool());
    }

    void theVolumeControlIsReachableByKeyboardAndWheel()
    {
        VolumeApp app;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("app", &app);
        QQuickWindow window;
        window.resize(400, 300);
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
import QtQuick
import MatrixClient
Item {
    width: 400
    height: 300
    CallParticipantTile {
        objectName: "volTile"
        x: 20; y: 20
        width: 300
        height: 180
        identity: "PA_remote"
        userId: "@bob:mock.local"
        displayName: "Bob"
        activeFocusOnTab: true
    }
}
)"), QUrl(QStringLiteral("qrc:/callvolumefocustest.qml")));
        QVERIFY2(component.errors().isEmpty(),
                 qPrintable(component.errorString()));
        std::unique_ptr<QObject> owner(component.create());
        auto *outer = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(outer != nullptr);
        outer->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        settle();

        auto *tile = outer->findChild<QQuickItem *>(QStringLiteral("volTile"));
        QVERIFY(tile != nullptr);
        QVERIFY2(tile->property("_volumeOffered").toBool(),
                 "the fixture must offer the volume control");
        const auto button = [&]() {
            return tile->findChild<QQuickItem *>(
                QStringLiteral("callParticipantVolumeButton"));
        };
        QVERIFY2(button() == nullptr, "no hover and no focus: no button yet");

        tile->forceActiveFocus();
        settle();
        QVERIFY(tile->hasActiveFocus());
        QVERIFY2(button() != nullptr, "tile focus must reveal the button");

        button()->forceActiveFocus();
        settle();
        QVERIFY(window.activeFocusItem() != nullptr);
        QVERIFY2(button() != nullptr && button()->hasActiveFocus(),
                 "the focused volume button was destroyed as it took focus");

        QMetaObject::invokeMethod(tile, "openVolumeControl");
        settle(6);
        auto *slider = tile->findChild<QQuickItem *>(
            QStringLiteral("callParticipantVolumeSlider"));
        QVERIFY(slider != nullptr);
        QTRY_VERIFY(slider->hasActiveFocus());
        auto *popup = tile->findChild<QObject *>(
            QStringLiteral("callParticipantVolumePopup"));
        QVERIFY(popup != nullptr);
        QTRY_VERIFY(popup->property("opened").toBool());
        QTRY_COMPARE(popup->property("opacity").toDouble(), 1.0);

        const QPointF local(slider->width() / 2, slider->height() / 2);
        const QPointF scene = slider->mapToScene(local);
        QTest::mouseMove(&window, scene.toPoint());
        settle();
        QWheelEvent wheel(scene, window.mapToGlobal(scene.toPoint()),
                          QPoint(), QPoint(0, 120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(&window, &wheel);
        settle();

        QVERIFY2(app.call()->writes > 0 && app.call()->stored > 100,
                 qPrintable(QStringLiteral("the wheel did not change the "
                                           "volume (slider value %1, wheelEnabled %2, writes %3)")
                                .arg(slider->property("value").toDouble())
                                .arg(slider->property("wheelEnabled").toBool())
                                .arg(app.call()->writes)));
        QCOMPARE(app.call()->lastId, QStringLiteral("PA_remote"));

        QTest::keyClick(&window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("visible").toBool());
        settle();
        QVERIFY2(window.activeFocusItem() != nullptr,
                 "no item has focus after the volume popup closed");
        QVERIFY2(tile->hasActiveFocus() || button() != nullptr,
                 "the tile lost its keyboard focus when the popup closed");
    }

private:
    QTemporaryDir m_configHome;

};

QTEST_MAIN(CallUiContractTest)
#include "CallUiContractTest.moc"
