// v0.5.7: CacheStore plaintext-security regression tests.
//
// The invariant under test: decrypted encrypted-room plaintext is
// memory-only. Every CacheStore write path must refuse a TimelineEvent
// whose isEncrypted flag is set — even after the SDK decrypted it, even
// for local echoes, edits, or reply previews — and the unique marker
// body must never appear anywhere in the cache.sqlite file.

#include "storage/CacheStore.h"

#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

const QString kRoom = QStringLiteral("!secure:example.org");
const QString kMarker = QStringLiteral("LIGHTNING_CACHE_TEST_057");

TimelineEvent plainEvent(const QString &eventId, const QString &body)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.roomId = kRoom;
    e.sender = QStringLiteral("@alice:example.org");
    e.body = body;
    e.timestamp = QDateTime::fromMSecsSinceEpoch(1700000000000);
    return e;
}

TimelineEvent encryptedEvent(const QString &eventId, const QString &body)
{
    TimelineEvent e = plainEvent(eventId, body);
    e.isEncrypted = true;
    e.isDecrypted = true; // decrypted in memory — still must not persist
    return e;
}

} // namespace

class CacheStoreSecurityTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void plainEventRoundTrips();
    void encryptedDecryptedEventNotPersisted();
    void encryptedUndecryptableEventNotPersisted();
    void encryptedLocalEchoNotPersisted();
    void encryptedEditNotPersisted();
    void encryptedReplacementPurgesPlaceholder();
    void encryptedReplyPreviewNotPersisted();
    void encryptedMediaMetadataNotPersisted();
    void markerNeverAppearsInDatabaseFile();

private:
    QString databaseFilePath() const;

    QTemporaryDir m_dataDir;
    CacheStore *m_store = nullptr;
};

void CacheStoreSecurityTest::initTestCase()
{
    QVERIFY(m_dataDir.isValid());
    // Isolate the XDG data root so the test can never touch a real cache.
    qputenv("XDG_DATA_HOME", m_dataDir.path().toUtf8());
}

void CacheStoreSecurityTest::init()
{
    m_store = new CacheStore(this);
    QVERIFY(m_store->openFor(QStringLiteral("@cache-test:example.org")));
}

void CacheStoreSecurityTest::cleanup()
{
    m_store->clearAll();
    m_store->close();
    delete m_store;
    m_store = nullptr;
}

QString CacheStoreSecurityTest::databaseFilePath() const
{
    // Locate cache.sqlite under the isolated XDG data root.
    QDirIterator it(m_dataDir.path(), { QStringLiteral("cache.sqlite") },
                    QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
}

void CacheStoreSecurityTest::plainEventRoundTrips()
{
    // Sanity: the unencrypted path (HTTP/mock backends) is unchanged.
    m_store->appendEvent(plainEvent(QStringLiteral("$plain"),
                                    QStringLiteral("plain body")));
    const auto loaded = m_store->loadTimeline(kRoom);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().body, QStringLiteral("plain body"));
}

void CacheStoreSecurityTest::encryptedDecryptedEventNotPersisted()
{
    m_store->appendEvent(encryptedEvent(QStringLiteral("$enc1"), kMarker));
    QVERIFY(m_store->loadTimeline(kRoom).isEmpty());

    m_store->updateEvent(encryptedEvent(QStringLiteral("$enc1"), kMarker));
    QVERIFY(m_store->loadTimeline(kRoom).isEmpty());
}

void CacheStoreSecurityTest::encryptedUndecryptableEventNotPersisted()
{
    TimelineEvent e = encryptedEvent(QStringLiteral("$utd"),
                                     QStringLiteral("[unable to decrypt yet]"));
    e.isDecrypted = false;
    e.undecryptable = true;
    m_store->appendEvent(e);
    QVERIFY(m_store->loadTimeline(kRoom).isEmpty());
}

void CacheStoreSecurityTest::encryptedLocalEchoNotPersisted()
{
    TimelineEvent echo = encryptedEvent(QStringLiteral("$echo"), kMarker);
    echo.status = TimelineEvent::Sending;
    echo.isLocalEcho = true;
    echo.transactionId = QStringLiteral("txn1");
    m_store->appendEvent(echo);
    QVERIFY(m_store->loadTimeline(kRoom).isEmpty());
}

void CacheStoreSecurityTest::encryptedEditNotPersisted()
{
    TimelineEvent edit = encryptedEvent(QStringLiteral("$edited"), kMarker);
    edit.edited = true;
    m_store->updateEvent(edit);
    QVERIFY(m_store->loadTimeline(kRoom).isEmpty());
}

void CacheStoreSecurityTest::encryptedReplacementPurgesPlaceholder()
{
    // A plaintext placeholder row exists (e.g. from an older run)…
    m_store->appendEvent(plainEvent(QStringLiteral("local:txn9"),
                                    QStringLiteral("placeholder")));
    QCOMPARE(m_store->loadTimeline(kRoom).size(), 1);
    // …and its encrypted replacement must delete it rather than upsert
    // decrypted plaintext.
    m_store->replaceEventId(QStringLiteral("local:txn9"),
                            encryptedEvent(QStringLiteral("$real"), kMarker));
    QVERIFY(m_store->loadTimeline(kRoom).isEmpty());
}

void CacheStoreSecurityTest::encryptedReplyPreviewNotPersisted()
{
    TimelineEvent reply = encryptedEvent(QStringLiteral("$reply"), kMarker);
    reply.replyToEventId = QStringLiteral("$orig");
    reply.replyToSender = QStringLiteral("@bob:example.org");
    reply.replyToPreview = kMarker; // preview of an encrypted message
    m_store->appendEvent(reply);
    QVERIFY(m_store->loadTimeline(kRoom).isEmpty());
}

void CacheStoreSecurityTest::encryptedMediaMetadataNotPersisted()
{
    // v0.5.9: media rows from encrypted rooms — filename, media key, MIME —
    // must be refused exactly like text plaintext. Decrypted media bytes
    // themselves never reach CacheStore at all (the media bridge is
    // memory-only), so metadata is the only thing that could leak here.
    const QString marker = QStringLiteral("LIGHTNING_MEDIA_CACHE_TEST_059");
    TimelineEvent media = encryptedEvent(QStringLiteral("$encmedia"), marker);
    media.type = TimelineEvent::Image;
    media.mediaFilename = marker + QStringLiteral(".png");
    media.mediaKey = QStringLiteral("$encmedia");
    media.mediaMimetype = QStringLiteral("image/png");
    media.mediaSourceAvailable = true;
    m_store->appendEvent(media);
    QVERIFY(m_store->loadTimeline(kRoom).isEmpty());

    m_store->updateEvent(media);
    QVERIFY(m_store->loadTimeline(kRoom).isEmpty());

    // Raw-byte scan: the marker must not exist anywhere in cache.sqlite.
    m_store->close();
    const QString path = databaseFilePath();
    QVERIFY(!path.isEmpty());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray raw = file.readAll();
    QVERIFY(!raw.contains(marker.toUtf8()));
    QVERIFY(m_store->openFor(QStringLiteral("@cache-test:example.org")));
}

void CacheStoreSecurityTest::markerNeverAppearsInDatabaseFile()
{
    // After all the attempts above, scan the raw database bytes: the
    // unique marker plaintext must not exist anywhere in cache.sqlite.
    m_store->appendEvent(encryptedEvent(QStringLiteral("$final"), kMarker));
    RoomInfo encryptedRoom;
    encryptedRoom.id = kRoom;
    encryptedRoom.name = QStringLiteral("Encrypted room");
    encryptedRoom.encrypted = true;
    encryptedRoom.lastMessagePreview =
        QStringLiteral("LIGHTNING_ROOM_PREVIEW_CACHE_TEST_058");
    m_store->saveRoom(encryptedRoom);
    m_store->close();

    const QString dbPath = databaseFilePath();
    QVERIFY2(!dbPath.isEmpty(), "cache.sqlite not found under test data dir");
    QFile db(dbPath);
    QVERIFY(db.open(QIODevice::ReadOnly));
    const QByteArray raw = db.readAll();
    QVERIFY2(!raw.contains(kMarker.toUtf8()),
             "encrypted-room plaintext leaked into cache.sqlite");

    // Reopen for cleanup().
    QVERIFY(m_store->openFor(QStringLiteral("@cache-test:example.org")));
}

QTEST_GUILESS_MAIN(CacheStoreSecurityTest)
#include "CacheStoreSecurityTest.moc"
