
#include "app/SettingsManager.h"
#include "i18n/LocalizationManager.h"
#include "i18n/TimeLocale.h"

#include <QCoreApplication>
#include <QDate>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

void desktopWithLithuanianDates()
{
    qunsetenv("LC_ALL");
    qunsetenv("LANGUAGE");
    qunsetenv("LC_MESSAGES");
    qputenv("LANG", "en_US.UTF-8");
    qputenv("LC_TIME", "lt_LT.UTF-8");
    qputenv("LC_NUMERIC", "lt_LT.UTF-8");
}

QString readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

}

class LocaleFollowsLanguageTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("locale-follows-language-test"));
    }

    void anEnglishUiDoesNotTakeItsDatesFromLcTime()
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID)
        const QDate tenth(2026, 10, 10);
        if (QLocale::system().language() != QLocale::English
            || QLocale::system().toString(tenth, QStringLiteral("MMM"))
                   == QLatin1String("Oct"))
            QSKIP("this platform does not take the system locale's categories "
                  "from LC_TIME (the precondition of the defect)");
        const QLocale before;
        LocalizationManager loc(nullptr);
        loc.setLanguage(QStringLiteral("en"));
        QCOMPARE(QLocale().toString(tenth, QStringLiteral("d MMM")),
                 QStringLiteral("10 Oct"));
        QCOMPARE(QLocale().toString(1.5, 'f', 1), QStringLiteral("1.5"));
        QCOMPARE(QLocale().language(), QLocale::English);
        QLocale::setDefault(before);
#else
        QSKIP("the system locale is not environment-driven here");
#endif
    }

    void theDesktopsOwnConventionsAreKeptForItsLanguage()
    {
        using L = LocalizationManager;
        const QLocale enUS(QStringLiteral("en_US"));
        const QDate day(2026, 10, 20);
        QCOMPARE(L::formattingLocale(QStringLiteral("en"), enUS,
                                     QLocale(QStringLiteral("en_GB")))
                     .toString(day, QLocale::ShortFormat),
                 QStringLiteral("20/10/2026"));
        QCOMPARE(L::formattingLocale(QStringLiteral("en"), QLocale(QStringLiteral("en_GB")),
                                     QLocale(QStringLiteral("en_GB")))
                     .name(),
                 QStringLiteral("en_GB"));
        QCOMPARE(L::formattingLocale(QStringLiteral("en"), enUS,
                                     QLocale(QStringLiteral("lt_LT")))
                     .toString(day, QStringLiteral("d MMM")),
                 QStringLiteral("20 Oct"));
        QCOMPARE(L::formattingLocale(QStringLiteral("en"), QLocale(QStringLiteral("lt_LT")),
                                     QLocale(QStringLiteral("lt_LT")))
                     .language(),
                 QLocale::English);
        QCOMPARE(L::formattingLocale(QStringLiteral("de"), enUS, enUS).language(),
                 QLocale::German);
        QCOMPARE(L::formattingLocale(QStringLiteral("pt"), QLocale(QStringLiteral("pt_BR")),
                                     QLocale::c())
                     .name(),
                 QStringLiteral("pt_BR"));
    }

    void lcTimeInTheUiLanguageDecidesTheDateFormat()
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID)
        const QByteArray saved = qgetenv("LC_TIME");
        const QLocale before;
        const auto restore = qScopeGuard([&] {
            qputenv("LC_TIME", saved);
            QLocale::setDefault(before);
        });
        qputenv("LC_TIME", "en_GB.UTF-8");
        QCOMPARE(i18n::timeCategoryLocale().name(), QStringLiteral("en_GB"));
        LocalizationManager loc(nullptr);
        loc.setLanguage(QStringLiteral("en"));
        QCOMPARE(QLocale().toString(QDate(2026, 10, 20), QLocale::ShortFormat),
                 QStringLiteral("20/10/2026"));
#else
        QSKIP("the time category is not environment-driven here");
#endif
    }

    void followTheSystemClockKeepsTheCycleNotTheWords()
    {
        using i18n::followSystemClockFormat;
        const QLocale enUS(QStringLiteral("en_US"));
        const QLocale enGB(QStringLiteral("en_GB"));
        const QLocale de(QStringLiteral("de_DE"));
        QCOMPARE(followSystemClockFormat(enGB, enGB, enGB.timeFormat(QLocale::ShortFormat)),
                 enGB.timeFormat(QLocale::ShortFormat));
        QVERIFY(!i18n::usesTwelveHourClock(
            followSystemClockFormat(enGB, enGB, enGB.timeFormat(QLocale::ShortFormat))));
        const QLocale lt(QStringLiteral("lt_LT"));
        QCOMPARE(followSystemClockFormat(enUS, lt, lt.timeFormat(QLocale::ShortFormat)),
                 QStringLiteral("HH:mm"));
        const QLocale frCA(QStringLiteral("fr_CA"));
        const QString fromFrench =
            followSystemClockFormat(enUS, frCA, frCA.timeFormat(QLocale::ShortFormat));
        QVERIFY2(!fromFrench.contains(QLatin1Char('\'')), qPrintable(fromFrench));
        QVERIFY(!i18n::usesTwelveHourClock(fromFrench));
        QVERIFY(i18n::usesTwelveHourClock(
            followSystemClockFormat(de, enUS, enUS.timeFormat(QLocale::ShortFormat))));
        QCOMPARE(followSystemClockFormat(de, enGB, enGB.timeFormat(QLocale::ShortFormat)),
                 de.timeFormat(QLocale::ShortFormat));
        QCOMPARE(followSystemClockFormat(de, QLocale::c(), QStringLiteral("HH:mm:ss")),
                 de.timeFormat(QLocale::ShortFormat));
    }

    void theClockSettingFollowsTheDesktopsCycle()
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID)
        if (i18n::timeCategoryLocale().language() != QLocale::Lithuanian)
            QSKIP("LC_TIME is not the Lithuanian this case sets up");
        const QLocale before;
        LocalizationManager loc(nullptr);
        loc.setLanguage(QStringLiteral("en"));
        SettingsManager settings;
        QCOMPARE(settings.clockFormat(), 0);
        QCOMPARE(settings.clockTimeFormat(), QStringLiteral("HH:mm"));
        QLocale::setDefault(before);
#else
        QSKIP("the time category is not environment-driven here");
#endif
    }

    void aBindingOverAppLocaleFollowsASwitch()
    {
        const QLocale before;
        QLocale::setDefault(QLocale(QStringLiteral("en_US")));
        QQmlEngine engine;
        qmlRegisterSingletonInstance("LocaleProbe", 1, 0, "AppLocale",
                                     AppLocale::instance());
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQml
            import LocaleProbe
            QtObject {
                readonly property date when: new Date(2026, 9, 10)
                property string followed: Qt.locale(AppLocale.name).toString(when, "MMMM")
                property string stale: Qt.locale().toString(when, "MMMM")
                property string size: Number(1.5).toLocaleString(Qt.locale(AppLocale.name), "f", 1)
            }
        )QML", QUrl(QStringLiteral("probe.qml")));
        std::unique_ptr<QObject> probe(component.create());
        QVERIFY2(probe, qPrintable(component.errorString()));
        QCOMPARE(probe->property("followed").toString(), QStringLiteral("October"));
        QCOMPARE(probe->property("size").toString(), QStringLiteral("1.5"));

        QSignalSpy changed(AppLocale::instance(), &AppLocale::changed);
        QLocale::setDefault(QLocale(QStringLiteral("de_DE")));
        AppLocale::instance()->notifyChanged();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(AppLocale::instance()->name(), QStringLiteral("de"));
        QCOMPARE(probe->property("followed").toString(), QStringLiteral("Oktober"));
        QCOMPARE(probe->property("size").toString(), QStringLiteral("1,5"));
        QCOMPARE(probe->property("stale").toString(), QStringLiteral("October"));
        QLocale::setDefault(before);
    }

    void switchingTheLanguageAnnouncesItOnAppLocale()
    {
        const QLocale before;
        LocalizationManager loc(nullptr);
        QString nameSeen;
        connect(AppLocale::instance(), &AppLocale::changed, this,
                [&] { nameSeen = AppLocale::instance()->name(); });
        QSignalSpy changed(AppLocale::instance(), &AppLocale::changed);
        loc.setLanguage(QStringLiteral("en"));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(nameSeen, QLocale().bcp47Name());
        disconnect(AppLocale::instance(), nullptr, this, nullptr);
        QLocale::setDefault(before);
    }

    void everyQmlLocaleCallNamesAppLocale()
    {
        static const QRegularExpression localeCall(
            QStringLiteral("Qt\\.locale\\(([^)]*)\\)"));
        static const QRegularExpression bareToLocale(
            QStringLiteral("\\.toLocale(?:Date|Time)?String\\(\\s*\\)"));
        static const QRegularExpression sizeToFixed(
            QStringLiteral("toFixed\\([^\\n]*\\b[KMG]B\\b|\\b[KMG]B\\b[^\\n]*toFixed\\("));
        QStringList offenders;
        int files = 0;
        int calls = 0;
        QDirIterator it(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml"),
                        { QStringLiteral("*.qml") }, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QStringList lines = readAll(path).split(QLatin1Char('\n'));
            ++files;
            for (int i = 0; i < lines.size(); ++i) {
                const QString &line = lines.at(i);
                if (line.trimmed().startsWith(QLatin1String("//")))
                    continue;
                const QString where = QStringLiteral("%1:%2: %3")
                                          .arg(QFileInfo(path).fileName())
                                          .arg(i + 1)
                                          .arg(line.trimmed());
                auto calls_ = localeCall.globalMatch(line);
                while (calls_.hasNext()) {
                    ++calls;
                    if (calls_.next().captured(1).trimmed()
                        != QLatin1String("AppLocale.name"))
                        offenders << where;
                }
                if (bareToLocale.match(line).hasMatch()
                    || sizeToFixed.match(line).hasMatch())
                    offenders << where;
            }
        }
        QVERIFY2(files > 100, "qml/ was not found or is implausibly small");
        QVERIFY2(calls > 20, "the scan found implausibly few Qt.locale() calls");
        QVERIFY2(offenders.isEmpty(), qPrintable(offenders.join(QLatin1Char('\n'))));
    }

private:
    QTemporaryDir m_configHome;
};

int main(int argc, char **argv)
{
    desktopWithLithuanianDates();
    QCoreApplication app(argc, argv);
    LocaleFollowsLanguageTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "LocaleFollowsLanguageTest.moc"
