// A device whose published curve25519 identity key differs from its local
// Olm account can never decrypt anything addressed to it (peers encrypt to
// the server-published key), while sending still works. Detection must
// become durable application state; the tri-state keeps "could not be
// established" from reading as broken; a prompt explains it and offers the
// repair (a fresh sign-in); and undecryptable rows stop promising a wait.
// Drives the real RustSdkMatrixClient signal on a real AppController, with no
// session, network or key material.

#include "app/AppController.h"
#include "models/TimelineModel.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

namespace {
constexpr int kSignalTimeoutMs = 5000;
}

class EncryptionIdentityFaultTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    static RustSdkMatrixClient *rustClient(AppController &app)
    {
        // Parented to the AppController by makeClient, as VerificationFlowTest
        // locates it.
        return app.findChild<RustSdkMatrixClient *>();
    }

    // The prompt only shows in the chat shell, so UI cases go there first.
    static void reachChatShell(AppController &app) { app.showMain(); }

    struct Loaded {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
        QStringList warnings;
    };

    bool load(AppController &controller, const QString &component,
              Loaded &out, const QVariantMap *modelFixture = nullptr)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        connect(out.engine.get(), &QQmlEngine::warnings, this,
                [&out](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        out.warnings << e.toString();
                });
        out.engine->rootContext()->setContextProperty("app", &controller);
        if (modelFixture)
            out.engine->rootContext()->setContextProperty("model",
                                                          *modelFixture);
        QSignalSpy created(out.engine.get(),
                           &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"), component);
        if (created.isEmpty() && !created.wait(kSignalTimeoutMs))
            return false;
        out.root = qobject_cast<QQuickItem *>(
            created.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(900, 600);
        out.root->setParentItem(out.window->contentItem());
        out.window->show();
        QCoreApplication::processEvents();
        return true;
    }

    // A complete role map so the production delegate binds without warnings
    // (same shape as MediaPlaceholderQmlTest).
    static QVariantMap undecryptableRow(AppController &controller)
    {
        QVariantMap fixture;
        const auto roles = controller.timeline()->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            fixture.insert(QString::fromUtf8(it.value()), QVariant{});
        fixture.insert(QStringLiteral("isVirtual"), false);
        fixture.insert(QStringLiteral("isStateActivity"), false);
        fixture.insert(QStringLiteral("stateGroupEntries"), QVariantList{});
        fixture.insert(QStringLiteral("showSenderIdentity"), true);
        fixture.insert(QStringLiteral("eventId"), QStringLiteral("$fixture"));
        fixture.insert(QStringLiteral("itemId"), QStringLiteral("fixture-item"));
        fixture.insert(QStringLiteral("sender"),
                       QStringLiteral("@fixture:mock.local"));
        fixture.insert(QStringLiteral("senderDisplayName"),
                       QStringLiteral("Fixture"));
        fixture.insert(QStringLiteral("senderInitials"), QStringLiteral("F"));
        fixture.insert(QStringLiteral("body"), QString{});
        fixture.insert(QStringLiteral("eventType"), 0);
        fixture.insert(QStringLiteral("status"), 0);
        fixture.insert(QStringLiteral("isOwn"), false);
        fixture.insert(QStringLiteral("timestamp"),
                       QDateTime::currentDateTimeUtc());
        fixture.insert(QStringLiteral("redacted"), false);
        fixture.insert(QStringLiteral("edited"), false);
        fixture.insert(QStringLiteral("isEncrypted"), true);
        fixture.insert(QStringLiteral("isDecrypted"), false);
        // An encrypted event with no deterministic reason: the "Waiting for
        // keys…" case.
        fixture.insert(QStringLiteral("undecryptable"), true);
        fixture.insert(QStringLiteral("errorKind"), QString{});
        fixture.insert(QStringLiteral("isImage"), false);
        fixture.insert(QStringLiteral("isFile"), false);
        fixture.insert(QStringLiteral("isVideo"), false);
        fixture.insert(QStringLiteral("isAudio"), false);
        fixture.insert(QStringLiteral("isSticker"), false);
        fixture.insert(QStringLiteral("mediaIsVoice"), false);
        fixture.insert(QStringLiteral("mediaDurationMs"), 0);
        fixture.insert(QStringLiteral("mediaWidth"), 0);
        fixture.insert(QStringLiteral("mediaHeight"), 0);
        fixture.insert(QStringLiteral("mediaSize"), 0);
        fixture.insert(QStringLiteral("mediaSourceAvailable"), false);
        fixture.insert(QStringLiteral("mediaThumbAvailable"), false);
        fixture.insert(QStringLiteral("mediaKey"), QString{});
        fixture.insert(QStringLiteral("mediaFilename"), QString{});
        fixture.insert(QStringLiteral("mediaUrl"), QUrl{});
        fixture.insert(QStringLiteral("mediaThumbUrl"), QUrl{});
        fixture.insert(QStringLiteral("mediaMimetype"), QString{});
        fixture.insert(QStringLiteral("reactions"), QVariantList{});
        fixture.insert(QStringLiteral("replyToEventId"), QString{});
        fixture.insert(QStringLiteral("isThreadRoot"), false);
        fixture.insert(QStringLiteral("mentionsMe"), false);
        fixture.insert(QStringLiteral("mentionsRoom"), false);
        fixture.insert(QStringLiteral("isLocalEcho"), false);
        return fixture;
    }

    // The label the undecryptable action row shows.
    static QString undecryptableLabelText(QQuickItem *root)
    {
        const auto items = root->findChildren<QQuickItem *>();
        for (auto *item : items) {
            const QString text = item->property("text").toString();
            if (text.contains(QStringLiteral("Waiting for keys"))
                || text.contains(QStringLiteral("can't unlock")))
                return text;
        }
        return {};
    }
#endif

private Q_SLOTS:
    // Isolate the settings store: AppController builds a real SettingsManager
    // over the default QSettings.
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        QVERIFY(m_dataHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_dataHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("encryption-identity-fault-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // An unanswerable check (offline, keys not uploaded, a 5xx on /keys/query)
    // must not tell a healthy user their encryption is broken.
    void anUnestablishedAnswerIsNotAFault()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The identity-key check exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        QSignalSpy changed(&app,
                           &AppController::encryptionIdentityBrokenChanged);
        QVERIFY(!app.encryptionIdentityBroken());

        Q_EMIT rust->ownDeviceIdentityKeyChecked(false, false);
        QVERIFY2(!app.encryptionIdentityBroken(),
                 "\"could not be established\" was published as a broken "
                 "device: every offline user would be told their encryption "
                 "is destroyed");
        QCOMPARE(changed.count(), 0);
#endif
    }

    // A real mismatch becomes durable state and is announced once, however
    // often the backstop re-checks.
    void anExplicitMismatchBecomesDurableApplicationState()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The identity-key check exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        QSignalSpy changed(&app,
                           &AppController::encryptionIdentityBrokenChanged);

        Q_EMIT rust->ownDeviceIdentityKeyChecked(true, false);
        QVERIFY2(app.encryptionIdentityBroken(),
                 "the detection reached C++ and went no further than a log "
                 "line — the whole of B011");
        QCOMPARE(changed.count(), 1);

        // The 15-minute backstop does not re-announce.
        Q_EMIT rust->ownDeviceIdentityKeyChecked(true, false);
        QVERIFY(app.encryptionIdentityBroken());
        QCOMPARE(changed.count(), 1);
#endif
    }

    // Once broken, a check that cannot run does not report the device healthy
    // again.
    void anUnestablishedAnswerNeverClearsARealFault()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The identity-key check exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        Q_EMIT rust->ownDeviceIdentityKeyChecked(true, false);
        QVERIFY(app.encryptionIdentityBroken());

        Q_EMIT rust->ownDeviceIdentityKeyChecked(false, false);
        QVERIFY2(app.encryptionIdentityBroken(),
                 "an unanswerable check cleared a permanent fault");

        // A genuine agreement clears it: a repaired session is not accused.
        Q_EMIT rust->ownDeviceIdentityKeyChecked(true, true);
        QVERIFY(!app.encryptionIdentityBroken());
#endif
    }

    // Signing out is the repair, so the fault does not survive into the next
    // session; account switching detaches through the same signal.
    void signingOutForgetsTheFault()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The identity-key check exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        Q_EMIT rust->ownDeviceIdentityKeyChecked(true, false);
        QVERIFY(app.encryptionIdentityBroken());
        QSignalSpy changed(&app,
                           &AppController::encryptionIdentityBrokenChanged);

        Q_EMIT rust->loggedOut();
        QVERIFY2(!app.encryptionIdentityBroken(),
                 "the next account inherited the previous one's "
                 "undecryptable-device fault");
        QCOMPARE(changed.count(), 1);
#endif
    }

    // The prompt is hidden while nothing is known, shown for a real fault,
    // and names the repair.
    void thePromptAppearsOnlyForARealFaultAndOffersTheRepair()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The identity-key check exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        reachChatShell(app);

        Loaded ui;
        QVERIFY2(load(app, QStringLiteral("EncryptionBrokenPrompt"), ui),
                 "EncryptionBrokenPrompt.qml failed to load — the qWarning "
                 "above names the property or type that does not exist");
        QVERIFY2(!ui.root->property("shouldShow").toBool(),
                 "the card is showing before anything has been established");

        // An unanswerable check does not raise it either.
        Q_EMIT rust->ownDeviceIdentityKeyChecked(false, false);
        QCoreApplication::processEvents();
        QVERIFY2(!ui.root->property("shouldShow").toBool(),
                 "a check that could not be answered raised the card — "
                 "every offline user would be told their encryption is "
                 "destroyed");

        Q_EMIT rust->ownDeviceIdentityKeyChecked(true, false);
        QCoreApplication::processEvents();
        QVERIFY2(ui.root->property("shouldShow").toBool(),
                 "the user is left with \"Waiting for keys…\" and no "
                 "explanation — B011");

        // The repair is offered, with its cost stated on the confirmation.
        auto *fix = ui.root->findChild<QQuickItem *>(
            QStringLiteral("encryptionBrokenPromptFix"));
        QVERIFY(fix != nullptr);
        QVERIFY(fix->property("text").toString()
                    .contains(QStringLiteral("Sign out")));
        auto *consequences = ui.root->findChild<QObject *>(
            QStringLiteral("encryptionBrokenSignOutConsequences"));
        QVERIFY(consequences != nullptr);
        const QString cost = consequences->property("text").toString();
        // Stated for this account: with no usable backup the confirmation says
        // so outright rather than hedging.
        QVERIFY2(cost.contains(QStringLiteral("KEY BACKUP IS NOT SET UP")),
                 "the sign-out confirmation hedged about key backup on an "
                 "account that demonstrably has none: the user is deciding "
                 "whether to destroy local history and is owed the specific "
                 "answer, not the general one");
        QVERIFY2(cost.contains(QStringLiteral("not come back"))
                     || cost.contains(QStringLiteral("NOT come back")),
                 "the confirmation does not say history will not come back");
        QVERIFY2(cost.contains(QStringLiteral("NEW session")),
                 "the confirmation does not say this creates a new session");

        // Dismissal is per session: nothing persisted can silence a permanent
        // fault on a later launch.
        auto *dismiss = ui.root->findChild<QQuickItem *>(
            QStringLiteral("encryptionBrokenPromptDismiss"));
        QVERIFY(dismiss != nullptr);
        ui.root->setProperty("suppressed", true);
        QCoreApplication::processEvents();
        QVERIFY(!ui.root->property("shouldShow").toBool());

        QCOMPARE(ui.warnings, QStringList{});
#endif
    }

    // With the fault established, an undecryptable row stops saying
    // "Waiting for keys…" and points at the explanation.
    void theUndecryptableRowStopsPromisingAWaitThatCannotEnd()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("The identity-key check exists on the Rust backend only.");
#else
        AppController app(AppController::RustBackend);
        auto *rust = rustClient(app);
        QVERIFY(rust);
        const QVariantMap fixture = undecryptableRow(app);

        Loaded healthy;
        QVERIFY(load(app, QStringLiteral("MessageDelegate"), healthy,
                     &fixture));
        QCOMPARE(undecryptableLabelText(healthy.root),
                 QStringLiteral("Waiting for keys…"));

        // With the fault established the row says what is true.
        Q_EMIT rust->ownDeviceIdentityKeyChecked(true, false);
        QCoreApplication::processEvents();
        const QString broken = undecryptableLabelText(healthy.root);
        QVERIFY2(broken != QStringLiteral("Waiting for keys…"),
                 "the row still promises a wait that can never end");
        QVERIFY(broken.contains(QStringLiteral("can't unlock")));
#endif
    }

private:
    QTemporaryDir m_configHome;
    QTemporaryDir m_dataHome;
};

QTEST_MAIN(EncryptionIdentityFaultTest)
#include "EncryptionIdentityFaultTest.moc"
