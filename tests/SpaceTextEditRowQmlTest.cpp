// SpaceTextEditRow: the draft/dirty/permission contract the inline Space Home
// card and SpaceSettingsDialog share (master prompt Part 6, 2.3, UI-049..058).
//
// The inline card used to offer Rename/Save for an UNCHANGED value (sending
// an identical m.room.name / m.room.topic), used the faint default secondary
// kind, and checked nothing at submission time. The dialog had its own copy
// of the rules. Both now go through this component, so this suite proves the
// rules once, on the real compiled component:
//   * unchanged value -> no commit; the button says why;
//   * an edit enables a PRIMARY commit; one submission while pending;
//   * an empty name is refused, a cleared topic is allowed;
//   * permission and target identity are re-checked at submission;
//   * a target change re-snaps the field; a remote change re-snaps only an
//     untouched field;
//   * Return in the field takes the same guarded path;
//   * the busy spinner shows on the row that was committed, only.
// And two source facts: both forms instantiate the component.
//
// Offscreen QML: no homeserver, no real permissions. Rendering on a real
// desktop is NOT TESTED here.

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

} // namespace

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

    // UI-051: an unchanged value is not offered, and the button says why.
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
        // Whitespace around the same value is the same value once trimmed.
        field->setProperty("text", QStringLiteral("  Alpha "));
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(!submit(row));
        QVERIFY(sent().isEmpty());
    }

    // UI-049/052: an edit enables a primary commit (the inline card used the
    // faint default secondary kind).
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

    // UI-053: a name may not be emptied; a topic may be cleared.
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

    // UI-054: a permission lost, or roomInfo moved to another room, after the
    // button was enabled still refuses at submission.
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

    // UI-055: one operation while pending; the spinner shows on the row that
    // was committed and not on its sibling sharing the same pending flag.
    void oneSubmissionWhilePendingAndOnlyThatRowIsBusy()
    {
        auto *nameRow = item("nameRow");
        auto *topicRow = item("topicRow");
        item("nameField")->setProperty("text", QStringLiteral("Delta"));
        item("topicField")->setProperty("text", QStringLiteral("New topic"));
        QVERIFY(submit(nameRow));
        nameRow->setProperty("pending", true);
        topicRow->setProperty("pending", true);   // roomInfo.editPending is shared
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

    // UI-057/058: a target change always wins over typed text; a remote change
    // to the same target re-snaps only an untouched field.
    void targetChangeResnapsAndRemoteChangeSparesADraft()
    {
        auto *row = item("nameRow");
        auto *field = item("nameField");
        field->setProperty("text", QStringLiteral("Half typed"));
        row->setProperty("authoritative", QStringLiteral("Remote rename"));
        QCOMPARE(field->property("text").toString(), QStringLiteral("Half typed"));
        // The baseline moved, so the draft is still dirty against it.
        QVERIFY(row->property("dirty").toBool());

        auto *topicField = item("topicField");
        item("topicRow")->setProperty("authoritative", QStringLiteral("Remote topic"));
        QCOMPARE(topicField->property("text").toString(), QStringLiteral("Remote topic"));

        row->setProperty("authoritative", QStringLiteral("Bravo"));
        row->setProperty("targetId", QStringLiteral("!b:example.org"));
        QCOMPARE(field->property("text").toString(), QStringLiteral("Bravo"));
        row->setProperty("targetId", QStringLiteral("!a:example.org"));
    }

    // Return in the field goes through the same guards as the button.
    void returnInTheFieldTakesTheGuardedPath()
    {
        auto *field = item("nameField");
        field->forceActiveFocus();
        QVERIFY(field->hasActiveFocus());
        // Unchanged: Return sends nothing.
        QTest::keyClick(m_window, Qt::Key_Return);
        QVERIFY(sent().isEmpty());
        field->setProperty("text", QStringLiteral("Echo"));
        QTest::keyClick(m_window, Qt::Key_Return);
        QCOMPARE(sent(), QStringList{ QStringLiteral("Echo") });
    }

    // Both forms use the component, so they cannot drift apart again.
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
