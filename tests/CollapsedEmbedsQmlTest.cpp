// Settings > Appearance > Timeline > "Collapse media and link embeds". Drives
// the production MessageDelegate offscreen (as LinkPreviewQmlTest does),
// since every property here is a load-time or layout fact. Covered: the
// collapsed line replaces the attachment and the component is not even
// instantiated (a hidden Image still downloads and decodes); the line names
// the attachment, expands reversibly and works from the keyboard; loaded link
// previews collapse but consent gates do not; reply quotes are not embeds;
// and the setting is off by default.
#include <QtTest/QtTest>

#include <memory>
#include <utility>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "models/TimelineModel.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;
} // namespace

class CollapsedEmbedsQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
        QStringList warnings;
    };

    // A complete role map with safe defaults, taken from the real model's
    // roleNames so a later role arrives as a null QVariant rather than a
    // missing property.
    static QVariantMap baseFixture(AppController &controller)
    {
        QVariantMap f;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            f.insert(QString::fromUtf8(it.value()), QVariant{});
        f.insert(QStringLiteral("isVirtual"), false);
        f.insert(QStringLiteral("isStateActivity"), false);
        f.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        f.insert(QStringLiteral("showSenderIdentity"), true);
        f.insert(QStringLiteral("eventId"), QStringLiteral("$fixture"));
        f.insert(QStringLiteral("itemId"), QStringLiteral("fixture-item"));
        f.insert(QStringLiteral("sender"), QStringLiteral("@fixture:mock.local"));
        f.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Fixture"));
        f.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        f.insert(QStringLiteral("body"), QString{});
        f.insert(QStringLiteral("eventType"), 0);
        f.insert(QStringLiteral("status"), 0);
        f.insert(QStringLiteral("isOwn"), false);
        f.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc());
        f.insert(QStringLiteral("redacted"), false);
        f.insert(QStringLiteral("edited"), false);
        f.insert(QStringLiteral("isEncrypted"), false);
        f.insert(QStringLiteral("isDecrypted"), true);
        f.insert(QStringLiteral("undecryptable"), false);
        f.insert(QStringLiteral("errorKind"), QString{});
        f.insert(QStringLiteral("isImage"), false);
        f.insert(QStringLiteral("isFile"), false);
        f.insert(QStringLiteral("isVideo"), false);
        f.insert(QStringLiteral("isAudio"), false);
        f.insert(QStringLiteral("isSticker"), false);
        f.insert(QStringLiteral("mediaIsVoice"), false);
        f.insert(QStringLiteral("mediaDurationMs"), 0);
        f.insert(QStringLiteral("mediaWidth"), 0);
        f.insert(QStringLiteral("mediaHeight"), 0);
        f.insert(QStringLiteral("mediaSize"), 0);
        // False on purpose: with no bridge source and an empty thumb URL the
        // image resolves to no source, so the suite never touches the network
        // or media cache.
        f.insert(QStringLiteral("mediaSourceAvailable"), false);
        f.insert(QStringLiteral("mediaThumbAvailable"), false);
        f.insert(QStringLiteral("mediaKey"), QString{});
        f.insert(QStringLiteral("mediaFilename"), QString{});
        f.insert(QStringLiteral("mediaUrl"), QUrl{});
        f.insert(QStringLiteral("mediaThumbUrl"), QUrl{});
        f.insert(QStringLiteral("mediaMimetype"), QString{});
        f.insert(QStringLiteral("reactions"), QVariantList{});
        f.insert(QStringLiteral("replyToEventId"), QString{});
        f.insert(QStringLiteral("isThreadRoot"), false);
        f.insert(QStringLiteral("mentionsMe"), false);
        f.insert(QStringLiteral("mentionsRoom"), false);
        f.insert(QStringLiteral("isLocalEcho"), false);
        return f;
    }

    // A 1920x1080 PNG called holiday.png: the three facts the collapsed line
    // should carry, all asserted.
    static QVariantMap imageFixture(AppController &controller)
    {
        QVariantMap f = baseFixture(controller);
        f.insert(QStringLiteral("isImage"), true);
        f.insert(QStringLiteral("mediaFilename"), QStringLiteral("holiday.png"));
        f.insert(QStringLiteral("body"), QStringLiteral("holiday.png"));
        f.insert(QStringLiteral("mediaMimetype"), QStringLiteral("image/png"));
        f.insert(QStringLiteral("mediaWidth"), 1920);
        f.insert(QStringLiteral("mediaHeight"), 1080);
        f.insert(QStringLiteral("mediaKey"), QStringLiteral("fixture-media"));
        return f;
    }

    // The delegate reads room encryption and viewport facts from its host
    // pane. A real QML object, not a QVariantMap: several bindings call
    // methods on `timelineView`, which a map would turn into TypeErrors.
    static QObject *paneStandIn(QQmlEngine *engine, QObject *owner)
    {
        QQmlComponent component(engine);
        component.setData(R"QML(
import QtQuick
QtObject {
    property bool roomEncrypted: false
    property bool isDirectRoom: false
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
        out.window->resize(760, 640);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(700);
        out.window->show();
        QCoreApplication::processEvents();
        QObject *host = paneStandIn(out.engine.get(), out.root);
        if (!host)
            return false;
        out.root->setProperty("timelineView", QVariant::fromValue(host));
        QCoreApplication::processEvents();
        return true;
    }

    static QQuickItem *find(const Delegate &d, const char *name)
    {
        return d.root->findChild<QQuickItem *>(QLatin1String(name));
    }

    // Let nested Loaders build and layouts polish: toggling the collapse
    // destroys one subtree and builds another.
    static void settle()
    {
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents();
            QTest::qWait(20);
        }
    }

    static QVariantMap loadedPreview()
    {
        QVariantMap p;
        p.insert(QStringLiteral("state"), QStringLiteral("loaded"));
        p.insert(QStringLiteral("host"),
                 QStringLiteral("www.lightning-matrix.org"));
        p.insert(QStringLiteral("url"),
                 QStringLiteral("https://www.lightning-matrix.org/"));
        p.insert(QStringLiteral("title"),
                 QStringLiteral("Lightning — a native Matrix client"));
        p.insert(QStringLiteral("description"),
                 QStringLiteral("A Qt 6 desktop Matrix client."));
        return p;
    }

    static QVariantMap consentGatePreview()
    {
        QVariantMap p;
        p.insert(QStringLiteral("state"), QStringLiteral("requires_action"));
        p.insert(QStringLiteral("host"),
                 QStringLiteral("www.lightning-matrix.org"));
        p.insert(QStringLiteral("url"),
                 QStringLiteral("https://www.lightning-matrix.org/"));
        return p;
    }

    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;

private Q_SLOTS:
    // A private config root, so persisted settings flipped here never reach a
    // real store or another suite.
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("collapsed-embeds-qml-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // Off by default: a default flip would reach every existing install
    // unasked.
    void theSettingIsOffByDefault()
    {
        AppController controller(AppController::MockBackend);
        QCOMPARE(controller.settings()->collapseEmbeds(), false);

        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller), d));
        settle();
        QVERIFY2(find(d, "imageMedia") != nullptr,
                 "the picture is not rendered with the setting off — the "
                 "default is not today's behaviour");
        QVERIFY2(find(d, "collapsedEmbedRow") == nullptr,
                 "a summary line appeared with the setting off");
    }

    // The block becomes a line and the row gets shorter.
    void collapsingReplacesTheAttachmentWithOneLine()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller), d));
        settle();
        const qreal expandedHeight = d.root->implicitHeight();
        QVERIFY(expandedHeight > 0.0);

        // Flipped live on an existing delegate, as when a reader toggles the
        // switch with a room open.
        controller.settings()->setCollapseEmbeds(true);
        settle();

        auto *line = find(d, "collapsedEmbedRow");
        QVERIFY2(line != nullptr,
                 "no summary line after turning the setting on");
        QVERIFY(line->isVisible());
        QVERIFY2(line->height() > 0.0 && line->height() < 48.0,
                 qPrintable(QStringLiteral("the summary is %1px tall — that is "
                                           "not one line")
                                .arg(line->height())));
        QVERIFY2(d.root->implicitHeight() < expandedHeight,
                 qPrintable(QStringLiteral("the row is %1px collapsed against "
                                           "%2px expanded — collapsing saved "
                                           "nothing")
                                .arg(d.root->implicitHeight())
                                .arg(expandedHeight)));
    }

    // A collapsed attachment is not instantiated at all: a `visible: false`
    // Image still downloads, decodes and animates. Every MediaBridge call for
    // an attachment lives inside the component this looks for.
    void aCollapsedAttachmentIsNotEvenBuilt()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setCollapseEmbeds(true);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller), d));
        settle();

        QVERIFY(find(d, "collapsedEmbedRow") != nullptr);
        QVERIFY2(find(d, "imageMedia") == nullptr,
                 "the image component is still in the object tree while the "
                 "row is collapsed — it is merely hidden, so it still fetches "
                 "and decodes the picture");
    }

    // The line names the kind and what that surface already knows, and the
    // accessible name carries the same, since a screen reader gets nothing
    // from the glyph.
    void theLineSaysWhatTheAttachmentIs()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setCollapseEmbeds(true);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller), d));
        settle();

        auto *label = find(d, "collapsedEmbedLabel");
        QVERIFY(label != nullptr);
        const QString text = label->property("text").toString();
        QVERIFY2(text.contains(QStringLiteral("Image")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("holiday.png")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("1920")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("1080")), qPrintable(text));

        // `Accessible.name` is attached and not readable via
        // QObject::property(); it and the visible label both come from
        // `summaryText`, which is asserted.
        auto *line = find(d, "collapsedEmbedRow");
        QVERIFY(line != nullptr);
        const QString summary = line->property("summaryText").toString();
        QVERIFY2(summary.contains(QStringLiteral("Image"))
                     && summary.contains(QStringLiteral("holiday.png")),
                 qPrintable(QStringLiteral("the spoken summary is \"%1\" — a "
                                           "screen reader cannot tell what "
                                           "this attachment is")
                                .arg(summary)));

        // A GIF says GIF, not "Image".
        QVariantMap gif = imageFixture(controller);
        gif.insert(QStringLiteral("mediaMimetype"), QStringLiteral("image/gif"));
        gif.insert(QStringLiteral("mediaFilename"), QStringLiteral("cat.gif"));
        Delegate g;
        QVERIFY(createDelegate(controller, gif, g));
        settle();
        auto *gifLabel = find(g, "collapsedEmbedLabel");
        QVERIFY(gifLabel != nullptr);
        QVERIFY2(gifLabel->property("text").toString()
                     .contains(QStringLiteral("GIF")),
                 qPrintable(gifLabel->property("text").toString()));
    }

    // Expansion is reversible: the line stays above the picture, so the
    // setting keeps applying to rows the reader opened.
    void expandingRestoresTheAttachmentAndTheLineStays()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setCollapseEmbeds(true);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller), d));
        settle();
        auto *line = find(d, "collapsedEmbedRow");
        QVERIFY(line != nullptr);
        QCOMPARE(line->property("expanded").toBool(), false);
        QCOMPARE(d.root->property("embedExpanded").toBool(), false);

        // Exactly what the TapHandler does.
        QVERIFY(QMetaObject::invokeMethod(line, "toggleRequested"));
        settle();

        QCOMPARE(d.root->property("embedExpanded").toBool(), true);
        QVERIFY2(find(d, "imageMedia") != nullptr,
                 "expanding did not build the picture");
        auto *stillThere = find(d, "collapsedEmbedRow");
        QVERIFY2(stillThere != nullptr,
                 "the summary line vanished on expand — there is now no way "
                 "to collapse this row again");
        QCOMPARE(stillThere->property("expanded").toBool(), true);

        // And back.
        QVERIFY(QMetaObject::invokeMethod(stillThere, "toggleRequested"));
        settle();
        QCOMPARE(d.root->property("embedExpanded").toBool(), false);
        QVERIFY2(find(d, "imageMedia") == nullptr,
                 "collapsing again left the picture built");
    }

    // The line is the only way to the attachment while the setting is on, so
    // it must work without a pointer.
    void theLineIsOperableFromTheKeyboard()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setCollapseEmbeds(true);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller), d));
        QVERIFY(QTest::qWaitForWindowExposed(d.window.get()));
        d.window->requestActivate();
        settle();

        auto *line = find(d, "collapsedEmbedRow");
        QVERIFY(line != nullptr);
        QVERIFY2(line->property("activeFocusOnTab").toBool(),
                 "the summary line is not in the tab chain, so a keyboard "
                 "user cannot reach the attachment at all");
        line->forceActiveFocus();
        settle();
        QVERIFY2(line->hasActiveFocus(),
                 "the summary line refused focus");

        QTest::keyClick(d.window.get(), Qt::Key_Return);
        settle();
        QVERIFY2(d.root->property("embedExpanded").toBool(),
                 "Return on the focused summary line did not expand it");

        // Right/Left are directional: Right twice does not close it.
        QTest::keyClick(d.window.get(), Qt::Key_Right);
        settle();
        QCOMPARE(d.root->property("embedExpanded").toBool(), true);
        QTest::keyClick(d.window.get(), Qt::Key_Left);
        settle();
        QCOMPARE(d.root->property("embedExpanded").toBool(), false);
    }

    // Each kind names itself with what it knows: a voice message its length
    // (its filename is generated), a file its size.
    void eachKindNamesItselfWithWhatItKnows()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setCollapseEmbeds(true);

        QVariantMap voice = baseFixture(controller);
        voice.insert(QStringLiteral("isAudio"), true);
        voice.insert(QStringLiteral("mediaIsVoice"), true);
        voice.insert(QStringLiteral("mediaDurationMs"), 12000);
        voice.insert(QStringLiteral("mediaFilename"),
                     QStringLiteral("voice-message-1758000000.ogg"));
        Delegate v;
        QVERIFY(createDelegate(controller, voice, v));
        settle();
        auto *voiceLabel = find(v, "collapsedEmbedLabel");
        QVERIFY(voiceLabel != nullptr);
        const QString voiceText = voiceLabel->property("text").toString();
        QVERIFY2(voiceText.contains(QStringLiteral("Voice message")),
                 qPrintable(voiceText));
        QVERIFY2(voiceText.contains(QStringLiteral("0:12")),
                 qPrintable(voiceText));
        QVERIFY2(!voiceText.contains(QStringLiteral("1758000000")),
                 qPrintable(QStringLiteral("the generated filename is in the "
                                           "summary: \"%1\"").arg(voiceText)));
        QVERIFY2(find(v, "audioMedia") == nullptr,
                 "the audio card is built while collapsed — it prefetches");

        QVariantMap file = baseFixture(controller);
        file.insert(QStringLiteral("isFile"), true);
        file.insert(QStringLiteral("mediaFilename"),
                    QStringLiteral("budget.pdf"));
        file.insert(QStringLiteral("mediaMimetype"),
                    QStringLiteral("application/pdf"));
        file.insert(QStringLiteral("mediaSize"), 2 * 1024 * 1024);
        Delegate f;
        QVERIFY(createDelegate(controller, file, f));
        settle();
        auto *fileLabel = find(f, "collapsedEmbedLabel");
        QVERIFY(fileLabel != nullptr);
        const QString fileText = fileLabel->property("text").toString();
        QVERIFY2(fileText.contains(QStringLiteral("budget.pdf")),
                 qPrintable(fileText));
        QVERIFY2(fileText.contains(QStringLiteral("2.0 MB")),
                 qPrintable(fileText));
        QVERIFY2(find(f, "fileCard") == nullptr,
                 "the file card is built while collapsed");

        // Stickers arrive with no dimensions by design (rust/src/stickers.rs
        // sends `w: 0, h: 0`; a pack entry's `info` is advisory), so the
        // summary needs another second fact.
        QVariantMap sticker = baseFixture(controller);
        sticker.insert(QStringLiteral("isSticker"), true);
        sticker.insert(QStringLiteral("mediaMimetype"),
                       QStringLiteral("image/webp"));
        sticker.insert(QStringLiteral("mediaWidth"), 0);
        sticker.insert(QStringLiteral("mediaHeight"), 0);
        sticker.insert(QStringLiteral("mediaSize"), 42 * 1024);
        sticker.insert(QStringLiteral("mediaKey"),
                       QStringLiteral("fixture-sticker"));
        Delegate st;
        QVERIFY(createDelegate(controller, sticker, st));
        settle();
        auto *stickerLabel = find(st, "collapsedEmbedLabel");
        QVERIFY(stickerLabel != nullptr);
        const QString stickerText = stickerLabel->property("text").toString();
        QVERIFY2(stickerText.contains(QStringLiteral("42 KB")),
                 qPrintable(QStringLiteral("a dimensionless sticker carries "
                                           "no second fact: \"%1\"")
                                .arg(stickerText)));
        // A sticker that does know its size still leads with it.
        QVariantMap measured = sticker;
        measured.insert(QStringLiteral("mediaWidth"), 512);
        measured.insert(QStringLiteral("mediaHeight"), 512);
        Delegate ms;
        QVERIFY(createDelegate(controller, measured, ms));
        settle();
        auto *measuredLabel = find(ms, "collapsedEmbedLabel");
        QVERIFY(measuredLabel != nullptr);
        const QString measuredText =
            measuredLabel->property("text").toString();
        QVERIFY2(measuredText.contains(QStringLiteral("512×512")),
                 qPrintable(measuredText));
        QVERIFY2(!measuredText.contains(QStringLiteral("42 KB")),
                 qPrintable(QStringLiteral("the size fallback fired beside "
                                           "real dimensions: \"%1\"")
                                .arg(measuredText)));
    }

    // A loaded link preview collapses; a consent gate is already one band and
    // holds the reader's only control over an outbound request, so it does
    // not.
    void aLoadedLinkPreviewCollapsesAndAConsentGateDoesNot()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setCollapseEmbeds(true);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("https://www.lightning-matrix.org/"));
        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));

        d.root->setProperty("preview", QVariant::fromValue(consentGatePreview()));
        settle();
        QVERIFY2(find(d, "linkPreviewCard") != nullptr,
                 "the consent gate was collapsed — the privacy control is now "
                 "behind an extra click");
        QVERIFY2(find(d, "collapsedEmbedRow") == nullptr,
                 "a summary line replaced the consent gate");

        d.root->setProperty("preview", QVariant::fromValue(loadedPreview()));
        settle();
        auto *label = find(d, "collapsedEmbedLabel");
        QVERIFY2(label != nullptr, "the loaded preview card did not collapse");
        const QString text = label->property("text").toString();
        QVERIFY2(text.contains(QStringLiteral("Link")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("lightning-matrix.org")),
                 qPrintable(text));
        QVERIFY2(find(d, "linkPreviewCard") == nullptr,
                 "the loaded card is still built while collapsed");
    }

    // A reply quote is conversational context, not media, and stays.
    void theReplyQuoteIsNotAnEmbed()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setCollapseEmbeds(true);
        QVariantMap fixture = baseFixture(controller);
        fixture.insert(QStringLiteral("body"),
                       QStringLiteral("agreed, let us do that"));
        fixture.insert(QStringLiteral("replyToEventId"),
                       QStringLiteral("$target"));
        fixture.insert(QStringLiteral("replyToSender"),
                       QStringLiteral("Someone"));
        fixture.insert(QStringLiteral("replyToSenderId"),
                       QStringLiteral("@someone:mock.local"));
        fixture.insert(QStringLiteral("replyToBody"),
                       QStringLiteral("shall we move the meeting?"));
        Delegate d;
        QVERIFY(createDelegate(controller, fixture, d));
        settle();

        auto *quote = find(d, "replyNavigationTarget");
        QVERIFY2(quote != nullptr && quote->isVisible(),
                 "the reply quote was collapsed — it is context, not media");
        QVERIFY2(find(d, "collapsedEmbedRow") == nullptr,
                 "a summary line appeared on a plain text reply");
    }

    // This adds bindings to every timeline row, so load errors and binding
    // loops (invisible to source scans) are asserted directly.
    void theDelegateLoadsCleanlyWithTheSettingOn()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setCollapseEmbeds(true);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller), d));
        settle();
        QVERIFY(QMetaObject::invokeMethod(find(d, "collapsedEmbedRow"),
                                          "toggleRequested"));
        settle();
        // The loop assertion first: a binding loop leaves the component loaded
        // and scans passing while Qt abandons an evaluation.
        for (const QString &w : std::as_const(d.warnings)) {
            QVERIFY2(!w.contains(QStringLiteral("Binding loop")), qPrintable(w));
        }
        QCOMPARE(d.warnings, QStringList{});
    }

    // In Bubbles the bubble width comes from `bubbleContent.implicitWidth`, so
    // a child reading `bubble.width` and contributing implicit width back is a
    // cycle that Qt resolves by pinning it to one pixel. Asserted in both
    // states after layout settles.
    void theSummaryLineSurvivesTheBubblesLayout()
    {
        AppController controller(AppController::MockBackend);
        controller.settings()->setCollapseEmbeds(true);
        controller.settings()->setMessageLayout(1);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller), d));
        d.root->setProperty("isDirectRoom", true);
        settle();

        auto *line = find(d, "collapsedEmbedRow");
        QVERIFY(line != nullptr);
        auto *label = find(d, "collapsedEmbedLabel");
        QVERIFY(label != nullptr);
        QVERIFY2(line->width() > 40.0,
                 qPrintable(QStringLiteral("the summary row is %1px wide in "
                                           "Bubbles — it has been pinned by a "
                                           "width cycle, not laid out")
                                .arg(line->width())));
        QVERIFY2(label->width() > 10.0,
                 qPrintable(QStringLiteral("the summary label is %1px wide")
                                .arg(label->width())));

        QVERIFY(QMetaObject::invokeMethod(line, "toggleRequested"));
        settle();
        QVERIFY(find(d, "imageMedia") != nullptr);
        QVERIFY2(find(d, "collapsedEmbedRow")->width() > 40.0,
                 "the summary row collapsed to nothing once the picture was "
                 "beside it in a content-sized bubble");
        for (const QString &w : std::as_const(d.warnings)) {
            QVERIFY2(!w.contains(QStringLiteral("Binding loop")), qPrintable(w));
        }
    }
};

QTEST_MAIN(CollapsedEmbedsQmlTest)
#include "CollapsedEmbedsQmlTest.moc"
