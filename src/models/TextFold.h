#pragma once

#include <QChar>
#include <QString>

// One fold for every "type a few letters, find the name" surface, so the room
// list filter, the quick switcher and the local message search (whose index
// removes diacritics) agree: "cafe" finds "Café Zürich" and "CAFE" does too.
// NFD, drop the combining marks, case-fold. Applied to the query AND the
// candidate; folding only one side is the defect this exists to prevent.
namespace textfold {

inline QString fold(const QString &text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar ch : decomposed) {
        switch (ch.category()) {
        case QChar::Mark_NonSpacing:
        case QChar::Mark_SpacingCombining:
        case QChar::Mark_Enclosing:
            continue;
        default:
            out.append(ch);
        }
    }
    return out.toCaseFolded();
}

} // namespace textfold
