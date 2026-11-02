
#include "matrix/MockMatrixClient.h"
#include "models/QuickSwitcherModel.h"
#include "models/TextFold.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 2000;

QStringList names(const QuickSwitcherModel &m)
{
    QStringList out;
    for (int row = 0; row < m.rowCount(); ++row)
        out << m.data(m.index(row, 0), QuickSwitcherModel::NameRole).toString();
    return out;
}

QString categoryFor(const QuickSwitcherModel &m, const QString &name)
{
    for (int row = 0; row < m.rowCount(); ++row) {
        const QModelIndex idx = m.index(row, 0);
        if (m.data(idx, QuickSwitcherModel::NameRole).toString() == name)
            return m.data(idx, QuickSwitcherModel::CategoryRole).toString();
    }
    return {};
}
}

class QuickSwitcherTest : public QObject
{
    Q_OBJECT

    static bool login(MockMatrixClient &client)
    {
        QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
        client.login(QStringLiteral("https://mock.local"),
                     QStringLiteral("alice"), QStringLiteral("x"));
        if (!spy.wait(kSignalTimeoutMs))
            return false;
        client.startSync();
        return true;
    }

private Q_SLOTS:
    void matchingIgnoresAccentsAndCase()
    {
        const QString cafe = QStringLiteral("Caf\u00e9 Z\u00fcrich");
        QVERIFY(QuickSwitcherModel::matchScore(QStringLiteral("cafe"), cafe, {}) > 0);
        QVERIFY(QuickSwitcherModel::matchScore(QStringLiteral("ZURICH"), cafe, {}) > 0);
        QVERIFY(QuickSwitcherModel::matchScore(QStringLiteral("caf\u00e9"),
                                               QStringLiteral("Cafe"), {}) > 0);
        QVERIFY(QuickSwitcherModel::matchScore(
                    QStringLiteral("jose"), QStringLiteral("Direct chat"),
                    { QStringLiteral("Jos\u00e9") }) > 0);
        QCOMPARE(QuickSwitcherModel::matchScore(QStringLiteral("cafe"), cafe, {}),
                 1000);
        QVERIFY(QuickSwitcherModel::matchScore(QStringLiteral("xyz"), cafe, {}) < 0);
        QCOMPARE(textfold::fold(QStringLiteral("\u00c5ngstr\u00f6m")),
                 QStringLiteral("angstrom"));
    }

    void matchScorePolicy()
    {
        QCOMPARE(QuickSwitcherModel::matchScore(QString(), QStringLiteral("x"),
                                                {}), 0);
        QVERIFY(QuickSwitcherModel::matchScore(QStringLiteral("zzz"),
                                               QStringLiteral("General"), {})
                < 0);
        const int prefix = QuickSwitcherModel::matchScore(
            QStringLiteral("gen"), QStringLiteral("General"), {});
        const int word = QuickSwitcherModel::matchScore(
            QStringLiteral("team"), QStringLiteral("Dev Team"), {});
        const int sub = QuickSwitcherModel::matchScore(
            QStringLiteral("ener"), QStringLiteral("General"), {});
        QVERIFY(prefix > word);
        QVERIFY(word > sub);
        QVERIFY(sub > 0);
        QVERIFY(QuickSwitcherModel::matchScore(
                    QStringLiteral("bob"), QStringLiteral("Direct chat"),
                    { QStringLiteral("@bob:mock.local") }) > 0);
        QVERIFY(QuickSwitcherModel::matchScore(
                    QStringLiteral("bob chat"), QStringLiteral("Direct chat"),
                    { QStringLiteral("@bob:mock.local") }) > 0);
    }

    void emptyQueryListsAllRoomsWithCategories()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.refresh();

        const QStringList all = names(model);
        QVERIFY(all.contains(QStringLiteral("General")));
        QVERIFY(all.contains(QStringLiteral("Developers")));
        QVERIFY(all.contains(QStringLiteral("Bob")));
        QVERIFY(all.contains(QStringLiteral("Team")));

        QCOMPARE(categoryFor(model, QStringLiteral("Bob")),
                 QStringLiteral("dm"));
        QCOMPARE(categoryFor(model, QStringLiteral("Team")),
                 QStringLiteral("space"));
        QCOMPARE(categoryFor(model, QStringLiteral("General")),
                 QStringLiteral("room"));
    }

    void nameSearchFiltersAndRanks()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);

        model.setQuery(QStringLiteral("dev"));
        QCOMPARE(names(model), QStringList{ QStringLiteral("Developers") });

        model.setQuery(QStringLiteral("team"));
        QCOMPARE(names(model), QStringList{ QStringLiteral("Team") });
    }

    void dmMatchesByParticipantMxid()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);

        // The DM is named "Bob" and its participant is @bob:mock.local —
        // typing the mxid still finds it.
        model.setQuery(QStringLiteral("@bob"));
        const QStringList found = names(model);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first(), QStringLiteral("Bob"));
        QCOMPARE(categoryFor(model, QStringLiteral("Bob")),
                 QStringLiteral("dm"));
    }

    void noMatchYieldsEmpty()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.setQuery(QStringLiteral("nonexistent-room-xyz"));
        QCOMPARE(model.rowCount(), 0);
    }

    void resultAtCarriesRoutingPayload()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.setQuery(QStringLiteral("team"));
        QCOMPARE(model.rowCount(), 1);
        const QVariantMap r = model.resultAt(0);
        QCOMPARE(r.value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!space-team:mock.local"));
        QVERIFY(r.value(QStringLiteral("isSpace")).toBool());
        QVERIFY(!r.value(QStringLiteral("isInvite")).toBool());
    }

    void logoutClearsResults()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        QuickSwitcherModel model;
        model.setClient(&client);
        model.refresh();
        QVERIFY(model.rowCount() > 0);

        client.logout();
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(model.query().isEmpty());
    }
};

QTEST_MAIN(QuickSwitcherTest)
#include "QuickSwitcherTest.moc"
