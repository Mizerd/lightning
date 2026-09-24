#include "spaces/RailLayoutStore.h"

#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
// Shared with SettingsManager::forgetDeviceGlobalAccountResidue, which removes
// it when the last account is cleared.
constexpr auto kLayoutKey = SettingsManager::kRailLayoutKey;

// Pseudo rows (All rooms, orphans) cannot be ordered or filed; they stay at
// the top.
bool isPseudoSpace(const QString &id)
{
    return id.isEmpty() || id.startsWith(QLatin1Char('@'));
}
} // namespace

RailLayoutStore::RailLayoutStore(SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    if (m_settings) {
        // loggedOut (see setClient) fires before the active account moves,
        // and the resulting rebuild re-caches the outgoing account's layout.
        // sessionChanged fires after the move, so it leaves the cache right.
        connect(m_settings, &SettingsManager::sessionChanged, this,
                &RailLayoutStore::invalidate);
    }
}

void RailLayoutStore::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    if (!m_client)
        return;
    connect(m_client, &QObject::destroyed, this, [this] { m_client = nullptr; });
    connect(m_client, &MatrixClient::loggedOut, this,
            &RailLayoutStore::invalidate);
}

void RailLayoutStore::invalidate()
{
    // Unconditional: the next read must consult the account active now.
    m_loaded = false;
    m_cache = {};
    Q_EMIT layoutChanged();
}

const RailLayoutStore::Layout &RailLayoutStore::load() const
{
    if (m_loaded)
        return m_cache;
    m_loaded = true;
    m_cache = {};
    if (!m_settings)
        return m_cache;

    const QString json =
        m_settings->accountScopedValue(kLayoutKey, QString()).toString();
    if (json.isEmpty())
        return m_cache;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject())
        return m_cache;
    const QJsonObject object = doc.object();

    const QJsonArray folders = object.value(QStringLiteral("folders")).toArray();
    for (const QJsonValue &value : folders) {
        if (!value.isObject())
            continue;
        const QJsonObject entry = value.toObject();
        Folder folder;
        folder.id = entry.value(QStringLiteral("id")).toString();
        if (folder.id.isEmpty())
            continue;
        folder.name = entry.value(QStringLiteral("name")).toString()
                          .left(kMaxNameLength);
        folder.collapsed = entry.value(QStringLiteral("collapsed")).toBool();
        for (const QJsonValue &member :
             entry.value(QStringLiteral("spaceIds")).toArray()) {
            const QString id = member.toString();
            // A pseudo row can never be filed, even in a hand-edited config.
            if (!id.isEmpty() && !isPseudoSpace(id)
                && !folder.spaceIds.contains(id))
                folder.spaceIds.append(id);
        }
        m_cache.folders.append(folder);
        if (m_cache.folders.size() >= kMaxFolders)
            break;
    }
    for (const QJsonValue &value : object.value(QStringLiteral("order")).toArray()) {
        const QString id = value.toString();
        if (!id.isEmpty() && !isPseudoSpace(id) && !m_cache.order.contains(id))
            m_cache.order.append(id);
    }
    // Additive format: a layout without "expanded" loads with nothing
    // expanded. Never migrate what can be defaulted.
    for (const QJsonValue &value :
         object.value(QStringLiteral("expanded")).toArray()) {
        const QString id = value.toString();
        if (!id.isEmpty() && !isPseudoSpace(id)
            && !m_cache.expanded.contains(id)) {
            m_cache.expanded.append(id);
        }
    }
    // Also additive: without "childOrder"/"roomOrder", Matrix's own order.
    const auto readOrderMap = [](const QJsonObject &source) {
        QHash<QString, QStringList> out;
        for (auto it = source.constBegin(); it != source.constEnd(); ++it) {
            if (it.key().isEmpty() || isPseudoSpace(it.key()))
                continue;
            QStringList ids;
            for (const QJsonValue &value : it.value().toArray()) {
                const QString id = value.toString();
                if (!id.isEmpty() && !isPseudoSpace(id) && !ids.contains(id))
                    ids.append(id);
            }
            if (!ids.isEmpty())
                out.insert(it.key(), ids);
        }
        return out;
    };
    m_cache.childOrder =
        readOrderMap(object.value(QStringLiteral("childOrder")).toObject());
    m_cache.roomOrder =
        readOrderMap(object.value(QStringLiteral("roomOrder")).toObject());
    return m_cache;
}

void RailLayoutStore::save(const Layout &layout)
{
    if (!m_settings)
        return;
    const Layout next = dropEmptiedFolders(layout);
    QJsonArray folders;
    for (const Folder &folder : next.folders) {
        QJsonObject entry;
        entry.insert(QStringLiteral("id"), folder.id);
        entry.insert(QStringLiteral("name"), folder.name);
        entry.insert(QStringLiteral("collapsed"), folder.collapsed);
        entry.insert(QStringLiteral("spaceIds"),
                     QJsonArray::fromStringList(folder.spaceIds));
        folders.append(entry);
    }
    QJsonObject object;
    object.insert(QStringLiteral("folders"), folders);
    object.insert(QStringLiteral("order"),
                  QJsonArray::fromStringList(next.order));
    object.insert(QStringLiteral("expanded"),
                  QJsonArray::fromStringList(next.expanded));
    const auto writeOrderMap = [](const QHash<QString, QStringList> &source) {
        QJsonObject out;
        for (auto it = source.constBegin(); it != source.constEnd(); ++it) {
            if (it.value().isEmpty())
                continue;
            out.insert(it.key(), QJsonArray::fromStringList(it.value()));
        }
        return out;
    };
    object.insert(QStringLiteral("childOrder"),
                  writeOrderMap(next.childOrder));
    object.insert(QStringLiteral("roomOrder"), writeOrderMap(next.roomOrder));
    m_settings->setAccountScopedValue(
        kLayoutKey, QString::fromUtf8(
                        QJsonDocument(object).toJson(QJsonDocument::Compact)));
    m_cache = next;
    m_loaded = true;
    Q_EMIT layoutChanged();
}

RailLayoutStore::Layout
RailLayoutStore::dropEmptiedFolders(const Layout &layout) const
{
    // A folder the user emptied is removed by the write that emptied it.
    //
    // Empty means `spaceIds` is empty in the store, never "the rail drew no
    // members": during sync or a cold start the rendered members can
    // legitimately be none for a full folder.
    //
    // Only folders that were non-empty before this write: "New folder…"
    // deliberately creates an empty one for the user to fill.
    //
    // Done here because every path that removes a member ends in save().
    if (!m_loaded)
        return layout;   // nothing was loaded, so nothing was emptied
    Layout next = layout;
    for (int i = next.folders.size() - 1; i >= 0; --i) {
        if (!next.folders.at(i).spaceIds.isEmpty())
            continue;
        const QString id = next.folders.at(i).id;
        bool hadMembers = false;
        for (const Folder &before : m_cache.folders) {
            if (before.id != id)
                continue;
            hadMembers = !before.spaceIds.isEmpty();
            break;
        }
        if (!hadMembers)
            continue;
        next.folders.removeAt(i);
        // Its id is also a top-level entry.
        next.order.removeAll(id);
    }
    return next;
}

QString RailLayoutStore::makeFolderId(const Layout &layout)
{
    for (int candidate = 1; candidate <= kMaxFolders * 4; ++candidate) {
        const QString id = QStringLiteral("f") + QString::number(candidate);
        bool taken = false;
        for (const Folder &folder : layout.folders) {
            if (folder.id == id) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return id;
    }
    return {};
}

QVariantList RailLayoutStore::folders() const
{
    QVariantList out;
    for (const Folder &folder : load().folders) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), folder.id);
        entry.insert(QStringLiteral("name"), folder.name);
        entry.insert(QStringLiteral("collapsed"), folder.collapsed);
        entry.insert(QStringLiteral("spaceIds"), folder.spaceIds);
        out.append(entry);
    }
    return out;
}

QStringList RailLayoutStore::order() const
{
    return load().order;
}

QString RailLayoutStore::createFolder(const QString &name)
{
    Layout layout = load();
    if (layout.folders.size() >= kMaxFolders)
        return {};
    Folder folder;
    folder.id = makeFolderId(layout);
    if (folder.id.isEmpty())
        return {};
    folder.name = name.trimmed().left(kMaxNameLength);
    if (folder.name.isEmpty())
        folder.name = tr("Folder");
    layout.folders.append(folder);
    layout.order.append(folder.id);
    save(layout);
    return folder.id;
}

void RailLayoutStore::renameFolder(const QString &folderId, const QString &name)
{
    Layout layout = load();
    for (Folder &folder : layout.folders) {
        if (folder.id != folderId)
            continue;
        const QString clean = name.trimmed().left(kMaxNameLength);
        if (clean.isEmpty() || clean == folder.name)
            return;
        folder.name = clean;
        save(layout);
        return;
    }
}

void RailLayoutStore::deleteFolder(const QString &folderId)
{
    Layout layout = load();
    for (int i = 0; i < layout.folders.size(); ++i) {
        if (layout.folders.at(i).id != folderId)
            continue;
        // Members return to the top level where the folder was, not at the
        // end.
        const QStringList members = layout.folders.at(i).spaceIds;
        layout.folders.removeAt(i);
        const int at = layout.order.indexOf(folderId);
        if (at >= 0) {
            layout.order.removeAt(at);
            for (int m = members.size() - 1; m >= 0; --m) {
                if (!layout.order.contains(members.at(m)))
                    layout.order.insert(at, members.at(m));
            }
        } else {
            for (const QString &id : members) {
                if (!layout.order.contains(id))
                    layout.order.append(id);
            }
        }
        save(layout);
        return;
    }
}

void RailLayoutStore::setFolderCollapsed(const QString &folderId, bool collapsed)
{
    Layout layout = load();
    for (Folder &folder : layout.folders) {
        if (folder.id != folderId || folder.collapsed == collapsed)
            continue;
        folder.collapsed = collapsed;
        save(layout);
        return;
    }
}

QString RailLayoutStore::folderOf(const QString &spaceId) const
{
    for (const Folder &folder : load().folders) {
        if (folder.spaceIds.contains(spaceId))
            return folder.id;
    }
    return {};
}

void RailLayoutStore::setSpaceFolder(const QString &spaceId,
                                     const QString &folderId)
{
    if (spaceId.isEmpty() || isPseudoSpace(spaceId))
        return;
    Layout layout = load();
    bool changed = false;
    // A Space is in at most one folder: leaving the old one is part of
    // joining the new one.
    for (Folder &folder : layout.folders) {
        if (folder.id != folderId && folder.spaceIds.removeAll(spaceId) > 0)
            changed = true;
    }
    if (folderId.isEmpty()) {
        // Back to the top level, at the end: its old slot was the folder's.
        if (!layout.order.contains(spaceId)) {
            layout.order.append(spaceId);
            changed = true;
        }
    } else {
        bool known = false;
        for (Folder &folder : layout.folders) {
            if (folder.id != folderId)
                continue;
            known = true;
            if (!folder.spaceIds.contains(spaceId)) {
                folder.spaceIds.append(spaceId);
                changed = true;
            }
        }
        if (!known)
            return;
        // A filed Space is no longer a top-level entry.
        if (layout.order.removeAll(spaceId) > 0)
            changed = true;
    }
    if (changed)
        save(layout);
}

void RailLayoutStore::moveEntry(const QString &entryId, int toIndex)
{
    if (entryId.isEmpty() || isPseudoSpace(entryId))
        return;
    Layout layout = load();
    // An entry never dragged is not in `order` yet; the rail passes the
    // index it dropped at and everything before it is already materialised.
    const int from = layout.order.indexOf(entryId);
    if (from >= 0)
        layout.order.removeAt(from);
    const int clamped = qBound(0, toIndex, layout.order.size());
    layout.order.insert(clamped, entryId);
    save(layout);
}

namespace {

/// The stored arrangement first, for ids still present, then anything it does
/// not mention in the caller's order, so something new joins the end of the
/// run. A departed id is simply not returned (not cleaned up eagerly): an
/// unfinished load is not a change.
QStringList applyStoredOrder(const QStringList &stored,
                             const QStringList &known)
{
    if (known.size() < 2 || stored.isEmpty())
        return known;
    QStringList out;
    out.reserve(known.size());
    for (const QString &id : stored) {
        if (known.contains(id) && !out.contains(id))
            out.append(id);
    }
    for (const QString &id : known) {
        if (!out.contains(id))
            out.append(id);
    }
    return out;
}

QStringList sanitizedOrder(const QStringList &ids)
{
    QStringList out;
    out.reserve(ids.size());
    for (const QString &id : ids) {
        if (!id.isEmpty() && !isPseudoSpace(id) && !out.contains(id))
            out.append(id);
    }
    return out;
}

} // namespace

QStringList RailLayoutStore::orderedChildren(const QString &parentId,
                                             const QStringList &known) const
{
    if (parentId.isEmpty())
        return known;
    return applyStoredOrder(load().childOrder.value(parentId), known);
}

void RailLayoutStore::setChildOrder(const QString &parentId,
                                    const QStringList &childIds)
{
    if (parentId.isEmpty() || isPseudoSpace(parentId))
        return;
    const QStringList ids = sanitizedOrder(childIds);
    Layout next = load();
    if (ids.isEmpty()) {
        if (!next.childOrder.contains(parentId))
            return;
        next.childOrder.remove(parentId);
    } else {
        if (next.childOrder.value(parentId) == ids)
            return;
        next.childOrder.insert(parentId, ids);
    }
    save(next);
}

QStringList RailLayoutStore::orderedRooms(const QString &spaceId,
                                          const QStringList &known) const
{
    if (spaceId.isEmpty())
        return known;
    return applyStoredOrder(load().roomOrder.value(spaceId), known);
}

void RailLayoutStore::setRoomOrder(const QString &spaceId,
                                   const QStringList &roomIds)
{
    if (spaceId.isEmpty() || isPseudoSpace(spaceId))
        return;
    const QStringList ids = sanitizedOrder(roomIds);
    Layout next = load();
    if (ids.isEmpty()) {
        if (!next.roomOrder.contains(spaceId))
            return;
        next.roomOrder.remove(spaceId);
    } else {
        if (next.roomOrder.value(spaceId) == ids)
            return;
        next.roomOrder.insert(spaceId, ids);
    }
    save(next);
}

void RailLayoutStore::setTopLevelOrder(const QStringList &entryIds)
{
    Layout layout = load();
    QStringList next;
    for (const QString &id : entryIds) {
        if (id.isEmpty() || isPseudoSpace(id) || next.contains(id))
            continue;
        if (!folderOf(id).isEmpty())
            continue;   // filed: it is not a top-level entry
        next.append(id);
    }
    if (next == layout.order)
        return;
    layout.order = next;
    save(layout);
}

QStringList RailLayoutStore::expandedSpaceIds() const
{
    return load().expanded;
}

bool RailLayoutStore::spaceExpanded(const QString &spaceId) const
{
    return load().expanded.contains(spaceId);
}

void RailLayoutStore::setSpaceExpanded(const QString &spaceId, bool expanded)
{
    if (spaceId.isEmpty() || isPseudoSpace(spaceId))
        return;
    Layout layout = load();
    const bool has = layout.expanded.contains(spaceId);
    if (has == expanded)
        return;
    if (expanded)
        layout.expanded.append(spaceId);
    else
        layout.expanded.removeAll(spaceId);
    save(layout);
}

void RailLayoutStore::toggleSpaceExpanded(const QString &spaceId)
{
    setSpaceExpanded(spaceId, !spaceExpanded(spaceId));
}

QStringList RailLayoutStore::folderMembers(const QString &folderId) const
{
    for (const Folder &folder : load().folders) {
        if (folder.id == folderId)
            return folder.spaceIds;
    }
    return {};
}

QString RailLayoutStore::createFolderWithSpaces(const QStringList &spaceIds,
                                                int atIndex,
                                                const QString &name)
{
    QStringList members;
    for (const QString &id : spaceIds) {
        if (id.isEmpty() || isPseudoSpace(id) || members.contains(id))
            continue;
        members.append(id);
    }
    if (members.isEmpty())
        return {};
    Layout layout = load();
    if (layout.folders.size() >= kMaxFolders)
        return {};
    Folder folder;
    folder.id = makeFolderId(layout);
    if (folder.id.isEmpty())
        return {};
    folder.name = name.trimmed().left(kMaxNameLength);
    if (folder.name.isEmpty())
        folder.name = tr("Folder");
    folder.spaceIds = members;
    // The members leave wherever they were: another folder or the top level.
    for (Folder &other : layout.folders) {
        for (const QString &id : members)
            other.spaceIds.removeAll(id);
    }
    for (const QString &id : members)
        layout.order.removeAll(id);
    layout.folders.append(folder);
    const int clamped = atIndex < 0 ? layout.order.size()
                                    : qBound(0, atIndex, layout.order.size());
    layout.order.insert(clamped, folder.id);
    save(layout);
    return folder.id;
}

void RailLayoutStore::moveSpaceToFolder(const QString &spaceId,
                                        const QString &folderId, int index)
{
    if (spaceId.isEmpty() || isPseudoSpace(spaceId) || folderId.isEmpty())
        return;
    Layout layout = load();
    bool known = false;
    for (const Folder &folder : layout.folders) {
        if (folder.id == folderId) {
            known = true;
            break;
        }
    }
    if (!known)
        return;
    Layout next = layout;
    for (Folder &folder : next.folders) {
        if (folder.id == folderId)
            continue;
        folder.spaceIds.removeAll(spaceId);
    }
    for (Folder &folder : next.folders) {
        if (folder.id != folderId)
            continue;
        folder.spaceIds.removeAll(spaceId);
        const int clamped = index < 0
                                ? folder.spaceIds.size()
                                : qBound(0, index, folder.spaceIds.size());
        folder.spaceIds.insert(clamped, spaceId);
    }
    next.order.removeAll(spaceId);
    if (next.folders == layout.folders && next.order == layout.order)
        return;
    save(next);
}

void RailLayoutStore::applyArrangement(const QStringList &topLevel,
                                       const QVariantMap &folderMembers)
{
    Layout layout = load();
    Layout next = layout;

    QSet<QString> knownFolders;
    for (const Folder &folder : next.folders)
        knownFolders.insert(folder.id);

    // Every space the caller placed inside a folder, and which folder it is.
    QHash<QString, QString> assigned;
    QHash<QString, QStringList> rendered;
    for (auto it = folderMembers.constBegin(); it != folderMembers.constEnd();
         ++it) {
        if (!knownFolders.contains(it.key()))
            continue;   // a folder that no longer exists names nothing
        QStringList members;
        for (const QString &id : it.value().toStringList()) {
            if (id.isEmpty() || isPseudoSpace(id) || knownFolders.contains(id)
                || members.contains(id) || assigned.contains(id)) {
                continue;   // a Space is in at most one place
            }
            members.append(id);
            assigned.insert(id, it.key());
        }
        rendered.insert(it.key(), members);
    }

    QSet<QString> topLevelSet;
    QStringList order;
    for (const QString &id : topLevel) {
        if (id.isEmpty() || isPseudoSpace(id) || order.contains(id))
            continue;
        if (assigned.contains(id))
            continue;   // named as a folder member: not a top-level entry
        order.append(id);
        topLevelSet.insert(id);
    }

    for (Folder &folder : next.folders) {
        const auto it = rendered.constFind(folder.id);
        if (it != rendered.constEnd()) {
            folder.spaceIds = *it;
            continue;
        }
        // A folder the caller did not render (collapsed) keeps its members,
        // except any placed elsewhere by this call.
        for (int i = folder.spaceIds.size() - 1; i >= 0; --i) {
            const QString &id = folder.spaceIds.at(i);
            if (assigned.contains(id) || topLevelSet.contains(id))
                folder.spaceIds.removeAt(i);
        }
    }
    next.order = order;
    if (next.folders == layout.folders && next.order == layout.order)
        return;
    save(next);
}

QStringList RailLayoutStore::orderedSpaceIds(const QVariantList &spaces) const
{
    const Layout &layout = load();
    QStringList natural;
    QSet<QString> present;
    for (const QVariant &value : spaces) {
        const QString id =
            value.toMap().value(QStringLiteral("spaceId")).toString();
        if (id.isEmpty() || isPseudoSpace(id) || present.contains(id))
            continue;
        present.insert(id);
        natural.append(id);
    }

    QHash<QString, QString> folderOfSpace;
    for (const Folder &folder : layout.folders) {
        for (const QString &id : folder.spaceIds) {
            if (present.contains(id) && !folderOfSpace.contains(id))
                folderOfSpace.insert(id, folder.id);
        }
    }

    // Each folder expanded in place even when collapsed: this is an ordering
    // question.
    QStringList out;
    auto appendFolder = [&](const QString &folderId) {
        for (const Folder &folder : layout.folders) {
            if (folder.id != folderId)
                continue;
            for (const QString &id : folder.spaceIds) {
                if (present.contains(id) && !out.contains(id))
                    out.append(id);
            }
            return;
        }
    };
    auto isFolderId = [&layout](const QString &id) {
        for (const Folder &folder : layout.folders) {
            if (folder.id == id)
                return true;
        }
        return false;
    };
    QStringList top;
    for (const QString &id : layout.order) {
        if (folderOfSpace.contains(id))
            continue;
        top.append(id);
    }
    for (const Folder &folder : layout.folders) {
        if (!top.contains(folder.id))
            top.append(folder.id);
    }
    for (const QString &id : natural) {
        if (!folderOfSpace.contains(id) && !top.contains(id))
            top.append(id);
    }
    for (const QString &id : top) {
        if (isFolderId(id))
            appendFolder(id);
        else if (present.contains(id) && !out.contains(id))
            out.append(id);
    }
    return out;
}

QVariantList RailLayoutStore::arrange(const QVariantList &spaces) const
{
    const Layout &layout = load();

    QVariantList pseudo;
    QHash<QString, QVariantMap> byId;
    QStringList natural;
    for (const QVariant &value : spaces) {
        const QVariantMap entry = value.toMap();
        const QString id = entry.value(QStringLiteral("spaceId")).toString();
        if (isPseudoSpace(id)) {
            pseudo.append(entry);
            continue;
        }
        byId.insert(id, entry);
        natural.append(id);
    }

    // Folder membership, restricted to Spaces that actually exist right now.
    QHash<QString, QString> folderOfSpace;
    for (const Folder &folder : layout.folders) {
        for (const QString &id : folder.spaceIds) {
            if (byId.contains(id))
                folderOfSpace.insert(id, folder.id);
        }
    }

    // The user's order first, then anything new in model order, so a newly
    // joined Space appears at the bottom.
    QStringList top;
    for (const QString &id : layout.order) {
        if (folderOfSpace.contains(id))
            continue;            // filed since it was ordered
        if (byId.contains(id) || [&] {
                for (const Folder &folder : layout.folders) {
                    if (folder.id == id)
                        return true;
                }
                return false;
            }()) {
            top.append(id);
        }
    }
    for (const Folder &folder : layout.folders) {
        if (!top.contains(folder.id))
            top.append(folder.id);
    }
    for (const QString &id : natural) {
        if (!folderOfSpace.contains(id) && !top.contains(id))
            top.append(id);
    }

    QVariantList out = pseudo;
    for (const QString &id : top) {
        const Folder *folder = nullptr;
        for (const Folder &candidate : layout.folders) {
            if (candidate.id == id) {
                folder = &candidate;
                break;
            }
        }
        if (!folder) {
            auto it = byId.constFind(id);
            if (it == byId.constEnd())
                continue;
            QVariantMap entry = *it;
            entry.insert(QStringLiteral("kind"), QStringLiteral("space"));
            entry.insert(QStringLiteral("entryId"), id);
            entry.insert(QStringLiteral("folderId"), QString());
            entry.insert(QStringLiteral("folderLast"), false);
            out.append(entry);
            continue;
        }

        // The members that still exist, in the folder's own order.
        QStringList members;
        int unread = 0;
        int highlight = 0;
        // Up to four member avatars for the folder tile's composite preview,
        // so a collapsed folder is identifiable.
        QVariantList preview;
        for (const QString &memberId : folder->spaceIds) {
            auto it = byId.constFind(memberId);
            if (it == byId.constEnd())
                continue;
            members.append(memberId);
            unread += it->value(QStringLiteral("unreadTotal")).toInt();
            highlight += it->value(QStringLiteral("highlightTotal")).toInt();
            if (preview.size() < 4) {
                preview.append(QVariantMap{
                    { QStringLiteral("spaceId"), memberId },
                    { QStringLiteral("name"),
                      it->value(QStringLiteral("name")) },
                    { QStringLiteral("avatarUrl"),
                      it->value(QStringLiteral("avatarUrl")) },
                });
            }
        }
        // A folder with no resolved members still renders: it was created
        // empty, or its members have not synced yet. Emptiness is judged in
        // dropEmptiedFolders(), never here.
        QVariantMap entry;
        entry.insert(QStringLiteral("kind"), QStringLiteral("folder"));
        entry.insert(QStringLiteral("entryId"), folder->id);
        entry.insert(QStringLiteral("folderId"), folder->id);
        entry.insert(QStringLiteral("spaceId"), QString());
        entry.insert(QStringLiteral("name"), folder->name);
        entry.insert(QStringLiteral("collapsed"), folder->collapsed);
        entry.insert(QStringLiteral("memberIds"), members);
        entry.insert(QStringLiteral("memberPreview"), preview);
        entry.insert(QStringLiteral("childCount"), int(members.size()));
        entry.insert(QStringLiteral("unreadTotal"), unread);
        entry.insert(QStringLiteral("highlightTotal"), highlight);
        out.append(entry);

        if (folder->collapsed)
            continue;
        for (int m = 0; m < members.size(); ++m) {
            const QString &memberId = members.at(m);
            QVariantMap member = byId.value(memberId);
            member.insert(QStringLiteral("kind"), QStringLiteral("space"));
            member.insert(QStringLiteral("entryId"), memberId);
            member.insert(QStringLiteral("folderId"), folder->id);
            // The last member carries the container's rounded bottom.
            // RailEntryModel restamps this over nested rows.
            member.insert(QStringLiteral("folderLast"),
                          m == members.size() - 1);
            out.append(member);
        }
    }
    return out;
}
