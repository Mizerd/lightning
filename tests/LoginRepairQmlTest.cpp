
#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QPointer>
#include <QScopeGuard>

#include <functional>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/AppController.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"

namespace {
constexpr int kSignalTimeoutMs = 5000;
}

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
        QCoreApplication::processEvents();
        QTest::qWait(1);
        QCoreApplication::processEvents();
        const QPointF center = target->mapToScene(
            QPointF(target->width() / 2, target->height() / 2));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier,
                          center.toPoint());
        QCoreApplication::processEvents();
    }

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
        const QPointF center = button->mapToScene(
            QPointF(button->width() / 2, button->height() / 2));
        const auto *flick = item("loginFlick");
        QFAIL(qPrintable(QStringLiteral(
            "loginRepairConfirmDialog never opened: button=%1 center=(%2,%3) "
            "window=%4x%5 visible=%6 enabled=%7 contentY=%8 contentHeight=%9")
            .arg(button->objectName()).arg(center.x()).arg(center.y())
            .arg(m_window->width()).arg(m_window->height())
            .arg(button->isVisible()).arg(button->isEnabled())
            .arg(flick->property("contentY").toReal())
            .arg(flick->property("contentHeight").toReal())));
    }

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

    MockMatrixClient *mock() const
    {
        return qobject_cast<MockMatrixClient *>(
            m_controller->findChild<MatrixClient *>());
    }

    QQuickItem *shownLoginError() const
    {
        for (const char *name : { "loginErrorLabel", "loginErrorLabelNoForm" }) {
            QQuickItem *label = item(name);
            if (label && label->isVisible())
                return label;
        }
        return nullptr;
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

    void headingNamesTheRepairInsteadOfAddAccount()
    {
        auto *heading = item("loginScreenHeading");
        QVERIFY(heading);
        QCOMPARE(heading->property("text").toString(),
                 QStringLiteral("Sign in"));

        injectFailure(QStringLiteral("access_token_expired"),
                     QStringLiteral("@alice:example.org"),
                     QStringLiteral("https://example.org"));
        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());

        QCOMPARE(heading->property("text").toString(),
                 QStringLiteral("Fix this account"));
    }

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

        QVERIFY(!item("loginErrorLabel")->isVisible());

        auto *primary = item("loginRepairPrimaryAction");
        QVERIFY(primary);
        QTRY_VERIFY(primary->isVisible());
        QCOMPARE(primary->property("text").toString(),
                 QStringLiteral("Quarantine and rebuild"));
        QVERIFY(!item("loginRepairRetry")->isVisible());
        QVERIFY(!item("loginRepairRemoveAccount")->isVisible());
    }

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
            QStringLiteral("keyring_lost_session"),
            QStringLiteral("keyring_other_install"),
            QStringLiteral("cleanup_incomplete"),
        };
        for (const QString &code : codes) {
            injectFailure(code, QStringLiteral("@dana:example.org"),
                          QStringLiteral("https://example.org"));
            QTRY_VERIFY(item("loginRepairCard"));

            const bool helps = m_controller->localResetHelpsFor(code);
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
            QVERIFY2(item("loginRepairCard")->isVisible(),
                     qPrintable(QStringLiteral("reason '%1' renders no card")
                                    .arg(code)));
        }
    }

    void aSignInOfAnotherCopyAsksForASignInAndNoRebuild()
    {
        injectFailure(QStringLiteral("keyring_other_install"),
                      QStringLiteral("@gus:example.org"),
                      QStringLiteral("https://example.org"));
        auto *card = item("loginRepairCard");
        QTRY_VERIFY(card && card->isVisible());
        QVERIFY(!m_controller->localResetHelpsFor(QStringLiteral("keyring_other_install")));
        for (const char *name : {"loginRepairPrimaryAction", "loginRepairRetry",
                                 "loginRepairRetryKeyring"}) {
            auto *button = item(name);
            QVERIFY2(!button || !button->isVisible(), name);
        }
        const QString body = item("loginRepairBody")->property("text").toString();
        QVERIFY2(body.contains(QStringLiteral("@gus:example.org")), qPrintable(body));
        QVERIFY2(body.contains(QStringLiteral("another copy of Lightning"),
                               Qt::CaseInsensitive), qPrintable(body));
    }

    void aSignInTheKeyringLostOffersTryAgainAndNoRebuild()
    {
        injectFailure(QStringLiteral("keyring_lost_session"),
                      QStringLiteral("@fay:example.org"),
                      QStringLiteral("https://example.org"));
        auto *card = item("loginRepairCard");
        QTRY_VERIFY(card && card->isVisible());
        QVERIFY(!m_controller->localResetHelpsFor(QStringLiteral("keyring_lost_session")));
        auto *retry = item("loginRepairRetryKeyring");
        QVERIFY(retry);
        QVERIFY(retry->isVisible());
        for (const char *name : {"loginRepairPrimaryAction", "loginRepairRetry",
                                 "loginRepairRemoveAccount", "loginRepairOpenAccount"}) {
            auto *button = item(name);
            QVERIFY2(!button || !button->isVisible(), name);
        }
        const QString body = item("loginRepairBody")->property("text").toString();
        QVERIFY2(body.contains(QStringLiteral("@fay:example.org")), qPrintable(body));
        QVERIFY2(!body.contains(QStringLiteral("rebuild"), Qt::CaseInsensitive),
                 qPrintable(body));
        clearFailure();
    }

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

    void backIsHiddenWhileTheActiveAccountIsRevoked()
    {
        const QString active = QStringLiteral("@active-user:mock.local");
        // The seeded record must not outlive this case: on the mock backend a
        // logout while a saved account remains switches straight back into
        // it, and that restore lands in the NEXT case as a MainScreen. The
        // guard also runs when an assertion below returns early.
        auto forget = qScopeGuard([this, active] {
            m_controller->settings()->clearSessionForAccount(active);
            m_controller->auth()->logout();
            QTest::qWaitFor([this] {
                return !m_controller->loggedIn() && !m_controller->accountSwitching();
            }, kSignalTimeoutMs);
        });
        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("active-user"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());
        // The mock backend records no account, so give it one: the Back button
        // and the card both key on the ACTIVE saved account.
        m_controller->settings()->saveSession(QStringLiteral("https://mock.local"), active,
                                              QStringLiteral("MOCKDEVICE"),
                                              QStringLiteral("mock-token"), QString(),
                                              QStringLiteral("password"), QString());
        m_controller->settings()->setActiveAccountUserId(active);
        m_controller->showLogin();
        QCoreApplication::processEvents();
        QCOMPARE(m_controller->accounts()->activeUserId(), active);
        auto *back = item("backToAppButton");
        QVERIFY(back);
        QTRY_VERIFY(back->isVisible());

        injectFailure(QStringLiteral("access_token_revoked"), active,
                      QStringLiteral("https://mock.local"));
        QTRY_VERIFY(!back->isVisible());

        injectFailure(QStringLiteral("access_token_revoked"),
                      QStringLiteral("@other-account:example.org"),
                      QStringLiteral("https://example.org"));
        QTRY_VERIFY(back->isVisible());

        injectFailure(QStringLiteral("access_token_expired"), active,
                      QStringLiteral("https://mock.local"));
        QTRY_VERIFY(!back->isVisible());
        clearFailure();

        forget.dismiss();
        m_controller->settings()->clearSessionForAccount(active);
        m_controller->auth()->logout();
        QTRY_VERIFY(!m_controller->loggedIn() && !m_controller->accountSwitching());
        QVERIFY(!m_controller->settings()->savedAccountUserIds().contains(active));
        QCOMPARE(int(m_controller->currentScreen()), int(AppController::LoginScreen));
    }

    void accessTokenRevokedExposesNoDestructiveAction()
    {
        injectFailure(QStringLiteral("access_token_revoked"),
                     QStringLiteral("@henry:example.org"),
                     QStringLiteral("https://example.org"));

        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());

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
        QVERIFY2(bodyText.contains(QStringLiteral("new session"), Qt::CaseInsensitive)
                     && bodyText.contains(QStringLiteral("kept aside, not deleted"),
                                          Qt::CaseInsensitive),
                 qPrintable(bodyText));

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

    void accessTokenExpiredSaysTheSameSessionContinues()
    {
        injectFailure(QStringLiteral("access_token_expired"),
                      QStringLiteral("@henry:example.org"),
                      QStringLiteral("https://example.org"));

        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        auto *primary = item("loginRepairPrimaryAction");
        QVERIFY2(!primary || !primary->isVisible(),
                 "access_token_expired must not offer a destructive primary action");
        auto *retry = item("loginRepairRetry");
        QVERIFY(!retry || !retry->isVisible());

        auto *headline = item("loginRepairHeadline");
        QVERIFY(!headline
                || headline->property("text").toString()
                       != QStringLiteral("This session was signed out remotely"));
        auto *body = item("loginRepairBody");
        QVERIFY(body);
        const QString bodyText = body->property("text").toString();
        QVERIFY2(bodyText.contains(QStringLiteral("same session"), Qt::CaseInsensitive),
                 qPrintable(bodyText));
        QVERIFY2(!bodyText.contains(QStringLiteral("new session"), Qt::CaseInsensitive)
                     && !bodyText.contains(QStringLiteral("kept aside"),
                                           Qt::CaseInsensitive),
                 qPrintable(bodyText));
        auto *remove = item("loginRepairRemoveAccount");
        QVERIFY(remove);
        QVERIFY(remove->isVisible());
        clearFailure();
    }

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

        clickItem(cancelButton);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QVERIFY(card->isVisible());
        auto *result = item("loginRepairResult");
        QVERIFY(!result || !result->isVisible());
    }

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
        auto *result = item("loginRepairResult");
        QVERIFY(result);
        QTRY_VERIFY(result->isVisible());
        QVERIFY(!result->property("text").toString().isEmpty());
    }

    void addAccountFailureTargetsTheFailingAccountNotTheActiveOne()
    {
        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("active-user"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());

        m_controller->showLogin();
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

        injectFailure(QStringLiteral("session_without_device_id"),
                     QStringLiteral("@julia:example.org"),
                     QStringLiteral("https://example.org"));

        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(resetSpy.count(), 0);

        auto *newPrimary = item("loginRepairPrimaryAction");
        QVERIFY(newPrimary);
        QTRY_VERIFY(newPrimary->isVisible());
        QCOMPARE(item("userField")->property("text").toString(),
                 QStringLiteral("@julia:example.org"));
    }

    void loginScreenNeverCallsTheRawTextResetPath()
    {
        QFile file(QStringLiteral(QML_DIR "/LoginScreen.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(!text.contains(QStringLiteral("resetLocalRustSession(")));
    }

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

    void repairCardIsRevealedWhenTheScrollExtentArrivesLate()
    {
        auto *flick = item("loginFlick");
        QVERIFY(flick);
        const QSize originalSize = m_window->size();
        auto restore = qScopeGuard([this, flick, originalSize] {
            clearFailure();
            QQmlExpression binding(qmlContext(flick), flick, QStringLiteral(
                "contentHeight = Qt.binding(function() { "
                "return panel.y + panel.implicitHeight + AppTheme.spacingXL })"));
            binding.evaluate();
            m_window->resize(originalSize);
        });
        m_window->resize(640, 420);
        QQmlExpression hold(qmlContext(flick), flick,
                            QStringLiteral("contentHeight = 0; contentY = 0"));
        hold.evaluate();
        QVERIFY2(!hold.hasError(), qPrintable(hold.error().toString()));
        injectFailure(QStringLiteral("access_token_revoked"),
                      QStringLiteral("@late-layout:example.org"),
                      QStringLiteral("https://example.org"));
        auto *remove = item("loginRepairRemoveAccount");
        QVERIFY(remove);
        QTest::qWait(100);
        QVERIFY(remove->isVisible());
        const auto centerInViewport = [remove, flick] {
            return remove->mapToItem(flick,
                QPointF(remove->width() / 2, remove->height() / 2));
        };
        QVERIFY2(centerInViewport().y() > flick->height(),
                 qPrintable(QStringLiteral("clipped premise: y=%1 viewport=%2 extent=%3")
                     .arg(centerInViewport().y()).arg(flick->height())
                     .arg(flick->property("contentHeight").toReal())));
        QCOMPARE(flick->property("contentY").toReal(), 0.0);

        const auto *panel = item("loginPanel");
        QVERIFY(panel);
        flick->setProperty("contentHeight", panel->y()
            + panel->property("implicitHeight").toReal() + 100);
        QTRY_VERIFY(centerInViewport().y() > 0
                    && centerInViewport().y() < flick->height());
        openDialogVia(remove);
        auto *dialog = waitForObject("loginRepairConfirmDialog");
        QVERIFY(dialog);
        auto *cancel = waitForItem("loginRepairCancel");
        QVERIFY(cancel);
        clickItem(cancel);
        QTRY_VERIFY(!dialog->property("visible").toBool());
    }

    // A sign-in that fails without raising a card must say why even while a
    // card about another failure is up. RustSdkMatrixClient::login()'s
    // "already signed in on this device" redirect answers with loginFailed()
    // and no card, and both error labels were hidden by any rendered card, so
    // that answer was set and never shown (2026-09-30 campaign, "a redirected
    // sign-in says nothing"). The mock's "mock-fail" password stands in for
    // it: an interactive attempt answered by loginFailed() alone. On the
    // unfixed screen the QTRY below times out with both labels hidden.
    void aSignInErrorTheCardDoesNotExplainIsShownBesideIt()
    {
        const QString server = QStringLiteral("https://example.org");
        injectFailure(QStringLiteral("access_token_revoked"),
                      QStringLiteral("@alice:example.org"), server);
        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        QVERIFY(!shownLoginError());

        m_controller->auth()->login(server, QStringLiteral("@bob:example.org"),
                                    QStringLiteral("mock-fail"));
        QTRY_VERIFY(!m_controller->auth()->isLoggingIn());
        const QString error = m_controller->auth()->lastError();
        QVERIFY(!error.isEmpty());

        QVERIFY(card->isVisible());
        QVERIFY(!m_controller->signInErrorExplainedByCard());
        QTRY_VERIFY2(shownLoginError() != nullptr,
                     "a sign-in error the card does not explain was hidden "
                     "behind it");
        QCOMPARE(shownLoginError()->property("text").toString(), error);
    }

    void aRefusalAfterACardRaisedAloneIsShown()
    {
        injectFailure(QStringLiteral("removal_incomplete"),
                      QStringLiteral("@gone:example.org"),
                      QStringLiteral("https://example.org"));
        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        QTest::qWait(20);

        m_controller->auth()->login(QStringLiteral("you@example.org"),
                                    QStringLiteral("@bob:example.org"),
                                    QStringLiteral("unused"));
        const QString error = m_controller->auth()->lastError();
        QVERIFY(!error.isEmpty());
        QVERIFY(!m_controller->auth()->isLoggingIn());
        QVERIFY(card->isVisible());
        QTRY_VERIFY2(shownLoginError() != nullptr,
                     "a refusal after a card raised alone was hidden behind it");
        QCOMPARE(shownLoginError()->property("text").toString(), error);
    }

    // The other half: the error a failure sends WITH its card is that card's
    // own text and stays behind it, including when the failure re-raises the
    // card already showing (no change, so no localSessionFailureChanged()).
    // Raised inside the failure's own call, as in production
    // (failWithBlockReason() raises the card, then sends loginFailed()): the
    // mock moves to Error immediately before its loginFailed(), and the hook
    // raises there. Fails if the in-flight mark is set after
    // setLocalSessionFailure()'s no-change return.
    void anErrorThatCameWithItsCardStaysBehindIt()
    {
        const QString server = QStringLiteral("https://example.org");
        const QString alice = QStringLiteral("@alice:example.org");
        const QString reason = QStringLiteral("secret_backend_unavailable");
        injectFailure(reason, alice, server);
        auto *card = item("loginRepairCard");
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        QTest::qWait(20);

        int raised = 0;
        const auto hook = connect(
            mock(), &MatrixClient::connectionStateChanged, this,
            [&](MatrixClient::ConnectionState state) {
                if (state != MatrixClient::Error)
                    return;
                ++raised;
                m_controller->setLocalSessionFailure(reason, alice, server);
            });
        auto unhook = qScopeGuard([hook] { QObject::disconnect(hook); });

        m_controller->auth()->login(server, alice, QStringLiteral("mock-fail"));
        QTRY_VERIFY(!m_controller->auth()->isLoggingIn());
        QCOMPARE(raised, 1);
        QVERIFY(!m_controller->auth()->lastError().isEmpty());
        QCoreApplication::processEvents();

        QVERIFY(card->isVisible());
        QVERIFY(m_controller->signInErrorExplainedByCard());
        QVERIFY2(!shownLoginError(),
                 "the error the card itself explains was shown beside it");
    }

    void anErrorBesideTheCardSurvivesTheScreenBeingRebuilt()
    {
        const QString server = QStringLiteral("https://example.org");
        injectFailure(QStringLiteral("access_token_revoked"),
                      QStringLiteral("@alice:example.org"), server);
        QTRY_VERIFY(item("loginRepairCard")->isVisible());
        m_controller->auth()->login(server, QStringLiteral("@bob:example.org"),
                                    QStringLiteral("mock-fail"));
        QTRY_VERIFY(!m_controller->auth()->isLoggingIn());
        const QString error = m_controller->auth()->lastError();
        QTRY_VERIFY(shownLoginError() != nullptr);

        QPointer<QQuickItem> before = item("loginScreen");
        QVERIFY(before);
        QQuickItem *loader = before->parentItem();
        QVERIFY(loader);
        QVERIFY(loader->setProperty("active", false));
        QTRY_VERIFY(before.isNull());
        QVERIFY(loader->setProperty("active", true));
        QTRY_VERIFY(item("loginScreen") != nullptr);

        QTRY_VERIFY(item("loginRepairCard")->isVisible());
        QTRY_VERIFY2(shownLoginError() != nullptr,
                     "the rebuilt screen hid an error the card does not explain");
        QCOMPARE(shownLoginError()->property("text").toString(), error);
    }

    // Add-account whose return to the previous account is refused at once (a
    // keyring that cannot answer, a store that is gone: the Rust backend
    // refuses those inside restoreSession() and raises the card first).
    //  * The return restore is tried ONCE. Its failure re-entered the same
    //    handler with the account to return to still set, which restored it
    //    again: a recursion until the stack ran out, bounded here only by
    //    the mock's failure count (26 calls on the unfixed handler).
    //  * The card's own text is not shown a second time beside it.
    //  * "Back" is hidden: it could only restore that account again, and
    //    showMain() refused at once stays on this screen instead of opening
    //    a shell with no session.
    void anAddAccountWhoseReturnIsRefusedAtOnceStopsWithTheCard()
    {
        const QString active = QStringLiteral("@return-user:mock.local");
        const QString server = QStringLiteral("https://mock.local");
        const QString reason = QStringLiteral("saved_session_without_store");
        MockMatrixClient *client = mock();
        QVERIFY(client);
        QMetaObject::Connection hook;
        int errors = 0;
        auto forget = qScopeGuard([this, active, client, &hook] {
            QObject::disconnect(hook);
            client->setRestoreDelayForTest(0);
            client->failRestoresForTest(0);
            clearFailure();
            m_controller->settings()->clearSessionForAccount(active);
            m_controller->auth()->logout();
            QTest::qWaitFor([this] {
                return !m_controller->loggedIn() && !m_controller->accountSwitching();
            }, kSignalTimeoutMs);
        });
        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(server, QStringLiteral("return-user"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());
        m_controller->settings()->saveSession(server, active,
                                              QStringLiteral("MOCKDEVICE"),
                                              QStringLiteral("mock-token"), QString(),
                                              QStringLiteral("password"), QString());
        m_controller->settings()->setActiveAccountUserId(active);
        m_controller->showLogin();
        QCoreApplication::processEvents();
        auto *back = item("backToAppButton");
        QVERIFY(back);
        QTRY_VERIFY(back->isVisible());

        client->setRestoreDelayForTest(-1);
        client->failRestoresForTest(25);
        hook = connect(client, &MatrixClient::connectionStateChanged, this,
                       [&](MatrixClient::ConnectionState state) {
                           if (state == MatrixClient::Error && ++errors == 2)
                               m_controller->setLocalSessionFailure(reason, active,
                                                                    server);
                       });
        const int restoresBefore = client->restoreCallsForTest();
        m_controller->auth()->login(server, QStringLiteral("newcomer"),
                                    QStringLiteral("mock-fail"));
        QTRY_VERIFY(!m_controller->auth()->isLoggingIn());
        QCoreApplication::processEvents();

        QCOMPARE(client->restoreCallsForTest() - restoresBefore, 1);
        QVERIFY(!m_controller->loggedIn());
        QCOMPARE(m_controller->currentScreen(), AppController::LoginScreen);
        QTRY_VERIFY(item("loginRepairCard")->isVisible());
        QCOMPARE(m_controller->localSessionFailureUserId(), active);
        QCOMPARE(m_controller->auth()->lastError(),
                 QStringLiteral("mock: restore rejected"));
        QVERIFY(m_controller->signInErrorExplainedByCard());
        QVERIFY2(!shownLoginError(), "the card's own text was shown twice");
        QTRY_VERIFY2(!back->isVisible(),
                     "Back offered for the account whose restore just failed");

        QVERIFY(!m_controller->showMain());
        QCOMPARE(client->restoreCallsForTest() - restoresBefore, 2);
        QCOMPARE(m_controller->currentScreen(), AppController::LoginScreen);
        QVERIFY(!m_controller->auth()->lastError().isEmpty());
    }

    void anAttemptDuringAPendingReturnRestoreFailsAsItsOwn()
    {
        const QString active = QStringLiteral("@pending-user:mock.local");
        const QString server = QStringLiteral("https://mock.local");
        MockMatrixClient *client = mock();
        QVERIFY(client);
        auto forget = qScopeGuard([this, active, client] {
            client->setRestoreDelayForTest(0);
            client->failRestoresForTest(0);
            QTest::qWait(1200);
            clearFailure();
            m_controller->settings()->clearSessionForAccount(active);
            m_controller->auth()->logout();
            QTest::qWaitFor([this] {
                return !m_controller->loggedIn() && !m_controller->accountSwitching();
            }, kSignalTimeoutMs);
        });
        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(server, QStringLiteral("pending-user"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());
        m_controller->settings()->saveSession(server, active,
                                              QStringLiteral("MOCKDEVICE"),
                                              QStringLiteral("mock-token"), QString(),
                                              QStringLiteral("password"), QString());
        m_controller->settings()->setActiveAccountUserId(active);
        m_controller->showLogin();

        client->setRestoreDelayForTest(1000);
        const int restoresBefore = client->restoreCallsForTest();
        m_controller->auth()->login(server, QStringLiteral("newcomer"),
                                    QStringLiteral("mock-fail"));
        QTRY_COMPARE(client->restoreCallsForTest() - restoresBefore, 1);
        QVERIFY(!m_controller->loggedIn());

        QSignalSpy notices(m_controller, &AppController::errorReported);
        m_controller->auth()->login(server, QStringLiteral("newcomer"),
                                    QStringLiteral("mock-fail"));
        QVERIFY(m_controller->auth()->isLoggingIn());
        QTRY_VERIFY(!m_controller->auth()->isLoggingIn());
        const QString reason = QStringLiteral("mock: invalid credentials");

        QCOMPARE(client->restoreCallsForTest() - restoresBefore, 2);
        bool sawReason = false;
        for (const auto &call : notices) {
            if (call.at(0).toString() == reason)
                sawReason = true;
        }
        QVERIFY2(sawReason, "the attempt's failure was taken for the return "
                            "restore's and not reported");
        QCOMPARE(m_controller->auth()->lastError(), reason);
        QTRY_VERIFY(shownLoginError() != nullptr);
        QCOMPARE(shownLoginError()->property("text").toString(), reason);

        QTRY_VERIFY(m_controller->loggedIn());
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
