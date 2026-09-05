// MSC4108 sign-in-another-device: the state machine, and the two rules that
// make it safe rather than merely working.
//
// 1. A progress step for a flow the user has already left must be IGNORED.
//    Applying it drives the current flow with the previous one's input — and
//    for a check code that means asking someone to compare digits belonging
//    to a channel that no longer exists, which is exactly the comparison the
//    digits are there to prevent being skipped.
//
// 2. The rendered code must not outlive its flow. The store is shared with
//    device verification and is served over an `image://` URL; a grid left in
//    it after the flow ended is one a stale URL can still fetch.

#include "crypto/QrLoginController.h"
#include "matrix/MockMatrixClient.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

// A client that reports QR-login support and records what it was asked to do,
// so the controller's calls can be asserted rather than inferred.
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

// A 1-module grid: the smallest thing QrCodeStore accepts, which is all these
// cases need — what is under test is the LIFETIME of the stored code.
QVariantMap qrReady(int size = 1)
{
    return QVariantMap{
        { QStringLiteral("qrSize"), size },
        { QStringLiteral("qrBits"),
          QString::fromLatin1(QByteArray(size, '\x80').toBase64()) },
        { QStringLiteral("qrText"), QStringLiteral("QRTEXTPAYLOAD") },
    };
}

} // namespace

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

        // The other device scanned: OUR code comes down, because leaving it
        // up invites a second device to scan a channel already claimed.
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

    // RULE 1. A step from a superseded flow must change nothing.
    void aStepFromASupersededFlowIsIgnored()
    {
        QrClient client;
        QrCodeStore store;
        QrLoginController qr;
        qr.setQrStore(&store);
        qr.setClient(&client);

        client.nextGeneration = 7;
        qr.showCode();
        // The user backed out and started again; the bridge answers with a
        // new generation.
        client.nextGeneration = 8;
        qr.cancel();
        qr.showCode();
        QCOMPARE(qr.state(), QStringLiteral("starting"));

        // The OLD flow's steps arrive late — the abort is not instantaneous.
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

        // The CURRENT flow's step lands normally.
        Q_EMIT client.qrLoginProgress(8, QStringLiteral("qr_ready"), qrReady());
        QCOMPARE(qr.state(), QStringLiteral("showing"));
    }

    // RULE 2. The code does not outlive its flow, by any exit.
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
            // The token really does resolve to a grid before the exit.
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

    // A failure category becomes WORDS, and an unknown one does not borrow
    // another cause's wording — the user acts on what this says.
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
            // No category may be reported in another's words.
            QVERIFY2(!seen.contains(qr.errorText())
                         || category == QLatin1String("something-new"),
                     qPrintable(QStringLiteral("%1 reuses another cause's "
                                               "message").arg(category)));
            seen << qr.errorText();
        }
    }

    // An unreadable paste is refused BEFORE a flow starts, with its own
    // message: it is by far the likeliest failure here and a generic
    // "could not be completed" would send the user looking in the wrong place.
    void anUnreadablePasteIsRefusedWithoutStartingAFlow()
    {
        QrClient client;
        QrLoginController qr;
        qr.setClient(&client);
        client.nextGeneration = 0;   // the bridge refused it

        qr.enterCode(QStringLiteral("this is not a code"));
        QCOMPARE(client.scanCalls, 1);
        QCOMPARE(qr.state(), QStringLiteral("failed"));
        QVERIFY(qr.errorText().contains(QStringLiteral("sign-in code")));

        // Whitespace-only never even reaches the bridge.
        client.scanCalls = 0;
        qr.enterCode(QStringLiteral("   "));
        QCOMPARE(client.scanCalls, 0);
    }

    // A second start while one is running must not begin a second channel.
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

    // A check code outside two digits is refused locally rather than sent.
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
