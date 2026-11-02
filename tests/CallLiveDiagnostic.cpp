#include "calls/RtcController.h"
#include "calls/SfuCallController.h"
#include "calls/SfuMediaEngine.h"
#include "matrix/RustSdkMatrixClient.h"
#include "app/SettingsManager.h"

#include <QSignalSpy>
#include <QVideoFrame>
#include <QVideoSink>
#include <QDir>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {
QString env(const char *name)
{
    return QString::fromLocal8Bit(qgetenv(name));
}
}

class CallLiveDiagnostic : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (env("LIGHTNING_LIVE_USER").isEmpty()
            || env("LIGHTNING_LIVE_ROOM").isEmpty()) {
            QSKIP("live call diagnostic: set LIGHTNING_LIVE_* to run");
        }
        m_storePath = env("LIGHTNING_LIVE_STORE");
        if (m_storePath.isEmpty()) {
            QVERIFY(m_store.isValid());
            m_storePath = m_store.path();
        }
        QDir().mkpath(m_storePath);
        qputenv("XDG_DATA_HOME", m_storePath.toUtf8());
        qputenv("XDG_CONFIG_HOME", m_storePath.toUtf8());
        QLoggingCategory::setFilterRules(
            QStringLiteral("lightning.calls*.debug=true\n"
                           "lightning.rtc*.debug=true\n"));
    }

    void aRealCallCarriesMedia()
    {
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        RtcController rtc;
        SfuCallController group;
        SfuMediaEngine engine;

        QString whyNot;
        if (!SfuMediaEngine::runtimeAvailable(&whyNot))
            QSKIP(qPrintable(QStringLiteral("no media runtime: %1").arg(whyNot)));
        engine.setTestSourceMode(true);

        client.setCallMediaCapable(true);
        rtc.setClient(&client);
        // The same encryption resolver the app installs. Without it every room
        // reads as encrypted (the fail-closed default), and an unencrypted
        // room's media would be dropped as undecryptable.
        rtc.setEncryptionResolver([&client](const QString &roomId) {
            if (roomId.isEmpty())
                return RtcController::RoomEncryption::Unknown;
            const RoomInfo room = client.roomInfo(roomId);
            if (room.id.isEmpty() || !room.encryptionKnown)
                return RtcController::RoomEncryption::Unknown;
            return room.encrypted ? RtcController::RoomEncryption::Yes
                                  : RtcController::RoomEncryption::No;
        });
        rtc.setMediaAvailable(true);
        rtc.setMediaEncryptionAvailable(true);
        group.setClient(&client);
        group.setRtcController(&rtc);
        group.setMediaEngine(&engine);

        QSignalSpy loggedIn(&client, &MatrixClient::loginSucceeded);
        const bool restoring = client.restoreSession();
        if (!restoring) {
            client.login(env("LIGHTNING_LIVE_HOMESERVER"),
                         env("LIGHTNING_LIVE_USER"),
                         env("LIGHTNING_LIVE_PASSWORD"));
        }
        QVERIFY2(loggedIn.wait(60000),
                 restoring ? "session restore failed" : "login failed");
        qInfo() << (restoring ? "restored a saved session (no login spent)"
                              : "logged in");
        client.startSync();
        qInfo() << "logged in; syncing";

        const QString room = env("LIGHTNING_LIVE_ROOM");
        QTest::qWait(15000);

        rtc.discover(room);
        rtc.refresh(room);
        QTRY_VERIFY_WITH_TIMEOUT(!rtc.focusUrlFor(room).isEmpty()
                                     || rtc.joinBlockReason(room)
                                            != QLatin1String("undiscovered"),
                                 30000);
        qInfo() << "join block =" << rtc.joinBlockReason(room)
                << "focus known =" << !rtc.focusUrlFor(room).isEmpty();

        QSignalSpy failed(&group, &SfuCallController::callFailed);
        const bool withVideo = env("LIGHTNING_LIVE_VIDEO") == QLatin1String("1");
        bool joined = false;
        for (int attempt = 0; attempt < 20 && !joined; ++attempt) {
            joined = group.join(room, withVideo);
            if (!joined)
                QTest::qWait(3000);
        }
        qInfo() << "join() returned" << joined << "lastError" << group.lastError();
        QVERIFY2(joined, qPrintable(group.lastError()));

        if (env("LIGHTNING_LIVE_SCREEN") == QLatin1String("1")) {
            QTRY_VERIFY_WITH_TIMEOUT(
                group.stateInt()
                    == static_cast<int>(SfuCallController::State::Connected),
                45000);
            const bool shared = group.startScreenShare(0);
            qInfo() << "screen share started:" << shared;
        }

        bool holdOk = false;
        const int hold = env("LIGHTNING_LIVE_HOLD_MS").toInt(&holdOk);
        QTest::qWait(holdOk && hold > 0 ? hold : 40000);
        qInfo() << "participants json:" << group.participants();

        QVideoSink sink;
        int framesRendered = 0;
        connect(&sink, &QVideoSink::videoFrameChanged, this,
                [&framesRendered](const QVideoFrame &) { ++framesRendered; });
        QString remote;
        for (const QVariant &row : group.participants()) {
            const QVariantMap p = row.toMap();
            if (!p.value(QStringLiteral("local")).toBool()) {
                remote = p.value(QStringLiteral("identity")).toString();
                break;
            }
        }
        if (!remote.isEmpty()) {
            group.attachVideoSink(remote, &sink);
            QTest::qWait(8000);
            group.detachSink(&sink);
            qInfo() << "frames RENDERED on an attached sink:" << framesRendered;
        }

        qInfo() << "=== RESULT ==="
                << "state=" << group.stateInt()
                << "participants=" << group.participantCount()
                << "encrypted=" << group.mediaEncrypted()
                << "framesEncrypted=" << engine.framesEncrypted()
                << "framesDecrypted=" << engine.framesDecrypted()
                << "framesDropped=" << engine.framesDropped()
                << "lastError=" << group.lastError();
        for (const QList<QVariant> &row : failed)
            qWarning() << "callFailed:" << row.value(0).toString();

        group.leave();
        QTest::qWait(2000);

        QVERIFY2(engine.framesEncrypted() > 0,
                 "our own media never reached the wire");
        if (!remote.isEmpty() && withVideo) {
            QVERIFY2(framesRendered > 0,
                     "remote video decrypted but never reached an attached "
                     "sink: the routing key the tile waits on does not match "
                     "the one the engine routes under");
        }
        if (group.participantCount() > 1) {
            QVERIFY2(engine.framesDecrypted() > 0,
                     qPrintable(QStringLiteral(
                         "media arrived from %1 peer(s) and none decrypted; "
                         "dropped=%2")
                                    .arg(group.participantCount() - 1)
                                    .arg(engine.framesDropped())));
        }
    }

private:
    QTemporaryDir m_store;
    QString m_storePath;
};

QTEST_MAIN(CallLiveDiagnostic)
#include "CallLiveDiagnostic.moc"
