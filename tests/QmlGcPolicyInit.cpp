// Linked into every QML test (lightning_configure_qml_test): a test process
// builds its QML engines under the same collector policy as the application,
// before main() and so before the first engine. See src/app/QmlGcPolicy.h.
#include "app/QmlGcPolicy.h"

#include <QtGlobal>

namespace {
void applyQmlGcPolicyForTests()
{
    lightning::applyQmlGcPolicy();
}
} // namespace

Q_CONSTRUCTOR_FUNCTION(applyQmlGcPolicyForTests)
