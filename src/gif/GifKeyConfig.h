#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

// One source of truth for GIF provider key configuration. Resolution
// precedence for each provider (an empty value NEVER overrides a non-empty
// lower-precedence source):
//
//   1. process environment  (LIGHTNING_GIPHY_API_KEY / LIGHTNING_KLIPY_API_KEY)
//   2. local development env file (LIGHTNING_GIF_ENV_FILE override, else
//      ./lightning-gif.env in the working directory) — parsed with a small
//      explicit parser, never executed as shell
//   3. key compiled into an official release build
//   4. unavailable
//
// Key VALUES never leave this module except as the resolved key handed to the
// provider transport. Diagnostics may expose only the provider name, a
// configured boolean, the sanitized source class, and the key length.
namespace gif {

enum class KeySourceClass { Absent, BuildKey, EnvFile, Environment };

struct ResolvedKey {
    QString key;
    KeySourceClass source = KeySourceClass::Absent;
    bool configured() const { return !key.isEmpty(); }
};

// Presentation-safe name for diagnostics ("environment", "env file",
// "build key", "absent").
QString keySourceName(KeySourceClass source);

// Parse KEY=value assignments from env-file content. Supports optional
// `export `, single/double quotes, surrounding whitespace, blank lines,
// `#` comments, CRLF endings, and a missing final newline. Lines that are
// not simple assignments are ignored; nothing is ever executed.
QHash<QString, QString> parseEnvAssignments(const QByteArray &content);

// parseEnvAssignments over a file; empty map when the file is absent or
// unreadable.
QHash<QString, QString> readEnvFile(const QString &path);

// The local development env file: $LIGHTNING_GIF_ENV_FILE when set,
// otherwise ./lightning-gif.env relative to the working directory. Returns
// an empty string when neither exists.
QString gifEnvFilePath();

// Full-precedence resolution for "giphy" / "klipy".
ResolvedKey resolveProviderKeyDetailed(const QString &providerId);

} // namespace gif
