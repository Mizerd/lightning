#include <QtTest/QtTest>

#include <iterator>
#include <memory>

#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "ObjectCensus.h"
#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "media/MediaImageProvider.h"
#include "models/TimelineModel.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;

QString describeItem(QObject *object)
{
    QString id;
    if (QQmlContext *context = qmlContext(object))
        id = context->nameForObject(object);
    return QStringLiteral("%1#%2[%3]")
        .arg(census::className(object), id, object->objectName());
}

// Every hidden subtree whose parent is shown, with its object count.
void hiddenSubtrees(QQuickItem *item, QList<QPair<int, QString>> &out, const QString &path)
{
    for (QQuickItem *child : item->childItems()) {
        const QString childPath = path + QLatin1Char('/') + describeItem(child);
        if (!child->isVisible()) {
            const int n = int(census::objectsUnder(child).size());
            out.append({ n, childPath });
            continue;
        }
        hiddenSubtrees(child, out, childPath);
    }
}
} // namespace

class RowObjectCensusQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
        QObject *host = nullptr;
        QStringList warnings;
    };

    QTemporaryDir m_dir;
    QString m_png;

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
        fixture.insert(QStringLiteral("sender"), QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"), QStringLiteral("Hello there"));
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc());
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

    QVariantMap imageFixture(AppController &controller, const QString &mime) const
    {
        QVariantMap f = baseFixture(controller);
        f.insert(QStringLiteral("isImage"), true);
        f.insert(QStringLiteral("body"), QStringLiteral("picture.png"));
        f.insert(QStringLiteral("mediaFilename"), QStringLiteral("picture.png"));
        f.insert(QStringLiteral("mediaMimetype"), mime);
        f.insert(QStringLiteral("mediaWidth"), 320);
        f.insert(QStringLiteral("mediaHeight"), 200);
        f.insert(QStringLiteral("mediaUrl"), QUrl::fromLocalFile(m_png));
        return f;
    }

    static QObject *hostObject(QQmlEngine *engine, QObject *owner)
    {
        QQmlComponent component(engine);
        component.setData(R"QML(
import QtQuick
QtObject {
    property bool roomEncrypted: true
    property real contentY: 0
    property real height: 10000
    property bool speculativeMediaAllowed: true
    property bool userScrollActive: false
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

    bool createDelegate(AppController &controller, const QVariantMap &fixture, Delegate &out)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        out.engine->addImageProvider(QStringLiteral("lightning-media"),
                                     new MediaImageProvider(controller.mediaBridge()));
        connect(out.engine.get(), &QQmlEngine::warnings, this,
                [&out](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        out.warnings << e.toString();
                });
        out.engine->rootContext()->setContextProperty("app", &controller);
        out.engine->rootContext()->setContextProperty("model", fixture);
        QSignalSpy createdSpy(out.engine.get(), &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("MessageDelegate"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return false;
        out.root = qobject_cast<QQuickItem *>(createdSpy.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(760, 600);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(700);
        out.window->show();
        QCoreApplication::processEvents();
        out.host = hostObject(out.engine.get(), out.root);
        if (!out.host)
            return false;
        out.root->setProperty("timelineView", QVariant::fromValue(out.host));
        QCoreApplication::processEvents();
        QTest::qWait(50);
        return true;
    }

    QList<QPair<QString, QVariantMap>> surveyFixtures(AppController &c) const
    {
        QList<QPair<QString, QVariantMap>> out;
        out.append({ QStringLiteral("text"), baseFixture(c) });
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("showSenderIdentity"), false);
            out.append({ QStringLiteral("text-continuation"), f });
        }
        out.append({ QStringLiteral("image-png"), imageFixture(c, QStringLiteral("image/png")) });
        {
            QVariantMap f = imageFixture(c, QStringLiteral("image/png"));
            f.insert(QStringLiteral("showSenderIdentity"), false);
            out.append({ QStringLiteral("image-png-continuation"), f });
        }
        out.append({ QStringLiteral("image-jpeg"), imageFixture(c, QStringLiteral("image/jpeg")) });
        out.append({ QStringLiteral("image-gif"), imageFixture(c, QStringLiteral("image/gif")) });
        {
            QVariantMap f = imageFixture(c, QStringLiteral("image/png"));
            f.insert(QStringLiteral("mediaUrl"), QUrl{});
            f.insert(QStringLiteral("mediaSourceAvailable"), true);
            f.insert(QStringLiteral("mediaKey"), QStringLiteral("$fixture"));
            out.append({ QStringLiteral("image-bridge-pending"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("isSticker"), true);
            f.insert(QStringLiteral("body"), QStringLiteral("a sticker"));
            f.insert(QStringLiteral("mediaMimetype"), QStringLiteral("image/png"));
            f.insert(QStringLiteral("mediaWidth"), 128);
            f.insert(QStringLiteral("mediaHeight"), 128);
            f.insert(QStringLiteral("mediaUrl"), QUrl::fromLocalFile(m_png));
            out.append({ QStringLiteral("sticker"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("isVideo"), true);
            f.insert(QStringLiteral("body"), QStringLiteral("clip.mp4"));
            f.insert(QStringLiteral("mediaFilename"), QStringLiteral("clip.mp4"));
            f.insert(QStringLiteral("mediaMimetype"), QStringLiteral("video/mp4"));
            f.insert(QStringLiteral("mediaWidth"), 640);
            f.insert(QStringLiteral("mediaHeight"), 360);
            f.insert(QStringLiteral("mediaDurationMs"), 12000);
            f.insert(QStringLiteral("mediaSourceAvailable"), true);
            f.insert(QStringLiteral("mediaThumbAvailable"), true);
            f.insert(QStringLiteral("mediaKey"), QStringLiteral("$video"));
            out.append({ QStringLiteral("video"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("isFile"), true);
            f.insert(QStringLiteral("body"), QStringLiteral("notes.pdf"));
            f.insert(QStringLiteral("mediaFilename"), QStringLiteral("notes.pdf"));
            f.insert(QStringLiteral("mediaMimetype"), QStringLiteral("application/pdf"));
            f.insert(QStringLiteral("mediaSize"), 20480);
            f.insert(QStringLiteral("mediaSourceAvailable"), true);
            f.insert(QStringLiteral("mediaKey"), QStringLiteral("$file"));
            out.append({ QStringLiteral("file"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("isAudio"), true);
            f.insert(QStringLiteral("body"), QStringLiteral("voice.ogg"));
            f.insert(QStringLiteral("mediaFilename"), QStringLiteral("voice.ogg"));
            f.insert(QStringLiteral("mediaMimetype"), QStringLiteral("audio/ogg"));
            f.insert(QStringLiteral("mediaDurationMs"), 4000);
            f.insert(QStringLiteral("mediaSourceAvailable"), true);
            f.insert(QStringLiteral("mediaKey"), QStringLiteral("$audio"));
            out.append({ QStringLiteral("audio"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("replyToEventId"), QStringLiteral("$parent"));
            f.insert(QStringLiteral("replyToSender"), QStringLiteral("Someone"));
            f.insert(QStringLiteral("replyToPreview"), QStringLiteral("the original"));
            out.append({ QStringLiteral("reply"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("isThreadRoot"), true);
            f.insert(QStringLiteral("threadReplyCount"), 3);
            f.insert(QStringLiteral("threadLatestSenderDisplayName"), QStringLiteral("Bob"));
            f.insert(QStringLiteral("threadLatestPreview"), QStringLiteral("latest"));
            out.append({ QStringLiteral("thread-root"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            QVariantMap r1{ { QStringLiteral("key"), QStringLiteral("👍") },
                            { QStringLiteral("count"), 2 },
                            { QStringLiteral("byMe"), false } };
            QVariantMap r2{ { QStringLiteral("key"), QStringLiteral("🎉") },
                            { QStringLiteral("count"), 1 },
                            { QStringLiteral("byMe"), true } };
            f.insert(QStringLiteral("reactions"), QVariantList{ r1, r2 });
            out.append({ QStringLiteral("reactions-2"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            QVariantList receipts;
            for (int i = 0; i < 5; ++i)
                receipts.append(QVariantMap{
                    { QStringLiteral("userId"), QStringLiteral("@r%1:mock.local").arg(i) },
                    { QStringLiteral("displayName"), QStringLiteral("Reader %1").arg(i) } });
            f.insert(QStringLiteral("readReceipts"), receipts);
            f.insert(QStringLiteral("readReceiptsTotal"), 5);
            out.append({ QStringLiteral("receipts-5"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("isOwn"), true);
            f.insert(QStringLiteral("sentReceipt"), true);
            out.append({ QStringLiteral("own-sent"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("undecryptable"), true);
            f.insert(QStringLiteral("errorKind"), QStringLiteral("withheld"));
            f.insert(QStringLiteral("body"), QStringLiteral("Unable to decrypt"));
            out.append({ QStringLiteral("undecryptable"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("redacted"), true);
            f.insert(QStringLiteral("body"), QString{});
            out.append({ QStringLiteral("redacted"), f });
        }
        {
            QVariantMap f = baseFixture(c);
            f.insert(QStringLiteral("edited"), true);
            out.append({ QStringLiteral("edited"), f });
        }
        {
            QVariantMap f = imageFixture(c, QStringLiteral("image/png"));
            QVariantList items;
            for (int i = 0; i < 3; ++i)
                items.append(QVariantMap{
                    { QStringLiteral("mediaKey"), QStringLiteral("$g%1").arg(i) },
                    { QStringLiteral("kind"), QStringLiteral("image") },
                    { QStringLiteral("filename"), QStringLiteral("g%1.png").arg(i) },
                    { QStringLiteral("mimetype"), QStringLiteral("image/png") },
                    { QStringLiteral("width"), 320 },
                    { QStringLiteral("height"), 200 } });
            f.insert(QStringLiteral("galleryItems"), items);
            out.append({ QStringLiteral("gallery-3"), f });
        }
        return out;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_png = m_dir.filePath(QStringLiteral("picture.png"));
        QImage pixels(32, 20, QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::red);
        QVERIFY(pixels.save(m_png));
    }

    void survey()
    {
        if (qEnvironmentVariableIsEmpty("LIGHTNING_ROW_CENSUS_SURVEY"))
            QSKIP("set LIGHTNING_ROW_CENSUS_SURVEY=1 to print each row kind's census");
        AppController controller(AppController::MockBackend);
        const auto fixtures = surveyFixtures(controller);
        for (const auto &entry : fixtures) {
            Delegate d;
            QVERIFY2(createDelegate(controller, entry.second, d), qPrintable(entry.first));
            QTest::qWait(150);
            const auto all = census::objectsUnder(d.root);
            qInfo().noquote() << "SURVEY" << entry.first << "TOTAL" << census::describe(all, 14);
            QList<QPair<int, QString>> hidden;
            hiddenSubtrees(d.root, hidden, QString());
            std::stable_sort(hidden.begin(), hidden.end(),
                             [](const auto &a, const auto &b) { return a.first > b.first; });
            for (const auto &h : hidden) {
                if (h.first >= 2)
                    qInfo().noquote() << "SURVEY" << entry.first << "HIDDEN" << h.first << h.second;
            }
            for (const char *name : { "imageMedia", "stickerMedia", "videoMedia", "fileCard",
                                      "audioMedia", "messageGallery", "messageBody",
                                      "replyNavigationTarget", "threadSummaryCard",
                                      "reactionsFlow", "readReceiptStrip",
                                      "undecryptableActions", "messagePresentationRow" }) {
                const auto items = d.root->findChildren<QObject *>(QLatin1String(name));
                for (QObject *o : items)
                    qInfo().noquote() << "SURVEY" << entry.first << "SUBTREE" << name
                                      << census::describe(census::objectsUnder(o), 10);
            }
        }
    }

    void mediaRowsBuildNoBodyText()
    {
        AppController controller(AppController::MockBackend);
        const QStringList kinds{ QStringLiteral("image-png"), QStringLiteral("image-gif"),
                                 QStringLiteral("sticker"), QStringLiteral("video"),
                                 QStringLiteral("file"), QStringLiteral("audio"),
                                 QStringLiteral("gallery-3") };
        int checked = 0;
        for (const auto &entry : surveyFixtures(controller)) {
            if (!kinds.contains(entry.first))
                continue;
            Delegate d;
            QVERIFY2(createDelegate(controller, entry.second, d), qPrintable(entry.first));
            QVERIFY2(!d.root->findChild<QObject *>(QStringLiteral("messageBody")),
                     qPrintable(entry.first + QStringLiteral(" built a body it never shows")));
            QCOMPARE(d.warnings, QStringList{});
            ++checked;
        }
        QCOMPARE(checked, kinds.size());

        QVariantMap captioned = imageFixture(controller, QStringLiteral("image/png"));
        captioned.insert(QStringLiteral("body"), QStringLiteral("Sunset over the harbour"));
        Delegate c;
        QVERIFY(createDelegate(controller, captioned, c));
        auto *body = c.root->findChild<QQuickItem *>(QStringLiteral("messageBody"));
        QVERIFY(body && body->isVisible());
        QVERIFY(body->property("text").toString().contains(QStringLiteral("Sunset")));
    }

    void aContinuationRowWithoutBodyKeepsTheEmptyLineHeight()
    {
        AppController controller(AppController::MockBackend);
        for (bool compact : { false, true }) {
            controller.settings()->setMessageLayout(compact ? 1 : 0);
            QVariantMap f = imageFixture(controller, QStringLiteral("image/png"));
            f.insert(QStringLiteral("showSenderIdentity"), false);
            Delegate d;
            QVERIFY(createDelegate(controller, f, d));
            auto *loader = d.root->findChild<QQuickItem *>(QStringLiteral("messageBodyLoader"));
            auto *slot = d.root->findChild<QQuickItem *>(QStringLiteral("senderAvatarSlot"));
            QVERIFY(loader && slot);
            QVERIFY(!loader->property("item").value<QObject *>());

            QQmlComponent reference(d.engine.get());
            reference.setData("import QtQuick\nTextEdit { textFormat: TextEdit.RichText;"
                              " wrapMode: TextEdit.Wrap; readOnly: true; text: \"\" }",
                              QUrl());
            std::unique_ptr<QObject> edit(reference.create());
            QVERIFY2(edit, qPrintable(reference.errorString()));
            edit->setProperty("font", loader->property("bodyFont"));
            auto *editItem = qobject_cast<QQuickItem *>(edit.get());
            editItem->setParentItem(d.root);
            editItem->setWidth(400);
            QCoreApplication::processEvents();
            QVERIFY(editItem->implicitHeight() > 0.0);
            QCOMPARE(slot->implicitHeight(), editItem->implicitHeight());
        }
        controller.settings()->setMessageLayout(0);
    }

    void anImageBuildsItsSkeletonOnlyWhileItCanShow()
    {
        AppController controller(AppController::MockBackend);
        {
            QVariantMap pending = imageFixture(controller, QStringLiteral("image/png"));
            pending.insert(QStringLiteral("mediaUrl"), QUrl{});
            Delegate d;
            QVERIFY(createDelegate(controller, pending, d));
            auto *skeleton = d.root->findChild<QQuickItem *>(QStringLiteral("imageSkeleton"));
            QVERIFY2(skeleton && skeleton->isVisible(), "a loading image shows no skeleton");
        }
        Delegate d;
        QVERIFY(createDelegate(controller, imageFixture(controller, QStringLiteral("image/png")), d));
        auto *image = d.root->findChild<QQuickItem *>(QStringLiteral("messageStillImage"));
        QVERIFY(image);
        QTRY_COMPARE(image->property("status").toInt(), 1);
        QTRY_VERIFY2(!d.root->findChild<QObject *>(QStringLiteral("imageSkeleton")),
                     "a drawn image still carries its loading skeleton");

        QVERIFY(d.root->setProperty("mediaPixmapRetained", false));
        QTRY_COMPARE(image->property("status").toInt(), 0);
        QVERIFY(!d.root->findChild<QObject *>(QStringLiteral("imageSkeleton")));
        d.host->setProperty("userScrollActive", true);
        QVERIFY(d.root->setProperty("mediaPixmapRetained", true));
        QVERIFY2(!d.root->findChild<QObject *>(QStringLiteral("imageSkeleton")),
                 "the skeleton was built mid-scroll");
        QTRY_COMPARE(image->property("status").toInt(), 1);
        d.host->setProperty("userScrollActive", false);
        QTest::qWait(20);
        QVERIFY(!d.root->findChild<QObject *>(QStringLiteral("imageSkeleton")));
        QCOMPARE(d.warnings, QStringList{});
    }

    void hiddenMediaPlaceholdersAreBuiltOnlyWhileHidden()
    {
        AppController controller(AppController::MockBackend);
        const auto placeholders = [](QQuickItem *root) {
            int n = 0;
            for (QObject *o : census::objectsUnder(root))
                n += census::className(o) == QLatin1String("MediaHiddenPlaceholder") ? 1 : 0;
            return n;
        };
        for (const QString &kind : { QStringLiteral("image-png"), QStringLiteral("sticker"),
                                     QStringLiteral("gallery-3") }) {
            QVariantMap fixture;
            for (const auto &entry : surveyFixtures(controller))
                if (entry.first == kind)
                    fixture = entry.second;
            QVERIFY(!fixture.isEmpty());
            Delegate d;
            QVERIFY(createDelegate(controller, fixture, d));
            QCOMPARE(placeholders(d.root), 0);
            QVERIFY(d.root->setProperty("mediaHidden", true));
            QTRY_COMPARE(placeholders(d.root), 1);
            QTRY_VERIFY(d.root->findChild<QQuickItem *>(QStringLiteral("mediaHiddenPlaceholder")));
        }
    }

    void aVideoBuildsItsRetryOnlyAfterAFailure()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap f;
        for (const auto &entry : surveyFixtures(controller))
            if (entry.first == QLatin1String("video"))
                f = entry.second;
        Delegate d;
        QVERIFY(createDelegate(controller, f, d));
        auto *video = d.root->findChild<QQuickItem *>(QStringLiteral("videoMedia"));
        QVERIFY(video);
        QVERIFY(!video->findChild<QObject *>(QStringLiteral("videoThumbRetryButton")));
        QVERIFY(!video->findChild<QObject *>(QStringLiteral("videoPosterSkeleton")));

        d.host->setProperty("userScrollActive", true);
        QVERIFY(video->setProperty("bridgeFailed", true));
        QVERIFY2(!video->findChild<QObject *>(QStringLiteral("videoThumbRetryButton")),
                 "the retry was built mid-scroll");
        d.host->setProperty("userScrollActive", false);
        QTRY_VERIFY(video->findChild<QObject *>(QStringLiteral("videoThumbRetryButton")));
        auto *retry = video->findChild<QQuickItem *>(QStringLiteral("videoThumbRetryButton"));
        QVERIFY(retry->isVisible());
        QCOMPARE(d.warnings, QStringList{});
    }

    void aFileCardBuildsNoSaveProgressUntilSaving()
    {
        AppController controller(AppController::MockBackend);
        QVariantMap f;
        for (const auto &entry : surveyFixtures(controller))
            if (entry.first == QLatin1String("file"))
                f = entry.second;
        Delegate d;
        QVERIFY(createDelegate(controller, f, d));
        auto *card = d.root->findChild<QQuickItem *>(QStringLiteral("fileCard"));
        QVERIFY(card);
        int busy = 0;
        for (QObject *o : census::objectsUnder(card))
            busy += census::className(o) == QLatin1String("AppBusyIndicator") ? 1 : 0;
        QCOMPARE(busy, 0);
        QVERIFY(!card->findChild<QObject *>(QStringLiteral("fileSaveCancelButton")));
        QCOMPARE(d.warnings, QStringList{});
    }

    void convertedSubtreesStayWithinTheirCensus()
    {
        AppController controller(AppController::MockBackend);
        struct Bound { const char *kind; const char *subtree; int max; };
        const Bound bounds[] = {
            { "image-png", "messagePresentationRow", 135 },
            { "image-png", "imageMedia", 28 },
            { "video", "videoMedia", 40 },
            { "file", "fileCard", 30 },
            { "sticker", "stickerMedia", 33 },
            { "gallery-3", "messageGallery", 38 },
        };
        const auto fixtures = surveyFixtures(controller);
        int checked = 0;
        for (const Bound &b : bounds) {
            for (const auto &entry : fixtures) {
                if (entry.first != QLatin1String(b.kind))
                    continue;
                Delegate d;
                QVERIFY(createDelegate(controller, entry.second, d));
                QTest::qWait(150);
                QObject *subtree = d.root->findChild<QObject *>(QLatin1String(b.subtree));
                QVERIFY2(subtree, b.subtree);
                const auto objects = census::objectsUnder(subtree);
                QVERIFY2(objects.size() <= b.max,
                         qPrintable(QStringLiteral("%1 %2: %3")
                                        .arg(QLatin1String(b.kind), QLatin1String(b.subtree),
                                             census::describe(objects, 8))));
                ++checked;
            }
        }
        QCOMPARE(checked, int(std::size(bounds)));
    }
};

QTEST_MAIN(RowObjectCensusQmlTest)
#include "RowObjectCensusQmlTest.moc"
