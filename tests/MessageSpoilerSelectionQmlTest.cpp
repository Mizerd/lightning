// C17-F01: a covered spoiler must not be selectable. The selection highlight
// repaints the run in clear and Ctrl+C copies it, so MessageDelegate turns
// selectByMouse off for any body (single TextEdit or fenced-code segment)
// that still carries a covered spoiler, and back on once it is revealed.
// Real delegate, real sanitizer output, and a real mouse drag.
#include <QtTest/QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "models/MessageHtml.h"

namespace {
constexpr int kSignalTimeoutMs = 5000;
constexpr qreal kRowWidth = 640;

QString sanitized(bool reveal)
{
    MessageHtml::MentionStyle style;
    style.codeBackground = QStringLiteral("#303030");
    return MessageHtml::sanitize(
        QStringLiteral("Normal <span data-mx-spoiler>hiddenword</span> tail."),
        nullptr, QString(), style, reveal);
}
}

class MessageSpoilerSelectionQmlTest : public QObject
{
    Q_OBJECT

    struct Delegate {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
    };

    /// A complete role map, so the production delegate binds without
    /// undefined-property warnings. Same shape MediaPlaceholderQmlTest uses.
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
        f.insert(QStringLiteral("isLocalEcho"), false);
        f.insert(QStringLiteral("readReceipts"), QVariantList{});
        f.insert(QStringLiteral("readReceiptsTotal"), 0);
        f.insert(QStringLiteral("sentReceipt"), false);
        return f;
    }

    bool build(AppController &controller, const QVariantMap &fixture,
               Delegate &out, qreal rowWidth = kRowWidth)
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
        out.window->show();
        for (int i = 0; i < 6; ++i)
            QCoreApplication::processEvents();
        return true;
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

    /// Drags across the whole text item with the real pointer and returns
    /// what the TextEdit ended up selecting.
    static QString dragSelect(Delegate &d, QQuickItem *text)
    {
        const QPointF a = text->mapToScene(QPointF(1, text->height() / 2));
        const QPointF b = text->mapToScene(
            QPointF(text->width() - 1, text->height() / 2));
        QTest::mousePress(d.window.get(), Qt::LeftButton, Qt::NoModifier,
                          a.toPoint());
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(d.window.get(),
                             QPoint(int(a.x() + (b.x() - a.x()) * i / 8),
                                    int(a.y())));
        QTest::mouseRelease(d.window.get(), Qt::LeftButton, Qt::NoModifier,
                            b.toPoint());
        QCoreApplication::processEvents();
        return text->property("selectedText").toString();
    }

    void check(bool segmented, bool reveal)
    {
        AppController app(AppController::MockBackend);
        QVERIFY(app.settings());
        app.settings()->setMessageLayout(0);
        QVariantMap f = baseFixture();
        f.insert(QStringLiteral("showSenderIdentity"), false);
        f.insert(QStringLiteral("body"),
                 QStringLiteral("Normal hiddenword tail."));
        const QString html = sanitized(reveal);
        QCOMPARE(html.contains(QStringLiteral(";color:#303030")), !reveal);
        f.insert(QStringLiteral("formattedBody"), html);
        if (segmented) {
            QVariantMap seg;
            seg.insert(QStringLiteral("kind"), 0);
            seg.insert(QStringLiteral("text"), html);
            QVariantMap code;
            code.insert(QStringLiteral("kind"), 1);
            code.insert(QStringLiteral("text"), QStringLiteral("x = 1"));
            code.insert(QStringLiteral("language"), QString());
            f.insert(QStringLiteral("messageSegments"),
                     QVariantList{seg, code});
        }
        Delegate d;
        QVERIFY(build(app, f, d));
        QQuickItem *text = findVisual(
            d.root, segmented ? QStringLiteral("messageSegmentText")
                              : QStringLiteral("messageBody"));
        QVERIFY2(text && text->width() > 0, "no text item in the row");
        QCOMPARE(text->property("selectByMouse").toBool(), reveal);
        const QString got = dragSelect(d, text);
        if (reveal)
            QVERIFY2(got.contains(QStringLiteral("hiddenword")),
                     qPrintable(got));
        else
            QVERIFY2(!got.contains(QStringLiteral("hiddenword")),
                     qPrintable(got));
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("message-spoiler-selection-qml-test"));
    }

    void aCoveredSpoilerCannotBeSelected() { check(false, false); }
    void aRevealedSpoilerCanBeSelected() { check(false, true); }
    void aCoveredSpoilerInASegmentedBodyCannotBeSelected()
    {
        check(true, false);
    }
    void aRevealedSpoilerInASegmentedBodyCanBeSelected()
    {
        check(true, true);
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_MAIN(MessageSpoilerSelectionQmlTest)
#include "MessageSpoilerSelectionQmlTest.moc"
