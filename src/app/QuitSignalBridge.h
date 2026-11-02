#pragma once

#include <functional>

class QObject;

namespace lightning::quitsignals {

bool install(QObject *context, std::function<void(int signo)> onSignal);

}
