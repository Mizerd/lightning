// The recovery-key route to verifying a session (GitHub #22), the stale
// crypto-backend text, and the platform-aware secret-store advice.
//
// RecoveryKeyEntry is driven against a fake `app` so the test sees exactly
// what the component hands the backend: the key goes out once and the field is
// wiped, only the instance that submitted reacts to the outcome, and nothing
// is marked trusted by the QML itself.

#include <QtTest/QtTest>

#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>

#include "crypto/CryptoManager.h"

namespace {

QString readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// Stand-in for the slice of AppController the component reads.
class FakeApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString sessionTrustState MEMBER trustState NOTIFY trustChanged)
public:
    QString trustState = QStringLiteral("Not verified");
    QStringList keys;
    int healthRefreshes = 0;
    int trustRefreshes = 0;

    Q_INVOKABLE void requestRecoverFromBackup(const QString &key)
    {
        keys << key;
    }
    Q_INVOKABLE void refreshCryptoHealth() { ++healthRefreshes; }
    Q_INVOKABLE void refreshSessionTrustState() { ++trustRefreshes; }

Q_SIGNALS:
    void recoveryStateChanged(const QString &state, const QString &message);
    void trustChanged();
};

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ApplicationWindow {
    width: 480
    height: 360
    visible: true
    ColumnLayout {
        anchors.fill: parent
        RecoveryKeyEntry {
            objectName: "first"
            Layout.fillWidth: true
            fieldObjectName: "firstField"
        }
        RecoveryKeyEntry {
            objectName: "second"
            Layout.fillWidth: true
            fieldObjectName: "secondField"
        }
    }
}
)QML";

} // namespace

class RecoveryKeyEntryTest : public QObject
{
    Q_OBJECT

    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    FakeApp m_app;

    QQuickItem *find(const QString &name) const
    {
        return m_root->findChild<QQuickItem *>(name);
    }
    QString statusOf(const QString &field) const
    {
        auto *l = find(field + QStringLiteral("Status"));
        return l ? l->property("text").toString() : QStringLiteral("<missing>");
    }
    void submit(const QString &field, const QString &text)
    {
        auto *tf = find(field);
        QVERIFY(tf);
        tf->setProperty("text", text);
        auto *btn = find(field + QStringLiteral("Submit"));
        QVERIFY(btn);
        QVERIFY(btn->property("enabled").toBool());
        QMetaObject::invokeMethod(btn, "clicked");
    }

private slots:
    void initTestCase()
    {
        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), &m_app);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("recoveryscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        QVERIFY(qobject_cast<QQuickWindow *>(m_root));
        QVERIFY(find(QStringLiteral("firstField")));
        QVERIFY(find(QStringLiteral("secondField")));
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
    }

    void submitHandsTheKeyOverOnceAndWipesTheField()
    {
        m_app.keys.clear();
        submit(QStringLiteral("firstField"), QStringLiteral("EsTx test key"));
        QCOMPARE(m_app.keys, QStringList{QStringLiteral("EsTx test key")});
        // The key does not stay in a QML property.
        QCOMPARE(find(QStringLiteral("firstField"))->property("text").toString(),
                 QString());
        QCOMPARE(statusOf(QStringLiteral("firstField")),
                 QStringLiteral("Recovery started"));
        // The sibling instance neither shows nor reacts to it.
        QCOMPARE(statusOf(QStringLiteral("secondField")), QString());
    }

    void onlyTheSubmittingInstanceReactsToTheOutcome()
    {
        m_app.healthRefreshes = 0;
        m_app.trustRefreshes = 0;
        m_app.trustState = QStringLiteral("Verified");
        Q_EMIT m_app.recoveryStateChanged(QStringLiteral("ok"), QString());
        // The first instance is the one still waiting from the previous case.
        QVERIFY(statusOf(QStringLiteral("firstField"))
                    .contains(QStringLiteral("now verified")));
        QCOMPARE(statusOf(QStringLiteral("secondField")), QString());
        // One refresh, not one per instance on the page.
        QCOMPARE(m_app.healthRefreshes, 1);
        QCOMPARE(m_app.trustRefreshes, 1);
    }

    void successDoesNotClaimVerifiedWhenTheSdkSaysOtherwise()
    {
        m_app.trustState = QStringLiteral("Not verified");
        submit(QStringLiteral("secondField"), QStringLiteral("another key"));
        Q_EMIT m_app.recoveryStateChanged(QStringLiteral("ok"), QString());
        const QString text = statusOf(QStringLiteral("secondField"));
        QVERIFY(text.startsWith(QStringLiteral("Recovery complete")));
        QVERIFY2(!text.contains(QStringLiteral("now verified")),
                 "the QML must not claim trust the SDK has not reported");
    }

    void failureShowsTheReasonAndNeverTheKey()
    {
        submit(QStringLiteral("firstField"), QStringLiteral("secret-ish"));
        Q_EMIT m_app.recoveryStateChanged(QStringLiteral("failed"),
                                        QStringLiteral("Wrong key."));
        const QString text = statusOf(QStringLiteral("firstField"));
        QVERIFY(text.contains(QStringLiteral("Wrong key.")));
        QVERIFY(!text.contains(QStringLiteral("secret-ish")));
        // The field is usable again for another attempt.
        QVERIFY(find(QStringLiteral("firstField"))->property("enabled").toBool());
    }

    void theComponentNeverLogsOrStoresTheKey()
    {
        const QString src = readFile(QStringLiteral(QML_DIR "/RecoveryKeyEntry.qml"));
        QVERIFY(!src.isEmpty());
        QVERIFY(!src.contains(QStringLiteral("console.")));
        QVERIFY(!src.contains(QStringLiteral("print(")));
        QVERIFY(!src.contains(QStringLiteral("Settings")));
        QVERIFY(src.contains(QStringLiteral("TextInput.Password")));
    }

    // GitHub #22: every start-verification affordance also offers the key.
    void everyVerifySurfaceOffersTheRecoveryKey()
    {
        const QString settings = readFile(QStringLiteral(QML_DIR "/SettingsScreen.qml"));
        QVERIFY(!settings.isEmpty());
        const int offers = settings.count(QStringLiteral("Use recovery key instead"));
        // The crypto-health card, the plain verify row, and the Sessions
        // card's own wiring (the TrustCard button text lives in TrustCard).
        QVERIFY2(offers >= 2, qPrintable(QString::number(offers)));
        QVERIFY(settings.contains(QStringLiteral("onRecoveryRequested")));
        QVERIFY(settings.contains(QStringLiteral("showRecoveryOption")));

        const QString trust = readFile(QStringLiteral(QML_DIR "/TrustCard.qml"));
        QVERIFY(trust.contains(QStringLiteral("Use recovery key instead")));
        QVERIFY(trust.contains(QStringLiteral("signal recoveryRequested()")));

        const QString prompt = readFile(QStringLiteral(QML_DIR "/VerifySessionPrompt.qml"));
        QVERIFY(prompt.contains(QStringLiteral("RecoveryKeyEntry {")));
        QVERIFY(prompt.contains(QStringLiteral("Use recovery key instead")));
    }

    // recover() verifies this session only when the self-signing key is in
    // secret storage, so no recovery-key button may promise verification.
    void recoveryKeyButtonsDoNotPromiseVerification()
    {
        int seen = 0;
        for (const char *file : { "SettingsScreen.qml", "VerifySessionPrompt.qml" }) {
            const QString src = readFile(QStringLiteral(QML_DIR "/")
                                         + QString::fromLatin1(file));
            QVERIFY(!src.isEmpty());
            QRegularExpression re(QStringLiteral("buttonText:\\s*qsTr\\(\"([^\"]*)\"\\)"));
            auto it = re.globalMatch(src);
            while (it.hasNext()) {
                const QString label = it.next().captured(1);
                ++seen;
                QVERIFY2(!label.contains(QStringLiteral("erify")), qPrintable(label));
            }
        }
        QVERIFY2(seen >= 4, "the scan found almost no recovery-key buttons");
    }

    void theRecoveryBoxSaysItAlsoVerifiesThisSession()
    {
        const QString settings = readFile(QStringLiteral(QML_DIR "/SettingsScreen.qml"));
        const QString flat = QString(settings).replace(QRegularExpression(
            QStringLiteral("\"\\s*\\+\\s*\\n\\s*\"")), QString());
        QVERIFY(flat.contains(QStringLiteral("and verifies this session")));
        // The Settings search anchor still resolves to a real field.
        QVERIFY(settings.contains(QStringLiteral("fieldObjectName: \"recoveryInputField\"")));
    }

    // Reported on macOS: "v0.5.6" and "not implemented yet" in a build that has
    // had both for a long time.
    void cryptoBackendTextCarriesNoVersionOrStaleClaims()
    {
        for (const char *name : { "rust", "http", "mock" }) {
            CryptoManager crypto;
            crypto.setBackendName(QString::fromLatin1(name));
            const QString d = crypto.backendDescription();
            QVERIFY2(!QRegularExpression(QStringLiteral("\\bv\\d+\\.\\d")).match(d).hasMatch(),
                     qPrintable(d));
            QVERIFY2(!d.contains(QStringLiteral("not implemented")), qPrintable(d));
            QVERIFY2(!crypto.statusString().contains(QStringLiteral("initial")),
                     qPrintable(crypto.statusString()));
        }
        const QString src = readFile(QStringLiteral(SRC_DIR "/crypto/CryptoManager.cpp"));
        QVERIFY(!src.isEmpty());
        QVERIFY(!src.contains(QStringLiteral("v0.5.6")));
        QVERIFY(!src.contains(QStringLiteral("not implemented")));
    }

    // The Keychain backend cannot run off a Mac, so its two data-loss rules are
    // pinned by scan: a failed write must not have deleted the old token first,
    // and a "success" it does not understand is inconclusive, not a miss.
    void keychainStoreNeverDeletesBeforeItWrites()
    {
        const QString src = readFile(QStringLiteral(SRC_DIR "/storage/MacKeychainStore.cpp"));
        QVERIFY(!src.isEmpty());
        const int begin = src.indexOf(QStringLiteral("bool MacKeychainStore::storeSecret("));
        const int end = src.indexOf(QStringLiteral("QString MacKeychainStore::readSecret("), begin);
        QVERIFY(begin > 0 && end > begin);
        const QString store = src.mid(begin, end - begin);
        QVERIFY(store.contains(QStringLiteral("SecItemUpdate(")));
        QVERIFY(store.contains(QStringLiteral("errSecItemNotFound")));
        QVERIFY2(!store.contains(QStringLiteral("SecItemDelete(")),
                 "storeSecret deletes before it adds: a failed add loses the token");
        const int read = src.indexOf(QStringLiteral("CFGetTypeID(result)"));
        QVERIFY(read > 0);
        QVERIFY(src.mid(read, 200).contains(QStringLiteral("m_lastReadFailed = true")));
    }

    void secretStoreAdviceIsPlatformAware()
    {
        const QString settings = readFile(QStringLiteral(QML_DIR "/SettingsScreen.qml"));
        const int adviceAt = settings.indexOf(QStringLiteral("Install a Secret Service provider"));
        QVERIFY(adviceAt > 0);
        // The Linux-only remedy sits behind a platform test.
        const QString before = settings.mid(qMax(0, adviceAt - 900), 900);
        QVERIFY(before.contains(QStringLiteral("Qt.platform.os")));
        QVERIFY(before.contains(QStringLiteral("\"osx\"")));
        // And the success line no longer names the Secret Service on a
        // platform that has none.
        QVERIFY(!settings.contains(
            QStringLiteral("stored via the system Secret Service")));
    }
};

QTEST_MAIN(RecoveryKeyEntryTest)
#include "RecoveryKeyEntryTest.moc"
