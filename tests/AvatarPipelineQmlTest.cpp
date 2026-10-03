// Avatar pipeline: renders the production Avatar.qml through the real
// MediaBridge + MediaImageProvider stack (fake network only) and checks the
// result states:
//   * loading shows the circular skeleton, with no initials or colour flash;
//   * a decoded avatar renders over a transparent background: transparent
//     pixels reveal the surrounding surface, never the fallback colour;
//   * missing or failed avatars show initials coloured by the stable user id,
//     so a late display-name resolution cannot recolour the person;
//   * an avatar URL change resets cleanly (no stale bitmap).
#include <QtTest/QtTest>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>

#include "app/AccountAvatarStore.h"
#include "matrix/MatrixClient.h"
#include "media/MediaBridge.h"
#include "media/MediaImageProvider.h"

namespace {

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
    quint64 fetchMedia(const QString &, int, int) override { return 0; }
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
    void fail(quint64 opId, const QString &category)
    {
        Q_EMIT mediaFailed(opId, QString(), 0, category);
    }

    // Pure virtuals (inert).
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

// The one setting Avatar.qml reads: GIF autoplay (0 Always, 1 On hover,
// 2 Never).
class SettingsShim : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int gifAutoplay MEMBER m_gifAutoplay NOTIFY gifAutoplayChanged)
public:
    using QObject::QObject;
    int m_gifAutoplay = 0;
Q_SIGNALS:
    void gifAutoplayChanged();
};

// Minimal `app` context: Avatar.qml touches app.mediaBridge and, for animated
// avatars, app.settings (null here means "Never", as without settings).
class AppShim : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MediaBridge *mediaBridge READ mediaBridge CONSTANT)
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
public:
    explicit AppShim(MediaBridge *bridge, QObject *settings = nullptr,
                     QObject *parent = nullptr)
        : QObject(parent), m_bridge(bridge), m_settings(settings) {}
    MediaBridge *mediaBridge() const { return m_bridge; }
    QObject *settings() const { return m_settings; }

private:
    MediaBridge *m_bridge;
    QObject *m_settings;
};

// A real GIF: an edge x edge canvas, one full-canvas frame per colour, 100 ms
// each, looping. LZW codes are written 3 bits wide with a clear code every two
// literals, so the code table never grows past 3 bits.
QByteArray solidFramesGif(int edge, const QList<QColor> &colours)
{
    QByteArray g("GIF89a");
    const auto le16 = [&g](int v) {
        g.append(char(v & 0xff));
        g.append(char((v >> 8) & 0xff));
    };
    le16(edge);
    le16(edge);
    g.append(char(0x81)); // global colour table of four entries
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
        g.append(char(0x02)); // LZW minimum code size
        QList<int> codes;
        const int pixels = edge * edge;
        for (int p = 0; p < pixels; ++p) {
            if (p % 2 == 0)
                codes.append(4); // clear
            codes.append(f);
        }
        codes.append(5); // end of information
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

QByteArray jpegOf(int edge, const QColor &color)
{
    QImage image(edge, edge, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG");
    return bytes;
}

// A synthetic avatar: left half opaque red, right half fully transparent,
// with a soft alpha edge. After circular masking the transparent half sits
// inside the visible circle.
QByteArray halfTransparentPng(int edge)
{
    QImage image(edge, edge, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.fillRect(0, 0, edge / 2 - 2, edge, QColor(220, 30, 30));
    // Alpha gradient edge pixels.
    for (int x = edge / 2 - 2; x < edge / 2 + 2; ++x) {
        const int alpha = qMax(0, 255 - (x - (edge / 2 - 2)) * 85);
        p.fillRect(x, 0, 1, edge, QColor(220, 30, 30, alpha));
    }
    p.end();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
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

bool colorsClose(const QColor &a, const QColor &b, int tolerance = 12)
{
    return qAbs(a.red() - b.red()) <= tolerance
        && qAbs(a.green() - b.green()) <= tolerance
        && qAbs(a.blue() - b.blue()) <= tolerance;
}

const QColor kSurface(0, 160, 60); // distinctive backdrop, not in the palette

} // namespace

class AvatarPipelineQmlTest : public QObject
{
    Q_OBJECT

private:
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

    // Core stack without an Avatar: for tests that manipulate the bridge
    // (pre-marked failures) or spawn several Avatars against one bridge.
    bool prepareCore(Harness &h, bool withSettings = false)
    {
        h.client = std::make_unique<FakeClient>();
        h.bridge = std::make_unique<MediaBridge>();
        h.bridge->setClient(h.client.get());
        if (withSettings)
            h.settings = std::make_unique<SettingsShim>();
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
        h.window->setColor(kSurface);
        return true;
    }

    bool loadAvatar(Harness &h, int size, const QString &mxc,
                    const QString &name, const QString &colorKey)
    {
        QSignalSpy createdSpy(h.engine.get(),
                              &QQmlApplicationEngine::objectCreated);
        // Initial properties mirror production: the delegate binds
        // mxc/name/colorKey before Component.onCompleted's first refresh().
        h.engine->setInitialProperties({
            { QStringLiteral("size"), size },
            { QStringLiteral("name"), name },
            { QStringLiteral("colorKey"), colorKey },
            { QStringLiteral("mxc"), mxc },
        });
        h.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Avatar"));
        if (createdSpy.isEmpty() && !createdSpy.wait(3000))
            return false;
        h.avatar = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!h.avatar)
            return false;
        h.window->resize(size + 40, size + 40);
        h.avatar->setParentItem(h.window->contentItem());
        h.avatar->setPosition(QPointF(20, 20));
        h.window->show();
        QCoreApplication::processEvents();
        return true;
    }

    bool createAvatar(Harness &h, int size, const QString &mxc,
                      const QString &name, const QString &colorKey)
    {
        return prepareCore(h) && loadAvatar(h, size, mxc, name, colorKey);
    }

    // Additional Avatar instances against the same engine and bridge.
    static QQuickItem *spawnAvatar(Harness &h, QQmlComponent &component,
                                   int size, const QString &mxc,
                                   const QString &name,
                                   const QString &colorKey)
    {
        QObject *object = component.createWithInitialProperties(
            {
                { QStringLiteral("size"), size },
                { QStringLiteral("name"), name },
                { QStringLiteral("colorKey"), colorKey },
                { QStringLiteral("mxc"), mxc },
            },
            h.engine->rootContext());
        auto *item = qobject_cast<QQuickItem *>(object);
        if (!item) {
            delete object;
            return nullptr;
        }
        item->setParentItem(h.window->contentItem());
        return item;
    }

    static QString state(const Harness &h)
    {
        return h.avatar->property("presentationState").toString();
    }

    // Every avatar identity is fetched at one canonical edge regardless of
    // render size, so there is one fetch per identity.
    static int finalEdgeFetchIndex(const Harness &h, const QString &mxc,
                                   int size)
    {
        Q_UNUSED(size);
        for (int i = h.client->fetches.size() - 1; i >= 0; --i) {
            const auto &f = h.client->fetches.at(i);
            if (f.key == mxc && f.width == 224)
                return i;
        }
        return -1;
    }

    // The index of the fetch for the original (edge 0), or -1.
    static int originalFetchIndex(const Harness &h, const QString &mxc)
    {
        for (int i = h.client->fetches.size() - 1; i >= 0; --i) {
            const auto &f = h.client->fetches.at(i);
            if (f.key == mxc && f.width == 0)
                return i;
        }
        return -1;
    }

    static QColor centre(const Harness &h, int size)
    {
        const QImage frame = h.window->grabWindow();
        const qreal dpr = frame.devicePixelRatio();
        return QColor(frame.pixel(int((20 + size / 2) * dpr),
                                  int((20 + size / 2) * dpr)));
    }

    // An animated avatar at `size` whose still thumbnail (a PNG, as Synapse
    // renders a GIF) has loaded.
    bool readyAnimatedCandidate(Harness &h, int size, const QString &mxc,
                                int gifAutoplay, const QByteArray &thumb)
    {
        if (!prepareCore(h, true))
            return false;
        h.settings->m_gifAutoplay = gifAutoplay;
        if (!loadAvatar(h, size, mxc, QStringLiteral("Anim"),
                        QStringLiteral("@anim:x")))
            return false;
        // The offscreen suite may render in software, where the shape mask
        // cannot draw and motion is off by default; the logic is what these
        // cases test (pixels only where the backend can show them).
        h.avatar->setProperty("motionMaskable", true);
        const int thumbIndex = finalEdgeFetchIndex(h, mxc, size);
        if (thumbIndex < 0)
            return false;
        h.client->succeed(h.client->fetches.at(thumbIndex).opId, thumb);
        return QTest::qWaitFor([&h] {
            return state(h) == QStringLiteral("ready");
        }, 5000);
    }

private Q_SLOTS:
    // Autoplay "Always": the still thumbnail shows first, then the original is
    // probed and plays, masked to the circle; the pixels actually change
    // across frames. Leaving the screen tears the animation down and returns
    // its slot.
    void anAnimatedAvatarPlaysThenStopsOffScreen()
    {
        Harness h;
        const QString mxc = QStringLiteral("mxc://x/animated");
        const int size = 48;
        QVERIFY(readyAnimatedCandidate(h, size, mxc, 0,
                                       solidPng(64, QColor(200, 200, 200))));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        const QList<QColor> colours = { QColor(255, 0, 0), QColor(0, 255, 0),
                                        QColor(0, 0, 255),
                                        QColor(255, 255, 0) };
        h.client->succeed(h.client->fetches.at(original).opId,
                          solidFramesGif(64, colours));
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        QCOMPARE(h.bridge->motionSlotsInUseForTest(), 1);

        // Playing: the decoder advances through frames.
        auto *movie = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarAnimatedImage"));
        QVERIFY(movie);
        QVERIFY(movie->property("playing").toBool());
        QSet<int> frames;
        QSet<QRgb> seen;
        QStringList sampled;
        for (int i = 0; i < 16 && (frames.size() < 3 || seen.size() < 3); ++i) {
            frames.insert(movie->property("currentFrame").toInt());
            const QColor c = centre(h, size);
            sampled << c.name();
            for (const QColor &k : colours) {
                if (colorsClose(c, k, 40))
                    seen.insert(k.rgb());
            }
            QTest::qWait(60);
        }
        QVERIFY2(frames.size() >= 2,
                 qPrintable(QStringLiteral("the animation showed %1 frame(s)")
                                .arg(frames.size())));
        // The pixels: the shape mask is a shader effect, which the software
        // scene graph cannot draw, so they are only asserted where it can.
        // The GUI run on real displays covers the rest.
        const auto api = h.window->rendererInterface()->graphicsApi();
        if (api == QSGRendererInterface::Software) {
            qInfo("software scene graph: pixel check skipped (sampled %s)",
                  qPrintable(sampled.join(QLatin1Char(' '))));
        } else {
            QVERIFY2(seen.size() >= 2,
                     qPrintable(QStringLiteral("the avatar centre showed %1 of "
                                               "the animation's colours: %2")
                                    .arg(seen.size())
                                    .arg(sampled.join(QLatin1Char(' ')))));
            // Masked: the corner outside the circle is the window surface.
            const QImage frame = h.window->grabWindow();
            const qreal dpr = frame.devicePixelRatio();
            QVERIFY(colorsClose(
                QColor(frame.pixel(int(21 * dpr), int(21 * dpr))), kSurface));
        }

        h.avatar->setProperty("onScreen", false);
        QTRY_VERIFY_WITH_TIMEOUT(!h.avatar->property("motionShown").toBool(),
                                 5000);
        QCOMPARE(h.bridge->motionSlotsInUseForTest(), 0);
        auto *still = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarImage"));
        QVERIFY(still && still->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(colorsClose(centre(h, size),
                                             QColor(200, 200, 200), 20),
                                 5000);
        QCOMPARE(h.warnings, QStringList{});
    }

    // "Never" and reduced motion keep the still picture and fetch nothing
    // beyond the thumbnail.
    void neverAndReducedMotionFetchNoOriginal()
    {
        {
            Harness h;
            const QString mxc = QStringLiteral("mxc://x/never");
            QVERIFY(readyAnimatedCandidate(h, 48, mxc, 2,
                                           solidPng(64, Qt::gray)));
            QTest::qWait(200);
            QCOMPARE(originalFetchIndex(h, mxc), -1);
            QVERIFY(!h.avatar->property("motionShown").toBool());
        }
        {
            Harness h;
            QVERIFY(prepareCore(h, true));
            QObject *theme = h.engine->singletonInstance<QObject *>(
                QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
            QVERIFY(theme);
            theme->setProperty("reducedMotion", true);
            const QString mxc = QStringLiteral("mxc://x/reduced");
            QVERIFY(loadAvatar(h, 48, mxc, QStringLiteral("R"),
                               QStringLiteral("@r:x")));
            // Only reduced motion may be what stops it.
            h.avatar->setProperty("motionMaskable", true);
            h.client->succeed(
                h.client->fetches.at(finalEdgeFetchIndex(h, mxc, 48)).opId,
                solidPng(64, Qt::gray));
            QTRY_COMPARE_WITH_TIMEOUT(state(h), QStringLiteral("ready"), 5000);
            QTest::qWait(200);
            QCOMPARE(originalFetchIndex(h, mxc), -1);
            theme->setProperty("reducedMotion", false);
        }
    }

    // "On hover": nothing is fetched until the pointer rests on the avatar,
    // and then any format is probed, even behind a JPEG thumbnail.
    void onHoverProbesOnlyUnderThePointer()
    {
        Harness h;
        const QString mxc = QStringLiteral("mxc://x/hover");
        const int size = 48;
        QVERIFY(readyAnimatedCandidate(h, size, mxc, 1,
                                       jpegOf(64, QColor(90, 90, 90))));
        QTest::qWait(300);
        QCOMPARE(originalFetchIndex(h, mxc), -1);
        QTest::mouseMove(h.window.get(), QPoint(20 + size / 2, 20 + size / 2));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        h.client->succeed(h.client->fetches.at(original).opId,
                          solidFramesGif(64, { Qt::red, Qt::blue }));
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        // Leaving stops it.
        QTest::mouseMove(h.window.get(), QPoint(2, 2));
        QTRY_VERIFY_WITH_TIMEOUT(!h.avatar->property("motionShown").toBool(),
                                 5000);
        QCOMPARE(h.bridge->motionSlotsInUseForTest(), 0);
    }

    // A failed animation probe is about the animation only: the still
    // picture stays loaded, not replaced by initials.
    void aFailedProbeLeavesTheStillAvatarAlone()
    {
        Harness h;
        const QString mxc = QStringLiteral("mxc://x/probe-fails");
        QVERIFY(readyAnimatedCandidate(h, 48, mxc, 0, solidPng(64, Qt::gray)));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        h.client->fail(h.client->fetches.at(original).opId,
                       QStringLiteral("network"));
        QCoreApplication::processEvents();
        QCOMPARE(state(h), QStringLiteral("ready"));
        QVERIFY(!h.avatar->property("fetchFailed").toBool());
        auto *still = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarImage"));
        QVERIFY(still && still->isVisible());
        // The retry sweep of that mark does not disturb it either.
        h.bridge->setFailureRetryMsForTest(0);
        h.bridge->checkInflightTimeouts();
        QCoreApplication::processEvents();
        QCOMPARE(state(h), QStringLiteral("ready"));
        QVERIFY(!h.avatar->property("fetchFailed").toBool());
        QCOMPARE(h.warnings, QStringList{});
    }

    // The bridge's scratch file can be evicted while an avatar holds its URL.
    // Activation asks again, and a load error retries once, so either way
    // the animation is fetched back rather than latched off.
    void anEvictedAnimationIsFetchedBack()
    {
        Harness h;
        const QString mxc = QStringLiteral("mxc://x/evicted");
        QVERIFY(readyAnimatedCandidate(h, 40, mxc, 0, solidPng(64, Qt::gray)));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        const QByteArray gif = solidFramesGif(16, { Qt::red, Qt::blue });
        h.client->succeed(h.client->fetches.at(original).opId, gif);
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        const auto filePath = [&h, &mxc] {
            return QUrl(h.bridge->avatarAnimationSource(mxc, true))
                .toLocalFile();
        };

        // 1. Evicted while off screen: coming back asks the bridge again.
        h.avatar->setProperty("onScreen", false);
        QTRY_VERIFY_WITH_TIMEOUT(!h.avatar->property("motionShown").toBool(),
                                 5000);
        QVERIFY(QFile::remove(filePath()));
        h.avatar->setProperty("onScreen", true);
        QTRY_COMPARE_WITH_TIMEOUT(h.avatar->property("_motionSrc").toString(),
                                  QString(), 5000);
        const int refetch = originalFetchIndex(h, mxc);
        QVERIFY2(refetch > original, "coming back on screen reused a URL "
                                     "whose file is gone");
        h.client->succeed(h.client->fetches.at(refetch).opId, gif);
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);

        // 2. Evicted while waiting for a slot: the bridge is asked again
        // before the freed slot is taken, so the file is fetched back.
        QObject holders;
        for (int i = h.bridge->motionSlotsInUseForTest();
             i < MediaBridge::kMaxMotionSlots; ++i)
            QVERIFY(h.bridge->claimMotionSlot(new QObject(&holders)));
        QQmlComponent component(h.engine.get());
        component.loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Avatar"));
        QQuickItem *second = spawnAvatar(h, component, 40, mxc,
                                         QStringLiteral("Anim"),
                                         QStringLiteral("@anim:x"));
        QVERIFY(second);
        second->setProperty("motionMaskable", true);
        QTRY_VERIFY_WITH_TIMEOUT(
            !second->property("_motionSrc").toString().isEmpty(), 5000);
        QVERIFY(!second->property("motionShown").toBool()); // no slot
        const int before = originalFetchIndex(h, mxc);
        QVERIFY(QFile::remove(filePath()));
        h.bridge->releaseMotionSlot(holders.children().first());
        int again = -1;
        QTRY_VERIFY_WITH_TIMEOUT(
            (again = originalFetchIndex(h, mxc)) > before, 5000);
        h.client->succeed(h.client->fetches.at(again).opId, gif);
        QTRY_VERIFY_WITH_TIMEOUT(second->property("motionShown").toBool(),
                                 5000);
        QVERIFY(!second->property("_motionFailed").toBool());
        delete second;
    }

    // A URL that fails to load is asked for once more: it may name a file
    // that is gone. A second failure leaves the avatar still.
    void aUrlThatFailsToLoadIsAskedForAgainOnce()
    {
        Harness h;
        const QString mxc = QStringLiteral("mxc://x/stale-url");
        QVERIFY(readyAnimatedCandidate(h, 40, mxc, 0, solidPng(64, Qt::gray)));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        h.client->succeed(h.client->fetches.at(original).opId,
                          solidFramesGif(16, { Qt::red, Qt::blue }));
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        const QString good = h.avatar->property("_motionSrc").toString();
        // A stale URL, as a delegate could hold across an eviction.
        h.avatar->setProperty(
            "_motionSrc",
            QUrl::fromLocalFile(QDir::tempPath()
                                + QStringLiteral("/lightning-gone.gif"))
                .toString());
        QTRY_COMPARE_WITH_TIMEOUT(h.avatar->property("_motionSrc").toString(),
                                  good, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        QVERIFY(!h.avatar->property("_motionFailed").toBool());
    }

    // Playing through an eviction: the bridge must not delete a file an
    // AnimatedImage is showing. A decoder whose file vanishes stops at its
    // next loop without an error, frozen on its last frame for good.
    void aPlayingAnimationSurvivesEviction()
    {
        Harness h;
        const QString mxc = QStringLiteral("mxc://x/playing");
        QVERIFY(readyAnimatedCandidate(h, 40, mxc, 0, solidPng(64, Qt::gray)));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        h.client->succeed(h.client->fetches.at(original).opId,
                          solidFramesGif(16, { Qt::red, Qt::blue }));
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        const QString playing =
            QUrl(h.avatar->property("_motionSrc").toString()).toLocalFile();
        QVERIFY(QFile::exists(playing));
        QTRY_COMPARE_WITH_TIMEOUT(h.bridge->animationHoldsForTest(), 1, 5000);

        // More animations than the scratch cache keeps, the playing one the
        // least recently asked for (animated thumbnails: no second fetch).
        QString firstChurned;
        for (int i = 0; i < 70; ++i) {
            const QString other = QStringLiteral("mxc://x/churn%1").arg(i);
            h.bridge->avatarSource(other, 40);
            const int index = finalEdgeFetchIndex(h, other, 40);
            QVERIFY(index >= 0);
            h.client->succeed(h.client->fetches.at(index).opId,
                              solidFramesGif(8, { Qt::green, Qt::yellow }));
            const QString url = h.bridge->avatarAnimationSource(other, false);
            QVERIFY2(!url.isEmpty(), qPrintable(other));
            if (i == 0)
                firstChurned = QUrl(url).toLocalFile();
        }
        // The churn really evicted, and the playing file was spared.
        QVERIFY(!QFile::exists(firstChurned));
        QVERIFY2(QFile::exists(playing), "a playing animation's file was "
                                         "evicted from under it");
        // And it still plays: frames keep advancing past loop boundaries.
        auto *movie = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarAnimatedImage"));
        QVERIFY(movie);
        QSet<int> frames;
        for (int i = 0; i < 20 && frames.size() < 2; ++i) {
            frames.insert(movie->property("currentFrame").toInt());
            QTest::qWait(60);
        }
        QCOMPARE(frames.size(), 2);
    }

    // A file written again is a new URL, so an avatar still showing the old
    // one reloads instead of staying frozen on a file that is gone.
    void aRewrittenAnimationIsANewUrlAndReloads()
    {
        Harness h;
        const QString mxc = QStringLiteral("mxc://x/rewritten");
        QVERIFY(readyAnimatedCandidate(h, 40, mxc, 0, solidPng(64, Qt::gray)));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        const QByteArray gif = solidFramesGif(16, { Qt::red, Qt::blue });
        h.client->succeed(h.client->fetches.at(original).opId, gif);
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        const QString before = h.avatar->property("_motionSrc").toString();
        // The file goes while it plays (not by eviction, which a hold now
        // prevents), and the decoder stops at its next loop.
        QVERIFY(QFile::remove(QUrl(before).toLocalFile()));
        QTest::qWait(500);
        // Someone asks for it: fetched and written again.
        QCOMPARE(h.bridge->avatarAnimationSource(mxc, true), QString());
        const int refetch = originalFetchIndex(h, mxc);
        QVERIFY(refetch > original);
        h.client->succeed(h.client->fetches.at(refetch).opId, gif);
        QTRY_VERIFY_WITH_TIMEOUT(
            h.avatar->property("_motionSrc").toString() != before, 5000);
        QVERIFY(QFile::exists(
            QUrl(h.avatar->property("_motionSrc").toString()).toLocalFile()));
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        auto *movie = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarAnimatedImage"));
        QVERIFY(movie);
        QSet<int> frames;
        for (int i = 0; i < 30 && frames.size() < 2; ++i) {
            frames.insert(movie->property("currentFrame").toInt());
            QTest::qWait(60);
        }
        QCOMPARE(frames.size(), 2);
    }

    // The shared hold: it follows its AnimatedImage's source, keeps the file
    // through an eviction, and goes with the image.
    void anAnimationFileHoldFollowsItsImage()
    {
        Harness h;
        QVERIFY(prepareCore(h, true));
        // A bridge file, written through the avatar path.
        const QString mxc = QStringLiteral("mxc://x/held-by-image");
        h.bridge->avatarSource(mxc, 40);
        h.client->succeed(h.client->fetches.last().opId,
                          solidFramesGif(8, { Qt::red, Qt::blue }));
        const QString url = h.bridge->avatarAnimationSource(mxc, false);
        QVERIFY(!url.isEmpty());
        QQmlComponent component(h.engine.get());
        component.setData(QByteArrayLiteral(
            "import QtQuick\n"
            "import MatrixClient\n"
            "AnimatedImage {\n"
            "    width: 16; height: 16\n"
            "    cache: false\n"
            "    AnimationFileHold {}\n"
            "}\n"), QUrl(QStringLiteral("qrc:/test/HoldProbe.qml")));
        QObject *image = component.createWithInitialProperties(
            { { QStringLiteral("source"), url } }, h.engine->rootContext());
        QVERIFY2(image, qPrintable(component.errorString()));
        QCOMPARE(h.bridge->animationHoldsForTest(), 1);

        for (int i = 0; i < 70; ++i) {
            const QString other = QStringLiteral("mxc://x/c%1").arg(i);
            h.bridge->avatarSource(other, 40);
            h.client->succeed(h.client->fetches.last().opId,
                              solidFramesGif(8, { Qt::green, Qt::yellow }));
            QVERIFY(!h.bridge->avatarAnimationSource(other, false).isEmpty());
        }
        QVERIFY2(QFile::exists(QUrl(url).toLocalFile()),
                 "a held file was evicted");

        image->setProperty("source", QString());
        QCOMPARE(h.bridge->animationHoldsForTest(), 0);
        image->setProperty("source", url);
        QCOMPARE(h.bridge->animationHoldsForTest(), 1);
        delete image;
        QCOMPARE(h.bridge->animationHoldsForTest(), 0);
    }

    // Every AnimatedImage that shows a bridge scratch file declares the hold.
    // Read from the compiled module, which is what runs.
    void everyBridgeAnimationDeclaresItsHold()
    {
        const QStringList files = {
            QStringLiteral("Avatar.qml"),
            QStringLiteral("BannerMotion.qml"),
            QStringLiteral("StickerPicker.qml"),
            QStringLiteral("MessageDelegate.qml"),
            QStringLiteral("ImageViewerOverlay.qml"),
        };
        int sites = 0;
        for (const QString &name : files) {
            QFile file(QStringLiteral(":/qt/qml/MatrixClient/") + name);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(name));
            const QString src = QString::fromUtf8(file.readAll());
            const int images = int(src.count(QStringLiteral("AnimatedImage {")));
            const int holds = int(src.count(QStringLiteral("AnimationFileHold {}")));
            QVERIFY2(images > 0, qPrintable(name + QStringLiteral(
                                                "has no AnimatedImage: this "
                                                "row tests nothing")));
            QVERIFY2(holds == images,
                     qPrintable(QStringLiteral("%1: %2 AnimatedImage, %3 holds")
                                    .arg(name).arg(images).arg(holds)));
            sites += images;
        }
        QCOMPARE(sites, 8);
    }

    // A decoder whose file vanishes stops at its next loop with no error. The
    // avatar notices the stop by itself and asks again, which fetches the
    // animation back under a new URL.
    void aStalledDecoderAsksForItsFileAgain()
    {
        Harness h;
        const QString mxc = QStringLiteral("mxc://x/stalled");
        QVERIFY(readyAnimatedCandidate(h, 40, mxc, 0, solidPng(64, Qt::gray)));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        const QByteArray gif = solidFramesGif(16, { Qt::red, Qt::blue });
        h.client->succeed(h.client->fetches.at(original).opId, gif);
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        const QString before = h.avatar->property("_motionSrc").toString();
        QVERIFY(QFile::remove(QUrl(before).toLocalFile()));
        // Nothing else asks: the avatar's own stall handling must.
        int refetch = -1;
        QTRY_VERIFY_WITH_TIMEOUT(
            (refetch = originalFetchIndex(h, mxc)) > original, 5000);
        h.client->succeed(h.client->fetches.at(refetch).opId, gif);
        QTRY_VERIFY_WITH_TIMEOUT(
            h.avatar->property("_motionSrc").toString() != before
                && h.avatar->property("motionShown").toBool(), 5000);
        auto *movie = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarAnimatedImage"));
        QVERIFY(movie);
        QSet<int> frames;
        for (int i = 0; i < 30 && frames.size() < 2; ++i) {
            frames.insert(movie->property("currentFrame").toInt());
            QTest::qWait(60);
        }
        QCOMPARE(frames.size(), 2);
    }

    // A new URL on an item whose decoder had stopped still plays:
    // AnimatedImage clears `playing` when its movie stops, and would load the
    // new source paused on its first frame.
    void aNewSourceOnAStoppedDecoderPlays()
    {
        Harness h;
        const QString mxc = QStringLiteral("mxc://x/swapped");
        QVERIFY(readyAnimatedCandidate(h, 40, mxc, 0, solidPng(64, Qt::gray)));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        const QByteArray gif = solidFramesGif(16, { Qt::red, Qt::blue });
        h.client->succeed(h.client->fetches.at(original).opId, gif);
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        auto *movie = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarAnimatedImage"));
        QVERIFY(movie);
        const QString before = h.avatar->property("_motionSrc").toString();
        // The decoder stops (as it does when its file goes); its file is
        // still there, so asking changes nothing yet.
        movie->setProperty("playing", false);
        QTest::qWait(100);
        QCOMPARE(h.avatar->property("_motionSrc").toString(), before);
        // Then the file goes and is fetched back: the SAME item gets the new
        // URL.
        QVERIFY(QFile::remove(QUrl(before).toLocalFile()));
        QCOMPARE(h.bridge->avatarAnimationSource(mxc, true), QString());
        const int refetch = originalFetchIndex(h, mxc);
        QVERIFY(refetch > original);
        h.client->succeed(h.client->fetches.at(refetch).opId, gif);
        QTRY_VERIFY_WITH_TIMEOUT(
            h.avatar->property("_motionSrc").toString() != before, 5000);
        QCOMPARE(h.avatar->findChild<QQuickItem *>(
                     QStringLiteral("avatarAnimatedImage")),
                 movie);
        QSet<int> frames;
        for (int i = 0; i < 30 && frames.size() < 2; ++i) {
            frames.insert(movie->property("currentFrame").toInt());
            QTest::qWait(60);
        }
        QCOMPARE(frames.size(), 2);
    }

    // An avatar outside its Flickable's viewport (a list's cache delegate, a
    // lobby row scrolled away): no binding loop, no probe, nothing playing;
    // scrolled into view, it asks.
    //
    // The loop fires only when the viewport check FLIPS `_inViewport` from
    // inside `_wantsMotion`'s own change: a ListView lays a delegate out after
    // creating it, so the avatar is checked while it still sits in view and
    // has moved out by the time its picture is ready. The fixture reproduces
    // exactly that, and first proves the warning capture sees a binding loop.
    void anAvatarOutsideItsViewportStaysStillWithoutABindingLoop()
    {
        Harness h;
        QVERIFY(prepareCore(h, true));
        {
            // The capture works: a deliberate loop is seen.
            QQmlComponent loop(h.engine.get());
            loop.setData(QByteArrayLiteral(
                "import QtQuick\n"
                "Item {\n"
                "    property int a: b + 1\n"
                "    property int b: a + 1\n"
                "}\n"), QUrl(QStringLiteral("qrc:/test/LoopProbe.qml")));
            std::unique_ptr<QObject> looping(loop.create());
            QVERIFY(looping);
            QCoreApplication::processEvents();
            bool seen = false;
            for (const QString &w : std::as_const(h.warnings))
                seen = seen || w.contains(QStringLiteral("Binding loop"));
            QVERIFY2(seen, "the warning capture cannot see a binding loop, "
                           "so this case could not see one either");
            h.warnings.clear();
        }
        const QString mxc = QStringLiteral("mxc://x/deep");
        QQmlComponent component(h.engine.get());
        component.setData(QByteArrayLiteral(
            "import QtQuick\n"
            "import MatrixClient\n"
            "Flickable {\n"
            "    width: 200; height: 100\n"
            "    contentWidth: 200; contentHeight: 1000\n"
            "    property alias probe: deep\n"
            "    Avatar {\n"
            "        id: deep\n"
            "        y: 10; size: 48\n"
            "        mxc: \"mxc://x/deep\"; name: \"Deep\"; colorKey: \"@d:x\"\n"
            "        motionMaskable: true\n"
            "    }\n"
            "}\n"), QUrl(QStringLiteral("qrc:/test/ViewportProbe.qml")));
        QObject *root = component.create(h.engine->rootContext());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *flick = qobject_cast<QQuickItem *>(root);
        QVERIFY(flick);
        flick->setParentItem(h.window->contentItem());
        h.window->resize(240, 140);
        h.window->show();
        auto *deep = qvariant_cast<QQuickItem *>(root->property("probe"));
        QVERIFY(deep);
        QCoreApplication::processEvents();
        // Laid out away after it was checked, as a list delegate is: nothing
        // re-checks yet (the avatar is not ready), so the answer is stale.
        deep->setY(600);
        QCoreApplication::processEvents();
        QVERIFY2(deep->property("_inViewport").toBool(),
                 "the fixture no longer reaches readiness with a stale "
                 "in-view answer, so it cannot hit the loop");
        const int thumb = finalEdgeFetchIndex(h, mxc, 48);
        QVERIFY(thumb >= 0);
        h.client->succeed(h.client->fetches.at(thumb).opId,
                          solidPng(64, Qt::gray));
        QTRY_COMPARE_WITH_TIMEOUT(
            deep->property("presentationState").toString(),
            QStringLiteral("ready"), 5000);
        QTest::qWait(300);
        QVERIFY(!deep->property("_inViewport").toBool());
        QCOMPARE(originalFetchIndex(h, mxc), -1); // not probed off screen
        for (const QString &w : std::as_const(h.warnings))
            QVERIFY2(!w.contains(QStringLiteral("Binding loop")),
                     qPrintable(w));

        flick->setProperty("contentY", 560);
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        QVERIFY(deep->property("_inViewport").toBool());
        h.client->succeed(h.client->fetches.at(original).opId,
                          solidFramesGif(16, { Qt::red, Qt::blue }));
        QTRY_VERIFY_WITH_TIMEOUT(deep->property("motionShown").toBool(), 5000);
        // Scrolled away again: torn down, slot returned.
        flick->setProperty("contentY", 0);
        QTRY_VERIFY_WITH_TIMEOUT(!deep->property("motionShown").toBool(),
                                 5000);
        QCOMPARE(h.bridge->motionSlotsInUseForTest(), 0);
        for (const QString &w : std::as_const(h.warnings))
            QVERIFY2(!w.contains(QStringLiteral("Binding loop")),
                     qPrintable(w));
        delete root;
    }

    // A banner inside a Flickable stops when scrolled out, and holds its file
    // while it plays.
    void aBannerStopsOutOfViewAndHoldsItsFile()
    {
        Harness h;
        QVERIFY(prepareCore(h, true));
        const QString mxc = QStringLiteral("mxc://x/banner");
        // The still picture's full payload, as wideImageSource() fetches it.
        h.bridge->wideImageSource(mxc);
        QVERIFY(!h.client->fetches.isEmpty());
        h.client->succeed(h.client->fetches.last().opId,
                          solidFramesGif(30, { Qt::red, Qt::blue }));
        QQmlComponent component(h.engine.get());
        component.setData(QByteArrayLiteral(
            "import QtQuick\n"
            "import MatrixClient\n"
            "Flickable {\n"
            "    width: 200; height: 100\n"
            "    contentWidth: 200; contentHeight: 1000\n"
            "    property alias probe: motion\n"
            "    BannerMotion {\n"
            "        id: motion\n"
            "        y: 20; width: 150; height: 50\n"
            "        mxc: \"mxc://x/banner\"; stillReady: true\n"
            "    }\n"
            "}\n"), QUrl(QStringLiteral("qrc:/test/BannerProbe.qml")));
        QObject *root = component.create(h.engine->rootContext());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *flick = qobject_cast<QQuickItem *>(root);
        flick->setParentItem(h.window->contentItem());
        h.window->resize(240, 140);
        h.window->show();
        auto *motion = qvariant_cast<QQuickItem *>(root->property("probe"));
        QVERIFY(motion);
        QTRY_VERIFY_WITH_TIMEOUT(motion->property("shown").toBool(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.bridge->animationHoldsForTest(), 1, 5000);

        flick->setProperty("contentY", 500); // banner fully out
        QTRY_VERIFY_WITH_TIMEOUT(!motion->property("shown").toBool(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(h.bridge->animationHoldsForTest(), 0, 5000);
        flick->setProperty("contentY", 0);
        QTRY_VERIFY_WITH_TIMEOUT(motion->property("shown").toBool(), 5000);
        for (const QString &w : std::as_const(h.warnings))
            QVERIFY2(!w.contains(QStringLiteral("Binding loop")),
                     qPrintable(w));
        delete root;
    }

    // Two avatars of one identity share one probe; with every slot taken a
    // third stays still until one is freed.
    void playingAvatarsAreBoundedBySlots()
    {
        Harness h;
        const QString mxc = QStringLiteral("mxc://x/slots");
        QVERIFY(readyAnimatedCandidate(h, 40, mxc, 0, solidPng(64, Qt::gray)));
        int original = -1;
        QTRY_VERIFY_WITH_TIMEOUT((original = originalFetchIndex(h, mxc)) >= 0,
                                 5000);
        h.client->succeed(h.client->fetches.at(original).opId,
                          solidFramesGif(16, { Qt::red, Qt::blue }));
        QTRY_VERIFY_WITH_TIMEOUT(h.avatar->property("motionShown").toBool(),
                                 5000);
        QObject holders;
        QList<QObject *> others;
        for (int i = 1; i < MediaBridge::kMaxMotionSlots; ++i) {
            others.append(new QObject(&holders));
            QVERIFY(h.bridge->claimMotionSlot(others.last()));
        }

        QQmlComponent component(h.engine.get());
        component.loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Avatar"));
        QQuickItem *second = spawnAvatar(h, component, 40, mxc,
                                         QStringLiteral("Anim"),
                                         QStringLiteral("@anim:x"));
        QVERIFY(second);
        second->setProperty("motionMaskable", true);
        QTRY_COMPARE_WITH_TIMEOUT(
            second->property("presentationState").toString(),
            QStringLiteral("ready"), 5000);
        QTest::qWait(200);
        QVERIFY(!second->property("motionShown").toBool());
        QCOMPARE(originalFetchIndex(h, mxc), original); // no second probe

        h.bridge->releaseMotionSlot(others.first());
        QTRY_VERIFY_WITH_TIMEOUT(second->property("motionShown").toBool(),
                                 5000);
        QCOMPARE(h.bridge->motionSlotsInUseForTest(),
                 MediaBridge::kMaxMotionSlots);
        // A destroyed avatar gives its slot back.
        delete second;
        QCOMPARE(h.bridge->motionSlotsInUseForTest(),
                 MediaBridge::kMaxMotionSlots - 1);
    }

    // The loading placeholder must stay inside the avatar's shape. A sweeping
    // band is clipped to the bounding RECTANGLE, so on a disc it drew square
    // ends over the corners. Old code showed a visible band child here.
    void loadingSkeletonDrawsNoRectangularBand()
    {
        for (bool circle : { true, false }) {
            Harness h;
            QVERIFY(createAvatar(h, 40, QStringLiteral("mxc://x/shape"),
                                 QStringLiteral("Matas"),
                                 QStringLiteral("@matas:x")));
            h.avatar->setProperty("circle", circle);
            QCOMPARE(state(h), QStringLiteral("loading"));
            auto *skeleton = h.avatar->findChild<QQuickItem *>(
                QStringLiteral("avatarSkeleton"));
            QVERIFY(skeleton && skeleton->isVisible());
            QVERIFY(skeleton->property("animating").toBool());
            for (QQuickItem *child : skeleton->childItems())
                QVERIFY2(!child->isVisible(),
                         "a rectangular band is drawn over a shaped avatar");
        }
    }

    // A decoded avatar's transparent pixels reveal the surrounding surface,
    // not the fallback colour, at every common size.
    void transparentAvatarRevealsSurfaceNotFallback()
    {
        for (int size : { 24, 32, 48 }) {
            Harness h;
            QVERIFY(createAvatar(h, size, QStringLiteral("mxc://x/av%1").arg(size),
                                 QStringLiteral("Matas"),
                                 QStringLiteral("@matas:x")));

            // While loading: skeleton, no initials, no palette fill.
            QCOMPARE(state(h), QStringLiteral("loading"));
            auto *skeleton = h.avatar->findChild<QQuickItem *>(
                QStringLiteral("avatarSkeleton"));
            auto *initials = h.avatar->findChild<QQuickItem *>(
                QStringLiteral("avatarInitials"));
            QVERIFY(skeleton && skeleton->isVisible());
            QVERIFY(initials && !initials->isVisible());

            const int fetchIndex = finalEdgeFetchIndex(
                h, QStringLiteral("mxc://x/av%1").arg(size), size);
            QVERIFY(fetchIndex >= 0);
            h.client->succeed(h.client->fetches.at(fetchIndex).opId,
                              halfTransparentPng(64));
            QTRY_COMPARE_WITH_TIMEOUT(state(h), QStringLiteral("ready"), 5000);
            QVERIFY(!skeleton->isVisible());
            QVERIFY(!initials->isVisible());

            const QImage frame = h.window->grabWindow();
            QVERIFY(!frame.isNull());
            const qreal dpr = frame.devicePixelRatio();
            auto sample = [&](qreal x, qreal y) {
                return QColor(frame.pixel(int((20 + x) * dpr),
                                          int((20 + y) * dpr)));
            };
            // Inside the circle, transparent half: the window surface.
            const QColor rightInside = sample(size * 0.72, size * 0.5);
            QVERIFY2(colorsClose(rightInside, kSurface),
                     qPrintable(QStringLiteral(
                         "size %1: transparent avatar region was %2, expected "
                         "the surface colour %3 (fallback fill leaked through)")
                         .arg(size).arg(rightInside.name(), kSurface.name())));
            // Inside the circle, opaque half: the avatar's own pixels.
            const QColor leftInside = sample(size * 0.3, size * 0.5);
            QVERIFY2(colorsClose(leftInside, QColor(220, 30, 30), 40),
                     qPrintable(QStringLiteral(
                         "size %1: opaque avatar region was %2")
                         .arg(size).arg(leftInside.name())));
            // Outside the circle (corner): the surface, no square backing.
            const QColor corner = sample(1, 1);
            QVERIFY2(colorsClose(corner, kSurface),
                     qPrintable(QStringLiteral(
                         "size %1: corner outside the circle was %2")
                         .arg(size).arg(corner.name())));
            QCOMPARE(h.warnings, QStringList{});
        }
    }

    // The last known avatar per account is persisted. MediaBridge fetches
    // through the active client, so an inactive account's avatar cannot be
    // fetched at all, and the in-memory media cache is empty at launch.
    // Asserted on the store; rendering it is Avatar.fallbackSource's own
    // contract.
    void aStoredAvatarSurvivesTheProcessAndIsForgottenWithTheAccount()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("XDG_DATA_HOME", home.path().toUtf8());

        const QString uid = QStringLiteral("@someone:example.org");
        AccountAvatarStore store;
        QVERIFY2(store.avatarUrlFor(uid).isEmpty(),
                 "an account with nothing stored must report nothing, so the "
                 "row keeps its honest initials");

        // A real PNG: the store accepts only raster images this client already
        // accepts, judged by shape, not by a list of spellings (an image-class
        // payload starting with '<' is markup).
        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        buffer.close();

        QSignalSpy stored(&store, &AccountAvatarStore::avatarStored);
        QVERIFY2(store.store(uid, png), "a valid PNG was refused");
        QCOMPARE(stored.count(), 1);

        const QString url = store.avatarUrlFor(uid);
        QVERIFY2(url.startsWith(QStringLiteral("file://")),
                 qPrintable(QStringLiteral("not a local file url: %1").arg(url)));
        // The url must open the file it names: "file://" + "C:/..." made Qt
        // read the drive letter as a host on Windows.
        QVERIFY(QUrl(url).isLocalFile());
        QVERIFY2(QFileInfo::exists(QUrl(url).toLocalFile()),
                 qPrintable(QUrl(url).toLocalFile()));
        // A second store reads what the first wrote: the picture outlives the
        // object that fetched it.
        AccountAvatarStore reopened;
        QCOMPARE(reopened.avatarUrlFor(uid), url);

        // Markup, an over-cap payload and an unusable id are declined, and
        // `store` returns false so callers can tell "declined" from "stored".
        QVERIFY2(!store.store(uid, QByteArray("<svg xmlns=\"http://x\"></svg>")),
                 "markup was accepted as an avatar");
        QVERIFY2(!store.store(uid, QByteArray(AccountAvatarStore::kMaxBytes + 1, '\x89')),
                 "an over-cap payload was accepted");
        QVERIFY2(!store.store(QStringLiteral("not-a-user-id"), png),
                 "an unusable account id produced a file");
        // ...and none replaced the good picture.
        QCOMPARE(store.avatarUrlFor(uid), url);

        // Sign-out removes it. The store is app-level (reading account B's
        // picture while A is live is its purpose), so account-directory
        // removal does not sweep it.
        QVERIFY(store.forget(uid));
        QVERIFY(store.avatarUrlFor(uid).isEmpty());
        // "Target absent" and "removed" are different outcomes.
        QVERIFY2(!store.forget(uid),
                 "forgetting an account with no stored picture reported that "
                 "it removed something");
    }

    void missingAvatarShowsStableIdentityFallback()
    {
        Harness a;
        QVERIFY(createAvatar(a, 32, QString(), QStringLiteral("matas"),
                             QStringLiteral("@matas:x")));
        QCOMPARE(state(a), QStringLiteral("missing"));
        auto *initials = a.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarInitials"));
        QVERIFY(initials && initials->isVisible());
        QCOMPARE(initials->property("text").toString(), QStringLiteral("M"));
        const QColor before = a.avatar->property("color").value<QColor>();

        a.avatar->setProperty("name", QStringLiteral("Matas Petrauskas"));
        const QColor after = a.avatar->property("color").value<QColor>();
        QCOMPARE(after, before); // colour keyed on user id, not the name
        QCOMPARE(initials->property("text").toString(), QStringLiteral("MP"));
        QCOMPARE(a.warnings, QStringList{});
    }

    // A failed fetch falls back to initials (geometry preserved), and a later
    // cache completion still promotes to the real image.
    void failedFetchFallsBackThenRecovers()
    {
        Harness h;
        QVERIFY(createAvatar(h, 32, QStringLiteral("mxc://x/failing"),
                             QStringLiteral("Matas"), QStringLiteral("@matas:x")));
        QCOMPARE(state(h), QStringLiteral("loading"));
        const int fetchIndex = finalEdgeFetchIndex(
            h, QStringLiteral("mxc://x/failing"), 32);
        QVERIFY(fetchIndex >= 0);
        h.client->fail(h.client->fetches.at(fetchIndex).opId,
                       QStringLiteral("network"));
        QTRY_COMPARE_WITH_TIMEOUT(state(h), QStringLiteral("failed"), 5000);
        auto *initials = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarInitials"));
        QVERIFY(initials && initials->isVisible());
        QCOMPARE(h.avatar->width(), 32.0);
        QCOMPARE(h.avatar->height(), 32.0);
        QCOMPARE(h.warnings, QStringList{});
    }

    // Changing the avatar URL resets failure state and never shows the
    // previous user's image (delegate reuse).
    void avatarUrlChangeResetsCleanly()
    {
        Harness h;
        QVERIFY(createAvatar(h, 32, QStringLiteral("mxc://x/user-a"),
                             QStringLiteral("A"), QStringLiteral("@a:x")));
        const int fetchA = finalEdgeFetchIndex(
            h, QStringLiteral("mxc://x/user-a"), 32);
        QVERIFY(fetchA >= 0);
        h.client->succeed(h.client->fetches.at(fetchA).opId,
                          solidPng(64, QColor(200, 40, 40)));
        QTRY_COMPARE_WITH_TIMEOUT(state(h), QStringLiteral("ready"), 5000);

        h.avatar->setProperty("mxc", QStringLiteral("mxc://x/user-b"));
        QTRY_VERIFY_WITH_TIMEOUT(state(h) != QStringLiteral("ready"), 5000);
        int fetchB = -1;
        QTRY_VERIFY_WITH_TIMEOUT(
            (fetchB = finalEdgeFetchIndex(
                 h, QStringLiteral("mxc://x/user-b"), 32)) >= 0, 5000);
        h.client->succeed(h.client->fetches.at(fetchB).opId,
                          solidPng(64, QColor(40, 40, 200)));
        QTRY_COMPARE_WITH_TIMEOUT(state(h), QStringLiteral("ready"), 5000);

        const QImage frame = h.window->grabWindow();
        const qreal dpr = frame.devicePixelRatio();
        const QColor center(frame.pixel(int((20 + 16) * dpr),
                                        int((20 + 16) * dpr)));
        QVERIFY2(colorsClose(center, QColor(40, 40, 200), 40),
                 qPrintable(QStringLiteral("expected user B blue, got %1")
                                .arg(center.name())));
        QCOMPARE(h.warnings, QStringList{});
    }

    // An Avatar created while its cache key is failure-marked (another
    // surface's fetch failed first) shows initials immediately, never an
    // endless skeleton, and recovers by itself once the transient window
    // expires.
    void avatarCreatedUnderFailureMarkShowsInitialsThenAutoRecovers()
    {
        Harness h;
        QVERIFY(prepareCore(h));
        const QString mxc = QStringLiteral("mxc://x/marked");
        h.bridge->avatarSource(mxc, 32);
        QCOMPARE(h.client->fetches.size(), 1);
        h.client->fail(h.client->fetches.at(0).opId,
                       QStringLiteral("network"));

        QVERIFY(loadAvatar(h, 32, mxc, QStringLiteral("Matas"),
                           QStringLiteral("@matas:x")));
        // Immediately "failed": avatarSource() returned "" and
        // avatarFailureCategory reported the mark.
        QCOMPARE(state(h), QStringLiteral("failed"));
        auto *initials = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarInitials"));
        QVERIFY(initials && initials->isVisible());

        // Recovery: the watchdog sweep emits mediaRetryable, the Avatar
        // re-dispatches and the fetch succeeds.
        h.bridge->setFailureRetryMsForTest(1);
        QTest::qWait(5);
        h.bridge->checkInflightTimeouts();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->fetches.size(), 2, 5000);
        h.client->succeed(h.client->fetches.at(1).opId,
                          solidPng(64, QColor(40, 40, 200)));
        QTRY_COMPARE_WITH_TIMEOUT(state(h), QStringLiteral("ready"), 5000);
        QCOMPARE(h.warnings, QStringList{});
    }

    // One identity at four sizes against one bridge is one client fetch, and
    // every consumer reaches ready from it.
    void oneIdentityAtManySizesSharesOneFetchAndAllReachReady()
    {
        Harness h;
        QVERIFY(prepareCore(h));
        h.window->resize(400, 400);
        h.window->show();
        QQmlComponent component(h.engine.get());
        component.loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Avatar"));
        QVERIFY2(!component.isError(),
                 qPrintable(component.errorString()));

        const QString mxc = QStringLiteral("mxc://x/shared");
        QList<QQuickItem *> avatars;
        for (int size : { 30, 34, 48, 56 }) {
            auto *item = spawnAvatar(h, component, size, mxc,
                                     QStringLiteral("Matas"),
                                     QStringLiteral("@matas:x"));
            QVERIFY(item);
            avatars.append(item);
        }
        QCoreApplication::processEvents();
        QCOMPARE(h.client->fetches.size(), 1); // one canonical fetch
        QCOMPARE(h.client->fetches.first().width, 224);

        h.client->succeed(h.client->fetches.first().opId,
                          halfTransparentPng(224));
        for (auto *item : std::as_const(avatars)) {
            QTRY_COMPARE_WITH_TIMEOUT(
                item->property("presentationState").toString(),
                QStringLiteral("ready"), 5000);
        }
        QCOMPARE(h.client->fetches.size(), 1); // still exactly one
        QCOMPARE(h.warnings, QStringList{});
        qDeleteAll(avatars);
    }

    // Delegate churn (create/destroy/mxc swap) against injected failures and
    // stranded fetches: after quiescing no avatar is still loading and the
    // bridge's in-flight and queue counts are zero.
    void delegateChurnQuiescesWithNoEternalLoading()
    {
        Harness h;
        QVERIFY(prepareCore(h));
        h.window->resize(300, 300);
        h.window->show();
        QQmlComponent component(h.engine.get());
        component.loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Avatar"));
        QVERIFY2(!component.isError(),
                 qPrintable(component.errorString()));

        const QByteArray png = solidPng(64, QColor(80, 120, 200));
        constexpr int kIdentities = 8;
        const auto mxcFor = [](int n) {
            return QStringLiteral("mxc://x/churn%1").arg(n);
        };

        QList<QQuickItem *> live;
        int resolved = 0;
        // Deterministic mixed outcomes: most succeed, some fail, some are
        // stranded for the watchdog.
        const auto resolveOutcomes = [&](bool strandSome, bool failSome) {
            while (resolved < h.client->fetches.size()) {
                const auto &f = h.client->fetches.at(resolved);
                if (failSome && resolved % 5 == 2)
                    h.client->fail(f.opId, QStringLiteral("network"));
                else if (strandSome && resolved % 7 == 3)
                    ; // never answered; reclaimed by the watchdog
                else
                    h.client->succeed(f.opId, png);
                ++resolved;
            }
        };

        for (int i = 0; i < 300; ++i) {
            // Delegate reuse: swap an existing avatar to another identity.
            if (!live.isEmpty() && i % 3 == 0) {
                live[i % live.size()]->setProperty(
                    "mxc", mxcFor((i + 1) % kIdentities));
            }
            auto *item = spawnAvatar(
                h, component, 24 + (i % 4) * 8, mxcFor(i % kIdentities),
                QStringLiteral("U%1").arg(i % kIdentities),
                QStringLiteral("@u%1:x").arg(i % kIdentities));
            QVERIFY(item);
            live.append(item);
            while (live.size() > 6) {
                auto *victim = live.takeFirst();
                victim->setParentItem(nullptr);
                victim->deleteLater();
            }
            resolveOutcomes(true, true);
            if (i % 25 == 24) {
                // Reclaim stranded slots, then resolve what the pump
                // re-dispatched.
                h.bridge->setInflightTimeoutMsForTest(0);
                h.bridge->checkInflightTimeouts();
                h.bridge->setInflightTimeoutMsForTest(45 * 1000);
                resolveOutcomes(false, false);
                QCoreApplication::processEvents();
            }
        }
        QCoreApplication::processEvents();

        // Quiesce: reclaim fetches stranded since the last boundary (a 0ms
        // timeout also reclaims what this sweep re-dispatches; later rounds
        // recover those), then sweep and resolve with the normal timeout so
        // live re-dispatches are answered.
        h.bridge->setFailureRetryMsForTest(0);
        h.bridge->setInflightTimeoutMsForTest(0);
        h.bridge->checkInflightTimeouts();
        h.bridge->setInflightTimeoutMsForTest(45 * 1000);
        for (int round = 0; round < 10; ++round) {
            h.bridge->checkInflightTimeouts(); // sweep, then re-dispatch
            resolveOutcomes(false, false);
            QCoreApplication::processEvents();
        }

        QCOMPARE(h.bridge->inflightCountForTest(), 0);
        QCOMPARE(h.bridge->queuedCountForTest(), 0);
        for (auto *item : std::as_const(live)) {
            QTRY_VERIFY2_WITH_TIMEOUT(
                item->property("presentationState").toString()
                    != QStringLiteral("loading"),
                "an avatar stayed in eternal loading after quiescing", 5000);
        }
        // With every mark swept and fetch answered, all survivors reach the
        // real bitmap.
        for (auto *item : std::as_const(live)) {
            QTRY_COMPARE_WITH_TIMEOUT(
                item->property("presentationState").toString(),
                QStringLiteral("ready"), 5000);
        }
        qDeleteAll(live);
    }
};

QTEST_MAIN(AvatarPipelineQmlTest)
#include "AvatarPipelineQmlTest.moc"
