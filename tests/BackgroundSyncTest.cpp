
#include "app/SettingsManager.h"
#include "backdrop/ChatBackdropController.h"
#include "matrix/MockMatrixClient.h"
#include "matrix/RoomInfo.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace {

struct Read {
    QString scope;
    quint64 opId = 0;
};
struct Write {
    QString scope;
    int mode = -1;
    QString path;
    QString json;
    quint64 opId = 0;
};
struct Clear {
    QStringList known;
    quint64 opId = 0;
};

class FakeSyncClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsPersonalBackgroundSync() const override { return supports; }
    bool isLoggedIn() const override { return loggedIn; }
    bool initialSyncDone() const override { return synced; }
    QList<RoomInfo> rooms() const override { return roomList; }
    void readPersonalBackgrounds(const QString &scope, quint64 opId) override
    {
        reads.append({ scope, opId });
    }
    void downloadPersonalBackground(const QString &scope, const QString &expectedId,
                                    quint64 opId) override
    {
        downloads.append({ scope, opId });
        downloadIds.append(expectedId);
    }
    void writePersonalBackground(const QString &scope, int mode,
                                 const QString &localPath,
                                 const QString &requestedJson,
                                 quint64 opId) override
    {
        writes.append({ scope, mode, localPath, requestedJson, opId });
    }
    void clearAllPersonalBackgrounds(const QStringList &knownRooms,
                                     quint64 opId) override
    {
        clears.append({ knownRooms, opId });
    }

    QList<Write> writesOf(int mode) const
    {
        QList<Write> out;
        for (const Write &w : writes) {
            if (w.mode == mode)
                out.append(w);
        }
        return out;
    }

    bool supports = true;
    bool loggedIn = true;
    bool synced = true;
    QList<RoomInfo> roomList;
    QList<Read> reads;
    QList<Read> downloads;
    QStringList downloadIds;
    QList<Write> writes;
    QList<Clear> clears;
};

const QString kRoom = QStringLiteral("!room:example.org");
const QString kIdA = QStringLiteral("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
const QString kIdB = QStringLiteral("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
const QString kIdC = QStringLiteral("CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC");

constexpr int kUpload = 0;
constexpr int kPresentation = 1;
constexpr int kClearMode = 2;
constexpr int kSwitch = 3;

RoomInfo joined(const QString &id)
{
    RoomInfo r;
    r.id = id;
    r.name = QStringLiteral("Room %1").arg(id.mid(1, 4));
    r.membership = RoomInfo::Joined;
    return r;
}

QByteArray pictureBytes(const QColor &colour)
{
    QImage image(48, 32, QImage::Format_RGB32);
    image.fill(colour);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QVariantMap present(const QString &scope, const QString &id, int dim = 30)
{
    return QVariantMap{
        { QStringLiteral("scope"), scope },
        { QStringLiteral("state"), QStringLiteral("present") },
        { QStringLiteral("id"), id },
        { QStringLiteral("color"), QStringLiteral("#203040") },
        { QStringLiteral("presentation"),
          QVariantMap{ { QStringLiteral("dim"), dim },
                       { QStringLiteral("blur"), 0 },
                       { QStringLiteral("tint"), 25 },
                       { QStringLiteral("fit"), QStringLiteral("contain") },
                       { QStringLiteral("align"), QStringLiteral("top") } } },
    };
}

QVariantMap withState(const QString &scope, const QString &state)
{
    return QVariantMap{ { QStringLiteral("scope"), scope },
                        { QStringLiteral("state"), state } };
}

QVariantMap cleared(const QString &scope) { return withState(scope, QStringLiteral("cleared")); }

constexpr int kOn = 1;
constexpr int kOff = 0;

int filesIn(const QString &dir)
{
    return int(QDir(dir).entryList(QDir::Files).size());
}

QJsonObject storedStore()
{
    QSettings settings;
    return QJsonDocument::fromJson(
        settings.value(QStringLiteral("backdrop/personal")).toString().toUtf8()).object();
}

QJsonObject storedRecord(const QString &scope)
{
    const QJsonObject doc = storedStore();
    return scope.isEmpty()
        ? doc.value(QStringLiteral("default")).toObject()
        : doc.value(QStringLiteral("rooms")).toObject().value(scope).toObject();
}

QString stored(const QString &scope)
{
    return storedRecord(scope).value(QStringLiteral("remote")).toString();
}

QJsonObject requestOf(const Write &w)
{
    return QJsonDocument::fromJson(w.json.toUtf8()).object();
}

}

class BackgroundSyncTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_configHome;

    struct Rig {
        SettingsManager settings;
        FakeSyncClient client;
        ChatBackdropController backdrops;
        QTemporaryDir dir;
        explicit Rig(bool signedIn = true)
        {
            client.loggedIn = signedIn;
            client.roomList = { joined(kRoom) };
            backdrops.setStorageDirForTesting(dir.path());
            backdrops.setSettings(&settings);
            backdrops.setClient(&client);
        }
        void restart()
        {
            backdrops.setClient(nullptr);
            backdrops.setClient(&client);
        }
        void answerFullRead(const QVariantList &entries, int enabled = kOn)
        {
            QVERIFY(!client.reads.isEmpty());
            const Read r = client.reads.last();
            QCOMPARE(r.scope, QStringLiteral("*"));
            Q_EMIT client.personalBackgroundsRead(r.opId, r.scope, entries, {}, enabled);
        }
        void answerRead(const QString &scope, const QVariantList &entries,
                        int enabled = kOn)
        {
            QVERIFY(!client.reads.isEmpty());
            const Read r = client.reads.last();
            QCOMPARE(r.scope, scope);
            Q_EMIT client.personalBackgroundsRead(r.opId, r.scope, entries, {}, enabled);
        }
        void poke(const QString &scope, const QVariantList &entries, int enabled = kOn)
        {
            Q_EMIT client.personalBackgroundChanged(scope);
            answerRead(scope, entries, enabled);
        }
        void answerWrite(bool ok, const QVariantMap &entry, const QString &category = {})
        {
            QVERIFY(!client.writes.isEmpty());
            const Write w = client.writes.last();
            Q_EMIT client.personalBackgroundWritten(w.opId, w.scope, ok, entry, category);
        }
        void answerDownload(const Read &d, const QString &id, const QColor &colour)
        {
            answered.insert(d.opId);
            Q_EMIT client.personalBackgroundDownloaded(d.opId, d.scope, true,
                                                       present(d.scope, id),
                                                       pictureBytes(colour), QString());
        }
        void answerComparesDifferent()
        {
            const int n = int(client.downloads.size());
            for (int i = 0; i < n; ++i) {
                const Read d = client.downloads.at(i);
                if (answered.contains(d.opId))
                    continue;
                answered.insert(d.opId);
                answerDownload(d, client.downloadIds.at(i), Qt::yellow);
            }
        }
        QByteArray localBytes(const QString &scope) const
        {
            QFile file(QDir(dir.path()).filePath(
                storedRecord(scope).value(QStringLiteral("file")).toString()));
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        }
        QSet<quint64> answered;
        int answeredReads = 0;
        void setLocal(const QString &scope, const QColor &colour, double dim = 0.37)
        {
            QImage image(40, 30, QImage::Format_RGB32);
            image.fill(colour);
            QVERIFY(backdrops.prepareImageForTesting(image)
                        .value(QStringLiteral("ok")).toBool());
            QVERIFY(backdrops.setPersonal(scope, QVariantMap{
                { QStringLiteral("dim"), dim } }));
        }
        QString state() const
        {
            return backdrops.syncStatus().value(QStringLiteral("state")).toString();
        }
    };

    static void markNoticeSeen()
    {
        QSettings raw;
        raw.setValue(QStringLiteral("backdrop/syncNoticeAck"), true);
        raw.sync();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("background-sync-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void onByDefaultAndSilentWithoutABackend()
    {
        {
            Rig rig;
            QVERIFY(rig.backdrops.syncEnabled());
            QVERIFY(rig.backdrops.syncAvailable());
            QCOMPARE(rig.client.reads.size(), 1);
        }
        init();
        Rig rig;
        rig.client.supports = false;
        rig.restart();
        const int before = int(rig.client.reads.size());
        Q_EMIT rig.client.personalBackgroundChanged(QString());
        rig.backdrops.requestRoom(kRoom);
        QCOMPARE(rig.client.reads.size(), before);
        QCOMPARE(rig.state(), QStringLiteral("unavailable"));
    }

    void anotherDevicesChangesApplyLive()
    {
        Rig rig;
        rig.answerFullRead({});
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));

        rig.poke(QString(), { present(QString(), kIdA) });
        QCOMPARE(rig.client.downloads.size(), 1);
        rig.answerDownload(rig.client.downloads.last(), kIdA, Qt::darkBlue);
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-default"));
        QCOMPARE(filesIn(rig.dir.path()), 1);
        QCOMPARE(stored(QString()), kIdA);
        const QVariantMap mine = rig.backdrops.personalFor(QString());
        QCOMPARE(mine.value(QStringLiteral("fit")).toString(), QStringLiteral("contain"));
        QCOMPARE(mine.value(QStringLiteral("dim")).toDouble(), 0.30);

        rig.poke(QString(), { present(QString(), kIdA, 60) });
        QCOMPARE(rig.client.downloads.size(), 1);
        QCOMPARE(rig.backdrops.personalFor(QString()).value(QStringLiteral("dim")).toDouble(),
                 0.60);

        rig.poke(QString(), { present(QString(), kIdB) });
        QCOMPARE(rig.client.downloads.size(), 2);
        rig.answerDownload(rig.client.downloads.last(), kIdB, Qt::darkRed);
        QCOMPARE(stored(QString()), kIdB);
        QCOMPARE(filesIn(rig.dir.path()), 1);

        rig.poke(QString(), { cleared(QString()) });
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));
        QCOMPARE(filesIn(rig.dir.path()), 0);
        QVERIFY(rig.client.writes.isEmpty());
    }

    void absentOrInvalidKeepsTheLocalPictureAndBadBytesAreRefused()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(QString(), Qt::darkGreen);
        rig.answerWrite(true, present(QString(), kIdA));
        QCOMPARE(stored(QString()), kIdA);

        rig.poke(QString(), {});
        rig.poke(QString(), { withState(QString(), QStringLiteral("invalid")) });
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-default"));
        QCOMPARE(stored(QString()), kIdA);
        QVERIFY(rig.client.downloads.isEmpty());
        QCOMPARE(rig.client.writesOf(kUpload).size(), 1);

        rig.poke(QString(), { present(QString(), kIdB) });
        Q_EMIT rig.client.personalBackgroundDownloaded(
            rig.client.downloads.last().opId, QString(), true, present(QString(), kIdB),
            QByteArrayLiteral("<svg xmlns='http://www.w3.org/2000/svg'/>"), QString());
        QCOMPARE(stored(QString()), kIdA);
        QCOMPARE(rig.state(), QStringLiteral("failed"));
    }

    void earlierPicturesWaitForTheNoticeThenUploadOnce()
    {
        Rig rig(false);
        rig.setLocal(QString(), Qt::darkCyan);
        rig.setLocal(kRoom, Qt::darkMagenta);
        QVERIFY(rig.client.writes.isEmpty());
        rig.client.loggedIn = true;
        rig.restart();
        rig.answerFullRead({});
        QVERIFY(rig.backdrops.migrationNoticeNeeded());
        QVERIFY2(rig.client.writes.isEmpty(), "uploaded before the user was told");

        rig.backdrops.acknowledgeMigration(false);
        QVERIFY(!rig.backdrops.migrationNoticeNeeded());
        QCOMPARE(rig.client.writes.size(), 1);
        const Write first = rig.client.writes.last();
        QCOMPARE(first.scope, QString());
        QCOMPARE(first.mode, kUpload);
        QVERIFY(first.path.startsWith(rig.dir.path()));
        QVERIFY(QFileInfo::exists(first.path));
        QCOMPARE(requestOf(first).value(QStringLiteral("presentation")).toObject()
                     .value(QStringLiteral("dim")).toVariant().toString(),
                 QStringLiteral("37"));
        QVERIFY2(!first.json.contains(QRegularExpression(QStringLiteral("\\d\\.\\d"))),
                 qPrintable(first.json));
        QCOMPARE(rig.state(), QStringLiteral("working"));

        rig.answerWrite(true, present(QString(), kIdA));
        QCOMPARE(rig.client.writes.size(), 2);
        QCOMPARE(rig.client.writes.last().scope, kRoom);
        rig.answerWrite(true, present(kRoom, kIdB));
        QCOMPARE(stored(QString()), kIdA);
        QCOMPARE(stored(kRoom), kIdB);
        QCOMPARE(rig.state(), QStringLiteral("idle"));

        rig.restart();
        rig.answerFullRead({ present(QString(), kIdA), present(kRoom, kIdB) });
        QCOMPARE(rig.client.writes.size(), 2);
        QVERIFY(rig.client.downloads.isEmpty());
        QVERIFY(!rig.backdrops.migrationNoticeNeeded());
    }

    void theNoticeCanKeepPicturesOnThisDeviceOnly()
    {
        Rig rig(false);
        rig.setLocal(QString(), Qt::darkCyan);
        rig.client.loggedIn = true;
        rig.restart();
        rig.answerFullRead({});
        QVERIFY(rig.backdrops.migrationNoticeNeeded());
        rig.backdrops.acknowledgeMigration(true);
        QVERIFY(!rig.backdrops.syncEnabled());
        QCOMPARE(rig.client.writesOf(kUpload).size(), 0);
        QCOMPARE(rig.client.writesOf(kSwitch).size(), 1);
        QCOMPARE(requestOf(rig.client.writes.last()).value(QStringLiteral("enabled")).toBool(true),
                 false);
    }

    void theServersSwitchDecidesBeforeAnyUpload()
    {
        markNoticeSeen();
        Rig rig;
        rig.setLocal(QString(), Qt::darkBlue);
        QVERIFY2(rig.client.writes.isEmpty(), "uploaded before the switch was read");

        rig.answerFullRead({}, kOff);
        QVERIFY(!rig.backdrops.syncEnabled());
        QVERIFY(rig.client.writes.isEmpty());
        QCOMPARE(rig.state(), QStringLiteral("off"));
        rig.setLocal(kRoom, Qt::darkRed);
        QVERIFY(rig.client.writes.isEmpty());

        rig.poke(QString(), { cleared(QString()) });
        QVERIFY(rig.backdrops.syncEnabled());
        rig.answerFullRead({ cleared(QString()) });
        QVERIFY(!rig.client.writesOf(kUpload).isEmpty());
    }

    void aPresentationChangeNamesItsPicture()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkYellow);
        QVERIFY(rig.backdrops.setPersonal(kRoom, QVariantMap{
            { QStringLiteral("dim"), 0.5 }, { QStringLiteral("fit"), QStringLiteral("tile") } }));
        QCOMPARE(rig.client.writes.size(), 1);
        rig.answerWrite(true, present(kRoom, kIdA));
        QCOMPARE(rig.client.writes.size(), 2);
        const Write change = rig.client.writes.last();
        QCOMPARE(change.mode, kPresentation);
        QVERIFY(change.path.isEmpty());
        QCOMPARE(requestOf(change).value(QStringLiteral("expected_id")).toString(), kIdA);

        const int reads = int(rig.client.reads.size());
        rig.answerWrite(false, {}, QStringLiteral("changed"));
        QCOMPARE(rig.client.reads.size(), reads + 1);
        QCOMPARE(rig.client.reads.last().scope, kRoom);
        QVERIFY(rig.state() != QLatin1String("failed"));
        rig.answerRead(kRoom, { present(kRoom, kIdB) });
        QCOMPARE(rig.client.downloads.size(), 1);
        rig.answerDownload(rig.client.downloads.last(), kIdB, Qt::darkGreen);
        QCOMPARE(stored(kRoom), kIdB);
        QVERIFY(storedStore().value(QStringLiteral("pending")).toObject().isEmpty());
    }

    void aDifferentServerPictureIsKeptBothWays()
    {
        markNoticeSeen();
        Rig rig(false);
        rig.setLocal(QString(), Qt::darkCyan);
        rig.setLocal(kRoom, Qt::darkMagenta);
        rig.client.loggedIn = true;
        rig.restart();
        rig.answerFullRead({ present(QString(), kIdA), present(kRoom, kIdB) });
        QVERIFY2(rig.client.writes.isEmpty(), "uploaded over the other device's picture");
        QCOMPARE(rig.client.downloads.size(), 2);
        const QString defaultFile = storedRecord(QString()).value(QStringLiteral("file")).toString();
        rig.answerComparesDifferent();
        QCOMPARE(storedRecord(QString()).value(QStringLiteral("file")).toString(), defaultFile);
        QCOMPARE(filesIn(rig.dir.path()), 2);
        const QVariantList conflicts = rig.backdrops.syncConflicts();
        QCOMPARE(conflicts.size(), 2);
        QCOMPARE(rig.backdrops.syncUnsaved(), 2);

        rig.backdrops.resolveConflict(QString(), false);
        QCOMPARE(rig.client.downloads.size(), 3);
        rig.answerDownload(rig.client.downloads.last(), kIdA, Qt::darkBlue);
        QCOMPARE(stored(QString()), kIdA);
        rig.backdrops.resolveConflict(kRoom, true);
        QCOMPARE(rig.client.writes.size(), 1);
        QCOMPARE(rig.client.writes.last().mode, kUpload);
        rig.answerWrite(true, present(kRoom, kIdC));
        QCOMPARE(stored(kRoom), kIdC);
        QVERIFY(rig.backdrops.syncConflicts().isEmpty());
        QCOMPARE(rig.backdrops.syncUnsaved(), 0);
    }

    void owedChangesSurviveARestart()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkGray);
        rig.answerWrite(true, present(kRoom, kIdA));
        rig.backdrops.clearPersonal(kRoom);
        QCOMPARE(rig.client.writes.last().mode, kClearMode);
        rig.answerWrite(false, {}, QStringLiteral("network"));
        QCOMPARE(rig.state(), QStringLiteral("failed"));
        const int sent = int(rig.client.writes.size());

        rig.restart();
        rig.answerFullRead({ present(kRoom, kIdA) });
        QVERIFY2(rig.client.downloads.isEmpty(), "the removed picture came back");
        QCOMPARE(rig.client.writes.size(), sent + 1);
        const Write replay = rig.client.writes.last();
        QCOMPARE(replay.scope, kRoom);
        QCOMPARE(replay.mode, kClearMode);
        rig.answerWrite(true, cleared(kRoom));
        QVERIFY(storedStore().value(QStringLiteral("pending")).toObject().isEmpty());
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));
    }

    void downloadsAreQueuedBoundedAndNotRepeated()
    {
        Rig rig;
        QVariantList entries;
        for (int i = 0; i < 5; ++i)
            entries.append(present(QStringLiteral("!r%1:example.org").arg(i), kIdA));
        rig.answerFullRead(entries);
        QCOMPARE(rig.client.downloads.size(), 2);
        rig.answerDownload(rig.client.downloads.at(0), kIdA, Qt::red);
        QCOMPARE(rig.client.downloads.size(), 3);

        const Read failing = rig.client.downloads.at(1);
        Q_EMIT rig.client.personalBackgroundDownloaded(failing.opId, failing.scope, false,
                                                       {}, {}, QStringLiteral("network"));
        QCOMPARE(rig.client.downloads.size(), 4);
        rig.answerDownload(rig.client.downloads.at(2), kIdA, Qt::green);
        rig.answerDownload(rig.client.downloads.at(3), kIdA, Qt::blue);
        QCOMPARE(rig.client.downloads.size(), 5);
        const int asked = int(rig.client.downloads.size());
        rig.poke(failing.scope, { present(failing.scope, kIdA) });
        for (int i = asked; i < rig.client.downloads.size(); ++i)
            QVERIFY2(rig.client.downloads.at(i).scope != failing.scope,
                     "a failed picture was fetched again without a retry");
        rig.backdrops.retrySync();
        rig.answerRead(failing.scope, { present(failing.scope, kIdA) });
        bool again = false;
        for (int i = asked; i < rig.client.downloads.size(); ++i)
            again = again || rig.client.downloads.at(i).scope == failing.scope;
        QVERIFY(again);
    }

    void thePerRoomBoundIsCheckedBeforeDownloading()
    {
        QJsonObject rooms;
        for (int i = 0; i < ChatBackdropController::kMaxPersonalRooms; ++i) {
            rooms.insert(QStringLiteral("!full%1:example.org").arg(i),
                         QJsonObject{ { QStringLiteral("file"),
                                        QString(64, QLatin1Char('a')) + QStringLiteral(".jpg") },
                                      { QStringLiteral("remote"), kIdA } });
        }
        {
            QSettings raw;
            raw.setValue(QStringLiteral("backdrop/personal"),
                         QString::fromUtf8(QJsonDocument(QJsonObject{
                             { QStringLiteral("rooms"), rooms } }).toJson(QJsonDocument::Compact)));
            raw.sync();
        }
        Rig rig;
        rig.answerFullRead({ present(QStringLiteral("!one-more:example.org"), kIdB) });
        QVERIFY(rig.client.downloads.isEmpty());
        QCOMPARE(rig.backdrops.syncStatus().value(QStringLiteral("error")).toString(),
                 QStringLiteral("too_many"));
    }

    void aPartialRemovalIsReportedHonestly()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.backdrops.setSyncEnabled(false,true);
        QCOMPARE(rig.client.clears.size(), 1);
        Q_EMIT rig.client.personalBackgroundsCleared(rig.client.clears.last().opId,
                                                     false, 2, 0, 1, true);
        const QVariantMap status = rig.backdrops.syncStatus();
        QCOMPARE(status.value(QStringLiteral("removal")).toString(), QStringLiteral("partial"));
        QCOMPARE(status.value(QStringLiteral("removed")).toInt(), 2);
        QCOMPARE(status.value(QStringLiteral("removeFailed")).toInt(), 1);
    }

    void aFailedUploadIsReportedAndRetried()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(QString(), Qt::gray);
        QCOMPARE(rig.client.writes.size(), 1);
        rig.answerWrite(false, {}, QStringLiteral("rate_limited"));
        QVariantMap status = rig.backdrops.syncStatus();
        QCOMPARE(status.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        QCOMPARE(status.value(QStringLiteral("failed")).toInt(), 1);
        QCOMPARE(status.value(QStringLiteral("error")).toString(),
                 QStringLiteral("rate_limited"));
        QVERIFY(stored(QString()).isEmpty());
        rig.poke(QString(), {});
        QCOMPARE(rig.client.writes.size(), 1);

        rig.backdrops.retrySync();
        QCOMPARE(rig.client.writesOf(kUpload).size(), 2);
        rig.answerWrite(true, present(QString(), kIdA));
        QCOMPARE(rig.state(), QStringLiteral("idle"));
        QCOMPARE(stored(QString()), kIdA);
    }

    void removingAPictureRemovesTheServerCopy()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkYellow);
        rig.answerWrite(true, present(kRoom, kIdA));
        rig.backdrops.clearPersonal(kRoom);
        QCOMPARE(rig.client.writes.last().mode, kClearMode);
        QCOMPARE(rig.client.writes.last().scope, kRoom);
        rig.answerWrite(true, cleared(kRoom));
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));
    }

    void turningSyncOffKeepsOrRemovesTheServerCopies()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkGray);
        rig.answerWrite(true, present(kRoom, kIdA));

        rig.backdrops.setSyncEnabled(false,false);
        QVERIFY(!rig.backdrops.syncEnabled());
        QVERIFY(rig.client.clears.isEmpty());
        QCOMPARE(rig.client.writes.last().mode, kSwitch);
        rig.answerWrite(true, {});
        QVERIFY(stored(kRoom).isEmpty());
        QCOMPARE(storedRecord(kRoom).value(QStringLiteral("was")).toString(), kIdA);
        QCOMPARE(rig.state(), QStringLiteral("off"));
        const int reads = int(rig.client.reads.size());
        Q_EMIT rig.client.personalBackgroundChanged(kRoom);
        QCOMPARE(rig.client.reads.size(), reads);

        rig.backdrops.setSyncEnabled(false,true);
        QCOMPARE(rig.client.clears.size(), 1);
        QVERIFY(rig.client.clears.last().known.contains(kRoom));
        QVERIFY(stored(kRoom).isEmpty());
        QVERIFY(storedRecord(kRoom).value(QStringLiteral("was")).toString().isEmpty());
        QCOMPARE(rig.state(), QStringLiteral("removing"));
        Q_EMIT rig.client.personalBackgroundsCleared(rig.client.clears.last().opId,
                                                     true, 2, 0, 0, true);
        QVariantMap status = rig.backdrops.syncStatus();
        QCOMPARE(status.value(QStringLiteral("state")).toString(), QStringLiteral("off"));
        QCOMPARE(status.value(QStringLiteral("removal")).toString(), QStringLiteral("ok"));
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-room"));

        rig.backdrops.setSyncEnabled(true);
        QCOMPARE(rig.client.writes.last().mode, kSwitch);
        QCOMPARE(requestOf(rig.client.writes.last()).value(QStringLiteral("enabled")).toBool(),
                 true);
        rig.answerWrite(true, cleared(QString()));
        rig.answerFullRead({ cleared(kRoom) });
        QCOMPARE(rig.client.writes.last().scope, kRoom);
        QCOMPARE(rig.client.writes.last().mode, kUpload);
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-room"));
    }

    void removalWaitsForAnUploadInFlight()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(QString(), Qt::darkBlue);
        const quint64 upload = rig.client.writes.last().opId;
        rig.backdrops.setSyncEnabled(false, true);
        QVERIFY(rig.client.clears.isEmpty());
        QCOMPARE(rig.state(), QStringLiteral("removing"));
        Q_EMIT rig.client.personalBackgroundWritten(upload, QString(), true,
                                                    present(QString(), kIdA), {});
        QCOMPARE(rig.client.clears.size(), 1);
        QVERIFY(stored(QString()).isEmpty());
    }

    void aFreshSignInRestoresFromTheServer()
    {
        Rig rig;
        rig.answerFullRead({ present(QString(), kIdA), present(kRoom, kIdB) });
        QCOMPARE(rig.client.downloads.size(), 2);
        const QList<Read> asked = rig.client.downloads;
        for (const Read &d : asked)
            rig.answerDownload(d, d.scope.isEmpty() ? kIdA : kIdB,
                               d.scope.isEmpty() ? Qt::blue : Qt::red);
        QCOMPARE(filesIn(rig.dir.path()), 2);
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-room"));
        QCOMPARE(rig.backdrops.backdropFor(QStringLiteral("!other:example.org"))
                     .value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-default"));
        QVERIFY(rig.client.writes.isEmpty());

        Q_EMIT rig.client.loggedOut();
        Q_EMIT rig.client.personalBackgroundDownloaded(9999, QString(), true,
                                                       present(QString(), kIdB),
                                                       pictureBytes(Qt::green), QString());
        QCOMPARE(rig.backdrops.syncStatus().value(QStringLiteral("pending")).toInt(), 0);
    }

    void theSignOutDialogWarnsAboutUnsavedBackgrounds()
    {
        markNoticeSeen();
        Rig rig;
        rig.setLocal(QString(), Qt::darkBlue);
        QCOMPARE(rig.backdrops.syncUnsaved(), 1);
        rig.answerFullRead({});
        rig.answerWrite(true, present(QString(), kIdA));
        QCOMPARE(rig.backdrops.syncUnsaved(), 0);

        QFile file(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/AccountMenu.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(file.readAll());
        const qsizetype dialog = qml.indexOf(QStringLiteral("objectName: \"signOutConfirmDialog\""));
        const qsizetype warning = qml.indexOf(QStringLiteral("objectName: \"signOutUnsavedBackgrounds\""));
        QVERIFY(dialog > 0);
        QVERIFY2(warning > dialog, "the warning is not in the sign-out dialog");
        QVERIFY(qml.indexOf(QStringLiteral("app.backdrops.syncUnsaved"), warning) > warning);
        const qsizetype removals = qml.indexOf(QStringLiteral("objectName: \"signOutOwedRemovals\""));
        QVERIFY(removals > dialog);
        QVERIFY(qml.indexOf(QStringLiteral("app.backdrops.syncOwedRemovals"), removals) > removals);
    }

    void aFailedUploadNeverLendsItsPresentationAnEarlierId()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::red);
        rig.answerWrite(true, present(kRoom, kIdA));
        QCOMPARE(stored(kRoom), kIdA);

        rig.setLocal(kRoom, Qt::blue);
        QCOMPARE(rig.client.writes.last().mode, kUpload);
        QVERIFY(rig.backdrops.setPersonal(kRoom, QVariantMap{
            { QStringLiteral("dim"), 0.55 } }));
        rig.answerWrite(false, {}, QStringLiteral("network"));
        QVERIFY2(rig.client.writesOf(kPresentation).isEmpty(),
                 "a presentation was sent against the previous picture");
        QVERIFY(stored(kRoom).isEmpty());
        QVERIFY(storedStore().value(QStringLiteral("pending")).toObject().isEmpty());
        rig.backdrops.retrySync();
        const Write retry = rig.client.writes.last();
        QCOMPARE(retry.mode, kUpload);
        QCOMPARE(requestOf(retry).value(QStringLiteral("presentation")).toObject()
                     .value(QStringLiteral("dim")).toVariant().toString(),
                 QStringLiteral("55"));
    }

    void anotherDevicesRemovalNeverDeletesThisDevicesPictures()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(QString(), Qt::darkBlue);
        rig.answerWrite(true, present(QString(), kIdA));
        rig.setLocal(kRoom, Qt::darkRed);
        rig.answerWrite(true, present(kRoom, kIdB));
        QCOMPARE(filesIn(rig.dir.path()), 2);

        rig.poke(kRoom, { cleared(kRoom) }, kOff);
        QVERIFY(!rig.backdrops.syncEnabled());
        Q_EMIT rig.client.personalBackgroundChanged(QString());
        rig.answerRead(QString(), { cleared(QString()) }, kOff);
        QCOMPARE(filesIn(rig.dir.path()), 2);
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-room"));
        QCOMPARE(rig.backdrops.backdropFor(QStringLiteral("!other:example.org"))
                     .value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-default"));

        rig.backdrops.setSyncEnabled(true);
        rig.answerWrite(true, {});
        rig.answerFullRead({ cleared(QString()), cleared(kRoom) });
        rig.answerRead(kRoom, { cleared(kRoom) });
        QCOMPARE(filesIn(rig.dir.path()), 2);
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-room"));
        QVERIFY(storedRecord(kRoom).value(QStringLiteral("was")).toString().isEmpty());
    }

    void aPresentationEditWhileOffIsOwedNotAConflict()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkGreen);
        rig.answerWrite(true, present(kRoom, kIdA));
        rig.backdrops.setSyncEnabled(false, false);
        rig.answerWrite(true, {});
        const int sent = int(rig.client.writes.size());

        QVERIFY(rig.backdrops.setPersonal(kRoom, QVariantMap{
            { QStringLiteral("dim"), 0.7 } }));
        QCOMPARE(rig.client.writes.size(), sent);
        QCOMPARE(storedStore().value(QStringLiteral("pending")).toObject()
                     .value(kRoom).toString(), QStringLiteral("presentation"));

        rig.backdrops.setSyncEnabled(true);
        rig.answerWrite(true, {});
        rig.answerFullRead({ present(kRoom, kIdA) });
        QVERIFY2(rig.backdrops.syncConflicts().isEmpty(), "a false conflict");
        const Write owed = rig.client.writes.last();
        QCOMPARE(owed.mode, kPresentation);
        QCOMPARE(requestOf(owed).value(QStringLiteral("expected_id")).toString(), kIdA);
        rig.answerWrite(true, present(kRoom, kIdA, 70));
        QCOMPARE(stored(kRoom), kIdA);
        QVERIFY(storedStore().value(QStringLiteral("pending")).toObject().isEmpty());
    }

    void anOwedRemovalForALeftRoomIsDropped()
    {
        markNoticeSeen();
        {
            QSettings raw;
            raw.setValue(QStringLiteral("backdrop/personal"),
                         QStringLiteral("{\"pending\":{\"!gone:example.org\":\"clear\"}}"));
            raw.sync();
        }
        Rig rig;
        QCOMPARE(rig.backdrops.syncOwedRemovals(), 0);
        rig.answerFullRead({});
        QVERIFY(rig.client.writes.isEmpty());
        QVERIFY(storedStore().value(QStringLiteral("pending")).toObject().isEmpty());
    }

    void aRemovalIsRetriedAsARemoval()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::gray);
        rig.answerWrite(true, present(kRoom, kIdA));
        const int sent = int(rig.client.writes.size());
        rig.backdrops.setSyncEnabled(false, true);
        Q_EMIT rig.client.personalBackgroundsCleared(rig.client.clears.last().opId,
                                                     false, 0, 1, 0, true);
        QCOMPARE(rig.backdrops.syncStatus().value(QStringLiteral("removal")).toString(),
                 QStringLiteral("failed"));
        rig.backdrops.retryRemoval();
        QCOMPARE(rig.client.clears.size(), 2);
        QVERIFY(rig.client.clears.last().known.contains(kRoom));
        QCOMPARE(rig.client.writes.size(), sent);
        QVERIFY(!rig.backdrops.syncEnabled());
    }

    void aReadThatPredatesOurOwnWriteIsAskedAgain()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkGreen);
        QCOMPARE(rig.client.writes.size(), 1);
        rig.backdrops.requestRoom(kRoom);
        const Read early = rig.client.reads.last();
        QCOMPARE(early.scope, kRoom);
        rig.answerWrite(true, present(kRoom, kIdB));
        QCOMPARE(stored(kRoom), kIdB);
        const int readsBefore = int(rig.client.reads.size());
        Q_EMIT rig.client.personalBackgroundsRead(early.opId, kRoom,
                                                  { cleared(kRoom) }, {}, kOn);
        QCOMPARE(filesIn(rig.dir.path()), 1);
        QCOMPARE(rig.backdrops.personalFor(kRoom).isEmpty(), false);
        QCOMPARE(stored(kRoom), kIdB);
        QVERIFY(rig.client.reads.size() > readsBefore);
        QCOMPARE(rig.client.reads.last().scope, kRoom);
        QVERIFY(rig.client.reads.last().opId > early.opId);
        rig.answerRead(kRoom, { present(kRoom, kIdB) });
        QCOMPARE(filesIn(rig.dir.path()), 1);
        QVERIFY(rig.client.downloads.isEmpty());
    }

    void aReadIssuedBeforeOurSwitchWriteCannotUndoIt()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.backdrops.setSyncEnabled(false, false);
        QCOMPARE(rig.client.writesOf(kSwitch).size(), 1);
        Q_EMIT rig.client.personalBackgroundChanged(QString());
        const Read early = rig.client.reads.last();
        rig.answerWrite(true, {});
        QVERIFY(!rig.backdrops.syncEnabled());
        Q_EMIT rig.client.personalBackgroundsRead(early.opId, early.scope, {}, {}, kOn);
        QVERIFY2(!rig.backdrops.syncEnabled(), "a stale read turned sync back on");
        Q_EMIT rig.client.personalBackgroundChanged(QString());
        rig.answerRead(QString(), {}, kOn);
        QVERIFY(rig.backdrops.syncEnabled());
    }

    void aFullReadAsksTheServerForARoomWeWrote()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkGreen);
        rig.answerWrite(true, present(kRoom, kIdB));
        rig.backdrops.setSyncEnabled(false, false);
        rig.answerWrite(true, {});
        rig.backdrops.setSyncEnabled(true, false);
        rig.answerWrite(true, {});
        QCOMPARE(rig.client.reads.last().scope, QStringLiteral("*"));
        rig.answerFullRead({ cleared(kRoom) });
        QCOMPARE(rig.client.reads.last().scope, kRoom);
        rig.answerRead(kRoom, { present(kRoom, kIdB) });
        QCOMPARE(stored(kRoom), kIdB);
        QCOMPARE(filesIn(rig.dir.path()), 1);
    }

    void aDownloadNamesItsPictureAndAChangeReadsAgain()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({ present(QString(), kIdA) });
        QCOMPARE(rig.client.downloads.size(), 1);
        QCOMPARE(rig.client.downloadIds.last(), kIdA);
        const Read d = rig.client.downloads.last();
        Q_EMIT rig.client.personalBackgroundDownloaded(d.opId, d.scope, false, {}, {},
                                                       QStringLiteral("changed"));
        QCOMPARE(rig.state() == QStringLiteral("failed"), false);
        QCOMPARE(rig.client.reads.last().scope, QString());
        rig.answerRead(QString(), { present(QString(), kIdC) });
        QCOMPARE(rig.client.downloads.size(), 2);
        QCOMPARE(rig.client.downloadIds.last(), kIdC);
    }

    void aMigrationNeverOverwritesAnotherDevicesPicture()
    {
        markNoticeSeen();
        Rig rig(false);
        rig.setLocal(kRoom, Qt::darkCyan);
        rig.client.loggedIn = true;
        rig.restart();
        rig.answerFullRead({});
        QCOMPARE(rig.client.writesOf(kUpload).size(), 1);
        QCOMPARE(requestOf(rig.client.writes.last())
                     .value(QStringLiteral("only_if_empty")).toBool(), true);
        rig.answerWrite(false, {}, QStringLiteral("changed"));
        QCOMPARE(rig.client.reads.last().scope, kRoom);
        rig.answerRead(kRoom, { present(kRoom, kIdA) });
        const QString mine = storedRecord(kRoom).value(QStringLiteral("file")).toString();
        rig.answerComparesDifferent();
        QCOMPARE(rig.backdrops.syncConflicts().size(), 1);
        QCOMPARE(storedRecord(kRoom).value(QStringLiteral("file")).toString(), mine);
        QCOMPARE(filesIn(rig.dir.path()), 1);
        QCOMPARE(rig.state() == QStringLiteral("failed"), false);
        rig.setLocal(QString(), Qt::darkRed);
        QCOMPARE(requestOf(rig.client.writes.last())
                     .contains(QStringLiteral("only_if_empty")), false);
    }

    void theNoticeGoesWhenNothingWaitsForIt()
    {
        Rig rig(false);
        rig.setLocal(kRoom, Qt::darkCyan);
        rig.client.loggedIn = true;
        rig.restart();
        rig.answerFullRead({});
        QVERIFY(rig.backdrops.migrationNoticeNeeded());
        rig.poke(kRoom, { present(kRoom, kIdA) });
        rig.answerComparesDifferent();
        QCOMPARE(rig.backdrops.syncConflicts().size(), 1);
        QVERIFY2(!rig.backdrops.migrationNoticeNeeded(),
                 "the notice outlived the picture it was about");
        QCOMPARE(rig.client.writesOf(kUpload).size(), 0);
    }

    void anUnconfirmedMarkNeverDownloadsOverThisPicture()
    {
        markNoticeSeen();
        Rig rig(false);
        rig.setLocal(kRoom, Qt::darkCyan);
        rig.client.loggedIn = true;
        rig.restart();
        rig.answerFullRead({});
        QCOMPARE(rig.client.writesOf(kUpload).size(), 1);
        rig.answerWrite(true, present(kRoom, kIdA));
        QCOMPARE(stored(kRoom), kIdA);
        const QString mine = storedRecord(kRoom).value(QStringLiteral("file")).toString();

        rig.poke(kRoom, { present(kRoom, kIdB) });
        rig.answerComparesDifferent();
        QCOMPARE(rig.backdrops.syncConflicts().size(), 1);
        QCOMPARE(storedRecord(kRoom).value(QStringLiteral("file")).toString(), mine);
        QVERIFY2(QFileInfo::exists(QDir(rig.dir.path()).filePath(mine)),
                 "this device's picture was replaced");
        QCOMPARE(filesIn(rig.dir.path()), 1);
    }

    void aConfirmedMarkFollowsLaterChanges()
    {
        markNoticeSeen();
        Rig rig(false);
        rig.setLocal(kRoom, Qt::darkCyan);
        rig.client.loggedIn = true;
        rig.restart();
        rig.answerFullRead({});
        rig.answerWrite(true, present(kRoom, kIdA));
        rig.poke(kRoom, { present(kRoom, kIdA) });
        QVERIFY(!storedRecord(kRoom).contains(QStringLiteral("unconfirmed")));
        rig.poke(kRoom, { present(kRoom, kIdB) });
        QCOMPARE(rig.client.downloads.size(), 1);
        rig.answerDownload(rig.client.downloads.last(), kIdB, Qt::darkBlue);
        QCOMPARE(stored(kRoom), kIdB);
        QVERIFY(rig.backdrops.syncConflicts().isEmpty());
    }

    void theSamePictureComingBackIsNotAConflict()
    {
        markNoticeSeen();
        Rig rig(false);
        rig.setLocal(kRoom, Qt::darkCyan);
        rig.setLocal(QString(), Qt::darkGreen);
        rig.client.loggedIn = true;
        rig.restart();
        rig.answerFullRead({ present(QString(), kIdA), present(kRoom, kIdB) });
        QCOMPARE(rig.client.downloads.size(), 2);
        for (int i = 0; i < 2; ++i) {
            const Read d = rig.client.downloads.at(i);
            if (d.scope == kRoom) {
                Q_EMIT rig.client.personalBackgroundDownloaded(
                    d.opId, d.scope, true, present(d.scope, kIdB),
                    rig.localBytes(kRoom), QString());
            } else {
                QImage image(40, 30, QImage::Format_RGB32);
                image.fill(QColor(Qt::darkGreen));
                QByteArray png;
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                image.save(&buffer, "PNG");
                Q_EMIT rig.client.personalBackgroundDownloaded(
                    d.opId, d.scope, true, present(d.scope, kIdA), png, QString());
            }
        }
        QCOMPARE(stored(kRoom), kIdB);
        const QVariantList conflicts = rig.backdrops.syncConflicts();
        QCOMPARE(conflicts.size(), 1);
        QCOMPARE(conflicts.first().toMap().value(QStringLiteral("scope")).toString(), QString());
        QVERIFY(conflicts.first().toMap().value(QStringLiteral("looksSame")).toBool());
        QVERIFY(stored(QString()).isEmpty());
        QCOMPARE(filesIn(rig.dir.path()), 2);
        QVERIFY(rig.client.writes.isEmpty());
        Q_EMIT rig.settings.sessionChanged();
        QVERIFY(rig.backdrops.syncConflicts().first().toMap()
                    .value(QStringLiteral("looksSame")).toBool());
    }

    void anUnconfirmedMarkSurvivesAReload()
    {
        markNoticeSeen();
        Rig rig(false);
        rig.setLocal(kRoom, Qt::darkCyan);
        rig.client.loggedIn = true;
        rig.restart();
        rig.answerFullRead({});
        rig.answerWrite(true, present(kRoom, kIdA));
        const QString mine = storedRecord(kRoom).value(QStringLiteral("file")).toString();
        Q_EMIT rig.settings.sessionChanged();
        QVERIFY(storedRecord(kRoom).value(QStringLiteral("unconfirmed")).toBool());
        rig.poke(kRoom, { present(kRoom, kIdB) });
        rig.answerComparesDifferent();
        QCOMPARE(rig.backdrops.syncConflicts().size(), 1);
        QCOMPARE(storedRecord(kRoom).value(QStringLiteral("file")).toString(), mine);
        QVERIFY2(QFileInfo::exists(QDir(rig.dir.path()).filePath(mine)),
                 "this device's picture was replaced after a reload");
    }

    void lookalikeNoiseIsStillAConflict()
    {
        markNoticeSeen();
        Rig rig(false);
        auto noise = [](quint32 seed) {
            QImage image(1600, 1200, QImage::Format_RGB32);
            QRandomGenerator gen(seed);
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    image.setPixel(x, y, gen.generate() | 0xff000000u);
            return image;
        };
        QVERIFY(rig.backdrops.prepareImageForTesting(noise(1))
                    .value(QStringLiteral("ok")).toBool());
        QVERIFY(rig.backdrops.setPersonal(kRoom, QVariantMap{ { QStringLiteral("dim"), 0.37 } }));
        rig.client.loggedIn = true;
        rig.restart();
        rig.answerFullRead({ present(kRoom, kIdB) });
        QCOMPARE(rig.client.downloads.size(), 1);
        QByteArray other;
        QBuffer buffer(&other);
        buffer.open(QIODevice::WriteOnly);
        noise(2).save(&buffer, "PNG");
        const Read d = rig.client.downloads.last();
        Q_EMIT rig.client.personalBackgroundDownloaded(d.opId, d.scope, true,
                                                       present(kRoom, kIdB), other, QString());
        QCOMPARE(rig.backdrops.syncConflicts().size(), 1);
        QVERIFY(!rig.backdrops.syncConflicts().first().toMap()
                     .value(QStringLiteral("looksSame")).toBool());
        QVERIFY(stored(kRoom).isEmpty());
    }

    void startupNoticesWaitAndReadsAreCapped()
    {
        markNoticeSeen();
        Rig rig;
        QStringList rooms;
        for (int i = 0; i < 10; ++i) {
            rooms.append(QStringLiteral("!r%1:example.org").arg(i));
            rig.client.roomList.append(joined(rooms.last()));
        }
        QCOMPARE(rig.client.reads.size(), 1);
        for (const QString &room : std::as_const(rooms))
            Q_EMIT rig.client.personalBackgroundChanged(room);
        QCOMPARE(rig.client.reads.size(), 1);
        rig.answerFullRead({});
        auto inFlight = [&rig] {
            return int(rig.client.reads.size()) - 1 - rig.answeredReads;
        };
        QCOMPARE(inFlight(), 6);
        for (int round = 0; round < 10 && inFlight() > 0; ++round) {
            const Read r = rig.client.reads.at(1 + rig.answeredReads);
            ++rig.answeredReads;
            Q_EMIT rig.client.personalBackgroundsRead(r.opId, r.scope, {}, {}, kOn);
            QVERIFY(inFlight() <= 6);
        }
        QSet<QString> asked;
        for (int i = 1; i < rig.client.reads.size(); ++i)
            asked.insert(rig.client.reads.at(i).scope);
        QCOMPARE(asked.size(), 10);
    }

    void aWriteThatNeverAnswersTimesOut()
    {
        markNoticeSeen();
        Rig rig;
        rig.backdrops.setSyncTimeoutsForTesting(50, 50);
        rig.answerFullRead({});
        rig.setLocal(QString(), Qt::gray);
        const Write stuck = rig.client.writes.last();
        rig.setLocal(kRoom, Qt::darkGray);
        QCOMPARE(rig.client.writes.size(), 1);
        QTRY_COMPARE(rig.client.writes.size(), 2);
        QCOMPARE(rig.client.writes.last().scope, kRoom);
        QCOMPARE(rig.backdrops.syncStatus().value(QStringLiteral("error")).toString(),
                 QStringLiteral("timeout"));
        Q_EMIT rig.client.personalBackgroundWritten(stuck.opId, stuck.scope, true,
                                                    present(QString(), kIdA), {});
        QVERIFY(stored(QString()).isEmpty());
    }

    void theSettingsAndPromptContracts()
    {
        QFile file(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/ChatBackgroundSettings.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(file.readAll());
        QVERIFY(qml.contains(QStringLiteral("objectName: \"backgroundSyncSwitch\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"backgroundSyncOffDialog\"")));
        QVERIFY(qml.contains(QStringLiteral("objectName: \"backgroundSyncPrivacyHint\"")));
        QVERIFY(qml.contains(QStringLiteral("qsTr(\"Keep my backgrounds on my homeserver\")")));
        QVERIFY(qml.contains(QStringLiteral("administrator")));
        static const QRegularExpression offCall(
            QStringLiteral("setSyncEnabled\\(false,\\s*(true|false)\\)"));
        QCOMPARE(int(qml.count(offCall)), 2);
        const qsizetype dialog = qml.indexOf(QStringLiteral("id: syncOffDialog"));
        QVERIFY(dialog > 0);
        QVERIFY(qml.indexOf(offCall) > dialog);
        QVERIFY(qml.contains(QStringLiteral(".arg(s.removed).arg(s.removeFailed)")));
        QVERIFY(qml.contains(QStringLiteral("app.backdrops.retryRemoval()")));
        QVERIFY(qml.contains(QStringLiteral("devices keep their own copies until you")));
        QVERIFY(!qml.contains(QStringLiteral("Turn this on and off")));
        static const QRegularExpression bareText(QStringLiteral("\\btext:\\s*\"[A-Za-z]"));
        QVERIFY(!qml.contains(bareText));

        QFile prompt(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/BackgroundSyncPrompt.qml"));
        QVERIFY(prompt.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString p = QString::fromUtf8(prompt.readAll());
        for (const char *call : { "acknowledgeMigration(false)", "acknowledgeMigration(true)",
                                  "resolveConflict(root.conflict.scope, true)",
                                  "resolveConflict(root.conflict.scope, false)" })
            QVERIFY2(p.contains(QLatin1String(call)), call);
        QVERIFY(!p.contains(bareText));
        QVERIFY(p.contains(QStringLiteral("qsTr(\"Don't keep them on my homeserver\")")));
        QVERIFY(!p.contains(QStringLiteral("Keep on this device only")));
        QVERIFY(p.contains(QStringLiteral("administrator")));
        QVERIFY(p.contains(QStringLiteral("columns: root.noticeShown ? 1 : 2")));
        QFile main(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/Main.qml"));
        QVERIFY(main.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(QString::fromUtf8(main.readAll()).contains(QStringLiteral("BackgroundSyncPrompt {")));
    }
};

QTEST_MAIN(BackgroundSyncTest)
#include "BackgroundSyncTest.moc"
