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
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>

#include <functional>
#include <utility>

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
        oauthInFlight = true;
    }
    void beginOAuthSignUp(const QString &homeserver) override
    {
        signUps.append(homeserver);
        oauthInFlight = true;
    }
    void beginSsoLogin(const QString &homeserver, const QString &idpId) override
    {
        ssoStarts.append(homeserver + QLatin1Char('|') + idpId);
        ssoInFlight = true;
    }
    // As RustSdkMatrixClient: a no-op unless that flow runs, else a resolved
    // answer.
    void cancelOAuthLogin() override
    {
        if (std::exchange(oauthInFlight, false))
            Q_EMIT loginFailed(QStringLiteral("Sign-in was cancelled."));
    }
    void cancelSsoLogin() override
    {
        if (std::exchange(ssoInFlight, false))
            Q_EMIT loginFailed(QStringLiteral("Sign-in was cancelled."));
    }
    // Recorded and never answered: a server that has not replied yet.
    void login(const QString &, const QString &user, const QString &) override
    {
        logins.append(user);
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
    // As the backend does with a page it will not hand on: says so, then
    // reports the page anyway.
    void launchFails()
    {
        Q_EMIT browserLaunchFailed();
        if (oauthInFlight)
            Q_EMIT oauthBrowserUrlReady(QStringLiteral("http://example.org/authorize"));
        else if (ssoInFlight)
            Q_EMIT ssoBrowserUrlReady(QStringLiteral("http://example.org/sso"));
    }

    bool oauthInFlight = false;
    bool ssoInFlight = false;
    QStringList logins;
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
    // SettingsManager::SettingsReadProblem: 0 none, 1 unreadable, 2 accounts missing.
    Q_PROPERTY(int settingsReadProblem MEMBER readProblem NOTIFY readProblemChanged)
public:
    QString prefill = QStringLiteral("https://example.org");
    int readProblem = 0;
Q_SIGNALS:
    void prefillChanged();
    void readProblemChanged();
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
    // As AppController: whether the error is the card's own text. This fixture
    // raises its cards with their errors.
    Q_PROPERTY(bool signInErrorExplainedByCard MEMBER explainedByCard
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
    // As AppController: leaving cancels a browser sign-in still waiting,
    // whose backend answers at once.
    Q_INVOKABLE bool showMain()
    {
        ++mainShown;
        if (m_auth->browserLoginInProgress())
            m_auth->cancelBrowserLogin();
        return true;
    }
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
    bool explainedByCard = true;
    int retries = 0;
    int mainShown = 0;

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

    // Builds the screen over the current m_app: what the Loader in Main.qml
    // does each time the login screen is shown.
    void loadScreen()
    {
        m_engine = new QQmlApplicationEngine;
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const QQmlError &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"), m_app);
        if (m_rtl) {
            // The screen under a mirrored layout, as the app draws it in Arabic.
            QQmlComponent component(m_engine);
            component.setData(
                "import QtQuick\nimport MatrixClient\n"
                "LoginScreen { LayoutMirroring.enabled: true; "
                "LayoutMirroring.childrenInherit: true }",
                QUrl(QStringLiteral("file:///rtl.qml")));
            m_root = qobject_cast<QQuickItem *>(component.create(m_engine->rootContext()));
            QVERIFY2(m_root, qPrintable(component.errorString()));
        } else {
        QSignalSpy created(m_engine, &QQmlApplicationEngine::objectCreated);
        m_engine->loadFromModule(QStringLiteral("MatrixClient"), QStringLiteral("LoginScreen"));
        if (created.isEmpty())
            QVERIFY(created.wait(8000));
        m_root = qobject_cast<QQuickItem *>(created.at(0).at(0).value<QObject *>());
        }
        QVERIFY2(m_root, "LoginScreen.qml failed to load; the warning above names why");
        m_window = new QQuickWindow;
        m_window->resize(900, 900);
        m_root->setParentItem(m_window->contentItem());
        m_root->setSize(QSizeF(900, 900));
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(m_window));
    }

    bool m_rtl = false;

    // Throws the screen away, keeping m_app and the AuthManager, as the
    // Loader does when the session view replaces it. loadScreen() builds it
    // again: whatever m_app says in between is known BEFORE the new screen
    // exists, which is how a session the server ends reaches it.
    void unloadScreen()
    {
        delete m_window;
        m_window = nullptr;
        m_root = nullptr;
        delete m_engine;
        m_engine = nullptr;
        m_client->discoveries.clear();
    }

private slots:
    void init()
    {
        m_warnings.clear();
        m_client = new DiscoveringClient;
        m_auth = new AuthManager(m_client);
        m_app = new FakeApp(m_auth);
        loadScreen();
        if (QTest::currentTestFailed())
            return;
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
    // main way in, as Element shows it, with sign-up and the account page.
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
        // A browser that would not start ends the attempt (nothing could
        // finish it): the password form is back, with the reason under it.
        m_client->launchFails();
        QTRY_VERIFY(!m_auth->browserLoginInProgress());
        QVERIFY(!shown(QStringLiteral("browserLoginCancelButton")));
        QVERIFY(shown(QStringLiteral("passwordForm")));
        QVERIFY(shown(QStringLiteral("loginErrorLabel")));
        QVERIFY(!shown(QStringLiteral("loginErrorLabelNoForm")));
        QVERIFY(text(QStringLiteral("loginErrorLabel")).contains(QStringLiteral("browser")));
    }

    // C48-F01: the browser would not open and the password form took its
    // place: its first field has focus, so typing lands somewhere.
    void aBrowserThatWillNotOpenLeavesTheUsernameFocused()
    {
        m_client->answer(kServer, true, true, false, true, QStringLiteral("https://example.org/"));
        QCoreApplication::processEvents();
        QVERIFY(shown(QStringLiteral("browserLoginButton")));
        click(QStringLiteral("browserLoginButton"));
        m_client->launchFails();
        QTRY_VERIFY(!m_auth->browserLoginInProgress());
        QTRY_VERIFY(shown(QStringLiteral("passwordForm")));
        QQuickItem *user = find(QStringLiteral("userField"));
        QVERIFY(user);
        QTRY_VERIFY(user->hasActiveFocus());
    }

    // C48-F02: a password typed for one server never survives an edit of the
    // server address.
    void editingTheServerWipesATypedPassword()
    {
        m_client->answer(kServer, true, false, false);
        QCoreApplication::processEvents();
        QQuickItem *pass = find(QStringLiteral("passField"));
        QVERIFY(pass);
        QMetaObject::invokeMethod(pass, "forceActiveFocus");
        for (const char c : QByteArray("hunter2"))
            QTest::keyClick(m_window, c);
        QCOMPARE(pass->property("text").toString(), QStringLiteral("hunter2"));
        click(QStringLiteral("serverChangeButton"));
        QQuickItem *field = find(QStringLiteral("homeserverField"));
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_X);
        QCOMPARE(pass->property("text").toString(), QString());
    }

    // C48-F06: Enter on the address that was there leaves a password-chosen
    // sign-in on its form, as Escape does; it used to ask again and bring the
    // browser page back.
    void enterOnAnUnchangedServerKeepsThePasswordForm()
    {
        m_client->answer(kServer, true, true, false, true,
                         QStringLiteral("https://example.org/"));
        QCoreApplication::processEvents();
        click(QStringLiteral("passwordInsteadButton"));
        QTRY_VERIFY(shown(QStringLiteral("passwordForm")));
        click(QStringLiteral("serverChangeButton"));
        QQuickItem *field = find(QStringLiteral("homeserverField"));
        QTRY_VERIFY(field->hasActiveFocus());
        QMetaObject::invokeMethod(field, "accepted");
        QCoreApplication::processEvents();
        QVERIFY(shown(QStringLiteral("passwordForm")));
        QVERIFY(!shown(QStringLiteral("browserLoginButton")));
        QCOMPARE(m_auth->discoveryState(), QStringLiteral("done"));
    }

    // C48-D04: while a new address is typed, the last server's tick and name
    // are gone.
    void typingANewServerClearsTheOldAnswer()
    {
        m_client->answer(kServer, true, true, false, true,
                         QStringLiteral("https://example.org/"));
        QCoreApplication::processEvents();
        QCOMPARE(text(QStringLiteral("serverStatusLabel")), QStringLiteral("Found"));
        QVERIFY(text(QStringLiteral("browserLoginButton")).contains(QStringLiteral("example")));
        click(QStringLiteral("serverChangeButton"));
        QQuickItem *field = find(QStringLiteral("homeserverField"));
        QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_X);
        QVERIFY2(text(QStringLiteral("serverStatusLabel")) != QStringLiteral("Found"),
                 qPrintable(text(QStringLiteral("serverStatusLabel"))));
        QCOMPARE(text(QStringLiteral("browserLoginButton")), QStringLiteral("Continue"));
    }

    // C48-D03: after a failed sign-in the password field has focus, its text
    // is NOT selected (a timeout or a cancel also fails the sign-in, and a
    // stray keystroke must not replace the password), and the line carries an
    // icon.
    void aFailedSignInFocusesThePasswordWithoutSelectingItAndHasAnIcon()
    {
        m_client->answer(kServer, true, false, false, true,
                         QStringLiteral("https://example.org/"));
        QCoreApplication::processEvents();
        QQuickItem *user = find(QStringLiteral("userField"));
        QQuickItem *pass = find(QStringLiteral("passField"));
        user->setProperty("text", QStringLiteral("alice"));
        pass->setProperty("text", QStringLiteral("wrong"));
        click(QStringLiteral("loginSubmitButton"));
        Q_EMIT m_client->loginFailed(QStringLiteral("Wrong username or password."));
        QCoreApplication::processEvents();
        QTRY_VERIFY(pass->hasActiveFocus());
        QVERIFY(pass->property("selectedText").toString().isEmpty());
        QCOMPARE(pass->property("text").toString(), QStringLiteral("wrong"));
        QVERIFY(shown(QStringLiteral("loginErrorIcon")));
    }

    // C48-D02: mirrored, the reveal toggle is on the left and the password
    // text starts clear of it.
    void mirroredThePasswordTextClearsTheRevealToggle()
    {
        unloadScreen();
        m_rtl = true;
        loadScreen();
        m_rtl = false;
        if (QTest::currentTestFailed())
            return;
        m_client->answer(kServer, true, false, false, true,
                         QStringLiteral("https://example.org/"));
        QCoreApplication::processEvents();
        QQuickItem *pass = find(QStringLiteral("passField"));
        QQuickItem *eye = find(QStringLiteral("passwordRevealToggle"));
        QVERIFY(pass && eye);
        (void)placed(pass);
        QVERIFY2(eye->x() < pass->width() / 2, "the toggle is not at the left");
        QVERIFY2(pass->property("leftPadding").toDouble() >= eye->x() + eye->width(),
                 qPrintable(QStringLiteral("text starts under the toggle: leftPadding %1 mirrored %2 eye x %3 w %4")
                                .arg(pass->property("leftPadding").toDouble())
                                .arg(true)
                                .arg(eye->x()).arg(eye->width())));
    }

    // C48-F04: the form scrolls with the middle button as the timeline does.
    void theFormHasMiddleClickAutoscroll()
    {
        QQuickItem *scroller = find(QStringLiteral("loginMiddleClickScroller"));
        QVERIFY(scroller);
        QCOMPARE(scroller->property("view").value<QObject *>(),
                 static_cast<QObject *>(find(QStringLiteral("loginFlick"))));
    }

    // C48-F07: on a window too short for the form, Tab keeps the focused
    // control on screen.
    void tabKeepsTheFocusedControlInView()
    {
        m_client->answer(kServer, true, false, false);
        QCoreApplication::processEvents();
        m_window->resize(640, 360);
        m_root->setSize(QSizeF(640, 360));
        QCoreApplication::processEvents();
        QQuickItem *flick = find(QStringLiteral("loginFlick"));
        QVERIFY(flick);
        QVERIFY2(flick->property("contentHeight").toReal() > flick->height() + 40,
                 "the form must overflow for this to mean anything");
        QQuickItem *user = find(QStringLiteral("userField"));
        QMetaObject::invokeMethod(user, "forceActiveFocus");
        bool scrolled = false;
        for (int i = 0; i < 6; ++i) {
            QTest::keyClick(m_window, Qt::Key_Tab);
            QCoreApplication::processEvents();
            QQuickItem *item = m_window->activeFocusItem();
            if (!item)
                continue;
            bool inside = false;
            for (QQuickItem *p = item; p; p = p->parentItem())
                inside = inside || p == flick;
            if (!inside)
                continue;
            const QPointF at = item->mapToScene(QPointF(0, 0));
            QVERIFY2(at.y() >= flick->mapToScene(QPointF(0, 0)).y() - 1
                         && at.y() + item->height()
                                <= flick->mapToScene(QPointF(0, flick->height())).y() + 1,
                     qPrintable(QStringLiteral("focus item %1 out of view at y=%2 after tab %3")
                                    .arg(item->objectName()).arg(at.y()).arg(i)));
            scrolled = scrolled || flick->property("contentY").toReal() > 0;
        }
        QVERIFY(scrolled);
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

    // Reported 2026-10-01: an OAuth account on matrix.org was signed out
    // remotely and the repair form asked for a PASSWORD on the last-used
    // server (matrix.smetonis.net), under the failed account's user id. The
    // failure is recorded before the session ends and the Loader builds this
    // screen, so the root's prefill from the failure and the server field's
    // own prefill from the last-used server both run at completion, in an
    // order QML does not define. The failed account's server must win, and
    // its own way in (here the browser) be offered, not the other server's.
    void aFailureKnownBeforeTheScreenOpensUsesTheFailedAccountsServer()
    {
        const QString failedUser = QStringLiteral("@mizerd:matrix.org");
        const QString failedServer = QStringLiteral("https://matrix.org");
        const QString lastUsed = QStringLiteral("https://matrix.smetonis.example");
        unloadScreen();
        m_app->settings.prefill = lastUsed;
        m_app->fail(QStringLiteral("access_token_revoked"), failedUser, failedServer);
        loadScreen();
        QVERIFY(!QTest::currentTestFailed());
        QCoreApplication::processEvents();

        QCOMPARE(text(QStringLiteral("homeserverField")), failedServer);
        QCOMPARE(text(QStringLiteral("userField")), failedUser);
        // The last server asked about is the failed account's; nothing asked
        // about the other one after it.
        QVERIFY(!m_client->discoveries.isEmpty());
        QCOMPARE(m_client->discoveries.last(), failedServer);
        QVERIFY2(!m_client->discoveries.contains(lastUsed),
                 qPrintable(m_client->discoveries.join(QLatin1Char(','))));

        // matrix.org answers as MAS does (its own page, and a password API
        // beside it): the browser is the way in, and no password is asked.
        m_client->answer(failedServer, true, true, false, true,
                         QStringLiteral("https://matrix-client.matrix.org/"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(shown(QStringLiteral("browserLoginButton")));
        QVERIFY(!shown(QStringLiteral("passwordForm")));
        QVERIFY(shown(QStringLiteral("loginRepairCard")));
        click(QStringLiteral("browserLoginButton"));
        QCOMPARE(m_client->oauthStarts.size(), 1);
        QVERIFY2(m_client->oauthStarts.first().contains(QStringLiteral("matrix.org")),
                 qPrintable(m_client->oauthStarts.first()));
        QVERIFY(!m_client->oauthStarts.first().contains(QStringLiteral("smetonis")));
    }

    // With no failure the field still opens on the last-used server, asked
    // about once.
    void withoutAFailureTheScreenStillOpensOnTheLastUsedServer()
    {
        const QString lastUsed = QStringLiteral("https://matrix.smetonis.example");
        unloadScreen();
        m_app->settings.prefill = lastUsed;
        loadScreen();
        QVERIFY(!QTest::currentTestFailed());
        QCoreApplication::processEvents();
        QCOMPARE(text(QStringLiteral("homeserverField")), lastUsed);
        QCOMPARE(m_client->discoveries, QStringList{ lastUsed });
    }

    // The password field is as wide as the username field above it, and its
    // show/hide toggle is drawn inside it, with the typed text kept clear of
    // it. Until 2026-10-07 the toggle sat beside the field, so the password
    // field was ~38 px narrower than the one above it: two fields of one form
    // that did not line up. Measured on placed geometry.
    void thePasswordFieldLinesUpWithTheUsernameAndHoldsItsToggle()
    {
        m_client->answer(kServer, true, false, false, true,
                         QStringLiteral("https://example.org/"));
        QTRY_VERIFY(shown(QStringLiteral("passwordForm")));
        QQuickItem *user = find(QStringLiteral("userField"));
        QQuickItem *pass = find(QStringLiteral("passField"));
        QQuickItem *toggle = find(QStringLiteral("passwordRevealToggle"));
        QVERIFY(user);
        QVERIFY(pass);
        QVERIFY(toggle);
        QVERIFY(toggle->isVisible());

        const QPointF userAt = placed(user);
        const QPointF passAt = placed(pass);
        QVERIFY2(qAbs(userAt.x() - passAt.x()) < 0.5
                     && qAbs(user->width() - pass->width()) < 0.5,
                 qPrintable(QStringLiteral(
                     "username %1+%2 vs password %3+%4: the two fields of the "
                     "form do not line up")
                     .arg(userAt.x()).arg(user->width())
                     .arg(passAt.x()).arg(pass->width())));

        const QRectF fieldRect(passAt, QSizeF(pass->width(), pass->height()));
        const QRectF toggleRect(toggle->mapToScene(QPointF(0, 0)),
                                QSizeF(toggle->width(), toggle->height()));
        QVERIFY2(fieldRect.contains(toggleRect),
                 qPrintable(QStringLiteral(
                     "the show-password toggle (%1,%2 %3x%4) is not inside the "
                     "password field (%5,%6 %7x%8)")
                     .arg(toggleRect.x()).arg(toggleRect.y())
                     .arg(toggleRect.width()).arg(toggleRect.height())
                     .arg(fieldRect.x()).arg(fieldRect.y())
                     .arg(fieldRect.width()).arg(fieldRect.height())));
        // The text area ends before the toggle starts.
        const qreal textRight = passAt.x() + pass->width()
                                - pass->property("rightPadding").toReal();
        QVERIFY2(textRight <= toggleRect.left() + 0.5,
                 qPrintable(QStringLiteral(
                     "typed text runs to x=%1, under the toggle at x=%2")
                     .arg(textRight).arg(toggleRect.left())));
    }

    // Reported five times from one homeserver (B005, B007, B040, B046, B050):
    // a server offering its own sign-in page AND a password showed only the
    // browser button. The page stays the main way in, and the password is one
    // click away; another server starts on its own page again.
    void aServerWithItsOwnPageKeepsThePasswordOneClickAway()
    {
        m_client->answer(kServer, true, true, false, true,
                         QStringLiteral("https://example.org/"));
        QCoreApplication::processEvents();
        QVERIFY(shown(QStringLiteral("browserLoginButton")));
        QVERIFY(!shown(QStringLiteral("passwordForm")));
        QVERIFY(shown(QStringLiteral("passwordInsteadButton")));
        QCOMPARE(find(QStringLiteral("passwordInsteadButton"))->property("kind").toString(),
                 QStringLiteral("secondary"));

        click(QStringLiteral("passwordInsteadButton"));
        QTRY_VERIFY(shown(QStringLiteral("passwordForm")));
        QVERIFY(!shown(QStringLiteral("browserLoginButton")));
        QVERIFY(shown(QStringLiteral("browserInsteadButton")));
        QTRY_VERIFY(find(QStringLiteral("userField"))->hasActiveFocus());

        click(QStringLiteral("browserInsteadButton"));
        QTRY_VERIFY(shown(QStringLiteral("browserLoginButton")));
        QVERIFY(!shown(QStringLiteral("passwordForm")));

        // Chosen for one server only.
        click(QStringLiteral("passwordInsteadButton"));
        QTRY_VERIFY(shown(QStringLiteral("passwordForm")));
        typeServer(QStringLiteral("other.example"));
        m_client->answer(QStringLiteral("https://other.example"), true, true, false);
        QCoreApplication::processEvents();
        QTRY_VERIFY(shown(QStringLiteral("browserLoginButton")));
        QVERIFY(!shown(QStringLiteral("passwordForm")));

        // A page with no password beside it offers none.
        typeServer(QStringLiteral("third.example"));
        m_client->answer(QStringLiteral("https://third.example"), false, true, false);
        QCoreApplication::processEvents();
        QTRY_VERIFY(shown(QStringLiteral("browserLoginButton")));
        QVERIFY(!shown(QStringLiteral("passwordInsteadButton")));
    }

    // The same reports' second half (B005, B007, B050, B062, B075, B085): the
    // browser could not be handed the page (plain http), and the screen said
    // "Couldn't open your web browser... sign in another way" beside
    // "Waiting for your browser... the page that opened", until the timeout,
    // with no other way shown. Now one state: the wait is gone, the password
    // form is there, and one error under it says to use it.
    void aRefusedSignInPageFallsBackToThePasswordForm()
    {
        m_client->answer(kServer, true, true, false, true,
                         QStringLiteral("http://example.org/"));
        QCoreApplication::processEvents();
        click(QStringLiteral("browserLoginButton"));
        QCOMPARE(m_client->oauthStarts, QStringList{ kServer });
        QVERIFY(shown(QStringLiteral("browserLoginWaitingLabel")));

        m_client->launchFails();
        QTRY_VERIFY(!m_auth->browserLoginInProgress());
        QVERIFY(!shown(QStringLiteral("browserLoginWaitingLabel")));
        QVERIFY(!shown(QStringLiteral("browserLoginCancelButton")));
        QVERIFY(shown(QStringLiteral("passwordForm")));
        QVERIFY(!shown(QStringLiteral("browserLoginButton")));
        // Not offered back: the browser cannot be handed this page.
        QVERIFY(!shown(QStringLiteral("browserInsteadButton")));
        // Exactly one error, and it names the way forward.
        QVERIFY(shown(QStringLiteral("loginErrorLabel")));
        QVERIFY(!shown(QStringLiteral("loginErrorLabelNoForm")));
        QVERIFY2(text(QStringLiteral("loginErrorLabel")).contains(QStringLiteral("password")),
                 qPrintable(text(QStringLiteral("loginErrorLabel"))));

        // A server with no password: its button is back, with the reason and
        // no claim of a wait.
        typeServer(QStringLiteral("other.example"));
        m_client->answer(QStringLiteral("https://other.example"), false, true, false);
        QCoreApplication::processEvents();
        QTRY_VERIFY(shown(QStringLiteral("browserLoginButton")));
        click(QStringLiteral("browserLoginButton"));
        m_client->launchFails();
        QTRY_VERIFY(!m_auth->browserLoginInProgress());
        QVERIFY(shown(QStringLiteral("browserLoginButton")));
        QVERIFY(!shown(QStringLiteral("browserLoginWaitingLabel")));
        QVERIFY(shown(QStringLiteral("loginErrorLabelNoForm")));
        QVERIFY(!text(QStringLiteral("loginErrorLabelNoForm")).contains(QStringLiteral("another way")));
    }

    // A slow server held a password sign-in with no way out until the request
    // timed out (B041): Escape and Cancel give the form back, Change included.
    void aPasswordSignInCanBeCancelled()
    {
        m_client->answer(kServer, true, false, false, true,
                         QStringLiteral("https://example.org/"));
        QCoreApplication::processEvents();
        find(QStringLiteral("userField"))->setProperty("text", QStringLiteral("bob"));
        find(QStringLiteral("passField"))->setProperty("text", QStringLiteral("pw"));
        QVERIFY(!shown(QStringLiteral("passwordLoginCancelButton")));
        click(QStringLiteral("loginSubmitButton"));
        QCOMPARE(m_client->logins, QStringList{ QStringLiteral("bob") });
        QVERIFY(m_auth->isLoggingIn());
        QVERIFY(shown(QStringLiteral("passwordLoginCancelButton")));
        QVERIFY(!find(QStringLiteral("serverChangeButton"))->isEnabled());

        QTest::keyClick(m_window, Qt::Key_Escape);
        QTRY_VERIFY(!m_auth->isLoggingIn());
        QVERIFY(!shown(QStringLiteral("passwordLoginCancelButton")));
        QVERIFY(find(QStringLiteral("serverChangeButton"))->isEnabled());
        QVERIFY(find(QStringLiteral("loginSubmitButton"))->isEnabled());
        QVERIFY2(text(QStringLiteral("loginErrorLabel"))
                     .startsWith(QStringLiteral("Stopped waiting for the server.")),
                 qPrintable(text(QStringLiteral("loginErrorLabel"))));

        // The abandoned request answers; the next attempt is cancelled by the
        // button.
        Q_EMIT m_client->loginFailed(QStringLiteral("Can't reach the server."));
        click(QStringLiteral("loginSubmitButton"));
        QCOMPARE(m_client->logins.size(), 2);
        click(QStringLiteral("passwordLoginCancelButton"));
        QVERIFY(!m_auth->isLoggingIn());
    }

    // An error stayed on the form until the next submit, after the field it
    // was about had been corrected (B045).
    void editingAFieldClearsTheError()
    {
        m_client->answer(kServer, true, false, false, true,
                         QStringLiteral("https://example.org/"));
        QCoreApplication::processEvents();
        const auto failOnce = [this] {
            click(QStringLiteral("loginSubmitButton"));
            Q_EMIT m_client->loginFailed(QStringLiteral("Enter your username."));
            QCoreApplication::processEvents();
            QVERIFY(shown(QStringLiteral("loginErrorLabel")));
        };
        for (const char *name : { "userField", "passField" }) {
            failOnce();
            if (QTest::currentTestFailed())
                return;
            QQuickItem *field = find(QLatin1String(name));
            QVERIFY(field);
            field->forceActiveFocus();
            QTRY_VERIFY(field->hasActiveFocus());
            QTest::keyClick(m_window, Qt::Key_X);
            QVERIFY2(m_auth->lastError().isEmpty(), name);
            QVERIFY2(!shown(QStringLiteral("loginErrorLabel")), name);
        }
    }

    // Enter on the address already answered asks the server again; leaving
    // the field does not (B048).
    void enterAsksAnAnsweredServerAgain()
    {
        m_client->answer(kServer, true, false, false);
        QCoreApplication::processEvents();
        QCOMPARE(m_client->discoveries.size(), 1);
        typeServer(kServer);
        QCOMPARE(m_client->discoveries, QStringList({ kServer, kServer }));
        m_client->answer(kServer, true, false, false);
        QCoreApplication::processEvents();
        QTRY_VERIFY(shown(QStringLiteral("serverSummary")));

        click(QStringLiteral("serverChangeButton"));
        QQuickItem *field = find(QStringLiteral("homeserverField"));
        QMetaObject::invokeMethod(field, "editingFinished");
        QCOMPARE(m_client->discoveries.size(), 2);
    }

    // The usual path: an address typed, the debounce asks, "Found", then
    // Enter. That answer is newer than the edit, so Enter goes on to the way
    // in without asking again; a second probe would make every way in
    // disappear and come back.
    void enterAfterTheDebounceAnsweredDoesNotAskAgain()
    {
        m_client->answer(kServer, true, false, false);
        QCoreApplication::processEvents();
        QTRY_VERIFY(shown(QStringLiteral("serverSummary")));
        click(QStringLiteral("serverChangeButton"));
        QQuickItem *field = find(QStringLiteral("homeserverField"));
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());
        for (const char c : QByteArrayLiteral("other.example"))
            QTest::keyClick(m_window, c);
        QCOMPARE(field->property("text").toString(), QStringLiteral("other.example"));
        QTRY_COMPARE(m_client->discoveries.size(), 2);
        const QString other = m_client->discoveries.last();
        m_client->answer(other, true, false, false);
        QCoreApplication::processEvents();
        QCOMPARE(m_auth->discoveryState(), QStringLiteral("done"));

        QTest::keyClick(m_window, Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(m_client->discoveries.size(), 2);
        QCOMPARE(m_auth->discoveryState(), QStringLiteral("done"));
        QTRY_VERIFY(shown(QStringLiteral("passwordForm")));
    }

    // Back during a browser sign-in cancels it (AppController::showMain), and
    // that cancel's answer was kept through the restore that follows: a
    // login screen opened later showed "cancelled" for nothing.
    void backDuringABrowserSignInLeavesNoError()
    {
        m_client->answer(kServer, true, true, false);
        QCoreApplication::processEvents();
        click(QStringLiteral("browserLoginButton"));
        QVERIFY(m_auth->browserLoginInProgress());
        // Shown only with a saved account, which this fixture has none of;
        // its handler is what is under test.
        click(QStringLiteral("backToAppButton"));
        QCOMPARE(m_app->mainShown, 1);
        QVERIFY(!m_auth->browserLoginInProgress());
        QCOMPARE(m_auth->lastError(), QString());
        // The restore of the account the user went back to.
        Q_EMIT m_client->loginSucceeded(QStringLiteral("@alice:example.org"));
        QCOMPARE(m_auth->lastError(), QString());
    }

    // Saved settings that could not be read are said, not shown as a fresh
    // sign-in (B118's notice, read when SettingsManager offers it).
    void unreadableSettingsAreSaid()
    {
        QVERIFY(!shown(QStringLiteral("settingsReadProblemNotice")));
        m_app->settings.readProblem = 1; // SettingsUnreadable
        Q_EMIT m_app->settings.readProblemChanged();
        QTRY_VERIFY(shown(QStringLiteral("settingsReadProblemNotice")));
        QVERIFY(text(QStringLiteral("settingsReadProblemText"))
                    .contains(QStringLiteral("couldn't read its saved settings")));
        m_app->settings.readProblem = 0;
        Q_EMIT m_app->settings.readProblemChanged();
        QTRY_VERIFY(!shown(QStringLiteral("settingsReadProblemNotice")));
    }

    // A settings file that reads fine but lists no account while account data
    // exists is NOT damage (SettingsManager.h: a sign-in cancelled or killed
    // before its record was written looks the same), so its notice must not
    // claim the settings were unreadable. It used the unreadable-file text.
    void accountDataNoAccountListsIsNotCalledUnreadable()
    {
        m_app->settings.readProblem = 2; // AccountsMissing
        Q_EMIT m_app->settings.readProblemChanged();
        QTRY_VERIFY(shown(QStringLiteral("settingsReadProblemNotice")));
        const QString said = text(QStringLiteral("settingsReadProblemText"));
        QVERIFY2(!said.contains(QStringLiteral("couldn't read")), qPrintable(said));
        QVERIFY2(said.contains(QStringLiteral("doesn't list")), qPrintable(said));
        m_app->settings.readProblem = 0;
        Q_EMIT m_app->settings.readProblemChanged();
        QTRY_VERIFY(!shown(QStringLiteral("settingsReadProblemNotice")));
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
