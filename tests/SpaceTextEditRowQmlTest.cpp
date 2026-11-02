
#include <QFile>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QtTest>

#include <memory>

namespace {

const char kScene[] = R"QML(
import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import MatrixClient

Window {
    width: 520
    height: 200
    visible: true
    property var sent: []
    property var topics: []
    ColumnLayout {
        width: 500
        SpaceTextEditRow {
            objectName: "nameRow"
            Layout.fillWidth: true
            targetId: "!a:example.org"
            authoritative: "Alpha"
            canEdit: true
            requireNonEmpty: true
            buttonText: "Rename"
            fieldObjectName: "nameField"
            buttonObjectName: "nameButton"
            onSubmitted: (value) => sent = sent.concat([value])
        }
        SpaceTextEditRow {
            objectName: "topicRow"
            Layout.fillWidth: true
            targetId: "!a:example.org"
            authoritative: "A topic"
            canEdit: true
            fieldObjectName: "topicField"
            buttonObjectName: "topicButton"
            onSubmitted: (value) => topics = topics.concat([value])
        }
    }
}
)QML";

QString readSource(const QString &relative)
{
    QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR "/") + relative);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

}

class SpaceTextEditRowQmlTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    std::unique_ptr<QObject> m_root;
    QQuickWindow *m_window = nullptr;

    QQuickItem *item(const char *name)
    {
        return m_root->findChild<QQuickItem *>(QLatin1String(name));
    }
    QStringList sent(const char *prop = "sent")
    {
        return m_root->property(prop).toStringList();
    }
    bool submit(QQuickItem *row)
    {
        QVariant ok;
        QMetaObject::invokeMethod(row, "submit", Q_RETURN_ARG(QVariant, ok));
        return ok.toBool();
    }
    void resetRow(QQuickItem *row, const QString &authoritative)
    {
        row->setProperty("pending", false);
        row->setProperty("canEdit", true);
        row->setProperty("targetCurrent", true);
        row->setProperty("authoritative", authoritative);
        QMetaObject::invokeMethod(row, "reset");
        m_root->setProperty("sent", QVariantList());
        m_root->setProperty("topics", QVariantList());
    }

private Q_SLOTS:
    void initTestCase()
    {
        QQmlComponent component(&m_engine);
        component.setData(kScene, QUrl(QStringLiteral("spacetexteditrow.qml")));
        m_root.reset(component.create());
        QVERIFY2(m_root, qPrintable(component.errorString()));
        m_window = qobject_cast<QQuickWindow *>(m_root.get());
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
    }

    void init()
    {
        resetRow(item("nameRow"), QStringLiteral("Alpha"));
        resetRow(item("topicRow"), QStringLiteral("A topic"));
    }

    void anUnchangedValueCannotBeResent()
    {
        auto *row = item("nameRow");
        auto *field = item("nameField");
        auto *button = item("nameButton");
        QVERIFY(row && field && button);
        QCOMPARE(field->property("text").toString(), QStringLiteral("Alpha"));
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(!submit(row));
        QVERIFY(sent().isEmpty());
        field->setProperty("text", QStringLiteral("  Alpha "));
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(!submit(row));
        QVERIFY(sent().isEmpty());
    }

    void anEditEnablesAPrimaryCommit()
    {
        auto *row = item("nameRow");
        auto *field = item("nameField");
        auto *button = item("nameButton");
        QCOMPARE(button->property("kind").toString(), QStringLiteral("primary"));
        field->setProperty("text", QStringLiteral(" Beta "));
        QVERIFY(button->property("enabled").toBool());
        QVERIFY(submit(row));
        QCOMPARE(sent(), QStringList{ QStringLiteral("Beta") });
    }

    void anEmptyNameIsRefusedAndAClearedTopicIsNot()
    {
        item("nameField")->setProperty("text", QStringLiteral("   "));
        QVERIFY(!item("nameButton")->property("enabled").toBool());
        QVERIFY(!submit(item("nameRow")));
        QVERIFY(sent().isEmpty());

        item("topicField")->setProperty("text", QString());
        QVERIFY(item("topicButton")->property("enabled").toBool());
        QVERIFY(submit(item("topicRow")));
        QCOMPARE(sent("topics"), QStringList{ QString() });
    }

    void permissionAndTargetAreRecheckedAtSubmission()
    {
        auto *row = item("nameRow");
        item("nameField")->setProperty("text", QStringLiteral("Gamma"));
        row->setProperty("canEdit", false);
        QVERIFY(!submit(row));
        row->setProperty("canEdit", true);
        row->setProperty("targetCurrent", false);
        QVERIFY(!submit(row));
        QVERIFY(sent().isEmpty());
        row->setProperty("targetCurrent", true);
        QVERIFY(submit(row));
        QCOMPARE(sent().size(), 1);
    }

    void oneSubmissionWhilePendingAndOnlyThatRowIsBusy()
    {
        auto *nameRow = item("nameRow");
        auto *topicRow = item("topicRow");
        item("nameField")->setProperty("text", QStringLiteral("Delta"));
        item("topicField")->setProperty("text", QStringLiteral("New topic"));
        QVERIFY(submit(nameRow));
        nameRow->setProperty("pending", true);
        topicRow->setProperty("pending", true);
        QVERIFY(!submit(nameRow));
        QVERIFY(!submit(topicRow));
        QCOMPARE(sent().size(), 1);
        QVERIFY(nameRow->property("busy").toBool());
        QVERIFY(!topicRow->property("busy").toBool());
        QVERIFY(item("nameButton")->property("busy").toBool());
        nameRow->setProperty("pending", false);
        topicRow->setProperty("pending", false);
        QVERIFY(!nameRow->property("busy").toBool());
    }

    void targetChangeResnapsAndRemoteChangeSparesADraft()
    {
        auto *row = item("nameRow");
        auto *field = item("nameField");
        field->setProperty("text", QStringLiteral("Half typed"));
        row->setProperty("authoritative", QStringLiteral("Remote rename"));
        QCOMPARE(field->property("text").toString(), QStringLiteral("Half typed"));
        QVERIFY(row->property("dirty").toBool());

        auto *topicField = item("topicField");
        item("topicRow")->setProperty("authoritative", QStringLiteral("Remote topic"));
        QCOMPARE(topicField->property("text").toString(), QStringLiteral("Remote topic"));

        row->setProperty("authoritative", QStringLiteral("Bravo"));
        row->setProperty("targetId", QStringLiteral("!b:example.org"));
        QCOMPARE(field->property("text").toString(), QStringLiteral("Bravo"));
        row->setProperty("targetId", QStringLiteral("!a:example.org"));
    }

    void returnInTheFieldTakesTheGuardedPath()
    {
        auto *field = item("nameField");
        field->forceActiveFocus();
        QVERIFY(field->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_Return);
        QVERIFY(sent().isEmpty());
        field->setProperty("text", QStringLiteral("Echo"));
        QTest::keyClick(m_window, Qt::Key_Return);
        QCOMPARE(sent(), QStringList{ QStringLiteral("Echo") });
    }

    void bothSpaceFormsUseTheSharedRow()
    {
        const QString pane = readSource(QStringLiteral("qml/TimelinePane.qml"));
        const QString dialog = readSource(QStringLiteral("qml/SpaceSettingsDialog.qml"));
        QVERIFY(!pane.isEmpty() && !dialog.isEmpty());
        const QRegularExpression use(QStringLiteral("SpaceTextEditRow\\s*\\{"));
        QCOMPARE(pane.count(use), 2);
        QCOMPARE(dialog.count(use), 2);
        QVERIFY(pane.contains(QStringLiteral("fieldObjectName: \"spaceNameEditField\"")));
        QVERIFY(dialog.contains(QStringLiteral("fieldObjectName: \"spaceSettingsNameField\"")));
    }
};

QTEST_MAIN(SpaceTextEditRowQmlTest)
#include "SpaceTextEditRowQmlTest.moc"
