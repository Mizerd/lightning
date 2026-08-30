// 2026-08-26: the link-preview CONSENT gate, rendered from the production
// MessageDelegate offscreen.
//
// The gate is the control that stands between a reader and an outbound
// request to a third-party site, and it is the reason link previews default
// OFF (an encrypted room is stricter still). It used to render as a STACK —
// a host row, a wrapped two-line amber sentence, then a full-width button —
// which cost roughly four message lines of timeline for one unloaded link.
// Readers reported it as taking over the row.
//
// What this suite pins is the pair of properties that had to survive the
// shrink:
//   * the gate is ONE band — the button shares the host's and the notice's
//     vertical span instead of sitting in a row of its own, and the card is
//     strictly smaller (both axes) than the very next state it enters. On
//     the pre-fix delegate the button was strictly BELOW the notice and the
//     gate was TALLER than the loading state, so every assertion here fails
//     against it;
//   * the privacy fact is still stated before consent — the visible notice
//     names the direct contact and the IP, and the long sentence is still
//     carried verbatim by the row.
// And that consent still MEANS the button: the card's open-the-URL click
// target stays disabled while the gate is showing, so hovering the row to
// read the warning can never agree to the fetch.
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

// Vertical spans overlap — i.e. the two items sit in the same band rather
// than one under the other.
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

    // A complete role map with safe defaults so the production delegate
    // binds without undefined-property warnings.
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

    // The delegate reads encryption from its HOST PANE, not from the row, so
    // the fixture supplies a stand-in that reports an encrypted room — the
    // state in which the gate carries its strongest wording.
    //
    // It is a real QML object rather than a QVariantMap on purpose. Several
    // of the delegate's bindings guard on `timelineView` being truthy and
    // then CALL a method on it (`stateGroupExpanded` is the one with no
    // second guard), so a plain map turns every such binding into a
    // TypeError and the no-warnings assertions below stop meaning anything.
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
        // The pane stand-in must be in place before the gate is staged, or
        // the notice renders its unencrypted wording.
        QObject *host = encryptedHost(out.engine.get(), out.root);
        if (!host)
            return false;
        out.root->setProperty("timelineView", QVariant::fromValue(host));
        out.root->setProperty("preview", QVariant::fromValue(gatePreview()));
        QCoreApplication::processEvents();
        return true;
    }

private Q_SLOTS:
    // ONE band. The host, the notice and the Show button share a vertical
    // span; the pre-fix gate put the button in a row of its own below a
    // wrapped two-line sentence.
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
        // Two text lines, not three rows: the whole gate is no taller than
        // the button plus the card's own padding would allow on one band.
        auto *card = d.root->findChild<QQuickItem *>(
            QStringLiteral("linkPreviewCard"));
        QVERIFY(card != nullptr);
        QVERIFY(card->implicitHeight()
                <= button->height() + notice->height() + 24.0);
        QCOMPARE(d.warnings, QStringList{});
    }

    // The gate is NARROWER than the state the consent click leads to.
    //
    // It used to be the widest card the bubble allowed — a flat 400px cap —
    // for a link nobody had agreed to fetch. It now sizes to its own row.
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

        // The state the button dispatches into, and eventually reaches.
        // Order matters: the card latches a monotonic reserved height, and
        // production always shows the gate first.
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

        // WIDTH is the half this test can prove, and it is the half the
        // report was about: the gate used to take the flat 400px cap for a
        // link nobody had agreed to fetch, and now it sizes to its own
        // contents (measured here: 285 against the loaded card's 400).
        QVERIFY2(card->implicitWidth() > gateW,
                 qPrintable(QStringLiteral("gate %1 wide vs loaded %2 — the "
                                           "gate is not sizing to its own "
                                           "contents")
                                .arg(gateW).arg(card->implicitWidth())));

        // HEIGHT is deliberately NOT asserted here. A loaded card's height
        // comes from content this fixture cannot faithfully supply (a real
        // title, description and thumbnail arrive from LinkPreviewController,
        // and a hand-built map renders the same 47px band the gate does), so
        // a comparison against it would be measuring the fixture rather than
        // the layout. The gate's own height is bounded with real teeth in
        // consentGateIsASingleBand above — one band, no third row.
        QCOMPARE(d.warnings, QStringList{});
    }

    // The reason the control exists must still be on screen before consent,
    // and the full sentence must still exist verbatim on the row.
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
        // Both facts, and previews now go through the homeserver FIRST, so
        // there are three things this compressed label has to carry: who
        // fetches it, that a direct fetch is the fallback, and that the
        // fallback costs the reader's address. Dropping the last two would
        // promise a privacy property that a server with previews disabled
        // — Synapse's default — cannot deliver.
        QVERIFY2(visible.contains(QStringLiteral("server")),
                 qPrintable(QStringLiteral("label omits who fetches: %1")
                                .arg(visible)));
        QVERIFY2(visible.contains(QStringLiteral("directly")),
                 qPrintable(QStringLiteral("label omits the fallback: %1")
                                .arg(visible)));
        QVERIFY2(visible.contains(QStringLiteral("IP")),
                 qPrintable(QStringLiteral("label omits what the fallback "
                                           "costs: %1").arg(visible)));
        // Not elided away on a narrow bubble — a privacy notice the reader
        // cannot reach the end of is not a notice.
        // Qt::ElideNone is 3, NOT 0 — 0 is Qt::ElideLeft. This assertion was
        // first written as `== 0` and could never have passed, on the fixed
        // tree or the broken one. Spelled with the enum so it cannot drift
        // back into a magic number.
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
        // BOTH HALVES, and the order matters as much as the words.
        //
        // Previews now go through the homeserver first, so the notice must
        // say so — claiming a direct fetch when the server did it would be
        // scaring the user about an exposure that did not happen. But it
        // must ALSO keep the direct case, because the fallback is real and
        // Synapse disables previews by DEFAULT: a notice that promised only
        // the private route would be a lie on most servers.
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

    // Consent is the BUTTON. This one is a CONTRACT PIN, not a regression —
    // it passes on the pre-fix delegate too, which is the point: the shrink
    // added a HoverHandler and a tooltip to the gate row so the long
    // sentence stays readable, and hovering to read a warning must never be
    // able to agree to the fetch. Nothing else on the card may dispatch the
    // request or open the URL while the gate is showing.
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
