#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace stalltrace {

bool enabled();

void install(int thresholdMsOverride = -1);

void shutdown();

int stallCount();
qint64 lastStallMs();
QByteArray lastStallCategory();

// RAII category marker for heavy GUI-thread sections; the innermost active one
// is logged with a stall. Inert off the GUI thread. The argument must be a
// string literal (it is stored, not copied).
class Scope
{
public:
    explicit Scope(const char *category);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    const char *m_previous = nullptr;
    bool m_active = false;
};

}
