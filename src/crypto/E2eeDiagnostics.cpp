#include "crypto/E2eeDiagnostics.h"

#include <QCryptographicHash>

Q_LOGGING_CATEGORY(lcE2ee, "lightning.e2ee")

namespace matrix::e2ee {

QString redactId(const QString &id)
{
    if (id.isEmpty())
        return QStringLiteral("<none>");

    QChar sigil;
    QStringView rest(id);
    const QChar first = id.at(0);
    if (first == QLatin1Char('!') || first == QLatin1Char('$')
        || first == QLatin1Char('@') || first == QLatin1Char('#')) {
        sigil = first;
        rest = rest.mid(1);
    }

    // SHA-256 → first 8 hex chars: stable for correlation, not reversible to
    // the original id, and short enough to keep logs readable.
    const QByteArray digest = QCryptographicHash::hash(
        rest.toUtf8(), QCryptographicHash::Sha256);
    const QString token = QString::fromLatin1(digest.toHex().left(8));
    return sigil.isNull() ? token : (QString(sigil) + token);
}

} // namespace matrix::e2ee
