// The sign-in screen, loaded for real: the server first, then only the ways
// that server lets people in (docs/feature-contracts.md, "Sign-in screen").
//
// LoginScreen runs against a small `app` object and the real AuthManager over
// a backend that answers discovery the way RustSdkMatrixClient does (details,
// then methods, then providers a turn later), so every state is reachable
// without a server.

#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>

#include <functional>

namespace {

class DiscoveringClient : public MockMatrixClient
{
    Q_OBJECT
public:
    bool supportsOAuthLogin() const override { return true; }
    bool supportsSsoLogin() const override { return true; }
    void discoverAuthMethods(const QString &homeserver) override
    {
        discoveries.append(homeserver);
    }
    void requestSsoProviders(const QString &homeserver) override
    {
        providerRequests.append(homeserver);
    }
    void beginOAuthLogin(const QString &homeserver) override
    {
        oauthStarts.append(homeserver);
    }
    void beginOAuthSignUp(const QString &homeserver) override
    {
        signUps.append(homeserver);
    }
    void beginSsoLogin(const QString &homeserver, const QString &idpId) override
    {
        ssoStarts.append(homeserver + QLatin1Char('|') + idpId);
    }
    void cancelOAuthLogin() override {}
    void cancelSsoLogin() override
    {
        Q_EMIT loginFailed(QStringLiteral("Sign-in was cancelled."));
    }

    void answer(const QString &homeserver, bool password, bool oauth, bool sso,
                bool reachable = true,
                const QString &resolved = QString(),
                bool canCreate = false, const QString &account = QString())
    {
        Q_EMIT authDiscoveryDetails(homeserver, {
            { QStringLiteral("reachable"), reachable },
            { QStringLiteral("resolvedHomeserver"), resolved },
            { QStringLiteral("oauthCanCreate"), canCreate },
            { QStringLiteral("accountManagementUrl"), account },
        });
        Q_EMIT authMethodsDiscovered(homeserver, password, oauth, sso);
    }
    void providers(const QString &homeserver, const QVariantList &list)
    {
        Q_EMIT ssoProvidersReceived(homeserver, true, list);
    }
    void launchFails() { Q_EMIT browserLaunchFailed(); }

    QStringList discoveries;
    QStringList providerRequests;
    QStringList oauthStarts;
    QStringList signUps;
    QStringList ssoStarts;
};

class FakeSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString loginHomeserverPrefill MEMBER prefill NOTIFY prefillChanged)
public:
    QString prefill = QStringLiteral("https://example.org");
Q_SIGNALS:
    void prefillChanged();
};

// Only what LoginScreen.qml reads from `app`.
class FakeApp : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *auth READ authObject CONSTANT)
    Q_PROPERTY(QObject *settings READ settingsObject CONSTANT)
    Q_PROPERTY(QObject *accounts READ accountsObject CONSTANT)
    Q_PROPERTY(QString backendName READ backendName CONSTANT)
    Q_PROPERTY(bool loggedIn READ loggedIn CONSTANT)
    Q_PROPERTY(QString localSessionFailureReasonCode MEMBER failureReason
                   NOTIFY localSessionFailureChanged)
    Q_PROPERTY(QString localSessionFailureUserId MEMBER failureUserId
                   NOTIFY localSessionFailureChanged)
    Q_PROPERTY(QString localSessionFailureHomeserver MEMBER failureHomeserver
                   NOTIFY localSessionFailureChanged)
    Q_PROPERTY(QStringList accountRemovalLeftovers MEMBER leftovers
                   NOTIFY localSessionFailureChanged)
public:
    explicit FakeApp(AuthManager *auth) : m_auth(auth) {}
    QObject *authObject() const { return m_auth; }
    QObject *settingsObject() { return &settings; }
    QObject *accountsObject() const { return nullptr; }
    QString backendName() const { return QStringLiteral("rust"); }
    bool loggedIn() const { return false; }
    QString empty() const { return {}; }

    Q_INVOKABLE bool localResetHelpsFor(const QString &) const { return false; }
    Q_INVOKABLE void showMain() {}
    Q_INVOKABLE void switchToAccount(const QString &userId) { switched.append(userId); }
    Q_INVOKABLE void repairLocalSession() {}
    Q_INVOKABLE void removeAccount(const QString &userId) { removed.append(userId); }
    Q_INVOKABLE void retryAccountRemoval() { ++retries; }

    void fail(const QString &reason, const QString &userId, const QString &homeserver)
    {
        failureReason = reason;
        failureUserId = userId;
        failureHomeserver = homeserver;
        Q_EMIT localSessionFailureChanged();
    }
    Q_INVOKABLE void copySessionDiagnostics() {}

    FakeSettings settings;
    QString failureReason;
    QString failureUserId;
    QString failureHomeserver;
    QStringList switched;
    QStringList removed;
    QStringList leftovers;
    int retries = 0;

Q_SIGNALS:
    void localSessionFailureChanged();
    void localRustStoreResetResult(bool ok, const QString &message);

private:
    AuthManager *m_auth;
};

QVariantMap provider(const QString &id, const QString &name, const QString &brand)
{
    return { { QStringLiteral("id"), id }, { QStringLiteral("name"), name },
             { QStringLiteral("icon"), QString() }, { QStringLiteral("brand"), brand } };
}

const QString kServer = QStringLiteral("https://example.org");

} // namespace

class LoginScreenQmlTest : public QObject
{
    Q_OBJECT

    DiscoveringClient *m_client = nullptr;
    AuthManager *m_auth = nullptr;
    FakeApp *m_app = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;
    QQuickItem *m_root = nullptr;
    QStringList m_warnings;

    QQuickItem *find(const QString &name) const
    {
        std::function<QQuickItem *(QQuickItem *)> walk = [&](QQuickItem *node) -> QQuickItem * {
            if (node->objectName() == name)
                return node;
            for (QQuickItem *child : node->childItems()) {
                if (QQuickItem *hit = walk(child))
                    return hit;
            }
            return nullptr;
        };
        return walk(m_root);
    }
    QList<QQuickItem *> findAll(const QString &name) const
    {
        QList<QQuickItem *> out;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *node) {
            if (node->objectName() == name)
                out.append(node);
            for (QQuickItem *child : node->childItems())
                walk(child);
        };
        walk(m_root);
        return out;
    }
    bool shown(const QString &name) const
    {
        QQuickItem *item = find(name);
        return item && item->isVisible();
    }
    QString text(const QString &name) const
    {
        QQuickItem *item = find(name);
        return item ? item->property("text").toString() : QString();
    }
    void click(const QString &name)
    {
        QQuickItem *item = find(name);
        QVERIFY2(item, qPrintable(name));
        QVERIFY2(QMetaObject::invokeMethod(item, "clicked"), qPrintable(name));
    }

    // Where an item is drawn once the layouts have run. A layout places its
    // children when the window polishes, on its next frame, so a read straight
    // after a change can see the old places: theFieldsStayPutWhenTheWaysChange
    // compared an unplaced userField (y 117, its form still at y 0) with a
    // placed one (y 328) whenever that frame fell between its two reads, which
    // the timing of a parallel ctest made likely. grabWindow() polishes and
    // renders one.
    QPointF placed(QQuickItem *item)
    {
        QCoreApplication::processEvents();
        (void)m_window->grabWindow();
        return item->mapToScene(QPointF(0, 0));
    }

    // A new server typed into the field and Enter pressed (Qt emits
    // accepted, then editingFinished).
    void typeServer(const QString &server)
    {
        QQuickItem *field = find(QStringLiteral("homeserverField"));
        QVERIFY(field);
        if (!field->isVisible())
            click(QStringLiteral("serverChangeButton"));
        field->setProperty("text", server);
        QMetaObject::invokeMethod(field, "accepted");
        QMetaObject::invokeMethod(field, "editingFinished");
        QCoreApplication::processEvents();
    }

private slots:
    void init()
    {
        m_warnings.clear();
        m_client = new DiscoveringClient;
        m_auth = new AuthManager(m_client);
        m_app = new FakeApp(m_auth);
        m_engine = new QQmlApplicationEngine;
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const QQmlError &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), m_app);
        QSignalSpy created(m_engine, &QQmlApplicationEngine::objectCreated);
        m_engine->loadFromModule(QStringLiteral("MatrixClient"), QStringLiteral("LoginScreen"));
        if (created.isEmpty())
            QVERIFY(created.wait(8000));
        m_root = qobject_cast<QQuickItem *>(created.at(0).at(0).value<QObject *>());
        QVERIFY2(m_root, "LoginScreen.qml failed to load; the warning above names why");
        m_window = new QQuickWindow;
        m_window->resize(900, 900);
        m_root->setParentItem(m_window->contentItem());
        m_root->setSize(QSizeF(900, 900));
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(m_window));
        // The prefill is asked about as the screen opens.
        QCOMPARE(m_client->discoveries, QStringList{ kServer });
    }

    void cleanup()
    {
        delete m_window;
        m_window = nullptr;
        m_root = nullptr;
        delete m_engine;
        delete m_app;
        delete m_auth;
        delete m_client;
    }

    // A server with its own sign-in page (MAS, matrix.org): that page is the
    // one way in, as Element shows it, with sign-up and the account page.
    void aServerWithItsOwnPageShowsOnlyThatWayIn()
    {
        QVERIFY(shown(QStringLiteral("homeserverField")));
        QCOMPARE(text(QStringLiteral("serverStatusLabel")), QStringLiteral("Checking…"));
        m_client->answer(kServer, true, true, true, true,
                         QStringLiteral("https://matrix-client.example.org/"), true,
                         QStringLiteral("https://account.example.org/account/"));
        QCoreApplication::processEvents();

        QVERIFY(shown(QStringLiteral("serverSummary")));
        QVERIFY(!shown(QStringLiteral("homeserverField")));
        QCOMPARE(text(QStringLiteral("serverNameLabel")), QStringLiteral("example.org"));
        QCOMPARE(text(QStringLiteral("serverStatusLabel")), QStringLiteral("Found"));
        QVERIFY(!shown(QStringLiteral("serverNotEncryptedChip")));

        QVERIFY(shown(QStringLiteral("browserLoginButton")));
        QCOMPARE(text(QStringLiteral("browserLoginButton")),
                 QStringLiteral("Continue with example.org"));
        QCOMPARE(find(QStringLiteral("browserLoginButton"))->property("kind").toString(),
                 QStringLiteral("primary"));
        QVERIFY(!shown(QStringLiteral("passwordForm")));
        QVERIFY(!shown(QStringLiteral("ssoLoginButton")));
        QVERIFY(!shown(QStringLiteral("ssoProviderGrid")));
        QVERIFY(shown(QStringLiteral("createAccountButton")));
        QVERIFY(shown(QStringLiteral("forgotPasswordButton")));
        QVERIFY(m_client->providerRequests.isEmpty());

        click(QStringLiteral("createAccountButton"));
        QCOMPARE(m_client->signUps, QStringList{ kServer });
        QVERIFY(m_client->oauthStarts.isEmpty());
        // Waiting: only the way out is left.
        QVERIFY(shown(QStringLiteral("browserLoginCancelButton")));
        QVERIFY(!shown(QStringLiteral("browserLoginButton")));
        QVERIFY(!shown(QStringLiteral("accountLinks")));
    }

    // Password plus several providers: the form first, then "Or continue
    // with" and a grid of named buttons, each with the provider's logo or a
    // neutral glyph. Never a letter.
    void aPasswordServerWithProvidersShowsBothWithRealLogos()
    {
        m_client->answer(kServer, true, false, true, true, QStringLiteral("https://example.org/"));
        QTRY_COMPARE(m_client->providerRequests, QStringList{ kServer });
        m_client->providers(kServer, {
            provider(QStringLiteral("oidc-google"), QStringLiteral("Google"), QStringLiteral("google")),
            provider(QStringLiteral("oidc-gitlab"), QStringLiteral("GitLab"), QStringLiteral("gitlab")),
            // A brand token that names an Object property must not reach
            // the prototype: it gets the neutral glyph. "__proto__" is the
            // one a plain lookup would turn into a non-empty path (an object
            // coerces to a string; "constructor", a function, does not).
            provider(QStringLiteral("oidc-kc"), QStringLiteral("Keycloak"),
                     QStringLiteral("constructor")),
            provider(QStringLiteral("oidc-proto"), QStringLiteral("Example"),
                     QStringLiteral("__proto__")),
        });
        QCoreApplication::processEvents();

        QVERIFY(shown(QStringLiteral("passwordForm")));
        QVERIFY(shown(QStringLiteral("userField")));
        QVERIFY(shown(QStringLiteral("passField")));
        QVERIFY(!shown(QStringLiteral("browserLoginButton")));
        QCOMPARE(text(QStringLiteral("loginAlternativesDivider")),
                 QStringLiteral("Or continue with"));
        QVERIFY(shown(QStringLiteral("ssoProviderGrid")));
        QVERIFY(!shown(QStringLiteral("ssoLoginButton")));

        const QStringList names = { QStringLiteral("Google"), QStringLiteral("GitLab"),
                                    QStringLiteral("Keycloak"), QStringLiteral("Example") };
        const QStringList brands = { QStringLiteral("google"), QStringLiteral("gitlab"),
                                     QStringLiteral("generic"), QStringLiteral("generic") };
        for (int i = 0; i < names.size(); ++i) {
            QQuickItem *button = find(QStringLiteral("ssoProviderButton%1").arg(i));
            QVERIFY(button && button->isVisible());
            QCOMPARE(button->property("text").toString(), names.at(i));
            QQuickItem *logo = nullptr;
            for (QQuickItem *child : button->childItems())
                if (child->objectName() == QLatin1String("providerLogo"))
                    logo = child;
            QVERIFY2(logo, qPrintable(names.at(i)));
            QVERIFY(logo->isVisible());
            QCOMPARE(logo->property("shownBrand").toString(), brands.at(i));
        }
        QVERIFY(!shown(QStringLiteral("accountLinks")));

        click(QStringLiteral("ssoProviderButton1"));
        QCOMPARE(m_client->ssoStarts, QStringList{ kServer + QStringLiteral("|oidc-gitlab") });
        // The browser is in charge: the password form steps aside.
        QVERIFY(!shown(QStringLiteral("passwordForm")));
        QVERIFY(shown(QStringLiteral("browserLoginCancelButton")));
        // A browser that would not start is said during the wait, although
        // the form (and its error line) is hidden.
        m_client->launchFails();
        QCoreApplication::processEvents();
        QVERIFY(shown(QStringLiteral("loginErrorLabelNoForm")));
        QVERIFY(text(QStringLiteral("loginErrorLabelNoForm"))
                    .contains(QStringLiteral("web browser")));
    }

    // Single sign-on only (matrix.debian.social): no password form, and the
    // one provider is the main button.
    void aSingleSignOnServerHasNoPasswordForm()
    {
        m_client->answer(kServer, false, false, true, true, QStringLiteral("https://example.org/"));
        QTRY_COMPARE(m_client->providerRequests, QStringList{ kServer });
        m_client->providers(kServer, { provider(QStringLiteral("oidc-salsa"),
                                                QStringLiteral("Salsa"), QString()) });
        QCoreApplication::processEvents();

        QVERIFY(!shown(QStringLiteral("passwordForm")));
        QQuickItem *button = find(QStringLiteral("ssoProviderButton0"));
        QVERIFY(button && button->isVisible());
        QCOMPARE(button->property("text").toString(), QStringLiteral("Continue with Salsa"));
        QCOMPARE(button->property("kind").toString(), QStringLiteral("primary"));
        QVERIFY(text(QStringLiteral("ssoLoginHint")).contains(QStringLiteral("Salsa")));
    }

    void anUnreachableServerSaysSoAndOffersARetry()
    {
        m_client->answer(kServer, false, false, false, false);
        QCoreApplication::processEvents();
        QCOMPARE(text(QStringLiteral("serverStatusLabel")),
                 QStringLiteral("Can't reach this server. Check the address."));
        QVERIFY(shown(QStringLiteral("homeserverField")));
        QVERIFY(shown(QStringLiteral("serverRetryButton")));
        QVERIFY(!shown(QStringLiteral("passwordForm")));
        QVERIFY(!shown(QStringLiteral("browserLoginButton")));

        click(QStringLiteral("serverRetryButton"));
        QCOMPARE(m_client->discoveries.size(), 2);
        QCOMPARE(text(QStringLiteral("serverStatusLabel")), QStringLiteral("Checking…"));
    }

    void textThatIsNotAnAddressIsNotAsked()
    {
        m_client->answer(kServer, true, false, false);
        QCoreApplication::processEvents();
        typeServer(QStringLiteral("alice:secret@example.org"));
        QCOMPARE(m_client->discoveries.size(), 1);
        QCOMPARE(text(QStringLiteral("serverStatusLabel")),
                 QStringLiteral("That is not a server address. Enter one like matrix.org."));
        QVERIFY(!shown(QStringLiteral("serverRetryButton")));
        QVERIFY(!shown(QStringLiteral("passwordForm")));
    }

    // The warning follows the base URL discovery resolved, and a server on
    // this machine never gets it.
    void thePlainHttpWarningFollowsTheResolvedUrl()
    {
        m_client->answer(kServer, true, false, false, true,
                         QStringLiteral("http://192.168.1.20:8008/"));
        QCoreApplication::processEvents();
        QVERIFY(shown(QStringLiteral("serverNotEncryptedChip")));
        // A warning, not a gate: the form is still there.
        QVERIFY(shown(QStringLiteral("passwordForm")));

        typeServer(QStringLiteral("http://localhost:8008"));
        m_client->answer(QStringLiteral("http://localhost:8008"), true, false, false, true,
                         QStringLiteral("http://localhost:8008/"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(shown(QStringLiteral("serverSummary")));
        QVERIFY(!shown(QStringLiteral("serverNotEncryptedChip")));
        QCOMPARE(text(QStringLiteral("serverNameLabel")), QStringLiteral("http://localhost:8008"));
    }

    // "Change" turns the summary back into the field; Escape leaves it as it
    // was, without asking the server again.
    void changeEditsTheServerAndEscapeKeepsIt()
    {
        m_client->answer(kServer, true, false, false);
        QCoreApplication::processEvents();
        QVERIFY(shown(QStringLiteral("serverSummary")));
        QQuickItem *field = find(QStringLiteral("homeserverField"));
        const QPointF fieldAt = placed(field);
        const QPointF userAt = placed(find(QStringLiteral("userField")));
        QVERIFY2(userAt.y() > fieldAt.y(), "the form was read before it was laid out");

        click(QStringLiteral("serverChangeButton"));
        QVERIFY(field->isVisible());
        QTRY_VERIFY(field->hasActiveFocus());
        field->setProperty("text", QStringLiteral("somewhere.else"));
        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(shown(QStringLiteral("serverSummary")));
        QCOMPARE(field->property("text").toString(), kServer);
        QCOMPARE(m_client->discoveries.size(), 1);
        // Nothing moved while the row swapped.
        QCOMPARE(placed(field), fieldAt);
        QCOMPARE(placed(find(QStringLiteral("userField"))), userAt);
    }

    // The fields never move when the choices below them change: the card is
    // anchored at the top, so a server answering cannot slide it.
    void theFieldsStayPutWhenTheWaysChange()
    {
        QQuickItem *field = find(QStringLiteral("homeserverField"));
        const QPointF before = placed(field);
        m_client->answer(kServer, true, false, true);
        QTRY_COMPARE(m_client->providerRequests, QStringList{ kServer });
        m_client->providers(kServer, {
            provider(QStringLiteral("a"), QStringLiteral("A"), QString()),
            provider(QStringLiteral("b"), QStringLiteral("B"), QString()),
            provider(QStringLiteral("c"), QStringLiteral("C"), QString()),
        });
        QCoreApplication::processEvents();
        const QPointF userAt = placed(find(QStringLiteral("userField")));
        QVERIFY2(userAt.y() > before.y(), "the form was read before it was laid out");
        QCOMPARE(placed(find(QStringLiteral("serverSummary"))), before);

        // The server changes and everything below goes; nothing above moves.
        typeServer(QStringLiteral("other.example"));
        QCOMPARE(placed(field), before);
        m_client->answer(QStringLiteral("https://other.example"), true, false, false);
        QCoreApplication::processEvents();
        QTRY_VERIFY(shown(QStringLiteral("userField")));
        QCOMPARE(placed(find(QStringLiteral("userField"))), userAt);
    }

    // A session already on this device that blocks a new sign-in (D6: one a
    // failed restore left behind) always has a way out on this screen: open
    // it, or remove it from this device and sign in again.
    void aBlockingSessionOffersOpenAndRemove()
    {
        const QString dave = QStringLiteral("@dave:example.org");
        m_app->fail(QStringLiteral("existing_store_requires_restore"), dave, kServer);
        QCoreApplication::processEvents();
        QVERIFY(shown(QStringLiteral("loginRepairCard")));
        QVERIFY(text(QStringLiteral("loginRepairBody")).contains(dave));
        QVERIFY(shown(QStringLiteral("loginRepairOpenAccount")));
        QVERIFY(shown(QStringLiteral("loginRepairRemoveAccount")));
        // Nothing destructive is the main action.
        QVERIFY(!shown(QStringLiteral("loginRepairPrimaryAction")));

        click(QStringLiteral("loginRepairOpenAccount"));
        QCOMPARE(m_app->switched, QStringList{ dave });

        // Remove goes through the confirmation that names the account.
        click(QStringLiteral("loginRepairRemoveAccount"));
        QObject *dialog = m_window->contentItem()->findChild<QObject *>(
            QStringLiteral("loginRepairConfirmDialog"));
        if (!dialog)
            dialog = m_root->findChild<QObject *>(QStringLiteral("loginRepairConfirmDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QQuickItem *confirm = nullptr;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *node) {
            if (node->objectName() == QLatin1String("loginRepairConfirmAction"))
                confirm = node;
            for (QQuickItem *child : node->childItems())
                walk(child);
        };
        walk(m_window->contentItem());
        QVERIFY(confirm);
        QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
        QCOMPARE(m_app->removed, QStringList{ dave });
    }

    // A removal that left files behind: the card says what is still here and
    // tries again, with no button that no longer does anything.
    void aPartlyFailedRemovalSaysWhatIsLeftAndRetries()
    {
        const QString dave = QStringLiteral("@dave:example.org");
        m_app->leftovers = { QStringLiteral("/data/dave_example.org/matrix-rust-sdk-store") };
        m_app->fail(QStringLiteral("removal_incomplete"), dave, kServer);
        QCoreApplication::processEvents();
        QVERIFY(shown(QStringLiteral("loginRepairCard")));
        QVERIFY(text(QStringLiteral("loginRepairBody")).contains(dave));
        QVERIFY(text(QStringLiteral("loginRepairBody"))
                    .contains(QStringLiteral("matrix-rust-sdk-store")));
        QVERIFY(shown(QStringLiteral("loginRepairRetryRemoval")));
        QVERIFY(!shown(QStringLiteral("loginRepairOpenAccount")));
        QVERIFY(!shown(QStringLiteral("loginRepairRemoveAccount")));
        QVERIFY(!shown(QStringLiteral("loginRepairPrimaryAction")));
        click(QStringLiteral("loginRepairRetryRemoval"));
        QCOMPARE(m_app->retries, 1);
    }

    // A card that appears below the sign-in form is scrolled into view, its
    // buttons included: at the default window height they ended up below
    // the fold (LoginRepairQmlTest::accessTokenRevokedExposesNoDestructiveAction
    // clicked there and missed).
    void aRepairCardIsScrolledIntoView()
    {
        m_root->setSize(QSizeF(900, 520));
        m_client->answer(kServer, true, false, false);
        QCoreApplication::processEvents();
        QVERIFY(shown(QStringLiteral("passwordForm")));

        m_app->fail(QStringLiteral("access_token_revoked"),
                    QStringLiteral("@henry:example.org"), kServer);
        QQuickItem *card = find(QStringLiteral("loginRepairCard"));
        QQuickItem *flick = find(QStringLiteral("loginFlick"));
        QQuickItem *remove = find(QStringLiteral("loginRepairRemoveAccount"));
        QVERIFY(card && flick && remove);
        QTRY_VERIFY(card->isVisible() && card->height() > 0);
        // The premise: without scrolling the card does not fit. Measured in
        // content coordinates, and only once the column has placed the card:
        // right after it appears its y is still the unpositioned 0.
        const auto cardBottom = [&] {
            return card->mapToItem(flick, QPointF(0, card->height())).y()
                + flick->property("contentY").toReal();
        };
        QTRY_VERIFY2(cardBottom() > flick->height(),
                     qPrintable(QStringLiteral("card bottom %1 fits in %2: nothing to test")
                                    .arg(cardBottom()).arg(flick->height())));

        QTRY_VERIFY2(flick->property("contentY").toReal() > 0, "the view never scrolled");
        const QPointF centre = remove->mapToItem(
            flick, QPointF(remove->width() / 2, remove->height() / 2));
        QVERIFY2(centre.y() > 0 && centre.y() < flick->height(),
                 qPrintable(QStringLiteral("Remove is at %1 in a view %2 high")
                                .arg(centre.y()).arg(flick->height())));
    }

    // The card goes away (the failure is cleared, or the account removed):
    // the view returns within the shorter content. It used to stay scrolled
    // past the end, and the form jumped at the next size change.
    void aRepairCardThatGoesAwayLeavesNoScrollBehind()
    {
        m_root->setSize(QSizeF(900, 520));
        m_client->answer(kServer, true, false, false);
        QCoreApplication::processEvents();
        m_app->fail(QStringLiteral("access_token_revoked"),
                    QStringLiteral("@henry:example.org"), kServer);
        QQuickItem *flick = find(QStringLiteral("loginFlick"));
        QVERIFY(flick);
        QTRY_VERIFY2(flick->property("contentY").toReal() > 0, "the view never scrolled");

        m_app->fail(QString(), QString(), QString());
        QQuickItem *card = find(QStringLiteral("loginRepairCard"));
        QTRY_VERIFY(!card || !card->isVisible());
        const auto maxY = [&] {
            return qMax<qreal>(0, flick->property("contentHeight").toReal()
                                      - flick->height());
        };
        QTRY_VERIFY2(flick->property("contentY").toReal() <= maxY() + 0.5,
                     qPrintable(QStringLiteral("contentY %1 is past the end %2")
                                    .arg(flick->property("contentY").toReal())
                                    .arg(maxY())));
    }

    void noQmlWarnings()
    {
        m_client->answer(kServer, true, true, true, true, QStringLiteral("https://example.org/"), true,
                         QStringLiteral("https://account.example.org/"));
        QCoreApplication::processEvents();
        QCOMPARE(m_warnings, QStringList{});
    }
};

QTEST_MAIN(LoginScreenQmlTest)
#include "LoginScreenQmlTest.moc"
