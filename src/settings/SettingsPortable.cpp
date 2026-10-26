#include "settings/SettingsPortable.h"

#include "app/SettingsManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMetaType>
#include <QRegularExpression>

#include <cmath>

namespace settingsport {

namespace {
// A JSON number as milliseconds, or -1 when it is not a sane one. A bare
// static_cast of a double beyond the int64 range is undefined behaviour.
qint64 safeMs(const QJsonValue &v)
{
    if (!v.isDouble())
        return -1;
    const double d = v.toDouble();
    if (!std::isfinite(d) || d < 0 || d > 9.0e15)
        return -1;
    return static_cast<qint64>(d);
}
} // namespace

// Why each setting is where it is (the rule, so the next field has a place):
//
//  Export + Sync   a preference about how the app looks or behaves that means
//                  the same on every machine.
//  Export only     meaningful on THIS machine (window state, widths, zoom,
//                  volumes, capture size, hardware-dependent switches) or a
//                  setting that WIDENS what leaves the app or what is shown
//                  outside its protection (notification previews, link
//                  previews in encrypted rooms, strict device trust, keeping
//                  media): the homeserver can write account data, so it must
//                  not be able to loosen those on a client.
//  Neither         everything else: tokens, keys, recovery material, device
//                  and hardware ids, paths, account ids, geometry, dismissals.
//
// The table is generated from one spec so the flags stay a single column.
const QList<Field> &fields()
{
    static const QList<Field> table = {
#include "settings/SettingsSchemaTable.inc"
    };
    return table;
}

const Field *find(const QString &name)
{
    static const QHash<QString, int> index = [] {
        QHash<QString, int> h;
        const QList<Field> &all = fields();
        for (int i = 0; i < all.size(); ++i)
            h.insert(all.at(i).name, i);
        return h;
    }();
    const auto it = index.constFind(name);
    return it == index.constEnd() ? nullptr : &fields().at(it.value());
}

QStringList names(int flag)
{
    QStringList out;
    for (const Field &f : fields()) {
        if (f.flags & flag)
            out.append(f.name);
    }
    return out;
}

namespace {

bool hasControl(const QString &s)
{
    for (const QChar c : s) {
        if (c.isNull() || c.category() == QChar::Other_Control)
            return true;
    }
    return false;
}

Checked fail(const QString &reason)
{
    Checked c;
    c.reason = reason;
    return c;
}

Checked pass(const QVariant &v)
{
    Checked c;
    c.ok = true;
    c.value = v;
    return c;
}

bool isEnum(const QVariant &v)
{
    return v.metaType().flags().testFlag(QMetaType::IsEnumeration);
}

Checked checkString(const Field &f, const QString &s)
{
    if (s.size() > f.maxLen)
        return fail(QStringLiteral("length"));
    if (hasControl(s))
        return fail(QStringLiteral("control"));
    if (!f.allowed.isEmpty() && !f.allowed.contains(s))
        return fail(QStringLiteral("not_allowed"));
    return pass(s);
}

} // namespace

Checked check(const Field &f, const QVariant &raw)
{
    switch (f.kind) {
    case Kind::Bool:
        if (raw.metaType().id() != QMetaType::Bool)
            return fail(QStringLiteral("type"));
        return pass(raw.toBool());
    case Kind::Int: {
        qint64 n = 0;
        if (isEnum(raw)) {
            n = raw.toLongLong();
        } else {
            switch (raw.metaType().id()) {
            case QMetaType::Int:
            case QMetaType::UInt:
            case QMetaType::LongLong:
            case QMetaType::ULongLong:
                n = raw.toLongLong();
                break;
            case QMetaType::Double: {
                const double d = raw.toDouble();
                if (!std::isfinite(d) || std::floor(d) != d || std::fabs(d) > 9.0e15)
                    return fail(QStringLiteral("type"));
                n = static_cast<qint64>(d);
                break;
            }
            default:
                return fail(QStringLiteral("type"));
            }
        }
        if (n < f.lo || n > f.hi)
            return fail(QStringLiteral("range"));
        return pass(static_cast<int>(n));
    }
    case Kind::String:
        if (raw.metaType().id() != QMetaType::QString)
            return fail(QStringLiteral("type"));
        return checkString(f, raw.toString());
    case Kind::StringList: {
        QStringList items;
        if (raw.metaType().id() == QMetaType::QStringList) {
            items = raw.toStringList();
        } else if (raw.metaType().id() == QMetaType::QVariantList) {
            for (const QVariant &v : raw.toList()) {
                if (v.metaType().id() != QMetaType::QString)
                    return fail(QStringLiteral("type"));
                items.append(v.toString());
            }
        } else {
            return fail(QStringLiteral("type"));
        }
        if (items.size() > 64)
            return fail(QStringLiteral("length"));
        for (const QString &item : items) {
            const Checked one = checkString(f, item);
            if (!one.ok)
                return one;
        }
        // Canonical: sorted and unique, as the setter stores it.
        items.sort();
        items.removeDuplicates();
        return pass(items);
    }
    }
    return fail(QStringLiteral("type"));
}

QVariantMap readProperties(const QObject *object, int flag)
{
    QVariantMap out;
    if (!object)
        return out;
    for (const Field &f : fields()) {
        if (!(f.flags & flag))
            continue;
        const QVariant raw = object->property(f.name.toUtf8().constData());
        if (!raw.isValid())
            continue;
        const Checked c = check(f, raw);
        if (c.ok)
            out.insert(f.name, c.value);
    }
    return out;
}

QStringList writeProperties(QObject *object, const QVariantMap &values)
{
    QStringList failed;
    if (!object)
        return values.keys();
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const Field *f = find(it.key());
        if (!f || !check(*f, it.value()).ok
            || !object->setProperty(f->name.toUtf8().constData(), check(*f, it.value()).value))
            failed.append(it.key());
    }
    return failed;
}

QStringList presentationKeys()
{
    return { QStringLiteral("messageLayout"), QStringLiteral("messageSpacing"),
             QStringLiteral("threadPlacement") };
}

QVariantMap readSettings(const SettingsManager *settings, int flag)
{
    QVariantMap out = readProperties(settings, flag);
    if (!settings)
        return out;
    for (const QString &name : presentationKeys()) {
        if (!out.contains(name))
            continue;   // not in this flag's set
        const Field *f = find(name);
        const QVariant lightning = settings->lightningPresentationValue(name);
        const Checked c = f ? check(*f, lightning) : Checked{};
        if (c.ok)
            out.insert(name, c.value);
    }
    return out;
}

QStringList writeSettings(SettingsManager *settings, const QVariantMap &values)
{
    if (!settings)
        return values.keys();
    QVariantMap rest = values;
    QStringList failed;
    for (const QString &name : presentationKeys()) {
        const auto it = rest.constFind(name);
        if (it == rest.constEnd())
            continue;
        const Field *f = find(name);
        if (!f || !check(*f, it.value()).ok
            || !settings->setLightningPresentationValue(
                   name, check(*f, it.value()).value))
            failed.append(name);
        rest.remove(name);
    }
    failed += writeProperties(settings, rest);
    return failed;
}

// ---- settings file ---------------------------------------------------------

QByteArray buildDocument(const Document &doc, const QString &exportedAtIso)
{
    QJsonObject root;
    root.insert(QStringLiteral("format"), QString::fromLatin1(kFileFormat));
    root.insert(QStringLiteral("version"), kFileVersion);
    root.insert(QStringLiteral("app"), doc.app);
    root.insert(QStringLiteral("exportedAt"), exportedAtIso);

    QJsonObject settings;
    for (auto it = doc.settings.constBegin(); it != doc.settings.constEnd(); ++it) {
        const Field *f = find(it.key());
        // The allow-list is enforced HERE as well as on read: a value the
        // schema does not export is dropped, whoever handed it over.
        if (!f || !(f->flags & Export))
            continue;
        const Checked c = check(*f, it.value());
        if (!c.ok)
            continue;
        settings.insert(f->name, QJsonValue::fromVariant(c.value));
    }
    root.insert(QStringLiteral("settings"), settings);

    QJsonObject shortcuts;
    for (auto it = doc.shortcuts.constBegin(); it != doc.shortcuts.constEnd(); ++it)
        shortcuts.insert(it.key(), it.value());
    root.insert(QStringLiteral("shortcuts"), shortcuts);

    QJsonArray themes;
    for (const QString &payload : doc.themes) {
        const QJsonDocument parsed = QJsonDocument::fromJson(payload.toUtf8());
        if (parsed.isObject())
            themes.append(parsed.object());
    }
    root.insert(QStringLiteral("customThemes"), themes);
    if (doc.activeTheme >= 0 && doc.activeTheme < themes.size())
        root.insert(QStringLiteral("activeCustomTheme"), doc.activeTheme);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

ParseResult parseDocument(const QByteArray &bytes)
{
    ParseResult r;
    if (bytes.trimmed().isEmpty()) {
        r.error = QStringLiteral("empty");
        return r;
    }
    if (bytes.size() > kMaxFileBytes) {
        r.error = QStringLiteral("too_large");
        return r;
    }
    QJsonParseError err;
    const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !parsed.isObject()) {
        r.error = QStringLiteral("not_json");
        return r;
    }
    const QJsonObject root = parsed.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String(kFileFormat)
        || !root.value(QStringLiteral("version")).isDouble()) {
        r.error = QStringLiteral("not_settings");
        return r;
    }
    r.version = root.value(QStringLiteral("version")).toInt(0);
    if (r.version < 1) {
        r.error = QStringLiteral("not_settings");
        return r;
    }
    r.newerVersion = r.version > kFileVersion;
    r.doc.app = root.value(QStringLiteral("app")).toString().left(64);

    const QJsonObject settings = root.value(QStringLiteral("settings")).toObject();
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
        const Field *f = find(it.key());
        if (!f || !(f->flags & Export)) {
            r.unknown.append(it.key().left(64));
            continue;
        }
        const Checked c = check(*f, it.value().toVariant());
        if (c.ok)
            r.doc.settings.insert(f->name, c.value);
        else
            r.rejected.insert(f->name, c.reason);
    }
    // A version 1 file comes from a build where gifAutoplay "Never" (2) also
    // refused the speculative video/audio download; from version 2 that is
    // videoPrefetchEnabled alone. An old "Never" file imports with preloading
    // off, which is what it meant (SettingsManager::migratePrefetchFromAnimation
    // does the same for a settings file on disk).
    if (r.version < 2) {
        const QVariant mode = r.doc.settings.value(QStringLiteral("gifAutoplay"));
        const QVariant legacy = r.doc.settings.value(QStringLiteral("animateGifPreviews"));
        const bool never = mode.isValid() ? mode.toInt() == 2
                                          : (legacy.isValid() && !legacy.toBool());
        if (never)
            r.doc.settings.insert(QStringLiteral("videoPrefetchEnabled"), false);
    }

    static const QRegularExpression idPattern(
        QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
    const QJsonObject shortcuts = root.value(QStringLiteral("shortcuts")).toObject();
    for (auto it = shortcuts.constBegin(); it != shortcuts.constEnd(); ++it) {
        const QString seq = it.value().toString();
        if (r.doc.shortcuts.size() >= kMaxShortcuts || !idPattern.match(it.key()).hasMatch()
            || !it.value().isString() || seq.size() > 64 || hasControl(seq)) {
            r.shortcutsRejected.append(it.key().left(64));
            continue;
        }
        r.doc.shortcuts.insert(it.key(), seq);
    }

    const QJsonArray themes = root.value(QStringLiteral("customThemes")).toArray();
    for (const QJsonValue &t : themes) {
        if (r.doc.themes.size() >= kMaxThemes || !t.isObject()) {
            ++r.themesDropped;
            continue;
        }
        const QByteArray compact = QJsonDocument(t.toObject()).toJson(QJsonDocument::Compact);
        if (compact.size() > 64 * 1024) {
            ++r.themesDropped;
            continue;
        }
        r.doc.themes.append(QString::fromUtf8(compact));
    }
    const QJsonValue active = root.value(QStringLiteral("activeCustomTheme"));
    if (active.isDouble() && active.toInt(-1) >= 0 && active.toInt(-1) < r.doc.themes.size())
        r.doc.activeTheme = active.toInt();

    r.ok = true;
    return r;
}

QList<Change> diff(const QVariantMap &current, const QVariantMap &incoming)
{
    QList<Change> out;
    for (const Field &f : fields()) {
        const auto in = incoming.constFind(f.name);
        if (in == incoming.constEnd())
            continue;
        const QVariant now = current.value(f.name);
        if (now.isValid() && now == in.value())
            continue;
        out.append({ f.name, f.group, now, in.value() });
    }
    return out;
}

// ---- sync ------------------------------------------------------------------

ParsedBlob parseBlob(const QJsonObject &content, bool present)
{
    ParsedBlob p;
    p.present = present;
    if (!present)
        return p;
    if (content.isEmpty())
        return p; // `{}`: cleared
    const QJsonValue version = content.value(QStringLiteral("version"));
    if (!version.isDouble() || !content.value(QStringLiteral("settings")).isObject()) {
        p.readable = false;
        return p;
    }
    p.version = version.toInt(0);
    p.newerVersion = p.version > kSyncVersion;
    const QJsonObject settings = content.value(QStringLiteral("settings")).toObject();
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
        const Field *f = find(it.key());
        if (!f) {
            // Written by a build that knows more than this one: carried
            // through every write, never interpreted.
            p.keep.insert(it.key(), it.value());
            continue;
        }
        // Known but not usable HERE (not a synced setting in this build, or a
        // value this build cannot hold): reported, ignored, and carried
        // through every write verbatim, so an older build never undoes what
        // a newer one stored.
        if (!(f->flags & Sync) || !it.value().isObject()) {
            p.rejected.append(it.key());
            p.keep.insert(it.key(), it.value());
            continue;
        }
        const QJsonObject entry = it.value().toObject();
        const QJsonValue t = entry.value(QStringLiteral("t"));
        const Checked c = check(*f, entry.value(QStringLiteral("v")).toVariant());
        const qint64 tms = safeMs(t);
        if (!c.ok || tms < 0) {
            p.rejected.append(it.key());
            p.keep.insert(it.key(), it.value());
            continue;
        }
        p.entries.insert(f->name, { c.value, tms });
    }
    return p;
}

QJsonObject composeBlob(const Blob &entries, const QJsonObject &keep)
{
    QJsonObject settings = keep;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        QJsonObject e;
        e.insert(QStringLiteral("v"), QJsonValue::fromVariant(it.value().v));
        e.insert(QStringLiteral("t"), static_cast<double>(it.value().t));
        settings.insert(it.key(), e);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kSyncVersion);
    root.insert(QStringLiteral("settings"), settings);
    return root;
}

QJsonObject stateToJson(const State &s)
{
    QJsonObject synced, ts, pending;
    for (auto it = s.lastSynced.constBegin(); it != s.lastSynced.constEnd(); ++it)
        synced.insert(it.key(), QJsonValue::fromVariant(it.value()));
    for (auto it = s.lastTs.constBegin(); it != s.lastTs.constEnd(); ++it)
        ts.insert(it.key(), static_cast<double>(it.value()));
    for (auto it = s.pending.constBegin(); it != s.pending.constEnd(); ++it)
        pending.insert(it.key(), static_cast<double>(it.value()));
    QJsonObject future;
    for (auto it = s.futureT.constBegin(); it != s.futureT.constEnd(); ++it) {
        QJsonObject e;
        e.insert(QStringLiteral("t"), static_cast<double>(it.value()));
        e.insert(QStringLiteral("seen"), static_cast<double>(s.futureSeen.value(it.key())));
        future.insert(it.key(), e);
    }
    QJsonObject o;
    o.insert(QStringLiteral("future"), future);
    o.insert(QStringLiteral("synced"), synced);
    o.insert(QStringLiteral("ts"), ts);
    o.insert(QStringLiteral("pending"), pending);
    return o;
}

State stateFromJson(const QJsonObject &o)
{
    State s;
    const QJsonObject synced = o.value(QStringLiteral("synced")).toObject();
    for (auto it = synced.constBegin(); it != synced.constEnd(); ++it) {
        const Field *f = find(it.key());
        if (!f || !(f->flags & Sync))
            continue;
        const Checked c = check(*f, it.value().toVariant());
        if (c.ok)
            s.lastSynced.insert(f->name, c.value);
    }
    const QJsonObject ts = o.value(QStringLiteral("ts")).toObject();
    for (auto it = ts.constBegin(); it != ts.constEnd(); ++it) {
        if (s.lastSynced.contains(it.key()))
            s.lastTs.insert(it.key(), qMax<qint64>(0, safeMs(it.value())));
    }
    const QJsonObject pending = o.value(QStringLiteral("pending")).toObject();
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        const Field *f = find(it.key());
        if (f && (f->flags & Sync))
            s.pending.insert(it.key(), qMax<qint64>(0, safeMs(it.value())));
    }
    const QJsonObject future = o.value(QStringLiteral("future")).toObject();
    for (auto it = future.constBegin(); it != future.constEnd(); ++it) {
        const QJsonObject e = it.value().toObject();
        const qint64 t = safeMs(e.value(QStringLiteral("t")));
        const qint64 seen = safeMs(e.value(QStringLiteral("seen")));
        if (find(it.key()) && t >= 0 && seen >= 0) {
            s.futureT.insert(it.key(), t);
            s.futureSeen.insert(it.key(), seen);
        }
    }
    return s;
}

State detectLocalChanges(const QVariantMap &local, State state, qint64 now)
{
    for (const Field &f : fields()) {
        if (!(f.flags & Sync))
            continue;
        const auto value = local.constFind(f.name);
        if (value == local.constEnd())
            continue;
        const bool synced = state.lastSynced.contains(f.name);
        if (synced && state.lastSynced.value(f.name) == value.value()) {
            state.pending.remove(f.name); // changed back: nothing to say
            continue;
        }
        if (!synced) {
            // Never agreed with the server: a local change only when it
            // differs from the default (an untouched setting is not a choice).
            if (f.def.isValid() && f.def == value.value()) {
                state.pending.remove(f.name);
                continue;
            }
        }
        if (!state.pending.contains(f.name))
            state.pending.insert(f.name, now);
    }
    return state;
}

State absorbLocal(State state, const QVariantMap &local)
{
    for (const Field &f : fields()) {
        if (!(f.flags & Sync) || state.pending.contains(f.name))
            continue;
        const auto value = local.constFind(f.name);
        if (value == local.constEnd())
            continue;
        if (!state.lastSynced.contains(f.name) || state.lastSynced.value(f.name) != value.value()) {
            state.lastSynced.insert(f.name, value.value());
            state.lastTs.insert(f.name, 0);
        }
    }
    return state;
}

namespace {

// A server timestamp more than kMaxSkewMs ahead is taken as the moment it was
// first seen (remembered in `st`), not as `now`: `now` moves forward with every
// merge and would let a fast clock outrank every later edit for ever.
qint64 clampTs(const QString &name, qint64 t, qint64 now, State &st)
{
    if (t <= now + kMaxSkewMs) {
        st.futureT.remove(name);
        st.futureSeen.remove(name);
        return t;
    }
    if (st.futureT.value(name, -1) == t)
        return qMin(st.futureSeen.value(name, now), now);
    st.futureT.insert(name, t);
    st.futureSeen.insert(name, now);
    return now;
}

} // namespace

MergeResult merge(const ParsedBlob &server, const QVariantMap &local, const State &state, qint64 now)
{
    MergeResult r;
    r.readOnly = server.newerVersion || !server.readable;
    r.keep = server.keep;
    State st = detectLocalChanges(local, state, now);
    State applied = st;      // after applying server values, before any PUT
    State written = st;      // plus what the PUT carries
    Blob out = server.entries;
    bool write = false;

    for (const Field &f : fields()) {
        if (!(f.flags & Sync))
            continue;
        const QString &n = f.name;
        const QVariant localValue = local.value(n);
        const auto sv = server.entries.constFind(n);
        const bool pending = st.pending.contains(n);

        if (sv == server.entries.constEnd()) {
            if (pending) {
                const qint64 t = st.pending.value(n);
                out.insert(n, { localValue, t });
                written.lastSynced.insert(n, localValue);
                written.lastTs.insert(n, t);
                written.pending.remove(n);
                write = true;
            } else if (st.lastSynced.contains(n) && st.lastTs.value(n) > 0 && localValue.isValid()) {
                // We had agreed this key with the server and it is gone (a
                // concurrent writer replaced the blob): put it back.
                out.insert(n, { localValue, st.lastTs.value(n) });
                write = true;
            }
            continue;
        }

        const qint64 st_t = clampTs(n, sv->t, now, st);
        if (st_t != sv->t)
            out[n].t = st_t; // the hostile time is not written back
        const QVariant serverValue = sv->v;

        if (pending && localValue.isValid()) {
            const qint64 mine = st.pending.value(n);
            if (serverValue == localValue) {
                // Same value: nothing to say, just agree on the time.
                applied.lastSynced.insert(n, localValue);
                applied.lastTs.insert(n, st_t);
                applied.pending.remove(n);
                written.lastSynced.insert(n, localValue);
                written.lastTs.insert(n, st_t);
                written.pending.remove(n);
            } else if (mine >= st_t) {
                // Local change is the later one: it wins and is pushed.
                out.insert(n, { localValue, mine });
                written.lastSynced.insert(n, localValue);
                written.lastTs.insert(n, mine);
                written.pending.remove(n);
                write = true;
            } else {
                // A later change on another device replaced an unsynced one
                // here. Reported, never silent.
                r.applyLocal.insert(n, serverValue);
                r.lostLocal.append(n);
                applied.lastSynced.insert(n, serverValue);
                applied.lastTs.insert(n, st_t);
                applied.pending.remove(n);
                written.lastSynced.insert(n, serverValue);
                written.lastTs.insert(n, st_t);
                written.pending.remove(n);
            }
            continue;
        }

        // Not pending: the server's value is what this device should show.
        if (serverValue != localValue)
            r.applyLocal.insert(n, serverValue);
        applied.lastSynced.insert(n, serverValue);
        applied.lastTs.insert(n, st_t);
        written.lastSynced.insert(n, serverValue);
        written.lastTs.insert(n, st_t);
        // A server value OLDER than what this device last synced means a
        // concurrent writer clobbered the blob: re-assert ours.
        if (st.lastSynced.contains(n) && st.lastTs.value(n) > sv->t && st.lastSynced.value(n) != serverValue
            && localValue.isValid() && localValue == st.lastSynced.value(n)) {
            r.applyLocal.remove(n);
            applied.lastSynced.insert(n, localValue);
            applied.lastTs.insert(n, st.lastTs.value(n));
            written.lastSynced.insert(n, localValue);
            written.lastTs.insert(n, st.lastTs.value(n));
            out.insert(n, { localValue, st.lastTs.value(n) });
            write = true;
        }
    }

    applied.futureT = written.futureT = st.futureT;
    applied.futureSeen = written.futureSeen = st.futureSeen;
    r.toWrite = out;
    r.needsWrite = write && !r.readOnly;
    r.afterApply = applied;
    r.afterWrite = r.needsWrite ? written : applied;
    return r;
}

FirstRunPlan planFirstRun(const ParsedBlob &server, const QVariantMap &local)
{
    FirstRunPlan p;
    p.serverEmpty = server.entries.isEmpty();
    for (auto it = server.entries.constBegin(); it != server.entries.constEnd(); ++it) {
        if (local.contains(it.key()) && local.value(it.key()) != it.value().v)
            p.differing.append(it.key());
    }
    return p;
}

MergeResult adoptServer(const ParsedBlob &server, const QVariantMap &local, qint64 now)
{
    // Baseline = what this device holds now, nothing pending: merge then sees
    // every differing server key as "server value, not pending" and applies
    // it, and uploads nothing.
    State st;
    for (const Field &f : fields()) {
        if ((f.flags & Sync) && local.contains(f.name)) {
            st.lastSynced.insert(f.name, local.value(f.name));
            st.lastTs.insert(f.name, 0);
        }
    }
    return merge(server, local, st, now);
}

MergeResult uploadLocal(const ParsedBlob &server, const QVariantMap &local, qint64 now)
{
    State st = detectLocalChanges(local, State(), now);
    // Keys the server holds with a value equal to ours need no write.
    return merge(server, local, st, now + 1);
}

} // namespace settingsport
