
#include "app/AppController.h"
#include "crypto/QrImageProvider.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    width: 1100
    height: 900
    visible: true
    SettingsScreen {
        objectName: "settingsScreen"
        anchors.fill: parent
        section: "sessions"
    }
    VerificationDialog {
        objectName: "verificationDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
    }
}
)QML";

}

class VerificationQrQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QStringList m_warnings;

#ifdef ENABLE_RUST_SDK_BACKEND
    RustSdkMatrixClient *rust() const
    {
        return m_controller->findChild<RustSdkMatrixClient *>();
    }

    static constexpr int kModules = 21;
    static QByteArray grid()
    {
        return QByteArray(((kModules + 7) / 8) * kModules, '\x55');
    }
#endif

    QQuickItem *item(const char *name) const
    {
        return m_root ? m_root->findChild<QQuickItem *>(QLatin1String(name))
                      : nullptr;
    }

    void settle() { QCoreApplication::processEvents(); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("verification-qr-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::RustBackend);
        m_engine = new QQmlEngine;
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        m_engine->addImageProvider(QStringLiteral("lightning-qr"),
                                   new QrImageProvider(
                                       m_controller->qrCodeStore()));
        auto *component = new QQmlComponent(m_engine);
        component->setData(QByteArray(kScene),
                           QUrl(QStringLiteral("verificationqrscene.qml")));
        m_root = component->create();
        QVERIFY2(m_root, qPrintable(component->errorString()));
        component->setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QVERIFY(item("settingsScreen"));
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
        delete m_controller;
    }

    void theQrPanelIsAbsentUntilTheSdkProducesACode()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        auto *panel = item("verificationQrPanel");
        QVERIFY2(panel, "verificationQrPanel not found in SettingsScreen");
        QVERIFY(!panel->isVisible());

        Q_EMIT rust()->verificationRequestStarted(
            QStringLiteral("flow-qml-1"), QStringLiteral("@self:example.org"),
            true);
        Q_EMIT rust()->verificationReady(QStringLiteral("flow-qml-1"));
        settle();
        QCOMPARE(m_controller->verificationState(), QStringLiteral("ready"));
        QVERIFY(!panel->isVisible());
#endif
    }

    void aReadyCodeRendersTheImageAndTheScanGuidance()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        Q_EMIT rust()->verificationQrReady(QStringLiteral("flow-qml-1"),
                                           kModules, grid());
        settle();

        auto *panel = item("verificationQrPanel");
        QVERIFY(panel && panel->isVisible());

        auto *image = item("verificationQrImage");
        QVERIFY2(image, "verificationQrImage not found");
        const QString source = image->property("source").toUrl().toString();
        QVERIFY2(source.startsWith(QStringLiteral("image://lightning-qr/")),
                 qPrintable(source));
        // No flow id may reach a URL — only the opaque token.
        QVERIFY2(!source.contains(QStringLiteral("flow-qml-1")),
                 qPrintable(source));
        QCOMPARE(image->property("cache").toBool(), false);

        QTRY_COMPARE(image->property("status").toInt(), 1);
        const int rendered = image->property("implicitWidth").toInt();
        QVERIFY2(rendered > 0, "the QR provider returned no image");
        QCOMPARE(rendered % (kModules + 8), 0);
        QCOMPARE(image->property("implicitHeight").toInt(), rendered);

        QQmlExpression expr(qmlContext(image), image,
                            QStringLiteral("Accessible.name"));
        const QString accessible = expr.evaluate().toString();
        QVERIFY2(!accessible.isEmpty(), qPrintable(expr.error().toString()));
        QVERIFY(accessible.contains(QStringLiteral("QR"), Qt::CaseInsensitive));
        QVERIFY(accessible.contains(QStringLiteral("scan"),
                                    Qt::CaseInsensitive));

        auto *confirm = item("verificationQrConfirmButton");
        QVERIFY2(confirm, "verificationQrConfirmButton not found");
        QVERIFY(!confirm->isVisible());

        auto *status = item("verificationStatusLabel");
        QVERIFY(status);
        QVERIFY2(status->property("text").toString().contains(
                     QStringLiteral("Scan"), Qt::CaseInsensitive),
                 qPrintable(status->property("text").toString()));
#endif
    }

    void theScanReportSwapsInTheConfirmPrompt()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        Q_EMIT rust()->verificationQrScanned(QStringLiteral("flow-qml-1"));
        settle();

        auto *confirm = item("verificationQrConfirmButton");
        auto *reject = item("verificationQrRejectButton");
        QVERIFY(confirm && confirm->isVisible());
        QVERIFY(reject && reject->isVisible());
        QVERIFY(confirm->property("enabled").toBool());

        auto *status = item("verificationStatusLabel");
        QVERIFY(status);
        const QString text = status->property("text").toString();
        QVERIFY2(text.contains(QStringLiteral("scanned"), Qt::CaseInsensitive),
                 qPrintable(text));
        QVERIFY2(!text.contains(QStringLiteral("Verification complete")),
                 qPrintable(text));

        QMetaObject::invokeMethod(confirm, "clicked");
        settle();
        QVERIFY(m_controller->verificationQrConfirming());
        QVERIFY(m_controller->verificationState() != QLatin1String("done"));
        QVERIFY(!confirm->property("enabled").toBool());
#endif
    }

    void completionRetiresTheCodeAndTheWholePanel()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        Q_EMIT rust()->verificationDone(QStringLiteral("flow-qml-1"));
        settle();
        QCOMPARE(m_controller->verificationState(), QStringLiteral("done"));
        auto *panel = item("verificationQrPanel");
        QVERIFY(panel && !panel->isVisible());
#endif
    }

    void aDismissedCodeYieldsToTheEmojiPresentation()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("Verification exists on the Rust backend only.");
#else
        m_controller->cancelVerification();
        settle();

        Q_EMIT rust()->verificationRequestStarted(
            QStringLiteral("flow-qml-2"), QStringLiteral("@self:example.org"),
            true);
        Q_EMIT rust()->verificationReady(QStringLiteral("flow-qml-2"));
        Q_EMIT rust()->verificationQrReady(QStringLiteral("flow-qml-2"),
                                           kModules, grid());
        settle();
        auto *panel = item("verificationQrPanel");
        QVERIFY(panel && panel->isVisible());

        Q_EMIT rust()->verificationQrDismissed(
            QStringLiteral("flow-qml-2"), QStringLiteral("peer_started_sas"));
        settle();
        QVERIFY(!panel->isVisible());

        QVariantList emojis;
        QVariantMap emoji;
        emoji.insert(QStringLiteral("symbol"), QStringLiteral("E"));
        emoji.insert(QStringLiteral("description"), QStringLiteral("Emoji"));
        emojis.append(emoji);
        Q_EMIT rust()->verificationSasReady(QStringLiteral("flow-qml-2"),
                                            emojis, QVariantList{});
        settle();
        QCOMPARE(m_controller->verificationState(),
                 QStringLiteral("sas_ready"));
        QVERIFY(!panel->isVisible());

        m_controller->cancelVerification();
        settle();
        QVERIFY(!panel->isVisible());
#endif
    }

    void zzzNoQmlWarningsWereProduced()
    {
        QVERIFY2(m_warnings.isEmpty(),
                 qPrintable(m_warnings.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(VerificationQrQmlTest)
#include "VerificationQrQmlTest.moc"
