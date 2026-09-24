// The link-preview consent gate, rendered from the production MessageDelegate
// offscreen. The gate stands between a reader and a request to a third-party
// site (previews default off). Pinned:
//   * the gate is one band: host, notice and button share a vertical span,
//     and the card is smaller on both axes than the state it leads to;
//   * the privacy fact is stated before consent: the visible notice names
//     the direct contact and the IP, and the row carries the full sentence;
//   * only the button consents: the card's open-URL target stays disabled
//     while the gate shows, so hovering to read the warning cannot agree.
#include <QTextOption>
#include <QtTest/QtTest>

#include <memory>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "models/TimelineModel.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;

// Vertical spans overlap: the items share a band rather than stacking.
bool sharesBand(QQuickItem *a, QQuickItem *b)
{
    const QRectF ra = a->mapRectToItem(nullptr, a->boundingRect());
    const QRectF rb = b->mapRectToItem(nullptr, b->boundingRect());
    return ra.bottom() > rb.top() + 1.0 && rb.bottom() > ra.top() + 1.0;
}
} // namespace

class LinkPreviewQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
        QStringList warnings;
    };

    // A complete role map with safe defaults so the production delegate binds
    // without undefined-property warnings.
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
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("https://www.lightning-matrix.org/"));
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("edited"), false);
        fixture.insert(QStringLiteral("isEncrypted"), true);
        fixture.insert(QStringLiteral("isDecrypted"), true);
        fixture.insert(QStringLiteral("undecryptable"), false);
        fixture.insert(QStringLiteral("errorKind"), QString{});
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("isVideo"), false);
        fixture.insert(QStringLiteral("isAudio"), false);
        fixture.insert(QStringLiteral("isSticker"), false);
        fixture.insert(QStringLiteral("mediaIsVoice"), false);
        fixture.insert(QStringLiteral("mediaDurationMs"), 0);
        fixture.insert(QStringLiteral("mediaWidth"), 0);
        fixture.insert(QStringLiteral("mediaHeight"), 0);
        fixture.insert(QStringLiteral("mediaSize"), 0);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), false);
        fixture.insert(QStringLiteral("mediaThumbAvailable"), false);
        fixture.insert(QStringLiteral("mediaKey"), QString{});
        fixture.insert(QStringLiteral("mediaFilename"), QString{});
        fixture.insert(QStringLiteral("mediaUrl"), QUrl{});
        fixture.insert(QStringLiteral("mediaThumbUrl"), QUrl{});
        fixture.insert(QStringLiteral("mediaMimetype"), QString{});
        fixture.insert(QStringLiteral("reactions"), QVariantList{});
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("isThreadRoot"), false);
        fixture.insert(QStringLiteral("mentionsMe"), false);
        fixture.insert(QStringLiteral("mentionsRoom"), false);
        fixture.insert(QStringLiteral("isLocalEcho"), false);
        return fixture;
    }

    // The delegate reads encryption from its host pane, so this stand-in
    // reports an encrypted room (the gate's strongest wording). A real QML
    // object, not a QVariantMap: some bindings call methods on
    // `timelineView` (`stateGroupExpanded` has no second guard), and a map
    // would turn them into TypeErrors.
    static QObject *encryptedHost(QQmlEngine *engine, QObject *owner)
    {
        QQmlComponent component(engine);
        component.setData(R"QML(
import QtQuick
QtObject {
    property bool roomEncrypted: true
    property real contentY: 0
    property real height: 10000
    property bool speculativeMediaAllowed: true
    property bool stickToBottom: false
    property bool threadContext: false
    property string transientInteractionOwner: ""
    property string hoveredActionsKey: ""
    property string pinnedActionsKey: ""
    function stateGroupExpanded(groupId) { return false }
    function toggleStateGroup(groupId) {}
}
)QML",
                          QUrl());
        QObject *host = component.create();
        if (host)
            host->setParent(owner);
        return host;
    }

    static QVariantMap gatePreview()
    {
        QVariantMap preview;
        preview.insert(QStringLiteral("state"),
                       QStringLiteral("requires_action"));
        preview.insert(QStringLiteral("host"),
                       QStringLiteral("www.lightning-matrix.org"));
        preview.insert(QStringLiteral("url"),
                       QStringLiteral("https://www.lightning-matrix.org/"));
        return preview;
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
        out.engine->rootContext()->setContextProperty("app", &controller);
        out.engine->rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(out.engine.get(),
                              &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return false;
        out.root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(760, 480);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(700);
        out.window->show();
        QCoreApplication::processEvents();
        // The pane stand-in must be in place before the gate is staged, or the
        // notice uses its unencrypted wording.
        QObject *host = encryptedHost(out.engine.get(), out.root);
        if (!host)
            return false;
        out.root->setProperty("timelineView", QVariant::fromValue(host));
        out.root->setProperty("preview", QVariant::fromValue(gatePreview()));
        QCoreApplication::processEvents();
        return true;
    }

private Q_SLOTS:
    // One band: host, notice and Show button share a vertical span.
    void consentGateIsASingleBand()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(createDelegate(controller, baseFixture(controller), d));

        auto *host = d.root->findChild<QQuickItem *>(
            QStringLiteral("linkPreviewConsentHost"));
        auto *notice = d.root->findChild<QQuickItem *>(
            QStringLiteral("linkPreviewConsentNotice"));
        auto *button = d.root->findChild<QQuickItem *>(
            QStringLiteral("linkPreviewLoadButton"));
        QVERIFY(host != nullptr);
        QVERIFY(notice != nullptr);
        QVERIFY(button != nullptr);
        QVERIFY(host->isVisible());
        QVERIFY(notice->isVisible());
        QVERIFY(button->isVisible());

        QVERIFY(sharesBand(button, host));
        QVERIFY(sharesBand(button, notice));
        // Two text lines, not three rows: no taller than one band allows.
        auto *card = d.root->findChild<QQuickItem *>(
            QStringLiteral("linkPreviewCard"));
        QVERIFY(card != nullptr);
        QVERIFY(card->implicitHeight()
                <= button->height() + notice->height() + 24.0);
        QCOMPARE(d.warnings, QStringList{});
    }

    // The gate is narrower than the state the consent click leads to; it
    // sizes to its own row rather than the 400px cap.
    void consentGateIsNarrowerThanTheStateItLeadsTo()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(createDelegate(controller, baseFixture(controller), d));

        auto *card = d.root->findChild<QQuickItem *>(
            QStringLiteral("linkPreviewCard"));
        QVERIFY(card != nullptr);
        const qreal gateW = card->implicitWidth();
        const qreal gateH = card->implicitHeight();
        QVERIFY(gateW > 0.0);
        QVERIFY(gateH > 0.0);

        // The state the button dispatches into. Order matters: the card
        // latches a monotonic reserved height, and production shows the gate
        // first.
        QVariantMap loaded;
        loaded.insert(QStringLiteral("state"), QStringLiteral("loaded"));
        loaded.insert(QStringLiteral("host"),
                      QStringLiteral("www.lightning-matrix.org"));
        loaded.insert(QStringLiteral("title"),
                      QStringLiteral("Lightning — a native Matrix client"));
        loaded.insert(QStringLiteral("description"),
                      QStringLiteral("A Qt 6 desktop Matrix client built on "
                                     "the official Rust SDK, with real E2EE, "
                                     "threads and calls."));
        d.root->setProperty("preview", QVariant::fromValue(loaded));
        QCoreApplication::processEvents();

        // Width is what this can prove: the gate sizes to its contents rather
        // than the loaded card's 400px cap.
        QVERIFY2(card->implicitWidth() > gateW,
                 qPrintable(QStringLiteral("gate %1 wide vs loaded %2 — the "
                                           "gate is not sizing to its own "
                                           "contents")
                                .arg(gateW).arg(card->implicitWidth())));

        // Height is deliberately not compared: a loaded card's height depends
        // on content this fixture cannot supply faithfully. The gate's own
        // height is bounded in consentGateIsASingleBand.
        QCOMPARE(d.warnings, QStringList{});
    }

    // The privacy reason is on screen before consent, and the full sentence
    // is on the row verbatim.
    void consentGateStillStatesThePrivacyFact()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(createDelegate(controller, baseFixture(controller), d));

        auto *notice = d.root->findChild<QQuickItem *>(
            QStringLiteral("linkPreviewConsentNotice"));
        QVERIFY(notice != nullptr);
        QVERIFY(notice->isVisible());
        const QString visible = notice->property("text").toString();
        // Previews go through the homeserver first, so the label must carry
        // who fetches, that a direct fetch is the fallback, and that the
        // fallback exposes the reader's address (Synapse disables server
        // previews by default).
        QVERIFY2(visible.contains(QStringLiteral("server")),
                 qPrintable(QStringLiteral("label omits who fetches: %1")
                                .arg(visible)));
        QVERIFY2(visible.contains(QStringLiteral("directly")),
                 qPrintable(QStringLiteral("label omits the fallback: %1")
                                .arg(visible)));
        QVERIFY2(visible.contains(QStringLiteral("IP")),
                 qPrintable(QStringLiteral("label omits what the fallback "
                                           "costs: %1").arg(visible)));
        // Not elided on a narrow bubble. Qt::ElideNone is 3, not 0 (0 is
        // Qt::ElideLeft), hence the enum.
        const QVariant elideValue = notice->property("elide");
        QVERIFY2(elideValue.isValid()
                     && elideValue.toInt() == int(Qt::ElideNone),
                 qPrintable(QStringLiteral("notice elide=%1, expected "
                                           "Qt::ElideNone(%2) — the privacy "
                                           "notice is being truncated")
                                .arg(elideValue.toInt())
                                .arg(int(Qt::ElideNone))));
        QCOMPARE(notice->property("wrapMode").toInt(), int(QTextOption::WordWrap));

        auto *row = d.root->findChild<QQuickItem *>(
            QStringLiteral("linkPreviewConsentRow"));
        QVERIFY(row != nullptr);
        const QString full = row->property("fullPrivacyText").toString();
        // Both halves, in order: the notice must say the homeserver fetches
        // first, and must keep the direct fallback, which is real on most
        // servers.
        QVERIFY2(full.contains(QStringLiteral("homeserver")),
                 qPrintable(QStringLiteral(
                     "the consent notice no longer says the homeserver "
                     "loads the preview: %1").arg(full)));
        QVERIFY2(full.contains(QStringLiteral("does not see your IP")),
                 qPrintable(QStringLiteral(
                     "the notice no longer states the property the server "
                     "route buys: %1").arg(full)));
        QVERIFY2(full.contains(QStringLiteral("directly")),
                 qPrintable(QStringLiteral(
                     "the notice dropped the fallback — it would then be "
                     "false on any server with previews disabled: %1")
                         .arg(full)));
        QCOMPARE(d.warnings, QStringList{});
    }

    // Consent is the button. A contract pin: the gate row's HoverHandler and
    // tooltip must never consent, and nothing else on the card may dispatch
    // the request or open the URL while the gate shows.
    void onlyTheButtonConsents()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(createDelegate(controller, baseFixture(controller), d));

        auto *card = d.root->findChild<QQuickItem *>(
            QStringLiteral("linkPreviewCard"));
        QVERIFY(card != nullptr);
        const auto areas = card->findChildren<QQuickItem *>();
        int mouseAreas = 0;
        for (auto *item : areas) {
            if (QLatin1String(item->metaObject()->className())
                != QLatin1String("QQuickMouseArea"))
                continue;
            ++mouseAreas;
            QVERIFY(!item->property("enabled").toBool());
        }
        QVERIFY(mouseAreas > 0);
        QCOMPARE(d.warnings, QStringList{});
    }
};

QTEST_MAIN(LinkPreviewQmlTest)
#include "LinkPreviewQmlTest.moc"
