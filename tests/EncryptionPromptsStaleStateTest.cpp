// C56 round 1: state a closed Settings page must not keep.
//
//  * RecoveryKeyEntry: a failed or finished attempt's answer is retired by the
//    first edit, by the page being hidden and by a sign-out / sign-in, instead
//    of waiting for the next visit (and for a different session).
//  * CrossSigningSetupCard: an armed "Replace identity" confirmation and a
//    typed account password / recovery key are cleared when the card is hidden.
//
// Both components run against fake `app` objects exposing only what they read.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

namespace {

class FakeAccounts : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString activeUserId MEMBER userId NOTIFY changed)
public:
    QString userId;
Q_SIGNALS:
    void changed();
};

class FakeHealth : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool cryptoSupported MEMBER supported NOTIFY changed)
    Q_PROPERTY(QString crossSigningSetup MEMBER setup NOTIFY changed)
public:
    bool supported = true;
    QString setup = QStringLiteral("keys_missing");
Q_SIGNALS:
    void changed();
};

class FakeBackup : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString lastAction MEMBER lastAction NOTIFY changed)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY changed)
    Q_PROPERTY(QString errorCategory MEMBER errorCategory NOTIFY changed)
    Q_PROPERTY(QString recoveryKey MEMBER recoveryKey NOTIFY changed)
    Q_PROPERTY(QString notice MEMBER notice NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
    Q_PROPERTY(bool passwordRequired MEMBER passwordRequired NOTIFY changed)
    Q_PROPERTY(bool wrongPassword MEMBER wrongPassword NOTIFY changed)
    Q_PROPERTY(QString approvalUrl MEMBER approvalUrl NOTIFY changed)
    Q_PROPERTY(bool cancelling MEMBER cancelling NOTIFY changed)
public:
    QString lastAction, errorCategory, recoveryKey, notice, error, approvalUrl;
    bool busy = false, passwordRequired = false, wrongPassword = false,
         cancelling = false;
    Q_INVOKABLE void runAction(const QString &) {}
    Q_INVOKABLE void runCrossSigning(const QString &, const QString &, bool) {}
    Q_INVOKABLE void submitPassword(const QString &) {}
    Q_INVOKABLE void cancelAuth() {}
    Q_INVOKABLE void dismissRecoveryKey() {}
Q_SIGNALS:
    void changed();
};

class FakeApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString sessionTrustState MEMBER trustState NOTIFY changed)
    Q_PROPERTY(QObject *accounts READ accountsObject CONSTANT)
    Q_PROPERTY(QObject *cryptoHealth READ healthObject CONSTANT)
    Q_PROPERTY(QObject *backup READ backupObject CONSTANT)
    Q_PROPERTY(QString backendName MEMBER backend CONSTANT)
public:
    FakeAccounts accountsFake;
    FakeHealth health;
    FakeBackup backupFake;
    QString trustState = QStringLiteral("Not verified");
    QString backend = QStringLiteral("rust");
    QObject *accountsObject() { return &accountsFake; }
    QObject *healthObject() { return &health; }
    QObject *backupObject() { return &backupFake; }
    Q_INVOKABLE void requestRecoverFromBackup(const QString &) {}
    Q_INVOKABLE void refreshCryptoHealth() {}
    Q_INVOKABLE void refreshSessionTrustState() {}
Q_SIGNALS:
    void recoveryStateChanged(const QString &state, const QString &message);
    void loggedInChanged();
    void changed();
};

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MatrixClient

ApplicationWindow {
    width: 640
    height: 600
    visible: true
    Item {
        anchors.fill: parent
        ColumnLayout {
            anchors.fill: parent
            RecoveryKeyEntry {
                objectName: "entry"
                fieldObjectName: "entryField"
                Layout.fillWidth: true
            }
            Item {
                objectName: "holder"
                Layout.fillWidth: true
                Layout.preferredHeight: 360
                CrossSigningSetupCard {
                    objectName: "card"
                    anchors.left: parent.left
                    anchors.right: parent.right
                }
            }
        }
    }
}
)QML";

} // namespace

class EncryptionPromptsStaleStateTest : public QObject
{
    Q_OBJECT

    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    FakeApp m_app;

    QQuickItem *find(const QString &name) const
    {
        return m_root->findChild<QQuickItem *>(name);
    }
    QString status() const
    {
        auto *l = find(QStringLiteral("entryFieldStatus"));
        return l ? l->property("text").toString() : QStringLiteral("<missing>");
    }
    void failOnce()
    {
        auto *tf = find(QStringLiteral("entryField"));
        tf->setProperty("text", QStringLiteral("wrong key"));
        QMetaObject::invokeMethod(find(QStringLiteral("entryFieldSubmit")), "clicked");
        Q_EMIT m_app.recoveryStateChanged(QStringLiteral("failed"),
                                          QStringLiteral("Wrong key."));
        QVERIFY(status().contains(QStringLiteral("Wrong key.")));
    }

private slots:
    void initTestCase()
    {
        m_engine = new QQmlEngine;
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), &m_app);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("stalescene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        QVERIFY(find(QStringLiteral("entryField")));
        QVERIFY(find(QStringLiteral("card")));
    }
    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
    }

    // C56-F02 / C56-D03: the red line stayed beside the key being corrected.
    void theFirstEditAfterAFailureRetiresTheError()
    {
        failOnce();
        find(QStringLiteral("entryField"))->setProperty("text", QStringLiteral("r"));
        QCOMPARE(status(), QString());
    }

    // C56-F02: the error survived closing and reopening Settings.
    void hidingTheEntryRetiresTheAnswer()
    {
        failOnce();
        auto *entry = find(QStringLiteral("entry"));
        entry->setVisible(false);
        entry->setVisible(true);
        QCOMPARE(status(), QString());
    }

    // C56-F05: a success from the previous sign-in showed on the next one.
    void aSignOutAndSignInDoNotCarryTheLastSessionsResult()
    {
        auto *tf = find(QStringLiteral("entryField"));
        tf->setProperty("text", QStringLiteral("good key"));
        QMetaObject::invokeMethod(find(QStringLiteral("entryFieldSubmit")), "clicked");
        Q_EMIT m_app.recoveryStateChanged(QStringLiteral("ok"), QString());
        QVERIFY(status().startsWith(QStringLiteral("Recovery complete")));
        Q_EMIT m_app.loggedInChanged();
        QCOMPARE(status(), QString());
    }

    // C56-F01: "Replace identity" stayed armed after Settings closed.
    void hidingTheCardDisarmsTheResetConfirmation()
    {
        auto *card = find(QStringLiteral("card"));
        auto *button = find(QStringLiteral("resetCrossSigningButton"));
        QVERIFY(card && button);
        QVERIFY(card->isVisible());
        QMetaObject::invokeMethod(button, "clicked");
        QCOMPARE(card->property("pendingConfirm").toString(), QStringLiteral("reset"));

        find(QStringLiteral("holder"))->setVisible(false);
        QVERIFY(!card->isVisible());
        find(QStringLiteral("holder"))->setVisible(true);
        QCOMPARE(card->property("pendingConfirm").toString(), QString());
    }

    // C56-F01: the typed account password stayed in the field.
    void hidingTheCardWipesATypedPassword()
    {
        m_app.backupFake.setProperty("passwordRequired", true);
        auto *field = find(QStringLiteral("crossSigningPasswordField"));
        QVERIFY(field);
        field->setProperty("text", QStringLiteral("abc"));
        find(QStringLiteral("holder"))->setVisible(false);
        find(QStringLiteral("holder"))->setVisible(true);
        QCOMPARE(field->property("text").toString(), QString());
        m_app.backupFake.setProperty("passwordRequired", false);
    }
};

QTEST_MAIN(EncryptionPromptsStaleStateTest)
#include "EncryptionPromptsStaleStateTest.moc"
