// "Animated images" (Settings > Appearance > Motion and time): one rule for
// every animated image, qml/MotionPolicy.qml, fed by C++ facts (the
// gifAutoplay setting, Reduce motion, AppController::animationsSuspended).
// Pinned:
//   * the rule itself: Play automatically / Play on hover / Don't play, Reduce
//     motion turning automatic into on-hover, and an unreadable setting
//     playing nothing;
//   * an unfocused Lightning PAUSES a playing avatar on its frame (no frame
//     advances, nothing torn down) and resumes it;
//   * under Reduce motion an animated avatar still plays when the pointer
//     rests on it, and fetches nothing until then;
//   * a GIF on a link-preview card obeys the policy: still under Play on hover
//     until hovered, running under Play automatically, paused while
//     unfocused, never decoded under Don't play;
//   * a GIF row showing its still says it is a GIF;
//   * AppController::animationsSuspended follows the application state;
//   * every AnimatedImage in qml/ pauses on suspension and none reads the
//     setting around the rule (count-asserted sweep);
//   * the option lives in Appearance, where its search entry points.
#include <QtTest/QtTest>

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>

#if __has_include(<QtGui/private/qguiapplication_p.h>)
#include <QtGui/private/qguiapplication_p.h>
#define LIGHTNING_HAVE_APP_STATE_SEAM 1
#endif

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "media/MediaBridge.h"
#include "media/MediaImageProvider.h"
#include "models/TimelineModel.h"
#include "settings/SettingsPortable.h"

namespace {

constexpr int kTimeoutMs = 5000;

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    struct Fetch {
        quint64 opId;
        QString key;
        int width = 0;
    };
    QList<Fetch> fetches;

    bool supportsMediaBridge() const override { return true; }
    // In flight for ever: a 0 would read as a failed dispatch.
    quint64 fetchMedia(const QString &, int, int) override { return nextOp++; }
    quint64 fetchMxcThumbnail(const QString &mxc, int width, int) override
    {
        const quint64 op = nextOp++;
        fetches.append({ op, mxc, width });
        return op;
    }
    void succeed(quint64 opId, const QByteArray &bytes)
    {
        Q_EMIT mediaReady(opId, QString(), 0, bytes,
                          QStringLiteral("image/png"), QString());
    }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:x"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

// The settings the rule reads.
class SettingsShim : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int gifAutoplay MEMBER m_gifAutoplay NOTIFY gifAutoplayChanged)
public:
    using QObject::QObject;
    int m_gifAutoplay = 0;
    void setMode(int mode)
    {
        m_gifAutoplay = mode;
        Q_EMIT gifAutoplayChanged();
    }
Q_SIGNALS:
    void gifAutoplayChanged();
};

// Minimal `app`: the bridge, the settings, and the focus fact.
class AppShim : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MediaBridge *mediaBridge READ mediaBridge CONSTANT)
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
    Q_PROPERTY(bool animationsSuspended MEMBER m_suspended
                   NOTIFY animationsSuspendedChanged)
public:
    AppShim(MediaBridge *bridge, QObject *settings, QObject *parent = nullptr)
        : QObject(parent), m_bridge(bridge), m_settings(settings) {}
    MediaBridge *mediaBridge() const { return m_bridge; }
    QObject *settings() const { return m_settings; }
    void setSuspended(bool on)
    {
        m_suspended = on;
        Q_EMIT animationsSuspendedChanged();
    }
    bool m_suspended = false;
Q_SIGNALS:
    void animationsSuspendedChanged();

private:
    MediaBridge *m_bridge;
    QObject *m_settings;
};

// A real looping GIF: edge x edge, one full-canvas frame per colour, 100 ms
// each (the generator AvatarPipelineQmlTest uses).
QByteArray solidFramesGif(int edge, const QList<QColor> &colours)
{
    QByteArray g("GIF89a");
    const auto le16 = [&g](int v) {
        g.append(char(v & 0xff));
        g.append(char((v >> 8) & 0xff));
    };
    le16(edge);
    le16(edge);
    g.append(char(0x81));
    g.append('\0');
    g.append('\0');
    for (int i = 0; i < 4; ++i) {
        const QColor c = colours.value(i, Qt::black);
        g.append(char(c.red()));
        g.append(char(c.green()));
        g.append(char(c.blue()));
    }
    g.append(QByteArray("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19));
    for (int f = 0; f < colours.size() && f < 4; ++f) {
        g.append(QByteArray("\x21\xf9\x04\x00\x0a\x00\x00\x00", 8));
        g.append(char(0x2c));
        le16(0);
        le16(0);
        le16(edge);
        le16(edge);
        g.append('\0');
        g.append(char(0x02));
        QList<int> codes;
        const int pixels = edge * edge;
        for (int p = 0; p < pixels; ++p) {
            if (p % 2 == 0)
                codes.append(4);
            codes.append(f);
        }
        codes.append(5);
        QByteArray data;
        quint32 acc = 0;
        int bits = 0;
        for (const int code : std::as_const(codes)) {
            acc |= quint32(code) << bits;
            bits += 3;
            while (bits >= 8) {
                data.append(char(acc & 0xff));
                acc >>= 8;
                bits -= 8;
            }
        }
        if (bits > 0)
            data.append(char(acc & 0xff));
        for (qsizetype off = 0; off < data.size(); off += 255) {
            const QByteArray block = data.mid(off, 255);
            g.append(char(block.size()));
            g.append(block);
        }
        g.append('\0');
    }
    g.append(char(0x3b));
    return g;
}

QByteArray solidPng(int edge, const QColor &color)
{
    QImage image(edge, edge, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

const QList<QColor> kFrames = { QColor(255, 0, 0), QColor(0, 255, 0),
                                QColor(0, 0, 255), QColor(255, 255, 0) };

QString readQml(const QString &name)
{
    QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/") + name);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// The text of every `AnimatedImage {` block in `qml`, braces matched.
QStringList animatedImageBlocks(const QString &qml)
{
    QStringList out;
    static const QRegularExpression open(
        QStringLiteral("\\bAnimatedImage\\s*\\{"));
    auto it = open.globalMatch(qml);
    while (it.hasNext()) {
        const auto m = it.next();
        const qsizetype start = m.capturedStart();
        int depth = 0;
        for (qsizetype i = m.capturedEnd() - 1; i < qml.size(); ++i) {
            if (qml.at(i) == QLatin1Char('{')) {
                ++depth;
            } else if (qml.at(i) == QLatin1Char('}')) {
                if (--depth == 0) {
                    out << qml.mid(start, i - start + 1);
                    break;
                }
            }
        }
    }
    return out;
}

// An AnimatedImage block obeys the focus rule (MotionPolicy.qml): a dynamic
// `playing` folds `!MotionPolicy.suspended` into itself and sets no `paused`
// (QMovie loses a pause set before it starts), while a constant
// `playing: true` is held by `paused: MotionPolicy.suspended`. Checked as a
// rule, not a spelling.
bool suspendsCorrectly(const QString &block)
{
    static const QRegularExpression playingRe(QStringLiteral(
        "(?m)^\\s*playing:\\s*([^\\n]*(?:\\n\\s*(?:&&|\\|\\|)[^\\n]*)*)"));
    static const QRegularExpression pausedRe(
        QStringLiteral("(?m)^\\s*paused:\\s*([^\\n]*)"));
    const auto pm = playingRe.match(block);
    if (!pm.hasMatch())
        return false;
    const QString playing = pm.captured(1).simplified();
    const auto qm = pausedRe.match(block);
    const QString paused = qm.hasMatch() ? qm.captured(1).simplified() : QString();
    if (playing == QLatin1String("true"))
        return paused == QLatin1String("MotionPolicy.suspended");
    return playing.contains(QLatin1String("!MotionPolicy.suspended"))
        && paused.isEmpty();
}

// Frames shown over `ms` by an AnimatedImage.
QSet<int> framesOver(QQuickItem *movie, int ms)
{
    QSet<int> frames;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        frames.insert(movie->property("currentFrame").toInt());
        QTest::qWait(40);
    }
    return frames;
}

} // namespace

class AnimatedImagePolicyQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;

    struct Harness {
        std::unique_ptr<FakeClient> client;
        std::unique_ptr<MediaBridge> bridge;
        std::unique_ptr<SettingsShim> settings;
        std::unique_ptr<AppShim> shim;
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *avatar = nullptr;
        QStringList warnings;
    };

    void prepare(Harness &h, int mode)
    {
        h.client = std::make_unique<FakeClient>();
        h.bridge = std::make_unique<MediaBridge>();
        h.bridge->setClient(h.client.get());
        h.settings = std::make_unique<SettingsShim>();
        h.settings->m_gifAutoplay = mode;
        h.shim = std::make_unique<AppShim>(h.bridge.get(), h.settings.get());
        h.engine = std::make_unique<QQmlApplicationEngine>();
        connect(h.engine.get(), &QQmlEngine::warnings, this,
                [&h](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        h.warnings << e.toString();
                });
        h.engine->addImageProvider(QStringLiteral("lightning-media"),
                                   new MediaImageProvider(h.bridge.get()));
        h.engine->rootContext()->setContextProperty("app", h.shim.get());
        h.window = std::make_unique<QQuickWindow>();
    }

    static QObject *theme(Harness &h)
    {
        return h.engine->singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
    }

    static int fetchIndex(const Harness &h, const QString &mxc, int width)
    {
        for (int i = h.client->fetches.size() - 1; i >= 0; --i) {
            const auto &f = h.client->fetches.at(i);
            if (f.key == mxc && f.width == width)
                return i;
        }
        return -1;
    }

    // An avatar whose still thumbnail has loaded.
    bool readyAvatar(Harness &h, const QString &mxc, int size = 48)
    {
        QSignalSpy created(h.engine.get(),
                           &QQmlApplicationEngine::objectCreated);
        h.engine->setInitialProperties({
            { QStringLiteral("size"), size },
            { QStringLiteral("name"), QStringLiteral("Anim") },
            { QStringLiteral("colorKey"), QStringLiteral("@anim:x") },
            { QStringLiteral("mxc"), mxc },
        });
        h.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Avatar"));
        if (created.isEmpty() && !created.wait(3000))
            return false;
        h.avatar = qobject_cast<QQuickItem *>(
            created.at(0).at(0).value<QObject *>());
        if (!h.avatar)
            return false;
        h.window->resize(size + 40, size + 40);
        h.avatar->setParentItem(h.window->contentItem());
        h.avatar->setPosition(QPointF(20, 20));
        h.window->show();
        QCoreApplication::processEvents();
        // The software scene graph cannot draw the shape mask; the logic is
        // under test here (AvatarPipelineQmlTest does the same).
        h.avatar->setProperty("motionMaskable", true);
        const int thumb = fetchIndex(h, mxc, 224);
        if (thumb < 0)
            return false;
        h.client->succeed(h.client->fetches.at(thumb).opId,
                          solidPng(64, QColor(200, 200, 200)));
        return QTest::qWaitFor([&h] {
            return h.avatar->property("presentationState").toString()
                == QStringLiteral("ready");
        }, kTimeoutMs);
    }

    // ── MessageDelegate on the mock backend ──

    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
        QStringList warnings;
    };

    static QVariantMap baseFixture(AppController &controller)
    {
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("eventId"), QStringLiteral("$fixture"));
        fixture.insert(QStringLiteral("itemId"), QStringLiteral("fixture-item"));
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"),
                       QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"), QString{});
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("edited"), false);
        fixture.insert(QStringLiteral("isEncrypted"), false);
        fixture.insert(QStringLiteral("isDecrypted"), false);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("errorKind"), QString{});
        for (const char *flag : { "isImage", "isFile", "isVideo", "isAudio",
                                  "isSticker", "mediaIsVoice", "isThreadRoot",
                                  "mentionsMe", "mentionsRoom", "isLocalEcho",
                                  "mediaSourceAvailable", "mediaThumbAvailable" })
            fixture.insert(QString::fromLatin1(flag), false);
        for (const char *zero : { "mediaDurationMs", "mediaWidth", "mediaHeight",
                                  "mediaSize" })
            fixture.insert(QString::fromLatin1(zero), 0);
        for (const char *empty : { "mediaKey", "mediaFilename", "mediaMimetype",
                                   "replyToEventId" })
            fixture.insert(QString::fromLatin1(empty), QString{});
        fixture.insert(QStringLiteral("mediaUrl"), QUrl{});
        fixture.insert(QStringLiteral("mediaThumbUrl"), QUrl{});
        fixture.insert(QStringLiteral("reactions"), QVariantList{});
        return fixture;
    }

    bool createDelegate(AppController &controller, const QVariantMap &fixture,
                        Delegate &out)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        connect(out.engine.get(), &QQmlEngine::warnings, this,
                [&out](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        out.warnings << e.toString();
                });
        out.engine->addImageProvider(
            QStringLiteral("lightning-media"),
            new MediaImageProvider(controller.mediaBridge()));
        out.engine->rootContext()->setContextProperty("app", &controller);
        out.engine->rootContext()->setContextProperty("model", fixture);
        QSignalSpy created(out.engine.get(),
                           &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("MessageDelegate"));
        if (created.isEmpty() && !created.wait(3000))
            return false;
        out.root = qobject_cast<QQuickItem *>(
            created.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(760, 900);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(700);
        out.window->show();
        if (!QTest::qWaitForWindowExposed(out.window.get()))
            return false;
        QCoreApplication::processEvents();
        return true;
    }

    // A loaded page preview whose og:image is a looping GIF.
    static QVariantMap gifCardPreview()
    {
        const QByteArray gif = solidFramesGif(32, kFrames);
        return {
            { QStringLiteral("state"), QStringLiteral("loaded") },
            { QStringLiteral("url"), QStringLiteral("https://example.org/cat") },
            { QStringLiteral("host"), QStringLiteral("example.org") },
            { QStringLiteral("title"), QStringLiteral("A cat") },
            { QStringLiteral("description"), QStringLiteral("It moves") },
            { QStringLiteral("siteName"), QStringLiteral("Example") },
            { QStringLiteral("isGif"), true },
            { QStringLiteral("imageMime"), QStringLiteral("image/gif") },
            { QStringLiteral("imageSource"),
              QStringLiteral("data:image/gif;base64,")
                  + QString::fromLatin1(gif.toBase64()) },
            { QStringLiteral("imageWidth"), 32 },
            { QStringLiteral("imageHeight"), 32 },
        };
    }

#ifdef LIGHTNING_HAVE_APP_STATE_SEAM
    static void setAppState(Qt::ApplicationState state)
    {
        QGuiApplicationPrivate::setApplicationState(state, true);
        QCoreApplication::processEvents();
    }
#endif

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("animated-image-policy-qml-test"));
    }

    // The rule itself, for every setting with and without Reduce motion.
    void thePolicyFollowsTheSettingReducedMotionAndFocus()
    {
        Harness h;
        prepare(h, 0);
        QQmlComponent probe(h.engine.get());
        probe.setData(R"QML(
import QtQuick
import MatrixClient
QtObject {
    readonly property int mode: MotionPolicy.mode
    readonly property bool allowed: MotionPolicy.allowed
    readonly property bool autoplay: MotionPolicy.autoplay
    readonly property bool suspended: MotionPolicy.suspended
    readonly property bool playsIdle: MotionPolicy.plays(false)
    readonly property bool playsHovered: MotionPolicy.plays(true)
}
)QML",
                      QUrl());
        std::unique_ptr<QObject> p(probe.create(h.engine->rootContext()));
        QVERIFY2(p, qPrintable(probe.errorString()));
        QObject *t = theme(h);
        QVERIFY(t);

        struct Row { int setting; bool reduced; int mode; bool idle; bool hovered; };
        const QList<Row> rows = {
            { 0, false, 0, true, true },   // Play automatically
            { 0, true, 1, false, true },   // ... with Reduce motion: on hover
            { 1, false, 1, false, true },  // Play on hover
            { 1, true, 1, false, true },
            { 2, false, 2, false, false }, // Don't play
            { 2, true, 2, false, false },
        };
        int checked = 0;
        for (const Row &r : rows) {
            t->setProperty("reducedMotion", r.reduced);
            h.settings->setMode(r.setting);
            const QString at = QStringLiteral("setting %1 reduced %2")
                                   .arg(r.setting).arg(r.reduced);
            QVERIFY2(p->property("mode").toInt() == r.mode, qPrintable(at));
            QVERIFY2(p->property("allowed").toBool() == (r.mode != 2),
                     qPrintable(at));
            QVERIFY2(p->property("autoplay").toBool() == (r.mode == 0),
                     qPrintable(at));
            QVERIFY2(p->property("playsIdle").toBool() == r.idle, qPrintable(at));
            QVERIFY2(p->property("playsHovered").toBool() == r.hovered,
                     qPrintable(at));
            ++checked;
        }
        QCOMPARE(checked, 6);
        t->setProperty("reducedMotion", false);

        QCOMPARE(p->property("suspended").toBool(), false);
        h.shim->setSuspended(true);
        QCOMPARE(p->property("suspended").toBool(), true);
        h.shim->setSuspended(false);
        QCOMPARE(p->property("suspended").toBool(), false);

        // No readable setting plays nothing.
        h.settings->setMode(7);
        QCOMPARE(p->property("mode").toInt(), 2);
        QCOMPARE(h.warnings, QStringList{});
    }

    // Unfocused: a playing avatar holds its frame, keeps its decoder and its
    // slot, and runs again on focus. On the old code it kept decoding.
    void anUnfocusedWindowPausesAPlayingAvatarAndFocusResumesIt()
    {
        Harness h;
        prepare(h, 0);
        const QString mxc = QStringLiteral("mxc://x/focus");
        QVERIFY(readyAvatar(h, mxc));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = fetchIndex(h, mxc, 0)) >= 0,
                                 kTimeoutMs);
        h.client->succeed(h.client->fetches.at(original).opId,
                          solidFramesGif(64, kFrames));
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 kTimeoutMs);
        auto *movie = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarAnimatedImage"));
        QVERIFY(movie);
        QVERIFY2(framesOver(movie, 600).size() >= 2,
                 "the animation never ran: the pause below would prove nothing");

        h.shim->setSuspended(true);
        QTRY_VERIFY_WITH_TIMEOUT(movie->property("paused").toBool(), kTimeoutMs);
        const QSet<int> held = framesOver(movie, 700);
        QVERIFY2(held.size() == 1,
                 qPrintable(QStringLiteral("an unfocused avatar showed %1 frames")
                                .arg(held.size())));
        // Paused, not torn down: the same decoder, the slot still held.
        QCOMPARE(h.avatar->findChild<QQuickItem *>(
                     QStringLiteral("avatarAnimatedImage")), movie);
        QVERIFY(h.avatar->property("motionShown").toBool());
        QCOMPARE(h.bridge->motionSlotsInUseForTest(), 1);

        h.shim->setSuspended(false);
        QTRY_VERIFY_WITH_TIMEOUT(!movie->property("paused").toBool(), kTimeoutMs);
        QVERIFY(framesOver(movie, 600).size() >= 2);
        QCOMPARE(h.warnings, QStringList{});
    }

    // Reduce motion turns Play automatically into Play on hover: nothing is
    // fetched for a still avatar, and resting the pointer on it plays it. On
    // the old code Reduce motion made it unreachable.
    void reducedMotionPlaysAnAvatarOnlyUnderThePointer()
    {
        Harness h;
        prepare(h, 0);
        QObject *t = theme(h);
        QVERIFY(t);
        t->setProperty("reducedMotion", true);
        const auto restore = qScopeGuard([t] {
            t->setProperty("reducedMotion", false);
        });
        const QString mxc = QStringLiteral("mxc://x/reduced-hover");
        const int size = 48;
        QVERIFY(readyAvatar(h, mxc, size));
        QTest::qWait(300);
        QCOMPARE(fetchIndex(h, mxc, 0), -1);
        QVERIFY(!h.avatar->property("motionShown").toBool());

        QTest::mouseMove(h.window.get(), QPoint(20 + size / 2, 20 + size / 2));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = fetchIndex(h, mxc, 0)) >= 0,
                                 kTimeoutMs);
        h.client->succeed(h.client->fetches.at(original).opId,
                          solidFramesGif(64, kFrames));
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 kTimeoutMs);
        QTest::mouseMove(h.window.get(), QPoint(2, 2));
        QTRY_VERIFY_WITH_TIMEOUT(!h.avatar->property("motionShown").toBool(),
                                 kTimeoutMs);
        QCOMPARE(h.warnings, QStringList{});
    }

    // A GIF on a link-preview card. On the old code it ran under every
    // setting but Don't play, hovered or not, on screen or not, focused or
    // not.
    void aLinkPreviewGifFollowsThePolicy()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setGifAutoplay(1); // Play on hover
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("https://example.org/cat"));
        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        // No timeline pane here: the row is on screen by assignment, as the
        // pane's per-row Loader does.
        d.root->setProperty("rowOnScreen", true);
        d.root->setProperty("preview", gifCardPreview());
        QQuickItem *movie = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (movie = d.root->findChild<QQuickItem *>(
                 QStringLiteral("linkPreviewCardAnimation"))) != nullptr,
            kTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(movie->isVisible(), kTimeoutMs);
        QCOMPARE(movie->property("status").toInt(), 1); // Ready

        // Play on hover, pointer elsewhere: its first frame, not running.
        QCOMPARE(movie->property("playing").toBool(), false);
        QCOMPARE(framesOver(movie, 500).size(), 1);
        (void)d.window->grabWindow();
        const QPointF centre = movie->mapToScene(
            QPointF(movie->width() / 2, movie->height() / 2));
        QTest::mouseMove(d.window.get(), centre.toPoint());
        QTRY_VERIFY_WITH_TIMEOUT(movie->property("playing").toBool(), kTimeoutMs);
        QVERIFY(framesOver(movie, 600).size() >= 2);
        QTest::mouseMove(d.window.get(), QPoint(2, 2));
        QTRY_VERIFY_WITH_TIMEOUT(!movie->property("playing").toBool(), kTimeoutMs);

        // Play automatically: runs without the pointer, but not off screen.
        controller.settings()->setGifAutoplay(0);
        QTRY_VERIFY_WITH_TIMEOUT(movie->property("playing").toBool(), kTimeoutMs);
        d.root->setProperty("rowOnScreen", false);
        QTRY_VERIFY_WITH_TIMEOUT(!movie->property("playing").toBool(), kTimeoutMs);
        d.root->setProperty("rowOnScreen", true);
        QTRY_VERIFY_WITH_TIMEOUT(movie->property("playing").toBool(), kTimeoutMs);

#ifdef LIGHTNING_HAVE_APP_STATE_SEAM
        // Lightning loses focus: it stops on its frame; focus resumes it.
        setAppState(Qt::ApplicationActive);
        QCOMPARE(controller.animationsSuspended(), false);
        setAppState(Qt::ApplicationInactive);
        QCOMPARE(controller.animationsSuspended(), true);
        // Behaviour, not spelling: whichever property a site uses, no frame
        // may advance.
        QTest::qWait(100);
        QCOMPARE(framesOver(movie, 500).size(), 1);
        // The row leaves the screen and comes back while still unfocused:
        // `playing` goes false -> true underneath. A `paused` applied before
        // that start is lost (QMovie::start() clears it), which is how the
        // first version of this change animated an unfocused window.
        d.root->setProperty("rowOnScreen", false);
        QCoreApplication::processEvents();
        d.root->setProperty("rowOnScreen", true);
        QCoreApplication::processEvents();
        QCOMPARE(framesOver(movie, 500).size(), 1);
        // Same under Play on hover: the pointer arriving while unfocused
        // starts nothing.
        controller.settings()->setGifAutoplay(1);
        QTest::qWait(100);
        QTest::mouseMove(d.window.get(), centre.toPoint());
        QTest::qWait(150);
        QCOMPARE(framesOver(movie, 500).size(), 1);
        QTest::mouseMove(d.window.get(), QPoint(2, 2));
        controller.settings()->setGifAutoplay(0);
        setAppState(Qt::ApplicationActive);
        QTRY_VERIFY_WITH_TIMEOUT(movie->property("playing").toBool(), kTimeoutMs);
        QVERIFY(framesOver(movie, 600).size() >= 2);
#endif

        // Don't play: no source, so nothing decodes; the still is drawn.
        controller.settings()->setGifAutoplay(2);
        QTRY_VERIFY_WITH_TIMEOUT(
            movie->property("source").toUrl().isEmpty(), kTimeoutMs);
        QVERIFY(!movie->isVisible());
        QCOMPARE(d.warnings, QStringList{});
    }

    // A GIF row drawing its still (Don't play) says it is a GIF. On the old
    // code the badge belonged to the loading placeholder alone and vanished
    // with it.
    void aStillGifRowCarriesTheGifBadge()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString still = dir.filePath(QStringLiteral("still.png"));
        {
            QFile f(still);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(solidPng(64, QColor(90, 90, 90)));
        }
        AppController controller(AppController::MockBackend);
        controller.settings()->setGifAutoplay(2);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("isImage"), true);
        fixture.insert(QStringLiteral("body"), QStringLiteral("dance.gif"));
        fixture.insert(QStringLiteral("mediaFilename"),
                       QStringLiteral("dance.gif"));
        fixture.insert(QStringLiteral("mediaMimetype"),
                       QStringLiteral("image/gif"));
        fixture.insert(QStringLiteral("mediaWidth"), 64);
        fixture.insert(QStringLiteral("mediaHeight"), 64);
        fixture.insert(QStringLiteral("mediaKey"), QStringLiteral("$gif"));
        fixture.insert(QStringLiteral("mediaThumbUrl"), QUrl::fromLocalFile(still));
        fixture.insert(QStringLiteral("mediaUrl"), QUrl::fromLocalFile(still));
        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        QQuickItem *badge = nullptr;
        QList<QQuickItem *> images;
        for (QQuickItem *item : d.root->findChildren<QQuickItem *>()) {
            if (item->inherits("QQuickImage")
                && item->property("source").toUrl()
                       == QUrl::fromLocalFile(still))
                images << item;
        }
        QVERIFY2(!images.isEmpty(), "the still is not drawn from the fixture");
        QTRY_VERIFY_WITH_TIMEOUT(images.first()->property("status").toInt() == 1,
                                 kTimeoutMs);
        badge = d.root->findChild<QQuickItem *>(QStringLiteral("imageGifBadge"));
        QVERIFY2(badge, "no GIF badge on the image row");
        QTRY_VERIFY_WITH_TIMEOUT(badge->isVisible(), kTimeoutMs);
        // Drawn OVER the still, not under it: siblings paint by z, then by
        // order (a visible badge under an opaque picture is no badge).
        QQuickItem *stillItem = images.first();
        QCOMPARE(badge->parentItem(), stillItem->parentItem());
        const QList<QQuickItem *> siblings = badge->parentItem()->childItems();
        const bool above = badge->z() > stillItem->z()
            || (badge->z() == stillItem->z()
                && siblings.indexOf(badge) > siblings.indexOf(stillItem));
        QVERIFY2(above, "the GIF badge is painted under the still");
        QCOMPARE(d.warnings, QStringList{});
    }

    // Data use is not animation: Don't play leaves the speculative download
    // of a video to "Preload videos and audio" alone, and that switch still
    // stops it. On the old code Don't play (gif/autoplay 2) also refused the
    // video prefetch.
    void dontPlayLeavesVideoPreloadToItsOwnSwitch()
    {
        FakeClient bridgeClient; // declared first: outlives the controller
        struct Case { int mode; bool preload; bool expectPrefetch; };
        const QList<Case> cases = {
            { 2, true, true },   // Don't play, preload on: still prefetched
            { 0, false, false }, // Play automatically, preload off: never
        };
        int checked = 0;
        for (const Case &c : cases) {
            AppController controller(AppController::MockBackend);
            controller.mediaBridge()->setClient(&bridgeClient);
            QVERIFY(controller.mediaBridge()->supported());
            controller.settings()->setGifAutoplay(c.mode);
            controller.settings()->setVideoPrefetchEnabled(c.preload);
            const QString key = QStringLiteral("$video%1").arg(checked);
            QVariantMap fixture = baseFixture(controller);
            fixture.insert(QStringLiteral("eventId"), key);
            fixture.insert(QStringLiteral("isVideo"), true);
            fixture.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));
            fixture.insert(QStringLiteral("mediaFilename"), QStringLiteral("clip.mp4"));
            fixture.insert(QStringLiteral("mediaMimetype"), QStringLiteral("video/mp4"));
            fixture.insert(QStringLiteral("mediaWidth"), 640);
            fixture.insert(QStringLiteral("mediaHeight"), 360);
            fixture.insert(QStringLiteral("mediaSize"), 512 * 1024);
            fixture.insert(QStringLiteral("mediaSourceAvailable"), true);
            fixture.insert(QStringLiteral("mediaThumbAvailable"), true);
            fixture.insert(QStringLiteral("mediaKey"), key);
            Delegate d;
            QVERIFY(createDelegate(controller, fixture, d));
            d.root->setProperty("rowOnScreen", true);
            if (c.expectPrefetch) {
                QTRY_VERIFY2_WITH_TIMEOUT(
                    controller.mediaBridge()->hasPrefetch(key),
                    qPrintable(QStringLiteral("mode %1 preload %2: no prefetch")
                                   .arg(c.mode).arg(c.preload)), kTimeoutMs);
            } else {
                QTest::qWait(300);
                QVERIFY2(!controller.mediaBridge()->hasPrefetch(key),
                         "preload off still prefetched");
            }
            controller.mediaBridge()->setClient(nullptr);
            ++checked;
        }
        QCOMPARE(checked, 2);
    }

    // Upgrading must not start downloads a "Never" user had off in effect:
    // before the decoupling, gif/autoplay 2 refused video/audio preloading
    // whatever media/videoPrefetchEnabled said (default on). The migration
    // runs once, so turning preloading back on afterwards sticks. Without it
    // the first case reads true.
    void aNeverChoiceKeepsPreloadingOffAcrossTheUpgradeOnce()
    {
        struct Case { const char *what; QVariantMap stored; bool expect; };
        const QList<Case> cases = {
            { "Never", { { QStringLiteral("gif/autoplay"), 2 } }, false },
            { "legacy animateGifs=false, no gif/autoplay",
              { { QStringLiteral("previews/animateGifs"), false } }, false },
            { "Never with preloading explicitly on",
              { { QStringLiteral("gif/autoplay"), 2 },
                { QStringLiteral("media/videoPrefetchEnabled"), true } }, false },
            { "On hover", { { QStringLiteral("gif/autoplay"), 1 } }, true },
            { "fresh install", {}, true },
        };
        int checked = 0;
        for (const Case &c : cases) {
            {
                QSettings raw;
                raw.clear();
                for (auto it = c.stored.cbegin(); it != c.stored.cend(); ++it)
                    raw.setValue(it.key(), it.value());
                raw.sync();
            }
            SettingsManager settings;
            QVERIFY2(settings.videoPrefetchEnabled() == c.expect, c.what);
            ++checked;
        }
        QCOMPARE(checked, 5);
        // Once: the user turns preloading back on, and a restart keeps it.
        {
            QSettings raw;
            raw.clear();
            raw.setValue(QStringLiteral("gif/autoplay"), 2);
            raw.sync();
        }
        {
            SettingsManager first;
            QCOMPARE(first.videoPrefetchEnabled(), false);
            first.setVideoPrefetchEnabled(true);
        }
        {
            SettingsManager second;
            QCOMPARE(second.gifAutoplay(), 2);
            QCOMPARE(second.videoPrefetchEnabled(), true);
        }
        QSettings().clear();
    }

    // The same for a settings file exported before the decoupling (file
    // version 1): an old "Never" export imports with preloading off; a
    // version 2 export means what it says.
    void anOldNeverExportImportsWithPreloadingOff()
    {
        const auto parse = [](int version, const QJsonObject &values) {
            const QJsonObject root{ { "format", "lightning-settings" },
                                    { "version", version },
                                    { "settings", values } };
            return settingsport::parseDocument(QJsonDocument(root).toJson());
        };
        const auto preload = [](const settingsport::ParseResult &r) {
            return r.doc.settings.value(QStringLiteral("videoPrefetchEnabled"));
        };
        auto r = parse(1, { { "gifAutoplay", 2 }, { "videoPrefetchEnabled", true } });
        QVERIFY(r.ok);
        QCOMPARE(preload(r), QVariant(false));
        r = parse(1, { { "animateGifPreviews", false } });
        QCOMPARE(preload(r), QVariant(false));
        r = parse(1, { { "gifAutoplay", 0 }, { "videoPrefetchEnabled", true } });
        QCOMPARE(preload(r), QVariant(true));
        r = parse(2, { { "gifAutoplay", 2 }, { "videoPrefetchEnabled", true } });
        QCOMPARE(preload(r), QVariant(true));
        QVERIFY(!r.newerVersion);
        QCOMPARE(settingsport::kFileVersion, 2);
    }

    // The C++ fact the rule's suspension reads.
    void animationsSuspendedFollowsTheApplicationState()
    {
#ifndef LIGHTNING_HAVE_APP_STATE_SEAM
        QSKIP("QtGui private headers are not available");
#else
        AppController controller(AppController::MockBackend);
        QSignalSpy spy(&controller, &AppController::animationsSuspendedChanged);
        setAppState(Qt::ApplicationActive);
        QCOMPARE(controller.animationsSuspended(), false);
        spy.clear();
        setAppState(Qt::ApplicationInactive);
        QCOMPARE(controller.animationsSuspended(), true);
        setAppState(Qt::ApplicationHidden);
        QCOMPARE(controller.animationsSuspended(), true);
        QCOMPARE(spy.count(), 1); // Inactive -> Hidden is no change
        setAppState(Qt::ApplicationActive);
        QCOMPARE(controller.animationsSuspended(), false);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(controller.property("animationsSuspended").toBool(), false);
#endif
    }

    // Every AnimatedImage in qml/ pauses on suspension, and none reads the
    // setting or Reduce motion around MotionPolicy. Count-asserted, so a new
    // site has to be added here on purpose; a sweep that silently finds fewer
    // files is the defect it exists to catch.
    void everyAnimatedImageFollowsTheOneRule()
    {
        QDir qmlDir(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml"));
        const QStringList files =
            qmlDir.entryList({ QStringLiteral("*.qml") }, QDir::Files);
        QVERIFY2(files.size() > 100,
                 qPrintable(QStringLiteral("only %1 QML files found")
                                .arg(files.size())));
        QMap<QString, int> perFile;
        int blocks = 0;
        QStringList unpaused;
        QStringList settingReaders;
        for (const QString &name : files) {
            const QString qml = readQml(name);
            const QStringList found = animatedImageBlocks(qml);
            for (const QString &block : found) {
                ++blocks;
                perFile[name] += 1;
                if (!suspendsCorrectly(block))
                    unpaused << name;
            }
            if (name == QLatin1String("MotionPolicy.qml"))
                continue;
            // The setting governs animation only, so the one reader outside
            // MotionPolicy is the Settings row that shows and writes it.
            static const QRegularExpression read(
                QStringLiteral("app\\.settings\\.gifAutoplay(?!\\s*=[^=])"));
            auto it = read.globalMatch(qml);
            int reads = 0;
            while (it.hasNext()) {
                it.next();
                ++reads;
            }
            const int allowed =
                name == QLatin1String("SettingsScreen.qml") ? 1 : 0; // the combo's sync
            if (reads != allowed)
                settingReaders << QStringLiteral("%1 (%2)").arg(name).arg(reads);
        }
        const QMap<QString, int> expected = {
            { QStringLiteral("Avatar.qml"), 1 },
            { QStringLiteral("BannerMotion.qml"), 1 },
            { QStringLiteral("ChatBackdrop.qml"), 1 },
            { QStringLiteral("GifPicker.qml"), 1 },
            { QStringLiteral("ImageCropDialog.qml"), 1 },
            { QStringLiteral("ImageViewerOverlay.qml"), 1 },
            { QStringLiteral("MessageComposerBar.qml"), 1 },
            { QStringLiteral("MessageDelegate.qml"), 4 },
            { QStringLiteral("StickerPicker.qml"), 1 },
        };
        QCOMPARE(perFile, expected);
        QCOMPARE(blocks, 12);
        QVERIFY2(unpaused.isEmpty(),
                 qPrintable(QStringLiteral("AnimatedImage that does not stop "
                                           "while unfocused in: %1")
                                .arg(unpaused.join(QStringLiteral(", ")))));
        QVERIFY2(settingReaders.isEmpty(),
                 qPrintable(QStringLiteral("read the setting around "
                                           "MotionPolicy: %1")
                                .arg(settingReaders.join(QStringLiteral(", ")))));
        // Nor does any C++ gate data use on it: speculative downloads follow
        // "Preload videos and audio" (videoPrefetchEnabled) alone.
        int cppReaders = 0;
        int cppFiles = 0;
        QStringList cppHits;
        QDirIterator cit(QStringLiteral(LIGHTNING_SOURCE_DIR "/src"),
                         { QStringLiteral("*.cpp") }, QDir::Files,
                         QDirIterator::Subdirectories);
        while (cit.hasNext()) {
            const QString path = cit.next();
            ++cppFiles;
            if (path.endsWith(QLatin1String("/SettingsManager.cpp")))
                continue;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QString text = QString::fromUtf8(f.readAll());
            const int n = int(text.count(QStringLiteral("gifAutoplay()")));
            cppReaders += n;
            if (n)
                cppHits << path;
        }
        QVERIFY2(cppFiles > 100, qPrintable(QString::number(cppFiles)));
        QVERIFY2(cppReaders == 0, qPrintable(cppHits.join(QStringLiteral(", "))));
        // A timeline animation never runs for a row off screen.
        const QStringList delegateBlocks =
            animatedImageBlocks(readQml(QStringLiteral("MessageDelegate.qml")));
        int rowGated = 0;
        for (const QString &block : delegateBlocks) {
            const qsizetype at = block.indexOf(QStringLiteral("playing:"));
            if (at >= 0 && block.mid(at, 160).contains(QStringLiteral("rowOnScreen")))
                ++rowGated;
        }
        QCOMPARE(rowGated, 4);
    }

    // The option lives in Appearance > Motion and time, and Settings search
    // leads there.
    void theOptionLivesInAppearanceAndSearchFindsIt()
    {
        const QString qml = readQml(QStringLiteral("SettingsScreen.qml"));
        QVERIFY(!qml.isEmpty());
        // The search entry for the combo.
        const qsizetype anchor =
            qml.indexOf(QStringLiteral("anchor: \"gifAutoplayCombo\""));
        QVERIFY(anchor > 0);
        const qsizetype entry = qml.lastIndexOf(QStringLiteral("{ title:"), anchor);
        const QString text = qml.mid(entry, anchor - entry);
        QVERIFY2(text.contains(QStringLiteral("section: \"appearance\"")),
                 qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("qsTr(\"Animated images\")")));
        QVERIFY(text.contains(QStringLiteral("avatar")));
        // The combo sits in the Appearance page, under Reduce motion and
        // before the next page begins.
        const qsizetype reduce =
            qml.indexOf(QStringLiteral("objectName: \"reducedMotionCheck\""));
        const qsizetype combo =
            qml.indexOf(QStringLiteral("objectName: \"gifAutoplayCombo\""));
        const qsizetype nextPage =
            qml.indexOf(QStringLiteral("visible: root.section === \"shortcuts\""));
        QVERIFY(reduce > 0 && combo > 0 && nextPage > 0);
        QVERIFY2(reduce < combo && combo < nextPage,
                 "the Animated images combo is not in Appearance > Motion and time");
        QCOMPARE(qml.count(QStringLiteral("objectName: \"gifAutoplayCombo\"")), 1);
        // Data use lives in Privacy · Media, under one switch that says it
        // covers audio too; the Animated images hint no longer claims it.
        QVERIFY(qml.contains(QStringLiteral("text: qsTr(\"Preload videos and audio\")")));
        const qsizetype hint = qml.indexOf(QStringLiteral("objectName: \"gifAutoplayHint\""));
        QVERIFY(hint > 0);
        QVERIFY2(!qml.mid(hint, 900).contains(QStringLiteral("prefetch")),
                 "the Animated images hint still claims downloads");
        // Its three choices, by the words the user reads.
        const QString block = qml.mid(combo, 900);
        for (const char *label : { "qsTr(\"Play automatically\")",
                                   "qsTr(\"Play on hover\")",
                                   "qsTr(\"Don't play\")" })
            QVERIFY2(block.contains(QString::fromLatin1(label)), label);
    }
};

QTEST_MAIN(AnimatedImagePolicyQmlTest)
#include "AnimatedImagePolicyQmlTest.moc"
