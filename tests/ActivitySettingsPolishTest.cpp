
#include <QtTest>

#include "presence/MusicDetection.h"

class ActivitySettingsPolishTest : public QObject
{
    Q_OBJECT
private slots:
    void truncationEndsWithAnEllipsisInsideTheBudget()
    {
        const QString cut = musicdetect::cleanField(QString(500, QLatin1Char('t')), 100);
        QCOMPARE(cut.size(), 100);
        QVERIFY(cut.endsWith(QChar(0x2026)));
        const QString exact(100, QLatin1Char('x'));
        QCOMPARE(musicdetect::cleanField(exact, 100), exact);
        const QString emoji = QString(98, QLatin1Char('a')) + QStringLiteral("\U0001F600\U0001F600");
        const QString c2 = musicdetect::cleanField(emoji, 100);
        QVERIFY(c2.endsWith(QChar(0x2026)));
        QVERIFY(!QChar(c2.at(c2.size() - 2)).isHighSurrogate());
    }
};

QTEST_GUILESS_MAIN(ActivitySettingsPolishTest)
#include "ActivitySettingsPolishTest.moc"
