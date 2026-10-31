#include "app/AboutController.h"

#include "matrix/MatrixClient.h"
#include "models/MessageSearchController.h"

#include <QGuiApplication>
#include <QLocale>
#include <QClipboard>
#include <QSysInfo>

namespace {

QString shortCommit()
{
    const QString sha = QStringLiteral(LIGHTNING_SOURCE_SHA).trimmed();
    return sha.isEmpty() ? QStringLiteral("unknown") : sha.left(12);
}

QString categoryText(const QString &category)
{
    if (category == QLatin1String("network"))
        return AboutController::tr("The homeserver could not be reached.");
    if (category == QLatin1String("forbidden"))
        return AboutController::tr("The homeserver refused the request.");
    if (category == QLatin1String("too_large"))
        return AboutController::tr("The answer was too large to show.");
    return AboutController::tr("The homeserver did not give a usable answer.");
}

} // namespace

AboutController::AboutController(QObject *parent)
    : QObject(parent)
{
    // A reset step that never answers must not leave the button spinning.
    m_resetWatchdog.setSingleShot(true);
    m_resetWatchdog.setInterval(kResetWaitMs);
    connect(&m_resetWatchdog, &QTimer::timeout, this,
            [this] { finishReset(true); });
    // Compaction is reported after the clear and has its own, longer wait. It
    // never feeds the reset watchdog above.
    m_compactionWatchdog.setSingleShot(true);
    m_compactionWatchdog.setInterval(kCompactionWaitMs);
    connect(&m_compactionWatchdog, &QTimer::timeout, this, [this] {
        // Nothing more is waited for. A store that has not reported by now
        // will not (the backend task that would have sent it is gone), so it
        // is worded as silent, not as still working, and shows no figures.
        for (Compaction *c : {&m_eventCompaction, &m_mediaCompaction}) {
            if (c->expected && !c->got)
                c->gaveUp = true;
        }
        refreshSummary();
    });
}

void AboutController::setWaitsForTest(int resetMs, int compactionMs)
{
    m_resetWatchdog.setInterval(resetMs);
    m_compactionWatchdog.setInterval(compactionMs);
}

void AboutController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::homeserverInfoReceived, this,
                &AboutController::onHomeserverInfo);
        connect(m_client, &MatrixClient::accountDataListed, this,
                &AboutController::onAccountData);
        connect(m_client, &MatrixClient::userClientsListed, this,
                &AboutController::onUserClients);
        connect(m_client, &MatrixClient::storedMediaCleared, this,
                [this](quint64 opId, bool ok, qint64, qint64 keptBytes) {
                    if (opId != 0 && opId == m_mediaOp && ok)
                        m_keptFilesFreed += keptBytes;
                    onMediaCleared(opId, ok);
                });
        // After the step's own completion signal, on its own schedule.
        connect(m_client, &MatrixClient::cacheCompacted, this,
                &AboutController::onCacheCompacted);
        connect(m_client, &MatrixClient::eventCacheCleared, this,
                &AboutController::onEventCacheCleared);
        // Another account's server, listing and reset must not carry over.
        connect(m_client, &MatrixClient::loggedOut, this,
                &AboutController::onLoggedOut);
    }
    onLoggedOut();
}

void AboutController::setPackageKind(const QString &label)
{
    if (m_packageKind == label)
        return;
    m_packageKind = label;
    Q_EMIT versionSummaryChanged();
}

QString AboutController::versionSummary() const
{
    const QString platform = QStringLiteral("%1 (%2)")
        .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture());
    const QString kind = !m_packageKind.isEmpty()
        ? m_packageKind
        : QStringLiteral(LIGHTNING_ARTIFACT_KIND);
    // Labels are translated; the values (version, commit, platform, package
    // kind) stay raw so a pasted report is still unambiguous.
    return QStringLiteral("Lightning %1\n%2: %3\n%4: %5\n%6: %7")
        .arg(QStringLiteral(APP_VERSION), tr("Commit"), shortCommit(),
             tr("Platform"), platform, tr("Package"), kind);
}

QString AboutController::reportIssueUrl() const
{
    return QStringLiteral("https://github.com/Mizerd/lightning/issues");
}

void AboutController::copyVersion()
{
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(versionSummary());
}

void AboutController::refreshHomeserver()
{
    m_homeserverOp = m_client ? m_client->requestHomeserverInfo() : 0;
    m_homeserver.clear();
    m_homeserverState = m_homeserverOp != 0 ? QStringLiteral("loading")
                                            : QStringLiteral("unsupported");
    Q_EMIT homeserverChanged();
}

void AboutController::onHomeserverInfo(quint64 opId, const QVariantMap &info)
{
    if (opId == 0 || opId != m_homeserverOp)
        return;
    m_homeserverOp = 0;
    m_homeserver = info;
    m_homeserverState = info.value(QStringLiteral("ok")).toBool()
        ? QStringLiteral("ready") : QStringLiteral("failed");
    if (m_homeserverState == QLatin1String("failed"))
        m_homeserver.insert(QStringLiteral("error"),
            categoryText(info.value(QStringLiteral("category")).toString()));
    Q_EMIT homeserverChanged();
}

void AboutController::resetLocalCache()
{
    if (m_resetState == QLatin1String("running"))
        return;
    m_resetFailed.clear();
    m_resetDone.clear();
    m_resetSummary.clear();
    m_eventCompaction = {};
    m_mediaCompaction = {};
    m_keptFilesFreed = 0;
    m_clearSummary.clear();
    m_compactionWatchdog.stop();
    if (!m_client) {
        m_resetState = QStringLiteral("failed");
        m_resetSummary = tr("Nothing was cleared: you are not signed in.");
        Q_EMIT resetChanged();
        return;
    }
    m_resetState = QStringLiteral("running");
    m_resetPending = 0;
    // A step whose request was refused (op id 0) removed nothing: it is a
    // failure, never a silent success (CLAUDE.md section 6).
    m_mediaOp = m_client->clearStoredMedia();
    if (m_mediaOp != 0)
        ++m_resetPending;
    else
        m_resetFailed.append(tr("media cache"));
    m_eventCacheOp = m_client->clearEventCache();
    if (m_eventCacheOp != 0)
        ++m_resetPending;
    else
        m_resetFailed.append(tr("event cache"));
    // The index clears synchronously, through the controller that owns its
    // counters, and says whether it did.
    if (m_search && m_search->clearIndex())
        m_resetDone.append(tr("search index"));
    else
        m_resetFailed.append(tr("search index"));
    Q_EMIT resetChanged();
    if (m_resetPending == 0)
        finishReset(false);
    else
        m_resetWatchdog.start();
}

void AboutController::onMediaCleared(quint64 opId, bool ok)
{
    if (opId == 0 || opId != m_mediaOp)
        return;
    if (ok && m_client && m_client->reportsCacheCompaction())
        m_mediaCompaction = {m_mediaOp, true, false, {}, 0, 0};
    m_mediaOp = 0;
    stepFinished(tr("media cache"), ok);
}

void AboutController::onCacheCompacted(quint64 opId, const QString &store,
                                       const QString &outcome,
                                       qint64 bytesBefore, qint64 bytesAfter)
{
    Compaction *c = store == QLatin1String("event_cache") ? &m_eventCompaction
        : store == QLatin1String("media_store")           ? &m_mediaCompaction
                                                           : nullptr;
    // Matched to the clear that asked for it: a late report from an earlier
    // reset or another account lands nowhere.
    if (!c || opId == 0 || !c->expected || c->got || c->op != opId)
        return;
    c->got = true;
    c->outcome = outcome;
    c->before = qMax<qint64>(0, bytesBefore);
    c->after = qMax<qint64>(0, bytesAfter);
    refreshSummary();
}

QString AboutController::reclaimSummary() const
{
    // The sizes are the databases' footprint on disk, measured by the backend.
    // Only a store that finished contributes: an unfinished compaction has no
    // "after" yet (the VACUUM keeps running after a timeout), and "not
    // applicable" (no SQLite store: in-memory media, no media key) is neither
    // a figure nor a failure (CLAUDE.md section 6: absent is not failed).
    qint64 before = m_keptFilesFreed;
    qint64 after = 0;
    bool measured = m_keptFilesFreed > 0;
    QStringList failed;
    QStringList unfinished;
    QStringList silent;
    const struct { const Compaction &c; QString name; } stores[] = {
        {m_eventCompaction, tr("event cache")},
        {m_mediaCompaction, tr("media cache")},
    };
    for (const auto &entry : stores) {
        const Compaction &c = entry.c;
        if (!c.expected)
            continue;
        if (!c.got && c.gaveUp) {
            silent.append(entry.name);
        } else if (!c.got || c.outcome == QLatin1String("timed_out")) {
            unfinished.append(entry.name);
        } else if (c.outcome == QLatin1String("compacted")
                   || c.outcome == QLatin1String("unchanged")
                   || c.outcome == QLatin1String("failed")) {
            before += c.before;
            after += c.after;
            measured = true;
            if (c.outcome == QLatin1String("failed"))
                failed.append(entry.name);
        }
    }
    QStringList sentences;
    const QLocale locale;
    if (measured && (before > 0 || after > 0))
        sentences.append(tr("Disk use of those caches went from %1 to %2.")
                             .arg(locale.formattedDataSize(before),
                                  locale.formattedDataSize(after)));
    if (!failed.isEmpty())
        sentences.append(tr("Could not be compacted: %1.")
                             .arg(failed.join(QStringLiteral(", "))));
    if (!unfinished.isEmpty())
        sentences.append(tr("Still being compacted: %1. The space is returned "
                            "when that finishes.")
                             .arg(unfinished.join(QStringLiteral(", "))));
    if (!silent.isEmpty())
        sentences.append(tr("Compaction did not report back: %1.")
                             .arg(silent.join(QStringLiteral(", "))));
    return sentences.join(QLatin1Char(' '));
}

void AboutController::refreshSummary()
{
    // Only a fully successful clear carries a disk sentence.
    if (m_resetState != QLatin1String("done"))
        return;
    QString text = m_clearSummary;
    const QString reclaimed = reclaimSummary();
    if (!reclaimed.isEmpty())
        text += QLatin1Char(' ') + reclaimed;
    if (text == m_resetSummary)
        return;
    m_resetSummary = text;
    Q_EMIT resetChanged();
}

void AboutController::onEventCacheCleared(quint64 opId, bool ok)
{
    if (opId == 0 || opId != m_eventCacheOp)
        return;
    // A compaction report is due for a clear that succeeded, from a backend
    // that reports it. Opened before the step is counted finished so the
    // summary composed on the last step already waits for it.
    if (ok && m_client && m_client->reportsCacheCompaction())
        m_eventCompaction = {m_eventCacheOp, true, false, {}, 0, 0};
    m_eventCacheOp = 0;
    stepFinished(tr("event cache"), ok);
}

void AboutController::stepFinished(const QString &what, bool ok)
{
    if (m_resetState != QLatin1String("running"))
        return;
    if (ok)
        m_resetDone.append(what);
    else
        m_resetFailed.append(what);
    if (--m_resetPending <= 0)
        finishReset(false);
}

void AboutController::finishReset(bool timedOut)
{
    m_resetWatchdog.stop();
    m_mediaOp = 0;
    m_eventCacheOp = 0;
    if (timedOut) {
        m_resetState = QStringLiteral("failed");
        m_resetSummary = tr("Clearing did not finish. Some of the cache may "
                            "still be on disk; you can try again.");
    } else if (m_resetFailed.isEmpty() && !m_resetDone.isEmpty()) {
        m_resetState = QStringLiteral("done");
        m_clearSummary = tr("Cleared the event cache, the media cache and "
                            "the search index. Your sign-in and encryption "
                            "keys were not touched.");
        m_resetSummary = m_clearSummary;
        const QString reclaimed = reclaimSummary();
        if (!reclaimed.isEmpty())
            m_resetSummary += QLatin1Char(' ') + reclaimed;
        // A compaction still due is waited for on its own clock; the clear
        // itself is finished and stays "done".
        if ((m_eventCompaction.expected && !m_eventCompaction.got)
            || (m_mediaCompaction.expected && !m_mediaCompaction.got))
            m_compactionWatchdog.start();
    } else if (m_resetDone.isEmpty()) {
        m_resetState = QStringLiteral("failed");
        m_resetSummary = tr("Nothing was cleared (%1). Your sign-in and "
                            "encryption keys were not touched.")
                             .arg(m_resetFailed.join(QStringLiteral(", ")));
    } else {
        m_resetState = QStringLiteral("failed");
        m_resetSummary = tr("Cleared: %1. Could not clear: %2. Your sign-in "
                            "and encryption keys were not touched.")
                             .arg(m_resetDone.join(QStringLiteral(", ")),
                                  m_resetFailed.join(QStringLiteral(", ")));
    }
    Q_EMIT resetChanged();
    // After the state change, so a listener reads the finished reset. A
    // refused or failed clear step is not in m_resetDone: nothing to reload.
    // A watchdog on ANOTHER step does not undo an event-cache clear that
    // already succeeded.
    if (m_resetDone.contains(tr("event cache")))
        Q_EMIT eventCacheWasCleared();
}

void AboutController::loadAccountData(const QString &roomId)
{
    m_entries.clear();
    m_truncated = false;
    m_accountDataError.clear();
    m_accountDataRoom = roomId;
    m_accountDataOp = m_client ? m_client->requestAccountDataListing(roomId) : 0;
    m_accountDataState = m_accountDataOp != 0 ? QStringLiteral("loading")
                                              : QStringLiteral("unsupported");
    Q_EMIT accountDataChanged();
}

void AboutController::onAccountData(quint64 opId, const QVariantMap &listing)
{
    if (opId == 0 || opId != m_accountDataOp)
        return;
    m_accountDataOp = 0;
    if (listing.value(QStringLiteral("ok")).toBool()) {
        m_entries = listing.value(QStringLiteral("entries")).toList();
        m_truncated = listing.value(QStringLiteral("truncated")).toBool();
        m_accountDataState = QStringLiteral("ready");
    } else {
        m_entries.clear();
        m_accountDataState = QStringLiteral("failed");
        m_accountDataError =
            categoryText(listing.value(QStringLiteral("category")).toString());
    }
    Q_EMIT accountDataChanged();
}

void AboutController::clearAccountData()
{
    m_accountDataOp = 0;
    m_entries.clear();
    m_truncated = false;
    m_accountDataRoom.clear();
    m_accountDataError.clear();
    m_accountDataState = QStringLiteral("idle");
    Q_EMIT accountDataChanged();
}

void AboutController::requestUserClients(const QString &userId)
{
    if (!m_client || userId.isEmpty() || m_userClients.contains(userId))
        return;
    const quint64 op = m_client->requestUserClients(userId);
    QVariantMap row{{QStringLiteral("devices"), QVariantList{}}};
    row.insert(QStringLiteral("state"), op != 0 ? QStringLiteral("loading")
                                                : QStringLiteral("failed"));
    m_userClients.insert(userId, row);
    if (op != 0)
        m_userClientOps.insert(op, userId);
    ++m_clientsRevision;
    Q_EMIT userClientsChanged();
}

QVariantMap AboutController::userClients(const QString &userId) const
{
    const auto it = m_userClients.constFind(userId);
    if (it != m_userClients.constEnd())
        return *it;
    return QVariantMap{{QStringLiteral("state"), QStringLiteral("idle")},
                       {QStringLiteral("devices"), QVariantList{}}};
}

void AboutController::onUserClients(quint64 opId, const QVariantMap &result)
{
    const auto op = m_userClientOps.find(opId);
    if (op == m_userClientOps.end())
        return;
    const QString userId = op.value();
    m_userClientOps.erase(op);
    m_userClients.insert(
        userId, QVariantMap{
            {QStringLiteral("state"), result.value(QStringLiteral("ok")).toBool()
                 ? QStringLiteral("ready") : QStringLiteral("failed")},
            {QStringLiteral("devices"), result.value(QStringLiteral("devices"))}});
    ++m_clientsRevision;
    Q_EMIT userClientsChanged();
}

void AboutController::clearUserClients()
{
    if (m_userClients.isEmpty() && m_userClientOps.isEmpty())
        return;
    m_userClients.clear();
    m_userClientOps.clear();
    ++m_clientsRevision;
    Q_EMIT userClientsChanged();
}

void AboutController::onLoggedOut()
{
    clearUserClients();
    m_homeserverOp = 0;
    m_homeserver.clear();
    m_homeserverState = QStringLiteral("idle");
    m_resetWatchdog.stop();
    m_compactionWatchdog.stop();
    m_eventCompaction = {};
    m_mediaCompaction = {};
    m_mediaOp = 0;
    m_eventCacheOp = 0;
    m_resetState = QStringLiteral("idle");
    m_resetSummary.clear();
    clearAccountData();
    Q_EMIT homeserverChanged();
    Q_EMIT resetChanged();
}
