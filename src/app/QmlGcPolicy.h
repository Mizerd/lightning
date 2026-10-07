#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace lightning {

// Run every QML garbage collection to completion, as every Qt before 6.8 did.
//
// From Qt 6.8 the collector works in 5 ms slices (QV4_GC_TIMELIMIT) and, in
// at least 6.11.2, a cycle that spans a large synchronous creation (Main.qml
// building MainScreen through its Loader) can sweep the property and method
// storage of objects that are still alive: Qt warns "Cannot find member data"
// for every property read on them, and the first Connections among them
// with a `function onX()` handler dereferences a null method in
// QQmlConnections::connectSignalsToMethods() (upstream QTBUG-148459; the
// upstream fix, qtdeclarative 42c76d7acd, only drops the handler silently).
// Which run hits it depends on whether a 5 ms deadline expires at the wrong
// allocation, so it comes and goes with machine load.
//
// The variable is read when each QML engine is constructed, so this must run
// before the first one exists. A value the user set wins, so the incremental
// collector can still be switched back on to debug it.
inline bool applyQmlGcPolicy()
{
    if (qEnvironmentVariableIsSet("QV4_GC_TIMELIMIT"))
        return false;
    // An explicit QByteArray: qputenv takes const QByteArray& on Qt 6.8 and
    // QByteArrayView on 6.11, and both accept this.
    qputenv("QV4_GC_TIMELIMIT", QByteArray("0"));
    return true;
}

} // namespace lightning
