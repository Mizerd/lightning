// The Change password section's own handling of the secrets, on the real QML
// against the real controller (scripted client). Pins:
//   * all three fields are in Password echo mode, and the new ones stop at
//     Synapse's 512;
//   * after submit, after the section hides, and when the account page takes
//     over, all three fields are empty AND have no undo history: clear()
//     would keep it, so Ctrl+Z would bring a typed password back;
//   * the sign-out choice reaches the backend and resets after sending.
// Each wipe is checked against fields that could undo before it.

#include "app/PasswordChangeController.h"
#include "matrix/MockMatrixClient.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class PwClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    int changeCalls = 0;
    bool lastLogoutDevices = false;
    quint64 nextOp = 500;
    quint64 lastChangeOp = 0;
    quint64 lastProbeOp = 0;

    bool supportsPasswordChange() const override { return true; }
    quint64 changePassword(const QString &, const QString &,
                           bool logoutDevices) override
    {
        ++changeCalls;
        lastLogoutDevices = logoutDevices;
        lastChangeOp = nextOp++;
        return lastChangeOp;
    }
    quint64 probePasswordChange() override
    {
        lastProbeOp = nextOp++;
        return lastProbeOp;
    }
};

// Only what the section reads from `app`.
class StubApp final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *passwordChange READ passwordChange CONSTANT)
    Q_PROPERTY(QObject *accounts READ accounts CONSTANT)
public:
    explicit StubApp(QObject *passwordChange)
        : m_passwordChange(passwordChange)
    {
    }
    QObject *passwordChange() const { return m_passwordChange; }
    QObject *accounts() const { return nullptr; }

private:
    QObject *m_passwordChange;
};

constexpr int kPasswordEcho = 2; // TextInput.Password

const QStringList kFields = {
    QStringLiteral("currentPasswordField"),
    QStringLiteral("newPasswordField"),
    QStringLiteral("confirmPasswordField"),
};

} // namespace

class ChangePasswordSectionQmlTest : public QObject
{
    Q_OBJECT

private:
    // Types into a field the way a user would, so it has undo history.
    static bool type(QObject *field, const QString &text)
    {
        return QMetaObject::invokeMethod(field, "insert", Q_ARG(int, 0),
                                         Q_ARG(QString, text));
    }

    static bool fillAll(const QList<QObject *> &fields)
    {
        const QString next = QStringLiteral("a-new-passphrase-2026");
        return type(fields.at(0), QStringLiteral("the-current-one"))
               && type(fields.at(1), next) && type(fields.at(2), next);
    }

    static QString state(const QList<QObject *> &fields)
    {
        QStringList out;
        for (QObject *f : fields) {
            out << f->objectName() + QStringLiteral(" length=")
                       + QString::number(f->property("text").toString().size())
                       + QStringLiteral(" canUndo=")
                       + (f->property("canUndo").toBool()
                              ? QStringLiteral("true")
                              : QStringLiteral("false"));
        }
        return out.join(QStringLiteral("; "));
    }

    static bool emptyWithoutUndo(const QList<QObject *> &fields)
    {
        for (QObject *f : fields) {
            if (!f->property("text").toString().isEmpty()
                || f->property("canUndo").toBool()) {
                return false;
            }
        }
        return true;
    }

    static bool allCanUndo(const QList<QObject *> &fields)
    {
        for (QObject *f : fields) {
            if (!f->property("canUndo").toBool())
                return false;
        }
        return true;
    }

private Q_SLOTS:
    void fieldsAreMaskedAndWipedWithoutUndo()
    {
        PwClient client;
        PasswordChangeController ctl;
        ctl.setClient(&client);
        StubApp app(&ctl);

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &app);
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        warnings << e.toString();
                });
        QSignalSpy created(&engine, &QQmlApplicationEngine::objectCreated);
        engine.loadFromModule(QStringLiteral("MatrixClient"),
                              QStringLiteral("ChangePasswordSection"));
        if (created.isEmpty())
            QVERIFY(created.wait(8000));
        QObject *root = engine.rootObjects().value(0);
        QVERIFY2(root, qPrintable(warnings.join(QLatin1Char('\n'))));
        QCOMPARE(root->property("mode").toString(), QStringLiteral("form"));

        QList<QObject *> fields;
        for (const QString &name : kFields) {
            QObject *field = root->findChild<QObject *>(name);
            QVERIFY2(field, qPrintable(name));
            QCOMPARE(field->property("echoMode").toInt(), kPasswordEcho);
            fields << field;
        }
        QCOMPARE(fields.at(1)->property("maximumLength").toInt(), 512);
        QCOMPARE(fields.at(2)->property("maximumLength").toInt(), 512);
        QObject *signOut =
            root->findChild<QObject *>(QStringLiteral("signOutOtherDevicesCheck"));
        QVERIFY(signOut);

        // 1. Submit.
        QVERIFY(fillAll(fields));
        QVERIFY2(allCanUndo(fields), qPrintable(state(fields)));
        QVERIFY(signOut->setProperty("checked", true));
        QVERIFY(QMetaObject::invokeMethod(root, "submit"));
        QCOMPARE(client.changeCalls, 1);
        QVERIFY(client.lastLogoutDevices);
        QVERIFY(ctl.busy());
        QVERIFY2(emptyWithoutUndo(fields), qPrintable(state(fields)));
        QVERIFY(!signOut->property("checked").toBool());
        Q_EMIT client.passwordChangeFinished(client.lastChangeOp, true, QString());
        QVERIFY(!ctl.busy());

        // 2. The section hides (another settings page, or the window).
        QVERIFY(fillAll(fields));
        QVERIFY2(allCanUndo(fields), qPrintable(state(fields)));
        QVERIFY(root->setProperty("visible", false));
        QVERIFY2(emptyWithoutUndo(fields), qPrintable(state(fields)));
        QVERIFY(root->setProperty("visible", true));

        // 3. The probe hands the account to its account page: the form hides
        //    while the section stays shown.
        QVERIFY(fillAll(fields));
        QVERIFY2(allCanUndo(fields), qPrintable(state(fields)));
        ctl.refresh();
        QVERIFY(client.lastProbeOp != 0);
        Q_EMIT client.passwordChangeProbed(
            client.lastProbeOp, true, false,
            QStringLiteral("https://auth.example.org/account/"));
        QCOMPARE(root->property("mode").toString(), QStringLiteral("external"));
        QVERIFY2(emptyWithoutUndo(fields), qPrintable(state(fields)));

        QCOMPARE(client.changeCalls, 1);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(ChangePasswordSectionQmlTest)
#include "ChangePasswordSectionQmlTest.moc"
