#include "settings/SettingsSyncController.h"

#include "app/CustomThemeStore.h"
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"
#include "matrix/MatrixClient.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

using namespace settingsport;

Q_LOGGING_CATEGORY(lcSettingsSync, "lightning.settingssync")

namespace {

constexpr int kDebounceMs = 2000;
constexpr int kChangedDebounceMs = 1500;
constexpr int kRetryMs = 45 * 1000;

QString reasonText(const QString &code)
{
    // Short, user-facing; the codes come from SettingsPortable::check().
    if (code == QLatin1String("range"))
        return QObject::tr("out of range");
    if (code == QLatin1String("type"))
        return QObject::tr("wrong kind of value");
    if (code == QLatin1String("length"))
        return QObject::tr("too long");
    if (code == QLatin1String("control"))
        return QObject::tr("contains control characters");
    if (code == QLatin1String("not_allowed"))
        return QObject::tr("not a value this version knows");
    if (code == QLatin1String("needs_custom_theme"))
        return QObject::tr("needs a custom theme the file does not contain");
    return QObject::tr("not valid");
}

QString importErrorText(const QString &code)
{
    if (code == QLatin1String("empty"))
        return QObject::tr("That file is empty.");
    if (code == QLatin1String("too_large"))
        return QObject::tr("That file is too large to be a settings file.");
    if (code == QLatin1String("not_json"))
        return QObject::tr("That file is not a settings file (it is not valid JSON).");
    return QObject::tr("That file is not a Lightning settings file.");
}

// Human names for the integer-coded choices the import preview lists, so a
// row reads "Indigo Night -> Moss Light" and not "9 -> 8". Empty means "no
// name known": the caller shows the raw value.
QString enumText(const QString &name, const QVariant &v)
{
    if (!v.isValid() || v.typeId() == QMetaType::Bool)
        return {};
    bool ok = false;
    const int i = v.toInt(&ok);
    if (!ok)
        return {};
    if (name == QLatin1String("theme")) {
        switch (i) {
        case 0: return QObject::tr("Follow the system");
        case 1: return QObject::tr("Lightning Light");
        case 2: return QObject::tr("Lightning Dark");
        case 3: return QObject::tr("Graphite");
        case 4: return QObject::tr("Midnight");
        case 5: return QObject::tr("Nordic");
        case 6: return QObject::tr("Purple Dusk");
        case 7: return QObject::tr("Warm");
        case 8: return QObject::tr("Moss Light");
        case 9: return QObject::tr("Indigo Night");
        case 10: return QObject::tr("Deep Teal");
        case 11: return QObject::tr("Storm");
        case 12: return QObject::tr("Custom theme");
        }
    } else if (name == QLatin1String("messageLayout")) {
        switch (i) {
        case 0: return QObject::tr("Modern");
        case 1: return QObject::tr("Bubbles");
        case 2: return QObject::tr("Compact");
        case 3: return QObject::tr("Small avatars");
        }
    } else if (name == QLatin1String("layoutPreset")) {
        switch (i) {
        case 0: return QObject::tr("Lightning");
        case 1: return QObject::tr("Nheko compact");
        }
    } else if (name == QLatin1String("roomNavigationLayout")) {
        switch (i) {
        case 0: return QObject::tr("Classic");
        case 1: return QObject::tr("Channels");
        }
    } else if (name == QLatin1String("threadStyle")) {
        switch (i) {
        case 0: return QObject::tr("Thread panel");
        case 1: return QObject::tr("Inline threads");
        }
    } else if (name == QLatin1String("threadPlacement")) {
        switch (i) {
        case 0: return QObject::tr("Side panel");
        case 1: return QObject::tr("Main view");
        }
    } else if (name == QLatin1String("clockFormat")) {
        switch (i) {
        case 0: return QObject::tr("Follow the system");
        case 1: return QObject::tr("12-hour");
        case 2: return QObject::tr("24-hour");
        }
    } else if (name == QLatin1String("messageSpacing")) {
        switch (i) {
        case 0: return QObject::tr("Compact");
        case 1: return QObject::tr("Cozy");
        case 2: return QObject::tr("Roomy");
        }
    }
    return {};
}

} // namespace

SettingsSyncController::SettingsSyncController(QObject *parent)
    : QObject(parent)
{
    m_localTimer.setSingleShot(true);
    m_localTimer.setInterval(kDebounceMs);
    connect(&m_localTimer, &QTimer::timeout, this, [this] { checkLocalNow(); });
    m_changedTimer.setSingleShot(true);
    m_changedTimer.setInterval(kChangedDebounceMs);
    // The blob or the switch moved (the switch may have been turned on by
    // another device): ask the server, which is the only authority.
    connect(&m_changedTimer, &QTimer::timeout, this, [this] { requestRead(); });
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(kRetryMs);
    connect(&m_retryTimer, &QTimer::timeout, this, &SettingsSyncController::retry);
}

void SettingsSyncController::setDebounceForTesting(int ms)
{
    m_localTimer.setInterval(ms);
    m_changedTimer.setInterval(ms);
}

qint64 SettingsSyncController::nowMs() const
{
    return m_fixedNow > 0 ? m_fixedNow : QDateTime::currentMSecsSinceEpoch();
}

void SettingsSyncController::setSettings(SettingsManager *settings)
{
    m_settings = settings;
    connectSettings();
}

void SettingsSyncController::setShortcuts(ShortcutRegistry *shortcuts)
{
    m_shortcuts = shortcuts;
}

void SettingsSyncController::setCustomThemes(CustomThemeStore *themes)
{
    m_themes = themes;
}

void SettingsSyncController::connectSettings()
{
    if (!m_settings)
        return;
    const QMetaObject *mo = m_settings->metaObject();
    const int slot = metaObject()->indexOfSlot("onLocalTouched()");
    for (const Field &f : fields()) {
        if (!(f.flags & Sync))
            continue;
        const int p = mo->indexOfProperty(f.name.toUtf8().constData());
        if (p < 0)
            continue;
        const QMetaMethod notify = mo->property(p).notifySignal();
        if (!notify.isValid())
            continue;
        QMetaObject::connect(m_settings, notify.methodIndex(), this, slot);
    }
}

void SettingsSyncController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    resetForAccount();
    if (m_client) {
        connect(m_client, &MatrixClient::syncedSettingsRead, this, &SettingsSyncController::onRead);
        connect(m_client, &MatrixClient::syncedSettingsWritten, this, &SettingsSyncController::onWritten);
        connect(m_client, &MatrixClient::syncedSettingsChanged, this, &SettingsSyncController::onSyncedChanged);
        connect(m_client, &MatrixClient::loggedOut, this, [this] { resetForAccount(); });
        // Account data is in place once the first sync has run.
        connect(m_client, &MatrixClient::initialSyncDoneChanged, this, &SettingsSyncController::startSync);
    }
    Q_EMIT changed();
    startSync();
}

bool SettingsSyncController::supported() const
{
    return m_client && m_client->supportsSettingsSync();
}

QString SettingsSyncController::state() const
{
    if (!supported())
        return QStringLiteral("unsupported");
    if (!m_error.isEmpty())
        return QStringLiteral("error");
    if (m_choiceNeeded)
        return QStringLiteral("choice");
    if (m_readOp != 0 && !m_switchKnown)
        return QStringLiteral("checking");
    if (!m_enabled)
        return QStringLiteral("off");
    if (m_readOnly)
        return QStringLiteral("readOnly");
    if (m_readOp != 0 || m_writeOp != 0 || m_switchOp != 0)
        return QStringLiteral("busy");
    return QStringLiteral("idle");
}

QString SettingsSyncController::stateKey() const
{
    const QString user = m_settings ? m_settings->activeAccountUserId() : QString();
    if (user.isEmpty())
        return {};
    return QString::fromLatin1(QCryptographicHash::hash(user.toUtf8(), QCryptographicHash::Sha256)
                                   .toHex()
                                   .left(16));
}

void SettingsSyncController::resetForAccount()
{
    ++m_gen;
    m_readOp = m_writeOp = m_switchOp = 0;
    m_force = Force::None;
    m_afterSwitch = AfterSwitch::None;
    m_rereadWanted = false;
    m_started = false;
    m_enabled = false;
    m_switchKnown = false;
    m_choiceNeeded = false;
    m_choiceDiffering = 0;
    m_readOnly = false;
    m_error.clear();
    m_conflicts.clear();
    m_state = State();
    m_hasBaseline = false;
    m_lastSyncedMs = 0;
    m_userKey.clear();
    m_localTimer.stop();
    m_changedTimer.stop();
    m_retryTimer.stop();
}

void SettingsSyncController::loadState()
{
    m_state = State();
    m_hasBaseline = false;
    m_lastSyncedMs = 0;
    if (m_userKey.isEmpty())
        return;
    const QString base = QStringLiteral("settingsSync/") + m_userKey + QLatin1Char('/');
    m_state = stateFromJson(
        QJsonDocument::fromJson(m_store.value(base + QStringLiteral("state")).toString().toUtf8()).object());
    m_hasBaseline = m_store.value(base + QStringLiteral("baseline"), false).toBool();
    m_lastSyncedMs = m_store.value(base + QStringLiteral("lastSyncedMs"), 0).toLongLong();
}

void SettingsSyncController::persistState()
{
    if (m_userKey.isEmpty())
        return;
    const QString base = QStringLiteral("settingsSync/") + m_userKey + QLatin1Char('/');
    if (!m_hasBaseline) {
        m_store.remove(QStringLiteral("settingsSync/") + m_userKey);
        return;
    }
    m_store.setValue(base + QStringLiteral("state"),
                     QString::fromUtf8(QJsonDocument(stateToJson(m_state)).toJson(QJsonDocument::Compact)));
    m_store.setValue(base + QStringLiteral("baseline"), true);
    m_store.setValue(base + QStringLiteral("lastSyncedMs"), m_lastSyncedMs);
}

QVariantMap SettingsSyncController::localView() const
{
    return readProperties(m_settings.data(), Sync);
}

// ---- sync ------------------------------------------------------------------

void SettingsSyncController::startSync()
{
    if (!supported() || !m_settings || !m_client->initialSyncDone())
        return;
    const QString key = stateKey();
    if (key.isEmpty())
        return;
    if (key != m_userKey) {
        resetForAccount();
        m_userKey = key;
        loadState();
    }
    if (m_started)
        return;
    m_started = true;
    m_absorbLocal = true;
    requestRead();
}

void SettingsSyncController::requestRead(Force force)
{
    if (!supported())
        return;
    if (force != Force::None)
        m_force = force;
    if (m_readOp != 0) {
        m_rereadWanted = true;
        return;
    }
    m_readGen = m_gen;
    m_readOp = ++m_nextOp;
    Q_EMIT changed();
    m_client->readSyncedSettings(m_readOp);
}

void SettingsSyncController::failWith(const QString &category)
{
    m_error = category.isEmpty() ? QStringLiteral("failed") : category;
    qCWarning(lcSettingsSync) << "settings sync stopped category=" << m_error;
    m_retryTimer.start();
    Q_EMIT changed();
}

void SettingsSyncController::retry()
{
    m_error.clear();
    Q_EMIT changed();
    if (m_readOp == 0 && m_writeOp == 0 && m_switchOp == 0)
        requestRead();
}

void SettingsSyncController::onSyncedChanged()
{
    if (m_applying)
        return;
    m_changedTimer.start();
}

void SettingsSyncController::onLocalTouched()
{
    if (m_applying || !m_settings || stateKey() != m_userKey || !active())
        return;
    m_localTimer.start();
}

void SettingsSyncController::checkLocalNow()
{
    if (!m_settings)
        return;
    if (stateKey() != m_userKey) {
        startSync();
        return;
    }
    if (!active())
        return;
    const State next = detectLocalChanges(localView(), m_state, nowMs());
    if (!(next == m_state)) {
        m_state = next;
        persistState();
        Q_EMIT changed();
    }
    if (m_state.pending.isEmpty())
        return;
    if (m_writeOp != 0 || m_readOp != 0) {
        m_rereadWanted = true; // pushed as soon as the current round trip ends
        return;
    }
    requestRead();
}

void SettingsSyncController::onRead(quint64 opId, bool ok, const QString &category,
                                    const QString &blobState, const QString &blobJson,
                                    int switchState)
{
    if (opId != m_readOp)
        return;
    m_readOp = 0;
    if (m_readGen != m_gen)
        return;
    if (!ok) {
        failWith(category);
        return;
    }
    m_error.clear();
    if (switchState >= 0) {
        m_enabled = switchState == 1;
        m_switchKnown = true;
    }
    if (!m_enabled) {
        // Off (or never on): nothing is read into settings, no baseline kept,
        // so a later enable starts from the question.
        const bool had = m_hasBaseline;
        m_hasBaseline = false;
        m_choiceNeeded = false;
        m_state = State();
        if (had)
            persistState();
        Q_EMIT changed();
        return;
    }

    ParsedBlob server;
    if (blobState == QLatin1String("absent")) {
        server = parseBlob(QJsonObject(), false);
    } else if (blobState == QLatin1String("present")) {
        server = parseBlob(QJsonDocument::fromJson(blobJson.toUtf8()).object(), true);
    } else {
        server.present = true;
        server.readable = false;
    }
    m_readOnly = server.newerVersion || !server.readable;

    const QVariantMap local = localView();
    const qint64 now = nowMs();
    const Force force = m_force;
    m_force = Force::None;
    MergeResult r;

    if (!m_hasBaseline) {
        if (force == Force::Adopt) {
            r = adoptServer(server, local, now);
        } else if (force == Force::Upload) {
            r = uploadLocal(server, local, now);
        } else {
            // A real question only when this device holds a setting of its own
            // choosing that the account holds differently. A fresh install (all
            // defaults) just takes the account's; an empty account just takes
            // this device's.
            const State mine = detectLocalChanges(local, State(), now);
            QStringList clash;
            for (auto it = mine.pending.constBegin(); it != mine.pending.constEnd(); ++it) {
                const auto sv = server.entries.constFind(it.key());
                if (sv != server.entries.constEnd() && sv->v != local.value(it.key()))
                    clash.append(it.key());
            }
            if (!clash.isEmpty()) {
                m_choiceNeeded = true;
                m_choiceDiffering = int(clash.size());
                Q_EMIT changed();
                return;
            }
            r = uploadLocal(server, local, now);
        }
        m_choiceNeeded = false;
        m_choiceDiffering = 0;
    } else {
        if (m_absorbLocal)
            m_state = absorbLocal(m_state, local);
        r = merge(server, local, m_state, now);
    }
    m_absorbLocal = false;
    applyMerge(r, server);
}

void SettingsSyncController::applyMerge(const MergeResult &r, const ParsedBlob &server)
{
    Q_UNUSED(server);
    if (!r.applyLocal.isEmpty()) {
        m_applying = true;
        const QStringList failed = writeProperties(m_settings.data(), r.applyLocal);
        m_applying = false;
        if (!failed.isEmpty())
            qCWarning(lcSettingsSync) << "settings sync could not apply" << failed.size() << "value(s)";
        qCInfo(lcSettingsSync) << "settings sync applied" << r.applyLocal.size() << "value(s) from the account";
    }
    for (const QString &n : r.lostLocal) {
        const QString label = labelFor(n);
        if (!m_conflicts.contains(label))
            m_conflicts.append(label);
    }
    m_state = r.afterApply;

    if (r.needsWrite) {
        const QByteArray json = QJsonDocument(composeBlob(r.toWrite, r.keep)).toJson(QJsonDocument::Compact);
        if (json.size() > kMaxBlobBytes) {
            persistState();
            failWith(QStringLiteral("upload_too_large"));
            return;
        }
        m_afterWrite = r.afterWrite;
        m_writeGen = m_gen;
        m_writeOp = ++m_nextOp;
        persistState();
        Q_EMIT changed();
        qCInfo(lcSettingsSync) << "settings sync writing" << r.toWrite.size() << "key(s)";
        m_client->writeSyncedSettings(0, QString::fromUtf8(json), m_writeOp);
        return;
    }
    m_hasBaseline = true;
    m_lastSyncedMs = nowMs();
    persistState();
    Q_EMIT changed();
    if (m_rereadWanted) {
        m_rereadWanted = false;
        m_localTimer.start();
    }
}

void SettingsSyncController::sendSwitch(bool on, AfterSwitch after)
{
    if (!supported())
        return;
    m_switchTarget = on;
    m_afterSwitch = after;
    m_switchGen = m_gen;
    m_switchOp = ++m_nextOp;
    Q_EMIT changed();
    m_client->writeSyncedSettings(
        1, on ? QStringLiteral("{\"enabled\":true}") : QStringLiteral("{\"enabled\":false}"), m_switchOp);
}

void SettingsSyncController::onWritten(quint64 opId, int kind, bool ok, const QString &category)
{
    if (opId == m_writeOp && kind == 0) {
        m_writeOp = 0;
        if (m_writeGen != m_gen)
            return;
        if (!ok) {
            if (category == QLatin1String("sync_disabled")) {
                // Another device turned sync off while this write was in flight.
                m_enabled = false;
                m_hasBaseline = false;
                m_state = State();
                persistState();
                Q_EMIT changed();
                return;
            }
            failWith(category);
            return;
        }
        m_state = m_afterWrite;
        m_hasBaseline = true;
        m_lastSyncedMs = nowMs();
        m_error.clear();
        persistState();
        Q_EMIT changed();
        // Edits made while the write was in flight.
        m_localTimer.start();
        return;
    }
    if (opId != m_switchOp || (kind != 1 && kind != 2))
        return;
    m_switchOp = 0;
    if (m_switchGen != m_gen)
        return;
    if (!ok) {
        m_afterSwitch = AfterSwitch::None;
        failWith(category);
        return;
    }
    if (kind == 2) {
        m_afterSwitch = AfterSwitch::None;
        m_error.clear();
        Q_EMIT changed();
        return;
    }
    m_enabled = m_switchTarget;
    m_switchKnown = true;
    m_error.clear();
    if (!m_enabled) {
        ++m_gen; // anything still in flight belongs to the sync that just ended
        m_readOp = m_writeOp = 0;
        m_hasBaseline = false;
        m_choiceNeeded = false;
        m_state = State();
        m_conflicts.clear();
        persistState();
        if (m_afterSwitch == AfterSwitch::Clear) {
            m_switchGen = m_gen;
            m_switchOp = ++m_nextOp;
            m_afterSwitch = AfterSwitch::None;
            Q_EMIT changed();
            m_client->writeSyncedSettings(2, QStringLiteral("{}"), m_switchOp);
            return;
        }
        m_afterSwitch = AfterSwitch::None;
        Q_EMIT changed();
        return;
    }
    m_afterSwitch = AfterSwitch::None;
    m_hasBaseline = false;
    m_state = State();
    Q_EMIT changed();
    requestRead();
}

void SettingsSyncController::setSyncEnabled(bool on)
{
    if (!supported() || m_switchOp != 0)
        return;
    if (on == m_enabled)
        return;
    sendSwitch(on, AfterSwitch::Enable);
}

void SettingsSyncController::removeFromServer()
{
    if (!supported() || m_switchOp != 0)
        return;
    sendSwitch(false, AfterSwitch::Clear);
}

void SettingsSyncController::chooseAdoptServer()
{
    if (!m_choiceNeeded)
        return;
    m_choiceNeeded = false;
    requestRead(Force::Adopt);
}

void SettingsSyncController::chooseUploadMine()
{
    if (!m_choiceNeeded)
        return;
    m_choiceNeeded = false;
    requestRead(Force::Upload);
}

void SettingsSyncController::acknowledgeConflicts()
{
    if (m_conflicts.isEmpty())
        return;
    m_conflicts.clear();
    Q_EMIT changed();
}

QString SettingsSyncController::labelFor(const QString &name) const
{
    QString out;
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (i > 0 && c.isUpper() && !name.at(i - 1).isUpper())
            out += QLatin1Char(' ');
        out += (i == 0) ? c.toUpper() : (c.isUpper() ? c.toLower() : c);
    }
    out.replace(QStringLiteral("Gif"), QStringLiteral("GIF"));
    out.replace(QStringLiteral(" gif"), QStringLiteral(" GIF"));
    out.replace(QStringLiteral(" ui "), QStringLiteral(" UI "));
    out.replace(QStringLiteral(" mb"), QStringLiteral(" MB"));
    out.replace(QStringLiteral(" fps"), QStringLiteral(" FPS"));
    return out;
}

// ---- export / import ---------------------------------------------------------

QString SettingsSyncController::exportSettings(const QUrl &file)
{
    if (!m_settings)
        return tr("Settings are not available.");
    const QString path = file.toLocalFile();
    if (path.isEmpty())
        return tr("Choose a file to save to.");

    Document doc;
    doc.settings = readProperties(m_settings.data(), Export);
    doc.app = QCoreApplication::applicationVersion();
    if (m_shortcuts) {
        for (int row = 0; row < m_shortcuts->rowCount() && doc.shortcuts.size() < kMaxShortcuts; ++row) {
            const QModelIndex idx = m_shortcuts->index(row);
            const QString id = m_shortcuts->data(idx, ShortcutRegistry::IdRole).toString();
            const QString cur = m_shortcuts->sequenceFor(id);
            if (!id.isEmpty() && cur != m_shortcuts->defaultSequenceFor(id))
                doc.shortcuts.insert(id, cur);
        }
    }
    if (m_themes) {
        const QString active = m_themes->activeThemeId();
        const QVariantList themes = m_themes->themes();
        for (const QVariant &t : themes) {
            const QString id = t.toMap().value(QStringLiteral("id")).toString();
            const QString payload = m_themes->exportTheme(id);
            if (payload.isEmpty())
                continue;
            if (id == active)
                doc.activeTheme = int(doc.themes.size());
            doc.themes.append(payload);
        }
    }
    const QByteArray bytes =
        buildDocument(doc, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return tr("Could not write that file.");
    // The file holds preferences only, but there is no reason for it to be
    // world-readable.
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (out.write(bytes) != bytes.size() || !out.commit())
        return tr("Could not write that file.");
    return {};
}

QVariantMap SettingsSyncController::previewImport(const QUrl &file)
{
    QVariantMap out;
    m_haveImport = false;
    out.insert(QStringLiteral("ok"), false);
    if (!m_settings) {
        out.insert(QStringLiteral("error"), tr("Settings are not available."));
        return out;
    }
    QFile in(file.toLocalFile());
    if (!in.open(QIODevice::ReadOnly)) {
        out.insert(QStringLiteral("error"), tr("Could not read that file."));
        return out;
    }
    const QByteArray bytes = in.read(kMaxFileBytes + 1);
    ParseResult parsed = parseDocument(bytes);
    if (!parsed.ok) {
        out.insert(QStringLiteral("error"), importErrorText(parsed.error));
        return out;
    }

    // A Custom theme choice needs a custom theme to point at.
    if (parsed.doc.settings.contains(QStringLiteral("theme"))
        && parsed.doc.settings.value(QStringLiteral("theme")).toInt() == SettingsManager::CustomTheme
        && parsed.doc.themes.isEmpty() && (!m_themes || m_themes->themes().isEmpty())) {
        parsed.doc.settings.remove(QStringLiteral("theme"));
        parsed.rejected.insert(QStringLiteral("theme"), QStringLiteral("needs_custom_theme"));
    }

    const QVariantMap current = readProperties(m_settings.data(), Export);
    QVariantList changes;
    for (const Change &c : diff(current, parsed.doc.settings)) {
        QVariantMap row;
        row.insert(QStringLiteral("name"), c.name);
        row.insert(QStringLiteral("label"), labelFor(c.name));
        row.insert(QStringLiteral("group"), c.group);
        row.insert(QStringLiteral("from"), c.from);
        row.insert(QStringLiteral("to"), c.to);
        row.insert(QStringLiteral("fromText"), enumText(c.name, c.from));
        row.insert(QStringLiteral("toText"), enumText(c.name, c.to));
        changes.append(row);
    }
    QVariantList rejected;
    for (auto it = parsed.rejected.constBegin(); it != parsed.rejected.constEnd(); ++it) {
        QVariantMap row;
        row.insert(QStringLiteral("label"), labelFor(it.key()));
        row.insert(QStringLiteral("reason"), reasonText(it.value()));
        rejected.append(row);
    }
    int shortcutChanges = 0;
    if (m_shortcuts) {
        // An action this version does not have cannot change anything, so it
        // is not counted (applying it is skipped the same way).
        QSet<QString> known;
        for (int row = 0; row < m_shortcuts->rowCount(); ++row)
            known.insert(m_shortcuts->data(m_shortcuts->index(row), ShortcutRegistry::IdRole).toString());
        for (auto it = parsed.doc.shortcuts.constBegin(); it != parsed.doc.shortcuts.constEnd(); ++it) {
            if (known.contains(it.key()) && m_shortcuts->sequenceFor(it.key()) != it.value())
                ++shortcutChanges;
        }
    }
    int themesNew = 0;
    if (m_themes) {
        QStringList have;
        for (const QVariant &t : m_themes->themes())
            have.append(m_themes->exportTheme(t.toMap().value(QStringLiteral("id")).toString()));
        for (const QString &payload : parsed.doc.themes) {
            if (!have.contains(payload))
                ++themesNew;
        }
    }

    m_import = parsed;
    m_haveImport = true;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("version"), parsed.version);
    out.insert(QStringLiteral("newerVersion"), parsed.newerVersion);
    out.insert(QStringLiteral("changes"), changes);
    out.insert(QStringLiteral("rejected"), rejected);
    out.insert(QStringLiteral("unknownCount"), int(parsed.unknown.size()));
    out.insert(QStringLiteral("shortcutChanges"), shortcutChanges);
    out.insert(QStringLiteral("shortcutsRejected"), int(parsed.shortcutsRejected.size()));
    out.insert(QStringLiteral("themesNew"), themesNew);
    return out;
}

void SettingsSyncController::cancelImport()
{
    m_haveImport = false;
    m_import = ParseResult();
}

QVariantMap SettingsSyncController::applyImport()
{
    QVariantMap out;
    out.insert(QStringLiteral("ok"), false);
    if (!m_haveImport || !m_settings) {
        out.insert(QStringLiteral("error"), tr("Choose a file first."));
        return out;
    }
    const ParseResult parsed = m_import;
    m_haveImport = false;

    // Everything in `parsed` has already passed check(). Applied in one go; a
    // value the setter then refuses puts every earlier one back.
    const QVariantMap before = readProperties(m_settings.data(), Export);

    int themesAdded = 0;
    QStringList addedIds;
    QStringList idForIndex;
    if (m_themes && !parsed.doc.themes.isEmpty()) {
        QHash<QString, QString> idByPayload;
        for (const QVariant &t : m_themes->themes()) {
            const QString id = t.toMap().value(QStringLiteral("id")).toString();
            idByPayload.insert(m_themes->exportTheme(id), id);
        }
        for (const QString &payload : parsed.doc.themes) {
            if (idByPayload.contains(payload)) {
                idForIndex.append(idByPayload.value(payload));
                continue;
            }
            const QString reason = m_themes->importTheme(payload);
            if (!reason.isEmpty()) {
                idForIndex.append(QString());
                continue;
            }
            const QVariantList all = m_themes->themes();
            const QString id = all.isEmpty() ? QString() : all.last().toMap().value(QStringLiteral("id")).toString();
            idForIndex.append(id);
            addedIds.append(id);
            ++themesAdded;
        }
    }

    const int changedCount = int(diff(before, parsed.doc.settings).size());

    m_applying = true;
    const QStringList failed = writeProperties(m_settings.data(), parsed.doc.settings);
    if (!failed.isEmpty()) {
        writeProperties(m_settings.data(), before);
        for (const QString &id : addedIds)
            m_themes->deleteTheme(id);
        m_applying = false;
        out.insert(QStringLiteral("error"),
                   tr("Could not apply %1. Nothing was changed.").arg(labelFor(failed.first())));
        return out;
    }
    m_applying = false;

    if (m_themes && parsed.doc.activeTheme >= 0 && parsed.doc.activeTheme < idForIndex.size()
        && !idForIndex.at(parsed.doc.activeTheme).isEmpty()
        && parsed.doc.settings.value(QStringLiteral("theme")).toInt() == SettingsManager::CustomTheme)
        m_themes->setActiveThemeId(idForIndex.at(parsed.doc.activeTheme));

    int shortcutsSkipped = 0;
    if (m_shortcuts) {
        for (auto it = parsed.doc.shortcuts.constBegin(); it != parsed.doc.shortcuts.constEnd(); ++it) {
            if (m_shortcuts->sequenceFor(it.key()) == it.value())
                continue;
            if (!m_shortcuts->setBinding(it.key(), it.value()).isEmpty())
                ++shortcutsSkipped;
        }
    }

    out.insert(QStringLiteral("ok"), true);
    // The settings that actually changed, which is what the preview counted.
    // The file carries every exported setting, most of them already at the
    // value they hold here, so its size would report 73 for "1 change".
    out.insert(QStringLiteral("applied"), changedCount);
    out.insert(QStringLiteral("shortcutsSkipped"), shortcutsSkipped);
    out.insert(QStringLiteral("themesAdded"), themesAdded);
    // The edits are picked up by sync like any other.
    onLocalTouched();
    return out;
}
