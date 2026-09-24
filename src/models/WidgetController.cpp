#include "models/WidgetController.h"

#include <QJsonDocument>
#include <QUuid>

#include "app/UrlLauncher.h"
#include "matrix/MatrixClient.h"

#include <QUrl>

WidgetController::WidgetController(QObject *parent)
    : QAbstractListModel(parent)
{
}

void WidgetController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clear();
    if (!m_client) {
        Q_EMIT stateChanged();
        return;
    }
    connect(m_client, &MatrixClient::roomWidgetsReceived, this,
            [this](quint64 opId, const QString &roomId, bool ok,
                   bool canManage, const QVariantList &widgets) {
        // Check the room as well as the op id: ids are unique per request, not
        // per room.
        if (opId != m_pendingOp || roomId != m_roomId)
            return;
        m_pendingOp = 0;
        if (!ok) {
            setState(QStringLiteral("error"));
            return;
        }
        m_canManage = canManage;
        beginResetModel();
        m_rows.clear();
        for (const QVariant &value : widgets)
            m_rows.append(value.toMap());
        endResetModel();
        setState(QStringLiteral("ready"));
    });
    connect(m_client, &MatrixClient::roomWidgetWritten, this,
            [this](quint64 opId, const QString &roomId, bool ok,
                   const QString &category) {
        if (opId == 0 || opId != m_writeOp)
            return;
        // Final for our op whatever room is showing, or `writing` stays stuck
        // after a room switch.
        m_writeOp = 0;
        if (roomId != m_roomId) {
            Q_EMIT stateChanged();
            return;
        }
        // Nothing is applied optimistically: on success the list is re-read; a
        // refusal leaves it alone.
        if (ok)
            refresh();
        const QString error = ok ? QString() : category;
        if (error != m_lastWriteError) {
            m_lastWriteError = error;
            Q_EMIT lastWriteErrorChanged();
        }
        Q_EMIT stateChanged();
        Q_EMIT writeFinished(ok, category);
    });
    // One account's widgets must never surface under another's.
    connect(m_client, &MatrixClient::loggedOut, this, [this] { clear(); });
    Q_EMIT stateChanged();
}

void WidgetController::setPresentation(const QString &themeName,
                                       const QString &language)
{
    m_themeName = themeName;
    m_language = language;
}

bool WidgetController::supported() const
{
    return m_client && m_client->supportsWidgets();
}

void WidgetController::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    m_pendingOp = 0;   // an answer for the old room must not repaint the new one
    m_writeOp = 0;     // and a write for it is no longer ours to wait for
    // The new room has granted nothing until its own read says so.
    m_canManage = false;
    if (!m_lastWriteError.isEmpty()) {
        m_lastWriteError.clear();
        Q_EMIT lastWriteErrorChanged();
    }
    beginResetModel();
    m_rows.clear();
    endResetModel();
    Q_EMIT roomIdChanged();
    setState(QStringLiteral("idle"));
    Q_EMIT stateChanged();
}

bool WidgetController::urlIsAcceptable(const QString &text) const
{
    // Same rule as UrlLauncher and the Rust write: https, a host, no
    // credentials. Never publish a widget this client would refuse to open.
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    return url.isValid() && url.scheme().toLower() == QLatin1String("https")
        && !url.host().isEmpty() && url.userInfo().isEmpty();
}

QJsonObject WidgetController::widgetContent(const QString &kind,
                                            const QString &name,
                                            const QString &url,
                                            const QString &creatorUserId)
{
    // Element's shape, field for field, so every client can list it.
    // `waitForIframeLoad` is meaningless here but expected by readers.
    QJsonObject content{
        { QStringLiteral("type"), kind },
        { QStringLiteral("url"), url.trimmed() },
        { QStringLiteral("name"), name.trimmed() },
        { QStringLiteral("creatorUserId"), creatorUserId },
        { QStringLiteral("waitForIframeLoad"), true },
    };
    QJsonObject data;
    if (kind == QLatin1String("m.jitsi")) {
        // A Jitsi widget carries its conference in `data`, which Element reads;
        // derived from the URL's host and first path segment.
        const QUrl u(url.trimmed());
        const QString conference =
            u.path().section(QLatin1Char('/'), 0, 0, QString::SectionSkipEmpty);
        data.insert(QStringLiteral("domain"), u.host());
        data.insert(QStringLiteral("conferenceId"), conference);
        data.insert(QStringLiteral("isAudioOnly"), false);
    }
    content.insert(QStringLiteral("data"), data);
    return content;
}

void WidgetController::addWidget(const QString &kind, const QString &name,
                                 const QString &url)
{
    if (!supported() || m_roomId.isEmpty() || m_writeOp != 0 || !m_canManage)
        return;
    static const QStringList kKinds{
        QStringLiteral("m.custom"), QStringLiteral("m.etherpad"),
        QStringLiteral("m.jitsi"), QStringLiteral("m.video"),
        QStringLiteral("m.image"), QStringLiteral("m.grafana"),
    };
    if (!kKinds.contains(kind) || !urlIsAcceptable(url))
        return;
    // The id is the state key: a fresh UUID, so another client's write cannot
    // land on this key by accident.
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject content = widgetContent(
        kind, name.isEmpty() ? kind : name, url, m_client->currentUserId());
    const quint64 op = m_client->writeRoomWidget(
        m_roomId, id,
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Compact)));
    if (op == 0) {
        Q_EMIT writeFinished(false, QStringLiteral("rejected"));
        return;
    }
    m_writeOp = op;
    Q_EMIT stateChanged();
}

void WidgetController::removeWidget(int row)
{
    if (!supported() || m_roomId.isEmpty() || m_writeOp != 0 || !m_canManage)
        return;
    if (row < 0 || row >= m_rows.size())
        return;
    const QVariantMap &target = m_rows.at(row);
    // The tombstone must name the exact state key; rows whose key the reader
    // could not preserve exactly (or legacy `m.widget` rows) are not removable.
    if (target.value(QStringLiteral("removable"), true).toBool() == false)
        return;
    const QString id = target.value(QStringLiteral("stateKey"),
                                    target.value(QStringLiteral("id"))).toString();
    if (id.isEmpty())
        return;
    // An empty content object removes the widget, as in Element.
    const quint64 op = m_client->writeRoomWidget(m_roomId, id,
                                                 QStringLiteral("{}"));
    if (op == 0) {
        Q_EMIT writeFinished(false, QStringLiteral("rejected"));
        return;
    }
    m_writeOp = op;
    Q_EMIT stateChanged();
}

void WidgetController::clear()
{
    m_pendingOp = 0;
    m_writeOp = 0;
    m_canManage = false;
    beginResetModel();
    m_rows.clear();
    endResetModel();
    setState(QStringLiteral("idle"));
}

void WidgetController::setState(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

void WidgetController::refresh()
{
    if (!supported() || m_roomId.isEmpty())
        return;
    const QString language = m_language;
    const QString theme = m_themeName;
    const quint64 op = m_client->roomWidgets(m_roomId, theme, language);
    if (op == 0) {
        setState(QStringLiteral("error"));
        return;
    }
    m_pendingOp = op;
    setState(QStringLiteral("loading"));
}

int WidgetController::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant WidgetController::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const QVariantMap &row = m_rows.at(index.row());
    switch (role) {
    case WidgetIdRole:  return row.value(QStringLiteral("id"));
    case CreatorRole:   return row.value(QStringLiteral("creator"));
    case KindRole:      return row.value(QStringLiteral("kind"));
    case NameRole:      return row.value(QStringLiteral("name"));
    case UrlRole:       return row.value(QStringLiteral("url"));
    case RefusalRole:   return row.value(QStringLiteral("refusal"));
    case DisclosesRole: return row.value(QStringLiteral("discloses"));
    case OpenableRole:
        return !row.value(QStringLiteral("url")).toString().isEmpty();
    case StateKeyRole:  return row.value(QStringLiteral("stateKey"));
    // Absent (mock rows, older payloads) reads as removable, like
    // removeWidget().
    case RemovableRole: return row.value(QStringLiteral("removable"), true);
    default: return {};
    }
}

QHash<int, QByteArray> WidgetController::roleNames() const
{
    return {
        { WidgetIdRole,  "widgetId" },
        { CreatorRole,   "creator" },
        { KindRole,      "kind" },
        { NameRole,      "name" },
        { UrlRole,       "url" },
        { RefusalRole,   "refusal" },
        { DisclosesRole, "discloses" },
        { OpenableRole,  "openable" },
        { StateKeyRole,  "stateKey" },
        { RemovableRole, "removable" },
    };
}

bool WidgetController::openWidget(int row)
{
    if (row < 0 || row >= m_rows.size())
        return false;
    const QString address = m_rows.at(row).value(QStringLiteral("url")).toString();
    if (address.isEmpty())
        return false;
    const QUrl url(address);
    // Second gate after Rust's https/host/no-userinfo check: ask the single
    // desktop exit whether it would open the scheme at all.
    if (!url.isValid() || url.scheme() != QLatin1String("https"))
        return false;
    if (!lightning::urls::isOpenableExternally(url))
        return false;
    return lightning::urls::openExternally(url);
}

QVariantMap WidgetController::rowAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows.at(row);
}

QString WidgetController::disclosureText(const QString &key) const
{
    // Phrased as what the site learns, not as a field name.
    if (key == QLatin1String("user_id"))
        return tr("Your full Matrix ID");
    if (key == QLatin1String("display_name"))
        return tr("Your display name");
    if (key == QLatin1String("avatar_url"))
        return tr("A link to your profile picture");
    if (key == QLatin1String("device_id"))
        return tr("This device's ID");
    if (key == QLatin1String("room_id"))
        return tr("Which room you opened it from");
    if (key == QLatin1String("theme"))
        return tr("Which theme you use");
    if (key == QLatin1String("language"))
        return tr("Which language you use");
    if (key == QLatin1String("homeserver"))
        return tr("Your homeserver's address");
    if (key == QLatin1String("connection"))
        return tr("Your IP address, and anything your browser normally sends");
    // Unknown keys are disclosed rather than hidden, so the notice never
    // quietly shrinks.
    return tr("Something this build does not recognise (%1)").arg(key);
}

QString WidgetController::refusalText(const QString &reason) const
{
    if (reason == QLatin1String("not_https"))
        return tr("This widget's address is not HTTPS, so Lightning will not "
                  "open it.");
    if (reason == QLatin1String("has_userinfo"))
        return tr("This widget's address hides its real site behind a name "
                  "before the @, so Lightning will not open it.");
    if (reason == QLatin1String("templated_authority"))
        return tr("This widget builds its own address out of your profile, "
                  "which could send your details anywhere. Lightning will not "
                  "open it.");
    if (reason == QLatin1String("no_host"))
        return tr("This widget's address names no site.");
    if (reason == QLatin1String("not_a_url"))
        return tr("This widget's address is not a valid web address.");
    return tr("Lightning cannot open this widget.");
}
