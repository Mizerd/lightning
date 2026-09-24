// The local-session repair card: after a classified restore/login failure the
// repair action is invocable without typing, and targets the account that
// failed, never raw form text or (during add-account) the active account. See
// qml/LoginScreen.qml and qml/AccountMenu.qml. Classification comes from the
// Rust backend in production; here it is driven through the public
// AppController::setLocalSessionFailure(), with no test-only QML invokable.

#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>

#include <functional>
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

    // For items that only exist after a Popup/Dialog opens: popup content
    // reparents into Overlay.overlay and may not be discoverable right after
    // open(), so retry the lookup itself. QTRY_VERIFY's bare `return` cannot
    // be used in a pointer-returning helper, so this polls by hand.
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

    // A Dialog is a QQuickPopup (a QObject, not a QQuickItem), so it is found
    // with findChild<QObject*>; item()/waitForItem() reach only what is inside
    // it. This retries the lookup, not the open state: callers still poll
    // `visible` after open().
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
        // Let layout settle before reading geometry: a new reasonCode resizes
        // the card and can shift everything below it across several polish
        // passes, and a click at stale coordinates silently misses.
        QCoreApplication::processEvents();
        QTest::qWait(1);
        QCoreApplication::processEvents();
        const QPointF center = target->mapToScene(
            QPointF(target->width() / 2, target->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          center.toPoint());
        QCoreApplication::processEvents();
    }

    // Retry the click, not just the wait: the trigger button becomes visible
    // in the same beat as the click, and Layouts do not guarantee hit-testable
    // geometry until it has been polished visible, so a click can silently
    // miss.
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

    // setLocalSessionFailure() is public so a C++ test can drive the
    // classification without a test-only QML invokable.
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

        // No saved session: startup lands directly on LoginScreen, the same
        // state a failed startup restore settles into.
        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlApplicationEngine;
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings) {
                        const QString text = w.toString();
                        // The mock hands out media URLs on an unresolvable host,
                        // and timeline rows load media once in the viewport,
                        // including offscreen. Only this exact DNS message is
                        // dropped so real warnings still surface.
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
        // Each test starts from a clean, logged-out login screen.
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

    // The card appears, is fully labelled, and both fields already show the
    // failed account's identity without typing.
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

        // The generic error label is suppressed while the card shows.
        QVERIFY(!item("loginErrorLabel")->isVisible());

        auto *primary = item("loginRepairPrimaryAction");
        QVERIFY(primary);
        QTRY_VERIFY(primary->isVisible());
        QCOMPARE(primary->property("text").toString(),
                 QStringLiteral("Quarantine and rebuild"));
        QVERIFY(!item("loginRepairRetry")->isVisible());
        QVERIFY(!item("loginRepairRemoveAccount")->isVisible());
    }

    // saved_session_without_store: never implies the old device can return,
    // and never offers an action the backend refuses (repairLocalSession()
    // refuses this reason; there is no store to reset). The remedy is the
    // prefilled sign-in form.
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

    // Invariant over every reachable reason code: no card shows a destructive
    // action for a reason whose backend policy says a local reset cannot
    // repair it.
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
            // Both controls that reach repairLocalSession(): the primary and
            // loginRepairRetry.
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
            // Every reachable reason explains itself with a card rather than a
            // bare error line.
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

    // An unrecognised reasonCode must fall back to the plain error label, not
    // a blank form (the card needs repair.info, which is null here). lastError
    // is set too, as production pairs a classified failure with
    // loginFailed(); beginSsoLogin() is a public way to set it.
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

    // store_without_session_metadata (a store whose token is unreadable, e.g.
    // a locked keyring) gets its own card; suggestsLocalReset() is true for it
    // in RustSessionPolicy.cpp, so a destructive primary is correct here.
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

    // ambiguous_store_candidates gets an informational card only:
    // suggestsLocalReset() is false, since Lightning will not guess which of
    // several stores to clear.
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

    // access_token_revoked must never reach app.repairLocalSession(): the
    // local store holds the key material a user with a revoked token still
    // needs. loginRepairPrimaryAction and loginRepairRetry are the only
    // buttons that call it, so both must be absent or hidden.
    void accessTokenRevokedExposesNoDestructiveAction()
    {
        injectFailure(QStringLiteral("access_token_revoked"),
                     QStringLiteral("@henry:example.org"),
                     QStringLiteral("https://example.org"));

        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());

        // Never dereference a lookup that may be null; absent or hidden both
        // mean unreachable.
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

        // "Remove this account" stays available as a separately confirmed
        // fallback, going through app.removeAccount(), never the repair path
        // ("Repairing…" never appears).
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

    // The confirmation dialog names the exact account, and Cancel is the
    // default focused button.
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

        // Cancel leaves the failure and the card untouched.
        clickItem(cancelButton);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QVERIFY(card->isVisible());
        auto *result = item("loginRepairResult");
        QVERIFY(!result || !result->isVisible());
    }

    // Confirming invokes the zero-argument repair path: the whole flow needs
    // no typed input.
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
        // This checks that confirming reaches app.repairLocalSession() through
        // clicks only. MockBackend refuses the Rust-only reset quickly enough to
        // overwrite "Repairing…", so any non-empty result proves the call.
        auto *result = item("loginRepairResult");
        QVERIFY(result);
        QTRY_VERIFY(result->isVisible());
        QVERIFY(!result->property("text").toString().isEmpty());
    }

    // Add-account: another account is signed in and a different account's
    // login fails; the card targets the failing account.
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

    // The confirm dialog snapshots the failure it was opened for. When the
    // classified failure changes while it is open, the dialog closes and the
    // destructive call never fires for a failure the user did not review
    // (checked with a signal spy).
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

        // A different failure (account and reason) supersedes this one while
        // the dialog still shows @ivan.
        injectFailure(QStringLiteral("session_without_device_id"),
                     QStringLiteral("@julia:example.org"),
                     QStringLiteral("https://example.org"));

        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(resetSpy.count(), 0);

        // The new failure gets its own card; it is not swallowed.
        auto *newPrimary = item("loginRepairPrimaryAction");
        QVERIFY(newPrimary);
        QTRY_VERIFY(newPrimary->isVisible());
        QCOMPARE(item("userField")->property("text").toString(),
                 QStringLiteral("@julia:example.org"));
    }

    // LoginScreen never calls the raw-typed-text reset path.
    void loginScreenNeverCallsTheRawTextResetPath()
    {
        QFile file(QStringLiteral(QML_DIR "/LoginScreen.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(!text.contains(QStringLiteral("resetLocalRustSession(")));
    }

    // The login card must not move the fields under the cursor. The
    // homeserver probe is async and can hide the browser-login/SSO sections
    // below the fields; a card centred on its current height then slides
    // down, and a click aimed at "User" can put the password into the
    // clear-text homeserver field. Asserts the premise (the form got shorter)
    // before asserting the fields stayed put.
    void theLoginCardDoesNotMoveWhenOptionalSectionsDisappear()
    {
        QQuickItem *panel = item("loginPanel");
        QVERIFY2(panel, "the login card was not found");
        QQuickItem *user = item("userField");
        QQuickItem *pass = item("passField");
        QVERIFY(user && pass);

        const qreal panelY0 = panel->y();
        const qreal formH0 = panel->property("implicitHeight").toReal();
        const QPointF userScene0 = user->mapToScene(QPointF(0, 0));
        const QPointF passScene0 = pass->mapToScene(QPointF(0, 0));
        QVERIFY(formH0 > 0);

        // Shrink the form as the probe does: hide something below the password
        // field, where the optional sections live.
        const qreal passBottom = pass->mapToItem(panel, QPointF(0, pass->height())).y();
        QQuickItem *shrink = nullptr;
        qreal best = 0;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *node) {
            for (QQuickItem *c : node->childItems()) {
                if (c->isVisible() && c->height() > 20) {
                    const qreal top = c->mapToItem(panel, QPointF(0, 0)).y();
                    if (top > passBottom && c->height() > best) {
                        best = c->height();
                        shrink = c;
                    }
                }
                walk(c);
            }
        };
        walk(panel);
        QVERIFY2(shrink,
                 qPrintable(QStringLiteral(
                     "no visible section below the password field (bottom %1) "
                     "was found to shrink the card with")
                         .arg(passBottom)));
        shrink->setProperty("visible", false);
        QTest::qWait(60);

        const qreal formH1 = panel->property("implicitHeight").toReal();
        QVERIFY2(formH1 < formH0,
                 qPrintable(QStringLiteral(
                     "the form did not get shorter (%1 -> %2), so this case "
                     "would pass without testing anything")
                         .arg(formH0).arg(formH1)));

        const qreal moved = qAbs(panel->y() - panelY0);
        const qreal userMoved = qAbs(user->mapToScene(QPointF(0, 0)).y()
                                     - userScene0.y());
        const qreal passMoved = qAbs(pass->mapToScene(QPointF(0, 0)).y()
                                     - passScene0.y());
        shrink->setProperty("visible", true);
        QTest::qWait(60);

        QVERIFY2(moved < 1.0,
                 qPrintable(QStringLiteral(
                     "the login card moved %1px when the form shrank %2px; a "
                     "field moving under the pointer is how a password lands "
                     "in the clear-text homeserver box")
                         .arg(moved).arg(formH0 - formH1)));
        QVERIFY2(userMoved < 1.0 && passMoved < 1.0,
                 qPrintable(QStringLiteral(
                     "the User field moved %1px and the Password field %2px")
                         .arg(userMoved).arg(passMoved)));
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
