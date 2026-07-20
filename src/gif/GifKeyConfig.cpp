#include "gif/GifKeyConfig.h"

#include "gif/GifBuildKeys.h"

#include <QFile>
#include <QFileInfo>

namespace gif {
namespace {

QString envVariableNameFor(const QString &providerId)
{
    if (providerId == QLatin1String("giphy"))
        return QStringLiteral("LIGHTNING_GIPHY_API_KEY");
    if (providerId == QLatin1String("klipy"))
        return QStringLiteral("LIGHTNING_KLIPY_API_KEY");
    return {};
}

// Strip one matching pair of surrounding quotes.
QString unquoted(QString value)
{
    if (value.size() >= 2) {
        const QChar first = value.front();
        if ((first == QLatin1Char('"') || first == QLatin1Char('\''))
            && value.back() == first) {
            return value.mid(1, value.size() - 2);
        }
    }
    return value;
}

} // namespace

QString keySourceName(KeySourceClass source)
{
    switch (source) {
    case KeySourceClass::Environment: return QStringLiteral("environment");
    case KeySourceClass::EnvFile: return QStringLiteral("env file");
    case KeySourceClass::BuildKey: return QStringLiteral("build key");
    case KeySourceClass::Absent: break;
    }
    return QStringLiteral("absent");
}

QHash<QString, QString> parseEnvAssignments(const QByteArray &content)
{
    QHash<QString, QString> values;
    const QString text = QString::fromUtf8(content);
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1String("export ")))
            line = line.mid(7).trimmed();
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString name = line.left(eq).trimmed();
        // Only simple identifier names — anything else is not a plain
        // assignment and is ignored, never evaluated.
        bool validName = !name.isEmpty();
        for (const QChar c : name) {
            if (!c.isLetterOrNumber() && c != QLatin1Char('_')) {
                validName = false;
                break;
            }
        }
        if (!validName)
            continue;
        const QString value = unquoted(line.mid(eq + 1).trimmed());
        values.insert(name, value);
    }
    return values;
}

QHash<QString, QString> readEnvFile(const QString &path)
{
    if (path.isEmpty())
        return {};
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return {};
    // Env files are tiny; a defensive cap keeps a mistaken path harmless.
    return parseEnvAssignments(file.read(64 * 1024));
}

QString gifEnvFilePath()
{
    const QString override =
        qEnvironmentVariable("LIGHTNING_GIF_ENV_FILE").trimmed();
    if (!override.isEmpty())
        return QFileInfo::exists(override) ? override : QString();
    const QString local = QStringLiteral("lightning-gif.env");
    return QFileInfo::exists(local) ? local : QString();
}

ResolvedKey resolveProviderKeyDetailed(const QString &providerId)
{
    const QString envName = envVariableNameFor(providerId);
    if (envName.isEmpty())
        return {};

    ResolvedKey resolved;
    const QString fromEnvironment =
        qEnvironmentVariable(envName.toUtf8().constData()).trimmed();
    if (!fromEnvironment.isEmpty()) {
        resolved.key = fromEnvironment;
        resolved.source = KeySourceClass::Environment;
        return resolved;
    }

    const QString fromFile =
        readEnvFile(gifEnvFilePath()).value(envName).trimmed();
    if (!fromFile.isEmpty()) {
        resolved.key = fromFile;
        resolved.source = KeySourceClass::EnvFile;
        return resolved;
    }

    const QString fromBuild = buildKeyFor(providerId).trimmed();
    if (!fromBuild.isEmpty()) {
        resolved.key = fromBuild;
        resolved.source = KeySourceClass::BuildKey;
        return resolved;
    }
    return resolved;
}

} // namespace gif
