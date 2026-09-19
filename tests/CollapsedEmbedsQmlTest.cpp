// 2026-09-19: "modern media, too much clutter, please add an option to reduce
// all embeds in to single lines, with an expanding arrow or mouse over or
// keyboard shortcut something something".
//
// Settings → Appearance → Timeline → "Collapse media and link embeds" is that
// option, and this suite is what keeps it honest. It drives the PRODUCTION
// MessageDelegate offscreen, exactly as LinkPreviewQmlTest does, because
// every property under test here is a load-time/layout fact that a scan over
// the .qml source cannot see.
//
// WHAT EACH CASE WOULD DO TO THE UNFIXED TREE. The suite does not compile at
// all against a SettingsManager without `setCollapseEmbeds` — that is the C++
// half, and it is unambiguous. Given the property but a delegate that ignored
// it, every case below fails on its own assertion instead:
//
//   * collapsingReplacesTheAttachmentWithOneLine — `collapsedEmbedRow` is not
//     found (old tree: the object does not exist), and the row's height does
//     not fall.
//   * aCollapsedAttachmentIsNotEvenBuilt — `imageMedia` is still in the tree.
//     This is the assertion that matters most and the one a `visible: false`
//     implementation would fail: a hidden Image still downloads, decodes and,
//     for a GIF, animates. The collapse must not instantiate the component.
//   * theLineSaysWhatTheAttachmentIs — no label to read.
//   * expandingRestoresTheAttachmentAndTheLineStays — nothing to activate.
//   * theLineIsOperableFromTheKeyboard — same.
//   * aLoadedLinkPreviewCollapsesAndAConsentGateDoesNot — the loaded card is
//     still a card (first half), and on a naive implementation that collapsed
//     every preview state the consent gate would vanish behind a second click
//     (second half).
//   * theReplyQuoteIsNotAnEmbed — a naive "collapse everything in the bubble"
//     would take the reply quote with it.
//   * theSettingIsOffByDefault — a default flip is the one change that would
//     reach every existing install without being asked for.
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

    // A complete role map with safe defaults so the production delegate binds
    // without undefined-property warnings. Taken from the real model's
    // roleNames so a role added later arrives here as a null QVariant rather
    // than as a missing property.
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
        // FALSE, and not incidental: with no bridge source and an empty
        // thumb URL the image component resolves to no source at all, so
        // this suite never touches the network or the media cache. A `true`
        // here would make the fixture itself the thing under test.
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

    // A 1920×1080 PNG called holiday.png. The three facts the collapsed line
    // is supposed to carry are all here, and all three are asserted.
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

    // The delegate reads room encryption and several viewport facts from its
    // HOST PANE, not from the row. A real QML object, not a QVariantMap:
    // several bindings guard on `timelineView` being truthy and then CALL a
    // method on it, so a plain map turns each of those into a TypeError and
    // the no-warnings assertion stops meaning anything.
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

    // Let every nested Loader build and every layout polish. The collapse
    // toggle destroys one subtree and builds another, and a measurement
    // taken mid-swap is measuring neither.
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
    // A private config root, so flipping a persisted setting here can never
    // reach the maintainer's own store or another suite's — SettingsManager
    // writes through a plain QSettings and QmlComponentLoadTest records what
    // a leftover value costs the next run.
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

    // TODAY'S BEHAVIOUR IS UNCHANGED FOR ANYONE WHO DOES NOT OPT IN. A
    // default flip is the one thing here that would reach every existing
    // install without being asked for.
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

    // The block becomes a line, and the row gets shorter. Both halves: a
    // summary that appeared BESIDE a full-size picture would save nothing.
    void collapsingReplacesTheAttachmentWithOneLine()
    {
        AppController controller(AppController::MockBackend);
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller), d));
        settle();
        const qreal expandedHeight = d.root->implicitHeight();
        QVERIFY(expandedHeight > 0.0);

        // Flipped LIVE, on a delegate that already exists: this is what a
        // reader toggling the switch with a room open actually does, and a
        // one-shot read at creation would pass without supporting it.
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

    // THE ASSERTION THAT MATTERS MOST, and the one a `visible: false`
    // implementation fails. A hidden Image still downloads, decodes and (for
    // a GIF) animates; only a component that was never instantiated can be
    // said to fetch nothing. Every MediaBridge call site for an attachment
    // lives inside the component this looks for.
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

    // A collapsed embed that reads "Attachment" has replaced clutter with a
    // mystery. The line carries the kind and everything that surface already
    // knows, and the ACCESSIBLE NAME carries both too, because a screen
    // reader gets nothing from the glyph.
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

        // `Accessible.name` is an attached property and is not readable
        // through QObject::property(); it is built from `summaryText`, which
        // is, so that is what is asserted. Both the visible label and the
        // spoken name resolve from this one string, so a screen reader
        // cannot be told less than the screen shows.
        auto *line = find(d, "collapsedEmbedRow");
        QVERIFY(line != nullptr);
        const QString summary = line->property("summaryText").toString();
        QVERIFY2(summary.contains(QStringLiteral("Image"))
                     && summary.contains(QStringLiteral("holiday.png")),
                 qPrintable(QStringLiteral("the spoken summary is \"%1\" — a "
                                           "screen reader cannot tell what "
                                           "this attachment is")
                                .arg(summary)));

        // And a GIF says GIF, because "Image" for a GIF is the kind of
        // almost-right label that makes a reader expand it to find out.
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

    // The expansion is REVERSIBLE, which is why the line stays above the
    // picture instead of being replaced by it. A one-way expand would mean
    // the setting silently stopped applying to every row the reader had ever
    // opened, and there would be no way back short of leaving the room.
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

    // The disclosure is the ONLY way to the attachment while the setting is
    // on, so it has to work without a pointer — the same argument
    // MediaHiddenPlaceholder makes for "Show image".
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

        // Right/Left are directional, not toggling: pressing Right twice
        // must not close what the first press opened.
        QTest::keyClick(d.window.get(), Qt::Key_Right);
        settle();
        QCOMPARE(d.root->property("embedExpanded").toBool(), true);
        QTest::keyClick(d.window.get(), Qt::Key_Left);
        settle();
        QCOMPARE(d.root->property("embedExpanded").toBool(), false);
    }

    // A voice message has a generated filename nobody chose; its LENGTH is
    // the only thing worth a line. A file card says how big it is. Both are
    // "whatever that surface already knows", which is the rule the line is
    // written to.
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

        // A STICKER ARRIVES WITH NO DIMENSIONS BY DESIGN, and the summary
        // still owes the reader a second fact. rust/src/stickers.rs sends
        // `w: 0, h: 0` deliberately — a pack entry's `info` is advisory and
        // it will not put an image decoder in the bridge to fill it — so
        // "1920×1080" is unreachable for the one kind that usually has no
        // filename either, and the line collapsed to the bare word "Sticker".
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
        // And a sticker that DOES know its size still leads with it, so the
        // fallback has not displaced the better answer.
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

    // A LOADED preview is a block and collapses. A CONSENT GATE is already
    // one band and carries the only control the reader has over an outbound
    // request — putting a second click in front of it would be a worse trade
    // than the space it saves, and this is where that decision is pinned.
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

    // "All embeds" from a user means the big visual blocks. A reply quote is
    // conversational context, not media: a reply whose quote is one word of
    // chrome is unreadable, which is the opposite of decluttering.
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

    // The delegate is the hottest QML in the application and this adds
    // bindings to every row of it. A load-time error or a binding loop here
    // is invisible to every source scan (§16), so it is asserted directly.
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
        // The loop assertion first and by itself, because it is the one that
        // is otherwise unobservable: a binding loop leaves the component
        // loaded, the root non-null and every source scan passing, while Qt
        // has abandoned one evaluation and the property keeps whatever the
        // aborted pass left behind.
        for (const QString &w : std::as_const(d.warnings)) {
            QVERIFY2(!w.contains(QStringLiteral("Binding loop")), qPrintable(w));
        }
        QCOMPARE(d.warnings, QStringList{});
    }

    // AND IN BUBBLES, WHERE THE BUBBLE IS SIZED FROM ITS OWN CONTENT.
    //
    // In Modern and Compact `bubble.width` is a function of the row, so a
    // child that reads it is reading a constant. In Bubbles (DMs only) the
    // bubble's width comes from `bubbleContent.implicitWidth` — so a child
    // that reads `bubble.width` and contributes an implicit width back is in
    // a cycle, and §16 records what Qt does with one of those: it pins the
    // offender to ONE PIXEL of contributed width and says nothing. The
    // summary row is in exactly that position. Asserted here rather than
    // reasoned about, in both states, because the cycle only closes when the
    // layout has settled.
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
