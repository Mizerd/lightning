// B011: A DEVICE THAT CAN NEVER DECRYPT ANYTHING NOW SAYS SO.
//
// THE DEFECT (audit B006, diagnosed on a real account 2026-09-07). A device
// had published a curve25519 identity key that its own local Olm account did
// not hold. Peers encrypt to the key the SERVER publishes, so every room key
// and every call media key addressed to that device was unreadable,
// permanently: encrypted messages sit on "Waiting for keys…" forever, and an
// encrypted call is silent one way while the other side hears you perfectly,
// because SENDING is unaffected. Only matrix-sdk's own tracing could see it;
// a fresh sign-in repaired it instantly.
//
// Detection shipped in 0d4578d and STOPPED at one qCCritical into a log the
// user never reads. This suite covers the half that was missing: the answer
// reaching the application layer as durable state, the tri-state that keeps
// "could not be established" from ever reading as "your encryption is
// destroyed", the surface that explains it and offers the repair, and the
// timeline row that stops promising a wait that will never end.
//
// Drives the REAL RustSdkMatrixClient signal the poll dispatcher emits, on a
// real AppController — no session, no network, no key material anywhere.

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
        // Parented to the AppController by makeClient, exactly as
        // VerificationFlowTest locates it.
        return app.findChild<RustSdkMatrixClient *>();
    }

    // The prompt only shows in the chat shell, so every UI case puts the
    // controller there first (a sign-out is not an action you can offer on
    // the login screen).
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

    // A complete role map so the production delegate binds without
    // undefined-property warnings (same shape MediaPlaceholderQmlTest uses).
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
        // The reported state: an encrypted event with no deterministic
        // reason, which is what "Waiting for keys…" is the label for.
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

    // Walk the delegate for the label the undecryptable action row shows.
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
    // Isolate the settings store: AppController builds a real
    // SettingsManager, which is a default QSettings resolved from the
    // application identity.
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

    // THE CASE THAT MUST NEVER REGRESS. An unanswerable check — offline, keys
    // not uploaded yet, a 5xx on /keys/query — must not tell a healthy user
    // that their encryption is destroyed.
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

    // The real fault reaches the application layer as durable state, and is
    // announced exactly once however often the backstop re-checks.
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

        // The 15-minute backstop must not re-announce the same fault.
        Q_EMIT rust->ownDeviceIdentityKeyChecked(true, false);
        QVERIFY(app.encryptionIdentityBroken());
        QCOMPARE(changed.count(), 1);
#endif
    }

    // And the other half of the tri-state: once broken, a check that cannot
    // run must not quietly report the device healthy again.
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

        // A genuine agreement does clear it — a repaired session on the same
        // controller must not stay accused.
        Q_EMIT rust->ownDeviceIdentityKeyChecked(true, true);
        QVERIFY(!app.encryptionIdentityBroken());
#endif
    }

    // Signing out IS the repair, so the fault must not survive into the next
    // session. Account switching detaches through the same signal.
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

    // THE SURFACE. Hidden while nothing is known, shown the moment the fault
    // is real — and it names the repair rather than only the symptom.
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

        // An unanswerable check must not raise the card either.
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

        // The repair is offered, and the honest cost is stated on the
        // confirmation the user has to pass through.
        auto *fix = ui.root->findChild<QQuickItem *>(
            QStringLiteral("encryptionBrokenPromptFix"));
        QVERIFY(fix != nullptr);
        QVERIFY(fix->property("text").toString()
                    .contains(QStringLiteral("Sign out")));
        auto *consequences = ui.root->findChild<QObject *>(
            QStringLiteral("encryptionBrokenSignOutConsequences"));
        QVERIFY(consequences != nullptr);
        const QString cost = consequences->property("text").toString();
        QVERIFY2(cost.contains(QStringLiteral("key backup")),
                 "the sign-out confirmation does not say that history "
                 "outside key backup will not come back");
        QVERIFY2(cost.contains(QStringLiteral("NEW session")),
                 "the confirmation does not say this creates a new session");

        // Session-only dismissal: it stops nagging, and nothing is
        // persisted that could silence a permanent fault on a later launch.
        auto *dismiss = ui.root->findChild<QQuickItem *>(
            QStringLiteral("encryptionBrokenPromptDismiss"));
        QVERIFY(dismiss != nullptr);
        ui.root->setProperty("suppressed", true);
        QCoreApplication::processEvents();
        QVERIFY(!ui.root->property("shouldShow").toBool());

        QCOMPARE(ui.warnings, QStringList{});
#endif
    }

    // "Waiting for keys…" IS A LIE WHEN THE KEYS CAN NEVER ARRIVE. The row
    // that the reader is actually looking at stops promising a wait that has
    // no end, and points at the explanation the card carries.
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

        // With the fault established the same row says what is true.
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
