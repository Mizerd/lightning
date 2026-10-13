#pragma once

// The preference set that can leave this device: written to a settings file
// (export / import) and kept in account data (sync). ONE schema and ONE codec
// serve both, so what a file may carry and what the server may carry cannot
// drift apart.
//
// Pure Qt Core: no SettingsManager, no network, no QML. The schema is an
// ALLOW-LIST keyed by SettingsManager's Q_PROPERTY names. Anything not listed
// here (tokens, recovery material, device ids, store paths, account ids,
// window geometry, download folders, hardware ids) cannot be exported or
// synced, because nothing ever reads it.

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

namespace settingsport {

enum class Kind { Bool, Int, String, StringList };

// Where a setting may travel.
enum Flag {
    Export = 1, // the settings file
    Sync = 2,   // account data (always also Export)
};

struct Field
{
    QString name;      // SettingsManager property name
    Kind kind = Kind::Bool;
    qint64 lo = 0;     // Int: inclusive bounds
    qint64 hi = 0;
    int flags = 0;
    QString group;     // heading in the import preview
    int maxLen = 0;    // String: longest accepted; StringList: longest item
    QStringList allowed; // String/StringList: closed set when non-empty
    QVariant def;      // value of a setting nobody touched
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
    QString reason; // "type", "range", "length", "not_allowed", "control"
};
Checked check(const Field &field, const QVariant &raw);

// Read / write the schema's fields through a QObject's Q_PROPERTY system (the
// SettingsManager). A value that fails check() on the way out is omitted; one
// that fails on the way in is returned in `rejected` and not written.
QVariantMap readProperties(const QObject *object, int flag);
QStringList writeProperties(QObject *object, const QVariantMap &values);

// ---- settings file ---------------------------------------------------------

constexpr auto kFileFormat = "lightning-settings";
constexpr int kFileVersion = 1;
constexpr qint64 kMaxFileBytes = 512 * 1024;
constexpr int kMaxShortcuts = 256;
constexpr int kMaxThemes = 24;

struct Document
{
    QVariantMap settings;                // name -> value, Export fields only
    QMap<QString, QString> shortcuts;    // action id -> portable sequence
    QStringList themes;                  // custom-theme share payloads (compact JSON)
    int activeTheme = -1;                // index into themes, -1 none
    QString app;                         // exporting build's version, informational
};

QByteArray buildDocument(const Document &doc, const QString &exportedAtIso);

struct ParseResult
{
    bool ok = false;
    QString error;             // "empty", "too_large", "not_json", "not_settings"
    int version = 0;
    bool newerVersion = false; // written by a newer build; known keys still read
    Document doc;              // accepted values only
    QMap<QString, QString> rejected; // key -> reason
    QStringList unknown;       // keys not in the schema (ignored)
    QStringList shortcutsRejected;
    int themesDropped = 0;
};
ParseResult parseDocument(const QByteArray &bytes);

// One row of the import preview.
struct Change
{
    QString name;
    QString group;
    QVariant from;
    QVariant to;
};
QList<Change> diff(const QVariantMap &current, const QVariantMap &incoming);

// ---- sync ------------------------------------------------------------------

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
    Blob entries;               // valid Sync entries only
    QJsonObject keep;           // entries this build does not know, kept verbatim
    QStringList rejected;
};
ParsedBlob parseBlob(const QJsonObject &content, bool present = true);
QJsonObject composeBlob(const Blob &entries, const QJsonObject &keep);

// What this device last agreed with the server, and what it changed since.
struct State
{
    QVariantMap lastSynced;            // name -> value both sides held
    QMap<QString, qint64> lastTs;      // name -> t of lastSynced
    QMap<QString, qint64> pending;     // name -> time of an unsynced local change
    // A server timestamp from the future (a fast or hostile clock) counts as
    // the moment it was FIRST SEEN, remembered here, so it cannot slide
    // forward with `now` and win every later conflict.
    QMap<QString, qint64> futureT;     // name -> the raw server t that was ahead
    QMap<QString, qint64> futureSeen;  // name -> when that t was first seen
    bool operator==(const State &o) const
    {
        return lastSynced == o.lastSynced && lastTs == o.lastTs && pending == o.pending
            && futureT == o.futureT && futureSeen == o.futureSeen;
    }
};
QJsonObject stateToJson(const State &s);
State stateFromJson(const QJsonObject &o);

// Mark every Sync field whose local value differs from lastSynced as a local
// change at `now`. A change already pending keeps its (earlier) time. A field
// whose value returned to lastSynced stops being pending. Pure.
State detectLocalChanges(const QVariantMap &local, State state, qint64 now);

// The local values are DEVICE-global (a setter writes the active account's key
// and the shared fallback) while this state belongs to one account, so after an
// account switch or a restart the local view can differ from the baseline
// without the user having edited anything for THIS account. Accepting every
// such difference as the baseline (value only, time 0, so it is never re-pushed)
// leaves only edits noticed live (pending) as local changes.
State absorbLocal(State state, const QVariantMap &local);

struct MergeResult
{
    QVariantMap applyLocal;      // name -> value to write into the local settings
    QStringList lostLocal;       // local unsynced changes that a LATER server value replaced
    Blob toWrite;                // the whole blob to PUT (only meaningful when needsWrite)
    QJsonObject keep;            // unknown server entries to carry through the PUT
    bool needsWrite = false;
    bool readOnly = false;       // server schema is newer: never write
    State afterApply;            // commit immediately
    State afterWrite;            // commit when the PUT is acknowledged
};
MergeResult merge(const ParsedBlob &server, const QVariantMap &local, const State &state, qint64 now);

// First enable on a device that has no baseline.
struct FirstRunPlan
{
    QStringList differing;       // names where server and local disagree
    bool serverEmpty = true;
};
FirstRunPlan planFirstRun(const ParsedBlob &server, const QVariantMap &local);
// Adopt the server: its values win everywhere, baseline = server.
MergeResult adoptServer(const ParsedBlob &server, const QVariantMap &local, qint64 now);
// Upload this device: every Sync field that differs from its default (or from
// the server) is a local change at `now`, so it wins every conflict.
MergeResult uploadLocal(const ParsedBlob &server, const QVariantMap &local, qint64 now);

} // namespace settingsport
