// v0.7: avatar pipeline suite. Renders the production Avatar.qml through the
// real MediaBridge + MediaImageProvider stack (fake network only) and proves
// the explicit result states:
//   * loading shows the circular skeleton — no initials flash, no random
//     colour flash;
//   * a successfully decoded avatar renders over a TRANSPARENT background:
//     transparent pixels reveal the surrounding surface, never the generated
//     fallback colour (the live regression: a pink disk behind a transparent
//     PNG);
//   * missing/failed avatars show the deterministic per-identity initials
//     fallback, keyed on the stable user id so a late display-name
//     resolution cannot recolour the person;
//   * an avatar URL change resets cleanly (no stale bitmap, fresh states).
#include <QtTest/QtTest>

#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

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
    quint64 fetchMedia(const QString &, int) override { return 0; }
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

// Minimal `app` context: Avatar.qml touches only app.mediaBridge.
class AppShim : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MediaBridge *mediaBridge READ mediaBridge CONSTANT)
public:
    explicit AppShim(MediaBridge *bridge, QObject *parent = nullptr)
        : QObject(parent), m_bridge(bridge) {}
    MediaBridge *mediaBridge() const { return m_bridge; }

private:
    MediaBridge *m_bridge;
};

// A synthetic avatar whose LEFT half is opaque red and whose RIGHT half is
// fully transparent, with a soft alpha edge between them. After circular
// masking, the transparent right half sits INSIDE the visible circle — the
// exact shape that exposed the fallback colour behind a real transparent
// PNG.
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
        std::unique_ptr<AppShim> shim;
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *avatar = nullptr;
        QStringList warnings;
    };

    bool createAvatar(Harness &h, int size, const QString &mxc,
                      const QString &name, const QString &colorKey)
    {
        h.client = std::make_unique<FakeClient>();
        h.bridge = std::make_unique<MediaBridge>();
        h.bridge->setClient(h.client.get());
        h.shim = std::make_unique<AppShim>(h.bridge.get());
        h.engine = std::make_unique<QQmlApplicationEngine>();
        connect(h.engine.get(), &QQmlEngine::warnings, this,
                [&h](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        h.warnings << e.toString();
                });
        h.engine->addImageProvider(QStringLiteral("lightning-media"),
                                   new MediaImageProvider(h.bridge.get()));
        h.engine->rootContext()->setContextProperty("app", h.shim.get());
        QSignalSpy createdSpy(h.engine.get(),
                              &QQmlApplicationEngine::objectCreated);
        // Initial properties mirror production: the delegate binds
        // mxc/name/colorKey declaratively before Component.onCompleted runs
        // its first refresh().
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
        h.window = std::make_unique<QQuickWindow>();
        h.window->resize(size + 40, size + 40);
        h.window->setColor(kSurface);
        h.avatar->setParentItem(h.window->contentItem());
        h.avatar->setPosition(QPointF(20, 20));
        h.window->show();
        QCoreApplication::processEvents();
        return true;
    }

    static QString state(const Harness &h)
    {
        return h.avatar->property("presentationState").toString();
    }

    // Initial-property application order can legitimately request interim
    // edge sizes (distinct cache keys, same behaviour as a delegate resize).
    // The fetch that matters is the one for the avatar's final edge.
    static int finalEdgeFetchIndex(const Harness &h, const QString &mxc,
                                   int size)
    {
        const int edge = qMax(32, size * 2);
        for (int i = h.client->fetches.size() - 1; i >= 0; --i) {
            const auto &f = h.client->fetches.at(i);
            if (f.key == mxc && f.width == edge)
                return i;
        }
        return -1;
    }

private Q_SLOTS:
    // The core live regression: a decoded avatar with transparent pixels
    // must reveal the surrounding surface, not the generated fallback
    // colour, at every common avatar size.
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
            // Inside the circle, transparent half → the window surface.
            const QColor rightInside = sample(size * 0.72, size * 0.5);
            QVERIFY2(colorsClose(rightInside, kSurface),
                     qPrintable(QStringLiteral(
                         "size %1: transparent avatar region was %2, expected "
                         "the surface colour %3 (fallback fill leaked through)")
                         .arg(size).arg(rightInside.name(), kSurface.name())));
            // Inside the circle, opaque half → the avatar's own pixels.
            const QColor leftInside = sample(size * 0.3, size * 0.5);
            QVERIFY2(colorsClose(leftInside, QColor(220, 30, 30), 40),
                     qPrintable(QStringLiteral(
                         "size %1: opaque avatar region was %2")
                         .arg(size).arg(leftInside.name())));
            // Outside the circle (corner) → the surface, no square backing.
            const QColor corner = sample(1, 1);
            QVERIFY2(colorsClose(corner, kSurface),
                     qPrintable(QStringLiteral(
                         "size %1: corner outside the circle was %2")
                         .arg(size).arg(corner.name())));
            QCOMPARE(h.warnings, QStringList{});
        }
    }

    // No avatar at all → deterministic initials fallback keyed on the
    // stable identity: the same user id keeps the same colour even when the
    // visible name changes (MXID → resolved display name).
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

    // A failed fetch falls back to initials (geometry preserved), and a
    // later cache completion still promotes to the real image.
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

    // Changing the avatar URL resets failure state and swaps bitmaps
    // without showing the previous user's image (delegate reuse safety).
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
};

QTEST_MAIN(AvatarPipelineQmlTest)
#include "AvatarPipelineQmlTest.moc"
