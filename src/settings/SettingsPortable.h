#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

class SettingsManager;

namespace settingsport {

enum class Kind { Bool, Int, String, StringList };

enum Flag {
    Export = 1,
    Sync = 2,
};

struct Field
{
    QString name;
    Kind kind = Kind::Bool;
    qint64 lo = 0;
    qint64 hi = 0;
    int flags = 0;
    QString group;
    int maxLen = 0;
    QStringList allowed;
    QVariant def;
};

const QList<Field> &fields();
const Field *find(const QString &name);
QStringList names(int flag);

// One value against one field. Never clamps: an out-of-range or wrongly-typed
// value is REJECTED with a short reason, so an import cannot turn a hostile
// 9999 into "the nearest valid" value.
struct Checked
{
    bool ok = false;
    QVariant value;
    QString reason;
};
Checked check(const Field &field, const QVariant &raw);

QVariantMap readProperties(const QObject *object, int flag);
QStringList writeProperties(QObject *object, const QVariantMap &values);

QStringList presentationKeys();
QVariantMap readSettings(const SettingsManager *settings, int flag);
QStringList writeSettings(SettingsManager *settings, const QVariantMap &values);

constexpr auto kFileFormat = "lightning-settings";
constexpr int kFileVersion = 2;
constexpr qint64 kMaxFileBytes = 512 * 1024;
constexpr int kMaxShortcuts = 256;
constexpr int kMaxThemes = 24;

struct Document
{
    QVariantMap settings;
    QMap<QString, QString> shortcuts;
    QStringList themes;
    int activeTheme = -1;
    QString app;
};

QByteArray buildDocument(const Document &doc, const QString &exportedAtIso);

struct ParseResult
{
    bool ok = false;
    QString error;
    int version = 0;
    bool newerVersion = false;
    Document doc;
    QMap<QString, QString> rejected;
    QStringList unknown;
    QStringList shortcutsRejected;
    int themesDropped = 0;
};
ParseResult parseDocument(const QByteArray &bytes);

struct Change
{
    QString name;
    QString group;
    QVariant from;
    QVariant to;
};
QList<Change> diff(const QVariantMap &current, const QVariantMap &incoming);

constexpr int kSyncVersion = 1;
constexpr int kMaxBlobBytes = 48 * 1024; // keep in step with rust MAX_BLOB_BYTES
constexpr qint64 kMaxSkewMs = 5 * 60 * 1000;

struct Entry
{
    QVariant v;
    qint64 t = 0;
};
using Blob = QMap<QString, Entry>;

struct ParsedBlob
{
    bool present = false;
    bool readable = true;
    int version = 0;
    bool newerVersion = false;
    Blob entries;
    QJsonObject keep;
    QStringList rejected;
};
ParsedBlob parseBlob(const QJsonObject &content, bool present = true);
QJsonObject composeBlob(const Blob &entries, const QJsonObject &keep);

struct State
{
    QVariantMap lastSynced;
    QMap<QString, qint64> lastTs;
    QMap<QString, qint64> pending;
    // A server timestamp from the future (a fast or hostile clock) counts as
    // the moment it was FIRST SEEN, remembered here, so it cannot slide
    // forward with `now` and win every later conflict.
    QMap<QString, qint64> futureT;
    QMap<QString, qint64> futureSeen;
    bool operator==(const State &o) const
    {
        return lastSynced == o.lastSynced && lastTs == o.lastTs && pending == o.pending
            && futureT == o.futureT && futureSeen == o.futureSeen;
    }
};
QJsonObject stateToJson(const State &s);
State stateFromJson(const QJsonObject &o);

State detectLocalChanges(const QVariantMap &local, State state, qint64 now);

State absorbLocal(State state, const QVariantMap &local);

struct MergeResult
{
    QVariantMap applyLocal;
    QStringList lostLocal;
    Blob toWrite;
    QJsonObject keep;
    bool needsWrite = false;
    bool readOnly = false;       // server schema is newer: never write
    State afterApply;
    State afterWrite;
};
MergeResult merge(const ParsedBlob &server, const QVariantMap &local, const State &state, qint64 now);

struct FirstRunPlan
{
    QStringList differing;
    bool serverEmpty = true;
};
FirstRunPlan planFirstRun(const ParsedBlob &server, const QVariantMap &local);
MergeResult adoptServer(const ParsedBlob &server, const QVariantMap &local, qint64 now);
MergeResult uploadLocal(const ParsedBlob &server, const QVariantMap &local, qint64 now);

}
