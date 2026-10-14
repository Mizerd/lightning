#include "app/PushRulesController.h"

#include "matrix/MatrixClient.h"
#include "matrix/PushRulesSnapshot.h"

namespace {

// Mirrors rust/src/pushrules.rs clean_keyword so a refusal is shown at once
// instead of after a round trip. The Rust side stays the authority.
constexpr int kMaxKeywordChars = 120;

QString mentionWireKey(const QString &key)
{
    if (key == QLatin1String("userId"))
        return QStringLiteral("user_id");
    if (key == QLatin1String("displayName"))
        return QStringLiteral("display_name");
    if (key == QLatin1String("username"))
        return QStringLiteral("username");
    if (key == QLatin1String("room"))
        return QStringLiteral("room");
    return {};
}

QString mentionSnapshotKey(const QString &key)
{
    if (key == QLatin1String("userId"))
        return QStringLiteral("mentionUserId");
    if (key == QLatin1String("displayName"))
        return QStringLiteral("mentionDisplayName");
    if (key == QLatin1String("username"))
        return QStringLiteral("mentionUsername");
    if (key == QLatin1String("room"))
        return QStringLiteral("mentionRoom");
    return {};
}

QString eventSnapshotKey(const QString &key)
{
    if (key == QLatin1String("reactions"))
        return QStringLiteral("eventReactions");
    if (key == QLatin1String("invites"))
        return QStringLiteral("eventInvites");
    if (key == QLatin1String("notices"))
        return QStringLiteral("eventNotices");
    return {};
}

} // namespace

PushRulesController::PushRulesController(QObject *parent)
    : QObject(parent)
    , m_snap(PushRulesSnapshot::empty())
    , m_lastServer(PushRulesSnapshot::empty())
{
}

void PushRulesController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    onLoggedOut();
    if (m_client) {
        connect(m_client, &MatrixClient::pushRulesReceived, this,
                &PushRulesController::onSnapshot);
        connect(m_client, &MatrixClient::pushRuleWriteFinished, this,
                &PushRulesController::onWriteFinished);
        connect(m_client, &MatrixClient::initialSyncDoneChanged, this,
                &PushRulesController::onInitialSyncDoneChanged);
        connect(m_client, &MatrixClient::loggedOut, this,
                &PushRulesController::onLoggedOut);
    }
    Q_EMIT stateChanged();
}

bool PushRulesController::supported() const
{
    return m_client && m_client->supportsPushRules();
}

QVariantList PushRulesController::keywords() const
{
    return m_snap.value(QStringLiteral("keywords")).toList();
}

void PushRulesController::refresh()
{
    if (!supported() || m_refreshOp != 0)
        return;
    const quint64 opId = m_client->requestPushRules();
    if (opId == 0) {
        m_error = describeCategory(QStringLiteral("network"));
    } else {
        m_refreshOp = opId;
    }
    Q_EMIT stateChanged();
}

void PushRulesController::write(const QString &kind, const QString &key,
                                const QString &value,
                                const QVariantMap &optimistic)
{
    if (!supported())
        return;
    const QVariantMap before = m_snap;
    const quint64 opId = m_client->writePushRule(kind, key, value);
    if (opId == 0) {
        m_error = describeCategory(QStringLiteral("network"));
        Q_EMIT stateChanged();
        Q_EMIT writeFailed(m_error);
        return;
    }
    if (m_pending.isEmpty())
        m_lastServer = before;
    m_pending.insert(opId, kind);
    m_error.clear();
    for (auto it = optimistic.cbegin(); it != optimistic.cend(); ++it)
        m_snap.insert(it.key(), it.value());
    Q_EMIT stateChanged();
}

void PushRulesController::setMasterMute(bool on)
{
    write(QStringLiteral("master_mute"), QString(),
          on ? QStringLiteral("on") : QStringLiteral("off"),
          {{QStringLiteral("masterMute"), on}});
}

void PushRulesController::setDefaultAll(bool directMessages, bool allMessages)
{
    const QString prefix = directMessages ? QStringLiteral("dm")
                                          : QStringLiteral("rooms");
    write(QStringLiteral("default_level"), prefix,
          allMessages ? QStringLiteral("all") : QStringLiteral("mentions"),
          {{prefix + QStringLiteral("All"), allMessages},
           {prefix + QStringLiteral("Drift"), false}});
}

QString PushRulesController::levelName(int level)
{
    return level >= 2 ? QStringLiteral("loud")
        : level == 1 ? QStringLiteral("notify") : QStringLiteral("off");
}

void PushRulesController::setMention(const QString &key, int level)
{
    const QString wire = mentionWireKey(key);
    if (wire.isEmpty() || this->level(mentionSnapshotKey(key)) < 0)
        return;
    write(QStringLiteral("mention"), wire, levelName(level),
          {{mentionSnapshotKey(key), qBound(0, level, 2)}});
}

void PushRulesController::setEventRule(const QString &key, int level)
{
    const QString snapshotKey = eventSnapshotKey(key);
    if (snapshotKey.isEmpty() || this->level(snapshotKey) < 0)
        return;
    const int clamped = level >= 1 ? 1 : 0;
    write(QStringLiteral("event"), key, levelName(clamped),
          {{snapshotKey, clamped}});
}

QString PushRulesController::keywordProblem(const QString &keyword) const
{
    const QString k = keyword.trimmed();
    if (k.isEmpty())
        return tr("Type a word or phrase.");
    if (k.size() > kMaxKeywordChars)
        return tr("Keep a keyword under %1 characters.")
            .arg(kMaxKeywordChars + 1);
    if (k.startsWith(QLatin1Char('.')))
        return tr("A keyword cannot start with a full stop.");
    if (k.contains(QLatin1Char('/')))
        return tr("A keyword cannot contain a slash.");
    for (const QChar c : k) {
        if (c.isNonCharacter() || c.category() == QChar::Other_Control)
            return tr("A keyword cannot contain control characters.");
    }
    return {};
}

namespace {
QVariantList withKeyword(QVariantList list, const QString &keyword, int level,
                         bool remove)
{
    for (int i = 0; i < list.size(); ++i) {
        if (list.at(i).toMap().value(QStringLiteral("keyword")).toString()
            == keyword) {
            list.removeAt(i);
            break;
        }
    }
    if (!remove) {
        QVariantMap entry;
        entry.insert(QStringLiteral("keyword"), keyword);
        entry.insert(QStringLiteral("level"), level);
        list.append(entry);
    }
    return list;
}
} // namespace

bool PushRulesController::addKeyword(const QString &keyword, int level)
{
    const QString problem = keywordProblem(keyword);
    if (!problem.isEmpty()) {
        m_error = problem;
        Q_EMIT stateChanged();
        return false;
    }
    setKeywordLevel(keyword.trimmed(), level);
    return true;
}

void PushRulesController::setKeywordLevel(const QString &keyword, int level)
{
    const QString k = keyword.trimmed();
    if (!keywordProblem(k).isEmpty())
        return;
    const int clamped = qBound(0, level, 2);
    write(QStringLiteral("keyword_set"), k, levelName(clamped),
          {{QStringLiteral("keywords"),
            withKeyword(keywords(), k, clamped, false)}});
}

void PushRulesController::removeKeyword(const QString &keyword)
{
    const QString k = keyword.trimmed();
    if (!keywordProblem(k).isEmpty())
        return;
    write(QStringLiteral("keyword_remove"), k, QString(),
          {{QStringLiteral("keywords"), withKeyword(keywords(), k, 0, true)}});
}

void PushRulesController::onSnapshot(quint64 opId, bool ok,
                                     const QVariantMap &snapshot,
                                     const QString &category)
{
    const bool mine = opId == m_refreshOp || m_pending.contains(opId)
        || m_pending.isEmpty();
    if (opId == m_refreshOp)
        m_refreshOp = 0;
    if (!mine)
        return;
    if (!ok) {
        if (opId != 0 && m_pending.isEmpty()) {
            m_error = describeCategory(category);
            Q_EMIT stateChanged();
        }
        return;
    }
    m_lastServer = snapshot;
    // While writes are in flight the optimistic state is what the user just
    // chose; it is replaced once the last of them has answered.
    if (m_pending.isEmpty()) {
        m_snap = snapshot;
        m_loaded = true;
        m_error.clear();
    }
    Q_EMIT stateChanged();
}

void PushRulesController::onWriteFinished(quint64 opId, const QString &kind,
                                          bool ok, const QString &category)
{
    if (!m_pending.contains(opId))
        return;
    // The kind this controller sent, not the echoed argument.
    const QString sentKind = m_pending.take(opId);
    Q_UNUSED(kind);
    if (ok) {
        // The fresh snapshot follows; keep the optimistic state until then.
        Q_EMIT stateChanged();
        if (sentKind == QLatin1String("default_level"))
            Q_EMIT accountDefaultChanged();
        return;
    }
    // Put the last server state back, but keep the choices of writes that are
    // still in flight out of it: the next snapshot settles those.
    m_snap = m_lastServer;
    m_error = describeCategory(category);
    Q_EMIT stateChanged();
    Q_EMIT writeFailed(m_error);
}

void PushRulesController::onInitialSyncDoneChanged()
{
    if (m_client && m_client->initialSyncDone())
        refresh();
}

void PushRulesController::onLoggedOut()
{
    m_snap = PushRulesSnapshot::empty();
    m_lastServer = m_snap;
    m_pending.clear();
    m_refreshOp = 0;
    m_loaded = false;
    m_error.clear();
    Q_EMIT stateChanged();
}

QString PushRulesController::describeCategory(const QString &category)
{
    if (category == QLatin1String("unsupported"))
        return tr("Your homeserver does not have that notification rule.");
    if (category == QLatin1String("invalid"))
        return tr("That setting was not accepted.");
    if (category == QLatin1String("forbidden"))
        return tr("Your homeserver refused the change.");
    if (category == QLatin1String("rate_limited"))
        return tr("Your homeserver is limiting requests. Try again in a moment.");
    return tr("The change could not be saved. Check your connection and try "
              "again.");
}
