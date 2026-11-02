
#include "crypto/E2eeDiagnostics.h"

#include <QtTest/QtTest>

using matrix::e2ee::redactId;

class E2eeDiagnosticsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void emptyMapsToMarker()
    {
        QCOMPARE(redactId(QString()), QStringLiteral("<none>"));
    }

    void preservesSigilButNotTheBody()
    {
        const QString room = QStringLiteral("!secretRoom:example.org");
        const QString token = redactId(room);
        QVERIFY(token.startsWith(QLatin1Char('!')));
        QVERIFY(!token.contains(QStringLiteral("secretRoom")));
        QVERIFY(!token.contains(QStringLiteral("example.org")));
        QVERIFY(token.length() < room.length());

        const QString event = redactId(QStringLiteral("$abc123:example.org"));
        QVERIFY(event.startsWith(QLatin1Char('$')));
        const QString user = redactId(QStringLiteral("@alice:example.org"));
        QVERIFY(user.startsWith(QLatin1Char('@')));
        const QString session = redactId(QStringLiteral("AbCdEfSessionId"));
        QVERIFY(!session.contains(QStringLiteral("Session")));
        QVERIFY(!session.startsWith(QLatin1Char('!')));
    }

    void stableForCorrelationButDistinctForDifferentIds()
    {
        const QString a = QStringLiteral("$event-one:example.org");
        const QString b = QStringLiteral("$event-two:example.org");
        QCOMPARE(redactId(a), redactId(a));
        QVERIFY(redactId(a) != redactId(b));
    }

    void tokenIsNotReversibleToInput()
    {
        const QString id = QStringLiteral("$verylongsecreteventidentifier:hs");
        const QString token = redactId(id);
        QVERIFY(!id.contains(token.mid(1)));
        QVERIFY(!token.contains(QStringLiteral("verylongsecret")));
    }
};

QTEST_APPLESS_MAIN(E2eeDiagnosticsTest)
#include "E2eeDiagnosticsTest.moc"
