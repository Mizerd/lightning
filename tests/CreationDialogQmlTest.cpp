// NewConversationDialog: the Space tab instantiates no encryption control
// while the Room tab does (tabs are Loaders); a tab switch clears the shared
// UserSearchModel and focuses the new tab's first field; Create is disabled
// until the name is non-blank and the one error banner shows the
// controller's errorMessage; busy disables submit but not Cancel;
// existing-DM reuse rows render before the secondary "start anyway" action
// and wear the person's name; a target the server does not know is asked
// about before any room is made; Down/Down/Return in search selects the
// second result. A focused scene (as
// in ComposerQmlTest) with a FakeClient injected into app.conversations,
// since the full-shell mock cannot script this state.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/ConversationController.h"
#include "matrix/MatrixClient.h"
#include "models/UserSearchModel.h"

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    // Scripted state.
    QList<RoomInfo> mirror;
    QVariantList dmRooms;
    quint64 nextOp = 1;
    int searchCalls = 0;
    int createRoomCalls = 0;
    int createDmCalls = 0;
    QString lastSearchQuery;
    QVariantMap lastRoomOptions;
    quint64 lastOpId = 0;
    // The DM target check; off unless a case turns it on.
    bool profileLookupSupported = false;
    quint64 lastProfileOp = 0;

    // MatrixClient pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return mirror; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    bool supportsRoomManagement() const override { return true; }
    quint64 searchUsers(const QString &query, int) override
    {
        ++searchCalls;
        lastSearchQuery = query;
        lastOpId = nextOp++;
        return lastOpId;
    }
    QVariantList existingDirectRooms(const QString &) const override
    {
        return dmRooms;
    }
    quint64 fetchUserProfile(const QString &) override
    {
        if (!profileLookupSupported)
            return 0;
        lastProfileOp = nextOp++;
        return lastProfileOp;
    }
    quint64 createDirectChat(const QString &) override
    {
        ++createDmCalls;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 createRoom(const QVariantMap &options) override
    {
        ++createRoomCalls;
        lastRoomOptions = options;
        lastOpId = nextOp++;
        return lastOpId;
    }
};

QVariantMap searchRow(const QString &userId, const QString &name = {})
{
    QVariantMap row;
    row.insert(QStringLiteral("userId"), userId);
    row.insert(QStringLiteral("displayName"), name);
    row.insert(QStringLiteral("avatarUrl"), QString());
    return row;
}

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true
    color: AppTheme.background

    NewConversationDialog {
        objectName: "creationDialog"
        parent: Overlay.overlay
    }
}
)QML";

} // namespace

class CreationDialogQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    FakeClient *m_fake = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QObject *m_dialog = nullptr;
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

    // Popup contents live in the Overlay's visual tree, which findChild cannot
    // walk.
    QQuickItem *item(const char *name) const
    {
        if (auto *hit = m_window->findChild<QQuickItem *>(QLatin1String(name)))
            return hit;
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    static void collectItems(QQuickItem *parent, QList<QQuickItem *> *out)
    {
        if (!parent)
            return;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            out->append(child);
            collectItems(child, out);
        }
    }

    static bool subtreeHasClassName(QQuickItem *rootItem, const char *fragment)
    {
        QList<QQuickItem *> all;
        collectItems(rootItem, &all);
        for (QQuickItem *it : all) {
            if (QByteArray(it->metaObject()->className()).contains(fragment))
                return true;
        }
        return false;
    }

    void openDialog(const char *mode)
    {
        // openDialog(startMode, options): options are optional in QML, but
        // meta-invocation must match the full arity.
        QVERIFY(QMetaObject::invokeMethod(
            m_dialog, "openDialog",
            Q_ARG(QVariant, QVariant(QLatin1String(mode))),
            Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        QCoreApplication::processEvents();
    }

    void closeDialog()
    {
        QMetaObject::invokeMethod(m_dialog, "close");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        QCoreApplication::processEvents();
    }

    // Picks `userId` (shown as `name`) from a scripted search, as a person
    // would with the keyboard.
    void selectUserFromSearch(const QString &userId, const QString &name)
    {
        auto *field = item("dmUserPickerSearchField");
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());
        const int before = m_fake->searchCalls;
        field->setProperty("text", userId.mid(1, 3));
        QTRY_VERIFY(m_fake->searchCalls > before);
        Q_EMIT m_fake->userSearchFinished(m_fake->lastOpId, true,
                                          { searchRow(userId, name) }, false,
                                          QString());
        auto *results = item("dmUserPickerResults");
        QVERIFY(results);
        QTRY_COMPARE(results->property("count").toInt(), 1);
        QTest::keyClick(m_window, Qt::Key_Down);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_dialog->property("selectedUserId").toString(), userId);
    }

    ConversationController *conversations() const
    {
        return m_controller->conversations();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("creation-dialog-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_fake = new FakeClient;
        m_engine = new QQmlEngine(this);
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("creationscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QCoreApplication::processEvents();

        m_dialog = m_root->findChild<QObject *>(QStringLiteral("creationDialog"));
        QVERIFY(m_dialog);

        // Deterministic backend for busy, search and existing-DM state.
        conversations()->setClient(m_fake);
        conversations()->userSearch()->setDebounceMs(1);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
        delete m_fake;
    }

    void loadsWithoutQmlWarnings()
    {
        QCOMPARE(m_warnings, QStringList{});
    }

    // (a) The Space tab instantiates no encryption control; the Room tab does.
    // Loaders guarantee non-instantiation, not just invisibility.
    void spaceTabInstantiatesNoEncryptionControl()
    {
        openDialog("space");
        QTRY_VERIFY(item("spaceTab"));
        QVERIFY(item("spaceNameField"));
        QVERIFY(item("spaceCreateButton"));
        QVERIFY(!item("roomTab"));
        QVERIFY(!item("roomEncryptSwitch"));
        QVERIFY(!item("roomEncryptRow"));
        // No switch of any kind inside the Space tab's subtree.
        QVERIFY(!subtreeHasClassName(item("spaceTab"), "AppSwitch"));

        auto *roomChip = item("newConversationRoomChip");
        QVERIFY(roomChip);
        QMetaObject::invokeMethod(roomChip, "click");
        QTRY_VERIFY(item("roomTab"));
        // Loader teardown is deferred; let the event loop run it first.
        QTRY_VERIFY(!item("spaceTab"));
        QVERIFY(item("roomEncryptSwitch"));
        QVERIFY(subtreeHasClassName(item("roomTab"), "AppSwitch"));
        closeDialog();
    }

    // B056: the validation hint reserves its height, so the form does not
    // jump when the hint appears; and a tab switch drops the previous tab's
    // scroll offset instead of drawing the new form far from the title.
    void spaceFormDoesNotJumpAndTabSwitchResetsScroll()
    {
        openDialog("space");
        QTRY_VERIFY(item("spaceTab"));
        auto *nameField = item("spaceNameField");
        auto *create = item("spaceCreateButton");
        QVERIFY(nameField && create);
        QTRY_VERIFY(nameField->hasActiveFocus());
        auto *hint = item("spaceNameHint");
        auto *slot = item("spaceNameHintSlot");
        QVERIFY(hint && slot);
        QVERIFY(!hint->isVisible());
        const qreal slotHeight = slot->height();
        QVERIFY2(slotHeight > 0, "the hint's slot must reserve a line");
        const qreal createY = create->mapToItem(m_window->contentItem(),
                                                QPointF(0, 0)).y();
        const qreal dialogHeight = m_dialog->property("height").toReal();

        QTest::keyClick(m_window, Qt::Key_A);
        QTRY_COMPARE(nameField->property("text").toString(), QStringLiteral("a"));
        QTest::keyClick(m_window, Qt::Key_Backspace);
        QTRY_VERIFY(hint->isVisible());
        QCOMPARE(slot->height(), slotHeight);
        QCOMPARE(create->mapToItem(m_window->contentItem(), QPointF(0, 0)).y(),
                 createY);
        QCOMPARE(m_dialog->property("height").toReal(), dialogHeight);

        // A stale offset from the previous tab does not follow the switch.
        auto *flick = item("creationTabFlick");
        QVERIFY(flick);
        flick->setProperty("contentY", -80.0);
        auto *roomChip = item("newConversationRoomChip");
        QVERIFY(roomChip);
        QMetaObject::invokeMethod(roomChip, "click");
        QTRY_VERIFY(item("roomTab"));
        QTRY_COMPARE(flick->property("contentY").toReal(), 0.0);
        closeDialog();
    }

    // (b) A tab switch clears the shared search model and focuses the new
    // tab's first field.
    void tabSwitchClearsSearchAndMovesFocus()
    {
        openDialog("dm");
        auto *field = item("dmUserPickerSearchField");
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());

        const int before = m_fake->searchCalls;
        field->setProperty("text", QStringLiteral("ali"));
        QTRY_VERIFY(m_fake->searchCalls > before);
        QCOMPARE(conversations()->userSearch()->query(), QStringLiteral("ali"));
        QCOMPARE(conversations()->userSearch()->state(),
                 QStringLiteral("loading"));

        auto *roomChip = item("newConversationRoomChip");
        QVERIFY(roomChip);
        QMetaObject::invokeMethod(roomChip, "click");
        QTRY_VERIFY(item("roomNameField"));
        // Shared model cleared...
        QCOMPARE(conversations()->userSearch()->query(), QString());
        QCOMPARE(conversations()->userSearch()->state(), QStringLiteral("idle"));
        // ...and focus on the Room tab's first field.
        QTRY_VERIFY(item("roomNameField")->hasActiveFocus());
        closeDialog();
    }

    // (c) Create is disabled until the name is non-blank; a blank-after-edit
    // hint appears; the one banner shows controller errors.
    void createDisabledUntilNameAndErrorBannerShows()
    {
        openDialog("room");
        auto *nameField = item("roomNameField");
        auto *create = item("roomCreateButton");
        QVERIFY(nameField && create);
        QVERIFY(!create->property("enabled").toBool());
        QVERIFY(!item("roomNameHint")->isVisible());

        QTRY_VERIFY(nameField->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_A);
        QTRY_COMPARE(nameField->property("text").toString(), QStringLiteral("a"));
        QVERIFY(create->property("enabled").toBool());

        QTest::keyClick(m_window, Qt::Key_Backspace);
        QTRY_COMPARE(nameField->property("text").toString(), QString());
        QVERIFY(!create->property("enabled").toBool());
        // Blank after an edit: inline validation hint.
        QTRY_VERIFY(item("roomNameHint")->isVisible());

        // The single error banner mirrors the controller's errorMessage.
        auto *banner = item("creationErrorBanner");
        QVERIFY(banner);
        QVERIFY(!banner->isVisible());
        conversations()->createRoom({ { QStringLiteral("name"), QString() } });
        QTRY_VERIFY(banner->isVisible());
        QVERIFY(!conversations()->errorMessage().isEmpty());
        conversations()->clearError();
        QTRY_VERIFY(!banner->isVisible());
        closeDialog();
    }

    // (d) Busy disables submit; Cancel stays enabled; a failure shows in the
    // banner and ends busy.
    void busyDisablesSubmitButNeverCancel()
    {
        openDialog("room");
        auto *nameField = item("roomNameField");
        auto *create = item("roomCreateButton");
        auto *cancel = item("creationCancelButton");
        QVERIFY(nameField && create && cancel);

        nameField->setProperty("text", QStringLiteral("Test room"));
        QTRY_VERIFY(create->property("enabled").toBool());
        const int before = m_fake->createRoomCalls;
        QMetaObject::invokeMethod(create, "click");
        QCOMPARE(m_fake->createRoomCalls, before + 1);
        // The private default submits encrypted && !public; the optional avatar
        // never reaches the backend's create call.
        QCOMPARE(m_fake->lastRoomOptions.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Test room"));
        QCOMPARE(m_fake->lastRoomOptions.value(QStringLiteral("public")).toBool(),
                 false);
        QCOMPARE(m_fake->lastRoomOptions.value(QStringLiteral("encrypted")).toBool(),
                 true);
        QVERIFY(!m_fake->lastRoomOptions.contains(QStringLiteral("avatarPath")));

        QVERIFY(conversations()->busy());
        QTRY_VERIFY(!create->property("enabled").toBool());
        QVERIFY(item("creationBusyLabel")->isVisible());
        QVERIFY(cancel->property("enabled").toBool());

        Q_EMIT m_fake->roomCreateFinished(m_fake->lastOpId, false, QString(),
                                          QStringLiteral("network"), QString());
        QTRY_VERIFY(!conversations()->busy());
        QTRY_VERIFY(item("creationErrorBanner")->isVisible());
        QTRY_VERIFY(create->property("enabled").toBool());
        closeDialog();
    }

    // break-it s03: "My Room! #x:y" as the address got only "The server
    // rejected the request as invalid." Validated here now, as the user types,
    // and refused by the controller before /createRoom. Fails on the old
    // dialog: there was no hint, Create stayed enabled and the call was sent.
    void anInvalidAddressIsExplainedAndNeverSent()
    {
        openDialog("room");
        auto *nameField = item("roomNameField");
        auto *create = item("roomCreateButton");
        QVERIFY(nameField && create);
        nameField->setProperty("text", QStringLiteral("My Room"));
        m_dialog->setProperty("roomIsPublic", true);
        auto *aliasField = item("roomAliasField");
        QVERIFY(aliasField);
        QTRY_VERIFY(aliasField->isVisible());
        QTRY_VERIFY(create->property("enabled").toBool());

        aliasField->setProperty("text", QStringLiteral("My Room! #x:y"));
        auto *hint = item("roomAliasHint");
        QVERIFY2(hint, "no inline hint for the address");
        QTRY_VERIFY(hint->isVisible());
        QVERIFY2(hint->property("text").toString()
                     .contains(QStringLiteral("spaces")),
                 qPrintable(hint->property("text").toString()));
        QVERIFY(!create->property("enabled").toBool());

        // The controller refuses it too, whoever calls it.
        const int before = m_fake->createRoomCalls;
        conversations()->createRoom({ { QStringLiteral("name"), QStringLiteral("x") },
                                      { QStringLiteral("public"), true },
                                      { QStringLiteral("alias"),
                                        QStringLiteral("a:b") } });
        QCOMPARE(m_fake->createRoomCalls, before);
        QVERIFY(!conversations()->errorMessage().isEmpty());
        QVERIFY(!conversations()->errorMessage().contains(
            QStringLiteral("rejected the request")));

        // Fixed: the hint goes, Create comes back, and the localpart is sent
        // without the '#' a user types out of habit.
        aliasField->setProperty("text", QStringLiteral("#my-room"));
        QTRY_VERIFY(!hint->isVisible());
        QTRY_VERIFY(create->property("enabled").toBool());
        QMetaObject::invokeMethod(create, "click");
        QCOMPARE(m_fake->createRoomCalls, before + 1);
        QCOMPARE(m_fake->lastRoomOptions.value(QStringLiteral("alias")).toString(),
                 QStringLiteral("my-room"));
        Q_EMIT m_fake->roomCreateFinished(m_fake->lastOpId, false, QString(),
                                          QStringLiteral("network"), QString());
        QTRY_VERIFY(!conversations()->busy());
        closeDialog();
    }

    // break-it s03: after a failed create the banner stayed up while the user
    // corrected the address, reading as if the fix had failed too. An edit to
    // the form drops it. Fails on the old dialog, where only the next submit
    // replaced it.
    void editingTheFormClearsAStaleErrorBanner()
    {
        openDialog("room");
        auto *nameField = item("roomNameField");
        auto *create = item("roomCreateButton");
        QVERIFY(nameField && create);
        nameField->setProperty("text", QStringLiteral("My Room"));
        m_dialog->setProperty("roomIsPublic", true);
        auto *aliasField = item("roomAliasField");
        QVERIFY(aliasField);
        aliasField->setProperty("text", QStringLiteral("taken"));
        QTRY_VERIFY(create->property("enabled").toBool());
        QMetaObject::invokeMethod(create, "click");
        Q_EMIT m_fake->roomCreateFinished(m_fake->lastOpId, false, QString(),
                                          QStringLiteral("alias_taken"),
                                          QString());
        QTRY_VERIFY(!conversations()->busy());
        auto *banner = item("creationErrorBanner");
        QVERIFY(banner);
        QTRY_VERIFY(banner->isVisible());

        aliasField->setProperty("text", QStringLiteral("taken-2"));
        QTRY_VERIFY2(!banner->isVisible(),
                     "the banner still reports the previous address");
        QVERIFY(conversations()->errorMessage().isEmpty());
        closeDialog();
    }

    // (e)+(f) Down/Down/Return selects the second result; with an existing DM
    // the reuse row renders before "start anyway", and the primary start
    // button is hidden.
    void keyboardSelectsSecondResultAndReuseComesFirst()
    {
        QVariantMap dm;
        dm.insert(QStringLiteral("roomId"), QStringLiteral("!dm:example.org"));
        dm.insert(QStringLiteral("name"), QStringLiteral("Alice Two"));
        m_fake->dmRooms = { dm };

        openDialog("dm");
        auto *field = item("dmUserPickerSearchField");
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());

        const int before = m_fake->searchCalls;
        field->setProperty("text", QStringLiteral("alice"));
        QTRY_VERIFY(m_fake->searchCalls > before);
        Q_EMIT m_fake->userSearchFinished(
            m_fake->lastOpId, true,
            { searchRow(QStringLiteral("@alice:example.org"),
                        QStringLiteral("Alice")),
              searchRow(QStringLiteral("@alice2:example.org"),
                        QStringLiteral("Alice Two")) },
            false, QString());
        auto *results = item("dmUserPickerResults");
        QVERIFY(results);
        QTRY_COMPARE(results->property("count").toInt(), 2);

        QTest::keyClick(m_window, Qt::Key_Down);
        QTest::keyClick(m_window, Qt::Key_Down);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_dialog->property("selectedUserId").toString(),
                     QStringLiteral("@alice2:example.org"));
        QCOMPARE(m_dialog->property("selectedDisplayName").toString(),
                 QStringLiteral("Alice Two"));

        // Reuse first, the secondary escape hatch below, no primary.
        QCOMPARE(conversations()->existingDms().size(), 1);
        auto *reuseRow = item("existingDmRow_0");
        auto *openBtn = item("existingDmOpen_0");
        auto *anyway = item("dmStartAnywayButton");
        QVERIFY(reuseRow && reuseRow->isVisible());
        QVERIFY(openBtn && openBtn->isVisible());
        QVERIFY(anyway && anyway->isVisible());
        QCOMPARE(anyway->property("kind").toString(),
                 QStringLiteral("secondary"));
        auto *primary = item("dmStartButton");
        QVERIFY(!primary || !primary->isVisible());
        // Ordering settles with layout polish; poll.
        QTRY_VERIFY(reuseRow->mapToScene(QPointF(0, 0)).y()
                    < anyway->mapToScene(QPointF(0, 0)).y());

        m_fake->dmRooms.clear();
        closeDialog();
    }

    // Break-it s04: a DM the backend reported without a name read "Existing
    // conversation" with an "EC" avatar. It is a DM with the person on the
    // card, so it wears their name.
    void anUnnamedExistingDmIsLabelledWithThePerson()
    {
        QVariantMap dm;
        dm.insert(QStringLiteral("roomId"), QStringLiteral("!dm:example.org"));
        dm.insert(QStringLiteral("name"), QString());
        m_fake->dmRooms = { dm };

        openDialog("dm");
        selectUserFromSearch(QStringLiteral("@bob:example.org"),
                             QStringLiteral("Bob Builder"));
        if (QTest::currentTestFailed())
            return;
        auto *label = item("existingDmName_0");
        auto *avatar = item("existingDmAvatar_0");
        QVERIFY(label && avatar);
        QTRY_COMPARE(label->property("text").toString(),
                     QStringLiteral("Bob Builder"));
        QCOMPARE(avatar->property("name").toString(),
                 QStringLiteral("Bob Builder"));

        m_fake->dmRooms.clear();
        closeDialog();
    }

    // Break-it s04: the homeserver says the target does not exist (404 on the
    // profile). The dialog says so and creates nothing until asked to.
    void anUnknownTargetIsAskedAboutBeforeAnyRoomIsMade()
    {
        m_fake->profileLookupSupported = true;
        openDialog("dm");
        const QString nobody = QStringLiteral("@nobodyhere999:example.org");
        selectUserFromSearch(nobody, QString());
        if (QTest::currentTestFailed())
            return;

        auto *start = item("dmStartButton");
        auto *panel = item("dmUnknownTargetPanel");
        QVERIFY(start && panel);
        QTRY_VERIFY(start->isVisible());
        QVERIFY(!panel->isVisible());
        const int before = m_fake->createDmCalls;
        QMetaObject::invokeMethod(start, "click");
        QCOMPARE(m_fake->createDmCalls, before);

        Q_EMIT m_fake->userProfileFinished(m_fake->lastProfileOp, false, nobody,
                                           QString(), QString(),
                                           QStringLiteral("not_found"));
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(!start->isVisible());
        QCOMPARE(m_fake->createDmCalls, before);
        auto *text = item("dmUnknownTargetText");
        QVERIFY(text);
        QVERIFY(text->property("text").toString().contains(nobody));
        QCOMPARE(text->property("textFormat").toInt(), int(Qt::PlainText));

        // Cancel makes nothing and puts the ordinary action back.
        auto *cancel = item("dmUnknownTargetCancel");
        auto *anyway = item("dmUnknownTargetStartAnyway");
        QVERIFY(cancel && anyway);
        QMetaObject::invokeMethod(cancel, "click");
        QTRY_VERIFY(!panel->isVisible());
        QVERIFY(start->isVisible());
        QCOMPARE(m_fake->createDmCalls, before);

        // Start anyway creates exactly one.
        QMetaObject::invokeMethod(start, "click");
        Q_EMIT m_fake->userProfileFinished(m_fake->lastProfileOp, false, nobody,
                                           QString(), QString(),
                                           QStringLiteral("not_found"));
        QTRY_VERIFY(panel->isVisible());
        QMetaObject::invokeMethod(anyway, "click");
        QCOMPARE(m_fake->createDmCalls, before + 1);

        // Release the create guard for the cases after this one.
        Q_EMIT m_fake->dmCreateFinished(m_fake->lastOpId, false, QString(),
                                        QStringLiteral("forbidden"));
        QTRY_VERIFY(!conversations()->busy());
        m_fake->profileLookupSupported = false;
        closeDialog();
    }

    // (g) The omnibox offers "Create room" for plain text and "#name" (seeding
    // the Room tab), never for "@" text.
    void omniboxSuggestsCreatingARoomForNonAtText()
    {
        openDialog("dm");
        auto *field = item("dmUserPickerSearchField");
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());

        auto *suggestion = item("dmCreateRoomSuggestion");
        QVERIFY(suggestion);
        QVERIFY(!suggestion->isVisible());

        field->setProperty("text", QStringLiteral("@alice"));
        QCoreApplication::processEvents();
        QVERIFY(!suggestion->isVisible());

        field->setProperty("text", QStringLiteral("#design-team"));
        QCoreApplication::processEvents();
        QTRY_VERIFY(suggestion->isVisible());

        QMetaObject::invokeMethod(suggestion, "activateSuggestion");
        QTRY_VERIFY(item("roomTab"));
        auto *nameField = item("roomNameField");
        QVERIFY(nameField);
        QTRY_COMPARE(nameField->property("text").toString(),
                     QStringLiteral("design-team"));
        closeDialog();
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    CreationDialogQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "CreationDialogQmlTest.moc"
