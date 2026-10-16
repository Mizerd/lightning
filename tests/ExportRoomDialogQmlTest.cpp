// The Export room dialog, loaded for real (offscreen) against a stub `app`
// carrying a real RoomExportController over a scripted client. Pins:
//   * the dialog loads without QML warnings of its own;
//   * HTML is a format and the default; Text and JSON remain;
//   * the four ranges exist when history can be read, and only the loaded
//     range when it cannot;
//   * the attachment controls exist only for HTML, the count field only for
//     "latest" and the date fields only for "dates";
//   * bad counts, dates and size limits are refused before a file is picked;
//   * the encrypted-room opt-in is off on every open;
//   * picking a file hands startRoomExport exactly the chosen options.

#include "matrix/MatrixClient.h"
#include "models/RoomExportController.h"

#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class WalkClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    bool walk = true;
    bool supportsRoomExportWalk() const override { return walk; }

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

// Stands in for app.roomList: only findRoom.
class StubRoomList final : public QObject
{
    Q_OBJECT
public:
    bool encrypted = false;
    Q_INVOKABLE QVariantMap findRoom(const QString &) const
    {
        return QVariantMap{
            { QStringLiteral("name"), QStringLiteral("The Lounge") },
            { QStringLiteral("encryptionKnown"), true },
            { QStringLiteral("encrypted"), encrypted },
        };
    }
};

// Only what the dialog reads from `app`.
class StubApp final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *roomExport READ roomExport CONSTANT)
    Q_PROPERTY(QObject *roomList READ roomList CONSTANT)
    Q_PROPERTY(QString currentRoomId READ currentRoomId CONSTANT)
public:
    StubApp(QObject *roomExport, QObject *roomList)
        : m_roomExport(roomExport), m_roomList(roomList)
    {
    }
    QObject *roomExport() const { return m_roomExport; }
    QObject *roomList() const { return m_roomList; }
    QString currentRoomId() const { return QStringLiteral("!r:example.org"); }

    Q_INVOKABLE int exportableMessageCount() const { return 12; }
    Q_INVOKABLE QUrl defaultFileDialogFolder() const { return {}; }
    Q_INVOKABLE QString suggestedExportFileName(const QString &format) const
    {
        return QStringLiteral("room.") + format;
    }
    Q_INVOKABLE QString startRoomExport(const QUrl &fileUrl,
                                        const QString &format,
                                        const QString &range, int count,
                                        const QString &fromDate,
                                        const QString &toDate,
                                        bool includeAttachments,
                                        int sizeLimitMb,
                                        bool includeEncryptedText)
    {
        ++calls;
        lastFile = fileUrl;
        lastFormat = format;
        lastRange = range;
        lastCount = count;
        lastFrom = fromDate;
        lastTo = toDate;
        lastAttachments = includeAttachments;
        lastSizeMb = sizeLimitMb;
        lastEncryptedText = includeEncryptedText;
        return QString();
    }

    int calls = 0;
    QUrl lastFile;
    QString lastFormat;
    QString lastRange;
    int lastCount = 0;
    QString lastFrom;
    QString lastTo;
    bool lastAttachments = false;
    int lastSizeMb = 0;
    bool lastEncryptedText = true;

private:
    QObject *m_roomExport;
    QObject *m_roomList;
};

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 900
    height: 900
    visible: true
    ExportRoomDialog {
        objectName: "exportDialogUnderTest"
        parent: Overlay.overlay
    }
}
)QML";

} // namespace

class ExportRoomDialogQmlTest : public QObject
{
    Q_OBJECT

private:
    WalkClient m_client;
    RoomExportController m_exporter;
    StubRoomList m_rooms;
    StubApp *m_app = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QObject *m_dialog = nullptr;
    QStringList m_warnings;

    QObject *child(const char *name) const
    {
        if (QObject *hit = m_dialog->findChild<QObject *>(QLatin1String(name)))
            return hit;
        return m_root->findChild<QObject *>(QLatin1String(name));
    }

    bool visible(const char *name) const
    {
        QObject *o = child(name);
        return o && o->property("visible").toBool();
    }

    void openDialog()
    {
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "openDialog"));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
    }

    // A `var` property holding a JS array reads back as a QJSValue.
    static QVariant plain(const QVariant &value)
    {
        if (value.metaType() == QMetaType::fromType<QJSValue>())
            return value.value<QJSValue>().toVariant();
        return value;
    }

    QStringList stringList(QObject *object, const char *name) const
    {
        return plain(object->property(name)).toStringList();
    }

    QString inputProblem()
    {
        QVariant result;
        QMetaObject::invokeMethod(m_dialog, "inputProblem",
                                  Q_RETURN_ARG(QVariant, result));
        return result.toString();
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_exporter.setClient(&m_client);
        m_app = new StubApp(&m_exporter, &m_rooms);
        m_engine = new QQmlEngine(this);
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const QQmlError &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_app);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("exportscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        m_dialog = m_root->findChild<QObject *>(
            QStringLiteral("exportDialogUnderTest"));
        QVERIFY(m_dialog);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_app;
    }

    void loadsWithoutWarningsOfItsOwn()
    {
        openDialog();
        QStringList mine;
        for (const QString &w : std::as_const(m_warnings)) {
            if (w.contains(QStringLiteral("ExportRoomDialog")))
                mine << w;
        }
        QVERIFY2(mine.isEmpty(), qPrintable(mine.join(QLatin1Char('\n'))));
    }

    void htmlIsAFormatAndTheDefault()
    {
        openDialog();
        QCOMPARE(m_dialog->property("format").toString(),
                 QStringLiteral("html"));
        QObject *format = child("exportRoomFormat");
        QVERIFY(format);
        QStringList values;
        const QVariantList model = plain(format->property("model")).toList();
        for (const QVariant &entry : model)
            values << entry.toMap().value(QStringLiteral("value")).toString();
        QCOMPARE(values, (QStringList{ QStringLiteral("html"),
                                       QStringLiteral("text"),
                                       QStringLiteral("json") }));
    }

    void theFourRangesExistWhenHistoryCanBeRead()
    {
        openDialog();
        QCOMPARE(stringList(m_dialog, "rangeValues"),
                 (QStringList{ QStringLiteral("loaded"),
                               QStringLiteral("latest"),
                               QStringLiteral("all"),
                               QStringLiteral("dates") }));
        QObject *box = child("exportRoomRange");
        QVERIFY(box);
        QCOMPARE(box->property("count").toInt(), 4);
        QCOMPARE(m_dialog->property("rangeMode").toString(),
                 QStringLiteral("loaded"));

        // Without a history walk only the loaded range is offered.
        m_client.walk = false;
        Q_EMIT m_client.loginSucceeded(QStringLiteral("@me:example.org"));
        QTRY_COMPARE(stringList(m_dialog, "rangeValues"),
                     QStringList{ QStringLiteral("loaded") });
        m_client.walk = true;
        Q_EMIT m_client.loginSucceeded(QStringLiteral("@me:example.org"));
        QTRY_COMPARE(stringList(m_dialog, "rangeValues").size(),
                     4);
    }

    void eachControlAppearsOnlyWhereItApplies()
    {
        openDialog();
        QVERIFY(visible("exportRoomAttachmentOptions"));
        QVERIFY(visible("exportRoomAttachments"));
        QVERIFY(visible("exportRoomSizeLimit"));
        QVERIFY(!visible("exportRoomCount"));
        QVERIFY(!visible("exportRoomDateRow"));

        m_dialog->setProperty("format", QStringLiteral("text"));
        QTRY_VERIFY(!visible("exportRoomAttachmentOptions"));
        m_dialog->setProperty("format", QStringLiteral("json"));
        QVERIFY(!visible("exportRoomAttachmentOptions"));
        m_dialog->setProperty("format", QStringLiteral("html"));
        QTRY_VERIFY(visible("exportRoomAttachmentOptions"));

        m_dialog->setProperty("rangeMode", QStringLiteral("latest"));
        QTRY_VERIFY(visible("exportRoomCount"));
        QVERIFY(!visible("exportRoomDateRow"));
        m_dialog->setProperty("rangeMode", QStringLiteral("dates"));
        QTRY_VERIFY(visible("exportRoomDateRow"));
        QVERIFY(!visible("exportRoomCount"));
    }

    void badInputIsRefusedBeforeAFileIsPicked()
    {
        openDialog();
        QObject *count = child("exportRoomCount");
        QObject *from = child("exportRoomFromDate");
        QObject *to = child("exportRoomToDate");
        QObject *size = child("exportRoomSizeLimit");
        QVERIFY(count && from && to && size);

        m_dialog->setProperty("rangeMode", QStringLiteral("latest"));
        count->setProperty("text", QStringLiteral("0"));
        QVERIFY(!inputProblem().isEmpty());
        count->setProperty("text", QStringLiteral("250"));
        QVERIFY(inputProblem().isEmpty());

        m_dialog->setProperty("rangeMode", QStringLiteral("dates"));
        QVERIFY(!inputProblem().isEmpty()); // both empty
        from->setProperty("text", QStringLiteral("2026-02-01"));
        to->setProperty("text", QStringLiteral("2026-01-01"));
        QVERIFY(!inputProblem().isEmpty()); // reversed
        to->setProperty("text", QStringLiteral("2026-03-01"));
        QVERIFY(inputProblem().isEmpty());

        size->setProperty("text", QStringLiteral("0"));
        QVERIFY(!inputProblem().isEmpty());
        // Not an HTML export with attachments: the size limit is irrelevant.
        m_dialog->setProperty("format", QStringLiteral("text"));
        QVERIFY(inputProblem().isEmpty());
    }

    // B131/B135. A finished export used to close the dialog without a word,
    // and the scope sentence called state events "messages". Fails on the
    // old code: onFinished(ok) closed the dialog, and no success label or
    // Show-in-folder button existed.
    void aSuccessfulExportStaysOpenAndSaysSo()
    {
        openDialog();
        QVERIFY(!visible("exportRoomSuccess"));
        QVERIFY(!visible("exportRoomShowInFolder"));
        const QString scope =
            child("exportRoomScope")->property("text").toString();
        QVERIFY2(scope.contains(QLatin1String("events")), qPrintable(scope));
        QVERIFY2(!scope.contains(QLatin1String("messages")),
                 qPrintable(scope));

        Q_EMIT m_exporter.finished(
            true, QStringLiteral("Exported 12 events to room.html."));
        QVERIFY(m_dialog->property("visible").toBool());
        QTRY_VERIFY(visible("exportRoomSuccess"));
        QCOMPARE(child("exportRoomSuccess")->property("text").toString(),
                 QStringLiteral("Exported 12 events to room.html."));
        QVERIFY(visible("exportRoomShowInFolder"));
        QVERIFY(!visible("exportRoomFailure"));

        // A failure replaces it, and a reopen starts clean.
        Q_EMIT m_exporter.finished(false, QStringLiteral("Could not write."));
        QTRY_VERIFY(!visible("exportRoomSuccess"));
        QVERIFY(visible("exportRoomFailure"));
        Q_EMIT m_exporter.finished(true, QStringLiteral("Exported 1 event to a."));
        QTRY_VERIFY(visible("exportRoomSuccess"));
        QMetaObject::invokeMethod(m_dialog, "close");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        openDialog();
        QVERIFY(!visible("exportRoomSuccess"));
    }

    void theEncryptedOptInIsOffOnEveryOpen()
    {
        m_rooms.encrypted = true;
        openDialog();
        QVERIFY(visible("exportRoomEncryptedNotice"));
        QObject *box = child("exportRoomIncludeEncrypted");
        QVERIFY(box);
        QVERIFY(!box->property("checked").toBool());
        m_dialog->setProperty("includeEncryptedText", true);
        QMetaObject::invokeMethod(m_dialog, "close");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        openDialog();
        QVERIFY(!m_dialog->property("includeEncryptedText").toBool());
        QVERIFY(!box->property("checked").toBool());
        m_rooms.encrypted = false;
    }

    // The opt-in sentence is long; it must wrap inside the notice and not run
    // past the dialog's edge. Fails on the old code: the box was as wide as
    // its one-line label.
    void theEncryptedOptInLabelStaysInsideTheNotice()
    {
        m_rooms.encrypted = true;
        openDialog();
        auto *box = qobject_cast<QQuickItem *>(child("exportRoomIncludeEncrypted"));
        auto *notice = qobject_cast<QQuickItem *>(child("exportRoomEncryptedNotice"));
        QVERIFY(box && notice);
        QTRY_VERIFY(notice->width() > 0);
        const QPointF boxRight = box->mapToItem(notice, QPointF(box->width(), 0));
        QVERIFY2(boxRight.x() <= notice->width() + 0.5,
                 qPrintable(QStringLiteral("box right %1 > notice %2")
                                .arg(boxRight.x()).arg(notice->width())));
        m_rooms.encrypted = false;
    }

    void pickingAFileStartsTheExportWithTheChosenOptions()
    {
        openDialog();
        m_dialog->setProperty("rangeMode", QStringLiteral("latest"));
        child("exportRoomCount")->setProperty("text", QStringLiteral("250"));
        child("exportRoomSizeLimit")->setProperty("text", QStringLiteral("64"));
        QObject *save = child("exportRoomSaveDialog");
        QVERIFY(save);
        const QUrl file = QUrl::fromLocalFile(QStringLiteral("/tmp/x.html"));
        save->setProperty("selectedFile", file);
        QVERIFY(QMetaObject::invokeMethod(save, "accepted"));
        QCOMPARE(m_app->calls, 1);
        QCOMPARE(m_app->lastFile, file);
        QCOMPARE(m_app->lastFormat, QStringLiteral("html"));
        QCOMPARE(m_app->lastRange, QStringLiteral("latest"));
        QCOMPARE(m_app->lastCount, 250);
        QVERIFY(m_app->lastAttachments);
        QCOMPARE(m_app->lastSizeMb, 64);
        QVERIFY(!m_app->lastEncryptedText);

        // Text never asks for attachments, whatever the checkbox says.
        m_dialog->setProperty("format", QStringLiteral("text"));
        QVERIFY(QMetaObject::invokeMethod(save, "accepted"));
        QCOMPARE(m_app->lastFormat, QStringLiteral("text"));
        QVERIFY(!m_app->lastAttachments);
    }
};

QTEST_MAIN(ExportRoomDialogQmlTest)
#include "ExportRoomDialogQmlTest.moc"
