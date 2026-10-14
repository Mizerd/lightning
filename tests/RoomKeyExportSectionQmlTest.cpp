// The Export room keys section's own handling of the passphrase, on the real
// QML against the real controller (scripted client). Pins:
//   * both fields are in Password echo mode;
//   * the export button stays disabled for an empty, short, weak (as Element:
//     below zxcvbn 3, mapped to rung 3) or mismatched pair, saying why when
//     weak, and enables for a matching strong one;
//   * starting an export hands the backend exactly the typed passphrase and
//     the chosen file;
//   * after the export starts, after the section hides, and when the account
//     changes, both fields are empty AND have no undo history: clear() would
//     keep it, so Ctrl+Z would bring a typed passphrase back;
//   * the explanation of what the file is and how to keep it is on screen.

#include "app/RoomKeyExportController.h"
#include "matrix/MockMatrixClient.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

class ExportClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    int exportCalls = 0;
    quint64 nextOp = 700;
    quint64 lastOp = 0;
    QString lastPath;
    QString lastPassphrase;

    bool supportsRoomKeyExport() const override { return true; }
    quint64 exportRoomKeys(const QString &path,
                           const QString &passphrase) override
    {
        ++exportCalls;
        lastPath = path;
        lastPassphrase = passphrase;
        lastOp = nextOp++;
        return lastOp;
    }
};

// Stands in for app.accounts: only the signal the section listens to.
class StubAccounts final : public QObject
{
    Q_OBJECT
Q_SIGNALS:
    void activeUserIdChanged();
};

// Only what the section reads from `app`.
class StubApp final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *roomKeyExport READ roomKeyExport CONSTANT)
    Q_PROPERTY(QObject *accounts READ accounts CONSTANT)
    Q_PROPERTY(bool loggedIn READ loggedIn CONSTANT)
public:
    StubApp(QObject *roomKeyExport, QObject *accounts)
        : m_roomKeyExport(roomKeyExport), m_accounts(accounts)
    {
    }
    QObject *roomKeyExport() const { return m_roomKeyExport; }
    QObject *accounts() const { return m_accounts; }
    bool loggedIn() const { return true; }
    Q_INVOKABLE QUrl defaultFileDialogFolder() const { return {}; }

private:
    QObject *m_roomKeyExport;
    QObject *m_accounts;
};

constexpr int kPasswordEcho = 2; // TextInput.Password

const QString kPass = QStringLiteral("a-long-export-passphrase");

} // namespace

class RoomKeyExportSectionQmlTest : public QObject
{
    Q_OBJECT

private:
    static bool type(QObject *field, const QString &text)
    {
        return QMetaObject::invokeMethod(field, "insert", Q_ARG(int, 0),
                                         Q_ARG(QString, text));
    }

    static bool fill(QObject *a, QObject *b, const QString &first,
                     const QString &second)
    {
        return type(a, first) && type(b, second);
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
                || f->property("canUndo").toBool())
                return false;
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

    static void wipe(const QList<QObject *> &fields)
    {
        for (QObject *f : fields)
            f->setProperty("text", QString());
    }

private Q_SLOTS:
    void fieldsAreMaskedGatedAndWipedWithoutUndo()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ExportClient client;
        RoomKeyExportController ctl;
        ctl.setClient(&client);
        StubAccounts accounts;
        StubApp app(&ctl, &accounts);

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
                              QStringLiteral("RoomKeyExportSection"));
        if (created.isEmpty())
            QVERIFY(created.wait(8000));
        QObject *root = engine.rootObjects().value(0);
        QVERIFY2(root, qPrintable(warnings.join(QLatin1Char('\n'))));

        QObject *pass = root->findChild<QObject *>(
            QStringLiteral("roomKeyExportPassphraseField"));
        QObject *confirm = root->findChild<QObject *>(
            QStringLiteral("roomKeyExportConfirmField"));
        QObject *button =
            root->findChild<QObject *>(QStringLiteral("roomKeyExportButton"));
        QVERIFY(pass && confirm && button);
        const QList<QObject *> fields{ pass, confirm };
        for (QObject *f : fields)
            QCOMPARE(f->property("echoMode").toInt(), kPasswordEcho);

        // The explanation and the safekeeping warning are there and say what
        // matters.
        QObject *explain = root->findChild<QObject *>(
            QStringLiteral("roomKeyExportExplanation"));
        QObject *warn =
            root->findChild<QObject *>(QStringLiteral("roomKeyExportWarning"));
        QVERIFY(explain && warn);
        QVERIFY(explain->property("text").toString().contains(
            QStringLiteral("passphrase")));
        QVERIFY(warn->property("text").toString().contains(
            QStringLiteral("Anyone with this file")));

        // Gating: nothing, short, mismatched, then a matching pair.
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(fill(pass, confirm, QStringLiteral("short"),
                     QStringLiteral("short")));
        QVERIFY(!button->property("enabled").toBool());
        wipe(fields);
        // Long enough but weak, and matching: still blocked, as Element
        // blocks below zxcvbn 3, and the strength line says why.
        QVERIFY(fill(pass, confirm, QStringLiteral("abcdefghij"),
                     QStringLiteral("abcdefghij")));
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(root->property("tooWeak").toBool());
        QObject *strength = root->findChild<QObject *>(
            QStringLiteral("roomKeyExportStrengthLabel"));
        QVERIFY(strength);
        QVERIFY2(strength->property("text").toString().contains(
                     QStringLiteral("Too weak to export")),
                 qPrintable(strength->property("text").toString()));
        wipe(fields);
        QVERIFY(fill(pass, confirm, kPass, kPass + QLatin1Char('x')));
        QVERIFY(!button->property("enabled").toBool());
        QObject *mismatch = root->findChild<QObject *>(
            QStringLiteral("roomKeyExportMismatchLabel"));
        QVERIFY(mismatch && mismatch->property("visible").toBool());
        wipe(fields);
        QVERIFY(fill(pass, confirm, kPass, kPass));
        QVERIFY(button->property("enabled").toBool());

        // 1. A location is chosen: the export starts with exactly what was
        //    typed, and the fields are wiped.
        QVERIFY2(allCanUndo(fields), qPrintable(state(fields)));
        const QUrl file =
            QUrl::fromLocalFile(dir.filePath(QStringLiteral("keys.txt")));
        QVERIFY(QMetaObject::invokeMethod(root, "start",
                                          Q_ARG(QVariant, QVariant(file))));
        QCOMPARE(client.exportCalls, 1);
        QCOMPARE(client.lastPassphrase, kPass);
        QCOMPARE(client.lastPath, file.toLocalFile());
        QVERIFY(ctl.busy());
        QVERIFY2(emptyWithoutUndo(fields), qPrintable(state(fields)));
        QVERIFY(!button->property("enabled").toBool());
        QObject *progress =
            root->findChild<QObject *>(QStringLiteral("roomKeyExportProgress"));
        QVERIFY(progress && progress->property("visible").toBool());
        Q_EMIT client.roomKeyExportFinished(client.lastOp, true, 12, QString());
        QVERIFY(!ctl.busy());
        QObject *result =
            root->findChild<QObject *>(QStringLiteral("roomKeyExportResult"));
        QVERIFY(result);
        QCOMPARE(result->property("text").toString(),
                 RoomKeyExportController::describe(QString(), 12));

        // 2. The section hides.
        QVERIFY(fill(pass, confirm, kPass, kPass));
        QVERIFY2(allCanUndo(fields), qPrintable(state(fields)));
        QVERIFY(root->setProperty("visible", false));
        QVERIFY2(emptyWithoutUndo(fields), qPrintable(state(fields)));
        QVERIFY(root->setProperty("visible", true));

        // 3. The active account changes; the old result goes with it.
        QVERIFY(fill(pass, confirm, kPass, kPass));
        QVERIFY2(allCanUndo(fields), qPrintable(state(fields)));
        Q_EMIT accounts.activeUserIdChanged();
        QVERIFY2(emptyWithoutUndo(fields), qPrintable(state(fields)));
        QVERIFY(ctl.resultMessage().isEmpty());

        QCOMPARE(client.exportCalls, 1);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
    }
};

QTEST_MAIN(RoomKeyExportSectionQmlTest)
#include "RoomKeyExportSectionQmlTest.moc"
