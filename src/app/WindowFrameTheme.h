#pragma once

#include <QColor>
#include <QEvent>

namespace lightning::windowframe {

enum class Reapply { Ignore, AfterQt };

/// The events after which Qt may have rewritten a window's caption mode:
/// palette and system theme changes, the native window's creation, showing
/// and state changes. Never "now": the request always follows Qt's.
Reapply reapplyFor(QEvent::Type type);

const char *frameModeName(bool dark);

bool prefersDarkFrame(const QColor &window);

void install();

}
