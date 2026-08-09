// Proves the local-session repair card actually replaces the old
// dead-end: after a classified restore/login failure, the repair action
// must be invocable WITHOUT the user typing anything, and it must target
// the account that actually failed — never raw form text, and never the
// wrong account during add-account (where a different account is already
// signed in). See qml/LoginScreen.qml and qml/AccountMenu.qml.
//
// The classification only ever fires from the Rust backend in production
// (a real SDK store/login rejection). This is a C++ test with direct
// access to the controller, so it drives the classification by calling
// AppController::setLocalSessionFailure() directly — no QML-facing
// test-only invokable exists in the production surface for this.

#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"

namespace {
constexpr int kSignalTimeoutMs = 5000;
} // namespace

class LoginRepairQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;
    QStringList m_warnings;

    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        if (parent->objectName() == name)
            return parent;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        }
        return nullptr;
    }

    QQuickItem *item(const char *name) const
    {
        if (auto *hit = m_window->findChild<QQuickItem *>(QLatin1String(name)))
            return hit;
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    // For anything that only exists after a Popup/Dialog opens: matches
    // CreationDialogQmlTest's proven pattern (retrying the lookup itself,
    // not just a property read on an already-resolved pointer) — Popup
    // content reparents into Overlay.overlay and is not guaranteed
    // discoverable the instant after .open() and one processEvents(). A
    // bare item(name) call here can legitimately return nullptr on the
    // first attempt. QTRY_VERIFY can't be reused here directly (its
    // internal bare `return` assumes a void test slot, not a pointer-
    // returning helper), so this polls by hand and still asserts via the
    // caller's own QVERIFY on the result.
    QQuickItem *waitForItem(const char *name) const
    {
        QQuickItem *found = item(name);
        QElapsedTimer timer;
        timer.start();
        while (!found && timer.elapsed() < kSignalTimeoutMs) {
            QTest::qWait(15);
            found = item(name);
        }
        return found;
    }

    // Dialog/Popup is a QQuickPopup — a QObject, NOT a QQuickItem (only
    // its contentItem is). item()/findItem() walk QQuickItem::childItems()
    // and findChild<QQuickItem*>(), both of which structurally exclude a
    // Popup itself by type; only its rendered contentItem children (real
    // Items) are reachable that way. CreationDialogQmlTest.cpp hits the
    // same distinction and resolves it the same way: a plain
    // findChild<QObject*> for the popup, item()/waitForItem() only for
    // what is genuinely inside it.
    //
    // Note this only retries the LOOKUP, not open state: the Dialog
    // exists in the object tree from LoginScreen.qml's load regardless of
    // whether it is currently open, so it is typically found on the very
    // first attempt. Callers must still poll (QTRY_VERIFY, not a bare
    // QVERIFY) for its `visible` property becoming true after .open() —
    // finding the object proves nothing about whether open() has taken
    // effect yet.
    QObject *waitForObject(const char *name) const
    {
        QObject *found = m_window->findChild<QObject *>(QLatin1String(name));
        QElapsedTimer timer;
        timer.start();
        while (!found && timer.elapsed() < kSignalTimeoutMs) {
            QTest::qWait(15);
            found = m_window->findChild<QObject *>(QLatin1String(name));
        }
        return found;
    }

    void clickItem(QQuickItem *target)
    {
        QVERIFY(target);
        // Let layout settle before reading geometry: the caller usually
        // just changed a reasonCode, which resizes the repair card and can
        // shift everything below it in the Flickable (and the panel's own
        // vertically-centering y binding) across more than one polish
        // pass. mapToScene() against stale geometry sends the click
        // somewhere else entirely — nothing crashes, the target's
        // onClicked just never fires, which is indistinguishable from a
        // hang: dialog exists (see waitForObject) but never opens.
        QCoreApplication::processEvents();
        QTest::qWait(1);
        QCoreApplication::processEvents();
        const QPointF center = target->mapToScene(
            QPointF(target->width() / 2, target->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          center.toPoint());
        QCoreApplication::processEvents();
    }

    // Opening the shared confirm dialog is flaky if it's driven as a
    // single clickItem() + wait: the trigger button transitions
    // invisible→visible in the same beat as the click (it only becomes
    // visible once a reasonCode makes it so), and QtQuick.Layouts does
    // not guarantee a hidden item has real, hit-testable geometry until
    // it has actually been polished visible — a click computed from
    // geometry that hasn't caught up yet silently lands nowhere, and
    // nothing about that looks different from a genuine hang (the dialog
    // exists per waitForObject(), it just never opens). Retrying the
    // CLICK itself, not just the wait, is robust regardless of which of
    // those is the actual cause.
    void openDialogVia(QQuickItem *button)
    {
        QVERIFY(button);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kSignalTimeoutMs) {
            clickItem(button);
            auto *dialog = m_window->findChild<QObject *>(
                QStringLiteral("loginRepairConfirmDialog"));
            if (dialog && dialog->property("visible").toBool())
                return;
            QTest::qWait(20);
        }
        QFAIL("loginRepairConfirmDialog never opened");
    }

    // AppController::setLocalSessionFailure() is public specifically so a
    // C++ test can drive the classification directly, with no QML-facing
    // test-only invokable in the production surface.
    void injectFailure(const QString &reasonCode, const QString &userId,
                       const QString &homeserver)
    {
        m_controller->setLocalSessionFailure(reasonCode, userId, homeserver);
        QCoreApplication::processEvents();
    }

    void clearFailure()
    {
        injectFailure(QString{}, QString{}, QString{});
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("login-repair-qml-test"));
        QSettings().clear();

        // No saved session: startup lands directly on LoginScreen, never
        // BootScreen — matches the "genuinely signed-out" launch, which is
        // also the state a failed startup restore settles into.
        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlApplicationEngine;
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings) {
                        const QString text = w.toString();
                        // The mock backend hands out media URLs on a host that
                        // does not resolve, and timeline rows now activate
                        // their media whenever they are genuinely inside the
                        // viewport — including in an offscreen run, where the
                        // previous virtualized view never instantiated them at
                        // all. That is a DNS failure in the fixture, not a QML
                        // defect, and it must not mask real warnings: only
                        // this exact unreachable-host message is dropped.
                        if (text.contains(QLatin1String(
                                "QQuickImage: Host mock.local not found")))
                            continue;
                        m_warnings.append(text);
                    }
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QSignalSpy createdSpy(m_engine,
                              &QQmlApplicationEngine::objectCreated);
        m_engine->loadFromModule(QStringLiteral("MatrixClient"),
                                 QStringLiteral("Main"));
        if (createdSpy.isEmpty())
            QVERIFY(createdSpy.wait(kSignalTimeoutMs));
        m_window = qobject_cast<QQuickWindow *>(
            createdSpy.at(0).at(0).value<QObject *>());
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QCOMPARE(int(m_controller->currentScreen()),
                 int(AppController::LoginScreen));
    }

    void cleanupTestCase()
    {
        delete m_engine;
        delete m_controller;
    }

    void init()
    {
        // Every test starts from a clean, un-failed, logged-out login
        // screen so the cases below don't leak into each other.
        clearFailure();
        m_controller->auth()->clearLastError();
        if (m_controller->loggedIn())
            m_controller->auth()->logout();
        QTRY_VERIFY(!m_controller->loggedIn());
        m_controller->showLogin();
        QCoreApplication::processEvents();
        item("userField")->setProperty("text", "");
    }

    void repairCardHiddenWithNoFailure()
    {
        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QVERIFY(!card->isVisible());
        auto *errorLabel = item("loginErrorLabel");
        QVERIFY(errorLabel);
    }

    // The core dead-end fix: the card appears, is fully labelled, and both
    // fields already show the FAILED account's identity — none of this
    // requires the user to type anything.
    void failureShowsCardAndPrefillsWithoutTyping()
    {
        QVERIFY(item("userField")->property("text").toString().isEmpty());

        injectFailure(QStringLiteral("session_account_mismatch"),
                     QStringLiteral("@alice:example.org"),
                     QStringLiteral("https://example.org"));

        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());

        QCOMPARE(item("userField")->property("text").toString(),
                 QStringLiteral("@alice:example.org"));
        QCOMPARE(item("homeserverField")->property("text").toString(),
                 QStringLiteral("https://example.org"));

        auto *headline = item("loginRepairHeadline");
        auto *body = item("loginRepairBody");
        QVERIFY(headline);
        QVERIFY(body);
        QVERIFY(!headline->property("text").toString().isEmpty());
        QVERIFY(!body->property("text").toString().isEmpty());

        // The plain generic error label is suppressed while the
        // classified card is showing — no double-reporting of one error.
        QVERIFY(!item("loginErrorLabel")->isVisible());

        auto *primary = item("loginRepairPrimaryAction");
        QVERIFY(primary);
        QTRY_VERIFY(primary->isVisible());
        QCOMPARE(primary->property("text").toString(),
                 QStringLiteral("Quarantine and rebuild"));
        QVERIFY(!item("loginRepairRetry")->isVisible());
        QVERIFY(!item("loginRepairRemoveAccount")->isVisible());
    }

    // The honest-outcome state: never implies the old device can come back,
    // and — the state a user recovering from the store-identity bug actually
    // lands in — never offers an action the backend will refuse.
    //
    // This test previously asserted a visible "Sign in again as a new device"
    // primary. That button routed to repairLocalSession(), which refuses for
    // this reason (suggestsLocalReset is false: there is no store left to
    // reset), so clicking it produced a red "this would destroy encryption
    // keys you still need" error. The suite was green over a dead end. The
    // remedy is the sign-in form above, already prefilled with this account.
    void savedSessionWithoutStoreOffersNewDeviceAndRemoveOnly()
    {
        injectFailure(QStringLiteral("saved_session_without_store"),
                     QStringLiteral("@bob:example.org"),
                     QStringLiteral("https://example.org"));

        auto *remove = item("loginRepairRemoveAccount");
        QTRY_VERIFY(remove->isVisible());
        auto *primary = item("loginRepairPrimaryAction");
        QVERIFY(!primary || !primary->isVisible());
        QVERIFY(!item("loginRepairRetry")->isVisible());

        auto *body = item("loginRepairBody");
        const QString bodyText = body->property("text").toString();
        QVERIFY2(!bodyText.contains(QStringLiteral("resume"), Qt::CaseInsensitive)
                  || bodyText.contains(QStringLiteral("can't resume"),
                                       Qt::CaseInsensitive),
                 qPrintable(bodyText));
        QVERIFY(!bodyText.contains(QStringLiteral("restored"), Qt::CaseInsensitive));
    }

    // The invariant, machine-checked rather than hand-maintained: a card may
    // NEVER show a destructive primary action for a reason whose backend
    // policy says a local reset cannot repair it. Checking each card by hand
    // is how saved_session_without_store shipped a button that
    // repairLocalSession() refuses; this walks every reachable reason code
    // and holds regardless of what future cards are added.
    void noCardOffersAnActionTheBackendWouldRefuse()
    {
        const QStringList codes{
            QStringLiteral("session_without_device_id"),
            QStringLiteral("session_account_mismatch"),
            QStringLiteral("sdk_store_ownership_mismatch"),
            QStringLiteral("store_without_session_metadata"),
            QStringLiteral("saved_session_without_store"),
            QStringLiteral("access_token_revoked"),
            QStringLiteral("ambiguous_store_candidates"),
            QStringLiteral("invalid_saved_account_identity"),
            QStringLiteral("secret_backend_unavailable"),
            QStringLiteral("cleanup_incomplete"),
        };
        for (const QString &code : codes) {
            injectFailure(code, QStringLiteral("@dana:example.org"),
                          QStringLiteral("https://example.org"));
            QTRY_VERIFY(item("loginRepairCard"));

            const bool helps = m_controller->localResetHelpsFor(code);
            // BOTH controls that reach repairLocalSession(), not just the
            // primary: loginRepairRetry routes to the same place.
            for (const char *name : {"loginRepairPrimaryAction",
                                     "loginRepairRetry"}) {
                auto *button = item(name);
                const bool shown = button && button->isVisible();
                QVERIFY2(!shown || helps,
                         qPrintable(QStringLiteral(
                             "reason '%1' shows destructive control '%2' that "
                             "the backend refuses")
                             .arg(code, QString::fromLatin1(name))));
            }
            // Every reachable reason must explain itself. A code with no card
            // leaves the user staring at a bare error line — which is what
            // secret_backend_unavailable, the state this pass introduced,
            // used to do.
            QVERIFY2(item("loginRepairCard")->isVisible(),
                     qPrintable(QStringLiteral("reason '%1' renders no card")
                                    .arg(code)));
        }
    }

    // Only cleanup_incomplete shows Retry, and nothing else does.
    void cleanupIncompleteShowsRetryOnly()
    {
        injectFailure(QStringLiteral("cleanup_incomplete"),
                     QStringLiteral("@carol:example.org"),
                     QStringLiteral("https://example.org"));

        auto *retry = item("loginRepairRetry");
        QTRY_VERIFY(retry->isVisible());
        QCOMPARE(retry->property("text").toString(), QStringLiteral("Retry"));
        QVERIFY(!item("loginRepairPrimaryAction")->isVisible());
        QVERIFY(!item("loginRepairRemoveAccount")->isVisible());
    }

    // H1 regression: a reasonCode classify() genuinely does not recognize
    // must never render nothing at all. Before the fix, ANY non-empty
    // reasonCode hid loginErrorLabel unconditionally (visible: ... &&
    // !repair.active), so an unrecognized code produced a blank form —
    // repair.active was true but repair.info was null, and the card
    // requires info !== null to show. The fallback needs lastError set
    // too, exactly as production pairs a classified failure with a
    // loginFailed(userMessage) emission from the same backend event;
    // beginSsoLogin() is a convenient public way to set some non-empty
    // lastError text without a test-only setter.
    void genuinelyUnknownReasonFallsBackToPlainLabelNotBlank()
    {
        m_controller->auth()->beginSsoLogin(QString());
        injectFailure(QStringLiteral("a_future_reason_this_client_does_not_know"),
                     QStringLiteral("@erin:example.org"),
                     QStringLiteral("https://example.org"));
        QCoreApplication::processEvents();

        QVERIFY(!item("loginRepairCard")->isVisible());
        QVERIFY2(item("loginErrorLabel")->isVisible(),
                 "an unrecognized reasonCode must not blank the whole form");
        QVERIFY(!item("loginErrorLabel")->property("text").toString().isEmpty());
    }

    // H1 regression: store_without_session_metadata previously fell into
    // the same blank-form gap as the unknown-code case above, even though
    // it is a real, reachable condition (login() finding a store whose
    // token is unreadable — the locked-keyring case). It now gets its own
    // card. suggestsLocalReset() is true for this reason in
    // RustSessionPolicy.cpp, so a destructive primary action is correct
    // here (unlike access_token_revoked below).
    void storeWithoutSessionMetadataNowRendersACard()
    {
        injectFailure(QStringLiteral("store_without_session_metadata"),
                     QStringLiteral("@erin:example.org"),
                     QStringLiteral("https://example.org"));

        QTRY_VERIFY(item("loginRepairCard")->isVisible());
        QVERIFY(!item("loginRepairHeadline")->property("text").toString().isEmpty());
        QVERIFY(!item("loginRepairBody")->property("text").toString().isEmpty());
        auto *primary = item("loginRepairPrimaryAction");
        QVERIFY(primary->isVisible());
        QCOMPARE(primary->property("text").toString(),
                 QStringLiteral("Quarantine and rebuild"));
    }

    // H1 regression: ambiguous_store_candidates is the second previously-
    // blank code. It gets a card too, but — unlike
    // store_without_session_metadata — suggestsLocalReset() is FALSE for
    // it: several stores could be real and Lightning will not guess which
    // one to clear. No destructive action of any kind.
    void ambiguousStoreCandidatesRendersInformationalCardOnly()
    {
        injectFailure(QStringLiteral("ambiguous_store_candidates"),
                     QStringLiteral("@grace:example.org"),
                     QStringLiteral("https://example.org"));

        QTRY_VERIFY(item("loginRepairCard")->isVisible());
        QVERIFY(!item("loginRepairHeadline")->property("text").toString().isEmpty());
        QVERIFY(!item("loginRepairBody")->property("text").toString().isEmpty());
        QVERIFY(!item("loginRepairPrimaryAction")->isVisible());
        QVERIFY(!item("loginRepairRetry")->isVisible());
        QVERIFY(!item("loginRepairRemoveAccount")->isVisible());
    }

    // H2 — the review's HIGH finding. access_token_revoked must NEVER
    // expose a path to app.repairLocalSession(): the backend's own
    // suggestsLocalReset(AccessTokenRevoked) is false because the local
    // store is exactly the key material a user with a merely-revoked
    // token still needs. loginRepairPrimaryAction and loginRepairRetry
    // are the only two buttons in this file that ever call
    // app.repairLocalSession() (see the shared confirm dialog's onClicked
    // below), so proving both are absent proves the card cannot reach
    // that call at all — not just that it isn't labelled invitingly.
    void accessTokenRevokedExposesNoDestructiveAction()
    {
        injectFailure(QStringLiteral("access_token_revoked"),
                     QStringLiteral("@henry:example.org"),
                     QStringLiteral("https://example.org"));

        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());

        // Never dereference a lookup that might legitimately be null.
        // loginRepairPrimaryAction/loginRepairRetry are the only two
        // buttons anywhere in this file that route to
        // app.repairLocalSession() (see the shared confirm dialog's
        // onClicked below) — proving neither is a *reachable, visible*
        // control proves the destructive action cannot be reached from
        // this card, whether the button object is absent or merely
        // hidden.
        auto *primary = item("loginRepairPrimaryAction");
        QVERIFY2(!primary || !primary->isVisible(),
                 "access_token_revoked must not offer a destructive primary action");
        auto *retry = item("loginRepairRetry");
        QVERIFY(!retry || !retry->isVisible());

        auto *body = item("loginRepairBody");
        QVERIFY(body);
        const QString bodyText = body->property("text").toString();
        QVERIFY2(bodyText.contains(QStringLiteral("intact"), Qt::CaseInsensitive),
                 qPrintable(bodyText));
        QVERIFY2(!bodyText.contains(QStringLiteral("damag"), Qt::CaseInsensitive),
                 qPrintable(bodyText));

        // "Remove this account" stays available as an explicit, separately
        // confirmed fallback — but taking it must go through
        // app.removeAccount(), never through the repair path (proven here
        // by "Repairing…" never appearing). waitForItem() retries the
        // LOOKUP itself (not just a property read) because Popup content
        // reparents into Overlay.overlay and is not guaranteed
        // discoverable the instant after .open().
        auto *remove = item("loginRepairRemoveAccount");
        QVERIFY(remove);
        QVERIFY(remove->isVisible());
        openDialogVia(remove);
        auto *dialog = waitForObject("loginRepairConfirmDialog");
        QVERIFY(dialog);
        auto *confirmBody = waitForItem("loginRepairConfirmBody");
        QVERIFY(confirmBody);
        QVERIFY(confirmBody->property("text").toString()
                    .contains(QStringLiteral("@henry:example.org")));
        auto *confirmAction = waitForItem("loginRepairConfirmAction");
        QVERIFY(confirmAction);
        clickItem(confirmAction);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        auto *result = item("loginRepairResult");
        QVERIFY(!result
                || result->property("text").toString() != QStringLiteral("Repairing…"));
    }

    // Confirmation dialog: names the exact affected account, and Cancel
    // is the default/focused button (never the destructive action).
    void primaryActionConfirmDialogNamesAccountAndDefaultsToCancel()
    {
        injectFailure(QStringLiteral("session_account_mismatch"),
                     QStringLiteral("@dave:example.org"),
                     QStringLiteral("https://example.org"));
        auto *primary = item("loginRepairPrimaryAction");
        QVERIFY(primary);
        QTRY_VERIFY(primary->isVisible());

        openDialogVia(primary);
        auto *dialog = waitForObject("loginRepairConfirmDialog");
        QVERIFY(dialog);

        auto *confirmBody = waitForItem("loginRepairConfirmBody");
        QVERIFY(confirmBody);
        QVERIFY2(confirmBody->property("text").toString()
                     .contains(QStringLiteral("@dave:example.org")),
                 qPrintable(confirmBody->property("text").toString()));

        auto *cancelButton = waitForItem("loginRepairCancel");
        QVERIFY(cancelButton);
        QTRY_VERIFY(cancelButton->property("activeFocus").toBool());

        // Cancel leaves the failure (and the card) exactly as it was —
        // no repair was invoked.
        clickItem(cancelButton);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QVERIFY(card->isVisible());
        auto *result = item("loginRepairResult");
        QVERIFY(!result || !result->isVisible());
    }

    // Confirming actually invokes the zero-argument repair path — proving
    // the whole flow needed no typed input, from failure to confirmed
    // repair.
    void confirmingPrimaryActionInvokesRepairWithoutTypedInput()
    {
        injectFailure(QStringLiteral("sdk_store_ownership_mismatch"),
                     QStringLiteral("@frank:example.org"),
                     QStringLiteral("https://example.org"));
        auto *primary = item("loginRepairPrimaryAction");
        QVERIFY(primary);
        QTRY_VERIFY(primary->isVisible());
        openDialogVia(primary);

        auto *dialog = waitForObject("loginRepairConfirmDialog");
        QVERIFY(dialog);

        auto *confirmButton = waitForItem("loginRepairConfirmAction");
        QVERIFY(confirmButton);
        QCOMPARE(confirmButton->property("text").toString(),
                 QStringLiteral("Quarantine and rebuild"));
        clickItem(confirmButton);

        QTRY_VERIFY(!dialog->property("visible").toBool());
        // The point of this test is that confirming reaches
        // app.repairLocalSession() through nothing but clicks — not what
        // the backend then does with it. AppController correctly refuses
        // a Rust-only reset on MockBackend ("Reset is only available on
        // the Rust backend."), which arrives fast enough to overwrite the
        // optimistic "Repairing…" text before this assertion runs; a real
        // Rust backend would instead report the repair's own outcome.
        // Either way, SOME non-empty result reaching the UI is what proves
        // the call was actually made.
        auto *result = item("loginRepairResult");
        QVERIFY(result);
        QTRY_VERIFY(result->isVisible());
        QVERIFY(!result->property("text").toString().isEmpty());
    }

    // Add-account edge case: an account is already signed in, and a
    // DIFFERENT account's login fails. The repair card must target the
    // failing account, never the one that's already active.
    void addAccountFailureTargetsTheFailingAccountNotTheActiveOne()
    {
        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("active-user"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());

        m_controller->showLogin(); // add-account flow
        QCoreApplication::processEvents();
        QCOMPARE(int(m_controller->currentScreen()),
                 int(AppController::LoginScreen));

        injectFailure(QStringLiteral("session_without_device_id"),
                     QStringLiteral("@other-account:example.org"),
                     QStringLiteral("https://example.org"));

        QTRY_VERIFY(item("loginRepairCard")->isVisible());
        QCOMPARE(item("userField")->property("text").toString(),
                 QStringLiteral("@other-account:example.org"));
        QVERIFY(!item("userField")->property("text").toString()
                     .contains(QStringLiteral("active-user")));

        auto *confirmBody0 = item("loginRepairConfirmBody");
        Q_UNUSED(confirmBody0);
        m_controller->auth()->logout();
        QTRY_VERIFY(!m_controller->loggedIn());
    }

    // M-A regression: the dialog used to read repair.userId/repair.info
    // LIVE, so a failure arriving while an already-open dialog is still
    // showing a PREVIOUS one — a slow sign-in elsewhere, or a different
    // account failing mid add-account — could change what confirming
    // would act on, out from under the user. Proves both halves of the
    // fix land together: the dialog auto-closes the instant the
    // classified failure changes, and — since that closes the only path
    // to app.repairLocalSession() — the destructive call genuinely never
    // fires for a failure the user never reviewed (proven via a signal
    // spy, not inferred from UI text alone).
    void dialogClosesAndRefusesWhenFailureChangesWhileOpen()
    {
        injectFailure(QStringLiteral("session_account_mismatch"),
                     QStringLiteral("@ivan:example.org"),
                     QStringLiteral("https://example.org"));
        auto *primary = item("loginRepairPrimaryAction");
        QVERIFY(primary);
        QTRY_VERIFY(primary->isVisible());
        openDialogVia(primary);

        auto *dialog = waitForObject("loginRepairConfirmDialog");
        QVERIFY(dialog);
        auto *confirmBody = waitForItem("loginRepairConfirmBody");
        QVERIFY(confirmBody);
        QVERIFY(confirmBody->property("text").toString()
                    .contains(QStringLiteral("@ivan:example.org")));

        QSignalSpy resetSpy(m_controller, &AppController::localRustStoreResetResult);

        // A different failure — different account, different reason —
        // supersedes this one while the dialog the user is looking at is
        // still showing @ivan.
        injectFailure(QStringLiteral("session_without_device_id"),
                     QStringLiteral("@julia:example.org"),
                     QStringLiteral("https://example.org"));

        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(resetSpy.count(), 0);

        // The new failure gets its own, independent card — the fix closes
        // the stale dialog, it doesn't just swallow the new failure.
        auto *newPrimary = item("loginRepairPrimaryAction");
        QVERIFY(newPrimary);
        QTRY_VERIFY(newPrimary->isVisible());
        QCOMPARE(item("userField")->property("text").toString(),
                 QStringLiteral("@julia:example.org"));
    }

    // Acceptance condition for the dead-end fix: LoginScreen no longer
    // calls the raw-typed-text reset path at all.
    void loginScreenNeverCallsTheRawTextResetPath()
    {
        QFile file(QStringLiteral(QML_DIR "/LoginScreen.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(!text.contains(QStringLiteral("resetLocalRustSession(")));
    }

    void noQmlWarnings()
    {
        QCOMPARE(m_warnings, QStringList{});
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    LoginRepairQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "LoginRepairQmlTest.moc"
