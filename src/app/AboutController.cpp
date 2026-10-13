#include "app/AboutController.h"

#include "matrix/MatrixClient.h"
#include "models/MessageSearchController.h"

#include <QGuiApplication>
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
    m_resetWatchdog.setInterval(60 * 1000);
    connect(&m_resetWatchdog, &QTimer::timeout, this,
            [this] { finishReset(true); });
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
        connect(m_client, &MatrixClient::storedMediaCleared, this,
                [this](quint64 opId, bool ok, qint64, qint64) {
                    onMediaCleared(opId, ok);
                });
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
    return QStringLiteral("Lightning %1\nCommit: %2\nPlatform: %3\nPackage: %4")
        .arg(QStringLiteral(APP_VERSION), shortCommit(), platform, kind);
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
    m_resetSummary.clear();
    if (!m_client) {
        m_resetState = QStringLiteral("failed");
        m_resetSummary = tr("Nothing was cleared: you are not signed in.");
        Q_EMIT resetChanged();
        return;
    }
    m_resetState = QStringLiteral("running");
    m_resetPending = 0;
    m_mediaOp = m_client->clearStoredMedia();
    if (m_mediaOp != 0)
        ++m_resetPending;
    m_eventCacheOp = m_client->clearEventCache();
    if (m_eventCacheOp != 0)
        ++m_resetPending;
    // The index clears synchronously, through the controller that owns its
    // counters.
    if (m_search)
        m_search->clearIndex();
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
    m_mediaOp = 0;
    stepFinished(tr("media cache"), ok);
}

void AboutController::onEventCacheCleared(quint64 opId, bool ok)
{
    if (opId == 0 || opId != m_eventCacheOp)
        return;
    m_eventCacheOp = 0;
    stepFinished(tr("event cache"), ok);
}

void AboutController::stepFinished(const QString &what, bool ok)
{
    if (m_resetState != QLatin1String("running"))
        return;
    if (!ok)
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
    } else if (m_resetFailed.isEmpty()) {
        m_resetState = QStringLiteral("done");
        m_resetSummary = tr("Cleared the event cache, the media cache and "
                            "the search index. Your sign-in and encryption "
                            "keys were not touched.");
    } else {
        m_resetState = QStringLiteral("failed");
        m_resetSummary = tr("Could not clear: %1. Everything else was "
                            "cleared; your sign-in and encryption keys were "
                            "not touched.")
                             .arg(m_resetFailed.join(QStringLiteral(", ")));
    }
    Q_EMIT resetChanged();
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

void AboutController::onLoggedOut()
{
    m_homeserverOp = 0;
    m_homeserver.clear();
    m_homeserverState = QStringLiteral("idle");
    m_resetWatchdog.stop();
    m_mediaOp = 0;
    m_eventCacheOp = 0;
    m_resetState = QStringLiteral("idle");
    m_resetSummary.clear();
    clearAccountData();
    Q_EMIT homeserverChanged();
    Q_EMIT resetChanged();
}
