#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>

#include "app/AppController.h"
#include "app/DownloadsController.h"
#include "app/OpenRoomPermissions.h"
#include "app/RoomInfoController.h"
#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/MessageHtml.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace {
constexpr int kSignalTimeoutMs = 5000;
constexpr qreal kRowWidth = 640;
}

class MessageRailCollisionTest : public QObject
{
    Q_OBJECT

    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
    };

    static QVariantMap baseFixture()
    {
        QVariantMap f;
        f.insert(QStringLiteral("isVirtual"), false);
        f.insert(QStringLiteral("isStateActivity"), false);
        f.insert(QStringLiteral("isCallEvent"), false);
        f.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        f.insert(QStringLiteral("showSenderIdentity"), true);
        f.insert(QStringLiteral("eventId"), QStringLiteral("$rail"));
        f.insert(QStringLiteral("itemId"), QStringLiteral("rail-item"));
        f.insert(QStringLiteral("sender"), QStringLiteral("@a:mock.local"));
        f.insert(QStringLiteral("senderDisplayName"),
                 QStringLiteral("A Sender With A Fairly Long Name"));
        f.insert(QStringLiteral("senderInitials"), QStringLiteral("AS"));
        f.insert(QStringLiteral("body"), QStringLiteral("ok"));
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
        f.insert(QStringLiteral("mediaSourceAvailable"), false);
        f.insert(QStringLiteral("mediaMimetype"), QString{});
        f.insert(QStringLiteral("reactions"), QVariantList{});
        f.insert(QStringLiteral("replyToEventId"), QString{});
        f.insert(QStringLiteral("isThreadRoot"), false);
        f.insert(QStringLiteral("mentionsMe"), false);
        f.insert(QStringLiteral("mentionsRoom"), false);
        f.insert(QStringLiteral("roomMentionHighlighted"), false);
        f.insert(QStringLiteral("pushHighlighted"), false);
        f.insert(QStringLiteral("pushVerdictKnown"), false);
        f.insert(QStringLiteral("isLocalEcho"), false);
        f.insert(QStringLiteral("readReceipts"), QVariantList{});
        f.insert(QStringLiteral("readReceiptsTotal"), 0);
        f.insert(QStringLiteral("sentReceipt"), false);
        return f;
    }

    static QVariantList receipts(int count)
    {
        QVariantList out;
        for (int i = 0; i < count; ++i) {
            QVariantMap r;
            r.insert(QStringLiteral("userId"),
                     QStringLiteral("@r%1:mock.local").arg(i));
            r.insert(QStringLiteral("displayName"),
                     QStringLiteral("Reader %1").arg(i));
            r.insert(QStringLiteral("initials"), QStringLiteral("R%1").arg(i));
            r.insert(QStringLiteral("avatarUrl"), QString{});
            out.append(r);
        }
        return out;
    }

    static QVariantList reactions(int count)
    {
        static const char *keys[] = {"\xf0\x9f\x91\x8d", "\xf0\x9f\x8e\x89",
                                     "\xf0\x9f\x94\xa5"};
        QVariantList out;
        for (int i = 0; i < count; ++i) {
            QVariantMap r;
            r.insert(QStringLiteral("key"),
                     QString::fromUtf8(keys[i % 3]));
            r.insert(QStringLiteral("count"), i + 1);
            r.insert(QStringLiteral("byMe"), i == 0);
            r.insert(QStringLiteral("senders"),
                     QVariantList{QStringLiteral("@r:mock.local")});
            r.insert(QStringLiteral("names"),
                     QVariantList{QStringLiteral("Reader")});
            out.append(r);
        }
        return out;
    }

    bool build(AppController &controller, const QVariantMap &fixture,
               Delegate &out, qreal rowWidth = kRowWidth,
               bool direct = false)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        out.engine->rootContext()->setContextProperty("app", &controller);
        out.engine->rootContext()->setContextProperty("model", fixture);
        QSignalSpy created(out.engine.get(),
                           &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("MessageDelegate"));
        if (created.isEmpty() && !created.wait(kSignalTimeoutMs))
            return false;
        out.root = qobject_cast<QQuickItem *>(
            created.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(800, 480);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(rowWidth);
        if (direct)
            QQmlProperty::write(out.root, QStringLiteral("isDirectRoom"), true);
        out.window->show();
        for (int i = 0; i < 6; ++i)
            QCoreApplication::processEvents();
        return true;
    }

    static QQuickItem *forceActionBar(QQuickItem *root)
    {
        auto *loader = root->findChild<QQuickItem *>(
            QStringLiteral("messageActionBarLoader"));
        if (!loader)
            return nullptr;
        QQmlProperty::write(loader, QStringLiteral("active"), true);
        QQmlProperty::write(loader, QStringLiteral("visible"), true);
        QCoreApplication::processEvents();
        return loader;
    }

    static qreal rightEdgeIn(QQuickItem *item, QQuickItem *reference)
    {
        return item->mapToItem(reference, QPointF(item->width(), 0)).x();
    }
    static qreal leftEdgeIn(QQuickItem *item, QQuickItem *reference)
    {
        return item->mapToItem(reference, QPointF(0, 0)).x();
    }

    static QString arabic()
    {
        return QStringLiteral("\u0645\u0631\u062D\u0628\u0627 "
                              "\u0628\u0627\u0644\u0639\u0627\u0644\u0645");
    }

    static QTextDocument *documentOf(QQuickItem *textEdit)
    {
        auto *quick =
            textEdit->property("textDocument").value<QQuickTextDocument *>();
        return quick ? quick->textDocument() : nullptr;
    }

    static qreal paragraphStartIn(QQuickItem *textEdit, const QTextBlock &block,
                                  QQuickItem *reference)
    {
        QRectF r;
        QMetaObject::invokeMethod(textEdit, "positionToRectangle",
                                  Q_RETURN_ARG(QRectF, r),
                                  Q_ARG(int, block.position()));
        return textEdit->mapToItem(reference, r.topLeft()).x();
    }

    static double relativeLuminance(const QColor &c)
    {
        auto lin = [](double v) {
            return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF())
            + 0.0722 * lin(c.blueF());
    }
    static double contrastRatio(const QColor &a, const QColor &b)
    {
        const double la = relativeLuminance(a);
        const double lb = relativeLuminance(b);
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    }
    static QColor over(const QColor &ink, double alpha, const QColor &ground)
    {
        return QColor::fromRgbF(
            float(alpha * ink.redF() + (1.0 - alpha) * ground.redF()),
            float(alpha * ink.greenF() + (1.0 - alpha) * ground.greenF()),
            float(alpha * ink.blueF() + (1.0 - alpha) * ground.blueF()));
    }
    static double opacityUpTo(QQuickItem *item, QQuickItem *ground)
    {
        double o = 1.0;
        for (QQuickItem *it = item; it && it != ground; it = it->parentItem())
            o *= it->opacity();
        return o;
    }
    static QQuickItem *findVisual(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        for (QQuickItem *child : parent->childItems()) {
            if (child->objectName() == name)
                return child;
            if (QQuickItem *hit = findVisual(child, name))
                return hit;
        }
        return nullptr;
    }

private Q_SLOTS:
    void directMentionWithoutVerdictKeepsItsEdgeBar()
    {
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("mentionsMe"), true);
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *bubble = findVisual(d.root, QStringLiteral("messageContentColumn"));
        QVERIFY(bubble);
        QVERIFY(bubble->property("mentionBarVisible").toBool());

        f.insert(QStringLiteral("pushVerdictKnown"), true);
        Delegate quiet;
        QVERIFY(build(app, f, quiet));
        auto *quietBubble = findVisual(quiet.root, QStringLiteral("messageContentColumn"));
        QVERIFY(quietBubble);
        QVERIFY(!quietBubble->property("mentionBarVisible").toBool());

        f.insert(QStringLiteral("pushHighlighted"), true);
        app.settings()->setMessageLayout(1);
        Delegate bubbles;
        QVERIFY(build(app, f, bubbles, kRowWidth, true));
        auto *bubbleLayout = findVisual(bubbles.root, QStringLiteral("messageContentColumn"));
        QVERIFY(bubbleLayout);
        QVERIFY(!bubbleLayout->property("mentionBarVisible").toBool());
    }

    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("message-rail-collision-test"));
    }

    void downloadNoticeFollowsItsFileCard_data()
    {
        QTest::addColumn<int>("layout");
        QTest::addColumn<bool>("own");
        QTest::addColumn<int>("width");
        QTest::addColumn<QString>("caption");
        for (int layout : {0, 1, 2})
            for (bool own : {false, true})
                for (int width : {1000, 520})
                    for (const QString &caption : {QStringLiteral("tall"),
                              QStringLiteral("wide"), QStringLiteral("none")})
                        QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4")
                                           .arg(layout).arg(own).arg(width).arg(caption)))
                            << layout << own << width << caption;
        for (int layout : {0, 2}) {
            QTest::newRow(qPrintable(QStringLiteral("%1-wrapping-reactions").arg(layout)))
                << layout << false << 1000 << QStringLiteral("reactions");
            QTest::newRow(qPrintable(QStringLiteral("%1-thread-and-edited").arg(layout)))
                << layout << false << 1000 << QStringLiteral("thread-edited");
            QTest::newRow(qPrintable(QStringLiteral("%1-thread-and-edited-short").arg(layout)))
                << layout << false << 1000 << QStringLiteral("thread-edited-short");
        }
    }

    void downloadNoticeFollowsItsFileCard()
    {
        QFETCH(int, layout);
        QFETCH(bool, own);
        QFETCH(int, width);
        QFETCH(QString, caption);
        QTemporaryDir downloads;
        QVERIFY(downloads.isValid());
        AppController app(AppController::MockBackend);
        app.settings()->setTheme(SettingsManager::DarkTheme);
        app.settings()->setMessageLayout(layout);
        app.settings()->setAlwaysAskWhereToSave(false);
        app.settings()->setDownloadFolder(downloads.path());
        auto f = baseFixture();
        f[QStringLiteral("timestamp")] = QDateTime::fromString(
            QStringLiteral("2026-10-17T12:00:00Z"), Qt::ISODate);
        f[QStringLiteral("isOwn")] = own;
        f[QStringLiteral("showSenderIdentity")] = false;
        f[QStringLiteral("isFile")] = true;
        f[QStringLiteral("mediaFilename")] = QStringLiteral("original-name.pdf");
        const bool threadEdited = caption.startsWith(QLatin1String("thread-edited"));
        f[QStringLiteral("body")] = caption == QLatin1String("thread-edited-short")
            ? QStringLiteral("Caption line\n").repeated(2)
            : (caption == QLatin1String("tall")
                                   || caption == QLatin1String("thread-edited"))
            ? QStringLiteral("Caption line\n").repeated(12)
            : caption == QLatin1String("wide") ? QString(2000, QLatin1Char('W'))
                                              : QStringLiteral("original-name.pdf");
        if (caption == QLatin1String("reactions"))
            f[QStringLiteral("reactions")] = reactions(120);
        if (threadEdited) {
            f[QStringLiteral("edited")] = true;
            f[QStringLiteral("isThreadRoot")] = true;
            f[QStringLiteral("threadReplyCount")] = 3;
            f[QStringLiteral("threadLatestPreview")] = QStringLiteral("A thread reply");
        }
        f[QStringLiteral("readReceipts")] = receipts(5);
        f[QStringLiteral("readReceiptsTotal")] = 5;
        Delegate reference;
        QVERIFY(build(app, f, reference, width, layout == 1));
        f[QStringLiteral("mediaKey")] = QStringLiteral("rail-file");
        Delegate d;
        QVERIFY(build(app, f, d, width, layout == 1));
        for (auto *root : {reference.root, d.root}) {
            QQmlExpression pins(qmlContext(root), root, QStringLiteral(
                "AppTheme.mode = 2; AppTheme.textScale = 1"));
            pins.evaluate();
            QVERIFY2(!pins.hasError(), qPrintable(pins.error().toString()));
        }
        QQmlComponent scene(d.engine.get());
        scene.setData("import QtQuick\nFlickable { width: 1100; height: 650; "
                      "contentWidth: width; contentHeight: 3000; clip: true }", QUrl());
        std::unique_ptr<QObject> owner(scene.create());
        auto *view = qobject_cast<QQuickItem *>(owner.get());
        QVERIFY(view);
        view->setParentItem(d.window->contentItem());
        auto *content = view->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        d.root->setParentItem(content);
        d.root->setY(120);
        QQmlProperty::write(d.root, QStringLiteral("timelineView"),
                            QVariant::fromValue(view));
        QQmlComponent fallback(d.engine.get());
        fallback.setData("import QtQuick\nimport MatrixClient\n"
                         "PaneDownloadsCard { allItems: app.downloads.items }", QUrl());
        std::unique_ptr<QObject> paneOwner(fallback.create());
        auto *pane = qobject_cast<QQuickItem *>(paneOwner.get());
        QVERIFY(pane);
        pane->setParentItem(d.window->contentItem());
        const auto paneCount = [&] {
            QQmlExpression count(qmlContext(pane), pane, QStringLiteral("items.length"));
            return count.evaluate().toInt();
        };
        auto *card = findVisual(d.root, QStringLiteral("fileCard"));
        auto *referenceCard = findVisual(reference.root, QStringLiteral("fileCard"));
        const QString noticeName = QStringLiteral("attachmentDownloadNotice");
        QVERIFY(!findVisual(d.root, noticeName));
        QVERIFY(!findVisual(reference.root, noticeName));
        auto *body = findVisual(d.root, QStringLiteral("messageBody"));
        auto *referenceBody = findVisual(reference.root, QStringLiteral("messageBody"));
        auto *bar = forceActionBar(d.root);
        auto *pile = findVisual(d.root, QStringLiteral("readReceiptRow"));
        QVERIFY(card && referenceCard && body && referenceBody && bar && pile);
        auto rect = [](QQuickItem *item, QQuickItem *in) {
            return item->mapRectToItem(in, item->boundingRect());
        };
        QCOMPARE(d.root->height(), reference.root->height());
        QCOMPARE(body->width(), referenceBody->width());
        QCOMPARE(rect(body, d.root), rect(referenceBody, reference.root));
        QCOMPARE(rect(card, d.root), rect(referenceCard, reference.root));
        const qreal heightBefore = d.root->height();
        const qreal implicitBefore = d.root->implicitHeight();
        const qreal widthBefore = d.root->width();
        const QPointF positionBefore = d.root->position();
        const qreal captionBefore = body->width();
        const QRectF captionRectBefore = rect(body, d.root);
        const QRectF cardBefore = rect(card, d.root);
        QVERIFY(heightBefore > 0);
        app.downloads()->download(QStringLiteral("rail-file"),
                                  QStringLiteral("original-name.pdf"), {});
        QVERIFY(!app.downloads()->items().isEmpty());
        QPointer<QQuickItem> notice;
        QTRY_VERIFY((notice = findVisual(d.root, noticeName)) != nullptr);
        QVERIFY(!findVisual(reference.root, noticeName));
        const bool fits = layout == 1 && own
            ? cardBefore.left() - notice->property("margin").toReal()
                - notice->property("gap").toReal()
                >= notice->property("minWidth").toReal()
            : width == 1000 || (layout == 2 && caption == QLatin1String("tall"));
        QTRY_COMPARE(notice->isVisible(), fits);
        QTRY_COMPARE(paneCount(), fits ? 0 : 1);
        QCOMPARE(int(notice->isVisible()) + paneCount(), 1);
        QCOMPARE(d.root->height(), heightBefore);
        QCOMPARE(d.root->implicitHeight(), implicitBefore);
        QCOMPARE(d.root->width(), widthBefore);
        QCOMPARE(d.root->position(), positionBefore);
        QCOMPARE(body->width(), captionBefore);
        QCOMPARE(rect(body, d.root), captionRectBefore);
        QCOMPARE(rect(card, d.root), cardBefore);
        QList<QQuickItem *> occupied;
        auto *flow = findVisual(d.root, QStringLiteral("reactionsFlow"));
        QVERIFY(flow);
        for (auto *chip : flow->childItems()) {
            if (chip->isVisible() && (chip->objectName() == QLatin1String("reactionChip")
                                    || chip->objectName() == QLatin1String("reactionAddChip")))
                occupied.append(chip);
        }
        if (caption == QLatin1String("reactions")) {
            QVERIFY(!body->isVisible());
            QCOMPARE(occupied.size(), 121);
            QVERIFY(occupied.last()->y() > occupied.first()->y() + 2 * occupied.first()->height());
            QVERIFY(notice->isVisible());
            QVERIFY(std::none_of(occupied.cbegin(), occupied.cend(), [&](QQuickItem *chip) {
                return rect(chip, d.root).intersects(rect(notice, d.root));
            }));
        }
        if (threadEdited) {
            auto *summary = findVisual(d.root, QStringLiteral("threadSummaryCard"));
            auto *edited = findVisual(d.root, QStringLiteral("messageStatusText"));
            QVERIFY(summary && summary->isVisible());
            QVERIFY(edited && edited->isVisible());
            occupied << summary << edited;
            if (caption == QLatin1String("thread-edited-short")) {
                const QRectF candidate = rect(notice, d.root);
                const QRectF meta = rect(summary, d.root).united(rect(edited, d.root));
                QVERIFY(meta.right() < candidate.left());
            }
        }
        for (auto *item : occupied)
            QVERIFY(!notice->isVisible() || !rect(item, d.root).intersects(rect(notice, d.root)));
        if (fits) {
            const QRectF band = rect(notice, d.root);
            QVERIFY(d.root->boundingRect().contains(band));
            if (layout == 1 && own)
                QVERIFY(band.right() < cardBefore.left());
            else
                QVERIFY(band.left() > cardBefore.right());
            QVERIFY(band.top() >= cardBefore.top());
            QVERIFY(band.top() < cardBefore.bottom() && band.bottom() > cardBefore.top());
            QVERIFY(!rect(bar, d.root).intersects(band));
            QVERIFY(!rect(pile, d.root).intersects(band));
            QVERIFY(!rect(body, d.root).intersects(band));
            const QRectF beforeScroll = rect(notice, view);
            view->setProperty("contentY", 70);
            QCoreApplication::processEvents();
            QCOMPARE(rect(notice, view).top(), beforeScroll.top() - 70);
            QCOMPARE(rect(notice, view).left(), beforeScroll.left());
        }
        view->setProperty("contentY", 2500);
        QVERIFY(notice);
        QTRY_VERIFY(!notice->isVisible());
        QTRY_COMPARE(paneCount(), 1);
        QCOMPARE(d.root->height(), heightBefore);
        view->setProperty("contentY", 0);
        QVERIFY(notice);
        QTRY_COMPARE(notice->isVisible(), fits);
        QTRY_COMPARE(paneCount(), fits ? 0 : 1);
        const int id = app.downloads()->items().value(0).toMap()
                           .value(QStringLiteral("id")).toInt();
        app.downloads()->dismiss(id);
        QTRY_VERIFY(notice.isNull());
        QVERIFY(!findVisual(d.root, noticeName));
        QTRY_COMPARE(paneCount(), 0);
        QCOMPARE(d.root->height(), heightBefore);
        QCOMPARE(d.root->implicitHeight(), implicitBefore);
        QCOMPARE(d.root->width(), widthBefore);
        QCOMPARE(d.root->position(), positionBefore);
        QCOMPARE(body->width(), captionBefore);
        QCOMPARE(rect(body, d.root), captionRectBefore);
        QCOMPARE(rect(card, d.root), cardBefore);
        d.root->setParentItem(d.window->contentItem());
    }

    void messageSpacingMovesTheGapInEveryLayout_data()
    {
        QTest::addColumn<int>("layout");
        QTest::newRow("modern") << 0;
        QTest::newRow("bubbles") << 1;
        QTest::newRow("compact") << 2;
    }
    void messageSpacingMovesTheGapInEveryLayout()
    {
        QFETCH(int, layout);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(layout);

        const auto heightAt = [&](bool leader, int spacing) -> qreal {
            QVariantMap f = baseFixture();
            f.insert(QStringLiteral("showSenderIdentity"), leader);
            Delegate d;
            if (!build(app, f, d, kRowWidth, layout == 1))
                return -1.0;
            QQmlComponent comp(d.engine.get());
            comp.setData(QByteArray(
                             "import QtQuick\nimport MatrixClient\n"
                             "QtObject { Component.onCompleted: "
                             "AppTheme.messageSpacing = ")
                             + QByteArray::number(spacing) + " }",
                         QUrl());
            std::unique_ptr<QObject> setter(comp.create());
            if (!setter)
                return -2.0;
            for (int i = 0; i < 6; ++i)
                QCoreApplication::processEvents();
            return QQmlProperty::read(d.root, QStringLiteral("implicitHeight"))
                .toReal();
        };

        for (const bool leader : {true, false}) {
            const qreal compact = heightAt(leader, 0);
            const qreal cozy = heightAt(leader, 1);
            const qreal roomy = heightAt(leader, 2);
            const QString what = QStringLiteral("layout %1, %2")
                .arg(layout).arg(leader ? "sender's first message" : "continuation");
            QVERIFY2(compact > 0 && cozy > 0 && roomy > 0, qPrintable(what));
            const qreal base = leader ? (layout == 2 ? 2 : 12)
                                      : (layout == 2 ? 0 : 1);
            const qreal compactExtra = leader ? -4 : -1;
            const qreal roomyExtra = leader ? 6 : 2;
            QCOMPARE(compact - cozy, std::max(0.0, base + compactExtra) - base);
            QCOMPARE(roomy - cozy, base + roomyExtra - base);
            QVERIFY2(compact <= cozy && cozy < roomy, qPrintable(what));
        }
    }

    void aPendingLocalEchoOffersNoActionBar()
    {
        AppController app(AppController::MockBackend);
        const auto hostFor = [](QQmlEngine *engine, const QString &key) {
            QQmlComponent comp(engine);
            comp.setData(QByteArray(
                             "import QtQuick\nQtObject {"
                             " property string pinnedActionsKey: \"\";"
                             " property string hoveredActionsKey: \"")
                             + key.toUtf8() + "\" }",
                         QUrl());
            return std::unique_ptr<QObject>(comp.create());
        };

        {
            Delegate d;
            QVERIFY(build(app, baseFixture(), d));
            auto view = hostFor(d.engine.get(), QStringLiteral("rail-item"));
            QVERIFY(view);
            QQmlProperty::write(d.root, QStringLiteral("timelineView"),
                                QVariant::fromValue(view.get()));
            QCoreApplication::processEvents();
            QVERIFY2(QQmlProperty::read(d.root, QStringLiteral("actionsVisible"))
                         .toBool(),
                     "a hovered, sent row shows no bar: the fixture cannot "
                     "hover, and the echo case below proves nothing");
            QQmlProperty::write(d.root, QStringLiteral("timelineView"),
                                QVariant());
        }

        for (const QString &pendingId :
             { QString(), QStringLiteral("local:txn-1") }) {
            QVariantMap f = baseFixture();
            f.insert(QStringLiteral("isOwn"), true);
            f.insert(QStringLiteral("isLocalEcho"), true);
            f.insert(QStringLiteral("status"), 1);
            f.insert(QStringLiteral("eventId"), pendingId);
            f.insert(QStringLiteral("itemId"), QStringLiteral("echo-item"));
            Delegate d;
            QVERIFY(build(app, f, d));
            auto view = hostFor(d.engine.get(), QStringLiteral("echo-item"));
            QVERIFY(view);
            QQmlProperty::write(d.root, QStringLiteral("timelineView"),
                                QVariant::fromValue(view.get()));
            QCoreApplication::processEvents();
            QVERIFY2(!QQmlProperty::read(d.root, QStringLiteral("actionsVisible"))
                          .toBool(),
                     qPrintable(QStringLiteral(
                         "a pending echo (event id \"%1\") shows the hover "
                         "bar; its Reply and More cannot act on it")
                         .arg(pendingId)));
            QQmlProperty::write(d.root, QStringLiteral("timelineView"),
                                QVariant());
        }
    }

    void aFileCardNamesItsKindInWords_data()
    {
        QTest::addColumn<QString>("mime");
        QTest::addColumn<QString>("label");
        QTest::newRow("text") << "text/plain" << "Text";
        QTest::newRow("text+charset") << "text/plain; charset=utf-8" << "Text";
        QTest::newRow("tar.gz") << "application/x-compressed-tar" << "Archive";
        QTest::newRow("zip") << "application/zip" << "Archive";
        QTest::newRow("7z") << "application/x-7z-compressed" << "Archive";
        QTest::newRow("gzip") << "application/gzip" << "Archive";
        QTest::newRow("pdf") << "application/pdf" << "PDF";
        QTest::newRow("svg") << "image/svg+xml" << "SVG";
        QTest::newRow("binary") << "application/octet-stream" << "File";
        QTest::newRow("epub") << "application/epub+zip" << "EPUB";
        QTest::newRow("docx")
            << "application/vnd.openxmlformats-officedocument."
               "wordprocessingml.document"
            << "File";
    }
    void aFileCardNamesItsKindInWords()
    {
        QFETCH(QString, mime);
        QFETCH(QString, label);
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isFile"), true);
        f.insert(QStringLiteral("mediaSize"), 3000);
        f.insert(QStringLiteral("mediaMimetype"), mime);
        f.insert(QStringLiteral("mediaKey"), QStringLiteral("k73"));
        f.insert(QStringLiteral("mediaFilename"), QStringLiteral("a.tar.gz"));
        f.insert(QStringLiteral("body"), QStringLiteral("a.tar.gz"));
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *card = d.root->findChild<QQuickItem *>(QStringLiteral("fileCard"));
        QVERIFY2(card, "no file card in the row");
        QVariant out;
        QVERIFY(QMetaObject::invokeMethod(card, "fileTypeLabel",
                                          Q_RETURN_ARG(QVariant, out),
                                          Q_ARG(QVariant, mime)));
        QCOMPARE(out.toString(), label);
        auto *subtitle = findVisual(card, QStringLiteral("fileCardSubtitle"));
        QVERIFY2(subtitle, "no fileCardSubtitle on the card");
        QVERIFY2(subtitle->property("text").toString().endsWith(label),
                 qPrintable(subtitle->property("text").toString()));
        QCOMPARE(subtitle->property("textFormat").toInt(), 0);
    }

    void theLastLineClearsTheReceiptPileOnANarrowRow()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(0);
        const QString sentence = QStringLiteral(
            "second encrypted message about quokka and the other quokka ");
        int checked = 0;
        for (int chars = 30; chars <= 120; chars += 2) {
            QVariantMap f = baseFixture();
            f.insert(QStringLiteral("showSenderIdentity"), false);
            f.insert(QStringLiteral("body"),
                     sentence.repeated(3).left(chars).trimmed());
            f.insert(QStringLiteral("readReceipts"), receipts(1));
            f.insert(QStringLiteral("readReceiptsTotal"), 1);
            Delegate d;
            QVERIFY(build(app, f, d, 360));
            auto *body = findVisual(d.root, QStringLiteral("messageBody"));
            auto *pile = d.root->findChild<QQuickItem *>(
                QStringLiteral("readReceiptRow"));
            QVERIFY2(body && body->width() > 0, "no message body in the row");
            QVERIFY2(pile && pile->width() > 0, "no receipt facepile in the row");
            const qreal bodyRight = rightEdgeIn(body, d.root);
            const qreal pileLeft = leftEdgeIn(pile, d.root);
            QVERIFY2(bodyRight <= pileLeft + 0.5,
                     qPrintable(QStringLiteral(
                         "a %1-character body ends at %2 and the receipt "
                         "facepile starts at %3: the avatar is drawn over "
                         "the last line (reserve %4)")
                         .arg(chars).arg(bodyRight).arg(pileLeft)
                         .arg(d.root->property("receiptRailReserve").toReal())));
            ++checked;
        }
        QCOMPARE(checked, 46);
    }

    void aLongSenderNameElidesInsideTheHeaderCap_data()
    {
        QTest::addColumn<bool>("own");
        QTest::newRow("own") << true;
        QTest::newRow("other") << false;
    }
    void aLongSenderNameElidesInsideTheHeaderCap()
    {
        QFETCH(bool, own);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(0);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), own);
        f.insert(QStringLiteral("senderDisplayName"),
                 QStringLiteral("Me With A Fairly Long Display Name That Goes "
                                "On And On And On"));
        Delegate d;
        QVERIFY(build(app, f, d, 360));
        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *header = findVisual(d.root, QStringLiteral("senderIdentityHeader"));
        auto *name = findVisual(d.root, QStringLiteral("senderName"));
        auto *stamp = findVisual(d.root, QStringLiteral("senderTimestamp"));
        QVERIFY(bubble);
        QVERIFY2(header && name && stamp, "no sender header in the row");
        QVERIFY2(name->width() > 0, "the sender name measured empty");

        const qreal headerRight = rightEdgeIn(header, d.root);
        const qreal nameRight = rightEdgeIn(name, d.root);
        const qreal stampLeft = leftEdgeIn(stamp, d.root);
        const qreal stampRight = rightEdgeIn(stamp, d.root);
        QVERIFY2(name->property("truncated").toBool(),
                 "the long name was not elided");
        QVERIFY2(nameRight <= stampLeft + 0.5,
                 qPrintable(QStringLiteral(
                     "the name ends at %1, over the timestamp at %2")
                     .arg(nameRight).arg(stampLeft)));
        QVERIFY2(stampRight <= headerRight + 0.5,
                 qPrintable(QStringLiteral(
                     "the timestamp ends at %1, past the header cap at %2: "
                     "the name pushed it out")
                     .arg(stampRight).arg(headerRight)));
        QVERIFY2(headerRight <= rightEdgeIn(bubble, d.root) + 0.5,
                 "the header runs past its content column");
    }

    void aFailedStickerKeepsItsErrorInsideItsTile_data()
    {
        QTest::addColumn<int>("edge");
        QTest::newRow("64px") << 64;
        QTest::newRow("32px") << 32;
        QTest::newRow("180px") << 180;
    }
    void aFailedStickerKeepsItsErrorInsideItsTile()
    {
        QFETCH(int, edge);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(0);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isSticker"), true);
        f.insert(QStringLiteral("mediaWidth"), edge);
        f.insert(QStringLiteral("mediaHeight"), edge);
        f.insert(QStringLiteral("body"), QStringLiteral("a sticker"));
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *tile = findVisual(d.root, QStringLiteral("stickerMedia"));
        QVERIFY2(tile, "no sticker tile in the row");
        QVERIFY(QQmlProperty::write(tile, QStringLiteral("bridgeFailed"), true));
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();
        QCOMPARE(tile->width(), qreal(edge));
        QCOMPARE(tile->height(), qreal(edge));

        int drawn = 0;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            for (QQuickItem *c : it->childItems()) {
                if (!c->isVisible())
                    continue;
                if (c->inherits("QQuickText")
                    && !c->property("text").toString().isEmpty()) {
                    ++drawn;
                    const QRectF r = c->mapRectToItem(
                        tile, QRectF(0, 0, c->width(), c->height()));
                    QVERIFY2(r.top() >= -0.5 && r.bottom() <= edge + 0.5
                                 && r.left() >= -0.5 && r.right() <= edge + 0.5,
                             qPrintable(QStringLiteral(
                                 "\"%1\" is drawn at %2,%3 %4x%5 in a %6px "
                                 "tile: it spills over the neighbouring rows")
                                 .arg(c->property("text").toString())
                                 .arg(r.x()).arg(r.y()).arg(r.width())
                                 .arg(r.height()).arg(edge)));
                }
                walk(c);
            }
        };
        walk(tile);
        if (QTest::currentTestFailed())
            return;
        QVERIFY2(drawn >= 1, "the failed tile draws nothing at all");
        auto *error = findVisual(tile, QStringLiteral("stickerError"));
        QVERIFY2(error && error->isVisible(),
                 "the failed tile shows no error state");
        QVERIFY(QQmlProperty::read(tile, QStringLiteral("loadFailed")).toBool());
    }

    void moderatorDeleteIsOfferedWithNoRoomPanelOpen()
    {
        AppController app(AppController::MockBackend);
        auto *mock = app.findChild<MockMatrixClient *>();
        QVERIFY(mock);
        auto *facts = app.openRoomPermissions();
        QVERIFY(facts);
        const QString room = QStringLiteral("!general:mock.local");
        const QString other = QStringLiteral("!devs:mock.local");
        QVERIFY(mock->roomInfo(other).id == other);
        QVariantMap grant;
        grant.insert(QStringLiteral("ownMembership"), QStringLiteral("joined"));
        grant.insert(QStringLiteral("canSendMessages"), true);
        grant.insert(QStringLiteral("canRedactOthers"), true);
        mock->setOwnPermissionsForTest(room, grant);
        app.setCurrentRoomId(room);
        QTRY_VERIFY_WITH_TIMEOUT(facts->canRedactOthers(), kSignalTimeoutMs);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), false);
        f.insert(QStringLiteral("sender"), QStringLiteral("@peer:mock.local"));
        f.insert(QStringLiteral("eventId"), QStringLiteral("$peer-message"));
        f.insert(QStringLiteral("body"), QStringLiteral("hello"));
        Delegate d;
        QVERIFY(build(app, f, d));
        QVERIFY2(app.roomInfo()->roomId() != room,
                 "the fixture opened the info panel; this case is about the "
                 "path where nobody has");
        QVERIFY2(QQmlProperty::read(d.root, QStringLiteral("canRedactOthers"))
                     .toBool(),
                 "a moderator is not offered Delete on another member's "
                 "message until the room's info panel has been opened");

        QVariantMap demoted = grant;
        demoted.insert(QStringLiteral("canRedactOthers"), false);
        mock->setOwnPermissionsForTest(room, demoted);
        QTRY_VERIFY_WITH_TIMEOUT(!facts->canRedactOthers(), kSignalTimeoutMs);
        QVERIFY(!QQmlProperty::read(d.root, QStringLiteral("canRedactOthers"))
                     .toBool());

        mock->setOwnPermissionsForTest(room, grant);
        QTRY_VERIFY_WITH_TIMEOUT(facts->canRedactOthers(), kSignalTimeoutMs);
        app.setCurrentRoomId(other);
        QCOMPARE(facts->roomId(), other);
        QVERIFY2(!facts->canRedactOthers() && !facts->redactKnown(),
                 "the previous room's grant survived the switch");
        QCoreApplication::processEvents();
        QVERIFY(!QQmlProperty::read(d.root, QStringLiteral("canRedactOthers"))
                     .toBool());
    }

    void aFailedEchoSaysWhyItFailed_data()
    {
        QTest::addColumn<QString>("category");
        QTest::addColumn<QString>("phrase");
        QTest::newRow("rejected") << "rejected" << "the server refused it";
        QTest::newRow("network") << "network" << "connection problem";
        QTest::newRow("unknown") << "" << "failed";
    }
    void aFailedEchoSaysWhyItFailed()
    {
        QFETCH(QString, category);
        QFETCH(QString, phrase);
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("isLocalEcho"), true);
        f.insert(QStringLiteral("eventId"), QString());
        f.insert(QStringLiteral("status"), 2);
        f.insert(QStringLiteral("sendErrorCategory"), category);
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *status = findVisual(d.root, QStringLiteral("messageStatusText"));
        QVERIFY2(status, "a failed echo shows no status line");
        const QString text = status->property("text").toString();
        QVERIFY2(text.contains(phrase), qPrintable(text));
    }

    void aWideTableStaysInsideItsColumn_data()
    {
        QTest::addColumn<int>("columns");
        QTest::addColumn<bool>("rendersAsTable");
        QTest::newRow("300 cells in one row, flattened") << 300 << false;
        QTest::newRow("16 wide cells, bounded but wider than the row")
            << 16 << true;
    }
    void aWideTableStaysInsideItsColumn()
    {
        QFETCH(int, columns);
        QFETCH(bool, rendersAsTable);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(0);
        QString raw = QStringLiteral("<table><tr>");
        for (int c = 0; c < columns; ++c)
            raw += QStringLiteral("<td>WWWWWW</td>");
        raw += QStringLiteral("</tr></table>");
        const QString html = MessageHtml::sanitize(
            raw, nullptr, QString(),
            MessageHtml::MentionStyle{QString(), QString(),
                                      QStringLiteral("#203040"), false});
        QCOMPARE(html.contains(QStringLiteral("<table")), rendersAsTable);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("showSenderIdentity"), false);
        f.insert(QStringLiteral("body"), QStringLiteral("| WWWWWW |"));
        f.insert(QStringLiteral("formattedBody"), html);
        f.insert(QStringLiteral("readReceipts"), receipts(1));
        f.insert(QStringLiteral("readReceiptsTotal"), 1);
        Delegate d;
        QVERIFY(build(app, f, d, 360));
        auto *column = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *body = findVisual(d.root, QStringLiteral("messageBody"));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(column);
        QVERIFY2(body && body->width() > 0, "no message body in the row");
        QVERIFY2(pile && pile->width() > 0, "no receipt facepile in the row");
        QTextDocument *doc = documentOf(body);
        QVERIFY(doc);
        const qreal laidOut =
            std::max(doc->size().width(),
                     body->property("contentWidth").toReal());
        if (rendersAsTable) {
            QVERIFY2(laidOut > body->width() + 1,
                     qPrintable(QStringLiteral(
                         "the bounded table is laid out %1 wide in a %2 body; "
                         "make its cells wider")
                         .arg(laidOut).arg(body->width())));
        }
        const qreal drawn =
            body->clip() ? std::min(laidOut, body->width()) : laidOut;
        const qreal drawnRight = leftEdgeIn(body, d.root) + drawn;
        QVERIFY2(drawnRight <= rightEdgeIn(column, d.root) + 0.5,
                 qPrintable(QStringLiteral(
                     "the table is drawn to %1, past the content column's "
                     "right edge %2 (clip %3)")
                     .arg(drawnRight).arg(rightEdgeIn(column, d.root))
                     .arg(body->clip())));
        QVERIFY2(drawnRight <= leftEdgeIn(pile, d.root) + 0.5,
                 qPrintable(QStringLiteral(
                     "the table is drawn to %1, over the receipt facepile "
                     "at %2 (clip %3)")
                     .arg(drawnRight).arg(leftEdgeIn(pile, d.root))
                     .arg(body->clip())));
    }

    void qtBuildsNoMoreTableCellsThanTheBodyBudget_data()
    {
        QTest::addColumn<QString>("html");
        QTest::addColumn<qint64>("cells");
        const QString cells15 = QStringLiteral("<td>x</td>").repeated(15);
        const QString rows30 = QStringLiteral("<tr>").repeated(30);
        const QString full = QStringLiteral("<table><tr>")
            + QStringLiteral("<td>x</td>").repeated(16)
            + QStringLiteral("</tr>") + QStringLiteral("<tr>").repeated(24)
            + QStringLiteral("</table>");
        QTest::newRow("</td> escape")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><table></td></tr>") + rows30
                   + QStringLiteral("</table></table>")
            << qint64(16);
        QTest::newRow("</tr> escape")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><table></tr>") + rows30
                   + QStringLiteral("</table></table>")
            << qint64(16);
        QTest::newRow("</tbody> escape")
            << QStringLiteral("<table><tbody><tr>") + cells15
                   + QStringLiteral("<td><table></tbody>") + rows30
                   + QStringLiteral("</table></table>")
            << qint64(16);
        QTest::newRow("</b> escape")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><b><table></b></td></tr>") + rows30
                   + QStringLiteral("</table></table>")
            << qint64(16);
        QTest::newRow("cell in no row of the inner table")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><table><td>y")
                   + QStringLiteral("<tr>").repeated(383)
                   + QStringLiteral("</table></td></tr></table>")
            << qint64(16);
        QTest::newRow("implicitly closed <p>")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><p><table><tr><td><p>a<p>b</p></p>"
                                    "</td></tr>")
                   + rows30 + QStringLiteral("</table></td></tr></table>")
            << qint64(0);
        QTest::newRow("implicitly closed <li>")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><ul><li><table><tr><td><ul><li><li>a"
                                    "</li></li></td></tr>")
                   + rows30
                   + QStringLiteral("</table></li></ul></td></tr></table>")
            << qint64(0);
        QTest::newRow("</br> escape")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><b><table><tr><td><br><b></br></b>")
                   + QStringLiteral("<tr>").repeated(383)
                   + QStringLiteral("</table></b></td></tr></table>")
            << qint64(400);
        QTest::newRow("</hr> escape")
            << QStringLiteral("<table><tr>") + cells15
                   + QStringLiteral("<td><b><table><tr><td><hr><b></hr></b>")
                   + QStringLiteral("<tr>").repeated(383)
                   + QStringLiteral("</table></b></td></tr></table>")
            << qint64(400);
        QTest::newRow("300 legal 400-cell tables in one body")
            << full.repeated(300) << qint64(400);
        QTest::newRow("control: one legal 400-cell table") << full << qint64(400);
    }
    void qtBuildsNoMoreTableCellsThanTheBodyBudget()
    {
        QFETCH(QString, html);
        QFETCH(qint64, cells);
        QTextDocument doc;
        doc.setHtml(MessageHtml::sanitize(html, nullptr, QString()));
        qint64 built = 0;
        int widest = 0;
        const std::function<void(QTextFrame *)> walk = [&](QTextFrame *frame) {
            for (QTextFrame *child : frame->childFrames()) {
                if (auto *table = qobject_cast<QTextTable *>(child)) {
                    built += qint64(table->rows()) * table->columns();
                    widest = std::max(widest, table->columns());
                }
                walk(child);
            }
        };
        walk(doc.rootFrame());
        QCOMPARE(built, cells);
        QVERIFY2(widest <= 16, qPrintable(QString::number(widest)));
    }

    static QString longParagraph()
    {
        return QStringLiteral(
                   "A deliberately long paragraph that keeps going with "
                   "ordinary words so the line breaks fall wherever the "
                   "layout puts the right edge of the text column. ")
            .repeated(8)
            .trimmed();
    }
    void wideRowTextReachesTheRightEdge_data()
    {
        QTest::addColumn<int>("layout");
        QTest::addColumn<int>("width");
        for (int layout : {0, 2})
            for (int width : {1750, 1240, 1000, 600})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2")
                                             .arg(layout == 0 ? "modern" : "compact")
                                             .arg(width)))
                    << layout << width;
    }
    void wideRowTextReachesTheRightEdge()
    {
        QFETCH(int, layout);
        QFETCH(int, width);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(layout);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("body"), longParagraph());
        f.insert(QStringLiteral("replyToEventId"), QStringLiteral("$quoted"));
        f.insert(QStringLiteral("replyToSender"), QStringLiteral("Quoted Sender"));
        f.insert(QStringLiteral("replyToPreview"), longParagraph());
        Delegate d;
        QVERIFY(build(app, f, d, width));
        d.window->resize(width + 40, 600);
        QCoreApplication::processEvents();
        auto *row = d.root->findChild<QQuickItem *>(
            QStringLiteral("messagePresentationRow"));
        auto *body = findVisual(d.root, QStringLiteral("messageBody"));
        auto *quote = findVisual(d.root, QStringLiteral("replyNavigationTarget"));
        QVERIFY2(row && body && quote, "row, body or reply quote missing");
        QVERIFY(body->isVisible() && quote->isVisible());
        const qreal edge = rightEdgeIn(row, d.root);
        const qreal bodyRight = rightEdgeIn(body, d.root);
        const qreal quoteRight = rightEdgeIn(quote, d.root);
        const qreal painted = body->property("contentWidth").toReal();
        QVERIFY2(bodyRight >= edge - 1.0 && bodyRight <= edge + 0.5,
                 qPrintable(QStringLiteral(
                     "a %1px row: the body ends at %2 and the row at %3, "
                     "leaving %4px of empty band beside the text")
                     .arg(width).arg(bodyRight).arg(edge).arg(edge - bodyRight)));
        QVERIFY2(painted >= body->width() - 90,
                 qPrintable(QStringLiteral("the lines fill %1 of a %2px column")
                                .arg(painted).arg(body->width())));
        QVERIFY2(quoteRight >= edge - 1.0 && quoteRight <= edge + 0.5,
                 qPrintable(QStringLiteral(
                     "a %1px row: the reply quote ends at %2, the row at %3")
                     .arg(width).arg(quoteRight).arg(edge)));
    }

    void wideRowTextStopsAtTheReceiptPile_data()
    {
        QTest::addColumn<int>("layout");
        QTest::newRow("modern") << 0;
        QTest::newRow("compact") << 2;
    }
    void wideRowTextStopsAtTheReceiptPile()
    {
        QFETCH(int, layout);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(layout);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("body"), longParagraph());
        f.insert(QStringLiteral("readReceipts"), receipts(4));
        f.insert(QStringLiteral("readReceiptsTotal"), 4);
        Delegate d;
        QVERIFY(build(app, f, d, 1500));
        d.window->resize(1540, 600);
        QCoreApplication::processEvents();
        auto *body = findVisual(d.root, QStringLiteral("messageBody"));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY2(body && pile && pile->width() > 0, "body or facepile missing");
        const qreal bodyRight = rightEdgeIn(body, d.root);
        const qreal pileLeft = leftEdgeIn(pile, d.root);
        QVERIFY2(bodyRight <= pileLeft + 0.5,
                 qPrintable(QStringLiteral("the body (ends at %1) runs under "
                                           "the facepile (starts at %2)")
                                .arg(bodyRight).arg(pileLeft)));
        QVERIFY2(bodyRight >= pileLeft - 12.0,
                 qPrintable(QStringLiteral(
                     "the body ends at %1, %2px short of the facepile at %3: "
                     "a cap, not the receipt clearance, decides its width")
                     .arg(bodyRight).arg(pileLeft - bodyRight).arg(pileLeft)));
    }

    void theActionBarClearsTheReceiptPileOnAWideShortRow_data()
    {
        QTest::addColumn<int>("layout");
        QTest::newRow("modern") << 0;
        QTest::newRow("compact") << 2;
    }
    void theActionBarClearsTheReceiptPileOnAWideShortRow()
    {
        QFETCH(int, layout);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(layout);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(4));
        f.insert(QStringLiteral("readReceiptsTotal"), 4);
        Delegate d;
        QVERIFY(build(app, f, d, 1500));
        d.window->resize(1540, 600);
        QCoreApplication::processEvents();
        QQuickItem *bar = forceActionBar(d.root);
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        auto *body = findVisual(d.root, QStringLiteral("messageBody"));
        QVERIFY2(bar && pile && body, "bar, facepile or body missing");
        QVERIFY(bar->width() > 0 && pile->width() > 0);
        QVERIFY2(QQmlProperty::read(d.root, QStringLiteral("receiptBandMeetsBar"))
                     .toBool(),
                 "fixture: the row is not short enough for the bands to meet");
        const qreal barRight = rightEdgeIn(bar, d.root);
        const qreal pileLeft = leftEdgeIn(pile, d.root);
        QVERIFY2(barRight <= pileLeft + 1.0,
                 qPrintable(QStringLiteral("the action bar (ends at %1) runs "
                                           "into the facepile (starts at %2)")
                                .arg(barRight).arg(pileLeft)));
        QVERIFY2(rightEdgeIn(body, d.root) <= pileLeft + 0.5,
                 "the body runs under the facepile on a short wide row");
    }

    void aWideBubbleKeepsItsReadableWidth_data()
    {
        QTest::addColumn<bool>("own");
        QTest::newRow("incoming") << false;
        QTest::newRow("own") << true;
    }
    void aWideBubbleKeepsItsReadableWidth()
    {
        QFETCH(bool, own);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), own);
        f.insert(QStringLiteral("body"), longParagraph());
        Delegate d;
        QVERIFY(build(app, f, d, 1500,true));
        d.window->resize(1540, 600);
        QCoreApplication::processEvents();
        QVERIFY(d.root->property("bubbleMode").toBool());
        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *body = findVisual(d.root, QStringLiteral("messageBody"));
        QVERIFY(bubble && body);
        QVERIFY2(body->width() > 600 && body->width() <= 720.5,
                 qPrintable(QStringLiteral("a wide bubble's text is %1px wide")
                                .arg(body->width())));
        QVERIFY2(bubble->width() <= 740.5,
                 qPrintable(QStringLiteral("a wide bubble is %1px wide")
                                .arg(bubble->width())));
    }

    void theActionBarClearsTheReceiptPileOnAShortRow()
    {
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(4));
        f.insert(QStringLiteral("readReceiptsTotal"), 4);

        Delegate d;
        QVERIFY(build(app, f, d));
        QQuickItem *bar = forceActionBar(d.root);
        QVERIFY2(bar, "no messageActionBarLoader in the row");
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY2(pile, "no readReceiptRow in the row");
        QVERIFY2(pile->width() > 0, "the receipt facepile measured empty");
        QVERIFY2(bar->width() > 0, "the action bar measured empty");

        const qreal barRight = rightEdgeIn(bar, d.root);
        const qreal pileLeft = leftEdgeIn(pile, d.root);
        QVERIFY2(barRight <= pileLeft + 1.0,
                 qPrintable(QStringLiteral(
                     "the action bar (ends at %1) runs into the read-receipt "
                     "facepile (starts at %2) — the avatars paint over its "
                     "buttons and the buttons cannot be pressed")
                     .arg(barRight).arg(pileLeft)));
    }

    void theSentCheckClearsTheActionBarOnAShortRow()
    {
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("sentReceipt"), true);

        Delegate d;
        QVERIFY(build(app, f, d));
        auto *check = d.root->findChild<QQuickItem *>(
            QStringLiteral("sentReceiptCheck"));
        QVERIFY2(check, "no sentReceiptCheck in the row");
        QVERIFY2(check->isVisible() && check->width() > 0,
                 "the sent check is not drawn on the newest own message");
        QQuickItem *bar = forceActionBar(d.root);
        QVERIFY2(bar, "no messageActionBarLoader in the row");
        QVERIFY2(bar->width() > 0, "the action bar measured empty");

        const qreal barRight = rightEdgeIn(bar, d.root);
        const qreal checkLeft = leftEdgeIn(check, d.root);
        QVERIFY2(barRight <= checkLeft + 1.0,
                 qPrintable(QStringLiteral(
                     "the action bar (ends at %1) runs into the sent check "
                     "(starts at %2)").arg(barRight).arg(checkLeft)));
        QVERIFY(rightEdgeIn(check, d.root) <= d.root->width() + 1.0);
    }

    void theSentCheckGivesWayToReaders()
    {
        AppController app(AppController::MockBackend);
        {
            QVariantMap f = baseFixture();
            f.insert(QStringLiteral("isOwn"), true);
            Delegate d;
            QVERIFY(build(app, f, d));
            auto *check = d.root->findChild<QQuickItem *>(
                QStringLiteral("sentReceiptCheck"));
            QVERIFY(!check || !check->isVisible());
        }
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("sentReceipt"), true);
        f.insert(QStringLiteral("readReceipts"), receipts(2));
        f.insert(QStringLiteral("readReceiptsTotal"), 2);
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *check = d.root->findChild<QQuickItem *>(
            QStringLiteral("sentReceiptCheck"));
        QVERIFY2(!check || !check->isVisible(),
                 "the sent check is drawn beside the readers it gives way to");
    }

    void anOwnBubbleClearsTheSentCheck()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("sentReceipt"), true);

        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth,true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *check = d.root->findChild<QQuickItem *>(
            QStringLiteral("sentReceiptCheck"));
        QVERIFY(bubble);
        QVERIFY2(check && check->isVisible() && check->width() > 0,
                 "no sent check on the newest own bubble");
        const QRectF bubbleRect = bubble->mapRectToItem(d.root, bubble->boundingRect());
        const QRectF checkRect = check->mapRectToItem(d.root, check->boundingRect());
        QVERIFY(checkRect.top() >= bubbleRect.bottom());
        QVERIFY(qAbs(checkRect.right() - bubbleRect.right()) <= 1.0);
        QVERIFY(d.root->boundingRect().contains(checkRect));
        QVERIFY(!checkRect.intersects(bubbleRect));
    }

    void theCollapsedPileShowsThreeFacesAndAPill_data()
    {
        QTest::addColumn<int>("readers");
        QTest::addColumn<int>("faces");
        QTest::addColumn<int>("overflow");
        QTest::newRow("one reader") << 1 << 1 << 0;
        QTest::newRow("three readers") << 3 << 3 << 0;
        QTest::newRow("four readers") << 4 << 3 << 1;
        QTest::newRow("nine readers") << 9 << 3 << 6;
    }
    void theCollapsedPileShowsThreeFacesAndAPill()
    {
        QFETCH(int, readers);
        QFETCH(int, faces);
        QFETCH(int, overflow);
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(readers));
        f.insert(QStringLiteral("readReceiptsTotal"), readers);
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(pile);
        QCOMPARE(receiptChips(pile).size(), faces);
        auto *pill = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptOverflow"));
        QVERIFY(pill);
        QCOMPARE(pill->isVisible(), overflow > 0);
        if (overflow > 0)
            QCOMPARE(pill->childItems().first()->property("text").toString(),
                     QStringLiteral("+%1").arg(overflow));
        qreal want = 18 + (faces - 1) * 12;
        if (overflow > 0)
            want += -6 + pill->width();
        QVERIFY2(qAbs(pile->width() - want) < 0.5,
                 qPrintable(QStringLiteral("width=%1 want=%2")
                                .arg(pile->width()).arg(want)));
    }

    void hoveringAFaceLiftsItAndItsNeighboursAndMovesNothing()
    {
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(5));
        f.insert(QStringLiteral("readReceiptsTotal"), 5);
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(pile);
        QCOMPARE(pile->property("hoverIndex").toInt(), -1);
        QList<QQuickItem *> chips = receiptChips(pile);
        QCOMPARE(chips.size(), 3);
        for (QQuickItem *c : chips)
            QCOMPARE(c->property("liftNow").toReal(), 0.0);

        const qreal width0 = pile->width();
        const qreal x0 = pile->x();
        const qreal reserve0 = d.root->property("actionBarReceiptReserve").toReal();
        const qreal rail0 = d.root->property("receiptRailReserve").toReal();
        const qreal inset0 = d.root->property("bubbleReceiptInset").toReal();
        const qreal content0 = d.root->property("contentInnerCap").toReal();
        const qreal height0 = d.root->height();
        QList<qreal> chipX;
        for (QQuickItem *c : chips)
            chipX.append(c->x());

        const QPointF mid = chips[1]->mapToScene(QPointF(9, 9));
        QTest::mouseMove(d.window.get(), QPoint(10, 10));
        QTest::mouseMove(d.window.get(), mid.toPoint());
        QTRY_COMPARE(pile->property("hoverIndex").toInt(), 1);
        QTRY_VERIFY2(qAbs(chips[1]->property("liftNow").toReal() - 4.0) < 0.05,
                     "the hovered face did not rise 4px");
        QTRY_VERIFY(qAbs(chips[0]->property("liftNow").toReal() - 1.8) < 0.05);
        QTRY_VERIFY(qAbs(chips[2]->property("liftNow").toReal() - 1.8) < 0.05);
        QTRY_VERIFY(qAbs(chips[1]->scale() - 1.05) < 0.001);
        QCOMPARE(chips[0]->scale(), 1.0);
        QVERIFY(chips[1]->z() > chips[0]->z());
        QVERIFY(chips[1]->z() > chips[2]->z());

        QCOMPARE(pile->width(), width0);
        QCOMPARE(pile->x(), x0);
        for (int i = 0; i < chips.size(); ++i)
            QCOMPARE(chips[i]->x(), chipX[i]);
        QCOMPARE(receiptChips(pile).size(), 3);
        QCOMPARE(d.root->property("actionBarReceiptReserve").toReal(), reserve0);
        QCOMPARE(d.root->property("receiptRailReserve").toReal(), rail0);
        QCOMPARE(d.root->property("bubbleReceiptInset").toReal(), inset0);
        QCOMPARE(d.root->property("contentInnerCap").toReal(), content0);
        QCOMPARE(d.root->height(), height0);

        QTest::mouseMove(d.window.get(), QPoint(700, 440));
        QTRY_COMPARE(pile->property("hoverIndex").toInt(), -1);
        QTRY_VERIFY(qAbs(chips[1]->property("liftNow").toReal()) < 0.05);
        QTRY_VERIFY(qAbs(chips[1]->scale() - 1.0) < 0.001);
    }

    void thePointerOnTheLowerEdgeOfAFaceDoesNotFlicker()
    {
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(5));
        f.insert(QStringLiteral("readReceiptsTotal"), 5);
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(pile);
        const QList<QQuickItem *> chips = receiptChips(pile);
        QCOMPARE(chips.size(), 3);

        const QPointF edgeF = pile->mapToScene(
            QPointF(chips[1]->x() + 9, pile->height() - 1.5));
        const QPoint edgeP(int(std::floor(edgeF.x())), int(std::floor(edgeF.y())));
        QTest::mouseMove(d.window.get(), QPoint(10, 10));
        QTest::mouseMove(d.window.get(), edgeP);
        QTRY_COMPARE(pile->property("hoverIndex").toInt(), 1);
        for (int i = 0; i < 40; ++i) {
            QTest::qWait(16);
            QTest::mouseMove(d.window.get(), edgeP);
            QCOMPARE(pile->property("hoverIndex").toInt(), 1);
        }
        QVERIFY(qAbs(chips[1]->property("liftNow").toReal() - 4.0) < 0.05);

        const QPointF gap = pile->mapToScene(
            QPointF(chips[1]->x() - 3, pile->height() - 1.5));
        QTest::mouseMove(d.window.get(),
                         QPoint(int(std::floor(gap.x())), int(std::floor(gap.y()))));
        for (int i = 0; i < 10; ++i) {
            QTest::qWait(16);
            QVERIFY(pile->property("hoverIndex").toInt() >= 0);
        }
    }

    void aFaceOpensTheProfileAndThePillOpensTheList()
    {
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(5));
        f.insert(QStringLiteral("readReceiptsTotal"), 5);
        Delegate d;
        QVERIFY(build(app, f, d));

        QQmlComponent comp(d.engine.get());
        comp.setData(
            "import QtQuick\nQtObject { property string profileUser: \"\";"
            " property int lists: 0; property string pinnedActionsKey: \"\";"
            " property string hoveredActionsKey: \"\";"
            " property var openSenderProfile:"
            "   function(m, anchor) { profileUser = m.userId }\n"
            " property var openReceiptList:"
            "   function(r, total, p) { lists += 1 } }",
            QUrl());
        std::unique_ptr<QObject> view(comp.create());
        QVERIFY2(view, qPrintable(comp.errorString()));
        QQmlProperty::write(d.root, QStringLiteral("timelineView"),
                            QVariant::fromValue(view.get()));
        QCoreApplication::processEvents();

        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(pile);
        const QList<QQuickItem *> chips = receiptChips(pile);
        QCOMPARE(chips.size(), 3);
        QTest::mouseClick(d.window.get(), Qt::LeftButton, Qt::NoModifier,
                          chips[1]->mapToScene(QPointF(9, 9)).toPoint());
        QTRY_COMPARE(view->property("profileUser").toString(),
                     QStringLiteral("@r1:mock.local"));
        QCOMPARE(view->property("lists").toInt(), 0);

        auto *pill = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptOverflow"));
        QVERIFY(pill && pill->isVisible());
        QTest::mouseClick(d.window.get(), Qt::LeftButton, Qt::NoModifier,
                          pill->mapToScene(QPointF(pill->width() / 2, 9)).toPoint());
        QTRY_COMPARE(view->property("lists").toInt(), 1);
        QCOMPARE(view->property("profileUser").toString(),
                 QStringLiteral("@r1:mock.local"));
    }

    void theFacepileIsAKeyboardFocusStop()
    {
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(2));
        f.insert(QStringLiteral("readReceiptsTotal"), 2);
        Delegate d;
        QVERIFY(build(app, f, d));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(pile);
        QVERIFY2(pile->property("activeFocusOnTab").toBool(),
                 "the facepile cannot be reached with Tab");
    }

    void aClickOnTheFarRightOfARowMovesThePinToThatRow_data()
    {
        QTest::addColumn<int>("layout");
        QTest::newRow("modern") << 0;
        QTest::newRow("compact") << 2;
        QTest::newRow("bubbles") << 1;
    }
    void aClickOnTheFarRightOfARowMovesThePinToThatRow()
    {
        QFETCH(int, layout);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(layout);

        QVariantMap fa = baseFixture();
        fa.insert(QStringLiteral("itemId"), QStringLiteral("row-a"));
        fa.insert(QStringLiteral("eventId"), QStringLiteral("$a"));
        QVariantMap fb = baseFixture();
        fb.insert(QStringLiteral("itemId"), QStringLiteral("row-b"));
        fb.insert(QStringLiteral("eventId"), QStringLiteral("$b"));
        constexpr qreal width = 1500;

        Delegate a, b;
        QVERIFY(build(app, fa, a, width, layout == 1));
        QVERIFY(build(app, fb, b, width, layout == 1));
        for (Delegate *d : {&a, &b}) {
            d->window->resize(1500, 480);
            QCoreApplication::processEvents();
        }

        QQmlComponent comp(a.engine.get());
        comp.setData("import QtQuick\nQtObject { property string pinnedActionsKey: \"\";"
                     " property string hoveredActionsKey: \"\" }",
                     QUrl());
        std::unique_ptr<QObject> view(comp.create());
        QVERIFY2(view, qPrintable(comp.errorString()));
        QQmlProperty::write(a.root, QStringLiteral("timelineView"),
                            QVariant::fromValue(view.get()));
        QQmlProperty::write(b.root, QStringLiteral("timelineView"),
                            QVariant::fromValue(view.get()));
        QCoreApplication::processEvents();

        const auto click = [](Delegate &d, const QPointF &p) {
            QTest::mouseClick(d.window.get(), Qt::LeftButton, Qt::NoModifier,
                              p.toPoint());
        };
        auto *bubbleA = a.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        QVERIFY(bubbleA);
        click(a, bubbleA->mapToScene(QPointF(bubbleA->width() / 2,
                                             bubbleA->height() / 2)));
        QTRY_COMPARE(view->property("pinnedActionsKey").toString(),
                     QStringLiteral("row-a"));

        auto *bubbleB = b.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *presB = b.root->findChild<QQuickItem *>(
            QStringLiteral("messagePresentationRow"));
        QVERIFY(bubbleB && presB);
        const QPointF far = presB->mapToScene(
            QPointF(presB->width() - 24, presB->height() - 4));
        QVERIFY2(bubbleB->contains(bubbleB->mapFromScene(far)) == (layout != 1),
                 layout == 1 ? "fixture: the probe lands inside the bubble"
                             : "the content column no longer reaches the "
                               "row's right edge");
        QTest::mouseMove(b.window.get(), far.toPoint());
        click(b, far);
        QTRY_COMPARE_WITH_TIMEOUT(view->property("pinnedActionsKey").toString(),
                                  QStringLiteral("row-b"), 2000);
    }

    void aTallRowKeepsTheBarInTheCorner()
    {
        AppController app(AppController::MockBackend);
        QVariantMap shortRow = baseFixture();
        shortRow.insert(QStringLiteral("readReceipts"), receipts(4));
        shortRow.insert(QStringLiteral("readReceiptsTotal"), 4);
        QVariantMap tallRow = shortRow;
        tallRow.insert(
            QStringLiteral("body"),
            QStringLiteral("a body long enough to wrap over several lines so "
                           "that the row is far taller than the action bar "
                           "and the facepile at its bottom edge cannot reach "
                           "the bar at its top edge, which is the ordinary "
                           "case and must not be indented"));

        Delegate shortD;
        QVERIFY(build(app, shortRow, shortD));
        QQuickItem *shortBar = forceActionBar(shortD.root);
        QVERIFY(shortBar);
        const qreal shortRight = rightEdgeIn(shortBar, shortD.root);

        Delegate tallD;
        QVERIFY(build(app, tallRow, tallD));
        QQuickItem *tallBar = forceActionBar(tallD.root);
        QVERIFY(tallBar);
        auto *tallPile = tallD.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(tallPile);
        const qreal tallRight = rightEdgeIn(tallBar, tallD.root);

        QVERIFY2(tallD.root->height() > shortD.root->height() + 20,
                 "the fixture did not actually produce a taller row");
        QVERIFY2(tallRight > shortRight + 1.0,
                 qPrintable(QStringLiteral(
                     "a tall row indented the bar anyway (tall ends at %1, "
                     "short at %2): the reservation is unconditional")
                     .arg(tallRight).arg(shortRight)));
    }

    void aBubbleContainsItsOwnSenderHeader()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        Delegate d;
        QVERIFY(build(app, f, d));
        QQmlProperty::write(d.root, QStringLiteral("isDirectRoom"), true);
        QCoreApplication::processEvents();
        QVERIFY2(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool(),
                 "the fixture is not in Bubbles mode; the rest proves nothing");

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *header = d.root->findChild<QQuickItem *>(
            QStringLiteral("senderIdentityHeader"));
        QVERIFY2(bubble, "no bubble in the row");
        QVERIFY2(header, "no sender identity header in the row");
        QVERIFY2(header->width() > 40,
                 qPrintable(QStringLiteral(
                     "the identity header measured %1px wide — it was pinned "
                     "to its minimum by a cap derived from the bubble it "
                     "sizes").arg(header->width())));

        const qreal headerRight = rightEdgeIn(header, d.root);
        const qreal bubbleRight = rightEdgeIn(bubble, d.root);
        QVERIFY2(headerRight <= bubbleRight + 1.0,
                 qPrintable(QStringLiteral(
                     "the sender header (ends at %1) spills past its bubble "
                     "(ends at %2) and renders on the timeline background")
                     .arg(headerRight).arg(bubbleRight)));
    }

    void anOwnBubblePlacesItsReceiptBelow_data()
    {
        QTest::addColumn<bool>("readByOthers");
        QTest::newRow("sent-check") << false;
        QTest::newRow("reader-faces") << true;
    }
    void anOwnBubblePlacesItsReceiptBelow()
    {
        QFETCH(bool, readByOthers);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("showSenderIdentity"), false);
        f.insert(QStringLiteral("body"), QStringLiteral("ok"));
        if (readByOthers) {
            f.insert(QStringLiteral("readReceipts"), receipts(3));
            f.insert(QStringLiteral("readReceiptsTotal"), 3);
        } else {
            f.insert(QStringLiteral("sentReceipt"), true);
        }

        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(bubble);
        QVERIFY(pile);
        QVERIFY2(pile->isVisible() && pile->width() > 0,
                 "the receipt did not appear on the own message");
        auto *check = d.root->findChild<QQuickItem *>(
            QStringLiteral("sentReceiptCheck"));
        if (readByOthers)
            QVERIFY(!check || !check->isVisible());
        else
            QVERIFY(check && check->isVisible());

        const QPointF bubbleTop = bubble->mapToItem(d.root, QPointF(0, 0));
        const QPointF pileTop = pile->mapToItem(d.root, QPointF(0, 0));
        const QRectF pileRect(pileTop, QSizeF(pile->width(), pile->height()));
        const qreal bubbleBottom = bubbleTop.y() + bubble->height();
        QVERIFY2(pileRect.top() >= bubbleBottom + 1.0,
                 qPrintable(QStringLiteral("receipt top %1 is above bubble "
                                           "bottom %2")
                                .arg(pileRect.top()).arg(bubbleBottom)));
        QVERIFY2(qAbs(pileRect.right() - rightEdgeIn(bubble, d.root)) <= 1.0,
                 "the receipt does not end at the bubble's trailing edge");
        QVERIFY2(pileRect.bottom() <= d.root->height() + 0.5,
                 "the receipt hangs into the next message row");

        QQuickItem *bar = forceActionBar(d.root);
        QVERIFY2(bar && bar->width() > 0 && bar->height() > 0,
                 "the action bar did not measure");
        const QPointF barTop = bar->mapToItem(d.root, QPointF(0, 0));
        const QRectF barRect(barTop, QSizeF(bar->width(), bar->height()));
        QVERIFY2(!barRect.intersects(pileRect),
                 "the receipt overlaps the hover action bar on a short row");
    }

    void anOwnBubbleKeepsItsHeightAsReceiptsChange()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("showSenderIdentity"), false);
        f.insert(QStringLiteral("body"), QStringLiteral("ok"));
        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, true));
        QVERIFY(d.root->property("bubbleMode").toBool());

        auto *row = d.root->findChild<QQuickItem *>(
            QStringLiteral("messagePresentationRow"));
        const auto strip = [&d] {
            return d.root->findChild<QQuickItem *>(
                QStringLiteral("readReceiptStrip"));
        };
        const auto check = [&d] {
            return d.root->findChild<QQuickItem *>(
                QStringLiteral("sentReceiptCheck"));
        };
        QVERIFY(row);
        const qreal initialHeight = row->implicitHeight();
        const qreal initialDelegateHeight = d.root->implicitHeight();
        QVERIFY(initialHeight > 0);
        QVERIFY(!strip());

        auto verifyHeight = [&](const char *state) {
            QCoreApplication::processEvents();
            QVERIFY2(qAbs(row->implicitHeight() - initialHeight) <= 0.5, state);
            QVERIFY2(qAbs(d.root->implicitHeight() - initialDelegateHeight) <= 0.5,
                     state);
        };

        f.insert(QStringLiteral("sentReceipt"), true);
        d.engine->rootContext()->setContextProperty("model", f);
        verifyHeight("sent check changed the row height");
        QVERIFY(strip() && strip()->isVisible() && check()->isVisible());

        f.insert(QStringLiteral("readReceipts"), receipts(3));
        f.insert(QStringLiteral("readReceiptsTotal"), 3);
        d.engine->rootContext()->setContextProperty("model", f);
        verifyHeight("reader faces changed the row height");
        QVERIFY(strip() && strip()->isVisible() && !check()->isVisible());

        f.insert(QStringLiteral("sentReceipt"), false);
        f.insert(QStringLiteral("readReceipts"), QVariantList{});
        f.insert(QStringLiteral("readReceiptsTotal"), 0);
        d.engine->rootContext()->setContextProperty("model", f);
        verifyHeight("removing receipts changed the row height");
        QVERIFY(!strip());
    }

    void aPlainTextRowBuildsNoneOfItsRareParts()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), QVariantList{});
        f.insert(QStringLiteral("readReceiptsTotal"), 0);
        f.insert(QStringLiteral("sentReceipt"), false);
        f.insert(QStringLiteral("reactions"), QVariantList{});
        Delegate d;
        QVERIFY(build(app, f, d));
        for (const char *name : {"readReceiptStrip", "reactionsFlow",
                                 "replyNavigationTarget", "stateActivityGroup",
                                 "unreadDivider", "undecryptableActions",
                                 "decryptingSkeleton"}) {
            QVERIFY2(!d.root->findChild<QObject *>(QLatin1String(name)), name);
        }
        const auto objects = d.root->findChildren<QObject *>().size();
        QVERIFY2(objects < 250, qPrintable(QString::number(objects)));
    }

    void aGroupOfCallEventsOnlyTakesNoHeight()
    {
        AppController app(AppController::MockBackend);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isStateActivity"), true);
        f.insert(QStringLiteral("stateGroupLeader"), true);
        f.insert(QStringLiteral("stateGroupId"), QStringLiteral("g"));
        QVariantMap call;
        call.insert(QStringLiteral("eventKind"), QStringLiteral("m.call"));
        f.insert(QStringLiteral("stateGroupEntries"), QVariantList{call});
        Delegate d;
        QVERIFY(build(app, f, d));
        QCoreApplication::processEvents();
        QCOMPARE(d.root->implicitHeight(), 0.0);
    }

    void theActionBarDoesNotCoverAShortOwnBubble()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("body"), QStringLiteral("ok"));

        Delegate d;
        QVERIFY(build(app, f, d));
        QQmlProperty::write(d.root, QStringLiteral("isDirectRoom"), true);
        QCoreApplication::processEvents();
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        QVERIFY(bubble);
        QQuickItem *bar = forceActionBar(d.root);
        QVERIFY2(bar && bar->width() > 0, "the action bar measured empty");
        const QPointF bt = bubble->mapToItem(d.root, QPointF(0, 0));
        const QPointF at = bar->mapToItem(d.root, QPointF(0, 0));
        const QRectF bubbleRect(bt, QSizeF(bubble->width(), bubble->height()));
        const QRectF barRect(at, QSizeF(bar->width(), bar->height()));
        QVERIFY2(!bubbleRect.intersects(barRect),
                 qPrintable(QStringLiteral("action bar %1,%2 %3x%4 covers the "
                                           "own bubble %5,%6 %7x%8")
                                .arg(barRect.x()).arg(barRect.y())
                                .arg(barRect.width()).arg(barRect.height())
                                .arg(bubbleRect.x()).arg(bubbleRect.y())
                                .arg(bubbleRect.width()).arg(bubbleRect.height())));
        QVERIFY(barRect.left() >= 0);
    }

    static QRectF chipBand(QQuickItem *root)
    {
        QRectF acc;
        const auto chips = root->findChildren<QQuickItem *>(
            QStringLiteral("reactionChip"));
        auto add = root->findChildren<QQuickItem *>(
            QStringLiteral("reactionAddChip"));
        QList<QQuickItem *> all = chips;
        all += add;
        for (auto *c : all) {
            if (!c->isVisible() || c->width() <= 0)
                continue;
            const QPointF tl = c->mapToItem(root, QPointF(0, 0));
            const QRectF r(tl, QSizeF(c->width(), c->height()));
            acc = acc.isNull() ? r : acc.united(r);
        }
        return acc;
    }

    static qreal innerRight(QQuickItem *root, QQuickItem *bubble)
    {
        const qreal pad =
            QQmlProperty::read(root, QStringLiteral("bubblePad")).toReal();
        return bubble->mapToItem(root, QPointF(bubble->width(), 0)).x() - pad;
    }

    void ownBubbleReactionsHangUnderTheirBubble()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("reactions"), reactions(3));

        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        QVERIFY(bubble);
        const QRectF band = chipBand(d.root);
        QVERIFY2(!band.isNull(), "the fixture produced no reaction chips");

        const qreal bubbleLeft = leftEdgeIn(bubble, d.root);
        QVERIFY2(band.right() >= bubbleLeft - 240.0,
                 qPrintable(QStringLiteral(
                     "the reaction chips end at %1 but the own bubble starts "
                     "at %2 — they are stranded at the row's left edge, on "
                     "the incoming side of the conversation")
                     .arg(band.right()).arg(bubbleLeft)));
    }

    static qreal reactionGap(QQuickItem *root)
    {
        auto *bubble = root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *flow = root->findChild<QQuickItem *>(
            QStringLiteral("reactionsFlow"));
        if (!bubble || !flow || !flow->isVisible())
            return -1e9;
        const qreal bottom =
            bubble->mapToItem(root, QPointF(0, bubble->height())).y();
        return flow->mapToItem(root, QPointF(0, 0)).y() - bottom;
    }

    void reactionsSitAtTheDesignGapUnderTheirMessage_data()
    {
        QTest::addColumn<int>("layoutId");
        QTest::addColumn<bool>("own");
        QTest::newRow("modern") << 0 << false;
        QTest::newRow("compact") << 2 << false;
        QTest::newRow("bubbles-incoming") << 1 << false;
        QTest::newRow("bubbles-own") << 1 << true;
    }
    void reactionsSitAtTheDesignGapUnderTheirMessage()
    {
        QFETCH(int, layoutId);
        QFETCH(bool, own);
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(layoutId);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), own);
        f.insert(QStringLiteral("showSenderIdentity"), false);
        f.insert(QStringLiteral("reactions"), reactions(2));
        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, layoutId == 1));

        auto *flow = d.root->findChild<QQuickItem *>(
            QStringLiteral("reactionsFlow"));
        auto *strip = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptStrip"));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(flow && strip && pile);
        const qreal design = 2 + flow->property("designTopGap").toReal();
        QVERIFY2(design > 2, "reactionsFlow does not publish designTopGap");

        auto expectGap = [&](const char *state) {
            QCoreApplication::processEvents();
            const qreal gap = reactionGap(d.root);
            QVERIFY2(qAbs(gap - design) <= 0.5,
                     qPrintable(QStringLiteral("%1: gap bubble->reactions is "
                                "%2, design is %3")
                                .arg(QLatin1String(state)).arg(gap)
                                .arg(design)));
        };
        auto setReceipts = [&](int n, bool sent) {
            f.insert(QStringLiteral("readReceipts"),
                     n ? receipts(n) : QVariantList{});
            f.insert(QStringLiteral("readReceiptsTotal"), n);
            f.insert(QStringLiteral("sentReceipt"), sent);
            d.engine->rootContext()->setContextProperty("model", f);
        };

        expectGap("no receipts");
        setReceipts(3, false);
        QVERIFY(strip->isVisible());
        expectGap("readers present");
        const QRectF band = chipBand(d.root);
        const QPointF pt = pile->mapToItem(d.root, QPointF(0, 0));
        const QRectF pileRect(pt, QSizeF(pile->width(), pile->height()));
        QVERIFY(!band.isNull() && pileRect.width() > 0);
        QVERIFY2(!pileRect.intersects(band),
                 "the receipt overlaps the reaction pills");
        QVERIFY2(pileRect.center().y() >= band.top() - 1.0
                     && pileRect.center().y() <= band.bottom() + 1.0,
                 "the receipt is not on the pills' line");
        QVERIFY2(pileRect.left() >= band.right(),
                 "the receipt is not to the right of the pills");
        QVERIFY2(pileRect.right() <= d.root->width() + 0.5,
                 "the receipt hangs past the row");
        setReceipts(0, false);
        QVERIFY(!strip->isVisible());
        expectGap("readers left");
        setReceipts(0, true);
        expectGap("sent check only");
        setReceipts(0, false);
        expectGap("sent check left");
    }

    void aBubbleContainsItsOwnBodyText()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("body"), QString(220, QLatin1Char('W')));

        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *body = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(bubble);
        QVERIFY2(body && body->width() > 0, "no message body in the row");

        const qreal bodyRight = rightEdgeIn(body, d.root);
        QVERIFY2(bodyRight <= innerRight(d.root, bubble) + 0.6,
                 qPrintable(QStringLiteral(
                     "the body ends at %1, past the bubble's inner edge %2 "
                     "(bubble ends at %3) — the text is drawn on and over "
                     "the bubble's own border")
                     .arg(bodyRight).arg(innerRight(d.root, bubble))
                     .arg(rightEdgeIn(bubble, d.root))));
        QVERIFY2(bodyRight <= d.root->width() + 0.6,
                 qPrintable(QStringLiteral(
                     "the body ends at %1, past the row's own width %2")
                     .arg(bodyRight).arg(d.root->width())));
    }

    void aNarrowBubbleContainsItsFileCard()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("isFile"), true);
        f.insert(QStringLiteral("mediaSize"), 4500000);
        f.insert(QStringLiteral("mediaMimetype"), QStringLiteral("application/pdf"));
        f.insert(QStringLiteral("mediaKey"), QStringLiteral("k5"));
        f.insert(QStringLiteral("mediaFilename"),
                 QStringLiteral("a-really-long-attachment-filename.pdf"));
        f.insert(QStringLiteral("body"),
                 QStringLiteral("a-really-long-attachment-filename.pdf"));

        Delegate d;
        QVERIFY(build(app, f, d, 360, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *card = d.root->findChild<QQuickItem *>(
            QStringLiteral("fileCard"));
        QVERIFY(bubble);
        QVERIFY2(card && card->width() > 0, "no file card in the row");

        const qreal cardRight = rightEdgeIn(card, d.root);
        QVERIFY2(cardRight <= innerRight(d.root, bubble) + 0.6,
                 qPrintable(QStringLiteral(
                     "the file card ends at %1, past the bubble's inner edge "
                     "%2 (bubble ends at %3) — the card is painted outside "
                     "the bubble that is supposed to hold it")
                     .arg(cardRight).arg(innerRight(d.root, bubble))
                     .arg(rightEdgeIn(bubble, d.root))));
        QVERIFY2(cardRight <= d.root->width() + 0.6,
                 qPrintable(QStringLiteral(
                     "the file card ends at %1, past the row's own width %2")
                     .arg(cardRight).arg(d.root->width())));
    }

    void aNarrowBubbleWithReceiptsContainsItsSenderHeader()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("readReceipts"), receipts(4));
        f.insert(QStringLiteral("readReceiptsTotal"), 4);

        Delegate d;
        QVERIFY(build(app, f, d, 360, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *header = d.root->findChild<QQuickItem *>(
            QStringLiteral("senderIdentityHeader"));
        auto *pile = d.root->findChild<QQuickItem *>(
            QStringLiteral("readReceiptRow"));
        QVERIFY(bubble);
        QVERIFY2(header && header->width() > 0, "no sender header in the row");
        QVERIFY(pile);

        const qreal headerRight = rightEdgeIn(header, d.root);
        QVERIFY2(headerRight <= innerRight(d.root, bubble) + 0.6,
                 qPrintable(QStringLiteral(
                     "the sender header ends at %1, past the bubble's inner "
                     "edge %2 (bubble ends at %3) — the name and timestamp "
                     "render on the timeline background")
                     .arg(headerRight).arg(innerRight(d.root, bubble))
                     .arg(rightEdgeIn(bubble, d.root))));
        QVERIFY2(headerRight <= leftEdgeIn(pile, d.root) + 0.6,
                 qPrintable(QStringLiteral(
                     "the sender header ends at %1 and the receipt facepile "
                     "starts at %2 — the timestamp is under the avatars")
                     .arg(headerRight).arg(leftEdgeIn(pile, d.root))));
    }

    void aRightToLeftParagraphStartsAtTheColumnsRightEdge()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(0);

        const QStringList bodies = {
            arabic(),
            QString::fromUtf8("\xf0\x9f\x98\x80 ") + arabic(),
        };
        for (const QString &text : bodies) {
            QVariantMap f = baseFixture();
            f.insert(QStringLiteral("body"), text);
            Delegate d;
            QVERIFY(build(app, f, d));
            auto *column = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageContentColumn"));
            auto *body = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageBody"));
            QVERIFY(column);
            QVERIFY2(body && body->isVisible(), "no message body in the row");
            QTextDocument *doc = documentOf(body);
            QVERIFY(doc);
            QCOMPARE(doc->firstBlock().textDirection(), Qt::RightToLeft);
            const qreal start = paragraphStartIn(body, doc->firstBlock(), d.root);
            QVERIFY2(qAbs(start - rightEdgeIn(column, d.root)) <= 1.5,
                     qPrintable(QStringLiteral(
                         "the Arabic paragraph starts at %1 and the column "
                         "ends at %2 — it reads left-aligned")
                         .arg(start).arg(rightEdgeIn(column, d.root))));
        }

        QVariantMap g = baseFixture();
        g.insert(QStringLiteral("body"), QStringLiteral("Mixed: hello 123 ")
                                             + arabic() + QStringLiteral(" world"));
        Delegate e;
        QVERIFY(build(app, g, e));
        auto *mixed = e.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(mixed);
        QTextDocument *mixedDoc = documentOf(mixed);
        QVERIFY(mixedDoc);
        QCOMPARE(mixedDoc->firstBlock().textDirection(), Qt::LeftToRight);
        QVERIFY2(qAbs(paragraphStartIn(mixed, mixedDoc->firstBlock(), e.root)
                      - leftEdgeIn(mixed, e.root)) <= 1.5,
                 "a left-to-right line does not start at the body's left edge");
        QVERIFY2(mixed->width() <= mixed->implicitWidth() + 1.0,
                 qPrintable(QStringLiteral(
                     "a left-to-right body was widened to %1 (its text is %2)")
                     .arg(mixed->width()).arg(mixed->implicitWidth())));
    }

    void eachParagraphTakesItsOwnDirection()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(0);

        const QString english = QStringLiteral("Hello world");
        const QList<QStringList> orders = {
            { english, arabic() },
            { arabic(), english },
        };
        for (const QStringList &order : orders) {
            QVariantMap f = baseFixture();
            f.insert(QStringLiteral("body"), order.join(QStringLiteral("\n\n")));
            f.insert(QStringLiteral("formattedBody"),
                     QStringLiteral("<p>%1</p><p>%2</p>")
                         .arg(order.at(0), order.at(1)));
            Delegate d;
            QVERIFY(build(app, f, d));
            auto *column = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageContentColumn"));
            auto *body = d.root->findChild<QQuickItem *>(
                QStringLiteral("messageBody"));
            QVERIFY(column && body);
            QTextDocument *doc = documentOf(body);
            QVERIFY(doc);
            QCOMPARE(doc->blockCount(), 2);
            int checked = 0;
            for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
                const bool rtl = b.text() == arabic();
                QCOMPARE(b.textDirection(),
                         rtl ? Qt::RightToLeft : Qt::LeftToRight);
                const qreal edge = rtl ? rightEdgeIn(column, d.root)
                                       : leftEdgeIn(column, d.root);
                const qreal start = paragraphStartIn(body, b, d.root);
                QVERIFY2(qAbs(start - edge) <= 1.5,
                         qPrintable(QStringLiteral(
                             "paragraph %1 (%2) starts at %3, not at %4")
                             .arg(b.blockNumber())
                             .arg(rtl ? QStringLiteral("Arabic")
                                      : QStringLiteral("English"))
                             .arg(start).arg(edge)));
                ++checked;
            }
            QCOMPARE(checked, 2);
        }
    }

    void aRightToLeftBodyStartsAtItsBubblesInnerRightEdge()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("body"), arabic());
        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, true));
        QVERIFY(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool());

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        auto *body = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageBody"));
        QVERIFY(bubble);
        QVERIFY2(body && body->isVisible(), "no message body in the row");
        QTextDocument *doc = documentOf(body);
        QVERIFY(doc);
        QVERIFY2(body->implicitWidth() + 20 < bubble->width(),
                 "the fixture's header no longer outgrows its text");
        const qreal start = paragraphStartIn(body, doc->firstBlock(), d.root);
        QVERIFY2(qAbs(start - innerRight(d.root, bubble)) <= 1.5,
                 qPrintable(QStringLiteral(
                     "the Arabic paragraph starts at %1, the bubble's inner "
                     "edge is at %2")
                     .arg(start).arg(innerRight(d.root, bubble))));
        QVERIFY(rightEdgeIn(body, d.root) <= innerRight(d.root, bubble) + 0.6);
    }

    void textOnAnOwnBubbleIsReadableOnEveryPreset()
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(1);

        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("isOwn"), true);
        f.insert(QStringLiteral("isImage"), true);
        f.insert(QStringLiteral("mediaWidth"), 320);
        f.insert(QStringLiteral("mediaHeight"), 200);
        f.insert(QStringLiteral("mediaMimetype"), QStringLiteral("image/png"));
        f.insert(QStringLiteral("mediaFilename"), QStringLiteral("lake.png"));
        f.insert(QStringLiteral("body"),
                 QStringLiteral("Sunset over the lake"));
        f.insert(QStringLiteral("replyToEventId"), QStringLiteral("$quoted"));
        f.insert(QStringLiteral("replyToSender"), QStringLiteral("Bob"));
        f.insert(QStringLiteral("replyToPreview"),
                 QStringLiteral("the line being answered"));
        f.insert(QStringLiteral("edited"), true);

        Delegate d;
        QVERIFY(build(app, f, d, kRowWidth, true));
        QVERIFY2(QQmlProperty::read(d.root, QStringLiteral("bubbleMode")).toBool(),
                 "the fixture is not in Bubbles mode; the rest proves nothing");
        QVERIFY2(QQmlProperty::read(d.root, QStringLiteral("mediaCaptionBody"))
                     .toBool(),
                 "the fixture's body is not read as a caption");
        QTest::qWait(300);

        auto *bubble = d.root->findChild<QQuickItem *>(
            QStringLiteral("messageContentColumn"));
        QVERIFY(bubble);
        struct Probe {
            const char *what;
            QQuickItem *item;
        };
        const QList<Probe> probes = {
            { "the caption",
              findVisual(d.root, QStringLiteral("messageBody")) },
            { "the quoted reply line",
              findVisual(d.root, QStringLiteral("replyQuoteBody")) },
            { "the status line",
              findVisual(d.root, QStringLiteral("messageStatusText")) },
        };
        for (const Probe &p : probes) {
            QVERIFY2(p.item && p.item->isVisible(),
                     qPrintable(QStringLiteral("%1 is not on the bubble")
                                    .arg(QLatin1String(p.what))));
            QVERIFY2(p.item->mapToItem(bubble, QPointF(0, 0)).y() >= 0,
                     qPrintable(QStringLiteral("%1 is not inside the bubble")
                                    .arg(QLatin1String(p.what))));
        }

        QSet<QRgb> fills;
        int checked = 0;
        for (int mode = 1; mode <= 11; ++mode) {
            QQmlExpression set(qmlContext(d.root), d.root,
                               QStringLiteral("AppTheme.mode = %1").arg(mode));
            set.evaluate();
            QVERIFY2(!set.hasError(), qPrintable(set.error().toString()));
            QCoreApplication::processEvents();

            const QColor fill = bubble->property("color").value<QColor>();
            QVERIFY2(fill.isValid() && fill.alpha() == 255,
                     qPrintable(QStringLiteral("theme %1: the own bubble is "
                                               "not an opaque fill (%2)")
                                    .arg(mode).arg(fill.name(QColor::HexArgb))));
            fills.insert(fill.rgb());
            for (const Probe &p : probes) {
                const QColor ink = p.item->property("color").value<QColor>();
                QVERIFY(ink.isValid());
                const double alpha = ink.alphaF() * opacityUpTo(p.item, bubble);
                const double ratio = contrastRatio(over(ink, alpha, fill), fill);
                QVERIFY2(ratio >= 4.5,
                         qPrintable(QStringLiteral(
                             "theme %1: %2 is %3 at %4 opacity on the own "
                             "bubble %5 — %6:1, under the 4.5:1 text floor")
                                        .arg(mode)
                                        .arg(QLatin1String(p.what))
                                        .arg(ink.name(QColor::HexRgb))
                                        .arg(alpha, 0, 'f', 2)
                                        .arg(fill.name(QColor::HexRgb))
                                        .arg(ratio, 0, 'f', 2)));
            }
            ++checked;
        }
        QQmlExpression reset(qmlContext(d.root), d.root,
                             QStringLiteral("AppTheme.mode = 0"));
        reset.evaluate();
        QCOMPARE(checked, 11);
        QVERIFY2(fills.size() >= 8,
                 qPrintable(QStringLiteral("only %1 distinct own-bubble fills "
                                           "were measured")
                                .arg(fills.size())));
    }

private:
    static QList<QQuickItem *> receiptChips(QQuickItem *pile)
    {
        QList<QQuickItem *> out;
        for (QQuickItem *c : pile->childItems())
            if (c->objectName() == QLatin1String("readReceiptChip")
                && c->isVisible())
                out.append(c);
        std::sort(out.begin(), out.end(), [](QQuickItem *a, QQuickItem *b) {
            return a->property("chipIndex").toInt()
                   < b->property("chipIndex").toInt();
        });
        return out;
    }

    QTemporaryDir m_configHome;
};

QTEST_MAIN(MessageRailCollisionTest)
#include "MessageRailCollisionTest.moc"
