#pragma once

#include <QString>
#include <QVariantMap>

namespace MarkdownFormat {

struct Result {
    QString text;
    int selectionStart = 0;
    int selectionEnd = 0;
};

Result toggle(const QString &format, const QString &text,
              int selectionStart, int selectionEnd);

QVariantMap state(const QString &text, int selectionStart, int selectionEnd);

}
