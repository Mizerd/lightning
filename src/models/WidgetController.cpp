#include "models/WidgetController.h"

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
                   const QVariantList &widgets) {
        // BOTH checks. The op id alone would let an answer for a room the user
        // has since left repaint the panel, because ids are unique per request
        // and not per room.
        if (opId != m_pendingOp || roomId != m_roomId)
            return;
        m_pendingOp = 0;
        if (!ok) {
            setState(QStringLiteral("error"));
            return;
        }
        beginResetModel();
        m_rows.clear();
        for (const QVariant &value : widgets)
            m_rows.append(value.toMap());
        endResetModel();
        setState(QStringLiteral("ready"));
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
    beginResetModel();
    m_rows.clear();
    endResetModel();
    Q_EMIT roomIdChanged();
    setState(QStringLiteral("idle"));
}

void WidgetController::clear()
{
    m_pendingOp = 0;
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
    // The SECOND gate. Rust already refused anything but https with a host and
    // no userinfo; this asks the application's single desktop exit whether it
    // would open the scheme at all. Two independent checks on the one path
    // that leaves the process is the right number for a URL that arrived as
    // room state.
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
    // One sentence per key, phrased as what the SITE learns rather than as a
    // field name. "user_id" tells nobody anything; "your full Matrix ID" does.
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
    // An unknown key is disclosed HONESTLY rather than hidden: a widget API
    // that grows a variable Lightning does not recognise must not make the
    // notice quietly shorter.
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
