
#include "crypto/QrLoginController.h"
#include "matrix/MockMatrixClient.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class QrClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    bool supportsQrLogin() const override { return true; }

    quint64 nextGeneration = 7;
    int generateCalls = 0;
    int scanCalls = 0;
    int cancelCalls = 0;
    QString lastPayload;
    quint64 lastCheckGeneration = 0;
    int lastCheckCode = -1;

    quint64 qrLoginGenerate() override
    {
        ++generateCalls;
        return nextGeneration;
    }
    quint64 qrLoginScan(const QString &payload) override
    {
        ++scanCalls;
        lastPayload = payload;
        return nextGeneration;
    }
    void qrLoginSubmitCheckCode(quint64 generation, int code) override
    {
        lastCheckGeneration = generation;
        lastCheckCode = code;
    }
    void qrLoginCancel() override { ++cancelCalls; }
};

QVariantMap qrReady(int size = 1)
{
    return QVariantMap{
        { QStringLiteral("qrSize"), size },
        { QStringLiteral("qrBits"),
          QString::fromLatin1(QByteArray(size, '\x80').toBase64()) },
        { QStringLiteral("qrText"), QStringLiteral("QRTEXTPAYLOAD") },
    };
}

}

class QrLoginControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aFlowRunsThroughItsStatesAndPublishesACode()
    {
        QrClient client;
        QrCodeStore store;
        QrLoginController qr;
        qr.setQrStore(&store);
        qr.setClient(&client);
        QVERIFY(qr.available());
        QCOMPARE(qr.state(), QStringLiteral("idle"));

        qr.showCode();
        QCOMPARE(client.generateCalls, 1);
        QCOMPARE(qr.state(), QStringLiteral("starting"));

        Q_EMIT client.qrLoginProgress(7, QStringLiteral("qr_ready"), qrReady());
        QCOMPARE(qr.state(), QStringLiteral("showing"));
        QVERIFY2(!qr.qrSource().isEmpty(), "a rendered code must be servable");
        QCOMPARE(qr.qrText(), QStringLiteral("QRTEXTPAYLOAD"));

        Q_EMIT client.qrLoginProgress(7, QStringLiteral("check_code_needed"),
                                      {});
        QCOMPARE(qr.state(), QStringLiteral("waiting_for_code"));
        QVERIFY2(qr.qrSource().isEmpty(),
                 "the code must be withdrawn once the channel is claimed");
        QVERIFY(qr.qrText().isEmpty());

        qr.submitCheckCode(42);
        QCOMPARE(client.lastCheckGeneration, quint64(7));
        QCOMPARE(client.lastCheckCode, 42);

        Q_EMIT client.qrLoginProgress(
            7, QStringLiteral("waiting_for_auth"),
            QVariantMap{ { QStringLiteral("verificationUri"),
                           QStringLiteral("https://example.org/consent") } });
        QCOMPARE(qr.verificationUri(),
                 QStringLiteral("https://example.org/consent"));

        Q_EMIT client.qrLoginProgress(7, QStringLiteral("done"), {});
        QCOMPARE(qr.state(), QStringLiteral("done"));
        QVERIFY(!qr.busy());
    }

    void aStepFromASupersededFlowIsIgnored()
    {
        QrClient client;
        QrCodeStore store;
        QrLoginController qr;
        qr.setQrStore(&store);
        qr.setClient(&client);

        client.nextGeneration = 7;
        qr.showCode();
        client.nextGeneration = 8;
        qr.cancel();
        qr.showCode();
        QCOMPARE(qr.state(), QStringLiteral("starting"));

        Q_EMIT client.qrLoginProgress(7, QStringLiteral("qr_ready"), qrReady());
        QVERIFY2(qr.qrSource().isEmpty(),
                 "a superseded flow's code must not be displayed");
        QCOMPARE(qr.state(), QStringLiteral("starting"));

        Q_EMIT client.qrLoginProgress(
            7, QStringLiteral("check_code_shown"),
            QVariantMap{ { QStringLiteral("checkCode"), 11 } });
        QCOMPARE(qr.checkCode(), -1);

        Q_EMIT client.qrLoginProgress(
            7, QStringLiteral("failed"),
            QVariantMap{ { QStringLiteral("category"),
                           QStringLiteral("expired") } });
        QVERIFY2(qr.state() != QLatin1String("failed"),
                 "an old flow's failure must not fail the current one");

        Q_EMIT client.qrLoginProgress(8, QStringLiteral("qr_ready"), qrReady());
        QCOMPARE(qr.state(), QStringLiteral("showing"));
    }

    void everyExitClearsTheRenderedCode()
    {
        for (const QString &exit : { QStringLiteral("cancel"),
                                     QStringLiteral("logout"),
                                     QStringLiteral("failure"),
                                     QStringLiteral("done") }) {
            QrClient client;
            QrCodeStore store;
            QrLoginController qr;
            qr.setQrStore(&store);
            qr.setClient(&client);
            qr.showCode();
            Q_EMIT client.qrLoginProgress(7, QStringLiteral("qr_ready"),
                                          qrReady());
            const QString source = qr.qrSource();
            QVERIFY(!source.isEmpty());
            int modules = 0;
            QVERIFY(!store.gridFor(source.section(QLatin1Char('/'), -1),
                                   &modules).isEmpty());

            if (exit == QLatin1String("cancel")) {
                qr.cancel();
            } else if (exit == QLatin1String("logout")) {
                client.logout();
                QTest::qWait(50);
            } else if (exit == QLatin1String("failure")) {
                Q_EMIT client.qrLoginProgress(
                    7, QStringLiteral("failed"),
                    QVariantMap{ { QStringLiteral("category"),
                                   QStringLiteral("expired") } });
            } else {
                Q_EMIT client.qrLoginProgress(7, QStringLiteral("done"), {});
            }

            QVERIFY2(qr.qrSource().isEmpty(),
                     qPrintable(QStringLiteral("%1 left a code on screen")
                                    .arg(exit)));
            modules = 0;
            QVERIFY2(store.gridFor(source.section(QLatin1Char('/'), -1),
                                   &modules).isEmpty(),
                     qPrintable(QStringLiteral(
                         "%1 left a grid a stale URL could still fetch")
                                    .arg(exit)));
        }
    }

    void aLateStepAfterABareCancelIsDropped()
    {
        QrClient client;
        QrCodeStore store;
        QrLoginController qr;
        qr.setQrStore(&store);
        qr.setClient(&client);

        qr.showCode();
        qr.cancel();
        QCOMPARE(qr.state(), QStringLiteral("idle"));

        Q_EMIT client.qrLoginProgress(7, QStringLiteral("qr_ready"), qrReady());
        QVERIFY2(qr.qrSource().isEmpty(),
                 "a cancelled flow's code was put back on screen");
        QCOMPARE(qr.state(), QStringLiteral("idle"));

        qr.showCode();
        Q_EMIT client.qrLoginProgress(
            7, QStringLiteral("failed"),
            QVariantMap{ { QStringLiteral("category"),
                           QStringLiteral("expired") } });
        Q_EMIT client.qrLoginProgress(7, QStringLiteral("qr_ready"), qrReady());
        QVERIFY(qr.qrSource().isEmpty());
        QCOMPARE(qr.state(), QStringLiteral("failed"));
    }

    void cancellingDoesNotClearSomebodyElsesCode()
    {
        QrClient client;
        QrCodeStore store;
        QrLoginController qr;
        qr.setQrStore(&store);
        qr.setClient(&client);

        qr.showCode();
        Q_EMIT client.qrLoginProgress(7, QStringLiteral("qr_ready"), qrReady());
        QVERIFY(!qr.qrSource().isEmpty());

        const QString theirs = QStringLiteral("verification-token");
        QVERIFY(store.setCode(theirs, 1, QByteArray(1, '\x80')));

        qr.cancel();

        int modules = 0;
        QVERIFY2(!store.gridFor(theirs, &modules).isEmpty(),
                 "cancelling the sign-in cleared the verification code that "
                 "had replaced it in the shared store");
    }

    void everyFailureCategoryGetsItsOwnWords()
    {
        QStringList seen;
        for (const QString &category :
             { QStringLiteral("expired"), QStringLiteral("cancelled"),
               QStringLiteral("check_code"), QStringLiteral("unsupported"),
               QStringLiteral("something-new") }) {
            QrClient client;
            QrLoginController qr;
            qr.setClient(&client);
            qr.showCode();
            Q_EMIT client.qrLoginProgress(
                7, QStringLiteral("failed"),
                QVariantMap{ { QStringLiteral("category"), category } });
            QCOMPARE(qr.state(), QStringLiteral("failed"));
            QVERIFY(!qr.errorText().isEmpty());
            QVERIFY2(!seen.contains(qr.errorText())
                         || category == QLatin1String("something-new"),
                     qPrintable(QStringLiteral("%1 reuses another cause's "
                                               "message").arg(category)));
            seen << qr.errorText();
        }
    }

    void anUnreadablePasteIsRefusedWithoutStartingAFlow()
    {
        QrClient client;
        QrLoginController qr;
        qr.setClient(&client);
        client.nextGeneration = 0;

        qr.enterCode(QStringLiteral("this is not a code"));
        QCOMPARE(client.scanCalls, 1);
        QCOMPARE(qr.state(), QStringLiteral("failed"));
        QVERIFY(qr.errorText().contains(QStringLiteral("sign-in code")));

        client.scanCalls = 0;
        qr.enterCode(QStringLiteral("   "));
        QCOMPARE(client.scanCalls, 0);
    }

    void aFlowCannotBeStartedTwice()
    {
        QrClient client;
        QrLoginController qr;
        qr.setClient(&client);
        qr.showCode();
        QCOMPARE(client.generateCalls, 1);
        QVERIFY(qr.busy());
        qr.showCode();
        qr.enterCode(QStringLiteral("QRTEXT"));
        QCOMPARE(client.generateCalls, 1);
        QCOMPARE(client.scanCalls, 0);
    }

    void aCheckCodeOutsideTwoDigitsIsRefused()
    {
        QrClient client;
        QrLoginController qr;
        qr.setClient(&client);
        qr.showCode();
        qr.submitCheckCode(-1);
        QCOMPARE(client.lastCheckCode, -1);
        qr.submitCheckCode(100);
        QCOMPARE(client.lastCheckCode, -1);
        QVERIFY(!qr.errorText().isEmpty());
        qr.submitCheckCode(0);
        QCOMPARE(client.lastCheckCode, 0);
    }
};

QTEST_MAIN(QrLoginControllerTest)
#include "QrLoginControllerTest.moc"
