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
#include <QQmlComponent>
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

    // Core stack without any Avatar yet: needed by the tests that must
    // manipulate the bridge (pre-marked failures) or spawn several Avatars
    // against ONE bridge (canonical-fetch sharing, churn soak).
    bool prepareCore(Harness &h)
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

    // Additional Avatar instances against the SAME engine/bridge (delegate
    // churn, shared-fetch tests).
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

    // v0.7.1: every avatar identity is fetched at ONE canonical edge
    // regardless of the requested render size, so there is exactly one
    // fetch per identity; the helper keeps its call shape for clarity.
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

    // v0.7.1 live regression: an Avatar instantiated WHILE its cache key is
    // failure-marked (the room-header case — another surface's failed fetch
    // poisoned the key before this Avatar ever existed) must show honest
    // initials immediately, never an eternal skeleton, and must recover to
    // ready on its own once the transient window expires — with ZERO user
    // interaction.
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
        // Immediately "failed" (initials) — the suppressed avatarSource()
        // returned "" but avatarFailureCategory reported the mark.
        QCOMPARE(state(h), QStringLiteral("failed"));
        auto *initials = h.avatar->findChild<QQuickItem *>(
            QStringLiteral("avatarInitials"));
        QVERIFY(initials && initials->isVisible());

        // Autonomous recovery: the watchdog sweep emits mediaRetryable,
        // the Avatar re-dispatches, the fetch succeeds.
        h.bridge->setFailureRetryMsForTest(1);
        QTest::qWait(5);
        h.bridge->checkInflightTimeouts();
        QTRY_COMPARE_WITH_TIMEOUT(h.client->fetches.size(), 2, 5000);
        h.client->succeed(h.client->fetches.at(1).opId,
                          solidPng(64, QColor(40, 40, 200)));
        QTRY_COMPARE_WITH_TIMEOUT(state(h), QStringLiteral("ready"), 5000);
        QCOMPARE(h.warnings, QStringList{});
    }

    // v0.7.1: one identity rendered at four different sizes against one
    // bridge is exactly ONE client fetch, and every consumer reaches ready
    // from that single canonical-edge payload.
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

    // v0.7.1 mini-soak: delegate-reuse churn (create/destroy/mxc-swap)
    // against injected failures and stranded (watchdog-reclaimed) fetches.
    // After quiescing, ZERO avatars may remain in "loading", and the
    // bridge's in-flight and queue counts must both be zero — the
    // "reliability degrades the longer the app runs" regression.
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
        // stranded for the watchdog to reclaim.
        const auto resolveOutcomes = [&](bool strandSome, bool failSome) {
            while (resolved < h.client->fetches.size()) {
                const auto &f = h.client->fetches.at(resolved);
                if (failSome && resolved % 5 == 2)
                    h.client->fail(f.opId, QStringLiteral("network"));
                else if (strandSome && resolved % 7 == 3)
                    ; // never answered — reclaimed by the watchdog
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
                // Reclaim stranded slots (transient timeout marks), then
                // resolve whatever the pump re-dispatched.
                h.bridge->setInflightTimeoutMsForTest(0);
                h.bridge->checkInflightTimeouts();
                h.bridge->setInflightTimeoutMsForTest(45 * 1000);
                resolveOutcomes(false, false);
                QCoreApplication::processEvents();
            }
        }
        QCoreApplication::processEvents();

        // Quiesce. First reclaim the fetches stranded since the last
        // boundary (0ms timeout also reclaims whatever the sweep inside
        // this call re-dispatched — the rounds below recover those), then
        // sweep-and-resolve with the NORMAL in-flight timeout so live
        // re-dispatches are answered, not reclaimed.
        h.bridge->setFailureRetryMsForTest(0);
        h.bridge->setInflightTimeoutMsForTest(0);
        h.bridge->checkInflightTimeouts();
        h.bridge->setInflightTimeoutMsForTest(45 * 1000);
        for (int round = 0; round < 10; ++round) {
            h.bridge->checkInflightTimeouts(); // sweep → re-dispatch
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
        // With every mark swept and every fetch answered, the survivors
        // all reach the real bitmap.
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
