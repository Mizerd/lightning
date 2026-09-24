// Dismissing a link preview gives the row its space back. Pinned:
//   * dismissal is keyed per (room, event), never per URL: the result cache
//     is shared by URL, so a per-URL dismissal would collapse cards above the
//     reader and move the timeline;
//   * it is checked before the auto-load dispatch, so it also stops fetching;
//   * restoring grants no consent to contact the site;
//   * at the cap the oldest dismissal is released, never the newest refused
//     (MediaVisibilityStore's rule).

#include "matrix/MatrixClient.h"
#include "models/LinkPreviewController.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    quint64 lastOp = 0;
    QStringList requestedUrls;

    bool supportsUrlPreview() const override { return true; }
    quint64 fetchUrlPreview(const QString &url) override
    {
        requestedUrls.append(url);
        lastOp = nextOp++;
        return lastOp;
    }

    void succeed(quint64 opId, const QVariantMap &fields)
    {
        Q_EMIT urlPreviewFinished(opId, true, fields, QString());
    }

    // Pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
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

const QString kRoom = QStringLiteral("!room:example.org");
const QString kUrlA = QStringLiteral("https://example.org/a");
const QString kUrlB = QStringLiteral("https://other.example/b");

QString bodyWith(const QString &url)
{
    return QStringLiteral("look at ") + url;
}

} // namespace

class LinkPreviewDismissTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // A loaded card is dismissed, the row reports "none" so its Loader
    // deactivates, and the change is announced on that row's key.
    void dismissingALoadedPreviewCollapsesTheRow()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);
        controller.setAutoLoadUnencrypted(true);

        const QString ev = QStringLiteral("$e1");
        const QString body = bodyWith(kUrlA);

        QVariantMap state = controller.previewForEvent(kRoom, ev, body, false);
        QCOMPARE(state.value(QStringLiteral("state")).toString(),
                 QStringLiteral("loading"));
        QCOMPARE(client.requestedUrls.size(), 1);
        client.succeed(client.lastOp, { { QStringLiteral("title"),
                                          QStringLiteral("A title") } });

        state = controller.previewForEvent(kRoom, ev, body, false);
        QCOMPARE(state.value(QStringLiteral("state")).toString(),
                 QStringLiteral("loaded"));

        QSignalSpy spy(&controller, &LinkPreviewController::previewChanged);
        controller.dismissPreviewForEvent(kRoom, ev);
        QCOMPARE(spy.count(), 1);

        state = controller.previewForEvent(kRoom, ev, body, false);
        QCOMPARE(state.value(QStringLiteral("state")).toString(),
                 QStringLiteral("none"));
        QVERIFY2(state.value(QStringLiteral("dismissed")).toBool(),
                 "a dismissed row must say so, or the undo action cannot "
                 "know it has anything to offer");
        // url/host survive so the row can offer the preview back without
        // re-parsing the body.
        QCOMPARE(state.value(QStringLiteral("url")).toString(), kUrlA);
        QVERIFY(!state.value(QStringLiteral("host")).toString().isEmpty());
        QVERIFY(controller.isPreviewDismissed(kRoom + QChar(0x1f) + ev));
    }

    // A dismissed row never contacts the site when rebuilt, even with
    // automatic loading on.
    void aDismissedRowNeverDispatchesAFetch()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);
        controller.setAutoLoadUnencrypted(true);

        const QString ev = QStringLiteral("$e1");
        controller.dismissPreviewForEvent(kRoom, ev);

        const QVariantMap state =
            controller.previewForEvent(kRoom, ev, bodyWith(kUrlA), false);
        QCOMPARE(state.value(QStringLiteral("state")).toString(),
                 QStringLiteral("none"));
        QVERIFY(state.value(QStringLiteral("dismissed")).toBool());
        QVERIFY2(client.requestedUrls.isEmpty(),
                 "a dismissed row contacted the linked site anyway — the "
                 "dismissal check has to precede the dispatch");
    }

    // Only the clicked row may change: other rows with the same URL keep
    // their cards.
    void dismissalIsPerEventAndNeverPerUrl()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);
        controller.setAutoLoadUnencrypted(true);

        const QString first = QStringLiteral("$e1");
        const QString second = QStringLiteral("$e2");
        const QString body = bodyWith(kUrlA);

        controller.previewForEvent(kRoom, first, body, false);
        controller.previewForEvent(kRoom, second, body, false);
        QCOMPARE(client.requestedUrls.size(), 1); // deduplicated by URL
        client.succeed(client.lastOp, { { QStringLiteral("title"),
                                          QStringLiteral("A title") } });

        controller.dismissPreviewForEvent(kRoom, first);

        QCOMPARE(controller.previewForEvent(kRoom, first, body, false)
                     .value(QStringLiteral("state")).toString(),
                 QStringLiteral("none"));
        const QVariantMap other =
            controller.previewForEvent(kRoom, second, body, false);
        QCOMPARE(other.value(QStringLiteral("state")).toString(),
                 QStringLiteral("loaded"));
        QVERIFY2(!other.value(QStringLiteral("dismissed")).toBool(),
                 "dismissing one card collapsed another message's card — a "
                 "row above the reader would move under them");
    }

    // Undo is not consent: an unagreed preview comes back as the gate and
    // nothing is fetched.
    void restoringDoesNotConsentToAFetch()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);
        // Encrypted room, opt-in off: the privacy default.
        controller.setAutoLoadUnencrypted(true);
        controller.setAllowEncrypted(false);

        const QString ev = QStringLiteral("$e1");
        const QString body = bodyWith(kUrlA);

        QCOMPARE(controller.previewForEvent(kRoom, ev, body, true)
                     .value(QStringLiteral("state")).toString(),
                 QStringLiteral("requires_action"));
        QVERIFY(client.requestedUrls.isEmpty());

        controller.dismissPreviewForEvent(kRoom, ev);
        QCOMPARE(controller.previewForEvent(kRoom, ev, body, true)
                     .value(QStringLiteral("state")).toString(),
                 QStringLiteral("none"));

        QSignalSpy spy(&controller, &LinkPreviewController::previewChanged);
        controller.restorePreviewForEvent(kRoom, ev);
        QCOMPARE(spy.count(), 1);

        QCOMPARE(controller.previewForEvent(kRoom, ev, body, true)
                     .value(QStringLiteral("state")).toString(),
                 QStringLiteral("requires_action"));
        QVERIFY2(client.requestedUrls.isEmpty(),
                 "undoing a dismissal contacted the site — restore must clear "
                 "the dismissal only, never grant consent");
        QVERIFY(!controller.isPreviewDismissed(kRoom + QChar(0x1f) + ev));
    }

    // An edit that changes the URL drops the dismissal: it concerned the old
    // URL.
    void anEditThatChangesTheUrlClearsTheDismissal()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);
        controller.setAutoLoadUnencrypted(true);

        const QString ev = QStringLiteral("$e1");
        controller.previewForEvent(kRoom, ev, bodyWith(kUrlA), false);
        controller.dismissPreviewForEvent(kRoom, ev);
        QVERIFY(controller.isPreviewDismissed(kRoom + QChar(0x1f) + ev));

        const QVariantMap edited =
            controller.previewForEvent(kRoom, ev, bodyWith(kUrlB), false);
        QVERIFY2(!edited.value(QStringLiteral("dismissed")).toBool(),
                 "the new link inherited the old link's dismissal");
        QCOMPARE(edited.value(QStringLiteral("state")).toString(),
                 QStringLiteral("loading"));
        QCOMPARE(client.requestedUrls.size(), 2);
    }

    // Bounded; at the cap the oldest is released rather than the newest
    // refused, as in MediaVisibilityStore.
    void theDismissedSetIsBoundedAndReleasesTheOldest()
    {
        LinkPreviewController controller;
        const int cap = LinkPreviewController::kMaxDismissed;

        for (int i = 0; i < cap + 10; ++i)
            controller.dismissPreview(QStringLiteral("$k%1").arg(i));

        QCOMPARE(controller.dismissedCount(), cap);
        QVERIFY2(!controller.isPreviewDismissed(QStringLiteral("$k0")),
                 "the cap refused the newest dismissal instead of releasing "
                 "the oldest");
        QVERIFY(controller.isPreviewDismissed(
            QStringLiteral("$k%1").arg(cap + 9)));

        // An empty key is never a dismissal and never grows the set.
        controller.dismissPreview(QString());
        QCOMPARE(controller.dismissedCount(), cap);
        QVERIFY(!controller.isPreviewDismissed(QString()));
    }

    // Session-only, like the rest of this controller's state.
    void signingOutForgetsEveryDismissal()
    {
        FakeClient client;
        LinkPreviewController controller;
        controller.setClient(&client);

        controller.dismissPreviewForEvent(kRoom, QStringLiteral("$e1"));
        controller.dismissPreviewForEvent(kRoom, QStringLiteral("$e2"));
        QCOMPARE(controller.dismissedCount(), 2);

        client.logout();
        QCOMPARE(controller.dismissedCount(), 0);
    }
};

QTEST_MAIN(LinkPreviewDismissTest)
#include "LinkPreviewDismissTest.moc"
