// Linked into every QML test (lightning_configure_qml_test), beside
// QmlGcPolicyInit.cpp. Before main(), point XDG_DATA_HOME at a fresh
// directory removed at exit, so a suite that builds an AppController never
// reads or writes the developer's real app data -- among it the spell
// checker's personal dictionary (<app-data root>/spelling/
// personal-dictionary.txt), which AppController loads at construction.
// A suite that wants its own directory still sets XDG_DATA_HOME itself; it
// overrides this one. LIGHTNING_TEST_KEEP_DATA_HOME=1 opts out, for a
// deliberate run against real data.
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtGlobal>

namespace {
void useHermeticDataHome()
{
    if (qEnvironmentVariableIntValue("LIGHTNING_TEST_KEEP_DATA_HOME") == 1)
        return;
    // Function-local static: removed by its destructor after main returns.
    static QTemporaryDir dataHome(QDir::tempPath()
                                  + QStringLiteral("/lightning-test-data-XXXXXX"));
    if (dataHome.isValid())
        qputenv("XDG_DATA_HOME", QFile::encodeName(dataHome.path()));
}
} // namespace

Q_CONSTRUCTOR_FUNCTION(useHermeticDataHome)
