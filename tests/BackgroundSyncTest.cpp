// Personal chat backgrounds kept on the homeserver (ChatBackdropController's
// sync half; the protocol half is rust/src/bgsync.rs).
//
// The controller is driven through a fake client that records what it is
// asked and answers with the signals the Rust bridge emits, so every case
// runs the real reconcile, migration, queue, switch and settings logic.

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

    // The writes of one mode, in order (the switch writes are mode 3).
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
    QStringList downloadIds;   // the picture each download named
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

// A real picture, as another device's upload decrypts to.
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

// The account-wide switch as a read reports it (its own account-data type).
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

} // namespace

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
        // A restart of this device: everything in memory goes, the settings
        // (the personal store and its owed writes) stay.
        void restart()
        {
            backdrops.setClient(nullptr);
            backdrops.setClient(&client);
        }
        // Answers the "*" read the controller sends on start.
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
        // Answers every download in flight that is only a comparison with a
        // DIFFERENT picture (another size and colour): a real conflict.
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
        // The bytes of this device's own stored picture for a scope.
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

    // The one-time notice is answered, as on any account that saw it once.
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

    // The setting is on by default; without a backend that can keep it,
    // nothing is ever asked.
    void onByDefaultAndSilentWithoutABackend()
    {
        {
            Rig rig;
            QVERIFY(rig.backdrops.syncEnabled());
            QVERIFY(rig.backdrops.syncAvailable());
            QCOMPARE(rig.client.reads.size(), 1);   // the start-up "*" read
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

    // Another device sets, changes and removes the every-room background:
    // each change arrives as a poke, is re-read, and lands without a restart.
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

        // The same copy again (our own echo): nothing is downloaded, the
        // presentation follows.
        rig.poke(QString(), { present(QString(), kIdA, 60) });
        QCOMPARE(rig.client.downloads.size(), 1);
        QCOMPARE(rig.backdrops.personalFor(QString()).value(QStringLiteral("dim")).toDouble(),
                 0.60);

        // Changed on another device: replaced, the old file gone.
        rig.poke(QString(), { present(QString(), kIdB) });
        QCOMPARE(rig.client.downloads.size(), 2);
        rig.answerDownload(rig.client.downloads.last(), kIdB, Qt::darkRed);
        QCOMPARE(stored(QString()), kIdB);
        QCOMPARE(filesIn(rig.dir.path()), 1);

        // Removed on another device.
        rig.poke(QString(), { cleared(QString()) });
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));
        QCOMPARE(filesIn(rig.dir.path()), 0);
        QVERIFY(rig.client.writes.isEmpty());   // nothing echoed back
    }

    // "Nothing stored" and "malformed" are not "removed": neither deletes a
    // local picture. A picture that arrives undecodable is refused, reported,
    // and never stored.
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
        QCOMPARE(rig.client.writesOf(kUpload).size(), 1);   // nothing re-sent over it

        rig.poke(QString(), { present(QString(), kIdB) });
        Q_EMIT rig.client.personalBackgroundDownloaded(
            rig.client.downloads.last().opId, QString(), true, present(QString(), kIdB),
            QByteArrayLiteral("<svg xmlns='http://www.w3.org/2000/svg'/>"), QString());
        QCOMPARE(stored(QString()), kIdA);   // kept, not replaced
        QCOMPARE(rig.state(), QStringLiteral("failed"));
    }

    // Review M7, half one: pictures chosen before sync existed are uploaded
    // only after the one-time notice. OK uploads each ONCE, with integer
    // percentages only (canonical JSON), and marks it; a later start sends
    // nothing. Fails on the first version, which uploaded at once.
    void earlierPicturesWaitForTheNoticeThenUploadOnce()
    {
        Rig rig(/*signedIn=*/false);
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
        // One write at a time, the default first.
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

    // "Keep on this device only" in the notice turns the switch off for the
    // account, uploading nothing.
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

    // Review M7, half two: the switch is ACCOUNT-WIDE. Nothing is written
    // before the server's switch is known; an opt-out made on another device
    // stops this one (no upload); turning it back on elsewhere resumes it.
    // Fails on the first version, which uploaded on the local setting alone.
    void theServersSwitchDecidesBeforeAnyUpload()
    {
        markNoticeSeen();
        Rig rig;
        // Not answered yet: a new picture waits.
        rig.setLocal(QString(), Qt::darkBlue);
        QVERIFY2(rig.client.writes.isEmpty(), "uploaded before the switch was read");

        rig.answerFullRead({}, kOff);
        QVERIFY(!rig.backdrops.syncEnabled());
        QVERIFY(rig.client.writes.isEmpty());
        QCOMPARE(rig.state(), QStringLiteral("off"));
        rig.setLocal(kRoom, Qt::darkRed);
        QVERIFY(rig.client.writes.isEmpty());

        // Turned on again on another device.
        rig.poke(QString(), { cleared(QString()) });
        QVERIFY(rig.backdrops.syncEnabled());
        rig.answerFullRead({ cleared(QString()) });
        QVERIFY(!rig.client.writesOf(kUpload).isEmpty());
    }

    // Review M1: a presentation change names the picture it is for. When the
    // server holds another one ("changed"), this device follows the server
    // (read, then download) instead of marking a picture it never saw, and
    // reports no failure. A change queued behind this device's own upload
    // names that upload. Fails on the first version: no expected id.
    void aPresentationChangeNamesItsPicture()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkYellow);
        // A change while the upload is still in flight goes after it.
        QVERIFY(rig.backdrops.setPersonal(kRoom, QVariantMap{
            { QStringLiteral("dim"), 0.5 }, { QStringLiteral("fit"), QStringLiteral("tile") } }));
        QCOMPARE(rig.client.writes.size(), 1);
        rig.answerWrite(true, present(kRoom, kIdA));
        QCOMPARE(rig.client.writes.size(), 2);
        const Write change = rig.client.writes.last();
        QCOMPARE(change.mode, kPresentation);
        QVERIFY(change.path.isEmpty());
        QCOMPARE(requestOf(change).value(QStringLiteral("expected_id")).toString(), kIdA);

        // Another device replaced the picture meanwhile.
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

    // Review M3: a picture only this device has and a DIFFERENT one on the
    // server are both kept. Nothing is uploaded over the server's and the
    // local one is not deleted; the user picks. Fails on the first version,
    // which migrated (uploaded) over the other device's picture.
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
        // Both synced pictures are fetched only to compare: they differ.
        QCOMPARE(rig.client.downloads.size(), 2);
        const QString defaultFile = storedRecord(QString()).value(QStringLiteral("file")).toString();
        rig.answerComparesDifferent();
        QCOMPARE(storedRecord(QString()).value(QStringLiteral("file")).toString(), defaultFile);
        QCOMPARE(filesIn(rig.dir.path()), 2);
        const QVariantList conflicts = rig.backdrops.syncConflicts();
        QCOMPARE(conflicts.size(), 2);
        QCOMPARE(rig.backdrops.syncUnsaved(), 2);

        // The synced one for every room...
        rig.backdrops.resolveConflict(QString(), false);
        QCOMPARE(rig.client.downloads.size(), 3);
        rig.answerDownload(rig.client.downloads.last(), kIdA, Qt::darkBlue);
        QCOMPARE(stored(QString()), kIdA);
        // ...and this device's one for the room.
        rig.backdrops.resolveConflict(kRoom, true);
        QCOMPARE(rig.client.writes.size(), 1);
        QCOMPARE(rig.client.writes.last().mode, kUpload);
        rig.answerWrite(true, present(kRoom, kIdC));
        QCOMPARE(stored(kRoom), kIdC);
        QVERIFY(rig.backdrops.syncConflicts().isEmpty());
        QCOMPARE(rig.backdrops.syncUnsaved(), 0);
    }

    // Review M4: a removal (or presentation change) that could not reach the
    // homeserver is OWED, and survives a restart: it is replayed, and until
    // it lands the server's old copy is neither downloaded nor followed.
    // Fails on the first version, which forgot it and downloaded the copy
    // back.
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
        QCOMPARE(rig.client.writes.size(), sent + 1);   // replayed
        const Write replay = rig.client.writes.last();
        QCOMPARE(replay.scope, kRoom);
        QCOMPARE(replay.mode, kClearMode);
        rig.answerWrite(true, cleared(kRoom));
        QVERIFY(storedStore().value(QStringLiteral("pending")).toObject().isEmpty());
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));
    }

    // Review M5: downloads run two at a time; the per-room bound is checked
    // BEFORE downloading; a picture that failed is not fetched again on
    // every poke, only after Retry. Fails on the first version, which
    // started every download at once.
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

        // A failed picture waits for Retry.
        const Read failing = rig.client.downloads.at(1);
        Q_EMIT rig.client.personalBackgroundDownloaded(failing.opId, failing.scope, false,
                                                       {}, {}, QStringLiteral("network"));
        // Never more than two at once, and the queue drains as they land.
        QCOMPARE(rig.client.downloads.size(), 4);
        rig.answerDownload(rig.client.downloads.at(2), kIdA, Qt::green);
        rig.answerDownload(rig.client.downloads.at(3), kIdA, Qt::blue);
        QCOMPARE(rig.client.downloads.size(), 5);   // the last one, alone
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
        // 64 rooms already have their own picture (files need not exist).
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

    // Review M6: a removal pass that skipped scopes (a room left, a newer
    // schema) is PARTIAL, counted, never "ok".
    void aPartialRemovalIsReportedHonestly()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.backdrops.setSyncEnabled(false, /*removeServerCopies=*/true);
        QCOMPARE(rig.client.clears.size(), 1);
        Q_EMIT rig.client.personalBackgroundsCleared(rig.client.clears.last().opId,
                                                     false, 2, 0, 1, true);
        const QVariantMap status = rig.backdrops.syncStatus();
        QCOMPARE(status.value(QStringLiteral("removal")).toString(), QStringLiteral("partial"));
        QCOMPARE(status.value(QStringLiteral("removed")).toInt(), 2);
        QCOMPARE(status.value(QStringLiteral("removeFailed")).toInt(), 1);
    }

    // A failed upload is reported honestly and retried on request, not
    // silently and not in a loop.
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

    // Removing a picture locally removes the server copy too, so another
    // device and the next sign-in do not bring it back.
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

    // Off asks the user (QML) and then either keeps the server copies (and
    // tells the homeserver the switch is off, for every device) or removes
    // them; removing drops every "mirrors the server" mark, so turning sync
    // on later uploads afresh instead of following a copy that is gone.
    void turningSyncOffKeepsOrRemovesTheServerCopies()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkGray);
        rig.answerWrite(true, present(kRoom, kIdA));

        rig.backdrops.setSyncEnabled(false, /*removeServerCopies=*/false);
        QVERIFY(!rig.backdrops.syncEnabled());
        QVERIFY(rig.client.clears.isEmpty());
        QCOMPARE(rig.client.writes.last().mode, kSwitch);
        rig.answerWrite(true, {});
        // Still the server's copy, but dormant: nothing can delete it now.
        QVERIFY(stored(kRoom).isEmpty());
        QCOMPARE(storedRecord(kRoom).value(QStringLiteral("was")).toString(), kIdA);
        QCOMPARE(rig.state(), QStringLiteral("off"));
        const int reads = int(rig.client.reads.size());
        Q_EMIT rig.client.personalBackgroundChanged(kRoom);
        QCOMPARE(rig.client.reads.size(), reads);

        rig.backdrops.setSyncEnabled(false, /*removeServerCopies=*/true);
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
        // Local-only again, so it is uploaded, not deleted by the "cleared".
        QCOMPARE(rig.client.writes.last().scope, kRoom);
        QCOMPARE(rig.client.writes.last().mode, kUpload);
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-room"));
    }

    // Removal asked while an upload is in flight waits for it, so the
    // upload cannot land after the removal and resurrect the copy.
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

    // After a sign-out this device holds nothing; signing in again reads the
    // server copies and restores them.
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

        // Sign-out: answers for the old session are dropped.
        Q_EMIT rig.client.loggedOut();
        Q_EMIT rig.client.personalBackgroundDownloaded(9999, QString(), true,
                                                       present(QString(), kIdB),
                                                       pictureBytes(Qt::green), QString());
        QCOMPARE(rig.backdrops.syncStatus().value(QStringLiteral("pending")).toInt(), 0);
    }

    // Review M8: what a sign-out would lose is counted, and the sign-out
    // dialog shows it.
    void theSignOutDialogWarnsAboutUnsavedBackgrounds()
    {
        markNoticeSeen();
        Rig rig;
        rig.setLocal(QString(), Qt::darkBlue);   // before the switch is known
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
        // Owed removals are worded on their own line.
        const qsizetype removals = qml.indexOf(QStringLiteral("objectName: \"signOutOwedRemovals\""));
        QVERIFY(removals > dialog);
        QVERIFY(qml.indexOf(QStringLiteral("app.backdrops.syncOwedRemovals"), removals) > removals);
    }

    // Review R1: a NEW picture whose upload fails must not lend a queued
    // presentation change the id of the PREVIOUS picture. bgsync-2 fell back
    // to the scope's last upload, sent the presentation against the old
    // picture's id, and then marked the new picture as that old copy.
    void aFailedUploadNeverLendsItsPresentationAnEarlierId()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::red);
        rig.answerWrite(true, present(kRoom, kIdA));
        QCOMPARE(stored(kRoom), kIdA);

        rig.setLocal(kRoom, Qt::blue);                      // a new picture
        QCOMPARE(rig.client.writes.last().mode, kUpload);
        QVERIFY(rig.backdrops.setPersonal(kRoom, QVariantMap{
            { QStringLiteral("dim"), 0.55 } }));            // queued behind it
        rig.answerWrite(false, {}, QStringLiteral("network"));
        QVERIFY2(rig.client.writesOf(kPresentation).isEmpty(),
                 "a presentation was sent against the previous picture");
        QVERIFY(stored(kRoom).isEmpty());
        QVERIFY(storedStore().value(QStringLiteral("pending")).toObject().isEmpty());
        // The retry uploads the new picture, with its current presentation.
        rig.backdrops.retrySync();
        const Write retry = rig.client.writes.last();
        QCOMPARE(retry.mode, kUpload);
        QCOMPARE(requestOf(retry).value(QStringLiteral("presentation")).toObject()
                     .value(QStringLiteral("dim")).toVariant().toString(),
                 QStringLiteral("55"));
    }

    // Review R3: another device's "Remove server copies" never deletes this
    // device's pictures, now or later. The switch arrives with every read,
    // so even a room read that answers "cleared" first is seen with the
    // switch off; the marks go dormant, and turning sync on again here
    // deletes nothing. Fails on bgsync-2, which deleted both pictures.
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

        // The room's "cleared" arrives first, together with the switch off.
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

        // Turned on again HERE: the cleared copies delete nothing.
        rig.backdrops.setSyncEnabled(true);
        rig.answerWrite(true, {});
        rig.answerFullRead({ cleared(QString()), cleared(kRoom) });
        // The room was written this session, so the full read's (store)
        // answer for it is asked of the server again.
        rig.answerRead(kRoom, { cleared(kRoom) });
        QCOMPARE(filesIn(rig.dir.path()), 2);
        QCOMPARE(rig.backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("personal-room"));
        QVERIFY(storedRecord(kRoom).value(QStringLiteral("was")).toString().isEmpty());
    }

    // Review R4: a presentation edit made while sync is off keeps the
    // picture's (dormant) mark and is OWED: turning sync on sends it for that
    // picture, with no false conflict. bgsync-2 dropped the mark, and the
    // same picture on the server then came back as a "conflict".
    void aPresentationEditWhileOffIsOwedNotAConflict()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkGreen);
        rig.answerWrite(true, present(kRoom, kIdA));
        rig.backdrops.setSyncEnabled(false, false);
        rig.answerWrite(true, {});                    // the switch write
        const int sent = int(rig.client.writes.size());

        QVERIFY(rig.backdrops.setPersonal(kRoom, QVariantMap{
            { QStringLiteral("dim"), 0.7 } }));
        QCOMPARE(rig.client.writes.size(), sent);    // off: nothing sent
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

    // A removal owed for a room this account has left can never happen: it
    // is dropped, rather than replayed for ever and pinning the sign-out
    // warning.
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

    // A removal pass that did not finish is retried as a REMOVAL, without
    // turning sync on (which would upload this device's pictures).
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
        QCOMPARE(rig.client.writes.size(), sent);   // nothing uploaded, no switch
        QVERIFY(!rig.backdrops.syncEnabled());
    }

    // Live, 2026-10-07: a read can be answered from a moment before this
    // device's own write landed. A read issued while the write was in flight
    // and answered after it must not be acted on for that scope: a stale
    // "cleared" deleted the picture the user had just chosen. It is asked
    // again instead. Fails without the op-ordering guard.
    void aReadThatPredatesOurOwnWriteIsAskedAgain()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkGreen);
        QCOMPARE(rig.client.writes.size(), 1);
        // The room is opened while the upload is in flight (its first read);
        // the server answers that read before the upload's PUT lands.
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
        // ...and asked of the server again, which is then believed.
        QVERIFY(rig.client.reads.size() > readsBefore);
        QCOMPARE(rig.client.reads.last().scope, kRoom);
        QVERIFY(rig.client.reads.last().opId > early.opId);
        rig.answerRead(kRoom, { present(kRoom, kIdB) });
        QCOMPARE(filesIn(rig.dir.path()), 1);
        QVERIFY(rig.client.downloads.isEmpty());
    }

    // The same for the switch: a read issued before this device turned sync
    // off, answered after, still says "on" and must not turn it back on.
    // Fails without the guard.
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
        // A read issued after the write is believed (another device's change).
        Q_EMIT rig.client.personalBackgroundChanged(QString());
        rig.answerRead(QString(), {}, kOn);
        QVERIFY(rig.backdrops.syncEnabled());
    }

    // A full read takes every room from the store, which lags this device's
    // own writes until sync echoes them: a room written this session is
    // asked of the server instead. Fails without the guard (the stale
    // "cleared" dropped the mark and left the picture local-only).
    void aFullReadAsksTheServerForARoomWeWrote()
    {
        markNoticeSeen();
        Rig rig;
        rig.answerFullRead({});
        rig.setLocal(kRoom, Qt::darkGreen);
        rig.answerWrite(true, present(kRoom, kIdB));
        // Off and on again here (the live sequence): turning it on reads
        // everything again.
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

    // A download names the picture the read reported; one that found
    // another picture is not a failure of that picture, it reads again.
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

    // Live, 2026-10-07: after "Remove server copies" every device keeps its
    // pictures local-only, and turning sync on again migrates them on every
    // device AT ONCE. Each upload replaced the one before it and each loser
    // downloaded the winner's over its own, unasked. A migration now fills
    // only an empty place; one another device filled first is a conflict.
    // Fails on the plain upload.
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
        // Another device got there first.
        rig.answerWrite(false, {}, QStringLiteral("changed"));
        QCOMPARE(rig.client.reads.last().scope, kRoom);
        rig.answerRead(kRoom, { present(kRoom, kIdA) });
        const QString mine = storedRecord(kRoom).value(QStringLiteral("file")).toString();
        rig.answerComparesDifferent();
        QCOMPARE(rig.backdrops.syncConflicts().size(), 1);
        QCOMPARE(storedRecord(kRoom).value(QStringLiteral("file")).toString(), mine);
        QCOMPARE(filesIn(rig.dir.path()), 1);
        QCOMPARE(rig.state() == QStringLiteral("failed"), false);
        // A picture the user picks now is theirs to place: no such limit.
        rig.setLocal(QString(), Qt::darkRed);
        QCOMPARE(requestOf(rig.client.writes.last())
                     .contains(QStringLiteral("only_if_empty")), false);
    }

    // Live, 2026-10-07: the notice stayed up after its only picture had
    // turned into a conflict (the other device had one for the same place),
    // over a card that already asked the real question. It speaks only while
    // a picture still waits for it. Fails on the latch alone.
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

    // Review N2: "only if empty" is a check and a PUT, two requests. Two
    // devices can both find the place empty and both write it; the one that
    // wrote first then reads the other's picture. Its own mark is not trusted
    // until a read confirms it, so that is a conflict, never a download over
    // this device's picture (whose file the orphan sweep would then delete).
    // Fails while the migration's mark is trusted like any other.
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

        // The other device's PUT landed after ours.
        rig.poke(kRoom, { present(kRoom, kIdB) });
        rig.answerComparesDifferent();
        QCOMPARE(rig.backdrops.syncConflicts().size(), 1);
        QCOMPARE(storedRecord(kRoom).value(QStringLiteral("file")).toString(), mine);
        QVERIFY2(QFileInfo::exists(QDir(rig.dir.path()).filePath(mine)),
                 "this device's picture was replaced");
        QCOMPARE(filesIn(rig.dir.path()), 1);
    }

    // ...and once a read shows the server holds our upload, the mark is an
    // ordinary one: a later change on another device is followed.
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

    // Review LOW (a): re-enabling after a removal made every place a
    // "conflict" between two copies of ONE picture (4 identical cards live).
    // The synced picture is compared first: the same bytes, the file this one
    // was made from, or the same pixels adopt it with no card. Fails while
    // every different id is asked about.
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
                // The very bytes this device holds.
                Q_EMIT rig.client.personalBackgroundDownloaded(
                    d.opId, d.scope, true, present(d.scope, kIdB),
                    rig.localBytes(kRoom), QString());
            } else {
                // Another encoding (PNG) of the same pixels.
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
        // The same bytes: adopted, no card.
        QCOMPARE(stored(kRoom), kIdB);
        // The same pixels in other bytes: still a card (review-4 LOW 1),
        // with the synced one pre-selected; nothing adopted or replaced.
        const QVariantList conflicts = rig.backdrops.syncConflicts();
        QCOMPARE(conflicts.size(), 1);
        QCOMPARE(conflicts.first().toMap().value(QStringLiteral("scope")).toString(), QString());
        QVERIFY(conflicts.first().toMap().value(QStringLiteral("looksSame")).toBool());
        QVERIFY(stored(QString()).isEmpty());
        QCOMPARE(filesIn(rig.dir.path()), 2);
        QVERIFY(rig.client.writes.isEmpty());
        // ...and the hint survives a reload.
        Q_EMIT rig.settings.sessionChanged();
        QVERIFY(rig.backdrops.syncConflicts().first().toMap()
                    .value(QStringLiteral("looksSame")).toBool());
    }

    // Review F2: a reload (start-up, or sessionChanged for the same account)
    // read the store back through cleanPersonalRecord, which dropped
    // `unconfirmed`: the mark was silently confirmed and the N2 race was
    // back. Fails while the flag is not kept.
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
        Q_EMIT rig.settings.sessionChanged();   // the same account: a reload
        QVERIFY(storedRecord(kRoom).value(QStringLiteral("unconfirmed")).toBool());
        rig.poke(kRoom, { present(kRoom, kIdB) });
        rig.answerComparesDifferent();
        QCOMPARE(rig.backdrops.syncConflicts().size(), 1);
        QCOMPARE(storedRecord(kRoom).value(QStringLiteral("file")).toString(), mine);
        QVERIFY2(QFileInfo::exists(QDir(rig.dir.path()).filePath(mine)),
                 "this device's picture was replaced after a reload");
    }

    // Two DIFFERENT pictures that look alike once shrunk (random noise
    // averages to grey) are a conflict, not "the same": live, 2026-10-07,
    // a 32x32 comparison adopted another device's noise picture as this one.
    void lookalikeNoiseIsStillAConflict()
    {
        markNoticeSeen();
        Rig rig(false);
        auto noise = [](quint32 seed) {
            // Large enough that a 32x32 average is near-flat grey.
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

    // Review LOW (b): initial sync pokes every room at once, and each poke
    // was a read. Room notices wait for the start-up full read; one that
    // arrives while it is out is read after it; at most six single reads are
    // in flight, the rest queued, none dropped.
    void startupNoticesWaitAndReadsAreCapped()
    {
        markNoticeSeen();
        Rig rig;
        QStringList rooms;
        for (int i = 0; i < 10; ++i) {
            rooms.append(QStringLiteral("!r%1:example.org").arg(i));
            rig.client.roomList.append(joined(rooms.last()));
        }
        QCOMPARE(rig.client.reads.size(), 1);   // the full read, unanswered
        for (const QString &room : std::as_const(rooms))
            Q_EMIT rig.client.personalBackgroundChanged(room);
        QCOMPARE(rig.client.reads.size(), 1);
        rig.answerFullRead({});
        // Read after it, six at a time.
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

    // A write whose answer never comes is failed after a bound, and the next
    // queued write goes; its late answer is dropped.
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
        QVERIFY(stored(QString()).isEmpty());   // the late answer was dropped
    }

    // The Settings contract: one switch, a privacy sentence that names the
    // administrator, and turning it OFF always goes through the question
    // about the server copies. The corner prompt answers both one-time
    // questions both ways.
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
        // The partial removal names both counts; a removal is retried as a
        // removal; other devices keep their pictures, and the copy says so.
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
        // Off is account-wide, and the notice says where the key lives.
        QVERIFY(p.contains(QStringLiteral("qsTr(\"Don't keep them on my homeserver\")")));
        QVERIFY(!p.contains(QStringLiteral("Keep on this device only")));
        QVERIFY(p.contains(QStringLiteral("administrator")));
        // The notice's long second label is not squeezed beside the first:
        // its buttons stack (seen clipped live in a two-column row).
        QVERIFY(p.contains(QStringLiteral("columns: root.noticeShown ? 1 : 2")));
        QFile main(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/Main.qml"));
        QVERIFY(main.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(QString::fromUtf8(main.readAll()).contains(QStringLiteral("BackgroundSyncPrompt {")));
    }
};

QTEST_MAIN(BackgroundSyncTest)
#include "BackgroundSyncTest.moc"
